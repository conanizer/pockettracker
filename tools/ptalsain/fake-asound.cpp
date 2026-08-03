// fake-asound.cpp — a stand-in libasound.so.2, built ONLY for tools/ptalsain (MIDI plan E5).
//
// ⚠️⚠️ **WHY A FAKE LIBRARY AND NOT A MOCK CLASS.** The thing under test is not "does AlsaMidiIn call
// the functions I told it to call" — a mock injected through a seam would ask that, and would pass on a
// backend that never starts its reader thread, never resyncs a split message and closes its handle
// while the thread is still inside a read. The thing under test is the BACKEND AS THE APP BUILDS IT:
// the real dlopen, the real 17 dlsyms, the real thread, the real EAGAIN poll, the real close ordering.
// So this .so carries the SONAME `libasound.so.2` and the loader hands it to the backend's own
// `dlopen("libasound.so.2")` with not one line of production code changed for the test.
//
// ⚠️ **WHAT IT CANNOT TELL YOU, stated rather than discovered later:** it is OUR idea of how ALSA
// behaves. It cannot prove that snd_rawmidi_read really answers -EAGAIN on an empty non-blocking port,
// nor that -ENXIO really means "no stream in that direction". Those come from the documentation, and
// the independent anchor for the layer below them is `tools/ptalsa`, which compares every prototype and
// both stream constants against ALSA's OWN headers. Two checks, two authorities: ptalsa says the
// signatures are upstream's, this says the backend does the right thing when they behave as documented.
// What NEITHER covers is a real card — that stays an owed hardware check.
//
// ── The imaginary machine ────────────────────────────────────────────────────────────────────────
//
// One card, four rawmidi devices, chosen so that a single fixture exercises both directions' filters
// and the duplicate-name suffix:
//
//     hw:0,0  "PT Fake Duplex"     input + output
//     hw:0,1  "PT Fake Out Only"   output only      (must NOT appear on the INPUT list)
//     hw:0,2  "PT Fake In Only"    input only       (must NOT appear on the OUTPUT list)
//     hw:0,3  "PT Fake Duplex"     input + output   (the SAME name as hw:0,0 -> " #2")

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct FakeDevice {
    const char* name;
    bool        in;
    bool        out;
};

const FakeDevice kDevices[] = {
        {"PT Fake Duplex", true, true},
        {"PT Fake Out Only", false, true},
        {"PT Fake In Only", true, false},
        {"PT Fake Duplex", true, true},
};
constexpr int kDeviceCount = static_cast<int>(sizeof kDevices / sizeof kDevices[0]);

/** What `snd_rawmidi_info_*` fills in, and what `snd_ctl_rawmidi_info` answers against. */
struct FakeInfo {
    unsigned    device    = 0;
    unsigned    subdevice = 0;
    int         stream    = 0;
    const char* name      = "";
};

/** A handle `snd_rawmidi_open` hands back. `alive` is the use-after-close canary. */
struct FakeHandle {
    int  device = -1;
    bool input  = false;
    bool alive  = true;
};

struct State {
    std::mutex           mu;
    std::deque<uint8_t>  inbox;         // bytes waiting to be read by the app
    std::vector<uint8_t> written;       // bytes the app has written
    int                  opens          = 0;
    int                  closes         = 0;
    int                  lastOpenMode   = -1;
    std::string          lastOpenName;
    long long            reads          = 0;
    int                  readAfterClose = 0;
    int                  armedReadError = 0;   // returned once, then cleared
    int                  ctlOpens       = 0;
};

State& st() {
    static State s;
    return s;
}

int device_index_from_name(const char* name, int* card) {
    // "hw:C,D"
    int c = -1, d = -1;
    if (!name || std::sscanf(name, "hw:%d,%d", &c, &d) != 2) return -1;
    if (card) *card = c;
    if (c != 0 || d < 0 || d >= kDeviceCount) return -1;
    return d;
}

}  // namespace

extern "C" {

// ─── The libasound surface the two backends dlsym ────────────────────────────────────────────────

int snd_card_next(int* card) {
    if (!card) return -EINVAL;
    *card = (*card < 0) ? 0 : -1;   // one card, then the end — ALSA's own convention
    return 0;
}

int snd_ctl_open(void** ctl, const char* name, int /*mode*/) {
    if (!ctl || !name) return -EINVAL;
    if (std::strcmp(name, "hw:0") != 0) return -ENODEV;
    ++st().ctlOpens;
    static int handle = 0xC7;
    *ctl = &handle;
    return 0;
}

int snd_ctl_close(void* /*ctl*/) { return 0; }

int snd_ctl_rawmidi_next_device(void* /*ctl*/, int* device) {
    if (!device) return -EINVAL;
    const int next = (*device < 0) ? 0 : *device + 1;
    *device        = (next < kDeviceCount) ? next : -1;
    return 0;
}

int snd_rawmidi_info_malloc(void** info) {
    if (!info) return -EINVAL;
    *info = new FakeInfo();
    return 0;
}

void snd_rawmidi_info_free(void* info) { delete static_cast<FakeInfo*>(info); }

void snd_rawmidi_info_set_device(void* info, unsigned int val) {
    static_cast<FakeInfo*>(info)->device = val;
}

void snd_rawmidi_info_set_subdevice(void* info, unsigned int val) {
    static_cast<FakeInfo*>(info)->subdevice = val;
}

void snd_rawmidi_info_set_stream(void* info, int val) { static_cast<FakeInfo*>(info)->stream = val; }

const char* snd_rawmidi_info_get_name(const void* info) {
    return static_cast<const FakeInfo*>(info)->name;
}

/**
 * ⚠️ THE FILTER, and the whole reason this fake has four devices. -ENXIO is what a real card answers
 * for a device that has no stream in the direction asked for, and it is what keeps output-only ports
 * off the INPUT list and input-only ports off the OUTPUT one.
 */
int snd_ctl_rawmidi_info(void* /*ctl*/, void* infoRaw) {
    auto* info = static_cast<FakeInfo*>(infoRaw);
    if (info->device >= static_cast<unsigned>(kDeviceCount)) return -ENXIO;
    const FakeDevice& d = kDevices[info->device];
    const bool        wantInput = (info->stream == 1);   // SND_RAWMIDI_STREAM_INPUT
    if (wantInput ? !d.in : !d.out) return -ENXIO;
    info->name = d.name;
    return 0;
}

int snd_rawmidi_open(void** in, void** out, const char* name, int mode) {
    const int dev = device_index_from_name(name, nullptr);
    if (dev < 0) return -ENODEV;
    if (in && !kDevices[dev].in) return -ENXIO;
    if (out && !kDevices[dev].out) return -ENXIO;

    {
        std::lock_guard<std::mutex> g(st().mu);
        ++st().opens;
        st().lastOpenMode = mode;
        st().lastOpenName = name;
    }

    auto* h  = new FakeHandle();
    h->device = dev;
    h->input  = (in != nullptr);
    if (in) *in = h;
    if (out) *out = h;
    return 0;
}

int snd_rawmidi_close(void* handle) {
    auto* h = static_cast<FakeHandle*>(handle);
    if (!h) return -EINVAL;
    std::lock_guard<std::mutex> g(st().mu);
    ++st().closes;
    // ⚠️ NOT deleted: the canary in `snd_rawmidi_read` has to be able to notice a read that arrives
    // after this, and reading freed memory to find out would be the very bug it is looking for.
    h->alive = false;
    return 0;
}

long snd_rawmidi_read(void* handle, void* buffer, size_t size) {
    auto* h = static_cast<FakeHandle*>(handle);
    if (!h || !buffer || size == 0) return -EINVAL;

    std::lock_guard<std::mutex> g(st().mu);
    ++st().reads;
    if (!h->alive) {
        // ⭐ THE USE-AFTER-CLOSE CANARY. A backend that closes its handle before joining its reader
        // thread lands here, and on a real card this is a read through a freed snd_rawmidi_t*.
        ++st().readAfterClose;
        return -ENODEV;
    }
    if (st().armedReadError != 0) {
        const int err       = st().armedReadError;
        st().armedReadError = 0;
        return err;
    }
    if (st().inbox.empty()) return -EAGAIN;   // the ordinary non-blocking "nothing yet"

    auto*  outBytes = static_cast<uint8_t*>(buffer);
    size_t n        = 0;
    while (n < size && !st().inbox.empty()) {
        outBytes[n++] = st().inbox.front();
        st().inbox.pop_front();
    }
    return static_cast<long>(n);
}

long snd_rawmidi_write(void* handle, const void* buffer, size_t size) {
    auto* h = static_cast<FakeHandle*>(handle);
    if (!h || !h->alive) return -ENODEV;
    std::lock_guard<std::mutex> g(st().mu);
    const auto* b = static_cast<const uint8_t*>(buffer);
    st().written.insert(st().written.end(), b, b + size);
    return static_cast<long>(size);
}

int snd_rawmidi_drain(void* handle) {
    auto* h = static_cast<FakeHandle*>(handle);
    return (h && h->alive) ? 0 : -ENODEV;
}

const char* snd_strerror(int errnum) {
    static char buf[64];
    std::snprintf(buf, sizeof buf, "fake error %d", errnum);
    return buf;
}

// ─── The test's control surface (not part of ALSA) ───────────────────────────────────────────────

void pt_fake_push(const uint8_t* bytes, int n) {
    std::lock_guard<std::mutex> g(st().mu);
    for (int i = 0; i < n; ++i) st().inbox.push_back(bytes[i]);
}

int pt_fake_written(uint8_t* out, int max) {
    std::lock_guard<std::mutex> g(st().mu);
    const int n = static_cast<int>(st().written.size()) < max ? static_cast<int>(st().written.size())
                                                             : max;
    if (out && n > 0) std::memcpy(out, st().written.data(), static_cast<size_t>(n));
    return static_cast<int>(st().written.size());
}

void      pt_fake_clear_written() { std::lock_guard<std::mutex> g(st().mu); st().written.clear(); }
int       pt_fake_read_after_close() { std::lock_guard<std::mutex> g(st().mu); return st().readAfterClose; }
int       pt_fake_opens() { std::lock_guard<std::mutex> g(st().mu); return st().opens; }
int       pt_fake_closes() { std::lock_guard<std::mutex> g(st().mu); return st().closes; }
int       pt_fake_last_open_mode() { std::lock_guard<std::mutex> g(st().mu); return st().lastOpenMode; }
long long pt_fake_reads() { std::lock_guard<std::mutex> g(st().mu); return st().reads; }
void      pt_fake_arm_read_error(int err) { std::lock_guard<std::mutex> g(st().mu); st().armedReadError = err; }

const char* pt_fake_last_open_name() {
    std::lock_guard<std::mutex> g(st().mu);
    static std::string copy;
    copy = st().lastOpenName;
    return copy.c_str();
}

}  // extern "C"
