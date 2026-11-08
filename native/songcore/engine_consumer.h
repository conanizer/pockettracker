#ifndef POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H
#define POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H

// The engine consumer: bus events become engine calls. Thin — the derivation is voice_derive.h.

#include <cstdint>

#include "../audio-engine.h"
#include "table_automation.h"  // only for the cross-checks below
#include "automation.h"           // only for the cross-checks below
#include "effects.h"              // only for the cross-checks below; no effect is resolved here
#include "event.h"
#include "model.h"
#include "router.h"
#include "voice_derive.h"

namespace songcore {

// The engine's table rolls cap a command where the editor does.
static_assert(::tableFxCeiling(FX_INS) == effect_value_max(FX_INS) &&
              ::tableFxCeiling(FX_EQN) == effect_value_max(FX_EQN) &&
              ::tableFxCeiling(FX_CUT) == effect_value_max(FX_CUT),
              "audio-defs.h tableFxCeiling has drifted from effect_value_max");

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

    // Every table the engine lacks, pushed now — for a live key, which no note path pushes ahead of.
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
            // The mixer faders. VTR is subject to the EXTERNAL gate; VMV rides TRACK_GLOBAL, which is not.
            case CC_TRACK_VOL:   engine.scheduleTrackVolume(frame, track, v);    break;
            case CC_MASTER_VOL:  engine.scheduleMasterVolume(frame, v);          break;
            // TIM: global like the master fader, ungated for the same reason.
            case CC_DELAY_TIME:  engine.scheduleDelayTime(frame, v);             break;
            // Any other CC moves external gear only.
            default: break;
        }
    }

    void consume(const Event& ev) override {
        if (!engine_ || !project_) return;

        // The routing gate: an EXTERNAL instrument raises no voice here, cable or not.
        // ⚠️ Both consumers decide through the same two functions, or an event plays twice or never.
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

            // ⚠️ Three modes; an unknown one must not silently become a KIL.
            case EV_NOTE_OFF:
                switch (ev.noteOff.mode) {
                    case NOTE_OFF_CUT: engine_->scheduleCut(ev.frame, ev.track);        break;
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

            // MPG and MPB have no engine form; they reach external gear only.
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
        // ⚠️ A literal id returns before the instrument lookup: PAN on an FX-only step before the
        // track's first note has none, and must still apply.
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
