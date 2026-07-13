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
#include "event.h"
#include "model.h"
#include "router.h"
#include "voice_derive.h"

namespace songcore {

// Routing + plan_note_on (the note path itself) live in voice_derive.h — engine-agnostic, so
// tools/ptvoice can instantiate the same code against a recorder and golden it.

class EngineConsumer : public IMidiConsumer {
  public:
    EngineConsumer(AudioEngine* engine, const Project* project, const Routing* routing)
        : engine_(engine), project_(project), routing_(routing) {}

    // Transport is not an engine event — the host owns queue clearing and the master-EQ restore.
    void on_play(const std::string&, const std::string&, int64_t, int, int) override {}
    void on_stop() override {}

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
        switch (ev.type) {
            case EV_NOTE_ON:
                note_on(ev);
                break;

            case EV_NOTE_OFF:
                if (ev.noteOff.mode == NOTE_OFF_CUT) engine_->scheduleKill(ev.frame, ev.track);
                else                                 engine_->scheduleNoteOff(ev.frame, ev.track);
                break;

            case EV_CC: {
                const float v = f32_from_bits(ev.cc.valueBits);
                switch (ev.cc.param) {
                    case CC_VOLUME:      engine_->scheduleTrackPhraseVol(ev.frame, ev.track, v);  break;
                    case CC_PAN:         engine_->scheduleVoicePan(ev.frame, ev.track, v);        break;
                    case CC_REVERB_SEND: engine_->scheduleVoiceReverbSend(ev.frame, ev.track, v); break;
                    case CC_DELAY_SEND:  engine_->scheduleVoiceDelaySend(ev.frame, ev.track, v);  break;
                    default: break;   // ids beyond the four wired today arrive in MIDI phase D
                }
                break;
            }

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

            default: break;   // schema-complete: no other emitters exist (event-schema §3)
        }
    }

  private:
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
    bool tableLoaded_[POOL_TABLES] = {false};
    int  trackMask_ = 0;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_ENGINE_CONSUMER_H
