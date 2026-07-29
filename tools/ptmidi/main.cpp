// ptmidi — the EXTERNAL MIDI-out path (MIDI plan phase B). Host tool, no device, no cable.
//
// ⚠️ **NOT A CONFORMANCE TOOL, and the line matters here more than anywhere else in tools/.** Every
// tool with an `sN-` ctest name byte-compares against a golden recorded from the REAL Kotlin code it
// replaces — they encode what the app DOES. This one cannot: **MIDI out never existed in Kotlin**, so
// there is nothing to record and no golden to be had. It is hand-written assertions in the ptdispatch
// / ptmapper tradition: it encodes what the author believes correct, having read the plan and the
// prior art. Read it as a specification with an executable body, not as proof of equivalence.
//
// What it does cover, and what nothing else can:
//
//   • **the byte stream** — the whole point. Nothing else in the tree can see a MIDI byte: ptplay
//     stops at the bus record, ptvoice at the engine call, ptrender at the audio. The serializer, the
//     0–1 → 0–127 scaling, the note folding and the velocity rule are invisible to all four.
//   • **the note LIFECYCLE**, which is where a MIDI bug actually costs something: a missing note-off
//     is a note that sounds until the gear is power-cycled. Gate-to-next, LEN, KIL and panic each get
//     a case, and the panic case asserts the CC 123 backstop as well as the per-note offs.
//   • **the ROUTING GATE, from both sides.** An EXTERNAL instrument must raise NO voice in the engine,
//     and a SAMPLER instrument must put NO byte on the cable. Neither half is observable from inside
//     one consumer, which is exactly why the gate is a model predicate both of them ask.
//
// ⚠️ The engine-side half is asserted through `SongcoreHost::track_mask()` — the bit EngineConsumer
// sets on its way into `plan_note_on`. That is a real observable and not a proxy: the gate returns
// BEFORE the bit is set, so a gate that silently stopped working lights the bit and this goes red.
//
// Determinism: the whole run is driven by `schedule_song_range` (the render scheduler), which walks
// the song synchronously with no clock at all, followed by one `pump` past the end of time. There is
// no wall clock, no audio device and no thread anywhere in this tool.
//
//   ctest --test-dir tools/build -R b1-midi-out --output-on-failure -C Release
//
// Exit code 0 = all green, 1 = any assertion failed.

#include "../../native/songcore/host.h"
#include "../../native/songcore/midi_out.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using songcore::Instrument;
using songcore::InstrumentType;
using songcore::MidiCcSlot;
using songcore::Note;
using songcore::Project;
using songcore::SongcoreHost;

static int failures = 0;

// ── the verdict printer ──────────────────────────────────────────────────────────────────────────
// The NUMBER beside the verdict, always: a bare PASS cannot tell a working check from one that
// compared two things which are both wrong.

static void ok(bool cond, const std::string& what, const std::string& got, const std::string& want) {
    if (cond) {
        std::printf("[PASS] %-46s %s\n", what.c_str(), got.c_str());
    } else {
        std::printf("[FAIL] %-46s got %s, want %s\n", what.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}

static void eq_int(const std::string& what, long long got, long long want) {
    ok(got == want, what, std::to_string(got), std::to_string(want));
}

// ── A recording port ─────────────────────────────────────────────────────────────────────────────
// The IMidiOut a test drives. It is the same interface the ALSA/winmm/MidiManager backends implement,
// so what this tool exercises is the real send path and not a parallel one.

struct RecordingMidiOut : songcore::IMidiOut {
    std::vector<std::string> log;   // "B3 4A 64", in send order
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

// ── the scenario builder ─────────────────────────────────────────────────────────────────────────

/**
 * Two tracks, one of each kind, so every run measures the routing gate as well as the bytes:
 *   track 0 → chain 0 → phrase 0, instrument 0 (SAMPLER)
 *   track 1 → chain 1 → phrase 1, instrument 1 (EXTERNAL, channel 3)
 */
static Project build_project(int midiLen) {
    Project p = songcore::make_default_project();
    p.tempo = 128;

    Instrument& ext = p.instruments[1];
    ext.instrumentType = InstrumentType::EXTERNAL;
    ext.midiChannel = 3;
    ext.midiProgram = 41;
    ext.midiLen     = midiLen;
    ext.midiCC[0]   = MidiCcSlot{74, 100};

    // phrase 0 — the SAMPLER track: one note, so the engine gate has something to let through.
    p.phrases[0].steps[0].note = Note{0, 4};      // C-4
    p.phrases[0].steps[0].instrument = 0;

    // phrase 1 — the EXTERNAL track: a loud note, then a quieter one four steps later.
    p.phrases[1].steps[0].note = Note{0, 4};      // C-4  = MIDI 60
    p.phrases[1].steps[0].instrument = 1;
    p.phrases[1].steps[0].volume = 0x7F;
    p.phrases[1].steps[4].note = Note{4, 4};      // E-4  = MIDI 64
    p.phrases[1].steps[4].instrument = 1;
    p.phrases[1].steps[4].volume = 0x40;

    p.chains[0].phraseRefs[0] = 0;
    p.chains[1].phraseRefs[0] = 1;
    p.tracks[0].chainRefs.push_back(0);
    p.tracks[1].chainRefs.push_back(1);
    return p;
}

/** Schedule song row 0 through the bus and release every queued message. */
static void run_song(SongcoreHost& host) {
    host.schedule_song_range(0, 0, nullptr);
    host.midi_out().pump(INT64_MAX / 4);   // past the end of time: nothing may stay queued
}

static void dump(const RecordingMidiOut& out, const char* label) {
    std::printf("  -- %s (%d messages) --\n", label, static_cast<int>(out.log.size()));
    for (size_t i = 0; i < out.log.size(); ++i) std::printf("     %2d  %s\n", static_cast<int>(i), out.log[i].c_str());
}

static std::string at(const RecordingMidiOut& out, size_t i) {
    return i < out.log.size() ? out.log[i] : std::string("<none>");
}

int main() {
    std::printf("== ptmidi — EXTERNAL MIDI out (MIDI plan phase B) ==\n\n");

    // ── 1. The scaling, in the one place it lives (plan §11) ─────────────────────────────────────
    std::printf("-- scaling (to7bit / byte_to_7bit) --\n");
    eq_int("to7bit(0.0)", songcore::to7bit(0.0f), 0);
    eq_int("to7bit(1.0)", songcore::to7bit(1.0f), 127);
    eq_int("to7bit(0.5)", songcore::to7bit(0.5f), 64);
    eq_int("to7bit(-1) clamps", songcore::to7bit(-1.0f), 0);
    eq_int("to7bit(2) clamps", songcore::to7bit(2.0f), 127);
    eq_int("byte_to_7bit(0x00)", songcore::byte_to_7bit(0x00), 0);
    eq_int("byte_to_7bit(0x80)", songcore::byte_to_7bit(0x80), 64);
    eq_int("byte_to_7bit(0xFF)", songcore::byte_to_7bit(0xFF), 127);

    // ── 2. Gate-to-next (midiLen 0): the stream, byte for byte ───────────────────────────────────
    std::printf("\n-- gate-to-next (midiLen 0) --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        RecordingMidiOut out;
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/0);

        run_song(host);
        dump(out, "byte stream");

        // The first note states its patch (program, then the CC-A default, then pan), and only then
        // sounds. Channel 3 → status low nibble 3. Program 41 = 0x29. CC 74 = 0x4A, value 100 = 0x64.
        // Pan: the instrument default 0x80 → 128/255 → to7bit → 64 = 0x40. Velocity 0x7F × VOL 1.0.
        ok(at(out, 0) == "C3 29",       "note 1: program change",  at(out, 0), "C3 29");
        ok(at(out, 1) == "B3 4A 64",    "note 1: CC A default",    at(out, 1), "B3 4A 64");
        ok(at(out, 2) == "B3 0A 40",    "note 1: CC 10 pan",       at(out, 2), "B3 0A 40");
        ok(at(out, 3) == "93 3C 7F",    "note 1: note-on C-4",     at(out, 3), "93 3C 7F");
        // The second note ENDS the first before it sounds — this is gate-to-next, and it is the only
        // thing that stops the note at all when LEN is 0.
        ok(at(out, 4) == "83 3C 00",    "note 2: off for note 1 first", at(out, 4), "83 3C 00");
        // Program is de-duplicated (the channel is already on it); the CC default is NOT — it is a
        // patch STARTING value and a phase-D CCA may have moved it since. Pan has not moved.
        ok(at(out, 5) == "B3 4A 64",    "note 2: CC A default resent", at(out, 5), "B3 4A 64");
        ok(at(out, 6) == "93 40 40",    "note 2: note-on E-4 vel 64", at(out, 6), "93 40 40");
        eq_int("messages before the stop", static_cast<long long>(out.log.size()), 7);

        // ⚠️ The routing gate, both sides, on the same run: the SAMPLER track reached the engine and
        // the EXTERNAL one did not. `track_mask` bit N is set by EngineConsumer on its way into the
        // note path — the gate returns before it.
        eq_int("engine saw track 0 (SAMPLER)", (host.track_mask() >> 0) & 1, 1);
        eq_int("engine did NOT see track 1 (EXTERNAL)", (host.track_mask() >> 1) & 1, 0);

        // The stop owes the cable everything still sounding: the note, then the channel backstop.
        const size_t before = out.log.size();
        host.stop();
        ok(at(out, before) == "83 40 00",     "stop: note-off for the ringing note", at(out, before), "83 40 00");
        ok(at(out, before + 1) == "B3 7B 00", "stop: CC 123 all-notes-off on ch 3",  at(out, before + 1), "B3 7B 00");
        eq_int("stop: nothing left queued", static_cast<long long>(host.midi_out().pending_count()), 0);
    }

    // ── 3. LEN gate: the note ends on its own, LEN tics after it started ─────────────────────────
    std::printf("\n-- LEN gate (midiLen 6 = half a step) --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        RecordingMidiOut out;
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/6);

        run_song(host);
        dump(out, "byte stream");

        // Now the first note ends on its OWN gate, before the second note is even scheduled — so the
        // off arrives at index 4 as a LEN off rather than as note 2's cut, and note 2 needs no cut.
        ok(at(out, 3) == "93 3C 7F", "note 1: note-on",        at(out, 3), "93 3C 7F");
        ok(at(out, 4) == "83 3C 00", "note 1: LEN off",        at(out, 4), "83 3C 00");
        ok(at(out, 5) == "B3 4A 64", "note 2: CC A default",   at(out, 5), "B3 4A 64");
        ok(at(out, 6) == "93 40 40", "note 2: note-on",        at(out, 6), "93 40 40");
        ok(at(out, 7) == "83 40 00", "note 2: LEN off",        at(out, 7), "83 40 00");
        eq_int("LEN run message count", static_cast<long long>(out.log.size()), 8);

        // Everything is already off, so the stop has nothing per-note to send — but the CC 123
        // backstop still goes out, because our bookkeeping is not the device's.
        const size_t before = out.log.size();
        host.stop();
        ok(at(out, before) == "B3 7B 00", "stop: CC 123 only", at(out, before), "B3 7B 00");
        eq_int("stop added exactly one message", static_cast<long long>(out.log.size() - before), 1);
    }

    // ── 3b. …and the gate fires at the RIGHT FRAME, which the run above cannot tell ──────────────
    //
    // ⚠️ **The stream in case 3 is byte-identical to case 2 up to its last message.** A LEN off and a
    // gate-to-next cut land in the same place in the log, so "there is an off at index 4" passes on
    // BOTH settings and is therefore not a check at all. What separates them is WHEN, and the only
    // way to see a `when` through an interface that carries no time is to move the clock by hand:
    // pump to one frame before the gate and assert the off has NOT been sent, then to the gate itself
    // and assert it has. That also happens to be the only test anywhere of the release queue.
    std::printf("\n-- the LEN gate's FRAME (and the release queue) --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        RecordingMidiOut out;
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/6);

        // tempo 128 @ 44100: framesPerStep = 60000/128/4 × 44.1 = 5167 (truncated), /12 = 430 per tic,
        // so a 6-tic gate closes 2580 frames after the note. Derived here the same way timing.h does.
        const int64_t fps  = songcore::frames_per_step(128, 44100);
        const int64_t fpt  = songcore::frames_per_tic(fps);
        const int64_t gate = 6 * fpt;
        eq_int("framesPerStep(128, 44100)", fps, 5167);
        eq_int("framesPerTic", fpt, 430);

        const int64_t base = engine->getCurrentFrame();   // where the schedule starts
        host.schedule_song_range(0, 0, nullptr);

        host.midi_out().pump(base);
        eq_int("at the note's own frame: patch + note-on only", static_cast<long long>(out.log.size()), 4);
        ok(at(out, 3) == "93 3C 7F", "…and the last of them is the note-on", at(out, 3), "93 3C 7F");

        host.midi_out().pump(base + gate - 1);
        eq_int("one frame before the gate: still nothing", static_cast<long long>(out.log.size()), 4);

        host.midi_out().pump(base + gate);
        eq_int("at the gate: the off arrives", static_cast<long long>(out.log.size()), 5);
        ok(at(out, 4) == "83 3C 00", "…and it is the note-off", at(out, 4), "83 3C 00");

        // OFFSET holds the whole stream back by its own milliseconds — positive = MIDI later.
        host.set_midi_offset_ms(100);
        const int64_t offsetFrames = 100 * 44100 / 1000;
        const size_t  soFar = out.log.size();
        host.midi_out().pump(base + gate + offsetFrames - 1);
        eq_int("with OFFSET +100 ms, the next group is held", static_cast<long long>(out.log.size() - soFar), 0);
        host.stop();
    }

    // ── 4. A project with NO external instrument puts NOTHING on the cable ────────────────────────
    std::printf("\n-- a purely internal project is silent on the wire --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        SongcoreHost host(engine.get(), 44100);
        RecordingMidiOut out;
        host.set_midi_out(&out);
        Project p = build_project(0);
        p.instruments[1].instrumentType = InstrumentType::SAMPLER;   // the ONE difference
        host.edit_project() = p;

        run_song(host);
        eq_int("messages sent", static_cast<long long>(out.log.size()), 0);
        // …and now the engine sees BOTH tracks, which is the same gate answering the other way. A
        // check that passes on both settings is not a check; this is the discriminator.
        eq_int("engine saw track 0", (host.track_mask() >> 0) & 1, 1);
        eq_int("engine saw track 1", (host.track_mask() >> 1) & 1, 1);
        host.stop();
        eq_int("stop sent nothing (no channel ever used)", static_cast<long long>(out.log.size()), 0);
    }

    std::printf("\n%s\n", failures == 0 ? "ALL GREEN" : ("FAILURES: " + std::to_string(failures)).c_str());
    return failures == 0 ? 0 : 1;
}
