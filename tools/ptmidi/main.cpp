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
// Determinism: the byte-stream blocks are driven by `schedule_song_range` (the render scheduler), which
// walks the song synchronously with no clock at all, followed by one `pump` past the end of time. There
// is no wall clock and no audio device anywhere in this tool.
//
// ⚠️ **AND SINCE B3 THERE IS ONE THREAD, in the last block only, and it is deliberately the one
// non-deterministic thing here.** `pump` now runs on a sender thread while `consume` runs on the frame
// loop, and that is the single fact about B3 no single-threaded check can see — delete every lock in
// `midi_out.h` and all 67 earlier checks stay green. So that block asserts INVARIANTS rather than bytes
// (no note re-struck while sounding, nothing left sounding, nothing left queued): the interleaving picks
// the order, so there is no expected string, but a stuck note is a stuck note however the threads raced.
// It also asserts that the threads MET (`pumps`, `noteOns`) — the first version of it set its stop flag
// before the sender was ever scheduled and passed five of six checks having run nothing at all.
//
//   ctest --test-dir tools/build -R b1-midi-out --output-on-failure -C Release
//
// Exit code 0 = all green, 1 = any assertion failed.

#include "../../native/songcore/frame_estimator.h"
#include "../../native/songcore/host.h"
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
//
// ⚠️⚠️ **A PORT MUST BE DECLARED BEFORE THE HOST THAT WRITES TO IT — every block below does, and phase
// C is what made that matter.** `~ExternalConsumer` panics, and a panic EMITS: a port declared after
// the host is destroyed first, so that final panic writes into a dead object. Until phase C the bug
// was invisible here, because by teardown every block had stopped and `panic_locked` had nothing left
// to send. A running CLOCK always has something to send — the 0xFC — so the very first transport check
// died with `free(): double free detected in tcache 2`.
//
// It is not a defect in the app: both shells own the port in `main`, outside `app_run`'s frame, so the
// host is always destroyed first there. It is a rule about writing a test in this file, and it belongs
// beside the object it constrains — the same fix, and the same reasoning, as `MidiJitterRecorder` being
// declared before the host in shell/app.cpp.

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

/**
 * The port the B3 race block drives: no log, an INVARIANT.
 *
 * ⚠️ A byte log is the wrong instrument for a race — the interleaving decides the order, so there is no
 * expected string to compare against. What does not depend on the interleaving is the property the
 * cable cares about: **no note may be left sounding, and none may be re-struck while it is already
 * sounding.** This tracks that from the bytes alone, exactly as a synth would.
 *
 * CC 123 (all notes off) clears the channel, because it is a real all-notes-off on the wire and
 * `panic()` sends it as the backstop for notes our own bookkeeping has lost.
 */
struct CountingMidiOut : songcore::IMidiOut {
    long long noteOns = 0, noteOffs = 0, allNotesOff = 0;
    long long doubleOn = 0;   // a note-on for a note already sounding: a note that can never be ended
    bool      sounding[16][128] = {};

    int         device_count() override { return 1; }
    std::string device_name(int) override { return "COUNTER"; }
    bool        open(int) override { return true; }
    void        close() override {}
    bool        is_open() const override { return true; }

    void send(const uint8_t* data, int len) override {
        if (len < 2) return;
        const int status = data[0] & 0xF0, ch = data[0] & 0x0F, d1 = data[1] & 0x7F;
        if (status == songcore::MIDI_NOTE_ON && len >= 3 && data[2] > 0) {
            ++noteOns;
            if (sounding[ch][d1]) ++doubleOn;
            sounding[ch][d1] = true;
        } else if (status == songcore::MIDI_NOTE_OFF ||
                   (status == songcore::MIDI_NOTE_ON && len >= 3 && data[2] == 0)) {
            ++noteOffs;
            sounding[ch][d1] = false;
        } else if (status == songcore::MIDI_CC && d1 == songcore::MIDI_CC_ALL_NOTES_OFF) {
            ++allNotesOff;
            for (int n = 0; n < 128; ++n) sounding[ch][n] = false;
        }
    }

    int still_sounding() const {
        int n = 0;
        for (int c = 0; c < 16; ++c)
            for (int k = 0; k < 128; ++k)
                if (sounding[c][k]) ++n;
        return n;
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

// ── The engine-side witness for the PREVIEW lane ────────────────────────────────────────────────
//
// ⚠️ `track_mask` cannot do this job here: EngineConsumer sets that bit only for tracks 0-7 (the
// preview lane is 8, deliberately outside an eight-bit mask), and the preview never goes through that
// consumer anyway. `noteQueue` is private. What IS public is `onResumeRequested`, and
// `plan_note_on`'s sampler arm calls `engine.requestResume()` on the line before `scheduleNote` —
// AFTER the empty-slot check, so it is reached only when a voice is genuinely about to be raised.
//
// Stated plainly because it matters: this counts the last call BEFORE the schedule, not the schedule.
// `preview_note` makes one such call of its own before it branches, so the reading is
//   1 = the routing gate held (nothing entered the note path) · 2 = a voice was raised.
// That two-valued signal is what makes both controls fire, which a bare "did anything happen" could
// not: instrument 1 carries a sampleFilePath below precisely so that dropping the gate would show up
// as a 2 instead of quietly staying at 1 for want of a sample.
struct ResumeCounter {
    int n = 0;
    explicit ResumeCounter(AudioEngine& e) { e.onResumeRequested = [this]() { ++n; }; }
    int take() { const int v = n; n = 0; return v; }
};

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
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
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
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
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
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
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
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
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

    // ── 5. B5 — the live preview, on the cable and off the engine ────────────────────────────────
    //
    // ⚠️ **The claim phase B could not make until B5.** An audition of an EXTERNAL instrument reaches
    // the CABLE and raises NO voice; an audition of an internal one does the exact opposite. Neither
    // half is visible to anything above: the preview deliberately does NOT go through the router
    // (host.h says why — the schema carries neither `rootAudition` nor a per-preview table cache, and
    // no golden trace has ever contained a preview), so ptplay and ptvoice never see this path.
    //
    // ⚠️ BOTH instruments are given a `sampleFilePath` here and nowhere else in this tool. Instrument
    // 0 needs one because `plan_note_on` drops a note on an empty slot (voice_derive.h's
    // `sampleFilePath == null` convention) and a control that cannot fire certifies anything —
    // and instrument 1, the EXTERNAL one, needs one for the SAME reason on the other side: an
    // EXTERNAL instrument with no sample would raise no voice even with the gate removed, so the gate
    // check would pass by construction. A SAMPLER flipped to EXTERNAL keeps its sample; that is the
    // case being measured.
    std::printf("\n-- B5: the preview reaches the cable, and only the cable --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        ResumeCounter resumes(*engine);
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        Project p = build_project(/*midiLen=*/0);
        p.instruments[0].sampleFilePath = std::string("preview.wav");
        p.instruments[1].sampleFilePath = std::string("flipped.wav");
        p.instruments[1].volume = 0x80;   // the velocity claim below rides on this
        host.edit_project() = p;
        (void)resumes.take();

        host.preview_note(1, Note{0, 4}, /*durationFrames=*/0);   // the EXTERNAL instrument
        host.midi_out().pump(INT64_MAX / 4);
        dump(out, "EXTERNAL audition");

        // The patch states itself first, exactly as a sequenced note's does — that is the whole point
        // of previewing through the same consumer rather than a shortcut beside it.
        ok(at(out, 0) == "C3 29",    "preview: program change", at(out, 0), "C3 29");
        ok(at(out, 1) == "B3 4A 64", "preview: CC A default",   at(out, 1), "B3 4A 64");
        ok(at(out, 2) == "B3 0A 40", "preview: CC 10 pan",      at(out, 2), "B3 0A 40");
        // ⚠️ **0x40, not 0x5A.** VOL 0x80 = 0.502, and the wire gets full velocity SCALED by VOL —
        // which is byte-for-byte what the sequencer sends for this instrument at V=7F. Letting VOL
        // arrive in the engine path's crossed `velGain` slot would take it through `midi_velocity`'s
        // `velocity == -1` branch, which SQUARE-ROOTS it: 90 (0x5A), a preview audibly louder than
        // the note being previewed.
        ok(at(out, 3) == "93 3C 40", "preview: note-on, velocity = VOL", at(out, 3), "93 3C 40");
        eq_int("preview: nothing else on the wire", static_cast<long long>(out.log.size()), 4);
        eq_int("preview: the routing gate held, no voice raised (1 = gate)", resumes.take(), 1);

        // ── the control, and the lane hand-off, on the same gesture ──────────────────────────────
        // Auditioning an INTERNAL instrument next is the START-after-START case (START is exempt from
        // on_stop_preview, button_mapper.h). It must do two things: reach the engine, and end the note
        // the cable is still holding on the lane — the last thing in the app that resolves to it.
        const size_t afterExt = out.log.size();
        host.preview_note(0, Note{0, 4}, /*durationFrames=*/0);
        host.midi_out().pump(INT64_MAX / 4);
        ok(at(out, afterExt) == "83 3C 00", "internal audition ENDS the ringing external note",
           at(out, afterExt), "83 3C 00");
        eq_int("internal audition: and puts nothing of its own on the wire",
               static_cast<long long>(out.log.size() - afterExt), 1);
        eq_int("internal audition: a voice WAS raised (2 = the note path)", resumes.take(), 2);
        host.stop();
    }

    // ── 5b. The timed audition's note-off lands on its own frame ─────────────────────────────────
    //
    // ⚠️ Same trap as the LEN gate in case 3b: an off is an off is an off, three identical bytes
    // whether it arrived on time, early or at the end of the world. Only the FRAME separates them, so
    // the clock has to be moved by hand.
    std::printf("\n-- B5: the timed audition ends on its own frame --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/0);

        const int64_t base = engine->getCurrentFrame() + 100;   // preview_note's lead-in
        const int64_t dur  = 4000;
        host.preview_note(1, Note{0, 4}, dur);

        host.midi_out().pump(base);
        eq_int("at the note's frame: patch + note-on", static_cast<long long>(out.log.size()), 4);
        host.midi_out().pump(base + dur - 1);
        eq_int("one frame before the end: still nothing", static_cast<long long>(out.log.size()), 4);
        host.midi_out().pump(base + dur);
        eq_int("at the end: the off arrives", static_cast<long long>(out.log.size()), 5);
        ok(at(out, 4) == "83 3C 00", "…and it is the note-off", at(out, 4), "83 3C 00");
        host.stop();
    }

    // ── 5c. …and a LEN gate SHORTER than the audition still wins ─────────────────────────────────
    // `end_note` takes the min of the two, and this is the only place the preview's own duration and
    // an instrument's LEN can disagree. `min` cuts short, never extends.
    std::printf("\n-- B5: a LEN gate shorter than the audition wins --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/6);

        const int64_t base = engine->getCurrentFrame() + 100;
        const int64_t gate = 6 * songcore::frames_per_tic(songcore::frames_per_step(128, 44100));
        eq_int("the gate is shorter than the audition", gate < 4000 ? 1 : 0, 1);
        host.preview_note(1, Note{0, 4}, /*durationFrames=*/4000);

        host.midi_out().pump(base + gate - 1);
        eq_int("one frame before the LEN gate: still ringing", static_cast<long long>(out.log.size()), 4);
        host.midi_out().pump(base + gate);
        eq_int("at the LEN gate: the off arrives early", static_cast<long long>(out.log.size()), 5);
        ok(at(out, 4) == "83 3C 00", "…and it is the note-off", at(out, 4), "83 3C 00");
        host.stop();
    }

    // ── 5d. The ring-out audition: nothing but stop_preview ends it ──────────────────────────────
    //
    // ⚠️ Half of this is a "pass = nothing happened" check, which on its own cannot tell a working
    // gate-to-next from a preview that never sounded. The two halves run on the SAME audition: it must
    // survive the end of time, and then die on one call.
    std::printf("\n-- B5: the ring-out audition, and stop_preview --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/0);

        host.preview_instrument(1);   // START on INSTRUMENT: the instrument's own root, no timed kill
        host.midi_out().pump(INT64_MAX / 4);
        eq_int("audition: patch + note-on", static_cast<long long>(out.log.size()), 4);
        ok(at(out, 3) == "93 3C 7F", "audition: note-on at the instrument's ROOT", at(out, 3), "93 3C 7F");

        host.midi_out().pump(INT64_MAX / 4);
        eq_int("past the end of time: still ringing", static_cast<long long>(out.log.size()), 4);

        host.stop_preview();
        host.midi_out().pump(INT64_MAX / 4);
        eq_int("stop_preview: the off arrives", static_cast<long long>(out.log.size()), 5);
        ok(at(out, 4) == "83 3C 00", "…and it is the note-off", at(out, 4), "83 3C 00");
        host.stop();
    }

    // ── 5e. A transport start does not strand a ringing audition ─────────────────────────────────
    //
    // ⚠️ START is exempt from `on_stop_preview` (button_mapper.h), so an audition IS still ringing
    // when the transport begins. `on_play` resets the lane→instrument map; before B5 that stranded the
    // note — no later event on the preview lane would ever resolve to an external instrument again, so
    // `consume` returned at the gate and the off we owed could not be delivered by anything.
    std::printf("\n-- B5: a transport start ends a ringing audition --\n");
    {
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/0);

        host.preview_instrument(1);
        host.midi_out().pump(INT64_MAX / 4);
        eq_int("audition sounding", static_cast<long long>(out.log.size()), 4);

        const size_t before = out.log.size();
        host.schedule_song_range(0, 0, nullptr);   // emits t_play → on_play
        ok(at(out, before) == "83 3C 00",     "transport start: the audition's note-off",
           at(out, before), "83 3C 00");
        ok(at(out, before + 1) == "B3 7B 00", "transport start: the CC 123 backstop",
           at(out, before + 1), "B3 7B 00");
        eq_int("transport start: exactly those two", static_cast<long long>(out.log.size() - before), 2);
        host.stop();
    }

    // ── 6. B3 — the frame estimator: the staircase back into a line ──────────────────────────────
    //
    // ⚠️ **WHAT THIS BLOCK CAN AND CANNOT PROVE, because the difference decided how B3 was verified.**
    // `FrameEstimator` is fed its wall clock, so a synthetic clock measures the ARITHMETIC exactly —
    // the interpolation, the lead cap, monotonicity and the render-reset rule are all fully testable
    // here. What is NOT here is whether a real sender thread delivers on time: that needs a real audio
    // device, a real scheduler and a real wall clock, and it is measured by the shell's own instrument
    // (`POCKETTRACKER_MIDI_JITTER=1`, shell/midi-sender.h) with the thread turned off as its control.
    // Two halves, and neither can stand in for the other.
    std::printf("\n-- B3: FrameEstimator --\n");
    {
        using songcore::FrameEstimator;
        constexpr int SR = 48000;

        // The first observation is an ANCHOR, not an interpolation: with nothing to measure elapsed time
        // against, the honest answer is the device's own number.
        FrameEstimator e(SR);
        eq_int("first call anchors on the counter", e.estimate(1000, 5000000), 1000);

        // 1 ms later, the counter has not moved (it moves once per block) — but the device has.
        eq_int("interpolates between blocks (+1 ms)", e.estimate(1000, 5001000), 1000 + 48);
        eq_int("…and again (+2 ms)", e.estimate(1000, 5002000), 1000 + 96);

        // Rule 1, the lead cap: 100 ms with no block boundary means the audio device has STALLED, and
        // audio is the clock. Capped at 30 ms of lead, not 100.
        eq_int("lead cap holds a stalled device", e.estimate(1000, 5100000), 1000 + 30 * 48);

        // Rule 2: a clock that jitters backwards must not walk the estimate back with it.
        const int64_t high = e.estimate(1000, 5100000);
        eq_int("monotonic against a backwards clock", e.estimate(1000, 5099000), high);

        // A new anchor arrives: the estimate is the counter again, and the elapsed time starts over
        // rather than being counted twice.
        eq_int("a new block re-anchors", e.estimate(1512, 5101000), 1512);
        eq_int("…and interpolation restarts from it", e.estimate(1512, 5102000), 1512 + 48);
    }
    {
        // Rule 3, and it is the one that is easy to get wrong: `resetFrameCounter()` puts the counter
        // back to 0 at the start of every offline render. Monotonicity applied across that would pin the
        // estimate at its pre-render value FOREVER — permanently past the end of time, so every message
        // queued afterwards would release instantly.
        using songcore::FrameEstimator;
        FrameEstimator e(48000);
        e.estimate(2000000, 9000000);
        eq_int("…and it is still monotonic within the anchor", e.estimate(2000000, 9001000), 2000048);
        eq_int("a counter RESET re-anchors backwards", e.estimate(0, 9002000), 0);
        eq_int("…and interpolates from zero", e.estimate(0, 9003000), 48);
    }
    {
        // ── ⭐ THE MEASUREMENT, and the negative control is inside it ─────────────────────────────
        //
        // Simulate what the sender thread actually sees: a device advancing in 441-frame blocks (10 ms
        // at 44.1 kHz — what SDL negotiates on the dev box), read on a 1 ms tick. Compare BOTH the
        // estimator and the raw counter against the device's true position.
        //
        // This is the whole claim of B3's estimator expressed as two numbers, and the second is the
        // control: the raw counter is the pre-B3 behaviour, and it must be an order of magnitude worse.
        // A test where "the estimator is good" cannot be told from "the counter was already good" would
        // prove nothing at all.
        using songcore::FrameEstimator;
        constexpr int SR = 44100, BLOCK = 441;
        // ⚠️ **1003 µs, NOT 1000, AND THE FIRST TICK IS AT 137 µs — because the round numbers made this
        // check pass BY CONSTRUCTION.** At exactly 1000 µs a tick is exactly 44.1 frames and a 441-frame
        // block boundary falls on every tenth tick, so the estimator observed every step at the very
        // instant it happened and reported a perfect 0-frame error. A real tick is not aligned to the
        // audio device's period and never will be; the phase offset is what makes the "noticed up to one
        // tick late" bias real, which is the whole thing this bound is meant to state.
        constexpr int64_t TICK_US = 1003, PHASE_US = 137;

        FrameEstimator e(SR);
        int64_t worstEst = 0, worstRaw = 0;
        for (int64_t us = PHASE_US; us < 2000000; us += TICK_US) {
            // Truth: where the device is at this instant, to the frame.
            const int64_t truth = us * SR / 1000000;
            // What the counter says: the last COMPLETED block. (The engine writes it at the end of
            // processAudioBlock, so it trails truth by 0..BLOCK-1 frames.)
            const int64_t counter = truth / BLOCK * BLOCK;
            const int64_t est     = e.estimate(counter, us);
            worstEst = std::max<int64_t>(worstEst, std::llabs(truth - est));
            worstRaw = std::max<int64_t>(worstRaw, std::llabs(truth - counter));
        }
        const double estMs = 1000.0 * static_cast<double>(worstEst) / SR;
        const double rawMs = 1000.0 * static_cast<double>(worstRaw) / SR;

        // ⚠️ The NUMBERS beside the verdict. The bound is ~one tick, not zero: the anchor's wall time is
        // the moment the step was NOTICED, so the estimate trails truth by up to one tick interval.
        const int64_t tickFrames = TICK_US * SR / 1000000 + 1;
        ok(worstEst <= tickFrames,
           "estimator error <= 1 tick", std::to_string(worstEst) + " frames (" + std::to_string(estMs) + " ms)",
           "<= " + std::to_string(tickFrames) + " frames");
        // The control: without the estimator, the error is the BLOCK, on every block — bounded below by
        // one block minus one tick, because a tick can only land where a tick lands.
        ok(worstRaw >= BLOCK - tickFrames,
           "CONTROL raw counter error = one block", std::to_string(worstRaw) + " frames (" + std::to_string(rawMs) + " ms)",
           ">= " + std::to_string(BLOCK - tickFrames) + " frames");
        ok(worstRaw > worstEst * 8, "…so the estimator is 8x+ better",
           std::to_string(worstRaw) + " vs " + std::to_string(worstEst), "8x");
    }

    // ── 6b. B3 — two threads on one ExternalConsumer ──────────────────────────────────────────────
    //
    // ⚠️ **THE ONE THING B3 ADDED THAT NO OTHER CHECK IN THIS FILE TOUCHES: `pump` now runs on a
    // DIFFERENT THREAD from `consume`.** Every other block here is single-threaded and would stay green
    // with every lock in midi_out.h deleted.
    //
    // What this can catch, honestly stated: an UNLOCKED public entry point. `pending_` is a
    // std::vector<> being inserted into by one thread and erased from by the other, so a missing lock
    // is a corrupted heap and a crash or a wrong count, not a subtle drift — which is exactly what makes
    // it worth doing without a thread sanitizer. What it cannot catch is a rarer interleaving that
    // happens not to fire in 2000 iterations. Verified as a control: the lock removed from `pump`
    // aborts or miscounts here within a run.
    //
    // The INVARIANT asserted is the one that matters for a MIDI cable: whatever the interleaving,
    // every note-on the device received must have been answered by a note-off. A stuck note is the
    // failure mode; a lost message is not silently tolerable either, so the totals are checked too.
    std::printf("\n-- B3: consume and pump on two threads --\n");
    // ⚠️ FLUSHED, and only this block is. It is the one block here that can CRASH rather than fail an
    // assertion — the lock-removed control corrupts `pending_` and dies with an access violation — and
    // stdout is block-buffered when redirected to a file or a pipe, so without this the last line the
    // reader sees is from some earlier block and the control looks like it fired in the wrong place.
    std::fflush(stdout);
    {
        auto engine = std::make_unique<AudioEngine>();
        CountingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        host.edit_project() = build_project(/*midiLen=*/0);

        // ⚠️ **THE HARNESS HAS TO PROVE THE TWO THREADS MET, and the first version did not.** It fired
        // 200 scheduling passes and set `stop` before the sender had been scheduled even once: 0 pumps,
        // 0 note-ons, and five of the six assertions green — a race test that had run nothing, passing.
        // So both directions are now waited for explicitly: the producer does not start until the
        // sender is alive, and the run does not end until the sender has pumped a real number of times.
        // (The guardrails' rule: a test whose pass is *nothing happening* cannot tell a fix from a
        // misfire. `pumps` and `noteOns` are asserted for that reason, not for completeness.)
        std::atomic<bool>      alive{false}, stop{false};
        std::atomic<long long> pumps{0};
        // The sender thread's job, and nothing else: read a clock, call pump. Walking `now` forward one
        // step per pump releases the queue gradually rather than all at once, which is what puts the two
        // threads in the same data at the same time.
        std::thread sender([&] {
            alive.store(true);
            int64_t now = 0;
            while (!stop.load()) {
                host.midi_out().pump(now);
                now += 2000;
                pumps.fetch_add(1);
                std::this_thread::yield();
            }
        });
        while (!alive.load()) std::this_thread::yield();

        for (int i = 0; i < 200; ++i) {
            host.schedule_song_range(0, 0, nullptr);   // the producer: t_play, notes, t_stop
            std::this_thread::yield();
        }
        while (pumps.load() < 2000) std::this_thread::yield();
        stop.store(true);
        sender.join();

        host.stop();                          // the panic every note-on is owed
        host.midi_out().pump(INT64_MAX / 4);  // and drain whatever the race left queued

        ok(pumps.load() >= 2000, "the sender thread ran", std::to_string(pumps.load()) + " pumps", ">= 2000");
        ok(out.noteOns > 0, "note-ons reached the port", std::to_string(out.noteOns), "> 0");
        ok(out.noteOffs > 0, "note-offs reached the port", std::to_string(out.noteOffs), "> 0");
        // The two that are the point. A note struck while already sounding can never be ended (one
        // ActiveNote per lane), and a note still sounding at the end is a note that sounds forever.
        eq_int("no note re-struck while sounding", out.doubleOn, 0);
        eq_int("nothing left sounding", out.still_sounding(), 0);
        eq_int("nothing left queued", static_cast<long long>(host.midi_out().pending_count()), 0);
    }

    // ── 7. PHASE C — the 24 PPQN clock, the transport, and the song position ─────────────────────
    //
    // ⚠️ **WHAT THIS BLOCK CAN PROVE AND WHAT IT STRUCTURALLY CANNOT, because phase C's claim has two
    // units and only one of them is visible here.** The GRID is arithmetic — which frame each tick is
    // due on, what a tempo change does to it, how a stalled device is handled — and a synthetic clock
    // measures that exactly, to the frame. The other half of the claim is in MILLISECONDS: does a tick
    // actually LEAVE close to its due frame? Nothing here can see that (B3's finding, and phase C
    // inherits it whole) — that is the shell's `POCKETTRACKER_MIDI_JITTER=1` instrument, which since
    // phase C reports the clock stream separately and derives a BPM from wall time.
    //
    // ⚠️ **THE PUMP CADENCE IS DELIBERATELY NOT A MULTIPLE OF THE TICK PERIOD.** B3's estimator check
    // passed with a perfect 0-frame error because its 1000 µs tick divided the 441-frame audio block;
    // a clock harness aligned to the clock it measures is the same trap one layer up. So the grid is
    // walked with a 997-frame pump and a 313-frame phase, and the assertion is that every tick came out
    // ONCE, IN ORDER, AT ITS EXACT GRID FRAME — which a lucky alignment cannot manufacture.
    std::printf("\n-- C: the 24 PPQN grid --\n");
    {
        using songcore::MidiClock;

        // The sink the clock hands bytes to — the same shape `ExternalConsumer` gives it.
        struct ClockLog {
            std::vector<int64_t>     frames;
            std::vector<std::string> bytes;
            void operator()(int64_t f, const uint8_t* b, int len) {
                char buf[16];
                std::string s;
                for (int i = 0; i < len; ++i) {
                    std::snprintf(buf, sizeof buf, "%s%02X", i ? " " : "", b[i]);
                    s += buf;
                }
                frames.push_back(f);
                bytes.push_back(s);
            }
            long long count_of(const char* what) const {
                long long n = 0;
                for (const std::string& s : bytes) if (s == what) ++n;
                return n;
            }
        };

        // ⚠️ THE ANCHOR IS THE SCHEDULER'S OWN GRID, not a second copy of the clock's formula. A quarter
        // note is what `timing.h` says a quarter note is — `frames_per_step(tempo, sr) * 4`, the very
        // arithmetic that decides where a NOTE lands — so "tick 24k is on quarter k" is a statement
        // about the clock agreeing with the sequencer, which is the only thing phase-lock can mean.
        // (Two things derived from the same wrong formula would agree perfectly and prove nothing.)
        constexpr int SR = 44100, TEMPO = 128;
        const int64_t framesPerStep = songcore::frames_per_step(TEMPO, SR);       // 5168
        const int64_t fpq           = framesPerStep * 4;                          // 20672
        const int64_t EPOCH         = 100000;

        // The gate first: a clock that was never enabled emits nothing, whatever it is asked.
        {
            MidiClock c;
            ClockLog log;
            c.start(EPOCH, fpq, 0);
            c.pump(EPOCH + fpq * 4, fpq, log);
            eq_int("CONTROL sync disabled: no bytes at all", static_cast<long long>(log.bytes.size()), 0);
            eq_int("CONTROL sync disabled: not running", c.running() ? 1 : 0, 0);
        }

        MidiClock c;
        c.set_enabled(true);
        c.start(EPOCH, fpq, 0);
        ClockLog log;

        // Not due yet — and this is worth asserting rather than assuming: the transport bytes share
        // tick 0's frame, so an off-by-one here would fire Start a pump early, before the audio.
        c.pump(EPOCH - 1, fpq, log);
        eq_int("nothing before the epoch", static_cast<long long>(log.bytes.size()), 0);

        // Walk four quarters with a pump cadence that is coprime-ish to the tick period.
        constexpr int64_t PUMP_STEP = 997, PUMP_PHASE = 313;
        const int64_t end = EPOCH + fpq * 4;
        for (int64_t now = EPOCH - PUMP_PHASE; now <= end + PUMP_STEP; now += PUMP_STEP)
            c.pump(now, fpq, log);

        ok(log.bytes.size() > 1 && log.bytes[0] == "FA", "position 0 opens with Start",
           log.bytes.empty() ? "<none>" : log.bytes[0], "FA");
        eq_int("exactly one Start", log.count_of("FA"), 1);
        eq_int("no Continue at position 0", log.count_of("FB"), 0);

        // 24 ticks a quarter, four quarters, plus tick 0 itself.
        eq_int("clock ticks over four quarters", log.count_of("F8"), 4 * songcore::MIDI_PPQN + 1);

        // Every tick on its exact grid frame, in order, once. The Start shares frame 0 and is skipped.
        int64_t worstLate = 0;
        bool    ordered = true, exact = true;
        int64_t k = 0;
        for (size_t i = 0; i < log.bytes.size(); ++i) {
            if (log.bytes[i] != "F8") continue;
            const int64_t want = EPOCH + k * fpq / songcore::MIDI_PPQN;
            if (log.frames[i] != want) exact = false;
            if (i > 0 && log.frames[i] < log.frames[i - 1]) ordered = false;
            ++k;
        }
        ok(exact, "every tick on its exact grid frame", exact ? "all " + std::to_string(k) : "MISMATCH",
           "epoch + k*fpq/24");
        ok(ordered, "…and monotonically forward", ordered ? "yes" : "no", "yes");

        // ⭐ The phase-lock statement, against the SEQUENCER's grid rather than against the clock's own:
        // tick 6k is step k, tick 24k is quarter k. This is the check that would go red if the clock
        // followed a rounder, more accurate quarter than the one the notes are placed on.
        {
            MidiClock g;
            g.set_enabled(true);
            g.start(EPOCH, fpq, 0);
            ClockLog grid;
            for (int64_t t = 0; t < 4 * songcore::MIDI_PPQN + 1; ++t) {
                const int64_t at = g.next_tick_frame();
                g.pump(at, fpq, grid);
            }
            bool onSteps = true;
            for (int step = 0; step <= 16; ++step) {
                const size_t tick = static_cast<size_t>(step) * 6 + 1;   // +1: index 0 is the Start byte
                if (tick >= grid.frames.size()) { onSteps = false; break; }
                if (grid.frames[tick] != EPOCH + framesPerStep * step) onSteps = false;
            }
            ok(onSteps, "tick 6k lands on the scheduler's step k",
               onSteps ? "16 steps exact" : "DRIFTED", "epoch + framesPerStep*k");
        }

        // ⭐⭐ THE DRIFT CONTROL, and it is the reason the tick frame is a ratio and not an accumulator.
        // A `next += fpq/24` implementation looks identical for the first few bars. Over 150 quarters
        // (~70 s at this tempo) the truncation costs a sixth of a frame per tick — the number is printed
        // beside the verdict so it can never be mistaken for a rounding argument.
        //
        // ⚠️ Driven ONE TICK AT A TIME on purpose. A single pump past the end would exceed the burst cap
        // and skip the backlog, and the check would then be measuring the cap rather than the grid —
        // which is exactly how the first draft of it failed, reading 3121729 for a want of 3100200.
        {
            MidiClock d;
            d.set_enabled(true);
            d.start(0, fpq, 0);
            ClockLog sink;
            const int64_t TICKS = 150 * songcore::MIDI_PPQN;
            for (int64_t i = 0; i < TICKS; ++i) d.pump(d.next_tick_frame(), fpq, sink);
            const int64_t got   = d.next_tick_frame();
            const int64_t truth = TICKS * fpq / songcore::MIDI_PPQN;
            eq_int("150 quarters: exact, no accumulated drift", got, truth);
            eq_int("…and every one of them was sent", sink.count_of("F8"), TICKS);
            eq_int("…none skipped by the burst cap", d.dropped_ticks(), 0);
            // The accumulating form, computed here so the size of the error is visible, not argued.
            int64_t naive = 0;
            for (int64_t i = 0; i < TICKS; ++i) naive += fpq / songcore::MIDI_PPQN;
            const double driftMs = 1000.0 * static_cast<double>(truth - naive) / SR;
            ok(truth - naive > 400, "CONTROL an accumulator would have drifted",
               std::to_string(truth - naive) + " frames (" + std::to_string(driftMs) + " ms in 70 s)",
               "> 400 frames");
        }

        // ── A tempo change mid-take ──────────────────────────────────────────────────────────────
        {
            const int64_t fpqSlow = songcore::frames_per_step(64, SR) * 4;   // half speed
            MidiClock t;
            t.set_enabled(true);
            t.start(EPOCH, fpq, 0);
            ClockLog sink;
            // Ten ticks at the fast tempo…
            t.pump(EPOCH + 9 * fpq / songcore::MIDI_PPQN, fpq, sink);
            eq_int("ten ticks before the change", sink.count_of("F8"), 10);
            const int64_t predicted = EPOCH + 10 * fpq / songcore::MIDI_PPQN;   // where tick 10 was due
            // …then the user turns TEMPO. The tick about to fire keeps its slot — no jump, nothing lost.
            t.pump(predicted, fpqSlow, sink);
            const size_t first = sink.frames.size() - 1;
            eq_int("the tick at the change keeps its slot", sink.frames[first], predicted);
            // …and the one after it is spaced by the NEW period.
            t.pump(predicted + fpqSlow / songcore::MIDI_PPQN, fpqSlow, sink);
            eq_int("the next tick uses the new period",
                   sink.frames.back() - predicted, fpqSlow / songcore::MIDI_PPQN);
            ok(fpqSlow / songcore::MIDI_PPQN > fpq / songcore::MIDI_PPQN + 100,
               "CONTROL the two periods are far apart",
               std::to_string(fpq / songcore::MIDI_PPQN) + " -> " +
                   std::to_string(fpqSlow / songcore::MIDI_PPQN),
               "differ");
        }

        // ── The burst cap: an audio device that stalled and resumed ──────────────────────────────
        {
            MidiClock b;
            b.set_enabled(true);
            b.start(0, fpq, 0);
            ClockLog sink;
            b.pump(0, fpq, sink);                       // arm: Start + tick 0
            const long long before = sink.count_of("F8");
            // Ten ticks' worth is normal jitter and is NOT a burst — the control that the cap does not
            // fire on ordinary lateness.
            b.pump(10 * fpq / songcore::MIDI_PPQN, fpq, sink);
            eq_int("CONTROL a 10-tick backlog is sent in full", sink.count_of("F8") - before, 10);
            eq_int("CONTROL …and drops nothing", b.dropped_ticks(), 0);

            const long long mid = sink.count_of("F8");
            b.pump(fpq * 30, fpq, sink);                // ~30 quarters of stall, resumed at once
            eq_int("a stall past the cap sends ONE tick", sink.count_of("F8") - mid, 1);
            ok(b.dropped_ticks() > 600, "…and COUNTS what it skipped",
               std::to_string(b.dropped_ticks()) + " ticks", "> 600");
            // The grid is still the song's: the index counted every tick that went by.
            eq_int("…and the grid is still locked to the song",
                   b.next_tick_frame(), b.tick_index() * fpq / songcore::MIDI_PPQN);
        }
    }

    // ── 7b. PHASE C — the transport bytes on a real host, and where the playhead lands ────────────
    //
    // The grid above is arithmetic; this is the wiring. It drives `SongcoreHost` exactly as the app
    // does — play_song / play_phrase / stop — and reads the bytes off a recording port, so what it
    // asserts is that the transport verb a user presses produces the transport message the plan says.
    std::printf("\n-- C: transport + song position --\n");
    {
        // The fixture's whole job is to make the SPP arithmetic VISIBLE: song row 0 is two chain rows
        // long, row 1 is three, row 2 is ten. So a start at row 1 is 2*16 = 32 steps in, at row 2 it is
        // (2+3)*16 = 80, and at row 3 it is (2+3+10)*16 = 240 — which is past 127 and therefore the one
        // case that can tell an LSB/MSB swap from a correct 14-bit split.
        auto make_song = [] {
            Project p = songcore::make_default_project();
            p.tempo = 128;
            Instrument& ext = p.instruments[1];
            ext.instrumentType = InstrumentType::EXTERNAL;
            ext.midiChannel = 3;
            p.phrases[1].steps[0].note = Note{0, 4};
            p.phrases[1].steps[0].instrument = 1;
            const int rows[3] = {2, 3, 10};
            for (int songRow = 0; songRow < 3; ++songRow) {
                const int chainId = songRow;
                for (int r = 0; r < rows[songRow]; ++r) p.chains[chainId].phraseRefs[r] = 1;
                p.tracks[1].chainRefs.push_back(chainId);
            }
            return p;
        };

        struct Case { int row; const char* first; const char* second; const char* what; };
        const Case cases[] = {
            {0, "FA", nullptr, "row 0 -> Start"},
            {1, "F2 20 00", "FB", "row 1 -> SPP 32 + Continue"},
            {2, "F2 50 00", "FB", "row 2 -> SPP 80 + Continue"},
            {3, "F2 70 01", "FB", "row 3 -> SPP 240 + Continue (14-bit)"},
        };

        for (const Case& k : cases) {
            auto engine = std::make_unique<AudioEngine>();
            RecordingMidiOut out;
            SongcoreHost host(engine.get(), 44100);
            host.set_midi_out(&out);
            host.edit_project() = make_song();
            host.set_midi_sync_out(true);

            host.play_song(k.row);
            host.midi_out().pump(0);   // the transport bytes ride tick 0, which is frame 0 here

            ok(at(out, 0) == k.first, k.what, at(out, 0), k.first);
            if (k.second) ok(at(out, 1) == k.second, std::string(k.what) + " (2nd)", at(out, 1), k.second);
            // The clock starts on the same frame, immediately after the transport it belongs to.
            const size_t tick = k.second ? 2 : 1;
            ok(at(out, tick) == "F8", std::string(k.what) + " -> first tick", at(out, tick), "F8");
        }

        // PHRASE and CHAIN are auditions of one object, not positions in a song: Start, never SPP.
        for (int mode = 0; mode < 2; ++mode) {
            auto engine = std::make_unique<AudioEngine>();
            RecordingMidiOut out;
            SongcoreHost host(engine.get(), 44100);
            host.set_midi_out(&out);
            host.edit_project() = make_song();
            host.set_midi_sync_out(true);

            if (mode == 0) host.play_phrase(1); else host.play_chain(1);
            host.midi_out().pump(0);
            ok(at(out, 0) == "FA", mode == 0 ? "play_phrase -> Start" : "play_chain -> Start",
               at(out, 0), "FA");
        }

        // ── Stop, and the silence after it ───────────────────────────────────────────────────────
        {
            auto engine = std::make_unique<AudioEngine>();
            RecordingMidiOut out;
            SongcoreHost host(engine.get(), 44100);
            host.set_midi_out(&out);
            host.edit_project() = make_song();
            host.set_midi_sync_out(true);

            host.play_song(0);
            host.midi_out().pump(0);
            const size_t before = out.log.size();
            host.stop();
            ok(at(out, before) == "FC", "stop sends the transport Stop FIRST", at(out, before), "FC");

            const size_t after = out.log.size();
            host.midi_out().pump(44100 * 10);   // ten seconds later
            eq_int("no tick survives the stop", static_cast<long long>(out.log.size() - after), 0);
        }

        // ⭐⭐ THE CONTROL FOR THE WHOLE FEATURE, and it is the one that must fire: with SYNC OFF —
        // which is the DEFAULT, and therefore the state every one of the 86 checks above ran in — not
        // one byte of clock or transport may appear. If this ever went green with the gate removed,
        // every byte-stream assertion in this file would have been silently reading a different stream.
        {
            auto engine = std::make_unique<AudioEngine>();
            RecordingMidiOut out;
            SongcoreHost host(engine.get(), 44100);
            host.set_midi_out(&out);
            host.edit_project() = make_song();

            eq_int("CONTROL sync out defaults to OFF", host.midi_sync_out() ? 1 : 0, 0);
            host.play_song(1);
            host.midi_out().pump(44100 * 4);
            host.stop();
            long long realtime = 0;
            for (const std::string& s : out.log)
                if (!s.empty() && (s[0] == 'F')) ++realtime;   // F2/F8/FA/FB/FC — every byte phase C adds
            eq_int("CONTROL sync off: no clock, no transport, no SPP", realtime, 0);
        }

        // Turning sync OFF mid-take owes the device a Stop — a synth waiting on a clock that simply
        // stopped arriving looks exactly like the app having crashed.
        {
            auto engine = std::make_unique<AudioEngine>();
            RecordingMidiOut out;
            SongcoreHost host(engine.get(), 44100);
            host.set_midi_out(&out);
            host.edit_project() = make_song();
            host.set_midi_sync_out(true);
            host.play_song(0);
            host.midi_out().pump(0);

            const size_t before = out.log.size();
            host.set_midi_sync_out(false);
            ok(at(out, before) == "FC", "sync turned off mid-take sends Stop", at(out, before), "FC");
            eq_int("…and exactly that", static_cast<long long>(out.log.size() - before), 1);
            // CONTROL: doing it again owes nothing — the state, not the call site, decides.
            const size_t after = out.log.size();
            host.set_midi_sync_out(false);
            eq_int("CONTROL a second OFF sends nothing", static_cast<long long>(out.log.size() - after), 0);
        }
    }

    // ── 8. PHASE D — MPG / MPB / CCA-CCD, the FX commands that speak the CC map ───────────────────
    //
    // ⚠️ **THE CLAIM UNDER TEST IS "ROUTER EVENTS, NOT EXTERNAL-ONLY EFFECTS" (plan §8.3)**, and it has
    // two halves that no single instrument can see at once: the bytes a command puts on the wire, and
    // the fact that the SAME command on a SAMPLER instrument reaches the bus at all rather than being
    // filtered out somewhere on the way. The port sees the first; a consumer attached to the router
    // sees the second, which is what the last block below does.
    std::printf("\n-- D: MPG / MPB / CCA-CCD --\n");

    // 8a. The slot translation, alone. It is the only genuinely NEW logic on the engine's side of the
    // fence (everything after it is the four CC ids that VOL/PAN/REV/DEL have driven since B1), and it
    // is a pure function of the instrument — so it is checked as one, in the one place both consumers
    // read it from.
    {
        Instrument ins;
        ins.midiCC[0] = MidiCcSlot{74, 100};   // A → cutoff
        ins.midiCC[1] = MidiCcSlot{10, 64};    // B → pan, which the ENGINE also honours
        // C and D stay at the field default {-1,-1}: unassigned.
        eq_int("resolve: a literal id passes through", songcore::resolve_cc_param(ins, 91), 91);
        eq_int("resolve: CCA → the slot's number",     songcore::resolve_cc_param(ins, songcore::CC_SLOT_A), 74);
        eq_int("resolve: CCB → the slot's number",     songcore::resolve_cc_param(ins, songcore::CC_SLOT_B), 10);
        // ⚠️ THE CONTROL THAT MATTERS. An unassigned slot must answer −1 and NOT fall through to the
        // raw id: 128 masked to seven bits is CC 0, bank select. "Does nothing" and "silently re-banks
        // the device" are the two possible readings of this one line.
        eq_int("resolve: an unassigned CCC → nothing",  songcore::resolve_cc_param(ins, songcore::CC_SLOT_C), -1);
        eq_int("resolve: an unassigned CCD → nothing",  songcore::resolve_cc_param(ins, songcore::CC_SLOT_D), -1);
        ok(songcore::resolve_cc_param(ins, songcore::CC_SLOT_C) != songcore::MIDI_CC_BANK_MSB,
           "…and specifically NOT bank select", "-1", "not 0");
    }

    // 8a2. The RESOLVER's new arms. ⚠️ They belong here rather than in `ptresolve` even though that is
    // the resolver's own tool: its fixture is a golden RECORDED from the Kotlin sequencer, which
    // predates these codes and can never contain one. Hand-written cases added to a recorded file
    // would cost it exactly the provenance that makes it worth having — so ptresolve prints the new
    // fields (its line shape is fixed) and this hand-written tool asserts what they hold.
    {
        auto resolved = [](int t1, int v1, int t2, int v2) {
            songcore::PhraseStep s;
            s.fx1Type = t1; s.fx1Value = v1;
            s.fx2Type = t2; s.fx2Value = v2;
            return songcore::resolve_step_params(s, 0, 1.0f);
        };
        const auto mpg = resolved(songcore::FX_MPG, 0x41, 0, 0);
        eq_int("resolve MPG", mpg.midiProgram.value_or(-1), 0x41);
        // 0xFF is not typeable (effect_value_max caps MPG at 0x7F) but the resolver still guards it:
        // a program is seven bits and 0x80+ would fold onto a different patch inside the serializer.
        eq_int("resolve MPG masks to 7 bits", resolved(songcore::FX_MPG, 0xFF, 0, 0).midiProgram.value_or(-1), 0x7F);
        eq_int("MPG's value column stops at 7F", songcore::effect_value_max(songcore::FX_MPG), 127);
        eq_int("MPB's does not", songcore::effect_value_max(songcore::FX_MPB), 255);
        eq_int("resolve MPB keeps the raw byte", resolved(songcore::FX_MPB, 0xC0, 0, 0).midiBend.value_or(-1), 0xC0);

        // The four slots are INDEPENDENT fields, so two different letters on one step both survive —
        // where two of the SAME letter are last-wins like every other effect.
        const auto two = resolved(songcore::FX_CCA, 0x11, songcore::FX_CCC, 0x22);
        eq_int("CCA and CCC on one step: A", two.ccSlotValue[0].value_or(-1), 0x11);
        eq_int("CCA and CCC on one step: C", two.ccSlotValue[2].value_or(-1), 0x22);
        ok(!two.ccSlotValue[1].has_value(), "…and B stays unset", "unset", "unset");
        eq_int("two CCAs: last wins",
               resolved(songcore::FX_CCA, 0x11, songcore::FX_CCA, 0x22).ccSlotValue[0].value_or(-1), 0x22);
        // And the letter→field mapping is not off by one at either end.
        eq_int("CCD is slot 3", resolved(songcore::FX_CCD, 0x33, 0, 0).ccSlotValue[3].value_or(-1), 0x33);
    }

    // 8b. The byte stream. One phrase carrying all three commands, on the EXTERNAL track.
    {
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        Project p = build_project(/*midiLen=*/0);
        // step 0 already carries the C-4; the command rides with it.
        p.phrases[1].steps[0].fx1Type = songcore::FX_CCA; p.phrases[1].steps[0].fx1Value = 0x40;
        p.phrases[1].steps[2].fx1Type = songcore::FX_MPG; p.phrases[1].steps[2].fx1Value = 0x07;
        p.phrases[1].steps[3].fx1Type = songcore::FX_MPB; p.phrases[1].steps[3].fx1Value = 0xC0;
        host.edit_project() = p;

        run_song(host);
        dump(out, "byte stream");

        ok(at(out, 3) == "93 3C 7F", "note-on, as before", at(out, 3), "93 3C 7F");
        // ⚠️ **AFTER the note-on, and that ordering is the feature.** A note-on carries the
        // instrument's CC-slot DEFAULTS with it (index 1 is `B3 4A 64`); a step command is the
        // specific thing and the default the general one, so emitting the command first would have it
        // overwritten by the note's own default every single time — silently, and with a byte stream
        // that still looks busy. 0x40 → 64/255 → to7bit → 32 = 0x20.
        ok(at(out, 4) == "B3 4A 20", "CCA moves slot A's controller", at(out, 4), "B3 4A 20");
        ok(at(out, 5) == "C3 07",    "MPG on an empty step",          at(out, 5), "C3 07");
        // 0xC0 << 6 = 0x3000: LSB 0x00, MSB 0x60. A swapped pair would read "E3 60 00".
        ok(at(out, 6) == "E3 00 60", "MPB, 14-bit LSB then MSB",      at(out, 6), "E3 00 60");

        ok(at(out, 7) == "83 3C 00", "step 4: gate-to-next off",      at(out, 7), "83 3C 00");
        // ⚠️ **THE MPG's REAL TEST IS HERE, NOT AT INDEX 5.** The instrument's own program is sent only
        // when the channel is believed to have drifted off it, and the MPG is what made it drift. Drop
        // the `lastProgram_` write in `program_event` and this line disappears — the de-dup would keep
        // believing the channel is on program 41 while it is on 7, and every later note of the song
        // would play the wrong patch with nothing in the stream to say so.
        ok(at(out, 8) == "C3 29",    "…and the instrument re-asserts its program", at(out, 8), "C3 29");
        ok(at(out, 9) == "B3 4A 64", "…then its CC-A default",        at(out, 9), "B3 4A 64");
        ok(at(out, 10) == "93 40 40", "…then the note",               at(out, 10), "93 40 40");
        eq_int("messages before the stop", static_cast<long long>(out.log.size()), 11);

        // The stop owes a note-off, the CC 123 backstop — and now a BEND back to centre.
        const size_t before = out.log.size();
        host.stop();
        ok(at(out, before) == "83 40 00",     "stop: note-off",  at(out, before), "83 40 00");
        ok(at(out, before + 1) == "B3 7B 00", "stop: CC 123",    at(out, before + 1), "B3 7B 00");
        ok(at(out, before + 2) == "E3 00 40", "stop: bend back to centre", at(out, before + 2), "E3 00 40");
        eq_int("stop: and nothing else", static_cast<long long>(out.log.size() - before), 3);
    }

    // 8c. CONTROLS — each one changes exactly one thing about 8b and must change the stream.
    {
        // (i) An UNASSIGNED slot sends nothing at all.
        //
        // ⚠️ **IT MUST BE SLOT *A*, AND THE FIRST DRAFT OF THIS USED SLOT B AND PROVED LESS THAN IT
        // CLAIMED.** The failure being guarded against is a fall-through to the raw id, and the ids are
        // 128,129,130,131 — masked to seven bits those are CC 0,1,2,3. Only `CC_SLOT_A` masks to bank
        // select; with slot B the fall-through emits a harmless-looking modulation-wheel message, the
        // count check fires and the bank-select check stays green while the bug it names is present.
        // The control run said so: a check that fires is not necessarily one that fires where it must.
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        Project p = build_project(/*midiLen=*/0);
        p.instruments[1].midiCC[0] = MidiCcSlot{};            // slot A: back to unassigned
        p.phrases[1].steps[0].fx1Type = songcore::FX_CCA; p.phrases[1].steps[0].fx1Value = 0x40;
        host.edit_project() = p;
        run_song(host);
        dump(out, "unassigned slot A");

        long long bankSelects = 0;
        for (const std::string& s : out.log)
            if (s.size() >= 5 && s[0] == 'B' && s.substr(3, 2) == "00") ++bankSelects;
        eq_int("CONTROL unassigned slot: no bank select", bankSelects, 0);
        // The stream is block 2's minus the two CC-A defaults the slot no longer has, and the `CCA`
        // itself adds nothing: program, pan, note-on / off, note-on.
        eq_int("CONTROL unassigned slot: nothing added", static_cast<long long>(out.log.size()), 5);
        host.stop();
    }
    {
        // (ii) A bend that is ALREADY centre leaves nothing to undo, so the stop must NOT send one.
        // Without this, "the panic centres the bend" would pass on a version that centres all sixteen
        // channels unconditionally — i.e. one that stamps on state belonging to other gear.
        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        Project p = build_project(/*midiLen=*/0);
        p.phrases[1].steps[2].fx1Type = songcore::FX_MPB; p.phrases[1].steps[2].fx1Value = 0x80;
        host.edit_project() = p;
        run_song(host);
        ok(at(out, 4) == "E3 00 40", "a centred MPB is still sent", at(out, 4), "E3 00 40");

        const size_t before = out.log.size();
        host.stop();
        long long bends = 0;
        for (size_t i = before; i < out.log.size(); ++i)
            if (!out.log[i].empty() && out.log[i][0] == 'E') ++bends;
        eq_int("CONTROL centred bend: the stop adds no bend", bends, 0);
    }
    {
        // (iii) THE OTHER HALF OF THE CLAIM: the same three commands on a SAMPLER instrument reach the
        // BUS and not the cable. The port cannot show this — silence on a cable is also what a command
        // that was never emitted looks like — so the witness is a consumer attached to the router,
        // seeing the identical records the engine consumer sees.
        struct EventRecorder : songcore::IMidiConsumer {
            std::vector<std::string> log;
            void consume(const songcore::Event& ev) override {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%02X/%d", ev.type, static_cast<int>(ev.track));
                std::string s = buf;
                if (ev.type == songcore::EV_CC)      s += " param=" + std::to_string(ev.cc.param);
                if (ev.type == songcore::EV_PROGRAM) s += " prog=" + std::to_string(ev.program.program);
                if (ev.type == songcore::EV_PITCH_BEND) s += " v14=" + std::to_string(ev.pitchBend.value14);
                log.push_back(s);
            }
            void on_play(const std::string&, const std::string&, int64_t, int, int) override {}
            void on_stop() override {}
            int count(const std::string& needle) const {
                int n = 0;
                for (const std::string& s : log) if (s.find(needle) != std::string::npos) ++n;
                return n;
            }
        };

        auto engine = std::make_unique<AudioEngine>();
        RecordingMidiOut out;
        EventRecorder rec;
        SongcoreHost host(engine.get(), 44100);
        host.set_midi_out(&out);
        host.router().add_consumer(&rec);
        Project p = build_project(/*midiLen=*/0);
        p.instruments[0].midiCC[0] = MidiCcSlot{10, -1};      // slot A of the SAMPLER → pan
        p.phrases[0].steps[0].fx1Type = songcore::FX_CCA; p.phrases[0].steps[0].fx1Value = 0x40;
        p.phrases[0].steps[1].fx1Type = songcore::FX_MPG; p.phrases[0].steps[1].fx1Value = 0x07;
        p.phrases[0].steps[2].fx1Type = songcore::FX_MPB; p.phrases[0].steps[2].fx1Value = 0xC0;
        p.tracks[1].chainRefs.clear();                        // the EXTERNAL track sits this one out
        host.edit_project() = p;
        run_song(host);

        eq_int("sampler track: CCA reached the bus",
               rec.count("B0/0 param=" + std::to_string(songcore::CC_SLOT_A)), 1);
        eq_int("sampler track: MPG reached the bus", rec.count("C0/0 prog=7"), 1);
        eq_int("sampler track: MPB reached the bus", rec.count("E0/0 v14=12288"), 1);
        // ⚠️ …and the CC id on the bus is the SYMBOLIC one, not 10. That is the schema decision phase D
        // rests on: the letter travels and the consumer resolves it, because the step that carries a
        // `CCA` does not know which instrument will be sounding when it lands.
        eq_int("…carrying the LETTER, not the number", rec.count("param=10"), 0);
        // The routing gate, the other way: none of it reached the cable.
        eq_int("sampler track: nothing on the wire", static_cast<long long>(out.log.size()), 0);
        host.stop();
    }

    std::printf("\n%s\n", failures == 0 ? "ALL GREEN" : ("FAILURES: " + std::to_string(failures)).c_str());
    return failures == 0 ? 0 : 1;
}
