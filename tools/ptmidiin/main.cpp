// ptmidiin — the MIDI-IN path (MIDI plan phase E, increment E1). Host tool, no device, no cable.
//
// ⚠️ **NOT A CONFORMANCE TOOL**, for exactly ptmidi's reason and it is worth restating: MIDI in never
// existed in Kotlin either, so there is no golden to record and none is possible. These are
// hand-written assertions in the ptdispatch / ptmidi tradition — a specification with an executable
// body. It cannot tell you that you were right; it can tell you that you changed your mind.
//
// What it covers, and what nothing else in the tree can:
//
//   • **THE PROTOCOL.** Running status, real-time bytes arriving in the MIDDLE of another message,
//     System Common cancelling running status, SysEx, orphan data bytes. Every one of those is
//     something a real cable does and a naive parser gets wrong — and every one of them is invisible
//     downstream, because a mis-parsed stream produces perfectly well-formed events for notes nobody
//     played. There is no instrument anywhere else in this repo that reads a raw MIDI byte at all.
//   • **THE THREAD BOUNDARY.** Bytes arrive on a thread nothing else here runs on (a winmm callback,
//     an ALSA reader, an Android binder thread). Block 2's last case runs a real producer thread
//     against a real drain, because a single-threaded check of a ring buffer stays green with every
//     lock deleted — B3's lesson, applied to the object B3's lesson was about.
//   • **THE POLICY.** Which track hears channel 5, which instrument that track plays, and what a bus
//     record for it looks like. `Project::midiInputChannels` has round-tripped since B1 and been read
//     by NOTHING; this is the first thing that reads it, so it is also the first thing that could
//     notice it does not work.
//   • **THE JOIN.** Block 4 drives bytes through queue → parser → router → `ExternalConsumer` and
//     asserts what leaves on the wire. That is the property no unit block can have: the records this
//     file invents have to be records the EXISTING consumer accepts, and a keyboard playing an
//     EXTERNAL instrument is MIDI thru, byte for byte, through code neither half was written for.
//
// ⭐ **THE ANCHOR OUTSIDE THE ARITHMETIC**, and block 4 exists as much for this as for the join:
// velocity 64 in must be velocity 64 out. The in-path widens 0-127 to a 0-1 float and squares it into
// a velocity curve; the out-path takes a square root and narrows it back. Neither side can check
// itself — each is perfectly self-consistent while being wrong — but a keyboard byte that survives the
// round trip unchanged is a claim about both at once, and it is anchored on the number a human pressed.
//
//   ctest --test-dir tools/build -R e1-midi-in --output-on-failure -C Release
//
// Exit code 0 = all green, 1 = any assertion failed.

#include "../../native/songcore/host.h"
#include "../../native/songcore/midi_in.h"
#include "../../native/songcore/midi_out.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using songcore::Event;
using songcore::Instrument;
using songcore::InstrumentType;
using songcore::MidiCcSlot;
using songcore::MidiInMessage;
using songcore::MidiInputRouter;
using songcore::MidiInQueue;
using songcore::MidiParser;
using songcore::Note;
using songcore::Project;
using songcore::SongcoreHost;
using songcore::TrackInstruments;

static int failures = 0;

// ── the verdict printer ──────────────────────────────────────────────────────────────────────────
// The NUMBER beside the verdict, always (ptmidi's rule, and the memory's): a bare PASS cannot tell a
// working check from one that compared two things which are both wrong.

static void ok(bool cond, const std::string& what, const std::string& got, const std::string& want) {
    if (cond) {
        std::printf("[PASS] %-48s %s\n", what.c_str(), got.c_str());
    } else {
        std::printf("[FAIL] %-48s got %s, want %s\n", what.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}

static void eq_int(const std::string& what, long long got, long long want) {
    ok(got == want, what, std::to_string(got), std::to_string(want));
}

/**
 * `n` bytes as "90 3C 40".
 *
 * ⚠️ It exists because two checks in this file were first written with the expected string hand-typed
 * on BOTH sides of `ok`, so a failing run printed `got 00 01 02 03, want 00 01 02 03` — a verdict that
 * fires correctly and says nothing about why. Control 5 is what exposed it. **A `got` that was not
 * computed from the thing under test is not a reading**, and this repo's own rule is to print the
 * number beside the verdict for exactly this reason.
 */
static std::string hexbytes(const uint8_t* b, int n) {
    std::string s;
    char buf[8];
    for (int i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%s%02X", i ? " " : "", b[i]);
        s += buf;
    }
    return s;
}

// ── A recording port ─────────────────────────────────────────────────────────────────────────────
// ⚠️⚠️ **DECLARED BEFORE THE HOST THAT WRITES TO IT, in every block below.** `~ExternalConsumer`
// panics and a panic EMITS, so a port destroyed first is written into after it dies — the double free
// phase C found in ptmidi. The rule belongs beside the object it constrains.
struct RecordingMidiOut : songcore::IMidiOut {
    std::vector<std::string> log;
    bool opened = true;

    int         device_count() override { return 1; }
    std::string device_name(int) override { return "RECORDER"; }
    bool        open(int) override { opened = true; return true; }
    void        close() override { opened = false; }
    bool        is_open() const override { return opened; }

    void send(const uint8_t* data, int len) override {
        char buf[16];
        std::string s;
        for (int i = 0; i < len; ++i) {
            std::snprintf(buf, sizeof buf, "%s%02X", i ? " " : "", data[i]);
            s += buf;
        }
        log.push_back(s);
    }
};

static std::string at(const RecordingMidiOut& out, size_t i) {
    return i < out.log.size() ? out.log[i] : std::string("<none>");
}

static void dump(const RecordingMidiOut& out, const char* label) {
    std::printf("  -- %s (%d messages) --\n", label, static_cast<int>(out.log.size()));
    for (size_t i = 0; i < out.log.size(); ++i)
        std::printf("     %2d  %s\n", static_cast<int>(i), out.log[i].c_str());
}

// ── parser helpers ───────────────────────────────────────────────────────────────────────────────

/** Feed a byte stream; collect every completed message. */
static std::vector<MidiInMessage> parse_all(MidiParser& p, const std::vector<uint8_t>& bytes) {
    std::vector<MidiInMessage> out;
    for (uint8_t b : bytes)
        if (p.feed(b)) out.push_back(p.message());
    return out;
}

static std::string describe(const MidiInMessage& m) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%02X ch%d %d %d (len %d)", m.status, m.channel, m.data1, m.data2, m.len);
    return buf;
}

static MidiInMessage msg_at(const std::vector<MidiInMessage>& v, size_t i) {
    return i < v.size() ? v[i] : MidiInMessage{};
}

/**
 * The two note predicates, as a printable pair — "on", "off", "both" or "neither".
 *
 * ⚠️ Not decoration: the first draft asserted these with literal `"off"` strings on both sides, so a
 * failing run printed `got off, want off` and said nothing at all. Control 1 is what showed it — a
 * control that fires is only useful if its output names what went wrong. **The verdict must carry the
 * value that was actually computed**, which for a bool means computing it here rather than describing
 * it at the call site.
 */
static std::string note_verdict(const MidiInMessage& m) {
    const bool on = m.is_note_on(), off = m.is_note_off();
    return on && off ? "both" : on ? "on" : off ? "off" : "neither";
}

// ── router helpers ───────────────────────────────────────────────────────────────────────────────

/** A note-on/off/CC message, built the way a keyboard would send it. */
static MidiInMessage chan_msg(uint8_t status, uint8_t channel, uint8_t d1, uint8_t d2) {
    MidiInMessage m;
    m.status  = status;
    m.channel = channel;
    m.data1   = d1;
    m.data2   = d2;
    m.len     = (status == songcore::EV_PROGRAM) ? 2 : 3;
    return m;
}

static float f32(uint32_t bits) { return songcore::f32_from_bits(bits); }
static uint32_t f32_bits_(float v) { return songcore::f32_bits(v); }

// ── the E2 observer ──────────────────────────────────────────────────────────────────────────────
/**
 * What `SongcoreHost` hands out after it has drained, parsed and routed (block 9).
 *
 * ⚠️ It records messages that produced NO record too, because that is the case the host's own
 * `IMidiInObserver` contract is written for: a message that routed nowhere still arrived, and an
 * observer told only about the routed ones cannot tell a dead cable from an unmapped channel.
 */
struct RecordingMidiIn : songcore::IMidiInObserver {
    struct Seen {
        MidiInMessage      msg;
        std::vector<Event> events;
    };
    std::vector<Seen> seen;

    void on_midi_in(const MidiInMessage& m, const Event* ev, int n) override {
        Seen s;
        s.msg = m;
        for (int i = 0; i < n; ++i) s.events.push_back(ev[i]);
        seen.push_back(std::move(s));
    }

    /** Record `k` of message `i`, or a zeroed one — so a FAILED count check cannot become a crash. */
    Event event_at(size_t i, size_t k) const {
        if (i >= seen.size() || k >= seen[i].events.size()) return Event{};
        return seen[i].events[k];
    }
};

// ── block 10's instruments: the engine, in the unit the E4 claim is in ───────────────────────────

static constexpr int SR = 44100;

/**
 * One second of a steady 220 Hz tone in engine slot `sampleId`.
 *
 * ⚠️ A TONE and not a DC level: the master chain's high-pass and the limiter both act on DC, so a
 * constant would be measuring the DSP rather than the note. One second is long enough that nothing in
 * block 10 can end by the sample simply running out — which would read exactly like a working KIL.
 */
static void load_tone(AudioEngine& engine, int sampleId, int sampleRate) {
    std::vector<float> pcm(static_cast<size_t>(sampleRate));
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = 0.5f * std::sin(6.2831853f * 220.0f * static_cast<float>(i) /
                                 static_cast<float>(sampleRate));
    engine.loadSample(sampleId, pcm.data(), static_cast<int>(pcm.size()));
}

/** Render `frames` and return the loudest sample in them. THE instrument of block 10. */
static float render_peak(AudioEngine& engine, int frames, int sampleRate) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    engine.renderOffline(frames, buf.data(), sampleRate);
    float m = 0.0f;
    for (float v : buf) m = std::max(m, std::fabs(v));
    return m;
}

/**
 * Press or release a key: three bytes into the sink the way a backend's callback delivers them, then
 * one `poll`.
 *
 * ⚠️ **THE SINK AND NOT THE ROUTER**, deliberately — block 10's claim is about the whole path a real
 * cable takes, and a test that called `MidiInputRouter::route` itself would be asserting that the code
 * it just called does what it does.
 */
static void key(SongcoreHost& host, uint8_t status, uint8_t note, uint8_t velocity) {
    const uint8_t bytes[3] = {status, note, velocity};
    host.midi_in_sink().on_bytes(bytes, 3);
    host.poll();
}

int main() {
    std::printf("== ptmidiin - MIDI IN: the parser, the queue, the router (MIDI plan phase E1) ==\n\n");

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    // 1. THE PARSER — the wire, reported faithfully
    // ─────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("-- 1. parser: the plain cases --\n");
    {
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0x40});
        eq_int("plain note-on: message count", static_cast<long long>(v.size()), 1);
        ok(msg_at(v, 0).status == 0x90 && msg_at(v, 0).channel == 0 && msg_at(v, 0).data1 == 60 &&
               msg_at(v, 0).data2 == 64 && msg_at(v, 0).len == 3,
           "plain note-on: fields", describe(msg_at(v, 0)), "90 ch0 60 64 (len 3)");
        ok(note_verdict(msg_at(v, 0)) == "on", "plain note-on: predicates",
           note_verdict(msg_at(v, 0)), "on");
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0x85, 0x3C, 0x00});
        ok(msg_at(v, 0).status == 0x80 && msg_at(v, 0).channel == 5,
           "note-off status/channel split", describe(msg_at(v, 0)), "80 ch5 60 0 (len 3)");
        ok(note_verdict(msg_at(v, 0)) == "off", "note-off: predicates",
           note_verdict(msg_at(v, 0)), "off");
    }
    {
        // ⭐ THE CONVENTION EVERY CONTROLLER USES AND EVERY NAIVE PARSER MISSES. A note-on at velocity
        // zero is a note-off; a consumer switching on the status byte alone hears a note-on it never
        // answers, and an unanswered note-on on external gear sounds until the power is cut.
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0x00});
        ok(msg_at(v, 0).status == 0x90, "note-on vel 0: status is REPORTED as 0x90",
           describe(msg_at(v, 0)), "90 ch0 60 0 (len 3)");
        ok(note_verdict(msg_at(v, 0)) == "off",
           "note-on vel 0: IS a note-off by the predicate", note_verdict(msg_at(v, 0)), "off");
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0xC3, 0x07, 0xD2, 0x40});
        eq_int("two-byte messages: count", static_cast<long long>(v.size()), 2);
        ok(msg_at(v, 0).status == 0xC0 && msg_at(v, 0).channel == 3 && msg_at(v, 0).data1 == 7 &&
               msg_at(v, 0).len == 2,
           "program change: fields", describe(msg_at(v, 0)), "C0 ch3 7 0 (len 2)");
        ok(msg_at(v, 1).status == 0xD0 && msg_at(v, 1).channel == 2 && msg_at(v, 1).len == 2,
           "channel pressure: fields", describe(msg_at(v, 1)), "D0 ch2 64 0 (len 2)");
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0xE0, 0x00, 0x40});
        const int value14 = (static_cast<int>(msg_at(v, 0).data2) << 7) | msg_at(v, 0).data1;
        eq_int("pitch bend: LSB-first widens to centre", value14, 0x2000);
    }

    std::printf("\n-- 2. parser: running status --\n");
    {
        // 0x90 held across three note pairs — how a keyboard actually sends a fast passage. The last
        // pair is a release, sent as velocity 0, which is the whole reason running status is worth
        // having: the note-offs cost two bytes each instead of three.
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0x40, 0x3E, 0x50, 0x3C, 0x00});
        eq_int("running status: message count", static_cast<long long>(v.size()), 3);
        ok(msg_at(v, 1).status == 0x90 && msg_at(v, 1).data1 == 62 && msg_at(v, 1).data2 == 0x50,
           "running status: second message", describe(msg_at(v, 1)), "90 ch0 62 80 (len 3)");
        ok(note_verdict(msg_at(v, 2)) == "off", "running status: third is the release",
           note_verdict(msg_at(v, 2)), "off");
        eq_int("running status: no orphans", static_cast<long long>(p.orphan_bytes()), 0);
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0x3C, 0x40, 0x90, 0x3C, 0x40});
        eq_int("orphan data bytes before any status: count", static_cast<long long>(v.size()), 1);
        eq_int("orphan data bytes: counted, not swallowed",
               static_cast<long long>(p.orphan_bytes()), 2);
    }

    std::printf("\n-- 3. parser: real time interleaved (the one that bites) --\n");
    {
        // ⭐⭐ A CLOCK BYTE MAY LAND BETWEEN A STATUS BYTE AND ITS DATA. It is legal, every sequencer
        // sending sync does it, and a parser that resets on "any byte >= 0x80" eats the note it was in
        // the middle of. It only happens while a clock is running — i.e. only when the app is doing
        // the thing it was bought for, and never at a desk with a keyboard and no transport.
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0xF8, 0x40});
        eq_int("clock mid-message: message count", static_cast<long long>(v.size()), 2);
        ok(msg_at(v, 0).status == 0xF8 && msg_at(v, 0).len == 1 && msg_at(v, 0).is_realtime(),
           "clock mid-message: the clock arrives first, alone",
           describe(msg_at(v, 0)), "F8 ch0 0 0 (len 1)");
        ok(msg_at(v, 1).status == 0x90 && msg_at(v, 1).data1 == 60 && msg_at(v, 1).data2 == 0x40,
           "clock mid-message: THE NOTE SURVIVES INTACT",
           describe(msg_at(v, 1)), "90 ch0 60 64 (len 3)");
    }
    {
        // …and it must not disturb running status either.
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0x40, 0xFE, 0x3E, 0x40});
        eq_int("active sensing between messages: count", static_cast<long long>(v.size()), 3);
        ok(msg_at(v, 2).status == 0x90 && msg_at(v, 2).data1 == 62,
           "active sensing: running status survives", describe(msg_at(v, 2)), "90 ch0 62 64 (len 3)");
        eq_int("active sensing: no orphans", static_cast<long long>(p.orphan_bytes()), 0);
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0xFA, 0xF8, 0xFC});
        eq_int("transport bytes: three one-byte messages", static_cast<long long>(v.size()), 3);
        ok(msg_at(v, 0).status == 0xFA && msg_at(v, 2).status == 0xFC,
           "transport bytes: start and stop pass through",
           describe(msg_at(v, 0)) + " / " + describe(msg_at(v, 2)), "FA / FC");
    }

    std::printf("\n-- 4. parser: system common and SysEx --\n");
    {
        // System Common CANCELS running status (MIDI 1.0). The two bytes after the song select belong
        // to nothing and must be dropped, not folded into the note-on before them.
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0x40, 0xF3, 0x05, 0x3E, 0x40});
        eq_int("system common: message count", static_cast<long long>(v.size()), 2);
        ok(msg_at(v, 1).status == 0xF3 && msg_at(v, 1).data1 == 5 && msg_at(v, 1).len == 2,
           "system common: song select itself", describe(msg_at(v, 1)), "F3 ch0 5 0 (len 2)");
        eq_int("system common: CANCELS running status (2 orphans)",
               static_cast<long long>(p.orphan_bytes()), 2);
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0xF6, 0x90, 0x3C, 0x40});
        eq_int("tune request: zero-data message completes", static_cast<long long>(v.size()), 2);
        ok(msg_at(v, 0).status == 0xF6 && msg_at(v, 0).len == 1,
           "tune request: fields", describe(msg_at(v, 0)), "F6 ch0 0 0 (len 1)");
    }
    {
        // A SysEx body is full of bytes that look like data. None of them may become a message, and
        // none of them is an orphan — they are inside a message this parser deliberately skips.
        MidiParser p;
        auto v = parse_all(p, {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7, 0x90, 0x3C, 0x40});
        eq_int("sysex: body produces nothing, note after it survives",
               static_cast<long long>(v.size()), 1);
        ok(msg_at(v, 0).status == 0x90 && msg_at(v, 0).data1 == 60,
           "sysex: the note after F7", describe(msg_at(v, 0)), "90 ch0 60 64 (len 3)");
        eq_int("sysex: body is not counted as orphans", static_cast<long long>(p.orphan_bytes()), 0);
    }
    {
        MidiParser p;
        auto v = parse_all(p, {0x90, 0x3C, 0x40, 0xF0, 0x11, 0xF7, 0x3E, 0x40});
        eq_int("sysex: cancels running status too", static_cast<long long>(v.size()), 1);
        eq_int("sysex: the pair after it is orphaned",
               static_cast<long long>(p.orphan_bytes()), 2);
    }
    {
        MidiParser p;
        p.feed(0x90);
        p.feed(0x3C);
        p.reset();
        auto v = parse_all(p, {0x40});
        eq_int("reset(): a half-assembled message is forgotten",
               static_cast<long long>(v.size()), 0);
        eq_int("reset(): the leftover byte is an orphan",
               static_cast<long long>(p.orphan_bytes()), 1);
    }

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    // 5. THE QUEUE — the thread boundary
    // ─────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n-- 5. queue: round trip, wraparound, overflow --\n");
    {
        MidiInQueue q;
        const uint8_t in[] = {0x90, 0x3C, 0x40};
        q.on_bytes(in, 3);
        eq_int("queue: pending after push", q.pending(), 3);

        uint8_t outBuf[8] = {0};
        const int n = q.drain(outBuf, 8);
        eq_int("queue: drained count", n, 3);
        ok(hexbytes(outBuf, 3) == "90 3C 40", "queue: bytes survive in order",
           hexbytes(outBuf, 3), "90 3C 40");
        eq_int("queue: empty after drain", q.pending(), 0);
        eq_int("queue: nothing dropped", static_cast<long long>(q.dropped()), 0);
    }
    {
        // Force the ring index past CAPACITY so a wrap is actually exercised — a ring that only ever
        // ran forwards is a ring whose modulo has never been tested.
        MidiInQueue q;
        std::vector<uint8_t> chunk(700);
        for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<uint8_t>(i & 0x7F);

        std::vector<uint8_t> got;
        uint8_t buf[900];
        for (int round = 0; round < 3; ++round) {
            q.on_bytes(chunk.data(), static_cast<int>(chunk.size()));
            const int n = q.drain(buf, static_cast<int>(sizeof buf));
            got.insert(got.end(), buf, buf + n);
        }
        eq_int("queue: 2100 bytes across a wrap", static_cast<long long>(got.size()), 2100);
        bool exact = true;
        for (size_t i = 0; i < got.size(); ++i)
            if (got[i] != static_cast<uint8_t>((i % 700) & 0x7F)) exact = false;
        ok(exact, "queue: every byte identical across the wrap", exact ? "exact" : "corrupt", "exact");
        eq_int("queue: still nothing dropped", static_cast<long long>(q.dropped()), 0);
    }
    {
        // ⭐ Overflow is COUNTED, and the bytes it keeps are the OLDEST — the complete messages already
        // received, whose note-offs are still to come. Keeping the newest instead would be the one way
        // this file can strand a note.
        MidiInQueue q;
        std::vector<uint8_t> flood(MidiInQueue::CAPACITY + 50, 0x7F);
        for (int i = 0; i < 10; ++i) flood[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        q.on_bytes(flood.data(), static_cast<int>(flood.size()));

        eq_int("queue: overflow fills to capacity", q.pending(), MidiInQueue::CAPACITY);
        eq_int("queue: overflow counts the loss exactly", static_cast<long long>(q.dropped()), 50);

        uint8_t head[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        q.drain(head, 4);
        ok(hexbytes(head, 4) == "00 01 02 03", "queue: overflow keeps the OLDEST bytes",
           hexbytes(head, 4), "00 01 02 03");
    }
    {
        // ⚠️ **A REAL SECOND THREAD, because that is the one fact about this object no single-threaded
        // check can see** — delete the mutex and every case above stays green. And it asserts that the
        // two sides MET: B3's two-thread block passed five of six checks having run nothing at all.
        MidiInQueue q;
        std::atomic<bool> done{false};
        std::atomic<long long> pushed{0};

        std::thread producer([&] {
            const uint8_t m[3] = {0x90, 0x3C, 0x40};
            for (int i = 0; i < 20000; ++i) {
                q.on_bytes(m, 3);
                pushed += 3;
                if ((i & 0x3F) == 0) std::this_thread::yield();
            }
            done = true;
        });

        long long drained = 0;
        int drains = 0;
        uint8_t buf[256];
        while (!done || q.pending() > 0) {
            const int n = q.drain(buf, static_cast<int>(sizeof buf));
            if (n > 0) { drained += n; ++drains; }
            else std::this_thread::yield();
        }
        producer.join();

        ok(drains > 10, "two threads: the drain actually ran", std::to_string(drains) + " drains", "> 10");
        ok(pushed.load() == 60000, "two threads: the producer actually ran",
           std::to_string(pushed.load()), "60000");
        eq_int("two threads: every byte accounted for",
               drained + static_cast<long long>(q.dropped()), pushed.load());
    }

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    // 6. THE ROUTER — channel to track, track to instrument
    // ─────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n-- 6. router: the channel map --\n");
    {
        Project p = songcore::make_default_project();
        p.midiInputChannels[2] = 5;            // track 2 listens on channel 5, nothing else listens

        TrackInstruments ti;
        MidiInputRouter r;
        r.set_project(&p);
        r.set_track_instruments(&ti);
        r.set_fallback_instrument(7);

        Event ev[MidiInputRouter::MAX_EVENTS];

        eq_int("unmapped channel: no events", r.route(chan_msg(0x90, 9, 60, 64), 1000, ev, 8), 0);
        eq_int("unmapped channel: counted as unmapped", static_cast<long long>(r.unmapped()), 1);

        const int n = r.route(chan_msg(0x90, 5, 60, 64), 1000, ev, 8);
        eq_int("mapped channel: one event", n, 1);
        eq_int("mapped channel: lands on track 2", ev[0].track, 2);
        eq_int("mapped channel: type is note-on", ev[0].type, songcore::EV_NOTE_ON);
        eq_int("mapped channel: frame is the caller's", static_cast<long long>(ev[0].frame), 1000);
        eq_int("mapped channel: fallback instrument used", ev[0].instrument, 7);
    }
    {
        // Two tracks on one channel is a layer, not a mistake — and it is the case a `return` after the
        // first match would silently break.
        Project p = songcore::make_default_project();
        p.midiInputChannels[0] = 3;
        p.midiInputChannels[6] = 3;

        TrackInstruments ti;
        MidiInputRouter r;
        r.set_project(&p);
        r.set_track_instruments(&ti);
        r.set_fallback_instrument(1);

        Event ev[MidiInputRouter::MAX_EVENTS];
        const int n = r.route(chan_msg(0x90, 3, 60, 64), 0, ev, MidiInputRouter::MAX_EVENTS);
        eq_int("fan-out: two tracks share a channel", n, 2);
        ok(ev[0].track == 0 && ev[1].track == 6, "fan-out: both tracks, in order",
           std::to_string(ev[0].track) + "," + std::to_string(ev[1].track), "0,6");
    }
    {
        // ⭐ THE INSTRUMENT QUESTION. `TrackInstruments` — the shared rule both existing consumers ask
        // — wins once the sequencer has played something on the track; the UI's instrument is what
        // makes the feature work before that, on a stopped song, which is how everyone tries it first.
        Project p = songcore::make_default_project();
        p.midiInputChannels[1] = 0;

        TrackInstruments ti;
        MidiInputRouter r;
        r.set_project(&p);
        r.set_track_instruments(&ti);
        r.set_fallback_instrument(9);

        Event ev[MidiInputRouter::MAX_EVENTS];
        r.route(chan_msg(0x90, 0, 60, 64), 0, ev, 8);
        eq_int("instrument: fallback before the sequencer has played", ev[0].instrument, 9);

        Event seq{};
        seq.type = songcore::EV_NOTE_ON;
        seq.track = 1;
        seq.instrument = 33;
        ti.observe(seq);

        r.route(chan_msg(0x90, 0, 60, 64), 0, ev, 8);
        eq_int("instrument: the track's own wins once it has one", ev[0].instrument, 33);

        r.set_fallback_instrument(-1);
        TrackInstruments empty;
        r.set_track_instruments(&empty);
        eq_int("instrument: none at all means no event", r.route(chan_msg(0x90, 0, 60, 64), 0, ev, 8), 0);
        eq_int("instrument: counted as noInstrument", static_cast<long long>(r.noInstrument()), 1);
    }

    std::printf("\n-- 7. router: the record it builds --\n");
    {
        Project p = songcore::make_default_project();
        p.midiInputChannels[0] = 0;
        p.instruments[4].volume = 0xFF;
        p.instruments[4].pan    = 0x80;

        TrackInstruments ti;
        MidiInputRouter r;
        r.set_project(&p);
        r.set_track_instruments(&ti);
        r.set_fallback_instrument(4);

        Event ev[MidiInputRouter::MAX_EVENTS];

        r.route(chan_msg(0x90, 0, 60, 100), 0, ev, 8);
        eq_int("note-on: note is the wire's own byte", ev[0].noteOn.note, 60);
        eq_int("note-on: velocity byte carried verbatim", ev[0].noteOn.velocity, 100);
        // The SCHEDULER's wiring, copied: velGain is the velocity CURVE (v/127)², volGain the
        // instrument's volume. Getting these two the wrong way round is precisely B5's bug.
        const float wantCurve = (100.0f / 127.0f) * (100.0f / 127.0f);
        ok(std::abs(f32(ev[0].noteOn.velGainBits) - wantCurve) < 1e-6f,
           "note-on: velGain is the velocity curve", std::to_string(f32(ev[0].noteOn.velGainBits)),
           std::to_string(wantCurve));
        ok(std::abs(f32(ev[0].noteOn.volGainBits) - 1.0f) < 1e-6f,
           "note-on: volGain is the instrument volume", std::to_string(f32(ev[0].noteOn.volGainBits)),
           "1.0");
        eq_int("note-on: no phrase behind it (transpose 0)", ev[0].noteOn.transpose, 0);
        eq_int("note-on: instrument's own table", ev[0].noteOn.tableId, -1);

        // A release, however the keyboard spells it, is a track-scoped NoteOff — the same SHAPE the
        // sequencer's own KIL puts on the bus, and since E4 a different MODE. ⚠️ `NOTE_OFF_KEY`, not
        // `NOTE_OFF_RELEASE`: a KIL fades a one-shot and a key release must not, and the two are told
        // apart HERE or nowhere (block 10 is where that difference is heard).
        r.route(chan_msg(0x90, 0, 60, 0), 0, ev, 8);
        eq_int("release as vel-0 note-on: type", ev[0].type, songcore::EV_NOTE_OFF);
        eq_int("release as vel-0 note-on: track-scoped", ev[0].instrument, songcore::INSTRUMENT_NONE);
        eq_int("⭐ release as vel-0 note-on: mode is KEY, not a KIL's RELEASE",
               ev[0].noteOff.mode, songcore::NOTE_OFF_KEY);
        r.route(chan_msg(0x80, 0, 60, 64), 0, ev, 8);
        eq_int("release as a real note-off: same record", ev[0].type, songcore::EV_NOTE_OFF);

        r.route(chan_msg(0xB0, 0, 10, 127), 0, ev, 8);
        eq_int("CC: type", ev[0].type, songcore::EV_CC);
        eq_int("CC: controller number passes through literally", ev[0].cc.param, 10);
        ok(std::abs(f32(ev[0].cc.valueBits) - 1.0f) < 1e-6f, "CC: 127 widens to 1.0",
           std::to_string(f32(ev[0].cc.valueBits)), "1.0");

        r.route(chan_msg(0xC0, 0, 41, 0), 0, ev, 8);
        eq_int("program change: type", ev[0].type, songcore::EV_PROGRAM);
        eq_int("program change: value", ev[0].program.program, 41);

        r.route(chan_msg(0xE0, 0, 0x00, 0x40), 0, ev, 8);
        eq_int("pitch bend: type", ev[0].type, songcore::EV_PITCH_BEND);
        eq_int("pitch bend: centre", ev[0].pitchBend.value14, 0x2000);

        // ⚠️ Explicit drops, each counted under its OWN reason — "nothing happened" has four different
        // fixes and a single counter cannot tell you which one you need.
        const uint64_t unsupportedBefore = r.unsupported();
        eq_int("aftertouch: no bus form, no event", r.route(chan_msg(0xA0, 0, 60, 64), 0, ev, 8), 0);
        eq_int("channel pressure: no bus form, no event", r.route(chan_msg(0xD0, 0, 64, 0), 0, ev, 8), 0);
        eq_int("aftertouch: counted as unsupported",
               static_cast<long long>(r.unsupported() - unsupportedBefore), 2);

        MidiInMessage clock;
        clock.status = 0xF8;
        clock.len    = 1;
        const uint64_t nonChannelBefore = r.nonChannel();
        eq_int("clock byte: routed nowhere (SYNC IN is deferred)", r.route(clock, 0, ev, 8), 0);
        eq_int("clock byte: counted, so a future SYNC IN can find it",
               static_cast<long long>(r.nonChannel() - nonChannelBefore), 1);
    }

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    // 8. THE JOIN — bytes in, bytes out, through the consumer neither half was written for
    // ─────────────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n-- 8. join: a keyboard plays an EXTERNAL instrument (MIDI thru) --\n");
    {
        RecordingMidiOut out;                       // ⚠️ before the host — see the note on the struct
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);

        Project& p = host.edit_project();
        p = songcore::make_default_project();
        p.midiInputChannels[0] = 0;                 // track 0 listens on channel 1 (0-based 0)

        Instrument& ext = p.instruments[2];
        ext.instrumentType = InstrumentType::EXTERNAL;
        ext.midiChannel    = 3;
        ext.midiProgram    = 41;
        ext.volume         = 0xFF;
        ext.pan            = 0x80;
        // ⚠️ A sampleFilePath, and it is load-bearing exactly as it is in ptmidi's B5 fixture: without
        // one the note is dropped on the empty-slot convention anyway, and the routing check below
        // would pass by construction with the gate deleted.
        ext.sampleFilePath = std::string("fixture.wav");

        TrackInstruments ti;
        MidiInputRouter router;
        router.set_project(&p);
        router.set_track_instruments(&ti);
        router.set_fallback_instrument(2);

        // The real chain: a backend's bytes → the queue → the parser → the router → the consumer.
        // Running status, because that is how a keyboard sends a note and its release.
        MidiInQueue queue;
        const uint8_t wire[] = {0x90, 0x3C, 0x40, 0x3C, 0x00};
        queue.on_bytes(wire, static_cast<int>(sizeof wire));

        MidiParser parser;
        Event ev[MidiInputRouter::MAX_EVENTS];
        uint8_t buf[64];
        int64_t frame = 1000;
        const int got = queue.drain(buf, static_cast<int>(sizeof buf));
        eq_int("join: bytes drained", got, 5);
        for (int i = 0; i < got; ++i) {
            if (!parser.feed(buf[i])) continue;
            const int n = router.route(parser.message(), frame, ev, MidiInputRouter::MAX_EVENTS);
            for (int k = 0; k < n; ++k) host.midi_out().consume(ev[k]);
            frame += 100;
        }
        host.midi_out().pump(INT64_MAX / 4);
        dump(out, "what left on the wire");

        // Channel 3 → low nibble 3. Program 41 = 0x29. Pan 0x80 → 128/255 → to7bit = 0x40.
        ok(at(out, 0) == "C3 29", "join: the patch goes out first", at(out, 0), "C3 29");
        ok(at(out, 1) == "B3 0A 40", "join: CC 10 pan", at(out, 1), "B3 0A 40");
        // ⭐⭐ THE ANCHOR: velocity 0x40 was pressed, velocity 0x40 leaves. The in-path squares it into
        // a curve and the out-path takes the square root; neither can check itself, and this number
        // came from outside both.
        ok(at(out, 2) == "93 3C 40", "join: VELOCITY 64 IN, VELOCITY 64 OUT", at(out, 2), "93 3C 40");
        ok(at(out, 3) == "83 3C 00", "join: the release reaches the cable", at(out, 3), "83 3C 00");
        eq_int("join: nothing else was sent", static_cast<long long>(out.log.size()), 4);
    }
    {
        // The mirror, and it is the half no single consumer can see: a SAMPLER instrument driven from
        // the keyboard puts NO byte on the cable. Same router, same bytes, one field different.
        RecordingMidiOut out;
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);

        Project& p = host.edit_project();
        p = songcore::make_default_project();
        p.midiInputChannels[0] = 0;
        p.instruments[2].sampleFilePath = std::string("fixture.wav");   // a real sampler slot

        TrackInstruments ti;
        MidiInputRouter router;
        router.set_project(&p);
        router.set_track_instruments(&ti);
        router.set_fallback_instrument(2);

        Event ev[MidiInputRouter::MAX_EVENTS];
        const int n = router.route(chan_msg(0x90, 0, 60, 64), 1000, ev, MidiInputRouter::MAX_EVENTS);
        eq_int("routing gate: the router still produces the record", n, 1);
        for (int k = 0; k < n; ++k) host.midi_out().consume(ev[k]);
        host.midi_out().pump(INT64_MAX / 4);
        eq_int("routing gate: a SAMPLER note puts NO byte on the cable",
               static_cast<long long>(out.log.size()), 0);
    }

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    // 9. E2 — THE HOST OWNS THE DRAIN: a backend's bytes reach the bus without the shell's help
    // ─────────────────────────────────────────────────────────────────────────────────────────────
    //
    // Everything above this line is the protocol half (E1) driven by hand: the test itself called the
    // queue, the parser and the router in turn. E2 moved that sequence into `SongcoreHost::poll()`, and
    // ⚠️ **the sequence being right where the test drives it says NOTHING about the app doing it** —
    // three platforms' backends will each hand bytes to the same sink, and if the drain lived in the
    // shell there would be three drains diverging in one of them.
    //
    // So this block touches nothing but the HOST. It delivers bytes the way a winmm callback does — one
    // `on_bytes` on the sink the shell hands to `IMidiIn::set_sink` — and then only calls `poll()`.
    std::printf("\n-- 9. E2: SongcoreHost drains, parses and routes on poll() --\n");
    {
        RecordingMidiOut out;                       // ⚠️ before the host, as everywhere in this file
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);

        RecordingMidiIn observer;
        host.set_midi_in_observer(&observer);

        Project& p = host.edit_project();
        p = songcore::make_default_project();
        p.midiInputChannels[0] = 0;                 // track 0 listens on channel 1

        Instrument& ext = p.instruments[2];
        ext.instrumentType = InstrumentType::EXTERNAL;
        ext.midiChannel    = 3;
        ext.sampleFilePath = std::string("fixture.wav");   // ptmidi's B5 reason, block 8's note
        p.instruments[5].sampleFilePath = std::string("other.wav");

        // The UI's current instrument — what a live key plays on a track the sequencer has not touched.
        host.set_midi_in_instrument(2);

        // ── (a) the whole path, driven by nothing but a backend's `on_bytes` and a `poll` ──────────
        const uint8_t wire[] = {0x90, 0x3C, 0x40, 0x3C, 0x00};   // note-on + release, running status
        host.midi_in_sink().on_bytes(wire, static_cast<int>(sizeof wire));
        eq_int("E2: nothing is parsed before the poll", static_cast<long long>(observer.seen.size()), 0);
        eq_int("E2: …and the bytes are parked in the queue", host.midi_in_sink().pending(), 5);

        host.poll();
        eq_int("⭐ E2: poll() drained the queue", static_cast<long long>(host.midi_in_bytes()), 5);
        eq_int("⭐ E2: …parsed both messages (running status, as a keyboard sends it)",
               static_cast<long long>(host.midi_in_messages()), 2);
        eq_int("E2: the observer saw both", static_cast<long long>(observer.seen.size()), 2);
        eq_int("E2: the queue is empty afterwards", host.midi_in_sink().pending(), 0);

        // ⚠️ The LEAD-IN, and it is the one number this block adds that nothing else could check: a
        // live key has no lookahead, so a record stamped with the clock the frame just read is a record
        // in the immediate past. `preview_note`'s +100 frames, asked of the host's own clock.
        const int64_t clock = host.sequencer().clock();
        eq_int("⭐ E2: the record is stamped with the clock PLUS the preview lead-in",
               observer.event_at(0, 0).frame - clock, 100);
        // ⚠️ **AND THE SUBTRACTION ABOVE IS NOT ENOUGH ON ITS OWN, WHICH IS WHY THIS CASE EXISTS.** A
        // fresh engine sits at frame 0, so `frame - clock == 100` is equally true of a host that stamps
        // the LEAD ALONE and never reads the clock at all — the check would pass by construction, and
        // the number being round is exactly the reason to suspect it. A host with NO engine keeps
        // whatever clock a test puts in it, so this asks the question at a frame nobody could hardcode.
        {
            SongcoreHost headless(nullptr, 44100);
            RecordingMidiIn obs2;
            headless.set_midi_in_observer(&obs2);
            Project& hp = headless.edit_project();
            hp = songcore::make_default_project();
            hp.midiInputChannels[0] = 0;
            hp.instruments[1].sampleFilePath = std::string("fixture.wav");
            headless.set_midi_in_instrument(1);
            headless.sequencer().set_clock(50000);
            headless.midi_in_sink().on_bytes(wire, 3);
            headless.poll();
            eq_int("⭐ E2: …at a clock nobody could hardcode — 50000 + 100",
                   obs2.event_at(0, 0).frame, 50100);
        }

        // ── (b) ⭐⭐ WHOSE NOTE IS THIS — the host wires the CONSUMER's TrackInstruments, not its own ──
        //
        // The fallback answers while the sequencer has said nothing about track 0. The moment a real
        // note-on for another instrument passes through the bus, the SAME `TrackInstruments` the cable's
        // consumer keeps must answer instead — and that wiring lives in one line of the host's
        // constructor which nothing else in this repo can see. A host that kept a private, unfed copy
        // passes every other check in this file and answers `2` here forever.
        eq_int("E2: with the sequencer silent, the key plays the instrument the UI is showing",
               static_cast<int>(observer.event_at(0, 0).instrument), 2);

        Event seq{};
        seq.type       = songcore::EV_NOTE_ON;
        seq.frame      = clock;
        seq.track      = 0;
        seq.instrument = 5;
        seq.noteOn.note = 48;
        seq.noteOn.velocity = 100;
        seq.noteOn.velGainBits = seq.noteOn.volGainBits = seq.noteOn.panBits = f32_bits_(1.0f);
        seq.noteOn.start = -1; seq.noteOn.slice = -1; seq.noteOn.tableId = -1; seq.noteOn.tableRow = -1;
        host.midi_out().consume(seq);               // the bus learns: track 0 is playing instrument 5

        observer.seen.clear();
        host.midi_in_sink().on_bytes(wire, 3);      // the same key, pressed again
        host.poll();
        eq_int("⭐⭐ E2: once the SEQUENCER owns the track, the key plays THAT instrument",
               static_cast<int>(observer.event_at(0, 0).instrument), 5);

        // ── (c) a cable pulled mid-message: `reset_midi_in` is what stops a phantom note ───────────
        //
        // ⚠️ Two bytes in, then the port goes away. Without the reset the parser holds `90 3C` and the
        // NEXT port's first data byte completes a note nobody played, on the old device's channel — and
        // it is a note-ON, so it sounds until something answers it.
        observer.seen.clear();
        const uint64_t messagesBefore = host.midi_in_messages();
        const uint8_t half[] = {0x90, 0x3C};
        host.midi_in_sink().on_bytes(half, 2);
        host.poll();
        eq_int("E2: half a message completes nothing", static_cast<long long>(observer.seen.size()), 0);

        host.reset_midi_in();
        const uint8_t stray[] = {0x40};             // what the next port's first byte would be
        host.midi_in_sink().on_bytes(stray, 1);
        host.poll();
        eq_int("⭐ E2: after reset_midi_in the orphan completes NO note",
               static_cast<long long>(host.midi_in_messages() - messagesBefore), 0);
        ok(host.midi_in_parser().orphan_bytes() > 0, "E2: …and it is counted as an orphan, not lost",
           std::to_string(host.midi_in_parser().orphan_bytes()), "> 0");

        // ── (d) the four reasons for silence, asked of the host ────────────────────────────────────
        //
        // A keyboard that produces nothing has four completely different fixes, and the host's counters
        // are the only thing in the app that can say which — the shell's exit report prints all of them.
        host.reset_midi_in();
        host.set_midi_in_instrument(-1);            // nothing selected: no instrument to play
        const uint64_t noInstBefore = host.midi_in_router().noInstrument();
        p.midiInputChannels[0] = -1;                // …and no track mapped either
        const uint64_t unmappedBefore = host.midi_in_router().unmapped();
        host.midi_in_sink().on_bytes(wire, 3);
        host.poll();
        eq_int("E2: an unmapped channel is counted as UNMAPPED, not merely dropped",
               static_cast<long long>(host.midi_in_router().unmapped() - unmappedBefore), 1);

        // ⚠️ TRACK 1, not track 0, and the first draft of this check got it wrong in a way worth
        // keeping: track 0 had been taught above that it plays instrument 5, so it HAD an instrument
        // and the check read 0 with the code perfectly correct. A track with no history is the only
        // one for which the fallback is the whole answer.
        p.midiInputChannels[1] = 0;                 // mapped, but nothing to play it on
        host.midi_in_sink().on_bytes(wire, 3);
        host.poll();
        eq_int("E2: …and a mapped track with no instrument is counted separately",
               static_cast<long long>(host.midi_in_router().noInstrument() - noInstBefore), 1);

        // ── (e) ⚠️ what E2 does NOT do, stated as a check so E4 has to change it deliberately ──────
        //
        // Nothing consumes the records yet: the injection into the engine (and the decision about
        // whether a live key rides `MidiRouter` at all) is E4's, and it is where the §4.1 note-off rule
        // finally has a key release to apply to. The cable therefore stays silent — and this assertion
        // is what makes that a decision on the record rather than an omission somebody has to notice.
        // ── (e) ⭐ E4 CHANGED THIS ASSERTION DELIBERATELY, WHICH IS WHY IT WAS WRITTEN ─────────────
        //
        // E2 asserted that a live key put NO record anywhere: `injected` was 0 by construction, and the
        // line existed so that the increment which changed it had to come and edit this file rather
        // than discover the silence had quietly ended. It did. Track 0's instrument here is EXTERNAL
        // (instrument 2), so with THRU on — the default — the record reaches the cable's consumer.
        //
        // ⚠️ `out.log` is still EMPTY, and that is not a contradiction: `ExternalConsumer` QUEUES a
        // message at its due frame and `pump` releases it. This host's engine never processes a block,
        // so its clock sits at 0 while the record is stamped 100 — the byte is owed, not sent. Block 10
        // advances a real clock and reads the wire.
        eq_int("⭐ E4: the key IS injected now (E2 asserted 0 here on purpose)",
               static_cast<long long>(host.midi_in_injected()), 3);
        // ⚠️ **2 OF THE 3, AND THE ODD ONE OUT IS THE CHECK.** Records 1 and 2 (the note-on and its
        // release) resolved to instrument 2, which is EXTERNAL. Record 3 came after the SEQUENCER
        // taught the bus that track 0 is playing instrument 5 — a SAMPLER — so it is not a thru record
        // at all. A counter that read 3 here would be counting records handed to `ExternalConsumer`
        // rather than records that mean something to a cable, and the routing gate would be invisible.
        eq_int("⭐ E4: …and the EXTERNAL records (2 of the 3) went to the cable's consumer",
               static_cast<long long>(host.midi_in_thru_sent()), 2);
        eq_int("E4: nothing was withheld — thru is on by default",
               static_cast<long long>(host.midi_in_thru_suppressed()), 0);
        eq_int("E4: …and none of it has been RELEASED yet (the clock is still at 0)",
               static_cast<long long>(out.log.size()), 0);
    }

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    // 10. E4 — INJECTION: a live key raises a voice, and letting go of it does the RIGHT thing
    // ─────────────────────────────────────────────────────────────────────────────────────────────
    //
    // ⭐⭐ **THE CLAIM IS IN A UNIT NOTHING ABOVE THIS LINE CAN READ.** Every block so far ends at a bus
    // record: the right fields, on the right track, for the right instrument. E4's claim is that the
    // key is HEARD — and "a record was produced" cannot say that, because the routing gate, the
    // instrument's empty-slot convention and the note path all sit between the record and the sound.
    // So this block renders REAL AUDIO out of a REAL AudioEngine and measures the peak, which is the
    // only instrument that reads the unit the claim is in.
    //
    // ⚠️ And every reading is paired with the silence before it. A `peak > 0` on its own is equally
    // true of an engine humming to itself; the number that means something is the pair.
    std::printf("\n-- 10. E4: the injection — the key reaches the engine, and the §4.1 release rule --\n");
    {
        RecordingMidiOut out;                       // ⚠️ before the host, as everywhere in this file
        auto engine = std::make_unique<AudioEngine>();
        engine->setDeviceSampleRate(SR);
        SongcoreHost host(engine.get(), SR);
        host.set_midi_out(&out);

        Project& p = host.edit_project();
        p = songcore::make_default_project();
        p.midiInputChannels[0] = 0;                 // track 0 ← channel 1 (the one-shot)
        p.midiInputChannels[1] = 1;                 // track 1 ← channel 2 (the EXTERNAL instrument)
        p.midiInputChannels[2] = 2;                 // track 2 ← channel 3 (the ADSR one)

        // ⚠️ A ONE-SHOT: no loop, no ADSR, and a `sampleFilePath` because that string is the ONLY
        // "this slot is not empty" signal in the model — without it `derive_sampler_note` drops the
        // note and every check below would pass or fail for the wrong reason.
        Instrument& oneShot = p.instruments[1];
        oneShot.sampleFilePath = std::string("one-shot.wav");
        oneShot.loopMode       = "off";
        load_tone(*engine, oneShot.sampleId, SR);   // 1 s of steady tone — long enough to outlive a fade

        // The same, with an ADSR VOL envelope that HAS a release. `Voice::keyRelease` promotes it.
        Instrument& adsr = p.instruments[3];
        adsr.sampleFilePath = std::string("pad.wav");
        adsr.loopMode       = "off";
        adsr.modSlots[0].type    = songcore::ModType::ADSR;
        adsr.modSlots[0].dest    = songcore::ModDest::VOLUME;
        adsr.modSlots[0].amount  = 0xFF;
        adsr.modSlots[0].attack  = 0x00;
        adsr.modSlots[0].decay   = 0x00;
        adsr.modSlots[0].sustain = 0xFF;            // hold full level until the key is let go of
        // ⚠️ **TWO TICS, AND THE FIRST DRAFT USED 0x40 = 64.** The release time is `tics × framesPerTic`
        // (voice_derive.h) — at 120 BPM that is ~918 frames a tic, so 64 tics is 1.3 SECONDS and a
        // 8192-frame look at it read the same peak to six digits as the note itself. The check was
        // measuring the first millisecond of a very slow fall and calling it "no release at all".
        // ⭐ Match the window to the quantity, or the instrument reports a working fix as a failure.
        adsr.modSlots[0].release = 0x02;            // ~42 ms — over well inside one look
        load_tone(*engine, adsr.sampleId, SR);

        Instrument& ext = p.instruments[5];
        ext.instrumentType = InstrumentType::EXTERNAL;
        ext.midiChannel    = 3;
        ext.sampleFilePath = std::string("gear.wav");   // ptmidi's B5 reason — see block 8

        // ── (a) a live key RAISES A VOICE ─────────────────────────────────────────────────────────
        host.set_midi_in_instrument(1);
        eq_int("E4: nothing is sounding before the key", engine->getActiveVoiceCount(), 0);
        const float silence = render_peak(*engine, 1024, SR);
        ok(silence == 0.0f, "E4: …and the engine renders SILENCE", std::to_string(silence), "0");

        key(host, 0x90, 60, 64);                    // note-on, channel 1, C4, velocity 64
        eq_int("⭐ E4: the record was injected", static_cast<long long>(host.midi_in_injected()), 1);
        const float sounding = render_peak(*engine, 1024, SR);
        ok(sounding > 0.0f, "⭐⭐ E4: THE KEY IS HEARD — the engine renders audio",
           std::to_string(sounding), "> 0");
        eq_int("⭐ E4: …on a voice, on the track the map named", engine->getActiveVoiceCount(), 1);

        // ── (b) ⭐⭐ §4.1: LETTING GO OF A KEY DOES NOT CUT A ONE-SHOT ──────────────────────────────
        //
        // The rule the plan has carried since §4.1 was written, and E4 is the first increment with a
        // key release to apply it to: a drum hit plays out. `NOTE_OFF_KEY` → `scheduleKeyRelease` →
        // `SamplerVoice::keyRelease`, whose one-shot arm does NOTHING.
        key(host, 0x90, 60, 0);                     // the release, as every controller sends it
        // ⚠️⚠️ **THE FIRST WINDOW IS DISCARDED, AND A CONTROL IS WHAT PROVED IT HAS TO BE.** A kill
        // fade is `KILL_FADE_SAMPLES` = 256 frames, so a 4096-frame peak taken FROM the release still
        // contains the note at full level whether the voice was cut or not: with the §4.1 arm deleted
        // this check read **0.0566** against a healthy 0.0630 and passed, and only the voice count
        // failed. A peak is the loudest sample in a window, so the window has to start after the thing
        // being ruled out would have finished.
        // ⭐ The metric is part of the test — a window aligned with the event it is measuring answers a
        // different question than the one asked.
        render_peak(*engine, 2048, SR);             // any cut would be complete inside this
        const float afterRelease = render_peak(*engine, 4096, SR);
        ok(afterRelease > 0.0f, "⭐⭐ E4/§4.1: a released ONE-SHOT keeps playing",
           std::to_string(afterRelease), "> 0");
        eq_int("E4/§4.1: …and its voice is still allocated", engine->getActiveVoiceCount(), 1);

        // ── (c) ⭐⭐ …AND A KIL ON THE SAME VOICE STILL CUTS IT ─────────────────────────────────────
        //
        // **THE PAIR IS THE CHECK.** (b) alone is equally true of an engine that ignores every note-off
        // ever sent — including KIL's, which would be a silent regression in the sequencer, in a
        // channel no golden trace can see (the traces stop above the consumer). So the same voice, in
        // the same state, is now handed the OTHER call — the one the KIL arm makes — and it must go
        // quiet. Two modes, one voice, opposite outcomes: that is what makes E4 an addition rather
        // than a weakening.
        engine->scheduleNoteOff(engine->getCurrentFrame(), 0);   // exactly what NOTE_OFF_RELEASE does
        render_peak(*engine, 2048, SR);                          // the same discarded window, same reason
        const float afterKil = render_peak(*engine, 4096, SR);
        ok(afterKil == 0.0f && engine->getActiveVoiceCount() == 0,
           "⭐⭐ E4/§4.1: a KIL on the SAME voice DOES cut it",
           std::to_string(afterKil) + " peak, " + std::to_string(engine->getActiveVoiceCount()) +
               " voice(s)", "0 peak, 0 voices");

        // ── (d) the ADSR arm: a release ENVELOPE runs, where the one-shot ignored the same message ──
        //
        // ⚠️⚠️ **A DIFFERENT TRACK, AND THE FIRST DRAFT'S BUG IS WORTH THE PARAGRAPH.** This was
        // written on track 0 with `set_midi_in_instrument(3)`, and it quietly played instrument **1**
        // — the one-shot — for the whole check: `TrackInstruments` had learned track 0 from the key in
        // (a), and the fallback only answers for a track with NO history (midi_in.h). The peak was 2.4×
        // the one-shot's, which looked like a different instrument and was only the velocity curve
        // (100 vs 64). The check then "failed" against perfectly correct release code.
        // ⭐ A fixture that silently substitutes another instrument is the purest form of a test
        // measuring the wrong object — so the observer below ASSERTS which instrument it got, and the
        // key is played on a track nothing has touched.
        host.set_midi_in_instrument(3);
        RecordingMidiIn who;
        host.set_midi_in_observer(&who);
        key(host, 0x92, 60, 100);                   // channel 3 → track 2, which has no history
        eq_int("⭐ E4: the ADSR key really is playing the ADSR instrument",
               static_cast<int>(who.event_at(0, 0).instrument), 3);
        eq_int("E4: …on the track its channel names", static_cast<int>(who.event_at(0, 0).track), 2);
        const float adsrOn = render_peak(*engine, 2048, SR);
        ok(adsrOn > 0.0f, "E4: the ADSR instrument sounds", std::to_string(adsrOn), "> 0");

        // ⚠️ Two renders, and the first one is DISCARDED on purpose. A peak is the loudest sample in a
        // block, so a block that STARTS at the moment of release always contains the pre-release level
        // — measure that and every release ever written looks like no release at all. The reading is
        // the block AFTER the envelope has had its ~42 ms to run.
        key(host, 0x92, 60, 0);
        render_peak(*engine, 4096, SR);                       // the release runs out inside this one
        const float adsrTail = render_peak(*engine, 4096, SR);
        ok(adsrTail < adsrOn * 0.5f, "⭐ E4/§4.1: …and letting the key go RELEASES it (the level falls)",
           std::to_string(adsrTail) + " after, " + std::to_string(adsrOn) + " during", "much lower");

        // ── (e) ⭐⭐ MIDI THRU, AND THE FEEDBACK LOOP IT WOULD BE ON A LOOPBACK ─────────────────────
        //
        // A key on a track whose instrument is EXTERNAL is MIDI thru — the obvious use of an input
        // port, and the thing that makes an input port worth having with hardware attached. It is also
        // an amplifying feedback loop the moment the port it goes out of is the port it came in on,
        // which is exactly the desk rig §0.8 uses. So the shell suppresses it there and the host holds
        // the verdict; both halves are asserted, because a flag with only its ON case checked is a
        // flag that is not read.
        host.set_midi_in_instrument(5);             // the EXTERNAL one, on track 1 (channel 2)
        const uint64_t sentBefore = host.midi_in_thru_sent();
        key(host, 0x91, 64, 100);
        eq_int("⭐ E4: with THRU on, an EXTERNAL key reaches the cable's consumer",
               static_cast<long long>(host.midi_in_thru_sent() - sentBefore), 1);

        // …and it really leaves: advance the clock past the record's frame and pump.
        render_peak(*engine, 4096, SR);
        host.poll();
        const size_t wire = out.log.size();
        ok(wire > 0, "⭐⭐ E4: …and the byte LEAVES on the wire — MIDI thru, end to end",
           std::to_string(wire) + " message(s)", "> 0");
        // ⚠️ **SEARCHED FOR, NOT INDEXED.** `out.log[0]` is a CC 10 — phase D sends the instrument's
        // pan (and program, when it has one) ahead of its first note-on, in that order. A check written
        // against index 0 asserts the phase-D preamble and calls it a note.
        // ⭐ And the CHANNEL is the claim: the record arrived on MIDI channel 2 (`0x91`) and leaves on
        // the INSTRUMENT's channel 4 (`0x93`). The keyboard's channel chooses the TRACK; the
        // instrument's chooses the wire — the two are unrelated, and a thru that echoed the input
        // channel would be a MIDI patchbay rather than a tracker.
        // ⚠️ The log holds FORMATTED TEXT, not bytes (`RecordingMidiOut::send`) — the first draft
        // indexed it as if the entries were byte vectors, found nothing, and reported "no note-on at
        // all" while `93 40 64` sat right there in the dump. The `got` printed beside the verdict is
        // what showed it, which is the whole reason it is printed.
        size_t noteIdx = wire;
        for (size_t i = 0; i < wire; ++i)
            if (!out.log[i].empty() && out.log[i][0] == '9') { noteIdx = i; break; }
        // Velocity too: the key was pressed at 100 (0x64) and the instrument's volume is 0xFF, so the
        // byte on the wire is the byte a human pressed — E1's anchor, now with the engine in the path.
        ok(noteIdx < wire && out.log[noteIdx] == "93 40 64",
           "⭐ E4: …a note-on on the INSTRUMENT's channel (4), at the velocity pressed",
           noteIdx < wire ? at(out, noteIdx) : std::string("no note-on at all"), "93 40 64");

        // The other half of the same flag. ⚠️ `suppressed` and not "no bytes": the record still reaches
        // the ENGINE consumer, which drops it on the routing gate — so the cable being quiet has two
        // possible causes and only the counter can say which one this is.
        host.set_midi_in_thru(false);
        const size_t wireBefore = out.log.size();
        const uint64_t supprBefore = host.midi_in_thru_suppressed();
        key(host, 0x91, 67, 100);
        eq_int("⭐⭐ E4: with THRU off, the same key is WITHHELD from the cable",
               static_cast<long long>(host.midi_in_thru_suppressed() - supprBefore), 1);
        render_peak(*engine, 4096, SR);
        host.poll();
        eq_int("E4: …and no byte follows it out", static_cast<long long>(out.log.size() - wireBefore), 0);
        eq_int("E4: …while the record was still injected (silence has a NAMED cause)",
               static_cast<long long>(host.midi_in_thru_sent() - sentBefore), 1);
    }

    std::printf("\n%s  (%d failure%s)\n", failures ? "SOME CHECKS FAILED" : "ALL GREEN", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
