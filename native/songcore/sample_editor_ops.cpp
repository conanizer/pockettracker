// SampleEditorOps' bodies; what each one is for is said at its declaration.

#include "sample_editor_ops.h"

#include <algorithm>

#include "engine_setup.h"

namespace songcore {

float SampleEditorOps::sample_playback_position(int id) const {
    return engine_ ? engine_->getSamplePlaybackPosition(id) : -1.0f;
}

std::vector<float> SampleEditorOps::sample_waveform(int id, int bins, int startFrame, int endFrame,
                                                    int channel) const {
    std::vector<float> out(static_cast<size_t>(std::max(bins, 0)) * 2, 0.0f);
    if (!engine_ || bins <= 0) return out;

    const int  len   = engine_->getSampleLength(id);
    const bool whole = (startFrame <= 0) && (endFrame <= 0 || endFrame >= len);

    engine_->getSampleWaveformRangeSource(id, whole ? 0 : startFrame, whole ? len : endFrame,
                                          out.data(), bins, channel);
    return out;
}

std::vector<int> SampleEditorOps::detect_transients(int id, int sensitivity) const {
    if (!engine_) return {};
    int       markers[128];
    const int n = engine_->detectTransients(id, sensitivity, markers, 128);
    return std::vector<int>(markers, markers + std::max(n, 0));
}

int SampleEditorOps::find_zero_crossing(int id, int frame, int dir, int sourceMode) const {
    return engine_ ? engine_->findZeroCrossing(id, frame, dir, 512, sourceMode) : frame;
}

void SampleEditorOps::apply_sample_fx(int id, int fxType, int fxValue) {
    if (!engine_) return;
    engine_->applySampleFx(id, fxType, fxValue,
                           static_cast<float>(sample_rate_of(id)), project_.limiterPreGain);
}

bool SampleEditorOps::apply_rate_and_bits(int id, int factor, int bits) {
    return songcore::apply_rate_and_bits(engine_, routing_, rateCache_, id, factor, bits);
}

bool SampleEditorOps::pitch_shift_sample(int id, float semitones) {
    return songcore::pitch_shift_sample(engine_, rateCache_, id, semitones);
}

bool SampleEditorOps::time_stretch_sample(int id, float ratio, float grainMs) {
    return songcore::time_stretch_sample(engine_, rateCache_, id, ratio, grainMs,
                                         static_cast<float>(sample_rate_of(id)));
}

bool SampleEditorOps::sync_sample(int id, const SampleSync& sync) {
    if (!engine_) return true;
    return run_sample_sync(*engine_, rateCache_, id, sync, static_cast<float>(sample_rate_of(id)));
}

void SampleEditorOps::preview_sample_editor(int id, int sourceMode, int64_t selStart, int64_t selEnd,
                                            int totalFrames, int pitchSemitones, const SampleSync& sync) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
    Instrument& ins = project_.instruments[static_cast<size_t>(id)];

    const Note savedRoot = ins.root;
    // A pending pitch shift is heard by transposing the ROOT; nothing is resampled until SAVE.
    if (pitchSemitones != 0)
        ins.root = note_from_midi(std::clamp(note_to_midi(ins.root) + pitchSemitones, 0, 127));

    const int slot = prepare_sync_preview(*engine_, id, sourceMode, sync,
                                          static_cast<float>(sample_rate_of(id)));

    // A voice's window is keyed on the SLOT it plays from, so the params go to the scratch slot
    // when there is one.
    push_instrument_playback_params(*engine_, ins, slot);
    // After the push, which clears any previous window. A channel copy is frame-for-frame, so the
    // selection indexes it as it is; a synced one is scaled onto its new length.
    if (totalFrames > 0 && selEnd > selStart) {
        const int64_t len = engine_->getSampleLength(slot);
        const int64_t a   = std::clamp<int64_t>(selStart * len / totalFrames, 0, len);
        const int64_t b   = std::clamp<int64_t>(selEnd * len / totalFrames, 0, len);
        if (b > a) engine_->setInstrumentFrameWindow(slot, static_cast<int>(a), static_cast<int>(b));
    }
    preview_instrument_dry(*engine_, ins, slot, routing_.sampleRateRatio[id]);

    ins.root = savedRoot;   // read at SCHEDULE time, so it can go back at once
}

void SampleEditorOps::finish_sample_preview(int id) {
    if (!engine_ || id < 0 || id >= POOL_INSTRUMENTS) return;
    Instrument& ins = project_.instruments[static_cast<size_t>(id)];
    push_instrument_playback_params(*engine_, ins, id);
    // The push only reaches the instrument's own slot; a channel audition used the scratch slot.
    engine_->setInstrumentFrameWindow(SOURCE_PREVIEW_SLOT, -1, -1);
    // The three the DRY preview switched off.
    push_instrument_mod_eq_sends(*engine_, ins, engine_->getSampleRate());
}

bool SampleEditorOps::save_sample_wav(int id, const std::string& path, const std::vector<int>& cuePoints,
                                      int sourceMode, bool hasStereo, int bits) {
    if (!engine_) return false;
    return songcore::save_sample_wav(*engine_, routing_, id, path, cuePoints, sourceMode, hasStereo,
                                     bits);
}

void SampleEditorOps::adopt_saved_sample(int id, int bits) {
    if (!engine_) return;
    const int depth = songcore::resolve_save_bits(*engine_, id, bits);
    engine_->adoptSavedSampleFormat(id, depth, depth == 32 && engine_->isSampleFloat(id));
    rateCache_.clear(id);
}

int SampleEditorOps::chop_sample(int id, const std::string& dir, const std::string& baseName,
                                 const std::vector<std::pair<int64_t, int64_t>>& slices, int sourceMode,
                                 bool hasStereo, int bits) {
    if (!engine_) return 0;
    return songcore::chop_sample(*engine_, routing_, id, dir, baseName, slices, sourceMode, hasStereo,
                                 bits);
}


}  // namespace songcore
