// ptalsain — the LINUX MIDI backends, DRIVEN. MIDI plan phase E5. Linux only.
//
// ⚠️⚠️ **THE BLIND CHANNEL THIS TOOL EXISTS FOR.** Before it, every claim about `AlsaMidiOut` and
// `AlsaMidiIn` was one of two things: "it compiles" (which is not a claim about behaviour at all) or
// "`ptalsa` says the prototypes match upstream" (a claim about SIGNATURES, not about what the backend
// DOES with them). Nothing in this repo had ever run a line of the Linux MIDI code. The output backend
// shipped in B2b and has, to this day, never put a byte on a real cable — and the guardrails have a
// name for a component whose correct behaviour is silence.
//
// So this links the REAL backends — shell/midi-in-alsa.cpp, shell/midi-out-alsa.cpp,
// shell/alsa-rawmidi.cpp and the two shared bases — and gives them a libasound to talk to. The fake
// (fake-asound.cpp) carries the SONAME `libasound.so.2`, so the backend's own
// `dlopen("libasound.so.2")` finds it: the loader runs, all seventeen `dlsym`s run, the reader THREAD
// runs, the EAGAIN poll runs, the close ordering runs. **Not one line of production code knows this
// test exists**, which is the difference between this and a mock.
//
// ── ⚠️ WHAT IT CANNOT SAY, so that nobody reads more into a green run than is there ──────────────
//
//   • It cannot prove the REAL libasound behaves as the fake does. That comes from the documentation;
//     the independent anchor for the layer beneath it is `ptalsa`, which compares every prototype and
//     both stream constants against ALSA's own headers. Two checks, two authorities.
//   • It is not a cable. No USB-MIDI device has ever been attached to this project on Linux, and until
//     one is, "a keyboard plays a track on a handheld" remains an owed HARDWARE check (plan §0.3a).
//   • The Android backend is not here — its platform half is JNI and needs a phone. What it shares
//     with this one is `MidiInBase`, which block 3 drives.
//
//   ctest --test-dir tools/build-linux -R e5-alsa-io --output-on-failure
//
// Exit code 0 = all green, 1 = any assertion failed.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__linux__) || defined(__ANDROID__)
int main() {
    std::printf("ptalsain: not Linux - the ALSA backends do not exist in this build\n");
    return 0;
}
#else

#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

#include "../../shell/midi-in-alsa.h"
#include "../../shell/midi-out-alsa.h"

// The fake's control surface. Declared rather than included: it is a C ABI on purpose, so that what
// the test calls and what the backend dlopens are provably the same object in the same process.
extern "C" {
void        pt_fake_push(const uint8_t* bytes, int n);
int         pt_fake_written(uint8_t* out, int max);
void        pt_fake_clear_written();
int         pt_fake_read_after_close();
int         pt_fake_opens();
int         pt_fake_closes();
int         pt_fake_last_open_mode();
long long   pt_fake_reads();
void        pt_fake_arm_read_error(int err);
const char* pt_fake_last_open_name();
}

static int failures = 0;

// ── the verdict printer (ptmidi/ptmidiin's rule: the NUMBER beside the verdict, always) ──────────

static void ok(bool cond, const std::string& what, const std::string& got, const std::string& want) {
    if (cond) {
        std::printf("[PASS] %-52s %s\n", what.c_str(), got.c_str());
    } else {
        std::printf("[FAIL] %-52s got %s, want %s\n", what.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}

static void eq_int(const std::string& what, long long got, long long want) {
    ok(got == want, what, std::to_string(got), std::to_string(want));
}

static void eq_str(const std::string& what, const std::string& got, const std::string& want) {
    ok(got == want, what, "'" + got + "'", "'" + want + "'");
}

static std::string hexbytes(const uint8_t* b, int n) {
    std::string s;
    char        buf[8];
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%s%02X", i ? " " : "", b[i]);
        s += buf;
    }
    return s;
}

/**
 * Wait until `pred` holds, up to `ms`. Returns how long it took, or -1.
 *
 * ⚠️ A POLL AND NOT A SLEEP, and the difference is what the failure prints. A fixed `sleep(200)` would
 * make every timing-dependent check either flaky on a loaded machine or slow on every machine, and a
 * failure would say nothing about whether the byte was late or absent. This returns the elapsed
 * milliseconds so a green run says how fast the reader thread actually was — which is the only reading
 * anywhere in this repo of the latency the ALSA poll interval adds.
 */
static int wait_until(const std::function<bool()>& pred, int ms) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (pred()) {
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - start)
                                            .count());
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/** Where the backend's bytes land. The real one is `MidiInQueue`; this one remembers order. */
struct RecordingSink : songcore::IMidiInSink {
    mutable std::mutex   mu;
    std::vector<uint8_t> bytes;
    int                  calls = 0;

    void on_bytes(const uint8_t* data, int len) override {
        std::lock_guard<std::mutex> g(mu);
        ++calls;
        bytes.insert(bytes.end(), data, data + len);
    }

    size_t size() const {
        std::lock_guard<std::mutex> g(mu);
        return bytes.size();
    }
    std::string hex() const {
        std::lock_guard<std::mutex> g(mu);
        return hexbytes(bytes.data(), static_cast<int>(bytes.size()));
    }
};

int main() {
    std::printf("== ptalsain: the Linux ALSA MIDI backends, against a fake libasound.so.2 ==\n\n");

    // ── 1. ENUMERATION, BOTH DIRECTIONS, ONE FIXTURE ─────────────────────────────────────────────
    //
    // ⚠️ **THIS IS THE STREAM FILTER, AND IT IS THE ONE PER-DIRECTION FACT IN alsa-rawmidi.cpp.** The
    // fake card has an input-only device, an output-only device and two duplex ones with the SAME name.
    // Backwards, the INPUT row lists the user's synth and hides their keyboard — a failure that looks
    // like "no MIDI devices" rather than like a bug, and one no test could catch before this file.
    {
        std::printf("-- 1. enumeration: the STREAM filter, in both directions --\n");
        ptshell::AlsaMidiIn  in;
        ptshell::AlsaMidiOut out;

        ok(in.available(), "the dlopen found a libasound (the FAKE — see block 1's names)",
           in.available() ? "loaded" : "absent", "loaded");

        const int nIn  = in.device_count();
        const int nOut = out.device_count();

        // 3 of the 4: the output-only device must be missing from the input list, and vice versa. If
        // the real libasound had won the dlopen, this desk's card list would answer 0 and say so here.
        eq_int("INPUT lists the 3 devices that can SEND to us", nIn, 3);
        eq_int("OUTPUT lists the 3 devices that can RECEIVE from us", nOut, 3);

        eq_str("IN  [0] the duplex device", in.device_name(0), "PT Fake Duplex");
        eq_str("IN  [1] the INPUT-only device", in.device_name(1), "PT Fake In Only");
        eq_str("OUT [1] the OUTPUT-only device", out.device_name(1), "PT Fake Out Only");

        // ⚠️ The de-duplicator: two identical USB interfaces produce two identical names, and the
        // SETTINGS store a name — so an un-suffixed duplicate is a device the user can never pick.
        eq_str("IN  [2] a second device of the same name is suffixed", in.device_name(2),
               "PT Fake Duplex #2");
        eq_str("OUT [2] …and identically on the output side", out.device_name(2),
               "PT Fake Duplex #2");

        // Out of range is empty, not a crash: `refresh_midi_in_devices` walks 0..n-1 and the MIDI
        // screen's row indexes into what it got back.
        eq_str("IN  [9] out of range is empty", in.device_name(9), "");
    }

    // ── 2. OPEN: THE MODE AND THE NAME ───────────────────────────────────────────────────────────
    //
    // ⚠️ **NONBLOCK IS A CORRECTNESS DECISION, NOT A TUNING ONE** (midi-in-alsa.cpp argues it at
    // length): a BLOCKING read would park the reader thread inside libasound with no documented way to
    // wake it, and `close()` would join a thread that never returns — an app that hangs on quit, on a
    // handheld, in the shutdown path. Nothing but this check can see which mode was passed.
    {
        std::printf("\n-- 2. open: the mode and the hw name --\n");
        ptshell::AlsaMidiIn in;
        in.device_count();

        // ⚠️ THE OPEN IS ON ITS OWN LINE, and the first draft of this file had it inside the `ok(...)`
        // call — where C++ leaves argument evaluation order UNSPECIFIED, so the `got` string was built
        // from `is_open()` BEFORE the open ran and every successful open printed "closed". A verdict
        // that fires correctly while its reading is nonsense is exactly the "got not computed from the
        // object under test" failure the guardrails name, and it was visible only because the reading
        // sat beside the word PASS.
        const bool opened = in.open(1);
        ok(opened, "opening the INPUT-only device succeeds", in.is_open() ? "open" : "closed", "open");
        eq_str("…by the hw address of the device that was picked", pt_fake_last_open_name(), "hw:0,2");
        eq_int("⭐ …and NONBLOCK (SND_RAWMIDI_NONBLOCK = 2), never blocking",
               pt_fake_last_open_mode(), 2);
        eq_int("…and it is the port the index named", in.open_index(), 1);

        // An index the list does not have must not open anything — the port list is re-read on every
        // visit to the MIDI screen and a device can vanish between the read and the pick.
        ptshell::AlsaMidiIn in2;
        in2.device_count();
        ok(!in2.open(7), "an out-of-range index opens nothing", in2.is_open() ? "open" : "closed",
           "closed");
    }

    // ── 3. THE READER THREAD: BYTES ARRIVE, IN ORDER, WITHOUT THE APP ASKING ─────────────────────
    //
    // The whole point of the backend. ⭐ It is also the first time in this project that a byte has
    // travelled from a libasound `read` to an `IMidiInSink` on Linux.
    {
        std::printf("\n-- 3. the reader thread: delivery --\n");
        ptshell::AlsaMidiIn in;
        RecordingSink       sink;
        in.device_count();
        in.set_sink(&sink);   // ⚠️ BEFORE the open: an open port is already delivering (app.h/E2)
        const bool opened = in.open(0);
        ok(opened, "the duplex device opens for input", in.is_open() ? "open" : "closed", "open");

        const uint8_t note[3] = {0x90, 0x3C, 0x40};
        pt_fake_push(note, 3);
        const int ms = wait_until([&] { return sink.size() >= 3; }, 2000);
        ok(ms >= 0, "⭐ a note-on pushed at the port reaches the SINK",
           ms >= 0 ? ("after " + std::to_string(ms) + " ms") : std::string("never (2 s)"), "arrives");
        eq_str("…byte for byte", sink.hex(), "90 3C 40");
        eq_int("…and the PORT counted them (the exit report's first stage)",
               static_cast<long long>(in.bytes_received()), 3);

        // ⚠️ A BURST, AND SPLIT ACROSS PUSHES. ALSA hands over whatever a read returned, which can cut
        // a message in half; the byte ring above the seam resyncs by construction, and this is the
        // check that the backend does not try to be clever about it.
        const uint8_t half1[2] = {0x91, 0x40};
        const uint8_t half2[1] = {0x55};
        pt_fake_push(half1, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pt_fake_push(half2, 1);
        const int ms2 = wait_until([&] { return sink.size() >= 6; }, 2000);
        ok(ms2 >= 0, "a message SPLIT across two reads arrives whole",
           ms2 >= 0 ? sink.hex() : std::string("never (2 s)"), "90 3C 40 91 40 55");
        eq_str("…in order, with nothing invented between the halves", sink.hex(), "90 3C 40 91 40 55");

        // ⚠️ `set_sink(nullptr)` must be honoured immediately — it is what a shutting-down app uses.
        // The PORT still counts, and that difference is the whole design of the exit report: "we
        // received it and dropped it later" is a different bug from "nothing was sent to us".
        in.set_sink(nullptr);
        const uint64_t before = in.bytes_received();
        const size_t   sunk   = sink.size();
        const uint8_t  more[3] = {0x92, 0x30, 0x20};
        pt_fake_push(more, 3);
        const int ms3 = wait_until([&] { return in.bytes_received() >= before + 3; }, 2000);
        ok(ms3 >= 0, "with the sink UNWIRED the port still counts the bytes",
           std::to_string(in.bytes_received()) + " received", std::to_string(before + 3));
        eq_int("…and delivers not one of them", static_cast<long long>(sink.size() - sunk), 0);
    }

    // ── 4. CLOSE: THE THREAD IS JOINED BEFORE THE HANDLE DIES ────────────────────────────────────
    //
    // ⚠️⚠️ **THE BUG THIS BLOCK IS AIMED AT IS A USE-AFTER-FREE THAT ONLY APPEARS UNDER LOAD.** The
    // reader spends its life inside `snd_rawmidi_read(in_, …)`; close the handle first and the next
    // read is through a freed `snd_rawmidi_t*`. It would work on this desk nearly every time. The fake
    // keeps its handle object alive after `snd_rawmidi_close` for exactly this reason and counts any
    // read that arrives afterwards — a canary a real card cannot give us.
    {
        std::printf("\n-- 4. close: ordering, and the use-after-close canary --\n");
        ptshell::AlsaMidiIn in;
        RecordingSink       sink;
        in.device_count();
        in.set_sink(&sink);
        in.open(0);

        const uint8_t b[3] = {0x90, 0x40, 0x40};
        pt_fake_push(b, 3);
        wait_until([&] { return sink.size() >= 3; }, 2000);

        // ⚠️ **THE IDLE POLL IS MEASURED OVER A WINDOW, and the first draft measured it over none.** It
        // read `reads` immediately before the close, so the answer was "0 reads while open" on a
        // perfectly working loop — the window, not the loop, was the bug. 20 ms is twenty poll
        // intervals; anything above zero here means the thread is genuinely looping rather than having
        // delivered its one burst and died.
        const long long readsBefore = pt_fake_reads();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const long long readsIdle = pt_fake_reads();

        in.set_sink(nullptr);
        in.close();

        ok(!in.is_open(), "close() leaves the port closed", in.is_open() ? "open" : "closed", "closed");
        eq_int("⭐⭐ no read arrived after snd_rawmidi_close (the thread was JOINED first)",
               pt_fake_read_after_close(), 0);

        // The thread is really gone, not merely quiet: nothing reads the port any more.
        const long long readsAtClose = pt_fake_reads();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        eq_int("…and the reader thread has STOPPED (no reads in the 50 ms after)",
               pt_fake_reads() - readsAtClose, 0);
        ok(readsIdle > readsBefore, "…having polled all the while (the loop really loops)",
           std::to_string(readsIdle - readsBefore) + " reads in 20 idle ms", "> 0");

        // A byte arriving after the close reaches nothing at all — the E2 lifetime rule, from the other
        // side: `app.cpp` closes the port before `run` returns because the queue is inside `host`.
        const size_t sunk = sink.size();
        pt_fake_push(b, 3);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        eq_int("a byte pushed AFTER the close reaches nobody",
               static_cast<long long>(sink.size() - sunk), 0);
    }

    // ── 5. A FATAL READ ERROR: THE CABLE IS PULLED ───────────────────────────────────────────────
    //
    // -ENODEV repeats forever, so the reader stops rather than spinning on it — and the counters are
    // what make a stopped reader distinguishable from a keyboard nobody is playing. ⚠️ The close after
    // it must still be prompt: a thread that has already returned is a `join()` that returns at once,
    // and a `join()` on a thread that never started is the classic hang in the shutdown path.
    {
        std::printf("\n-- 5. a fatal read error --\n");
        ptshell::AlsaMidiIn in;
        RecordingSink       sink;
        in.device_count();
        in.set_sink(&sink);
        in.open(0);

        pt_fake_arm_read_error(-19);   // -ENODEV
        const int ms = wait_until([&] { return in.stopped_early(); }, 2000);
        ok(ms >= 0, "the reader stops on -ENODEV instead of spinning",
           ms >= 0 ? ("after " + std::to_string(ms) + " ms") : std::string("never (2 s)"), "stops");
        eq_int("…and COUNTS it, so the exit report can name the cause",
               static_cast<long long>(in.read_errors()), 1);
        eq_int("…on the shared port-error counter as well (winmm's MIM_ERROR twin)",
               static_cast<long long>(in.port_errors()), 1);

        const auto t0 = std::chrono::steady_clock::now();
        in.set_sink(nullptr);
        in.close();
        const auto closeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0)
                                     .count();
        ok(closeMs < 500, "…and closing a DEAD port is still prompt (no hang on quit)",
           std::to_string(closeMs) + " ms", "< 500 ms");
    }

    // ── 6. THE OUTPUT BACKEND, WHICH HAS NEVER BEEN DRIVEN AT ALL ────────────────────────────────
    //
    // ⚠️ B2b shipped this and nothing has ever executed it. The serializer above it is `ptmidi`'s
    // business; what is checked here is the last three inches — that the bytes handed to `send` are the
    // bytes that reach `snd_rawmidi_write`, and that a close really does panic all sixteen channels
    // first, which is written down in two places and observed in none.
    {
        std::printf("\n-- 6. the OUTPUT backend: the bytes reach the port --\n");
        ptshell::AlsaMidiOut out;
        out.device_count();
        pt_fake_clear_written();

        const bool opened = out.open(0);
        ok(opened, "the duplex device opens for output", out.is_open() ? "open" : "closed", "open");
        eq_int("…BLOCKING (mode 0), the opposite decision from the input port",
               pt_fake_last_open_mode(), 0);

        const uint8_t note[3] = {0x90, 0x3C, 0x64};
        out.send(note, 3);
        uint8_t   got[512];
        const int n = pt_fake_written(got, sizeof got);
        eq_int("one message in, one message out", n, 3);
        eq_str("…byte for byte", hexbytes(got, n < 3 ? n : 3), "90 3C 64");

        // ⭐ THE PANIC ON CLOSE. `MidiOutBase::panic_all_channels` sends CC 123 (all notes off) on all
        // sixteen channels; ALSA has no `midiOutReset`, so those bytes are ours to write, and a close
        // that skipped them leaves a synth droning after the app is gone. Nothing has ever observed
        // them leaving on this platform.
        //
        // ⚠️ THE EXPECTATION IS DERIVED, NOT REMEMBERED. The first draft of this check wanted ">= 96
        // bytes (two CCs per channel)" from memory of what a panic "should" be, and failed against
        // perfectly correct code that sends one. What is asserted now is the shape the source actually
        // has — 16 messages, channel 0 to 15 in order, CC 123 value 0 — which is a statement a future
        // change to `panic_all_channels` must deliberately update rather than accidentally satisfy.
        pt_fake_clear_written();
        out.close();
        const int panicBytes = pt_fake_written(got, sizeof got);
        eq_int("⭐ closing the port PANICS all 16 channels first (16 x 3 bytes)", panicBytes, 48);

        int  channelsSeen = 0;
        bool everyOneRight = (panicBytes == 48);
        for (int ch = 0; ch < 16 && panicBytes >= 48; ++ch) {
            const uint8_t* m = got + ch * 3;
            if (m[0] == static_cast<uint8_t>(0xB0 | ch) && m[1] == 123 && m[2] == 0) ++channelsSeen;
            else everyOneRight = false;
        }
        ok(everyOneRight && channelsSeen == 16, "…CC 123 value 0, on every channel, in order",
           std::to_string(channelsSeen) + "/16 correct (first: " +
                   (panicBytes >= 3 ? hexbytes(got, 3) : std::string("nothing")) + ")",
           "16/16 (first: B0 7B 00)");
    }

    std::printf("\n%s  (%d failure%s)\n", failures ? "SOME CHECKS FAILED" : "ALL GREEN", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

#endif  // __linux__ && !__ANDROID__
