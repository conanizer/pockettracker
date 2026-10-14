// The engine's lifecycle, the schedule calls the control thread makes, the per-note modulation, the
// EQ, meters and scopes, the bus setters, the offline render and the metronome. The sample pool,
// the SoundFont bank, the table walk and the block itself are in the engine-*.cpp files beside it.
#include "audio-engine.h"
#include "kissfft/kiss_fftr.h"
#include "mods/mod-runner.h"
#include "mods/modules/pitch-slide-module.h"
#include "mods/modules/vibrato-module.h"
#include <cstdint>
#include <vector>
// MSVC signals x86/x64 only through _M_X64 / _M_IX86; without those arms it loses denormal protection.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && _M_IX86_FP >= 2)
#define PT_HAS_SSE_DENORMAL_CTRL 1
#include <pmmintrin.h>  // FTZ/DAZ MXCSR macros for setFlushToZeroForCurrentThread
#endif

SoundfontVoice sfVoices[SF_VOICE_COUNT];

AudioEngine::AudioEngine() {
    for (int i = 0; i < 256; i++) {
        samples[i] = nullptr;
        samplesRight[i] = nullptr;
        sampleLengths[i] = 0;
        sampleGen[i] = 0;
        sampleBitDepth[i] = 16;
    }
    resetTic00Cursors();
    globalFrameCounter.store(0, std::memory_order_relaxed);
    noteSeedEntropy = ((uint32_t)nowMs() * 2654435761u) | 1u;  // vary RND/DRNK per app session

    // 64 covers a dense AUS/AUF ramp on all eight tracks. ⚠️ Not a hard bound: after a stall the
    // drain takes everything overdue and may allocate once in the callback.
    noteBatch.reserve(64);
    killBatch.reserve(64);
    paramBatch.reserve(64);

    // Built at the fallback rate; setDeviceSampleRate rebuilds them at the device's.
    const float defaultRate = static_cast<float>(effectsSampleRate);
    reverbSend.reset(defaultRate);
    delaySend.reset(defaultRate);
    masterChain.reset(defaultRate);
}

void AudioEngine::setDeviceSampleRate(int sr) {
    if (sr <= 0) return;
    deviceSampleRate.store(sr, std::memory_order_relaxed);
    if (sr == effectsSampleRate) return;
    effectsSampleRate = sr;
    resetEffectState();     // reads the rate just stored; the buses are at factory defaults now
    prepareReverb();
    busReplayRequested.store(true, std::memory_order_release);
}

AudioEngine::~AudioEngine() {
    // ⚠️ First: a background preset load captures `this`.
    discardSoundfontLoad();

    // The backend is destroyed before the engine, so no callback runs during this.
    for (int i = 0; i < 256; i++) {
        if (samples[i])              delete[] samples[i];
        if (samplesRight[i])         delete[] samplesRight[i];
        if (sampleBackups[i])        delete[] sampleBackups[i];
        if (sampleBackupsRight[i])   delete[] sampleBackupsRight[i];
        freeRateCache(i);
    }
    delete[] sampleClipboard;
    delete[] sampleClipboardRight;
    delete[] fxPreviewBackup;
    delete[] fxPreviewBackupRight;
}


void AudioEngine::stopAll() {
    stopRampRequested.store(false, std::memory_order_relaxed);   // superseded: nothing is left to ramp
    for (int i = 0; i < MAX_VOICES; i++) {
        voices[i].stop();
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        sfVoices[t].hardStop();
    }
    resetTic00Cursors();   // PLAY starts every table at row 0
    LOGD("stopAll: voices and SF notes cleared, stream stays running");
}

void AudioEngine::stopAllRamped() {
    stopRampRequested.store(true, std::memory_order_release);
    stoppedSinceTake.store(true, std::memory_order_relaxed);
}

void AudioEngine::startTake() {
    if (stoppedSinceTake.exchange(false, std::memory_order_relaxed))
        ottRestartRequested.store(true, std::memory_order_release);
}

void AudioEngine::startStopRamp(int64_t blockStartFrame) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].isActive) voices[i].startFadeOut(KILL_FADE_SAMPLES);
        else                    voices[i].stop();   // idle slot: clear any stale fade state
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        if (sfVoices[t].isActive) sfVoices[t].startStopFade(KILL_FADE_SAMPLES);
        else                      sfVoices[t].hardStop();
    }
    resetTic00Cursors();   // as stopAll(): PLAY starts at row 0
    // When the audio thread reclaims the slots (see processAudioBlock).
    stopRampEndFrame.store(blockStartFrame + KILL_FADE_SAMPLES, std::memory_order_relaxed);
}

int AudioEngine::getActiveVoiceCount() {
    const VoiceView& view = voiceView();
    int count = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (view.sampler[i].active) count++;
    }
    return count;
}

void AudioEngine::getTrackActiveNotes(int* out, int trackCount) {
    for (int t = 0; t < trackCount; t++) out[t] = -1;
    const VoiceView& view = voiceView();
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!view.sampler[v].active) continue;
        int t = view.sampler[v].trackId;
        if (t >= 0 && t < trackCount && out[t] == -1) out[t] = view.sampler[v].note;
    }
    // Then the SoundFont pool; a sampler note wins over an SF note still releasing.
    for (int t = 0; t < SF_VOICE_COUNT && t < trackCount; t++) {
        if (out[t] == -1 && view.sf[t].active) {
            out[t] = view.sf[t].note;
        }
    }
}

int AudioEngine::getSampleRate() {
    return deviceSampleRate.load(std::memory_order_relaxed);
}

// Denormals in feedback tails cost 10-100× the CPU.
void AudioEngine::setFlushToZeroForCurrentThread() {
    thread_local bool done = false;
    if (done) return;
    done = true;
#if defined(__aarch64__)
    uint64_t fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);  // FZ bit
    asm volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__arm__)
    uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1U << 24);  // FZ bit
    asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(PT_HAS_SSE_DENORMAL_CTRL)
    // FTZ (outputs) + DAZ (inputs).
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

int64_t AudioEngine::getCurrentFrame() {
    return globalFrameCounter.load(std::memory_order_relaxed);
}

void AudioEngine::scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                               float frequency, float baseFrequency, float volume, float phraseVolume, float pan,
                               int startPointOverride, int endPointOverride, int tableId, int tableTicRate,
                               int noteOctave, int notePitch,
                               float pslInitialOffset, float pslDuration,
                               float pbnRate, float vibratoSpeed, float vibratoDepth,
                               int tableStartRow) {
    ScheduledNote note{};
    note.targetFrame        = targetFrame;
    note.sampleId           = sampleId;
    note.trackId            = trackId;
    note.frequency          = frequency;
    note.baseFrequency      = baseFrequency;
    note.volume             = volume;
    note.phraseVolume       = phraseVolume;
    note.pan                = pan;
    note.startPointOverride = startPointOverride;
    note.endPointOverride   = endPointOverride;
    note.tableId            = tableId;
    note.tableTicRate       = tableTicRate;
    note.noteOctave         = noteOctave;
    note.notePitch          = notePitch;
    note.pslInitialOffset   = pslInitialOffset;
    note.pslDuration        = pslDuration;
    note.pbnRate            = pbnRate;
    note.vibratoSpeed       = vibratoSpeed;
    note.vibratoDepth       = vibratoDepth;
    note.tableStartRow      = tableStartRow;
    noteQueue.schedule(note);
}

void AudioEngine::scheduleProgramNote(int64_t targetFrame, int trackId, int instrumentId,
                                      const songcore::NoteOnPayload& noteOn, int tempo,
                                      bool rootAudition) {
    ScheduledNote note{};
    note.targetFrame  = targetFrame;
    note.trackId      = trackId;
    note.instrumentId = instrumentId;   // >= 0 marks the note as not yet resolved
    note.noteOn       = noteOn;
    note.tempo        = tempo;
    note.rootAudition = rootAudition;
    noteQueue.schedule(note);
}

bool AudioEngine::resolveScheduledNote(ScheduledNote& note) {
    // With no INS on the way this returns the instrument and table it was given.
    int chainTableId = -1;
    const int sounding = resolveChain(note.trackId, note.instrumentId, note.noteOn.tableId,
                                      &chainTableId, &note.carry);
    if (sounding < 0) return false;                 // an empty slot, or external gear: silence
    note.instrumentId  = sounding;
    note.noteOn.tableId = chainTableId;             // the voice runs the LAST link's table

    const songcore::Program p = programView(note.instrumentId);
    const songcore::DerivedNote d =
        songcore::derive_note(note.noteOn, note.targetFrame, note.trackId, note.instrumentId, p,
                              note.tempo, deviceSampleRate.load(std::memory_order_relaxed),
                              getSampleLength(p.sampleId), note.rootAudition);
    if (!d.valid) return false;

    // The fields both note kinds carry under the same name.
    const auto common = [&](const auto& a) {
        note.sampleId         = a.sampleId;
        note.pan              = a.pan;
        note.tableId          = a.tableId;
        note.tableTicRate     = a.tableTicRate;
        note.noteOctave       = a.noteOctave;
        note.notePitch        = a.notePitch;
        note.pslInitialOffset = a.pslInitialOffset;
        note.pslDuration      = a.pslDuration;
        note.pbnRate          = a.pbnRate;
        note.vibratoSpeed     = a.vibratoSpeed;
        note.vibratoDepth     = a.vibratoDepth;
        note.tableStartRow    = a.tableStartRow;
    };
    note.isSoundfont = d.isSoundfont;
    if (d.isSoundfont) {
        const songcore::SoundfontNoteArgs& a = d.soundfont;
        common(a);
        note.sfSlot             = a.sfSlot;
        note.midiNote           = a.midiNote;
        note.midiVelocity       = a.midiVelocity;
        note.volume             = a.vol;
        note.phraseVolume       = a.phraseVol;
        note.sfBank             = a.bank;
        note.sfPreset           = a.preset;
        note.frequency          = 440.0f;
        note.baseFrequency      = 440.0f;
        note.startPointOverride = -1;
        note.detuneSemitones    = a.detuneSemitones;
    } else {
        const songcore::SamplerNoteArgs& a = d.sampler;
        common(a);
        note.frequency          = a.frequency;
        note.baseFrequency      = a.baseFrequency;
        note.volume             = a.volume;
        note.phraseVolume       = a.phraseVolume;
        note.startPointOverride = a.startPointOverride;
        note.endPointOverride   = a.endPointOverride;
    }
    return true;
}

// Audio thread. The stored `sliceMarkers` is null (a self-pointer would not survive the copy);
// it is pointed at this side's markers here.
songcore::Program AudioEngine::programView(int id) const {
    if (id < 0 || id >= songcore::PROGRAM_SLOTS) return songcore::Program{};
    songcore::Program p = programs[id].program;
    p.sliceMarkers = programs[id].markers;
    return p;
}

// Audio thread, once per block.
void AudioEngine::syncInstrumentData() {
    instrumentParams.sync();
    instrumentModSlots.sync();
    programs.sync();
    sfEnvOverrides.sync();
    eqPresets.sync();
    eqPresetHex.sync();
}

void AudioEngine::scheduleKill(int64_t targetFrame, int trackId) {
    killQueue.schedule({ targetFrame, trackId, KILL_HARD });
}

void AudioEngine::scheduleNoteOff(int64_t targetFrame, int trackId) {
    killQueue.schedule({ targetFrame, trackId, KILL_SOFT });
}

void AudioEngine::scheduleKeyRelease(int64_t targetFrame, int trackId) {
    killQueue.schedule({ targetFrame, trackId, KILL_KEY_OFF });
}

void AudioEngine::scheduleCut(int64_t targetFrame, int trackId) {
    killQueue.schedule({ targetFrame, trackId, KILL_CUT });
}

void AudioEngine::scheduleNoteOffAll(int64_t targetFrame) {
    // Every lane, the preview one included.
    for (int t = 0; t < SF_VOICE_COUNT; ++t) scheduleNoteOff(targetFrame, t);
}

void AudioEngine::clearScheduledNotes() {
    noteQueue.clear();
    killQueue.clear();
    paramUpdateQueue.clear();
}

void AudioEngine::clearScheduledNotesFrom(int64_t fromFrame, int trackId) {
    noteQueue.clearFrom(fromFrame, trackId);
    killQueue.clearFrom(fromFrame, trackId);
    paramUpdateQueue.clearFrom(fromFrame, trackId);
}

void AudioEngine::scheduleVoiceTableRow(int64_t targetFrame, int trackId, int row) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, (float)row, PARAM_UPDATE_TABLE_ROW, 0.0f });
}

void AudioEngine::scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol) {
    paramUpdateQueue.schedule({ targetFrame, trackId, (int)MOD_SRC_PHRASE_VOL, phraseVol });
}

// ── Live per-note and mixer FX: applied on the audio thread at the exact frame, live and offline ──

void AudioEngine::scheduleVoiceCc(int64_t targetFrame, int trackId, int cc, float value) {
    paramUpdateQueue.schedule({ targetFrame, trackId, cc, value, PARAM_UPDATE_VOICE_CC, 0.0f });
}

void AudioEngine::scheduleVoiceReverse(int64_t targetFrame, int trackId, bool reverse, bool restart) {  // BCK
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, reverse ? 1.0f : 0.0f,
                                PARAM_UPDATE_REVERSE, restart ? 1.0f : 0.0f });
}

void AudioEngine::scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot) {                // EQN
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, (float)slot, PARAM_UPDATE_EQ_SLOT, 0.0f });
}

void AudioEngine::scheduleMasterEqSlot(int64_t targetFrame, int slot) {                            // EQM
    paramUpdateQueue.schedule({ targetFrame, -1, 0, (float)slot, PARAM_UPDATE_MASTER_EQ, 0.0f });
}

void AudioEngine::scheduleVoiceEqBands(int64_t targetFrame, int trackId, const EqBandsHex& bands) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, 0.0f, PARAM_UPDATE_EQ_BANDS, 0.0f, bands });
}

void AudioEngine::scheduleMasterEqBands(int64_t targetFrame, const EqBandsHex& bands) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, 0.0f, PARAM_UPDATE_MASTER_EQ_BANDS, 0.0f, bands });
}

// Authored hex → Hz/dB/Q. ⚠️ setEqBand's curves too: a morph must land exactly on its end presets.
static EqBandData eqBandFromHex(int type, int freqHex, int gainHex, int qHex) {
    EqBandData b;
    b.type   = type;
    b.freqHz = 20.0f * powf(1000.0f, freqHex / 255.0f);
    b.gainDb = gainHex / 10.0f - 12.0f;
    b.q      = 0.1f  * powf(100.0f,  qHex   / 255.0f);
    return b;
}

void AudioEngine::applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands) {
    bool any = false;
    for (int i = 0; i < 3; i++) {
        EqBandData d = eqBandFromHex(bands.type[i], bands.freq[i], bands.gain[i], bands.q[i]);
        eq.bands[i].setParams(d.type, d.freqHz, d.gainDb, d.q);
        if (d.type != 0) any = true;
    }
    eq.active = any;
}

// The module was initialised by its owner; only the band params are set. `active` means some band
// is on, so an all-off preset costs nothing per sample.
void AudioEngine::applyEqPresetToModule(EqModule& eq, int slot) {
    if (slot < 0 || slot >= 128) { eq.active = false; return; }
    const EqPresetBank& preset = eqPresets[slot];
    bool any = false;
    for (int i = 0; i < 3; i++) {
        eq.bands[i].setParams(preset.bands[i].type, preset.bands[i].freqHz,
                              preset.bands[i].gainDb, preset.bands[i].q);
        if (preset.bands[i].type != 0) any = true;
    }
    eq.active = any;
}

static const int SPECTRUM_FFT_SIZE = 2048;

// Hann window + FFT → numBins log-spaced magnitudes in [0,1].
// ⚠️ The window, bin map and FFT config are unlocked statics: the UI poll thread is the only caller.

static const float* spectrum_hann_window() {
    static float w[SPECTRUM_FFT_SIZE];
    static const bool built = [] {
        for (int i = 0; i < SPECTRUM_FFT_SIZE; i++)
            w[i] = 0.5f * (1.0f - cosf(static_cast<float>(2.0f * M_PI * i / (SPECTRUM_FFT_SIZE - 1))));
        return true;
    }();
    (void)built;
    return w;
}

// Which FFT bin each output bin reads. Four slots, since the EQ panel and the strip ask different counts.
static const int* spectrum_bin_map(int numBins, float sampleRate) {
    struct Entry {
        int              numBins    = 0;
        float            sampleRate = 0.0f;
        std::vector<int> idx;
    };
    static Entry cache[4];
    static int   next = 0;

    for (Entry& e : cache)
        if (e.numBins == numBins && e.sampleRate == sampleRate) return e.idx.data();

    Entry& e = cache[next];
    next = (next + 1) % 4;
    e.numBins    = numBins;
    e.sampleRate = sampleRate;
    e.idx.resize((size_t)numBins);

    const float fMin = 20.0f, fMax = 20000.0f;
    const float logRange = logf(fMax / fMin);
    const float denom    = (numBins > 1) ? (float)(numBins - 1) : 1.0f;
    for (int bi = 0; bi < numBins; bi++) {
        float t    = (float)bi / denom;
        float freq = fMin * expf(t * logRange);
        int bin    = (int)(freq * SPECTRUM_FFT_SIZE / sampleRate + 0.5f);
        if (bin < 1)                      bin = 1;
        if (bin >= SPECTRUM_FFT_SIZE / 2) bin = SPECTRUM_FFT_SIZE / 2 - 1;
        e.idx[(size_t)bi] = bin;
    }
    return e.idx.data();
}

static void computeSpectrumFFT(kiss_fft_scalar* input, int numBins, float* out, float sampleRate) {
    const float* window = spectrum_hann_window();
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) input[i] *= window[i];

    // Allocated once, never freed.
    static kiss_fftr_cfg cfg = kiss_fftr_alloc(SPECTRUM_FFT_SIZE, 0, nullptr, nullptr);
    kiss_fft_cpx cpx_out[SPECTRUM_FFT_SIZE / 2 + 1];
    kiss_fftr(cfg, input, cpx_out);

    const int* binOf = spectrum_bin_map(numBins, sampleRate);
    for (int bi = 0; bi < numBins; bi++) {
        const int bin = binOf[bi];

        float re  = cpx_out[bin].r;
        float im  = cpx_out[bin].i;
        float mag = sqrtf(re*re + im*im) / (SPECTRUM_FFT_SIZE * 0.5f);

        float db         = 20.0f * log10f(mag + 1e-9f);
        float normalized = (db + 80.0f) / 80.0f;
        out[bi] = fmaxf(0.0f, fminf(1.0f, normalized));
    }
}

static void readCircularBuffer(const float* buf, int writeIdx, int bufSize, kiss_fft_scalar* input) {
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        int idx = (writeIdx - SPECTRUM_FFT_SIZE + i + bufSize) % bufSize;
        input[i] = buf[idx];
    }
}

void AudioEngine::getSpectrumMagnitudes(int numBins, float* out) {
    getSpectrumMagnitudesForSource(0, -1, numBins, out);
}

void AudioEngine::getSpectrumMagnitudesForSource(int source, int instrId, int numBins, float* out) {
    lastSpectrumReadMs.store(nowMs(), std::memory_order_relaxed);  // demand signal for the capture gate
    if (source == 3) instrSpectrumInstrId.store(instrId, std::memory_order_relaxed);

    kiss_fft_scalar input[SPECTRUM_FFT_SIZE];
    {
        std::lock_guard<std::mutex> lock(spectrumMutex);
        switch (source) {
            case 1:  readCircularBuffer(delaySpectrumBuffer,  delaySpectrumWriteIdx,  SPECTRUM_SIZE, input); break;
            case 2:  readCircularBuffer(reverbSpectrumBuffer, reverbSpectrumWriteIdx, SPECTRUM_SIZE, input); break;
            case 3:  readCircularBuffer(instrSpectrumBuffer,  instrSpectrumWriteIdx,  SPECTRUM_SIZE, input); break;
            default: readCircularBuffer(spectrumBuffer,       spectrumWriteIdx,       SPECTRUM_SIZE, input); break;
        }
    }
    computeSpectrumFFT(input, numBins, out, (float)getSampleRate());
}

void AudioEngine::getWaveform(float* outBuffer, int bufferSize) {
    std::lock_guard<std::mutex> lock(waveformMutex);
    for (int i = 0; i < bufferSize && i < WAVEFORM_SIZE; i++) {
        int readIndex = (waveformIndex + i) % WAVEFORM_SIZE;
        outBuffer[i] = waveformBuffer[readIndex];
    }
}

void AudioEngine::getTrackPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    for (int i = 0; i < 8; i++) {
        outBuffer[i * 2]     = trackPeaksL[i];
        outBuffer[i * 2 + 1] = trackPeaksR[i];
    }
}

void AudioEngine::getMasterPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    outBuffer[0] = masterPeakL;
    outBuffer[1] = masterPeakR;
}

void AudioEngine::getSendPeaks(float* outBuffer) {
    std::lock_guard<std::mutex> lock(peakMutex);
    outBuffer[0] = sendPeakRevL;
    outBuffer[1] = sendPeakRevR;
    outBuffer[2] = sendPeakDelL;
    outBuffer[3] = sendPeakDelR;
}

void AudioEngine::decayPeaks() {
    std::lock_guard<std::mutex> lock(peakMutex);
    const auto fall = [](float& p) { p *= 0.92f; if (p < 0.001f) p = 0.0f; };
    for (int t = 0; t < 8; t++) { fall(trackPeaksL[t]); fall(trackPeaksR[t]); }
    for (float* p : { &masterPeakL, &masterPeakR, &sendPeakRevL, &sendPeakRevR, &sendPeakDelL, &sendPeakDelR })
        fall(*p);
}

void AudioEngine::decayWaveform() {
    std::lock_guard<std::mutex> lock(waveformMutex);
    const float WAVEFORM_DECAY = 0.90f;

    for (int i = 0; i < WAVEFORM_SIZE; i++) {
        waveformBuffer[i] *= WAVEFORM_DECAY;
        if (fabsf(waveformBuffer[i]) < 0.001f) waveformBuffer[i] = 0.0f;
    }
    for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            trackWaveformBuffer[t][i] *= WAVEFORM_DECAY;
            if (fabsf(trackWaveformBuffer[t][i]) < 0.001f) trackWaveformBuffer[t][i] = 0.0f;
        }
    }
}

void AudioEngine::getTrackWaveforms(float* outBuffer, bool* activeFlags) {
    lastTrackWaveformReadMs.store(nowMs(), std::memory_order_relaxed);  // demand signal for the OCTA gate
    std::lock_guard<std::mutex> lock(waveformMutex);
    for (int t = 0; t < TRACK_WAVEFORM_COUNT; t++) {
        activeFlags[t] = trackHasVoice[t];
        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            int readIdx = (trackWaveformIndex + i) % WAVEFORM_SIZE;
            outBuffer[t * WAVEFORM_SIZE + i] = trackWaveformBuffer[t][readIdx];
        }
    }
}

// The apply* forms carry no log line, so VTR/VMV can call them on the audio thread.
void AudioEngine::applyTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    trackVolumes[trackId].store(volume, std::memory_order_relaxed);
}

void AudioEngine::applyMasterVolume(float volume) {
    masterVolume.store(volume, std::memory_order_relaxed);
}

void AudioEngine::setTrackVolume(int trackId, float volume) {
    if (trackId < 0 || trackId >= 8) return;
    applyTrackVolume(trackId, volume);
    LOGD("🔊 Track %d volume set to %.2f", trackId, volume);
}

void AudioEngine::setTrackMuted(int trackId, bool muted) {
    if (trackId < 0 || trackId >= 8) return;
    trackMuted[trackId].store(muted, std::memory_order_relaxed);
    // The mix ramps the gate over MUTE_GATE_SAMPLES; the voices keep running underneath.
    LOGD("🔇 Track %d %s", trackId, muted ? "muted" : "unmuted");
}

void AudioEngine::setBusMutes(bool revMuted, bool dlyMuted, bool dryBusMuted) {
    revReturnMuted.store(revMuted, std::memory_order_relaxed);
    delayReturnMuted.store(dlyMuted, std::memory_order_relaxed);
    dryMuted.store(dryBusMuted, std::memory_order_relaxed);
}

void AudioEngine::setMasterVolume(float volume) {
    applyMasterVolume(volume);
    LOGD("🔊 Master volume set to %.2f", volume);
}

void AudioEngine::setPreviewTrack(int trackId) {
    previewLaneTrack.store((trackId >= 0 && trackId < 8) ? trackId : -1, std::memory_order_relaxed);
}

// VTR / VMV. A global update carries trackId -1.
void AudioEngine::scheduleTrackVolume(int64_t targetFrame, int trackId, float volume) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, volume, PARAM_UPDATE_TRACK_VOL, 0.0f });
}

void AudioEngine::scheduleMasterVolume(int64_t targetFrame, float volume) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, volume, PARAM_UPDATE_MASTER_VOL, 0.0f });
}

void AudioEngine::scheduleDelayTime(int64_t targetFrame, float time) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, time, PARAM_UPDATE_DELAY_TIME, 0.0f });
}

void AudioEngine::refreshSoundingInstrument(int instrumentId) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    // Named fields: `instrId` sits past `eqBands`, beyond any positional initialiser.
    ScheduledParamUpdate update{};
    update.targetFrame = globalFrameCounter.load(std::memory_order_relaxed);
    update.trackId     = -1;
    update.action      = PARAM_UPDATE_INSTRUMENT;
    update.instrId     = instrumentId;
    paramUpdateQueue.schedule(update);
}

void AudioEngine::setOttDepth(int depth) {
    busSettings.ottDepth = depth; busSettings.ottForRender = false;
    recordBus(BUS_OTT);
}

void AudioEngine::setOttDepthForRender(int depth) {
    busSettings.ottDepth = depth; busSettings.ottForRender = true;
    recordBus(BUS_OTT);
}

void AudioEngine::setMasterFx(int fx) {
    busSettings.masterFx = fx;
    recordBus(BUS_MASTER_FX);
}

void AudioEngine::setDustDepth(int depth) {
    busSettings.dustDepth = depth; busSettings.dustForRender = false;
    recordBus(BUS_DUST);
}

void AudioEngine::setDustDepthForRender(int depth) {
    busSettings.dustDepth = depth; busSettings.dustForRender = true;
    recordBus(BUS_DUST);
}

void AudioEngine::setLimiterPreGain(int depth) {
    busSettings.limiterPreGain = depth;
    recordBus(BUS_LIMITER);
}

IAudioVoice* AudioEngine::findActiveVoiceForTrack(int trackId) {
    // The current note, for mid-note updates. A releasing SF or fading sampler voice is the previous
    // note's tail — and a decayed SF note stays isActive — so neither is a target.
    if (trackId >= 0 && trackId < SF_VOICE_COUNT &&
        sfVoices[trackId].isActive && !sfVoices[trackId].isReleasingOnly) {
        return &sfVoices[trackId];
    }
    for (int v = 0; v < MAX_VOICES; v++) {
        if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == trackId) {
            return &voices[v];
        }
    }
    return nullptr;
}

void AudioEngine::schedulePitchBend(int64_t targetFrame, int trackId, float semitonesPerStep, int tempo) {
    // Per step → per frame. 0 = stop.
    float ratePerFrame = 0.0f;
    if (fabsf(semitonesPerStep) >= 0.0001f) {
        float sr = (float)getSampleRate();
        float framesPerStep = sr / (tempo / 60.0f * 4.0f * 12.0f) * 12.0f;
        ratePerFrame = semitonesPerStep / framesPerStep;
    }
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, ratePerFrame, PARAM_UPDATE_PITCH_BEND, 0.0f });
}

void AudioEngine::scheduleVibrato(int64_t targetFrame, int trackId, float speed, float depth) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, speed, PARAM_UPDATE_VIBRATO, depth });
}

void AudioEngine::setInstrumentModulation(int sampleId, int slotIndex,
                                          int type, int dest, float amount,
                                          int attackSamples, int holdSamples, int decaySamples,
                                          float sustainLevel, float lfoHz, int oscShape,
                                          int releaseSamples, int lfoTrigMode) {
    if (sampleId < 0 || sampleId >= 256 || slotIndex < 0 || slotIndex >= 4) return;
    InstrumentModSlot& slot = instrumentModSlots.edit(sampleId)[slotIndex];
    slot.type = type;
    slot.dest = dest;
    slot.amount = amount;
    slot.attackSamples = attackSamples;
    slot.holdSamples = holdSamples;
    slot.decaySamples = decaySamples;
    slot.sustainLevel = sustainLevel;
    slot.lfoHz = lfoHz;
    slot.oscShape = oscShape;
    slot.lfoTrigMode = lfoTrigMode;
    slot.releaseSamples = releaseSamples;
    instrumentModSlots.publish(sampleId);
}

void AudioEngine::initVoiceModSlots(IAudioVoice& voice, int sampleId, int64_t currentFrame, float sampleRate) {
    for (int m = 0; m < 4; m++) {
        const InstrumentModSlot& src = instrumentModSlots[sampleId][m];
        VoiceModSlot& dst = voice.voiceMods[m];
        dst.type = src.type;
        dst.dest = src.dest;
        dst.amount = src.amount;
        dst.attackSamples = src.attackSamples;
        dst.holdSamples = src.holdSamples;
        dst.decaySamples = src.decaySamples;
        dst.sustainLevel = src.sustainLevel;
        dst.lfoHz = src.lfoHz;
        dst.oscShape = src.oscShape;
        dst.lfoTrigMode = src.lfoTrigMode;
        dst.releaseSamples = src.releaseSamples;
        dst.effectiveAmt = src.amount;
        dst.effectiveRateMult = 1.0f;
        dst.prevEnvValue = 0.0f;
        dst.stage = (src.type != 0) ? 1 : 0;
        dst.envValue = 0.0f;
        dst.stageCounter = 0;

        // RND/DRNK seed: per note, per slot, per render.
        dst.lfoRngState  = (((uint32_t)(uint64_t)currentFrame) * 747796405u
                            ^ (uint32_t)(m + 1) * 2891336453u
                            ^ noteSeedEntropy) | 1u;
        dst.lfoRandValue = (src.type == 3 && src.oscShape >= 8)
                           ? xorshift32Bipolar(dst.lfoRngState) : 0.0f;

        // RETG/ONCE restart the cycle at note-on; FREE and HOLD follow the global clock, and HOLD
        // freezes that value for the note.
        dst.lfoPhase = 0.0f;
        if (src.type == 3 && (src.lfoTrigMode == 0 || src.lfoTrigMode == 2)) {
            double cycles = (double)currentFrame * (double)src.lfoHz / (double)sampleRate;
            dst.lfoPhase = (float)(fmod(cycles, 1.0) * 2.0 * M_PI);
            if (src.lfoTrigMode == 2) {
                dst.envValue = (src.oscShape >= 8) ? dst.lfoRandValue
                                                   : lfoShape(dst.lfoPhase, src.oscShape);
            }
        }
    }
}

void AudioEngine::clearInstrumentModulation(int sampleId) {
    if (sampleId < 0 || sampleId >= 256) return;
    for (int m = 0; m < 4; m++) {
        instrumentModSlots.edit(sampleId)[m] = InstrumentModSlot();
    }
    instrumentModSlots.publish(sampleId);
}

void AudioEngine::updateVoiceModulation(IAudioVoice& voice, int numFrames, float sampleRate) {
    runModMatrix(voice, numFrames, sampleRate);
}

void AudioEngine::updateVoicePitchMod(Voice& voice, int numFrames, float sampleRate) {
    tickPitchSlide(voice, numFrames);
    tickVibrato(voice, numFrames, sampleRate);
}

float AudioEngine::getModulatedPlaybackRate(Voice& voice) {
    // Table pitch, slide, vibrato and mod slots, plus FIN in params.base — added, not replaced.
    float rateMod = powf(2.0f,
                         (voice.modDestValues[PARAM_PITCH] + voice.params.base[PARAM_PITCH]) / 12.0f);
    const float rate = voice.playbackRate * rateMod;

    // OSCILLATOR loop mode: one trip round the loop is one cycle of the played note, so every pitch
    // source still applies. The loop's length is timbre here, not pitch — LPO is how it is played.
    if (voice.loopMode == LOOP_MODE_OSCILLATOR && voice.baseFrequency > 0.0f) {
        const int loopLength = voice.actualLoopEnd - voice.actualLoopStart;
        const float sr = (float)getSampleRate();
        if (loopLength > 0 && sr > 0.0f) return rate * ((float)loopLength * voice.baseFrequency / sr);
    }
    return rate;
}

void AudioEngine::renderOffline(int numFrames, float* output, int sampleRate) {
    setFlushToZeroForCurrentThread();
    for (int i = 0; i < numFrames * 2; i++) output[i] = 0.0f;

    // The live path's granularity, so the export matches what was heard.
    int rendered = 0;
    while (rendered < numFrames) {
        int chunk = std::min(PROCESS_SUBBLOCK, numFrames - rendered);
        processAudioBlock(output + rendered * 2, chunk, 2, (float)sampleRate);
        rendered += chunk;
    }
}

void AudioEngine::resetFrameCounter() {
    globalFrameCounter.store(0, std::memory_order_relaxed);
    noteSeedEntropy = ((uint32_t)nowMs() * 2654435761u) | 1u;
}

void AudioEngine::resetEffectState() {
    const float sr = (float)getSampleRate();
    reverbSend.reset(sr);   // also reseeds the reverb's LCG
    delaySend.reset(sr);
    masterChain.reset(sr);
    // Not replayed here: a render must depend on the project alone.
    LOGD("🎬 Effect chains reset to clean state");
}

// Control thread, before the record is published, so the block applying ALGO or SIZE finds it ready.
void AudioEngine::prepareReverb() {
    const BusSettings& s = busSettings;
    reverbSend.dragonfly.prepare(s.reverbAlgo, s.reverbDecay, s.reverbSize, s.reverbDamp, s.reverbMod,
                                 static_cast<float>(getSampleRate()));
}

// Audio thread, top of the block: every group that changed, or all after a rate change.
void AudioEngine::applyBusSettings() {
    busPublisher.read(busLive, busSeen);
    const bool all = busReplayRequested.exchange(false, std::memory_order_acquire) && busLive.pushed;
    const BusSettings& s = busLive;
    const auto due = [&](BusGroup g) {
        if (!all && s.seq[g] == busApplied[g]) return false;
        busApplied[g] = s.seq[g];
        return true;
    };
    if (due(BUS_REVERB_PARAMS)) reverbSend.setParams(s.reverbDecay, s.reverbDamp, s.reverbSize);
    if (due(BUS_REVERB_ALGO))   reverbSend.setAlgo(s.reverbAlgo);
    if (due(BUS_REVERB_CHAR))   reverbSend.setCharacter(s.reverbPre, s.reverbWidth, s.reverbMod);
    if (due(BUS_REVERB_INEQ))   applyEqPresetToModule(reverbSend.inputEq, s.reverbInputEq);
    if (due(BUS_DELAY_TIME)) {
        if (s.delaySync) delaySend.setTimeSync(s.delayTime, s.delayBpm);
        else             delaySend.setTimeFree(s.delayTime);
    }
    if (due(BUS_DELAY_FEEDBACK)) delaySend.feedback = s.delayFeedback / 255.0f;
    if (due(BUS_DELAY_CHAR))     delaySend.setCharacter(s.delayPong, s.delayTone, s.delayWobble);
    if (due(BUS_DELAY_INEQ))     applyEqPresetToModule(delaySend.inputEq, s.delayInputEq);
    if (due(BUS_MASTER_EQ))      applyEqPresetToModule(masterChain.masterEq, s.masterEqSlot);
    if (due(BUS_OTT)) {
        if (s.ottForRender) masterChain.ott.resetForRender(s.ottDepth / 255.0f);
        else                masterChain.ott.setDepth(s.ottDepth / 255.0f);
    }
    if (due(BUS_MASTER_FX)) masterChain.setMasterFx(s.masterFx);
    if (due(BUS_DUST)) {
        if (s.dustForRender) masterChain.setDustDepthForRender(s.dustDepth / 255.0f);
        else                 masterChain.setDustDepth(s.dustDepth / 255.0f);
    }
    if (due(BUS_LIMITER)) masterChain.setLimiterPreGain(1.0f + (s.limiterPreGain / 255.0f) * 3.0f);
}

void AudioEngine::setOfflineRendering(bool offline) {
    isOfflineRendering.store(offline);
    LOGD("🎬 Offline rendering: %s", offline ? "ON" : "OFF");
}

void AudioEngine::setTempo(int tempo) {
    // The table advance divides by it.
    currentTempo.store(std::max(1, tempo), std::memory_order_relaxed);
}

// ─── The metronome ───────────────────────────────────────────────────────────────────────────────

void AudioEngine::setMetronome(bool enabled, float gain) {
    metronomeOn.store(enabled, std::memory_order_relaxed);
    metronomeGain.store(std::max(0.0f, std::min(1.0f, gain)), std::memory_order_relaxed);
}

void AudioEngine::startMetronome(int64_t startFrame, int64_t framesPerBeat) {
    metronomeBeatFrames.store(framesPerBeat > 0 ? framesPerBeat : 0, std::memory_order_relaxed);
    metronomeEpoch.store(startFrame, std::memory_order_relaxed);
}

void AudioEngine::setMetronomeBeat(int64_t framesPerBeat) {
    if (framesPerBeat > 0) metronomeBeatFrames.store(framesPerBeat, std::memory_order_relaxed);
}

void AudioEngine::stopMetronome() {
    metronomeEpoch.store(-1, std::memory_order_relaxed);
}

void AudioEngine::renderMetronome(float* output, int numFrames, int channelCount, float sampleRate,
                                  int64_t blockStartFrame, bool offlineRender) {
    // No click in an export; drop the one in flight too.
    if (offlineRender) { metroClickPos_ = -1; return; }

    const int64_t epoch = metronomeEpoch.load(std::memory_order_relaxed);
    const int64_t beat  = metronomeBeatFrames.load(std::memory_order_relaxed);

    if (epoch != metroEpoch_) {
        // A new take: beat 0, the downbeat, is the transport's start.
        metroEpoch_      = epoch;
        metroBeatFrames_ = beat;
        metroIndex_      = 0;
        metroCount_      = 0;
        metroClickPos_   = -1;
    } else if (beat > 0 && beat != metroBeatFrames_) {
        // Tempo changed mid-take: rebase on the next beat, so it keeps its place and later ones
        // take the new spacing (as songcore::MidiClock::rebase).
        metroEpoch_      = metroEpoch_ + metroIndex_ * metroBeatFrames_;
        metroIndex_      = 0;
        metroBeatFrames_ = beat;
    }

    if (metroEpoch_ < 0 || metroBeatFrames_ <= 0) return;   // nothing is playing

    // The grid walks while silent, so switching on mid-take waits for the next beat.
    const float gain = metronomeOn.load(std::memory_order_relaxed)
                     ? metronomeGain.load(std::memory_order_relaxed) : 0.0f;
    if (gain <= 0.0f) {
        metroClickPos_ = -1;
        const int64_t ahead = blockStartFrame + numFrames - (metroEpoch_ + metroIndex_ * metroBeatFrames_);
        if (ahead > 0) {
            const int64_t passed = (ahead + metroBeatFrames_ - 1) / metroBeatFrames_;
            metroIndex_ += passed;
            metroCount_ += passed;
        }
        return;
    }

    // ⚠️ After a device stall the frame counter jumps: snap the grid forward instead of clicking once
    // per block to catch up. The count keeps every skipped beat, so the accent stays on the bar.
    const int64_t behind = blockStartFrame - (metroEpoch_ + metroIndex_ * metroBeatFrames_);
    if (behind > metroBeatFrames_) {
        const int64_t skipped = behind / metroBeatFrames_;
        metroIndex_ += skipped;
        metroCount_ += skipped;
    }

    const int clickFrames = std::max(1, static_cast<int>(sampleRate * METRONOME_CLICK_SEC));

    for (int i = 0; i < numFrames; i++) {
        // A beat always restarts the click — at high tempos a beat is shorter than the click.
        if (blockStartFrame + i >= metroEpoch_ + metroIndex_ * metroBeatFrames_) {
            const bool accent = (metroCount_ % METRONOME_BEATS_PER_BAR) == 0;
            metroClickPhase_  = 0.0f;
            metroClickStep_   = 2.0f * static_cast<float>(M_PI) *
                                (accent ? METRONOME_ACCENT_HZ : METRONOME_BEAT_HZ) / sampleRate;
            metroClickPos_    = 0;
            metroIndex_++;
            metroCount_++;
        }
        if (metroClickPos_ < 0) continue;

        // A cubic decay that reaches exactly zero, on a sine from phase 0: no step at either end.
        const float t   = static_cast<float>(metroClickPos_) / static_cast<float>(clickFrames);
        const float env = (1.0f - t) * (1.0f - t) * (1.0f - t);
        const float s   = sinf(metroClickPhase_) * env * gain;
        metroClickPhase_ += metroClickStep_;
        if (++metroClickPos_ >= clickFrames) metroClickPos_ = -1;

        // Clamped: this is summed after the limiter.
        for (int ch = 0; ch < channelCount; ch++) {
            float& out = output[i * channelCount + ch];
            out = fmaxf(-1.0f, fminf(1.0f, out + s));
        }
    }
}
