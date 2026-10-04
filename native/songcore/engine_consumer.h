#ifndef POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H
#define POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H

// ─── The engine consumer — bus events become audio ───────────────────────────────────────────────
//
// Everything below the event seam, for every platform. Deliberately thin: the derivation (frequency,
// slice window, SF slot/velocity, tick→frame, modulation pushes) is pure functions in voice_derive.h,
// where it can be tested without an engine. What is left here is look up, derive, call.

#include <cstdint>

#include "../audio-engine.h"
#include "../table_automation.h"  // only for the cross-checks below
#include "automation.h"           // only for the cross-checks below
#include "effects.h"              // only for the cross-checks below; no effect is resolved here
#include "event.h"
#include "model.h"
#include "router.h"
#include "voice_derive.h"

namespace songcore {

// ⚠️ The table engine keeps its own copy of the effect codes it processes (`native/audio-defs.h`),
// because it sits below this seam. This is the one place both lists are visible, so they are made to
// agree here — a drifted code would draw by its right name and do something else.
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
static_assert(::FX_LPF    == FX_LPF,    "audio-defs.h FX_LPF has drifted from effects.h");
static_assert(::FX_HPF    == FX_HPF,    "audio-defs.h FX_HPF has drifted from effects.h");
static_assert(::FX_BPF    == FX_BPF,    "audio-defs.h FX_BPF has drifted from effects.h");
static_assert(::FX_DRV    == FX_DRV,    "audio-defs.h FX_DRV has drifted from effects.h");
static_assert(::FX_CRU    == FX_CRU,    "audio-defs.h FX_CRU has drifted from effects.h");
static_assert(::FX_FIN    == FX_FIN,    "audio-defs.h FX_FIN has drifted from effects.h");
static_assert(::FX_LPO    == FX_LPO,    "audio-defs.h FX_LPO has drifted from effects.h");
static_assert(::FX_TIM    == FX_TIM,    "audio-defs.h FX_TIM has drifted from effects.h");
static_assert(::FX_RND    == FX_RND,    "audio-defs.h FX_RND has drifted from effects.h");
static_assert(::FX_PAN    == FX_PAN,    "audio-defs.h FX_PAN has drifted from effects.h");
static_assert(::FX_RNL    == FX_RNL,    "audio-defs.h FX_RNL has drifted from effects.h");
static_assert(::FX_CHA    == FX_CHA,    "audio-defs.h FX_CHA has drifted from effects.h");
static_assert(::FX_INS    == FX_INS,    "audio-defs.h FX_INS has drifted from effects.h");
static_assert(::tableFxCeiling(FX_INS) == effect_value_max(FX_INS) &&
              ::tableFxCeiling(FX_EQN) == effect_value_max(FX_EQN) &&
              ::tableFxCeiling(FX_CUT) == effect_value_max(FX_CUT),
              "audio-defs.h tableFxCeiling has drifted from effect_value_max");

// CRU's nibble split is spelled on both sides of the seam too; checked over every byte.
constexpr bool crush_split_matches_the_engine() {
    for (int v = 0; v <= 255; ++v)
        if (::crushBitsOf(v) != crush_cmd_bits(v) ||
            ::crushDownsampleOf(v) != crush_cmd_downsample(v)) return false;
    return true;
}
static_assert(crush_split_matches_the_engine(),
              "audio-defs.h's CRU nibble split has drifted from effects.h's");

// ─── …and the third list: what a TABLE row's AUS may ramp ────────────────────────────────────────
//
// `native/table_automation.h` serves both the engine and the table editor, so it spells its codes as
// literals. Checked here, in both directions, so neither list can grow an entry the other lacks.
static_assert(table_automation::FX_AUS_CODE == FX_AUS,
              "table_automation.h AUS has drifted from effects.h");
static_assert(table_automation::FX_AUF_CODE == FX_AUF,
              "table_automation.h AUF has drifted from effects.h");
static_assert(table_automation::EQ_PRESET_SLOTS == POOL_EQPRESETS,
              "table_automation.h's preset-slot ceiling has drifted from the pool size");

constexpr bool table_arms_match_the_engine() {
    for (int i = 0; i < table_automation::ARM_COUNT; ++i) {
        const int c = table_automation::ARMS[i].code;
        if (c != ::FX_HOP && c != ::FX_TIC && c != ::FX_KILL && c != ::FX_OFFSET &&
            c != ::FX_THO && c != ::FX_VOLUME && c != ::FX_EQN && c != ::FX_EQM &&
            c != ::FX_CUT && c != ::FX_RES &&
            c != ::FX_LPF && c != ::FX_HPF && c != ::FX_BPF &&
            c != ::FX_DRV && c != ::FX_CRU && c != ::FX_FIN &&
            c != ::FX_LPO && c != ::FX_TIM && c != ::FX_PAN) return false;
    }
    return table_automation::has_arm(::FX_HOP)    && table_automation::has_arm(::FX_TIC)  &&
           table_automation::has_arm(::FX_KILL)   && table_automation::has_arm(::FX_OFFSET) &&
           table_automation::has_arm(::FX_THO)    && table_automation::has_arm(::FX_VOLUME) &&
           table_automation::has_arm(::FX_EQN)    && table_automation::has_arm(::FX_EQM)  &&
           table_automation::has_arm(::FX_CUT)    && table_automation::has_arm(::FX_RES)  &&
           table_automation::has_arm(::FX_LPF)    && table_automation::has_arm(::FX_HPF)  &&
           table_automation::has_arm(::FX_BPF)    &&
           table_automation::has_arm(::FX_DRV)    && table_automation::has_arm(::FX_CRU)  &&
           table_automation::has_arm(::FX_FIN)    && table_automation::has_arm(::FX_LPO) &&
           table_automation::has_arm(::FX_TIM)    && table_automation::has_arm(::FX_PAN);
}
static_assert(table_arms_match_the_engine(),
              "table_automation.h's arm list and audio-defs.h's effect codes disagree — one of them "
              "has an effect the other does not");

// The `rampable` / `eqPreset` flags are derived from the registry, not chosen: a wrong one fails here.
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

// Routing and plan_note_on (the note path) live in voice_derive.h, engine-agnostic so the tests can
// check them.

class EngineConsumer : public IMidiConsumer {
  public:
    EngineConsumer(AudioEngine* engine, const Project* project, const Routing* routing)
        : engine_(engine), project_(project), routing_(routing) {}

    // Transport belongs to the host. The track→instrument map is ours, and a stale one would route the
    // first track-scoped event of a new take at the last take's instrument.
    void on_play(const std::string&, const std::string&, int64_t, int, int) override { tracks_.reset(); }
    void on_stop() override { tracks_.reset(); }

    // A push may have changed the tables, so the "already sent" cache must not survive it.
    void invalidate_tables() {
        for (int i = 0; i < POOL_TABLES; ++i) tableLoaded_[i] = false;
    }

    // Which of tracks 0-7 had a note scheduled this session; the OCTA visualizer lights one lane per
    // bit. The host clears it on stop.
    int  track_mask() const { return trackMask_; }
    void clear_track_mask() { trackMask_ = 0; }

    /**
     * Every table the engine does not hold yet, pushed now. The note path pushes lazily; a live key is
     * scheduled by the audio thread with nothing pushed ahead of it, so the host calls this from its
     * poll. Both share the cache.
     */
    void push_tables(const Project& project) {
        if (!engine_) return;
        const int count = static_cast<int>(project.tables.size());
        for (int id = 0; id < count && id < POOL_TABLES; ++id) {
            if (tableLoaded_[id]) continue;
            push_table(*engine_, project, id);
            tableLoaded_[id] = true;
        }
    }

    /** A live key's record, already played by the engine: only learn which instrument the track is on,
     *  so the next track-scoped event resolves the way the key did. */
    void observe_live(const Event& ev) { tracks_.observe(ev); }

    /** One resolved controller onto the engine's live-parameter queues. Shared by the bus consumer and
     *  a live key's drain (host.h), so a controller means the same from a phrase and from a cable. */
    template <typename Engine>
    static void apply_cc(Engine& engine, int64_t frame, uint8_t track, int param, float v) {
        switch (param) {
            case CC_VOLUME:      engine.scheduleTrackPhraseVol(frame, track, v);  break;
            // Per-voice: they shape the sounding note and are gone with it, so nothing to restore on
            // stop(). See the engine's `applyVoiceCc`.
            case CC_PAN:
            case CC_REVERB_SEND:
            case CC_DELAY_SEND:
            case CC_FILTER_CUT:
            case CC_FILTER_RES:
            case CC_FILTER_LP:
            case CC_FILTER_HP:
            case CC_FILTER_BP:
            case CC_DRIVE:
            case CC_CRUSH:
            case CC_FINE_TUNE:
            case CC_LOOP_SLIDE:
                engine.scheduleVoiceCc(frame, track, param, v);
                break;
            // The mixer faders. Engine-only: `midi_out.h` drops both.
            // ⚠️ VTR is subject to the EXTERNAL gate and VMV is not, deliberately: an EXTERNAL track
            // makes no audio here, while the master carries every other track. VMV rides
            // TRACK_GLOBAL, which the gate never claims.
            case CC_TRACK_VOL:   engine.scheduleTrackVolume(frame, track, v);    break;
            case CC_MASTER_VOL:  engine.scheduleMasterVolume(frame, v);          break;
            // TIM: global like the master fader, ungated for the same reason.
            case CC_DELAY_TIME:  engine.scheduleDelayTime(frame, v);             break;
            // Every other CC id is dropped here — a gap, not a decision. Attack/release and the GP
            // drive/crush are instrument-static in this engine; wiring one needs a ParamUpdateQueue
            // entry and a per-voice override. Until then a `CCA` on 72 moves external gear only.
            default: break;
        }
    }

    void consume(const Event& ev) override {
        if (!engine_ || !project_) return;

        // ── The routing gate ─────────────────────────────────────────────────────────────────────
        //
        // An EXTERNAL instrument belongs to ExternalConsumer (midi_out.h) and raises no voice here,
        // even with no cable attached.
        // ⚠️ Both consumers decide through `instrument_routes_external` and `TrackInstruments`: an
        // event both claim plays twice, one neither claims is silence.
        const int16_t prev  = tracks_.current(ev.track);
        const int16_t instr = tracks_.observe(ev);
        if (is_external(instr)) {
            // ⚠️ A track flipping from an internal instrument to an external one would leave its
            // voice with nothing to stop it — later events route away — so the flip ends it.
            if (ev.type == EV_NOTE_ON && prev >= 0 && !is_external(prev))
                engine_->scheduleKill(ev.frame, ev.track);
            return;
        }

        switch (ev.type) {
            case EV_NOTE_ON:
                note_on(ev);
                break;

            // ⚠️ Three modes. NOTE_OFF_KEY is a live key let go: a one-shot without a release
            // envelope ignores it and plays out, where a KIL fades it (`SamplerVoice::keyRelease`).
            // An unknown mode must not silently become a KIL.
            case EV_NOTE_OFF:
                switch (ev.noteOff.mode) {
                    case NOTE_OFF_CUT: engine_->scheduleKill(ev.frame, ev.track);       break;
                    case NOTE_OFF_KEY: engine_->scheduleKeyRelease(ev.frame, ev.track); break;
                    default:           engine_->scheduleNoteOff(ev.frame, ev.track);    break;
                }
                break;

            case EV_CC:
                // A CCA-CCD slot names the instrument's chosen controller, resolved against the same
                // instrument the gate used — so one FX column drives a sampler and external gear alike.
                apply_cc(*engine_, ev.frame, ev.track, resolve_cc(instr, ev.cc.param),
                         f32_from_bits(ev.cc.valueBits));
                break;

            // No engine path for either:
            //   • MPG: no notion of a program here; a per-track SF preset override does not exist.
            //   • MPB: `schedulePitchBend` is PBN, a RATE; an absolute bend has no param to hold it.
            // Both reach external gear (midi_out.h); on a sampler they are silent.
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

            // ⚠️ EQN's morph is gated and EQM's is not — the VTR/VMV asymmetry, for the same reason.
            case EV_EXT_EQ_MORPH:
                engine_->scheduleVoiceEqBands(ev.frame, ev.track, eq_bands_of(ev));
                break;

            case EV_EXT_MASTER_EQ_MORPH:
                engine_->scheduleMasterEqBands(ev.frame, eq_bands_of(ev));
                break;

            default: break;   // schema-complete: no other emitters exist
        }
    }

  private:
    /** A morph tick's payload in the engine's struct: the same twelve hex numbers, copied. */
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

    /** The controller number for this event, via `resolve_cc_param`, so `CCA` means the same here as on
     *  the wire. −1 (unknown instrument, unassigned slot) matches no case and is a no-op. */
    int resolve_cc(int16_t instrument, uint8_t param) const {
        // ⚠️ A literal id returns before the instrument is looked up. PAN/REV/DEL on an FX-only step
        // before the track's first note resolve to INSTRUMENT_NONE and must still apply; only a slot
        // letter needs an instrument to translate it.
        if (cc_slot_index(param) < 0) return param;
        if (instrument < 0 || static_cast<size_t>(instrument) >= project_->instruments.size()) return -1;
        return resolve_cc_param(project_->instruments[static_cast<size_t>(instrument)], param);
    }

    bool is_external(int16_t instrument) const {
        if (instrument < 0 || static_cast<size_t>(instrument) >= project_->instruments.size()) return false;
        return instrument_routes_external(project_->instruments[static_cast<size_t>(instrument)]);
    }

    void note_on(const Event& ev) {
        // `ev.track` is unsigned; the upper bound excludes TRACK_PREVIEW and TRACK_GLOBAL.
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
