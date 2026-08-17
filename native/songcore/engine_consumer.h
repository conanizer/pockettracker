#ifndef POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H
#define POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H

// ─── The engine consumer — bus events become audio ───────────────────────────────────────────────
//
// The C++ port of Kotlin's AudioEngine.scheduleNote() and its param-event siblings: everything BELOW
// the event-schema seam. THIS is the class that makes the app portable — on Android it replaces the
// Kotlin consumer, and the Linux/SDL shell inherits it unchanged. After it, there is no second
// implementation of "what a note means", which is the whole point of the songcore migration.
//
// It is deliberately thin. All the *derivation* — frequency, base frequency, the slice window, the SF
// slot/velocity/root transpose, tick→frame conversions, the modulation pushes — lives in voice_derive.h
// as pure functions, because the conformance trace stops at the router ABOVE this file and cannot
// prove any of it. Pure functions can be goldened against the real Kotlin code (S5ConsumerGoldenTest →
// tools/ptvoice); engine calls cannot. What is left here is the plumbing: look up, derive, call.
//
// The end-to-end check on top of that golden: an offline render is deterministic, so the same project
// rendered with ENGINE=KT and ENGINE=C++ must produce BYTE-IDENTICAL WAVs.

#include <cstdint>

#include "../audio-engine.h"
#include "../table_automation.h"  // …and the same, for the table-ramp cross-check below
#include "automation.h"           // …likewise: the registry one half of that check reads
#include "effects.h"    // …only for the cross-check below; the consumer itself resolves no effects
#include "event.h"
#include "model.h"
#include "router.h"
#include "voice_derive.h"

namespace songcore {

// ⚠️ THE TABLE ENGINE KEEPS ITS OWN COPY OF THE EFFECT CODES IT PROCESSES (`native/audio-defs.h`),
// because it sits below this seam and must not depend on the model. A copy held together by a comment
// is a copy that drifts, and the drift is SILENT in the worst way: a table cell would go on being
// typeable and drawn by its right name while the row it lands on did something else, or nothing.
// This is the one place both lists are visible, so it is where they are made to agree.
static_assert(::FX_HOP    == FX_HOP,    "audio-defs.h FX_HOP has drifted from effects.h");
static_assert(::FX_TIC    == FX_TIC,    "audio-defs.h FX_TIC has drifted from effects.h");
static_assert(::FX_KILL   == FX_KILL,   "audio-defs.h FX_KILL has drifted from effects.h");
static_assert(::FX_OFFSET == FX_OFFSET, "audio-defs.h FX_OFFSET has drifted from effects.h");
static_assert(::FX_THO    == FX_THO,    "audio-defs.h FX_THO has drifted from effects.h");
static_assert(::FX_VOLUME == FX_VOLUME, "audio-defs.h FX_VOLUME has drifted from effects.h");
static_assert(::FX_EQN    == FX_EQN,    "audio-defs.h FX_EQN has drifted from effects.h");
static_assert(::FX_EQM    == FX_EQM,    "audio-defs.h FX_EQM has drifted from effects.h");
static_assert(::FX_CUT    == FX_CUT,    "audio-defs.h FX_CUT has drifted from effects.h");
static_assert(::FX_RES    == FX_RES,    "audio-defs.h FX_RES has drifted from effects.h");

// ─── …and the third list: what a TABLE row's AUS may ramp ────────────────────────────────────────
//
// `native/table_automation.h` runs the pairing for BOTH the engine and the table editor, so it can
// name neither side's constants and spells its codes as literals. This is the one file that sees all
// three lists, so it is where they are made to agree — and the agreement is what makes the ramp-able
// set an INTERSECTION that is derived rather than a hand-written list of five.
static_assert(table_automation::FX_AUS_CODE == FX_AUS,
              "table_automation.h AUS has drifted from effects.h");
static_assert(table_automation::FX_AUF_CODE == FX_AUF,
              "table_automation.h AUF has drifted from effects.h");
static_assert(table_automation::EQ_PRESET_SLOTS == POOL_EQPRESETS,
              "table_automation.h's preset-slot ceiling has drifted from the pool size");

// Both directions, so neither list can grow an entry the other has not: every arm declared there is
// a code the table engine processes, and every code the table engine processes is declared there.
constexpr bool table_arms_match_the_engine() {
    for (int i = 0; i < table_automation::ARM_COUNT; ++i) {
        const int c = table_automation::ARMS[i].code;
        if (c != ::FX_HOP && c != ::FX_TIC && c != ::FX_KILL && c != ::FX_OFFSET &&
            c != ::FX_THO && c != ::FX_VOLUME && c != ::FX_EQN && c != ::FX_EQM &&
            c != ::FX_CUT && c != ::FX_RES) return false;
    }
    return table_automation::arm_for(::FX_HOP)    && table_automation::arm_for(::FX_TIC)  &&
           table_automation::arm_for(::FX_KILL)   && table_automation::arm_for(::FX_OFFSET) &&
           table_automation::arm_for(::FX_THO)    && table_automation::arm_for(::FX_VOLUME) &&
           table_automation::arm_for(::FX_EQN)    && table_automation::arm_for(::FX_EQM)  &&
           table_automation::arm_for(::FX_CUT)    && table_automation::arm_for(::FX_RES);
}
static_assert(table_arms_match_the_engine(),
              "table_automation.h's arm list and audio-defs.h's effect codes disagree — one of them "
              "has an effect the other does not");

// ⭐ THE FLAGS ARE NOT A JUDGEMENT CALL, and this is what says so. `rampable` is exactly "the
// registry admits it" and `eqPreset` is exactly "…and its endpoints are preset slots" — so giving
// `OFF` a registry row, or the table a `PAN` arm, changes which effects a table AUS can ramp with no
// third list to remember. Getting either flag wrong by hand fails here rather than at the ear.
constexpr bool table_arm_flags_match_the_registry() {
    for (int i = 0; i < table_automation::ARM_COUNT; ++i) {
        const AutomatableParam* p = automatable_param(table_automation::ARMS[i].code);
        if (table_automation::ARMS[i].rampable != (p != nullptr)) return false;
        if (table_automation::ARMS[i].eqPreset != (p != nullptr && p->kind == RampKind::EQ_PRESET))
            return false;
    }
    return true;
}
static_assert(table_arm_flags_match_the_registry(),
              "a table arm's rampable/eqPreset flag disagrees with AUTOMATABLE_PARAMS");

// Routing + plan_note_on (the note path itself) live in voice_derive.h — engine-agnostic, so
// tools/ptvoice can instantiate the same code against a recorder and golden it.

class EngineConsumer : public IMidiConsumer {
  public:
    EngineConsumer(AudioEngine* engine, const Project* project, const Routing* routing)
        : engine_(engine), project_(project), routing_(routing) {}

    // Transport is not an engine event — the host owns queue clearing and the master-EQ restore. The
    // one thing that IS ours: the track→instrument map is per-session, and a stale one would route the
    // first track-scoped event of a new take at the last take's instrument.
    void on_play(const std::string&, const std::string&, int64_t, int, int) override { tracks_.reset(); }
    void on_stop() override { tracks_.reset(); }

    // A push may have changed the tables, so the "already sent to the engine" cache must not survive
    // it. Kotlin's `loadedTables` is the same lazy cache, invalidated by its own edit hooks.
    void invalidate_tables() {
        for (int i = 0; i < POOL_TABLES; ++i) tableLoaded_[i] = false;
    }

    // Which of tracks 0-7 have had a note scheduled this session (AudioEngine.phraseTrackMask): the
    // OCTA visualizer lights one scope lane per bit. The host clears it on stop, where Kotlin clears
    // it in clearScheduledNotes()/stopAll().
    int  track_mask() const { return trackMask_; }
    void clear_track_mask() { trackMask_ = 0; }

    void consume(const Event& ev) override {
        if (!engine_ || !project_) return;

        // ── The routing gate (MIDI plan §1) ──────────────────────────────────────────────────────
        //
        // An instrument names its consumer. Everything routed EXTERNAL belongs to ExternalConsumer
        // (midi_out.h) and must not raise a voice here — INCLUDING when no cable is attached, because
        // EXTERNAL means "not this engine", not "this engine unless something better exists".
        //
        // ⚠️ The verdict comes from `instrument_routes_external` (model.h) and `TrackInstruments`
        // (router.h), which the other consumer also uses: an event both consumers claim is played
        // twice, and one neither claims is silence.
        const int16_t prev  = tracks_.current(ev.track);
        const int16_t instr = tracks_.observe(ev);
        if (is_external(instr)) {
            // ⚠️ A track that FLIPS from an internal instrument to an external one leaves a voice
            // sounding with nothing left to stop it: no later event on that track resolves to this
            // consumer, so the note-off that would have cut it is routed away. The note-on that
            // caused the flip is where that voice ends.
            if (ev.type == EV_NOTE_ON && prev >= 0 && !is_external(prev))
                engine_->scheduleKill(ev.frame, ev.track);
            return;
        }

        switch (ev.type) {
            case EV_NOTE_ON:
                note_on(ev);
                break;

            // ⚠️ THREE MODES SINCE E4, and the third is not a synonym for the second. NOTE_OFF_KEY is
            // a live key let go of (MIDI plan §4.1): a one-shot with no release envelope IGNORES it
            // and plays out, where a KIL fades the same voice. The difference lives in
            // `SamplerVoice::keyRelease`; what belongs here is only the translation, and an explicit
            // third arm rather than a widened `else` — a mode this switch does not know must not
            // silently become a KIL.
            case EV_NOTE_OFF:
                switch (ev.noteOff.mode) {
                    case NOTE_OFF_CUT: engine_->scheduleKill(ev.frame, ev.track);       break;
                    case NOTE_OFF_KEY: engine_->scheduleKeyRelease(ev.frame, ev.track); break;
                    default:           engine_->scheduleNoteOff(ev.frame, ev.track);    break;
                }
                break;

            case EV_CC: {
                const float v = f32_from_bits(ev.cc.valueBits);
                // A symbolic slot id (CCA-CCD) names a controller the INSTRUMENT chose, so it is
                // resolved here — against the same instrument the routing gate above just used —
                // and then treated as any other controller number. That is what makes one FX column
                // drive a sampler and an external synth: `CCA` on an instrument whose slot A is CC 10
                // pans this engine's voice and pans the gear, from the same phrase (plan §6/§8.3).
                const int param = resolve_cc(instr, ev.cc.param);
                switch (param) {
                    case CC_VOLUME:      engine_->scheduleTrackPhraseVol(ev.frame, ev.track, v);  break;
                    case CC_PAN:         engine_->scheduleVoicePan(ev.frame, ev.track, v);        break;
                    case CC_REVERB_SEND: engine_->scheduleVoiceReverbSend(ev.frame, ev.track, v); break;
                    case CC_DELAY_SEND:  engine_->scheduleVoiceDelaySend(ev.frame, ev.track, v);  break;
                    // CUT / RES. Both write the SOUNDING voice's own filter and are gone with it, so
                    // there is no restore on stop() — the instrument's values come back with the next
                    // note-on. An instrument with FILTER TYPE = OFF runs no filter and swallows them.
                    case CC_FILTER_CUT:  engine_->scheduleVoiceFilterCut(ev.frame, ev.track, v);  break;
                    case CC_FILTER_RES:  engine_->scheduleVoiceFilterRes(ev.frame, ev.track, v);  break;
                    // The mixer faders (VTR / VMV). Engine-only ids — `midi_out.h` drops both, which
                    // is the one place the two consumers are meant to disagree (event.h).
                    //
                    // ⚠️ VTR IS SUBJECT TO THE EXTERNAL GATE ABOVE AND VMV IS NOT, and that asymmetry
                    // is the right one rather than an oversight: a track playing an EXTERNAL instrument
                    // makes no audio HERE, so its fader has nothing to move, while the master fader
                    // carries every other track and must move whatever track the effect was typed on.
                    // VMV rides TRACK_GLOBAL, which the gate cannot resolve to an instrument and so
                    // never claims.
                    case CC_TRACK_VOL:   engine_->scheduleTrackVolume(ev.frame, ev.track, v);    break;
                    case CC_MASTER_VOL:  engine_->scheduleMasterVolume(ev.frame, v);             break;
                    // ⚠️ EVERY OTHER §6 ID IS DROPPED HERE, AND THAT IS A GAP, NOT A DECISION.
                    // Attack/release (72/73) and the GP drive/crush pair are real sampler params — but
                    // they are INSTRUMENT-STATIC in this engine (setInstrumentParams), and only the ids
                    // above have an entry in the sample-accurate ParamUpdateQueue. Wiring one means a
                    // new queue entry and a per-voice override in the DSP, which is what CUT/RES cost;
                    // until then a `CCA` pointing at 72 moves external gear and nothing here.
                    default: break;
                }
                break;
            }

            // ⚠️ NO ENGINE PATH EXISTS FOR EITHER, AND SAYING SO IS THE POINT OF THE ARMS.
            //   • MPG: this engine has no notion of a program. The soundfont module does (a TSF
            //     preset), but selecting one mid-take needs a per-track preset override that
            //     scheduleSoundfontNote does not have.
            //   • MPB: `schedulePitchBend` is PBN — a RATE in semitones per step, applied over time.
            //     An absolute 14-bit bend is a different quantity and there is no param to hold it.
            // Both reach external gear correctly (midi_out.h); on a sampler they are silent. An empty
            // arm that names why beats a `default:` that cannot tell "dropped" from "forgotten".
            case EV_PROGRAM:
            case EV_PITCH_BEND:
                break;

            case EV_EXT_PITCH_RATE:
                engine_->schedulePitchBend(ev.frame, ev.track,
                                           f32_from_bits(ev.extPitchRate.rateBits),
                                           ev.extPitchRate.tempo);
                break;

            case EV_EXT_VIBRATO:
                engine_->scheduleVibrato(ev.frame, ev.track,
                                         f32_from_bits(ev.extVibrato.speedBits),
                                         f32_from_bits(ev.extVibrato.depthBits));
                break;

            case EV_EXT_TABLE_ROW:
                engine_->scheduleVoiceTableRow(ev.frame, ev.track, ev.extTableRow.row);
                break;

            case EV_EXT_REVERSE:
                engine_->scheduleVoiceReverse(ev.frame, ev.track,
                                              ev.extReverse.reverse != 0, ev.extReverse.restart != 0);
                break;

            case EV_EXT_EQ_SLOT:
                engine_->scheduleVoiceEqSlot(ev.frame, ev.track, ev.extEqSlot.slot);
                break;

            case EV_EXT_MASTER_EQ:
                engine_->scheduleMasterEqSlot(ev.frame, ev.extMasterEq.slot);
                break;

            // ⚠️ EQN's morph is subject to the external gate above and EQM's is not — the same
            // asymmetry VTR/VMV carry, and the right one: a track playing an EXTERNAL instrument
            // makes no audio here, so its EQ has nothing to move, while the master EQ carries every
            // other track and must move whatever track the fade was typed on. The master tick rides
            // TRACK_GLOBAL, which the gate cannot resolve to an instrument and so never claims.
            case EV_EXT_EQ_MORPH:
                engine_->scheduleVoiceEqBands(ev.frame, ev.track, eq_bands_of(ev));
                break;

            case EV_EXT_MASTER_EQ_MORPH:
                engine_->scheduleMasterEqBands(ev.frame, eq_bands_of(ev));
                break;

            default: break;   // schema-complete: no other emitters exist (event-schema §3)
        }
    }

  private:
    /**
     * A morph tick's payload in the engine's own struct. The two forms hold the same twelve authored
     * hex numbers in the same order; this is the seam's job — copying, not deriving.
     */
    static EqBandsHex eq_bands_of(const Event& ev) {
        EqBandsHex b;
        for (int i = 0; i < 3; ++i) {
            b.type[i] = ev.extEqMorph.type[i];
            b.freq[i] = ev.extEqMorph.freq[i];
            b.gain[i] = ev.extEqMorph.gain[i];
            b.q[i]    = ev.extEqMorph.q[i];
        }
        return b;
    }

    /**
     * The controller number for this event, via the model's shared rule (`resolve_cc_param`) — so a
     * `CCA` means the same controller here as it does on the wire. −1 (unknown instrument, or an
     * unassigned slot) matches no case in the switch and is therefore a no-op.
     */
    int resolve_cc(int16_t instrument, uint8_t param) const {
        // ⚠️ A LITERAL id RETURNS BEFORE THE INSTRUMENT IS EVEN LOOKED AT, and that early return is
        // load-bearing rather than an optimisation. PAN/REV/DEL are track-scoped and older than any of
        // this: on an FX-only step before the track's first note-on, `TrackInstruments` resolves to
        // INSTRUMENT_NONE and they are applied ANYWAY — the engine acts on whatever voice the track
        // has. Requiring an instrument for every CC would silently drop those, in a channel neither
        // the traces (which stop above this file) nor `live == render` (same code both sides) can see.
        // Only a SLOT id genuinely needs an instrument, because only a letter needs translating.
        if (cc_slot_index(param) < 0) return param;
        if (instrument < 0 || static_cast<size_t>(instrument) >= project_->instruments.size()) return -1;
        return resolve_cc_param(project_->instruments[static_cast<size_t>(instrument)], param);
    }

    bool is_external(int16_t instrument) const {
        if (instrument < 0 || static_cast<size_t>(instrument) >= project_->instruments.size()) return false;
        return instrument_routes_external(project_->instruments[static_cast<size_t>(instrument)]);
    }

    void note_on(const Event& ev) {
        // `ev.track <= 7` alone: the field is a uint8_t, so the `>= 0` half of Kotlin's guard is a
        // tautology here (gcc says so). The bound that does the work is the upper one — TRACK_PREVIEW
        // and TRACK_GLOBAL are above it, and neither belongs in an eight-bit track mask.
        if (ev.track <= 7) trackMask_ |= (1 << ev.track);
        plan_note_on(*engine_, ev, *project_, *routing_, tableLoaded_);
    }

    AudioEngine*   engine_  = nullptr;
    const Project* project_ = nullptr;
    const Routing* routing_ = nullptr;
    TrackInstruments tracks_;
    bool tableLoaded_[POOL_TABLES] = {false};
    int  trackMask_ = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H
