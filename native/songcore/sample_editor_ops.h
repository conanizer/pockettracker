#ifndef POCKETTRACKER_SONGCORE_SAMPLE_EDITOR_OPS_H
#define POCKETTRACKER_SONGCORE_SAMPLE_EDITOR_OPS_H

// The sample editor's engine side: what it reads of a sample and every operation it runs on one.
// Each is guarded on the engine, so tests run with no device. Owned by `SongcoreHost`, which hands
// it the project and the Routing it keeps.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../audio-engine.h"
#include "model.h"
#include "sample_edit.h"
#include "voice_derive.h"   // Routing

namespace songcore {

class SampleEditorOps {
  public:
    SampleEditorOps(AudioEngine* engine, Project& project, Routing& routing)
        : engine_(engine), project_(project), routing_(routing) {}

    int  sample_length(int id) const { return engine_ ? engine_->getSampleLength(id) : 0; }
    bool has_stereo_data(int id) const { return engine_ && engine_->hasStereoData(id); }
    // The depth the sample came in at — the ceiling of the BIT cell.
    int  sample_bit_depth(int id) const { return engine_ ? engine_->getSampleBitDepth(id) : 16; }

    // The file's rate; 44100 for an empty slot (sample_edit.h).
    int sample_rate_of(int id) const { return original_sample_rate(engine_, routing_, id); }

    // 0..1 while the sample sounds, else −1 — the waveform's playhead.
    float sample_playback_position(int id) const;

    // `bins` (min, max) pairs. `channel`: 0 left, 1 right, 2 averaged. (0, 0) = the whole sample.
    std::vector<float> sample_waveform(int id, int bins, int startFrame = 0, int endFrame = 0,
                                       int channel = 2) const;

    // Slice boundaries at `sensitivity`, at most 128.
    std::vector<int> detect_transients(int id, int sensitivity) const;

    // The nearest zero crossing (`dir` −1 back, +1 forward, 0 either) in the signal SOURCE will cut.
    int find_zero_crossing(int id, int frame, int dir, int sourceMode = 0) const;

    int clipboard_length() const { return engine_ ? engine_->getClipboardLength() : 0; }

    // The ops that rebuild a buffer return false only when memory was refused; nothing then changed.
    bool backup_sample(int id) { return !engine_ || engine_->backupSample(id); }
    bool undo_sample(int id) { return !engine_ || engine_->undoSample(id); }
    // The editor is closing: drop its undo copy.
    void free_sample_undo(int id) { if (engine_) engine_->freeSampleUndo(id); }

    bool crop_sample(int id, int start, int end) { return !engine_ || engine_->cropSample(id, start, end); }
    bool delete_sample_region(int id, int start, int end) { return !engine_ || engine_->deleteSampleRegion(id, start, end); }
    bool copy_region(int id, int start, int end) { return !engine_ || engine_->copyRegion(id, start, end); }
    bool paste_region(int id, int insertAt) { return !engine_ || engine_->pasteRegion(id, insertAt); }

    void normalize_sample(int id, int start, int end) { if (engine_) engine_->normalizeSample(id, start, end); }
    void fade_in_sample(int id, int start, int end) { if (engine_) engine_->fadeInSample(id, start, end); }
    void fade_out_sample(int id, int start, int end) { if (engine_) engine_->fadeOutSample(id, start, end); }
    void silence_region(int id, int start, int end) { if (engine_) engine_->silenceRegion(id, start, end); }
    void reverse_sample(int id, int start, int end) { if (engine_) engine_->reverseSample(id, start, end); }

    // The FX row: START applies for real and plays, the next gesture restores this backup; only APPLY
    // keeps it. Separate from the undo slot.
    bool save_fx_preview_backup(int id) { return !engine_ || engine_->saveFxPreviewBackup(id); }
    void restore_fx_preview_backup() { if (engine_) engine_->restoreFxPreviewBackup(); }

    void apply_sample_fx(int id, int fxType, int fxValue);

    // The three that change the rate ratio, so live in songcore (sample_edit.h).
    bool apply_rate_and_bits(int id, int factor, int bits);
    bool pitch_shift_sample(int id, float semitones);
    bool time_stretch_sample(int id, float ratio, float grainMs = sola::DEFAULT_GRAIN_MS);
    // The FX row's SYNC, for good. ⚠️ False can follow a pitch that went through (`run_sample_sync`).
    bool sync_sample(int id, const SampleSync& sync);

    // The editor's START: the sample dry at its root, through the SOURCE channel, windowed to the
    // selection. `sync` previews the FX row's SYNC from a copy.
    // ⚠️ The window goes in as FRAMES, not the 0-255 start/end grid — the audition must be exactly
    // what CROP keeps. It is read when the note fires, so `finish_sample_preview()` ends it later.
    void preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                               int totalFrames, int pitchSemitones, const SampleSync& sync = {});

    // Put the instrument back (window, EQ, sends, modulation) — on the dispatcher's 100 ms deadline,
    // or at once on a second START.
    void finish_sample_preview(int id);

    // The edited PCM → a WAV, slices in the `cue ` chunk. `bits` 0 = the loaded depth.
    bool save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                         int sourceMode, bool hasStereo, int bits = 0);

    // After a SAVE that kept the buffer: it is the file now — take its depth, drop the RATE/BIT
    // original, or the next RATE/BIT touch would restore pre-save audio.
    void adopt_saved_sample(int id, int bits);

    // Every slice → its own WAV in `dir`. Returns how many were written.
    int chop_sample(int id, const std::string& dir, const std::string& baseName,
                    const std::vector<std::pair<int64_t, int64_t>>& slices, int sourceMode,
                    bool hasStereo, int bits = 0);

  private:
    AudioEngine* engine_;
    Project&     project_;
    Routing&     routing_;
    // The RATE row's ratio cache (sample_edit.h), so LOFI → HIGH restores rather than compounds.
    RateCache    rateCache_;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_SAMPLE_EDITOR_OPS_H
