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
// MSVC defines neither __SSE2__ nor __x86_64__ — it signals x86/x64 with _M_X64 / _M_IX86 — so the
// host build would silently lose denormal protection without these arms. SSE2 is baseline on x64.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86) && _M_IX86_FP >= 2)
#define PT_HAS_SSE_DENORMAL_CTRL 1
#include <pmmintrin.h>  // FTZ/DAZ MXCSR macros for setFlushToZeroForCurrentThread
#endif

// Definition of the per-track soundfont voice array (extern declared in audio-engine.h):
// song tracks 0-7 + the preview lane (track 8).
SoundfontVoice sfVoices[SF_VOICE_COUNT];

AudioEngine::AudioEngine() {
    for (int i = 0; i < 256; i++) {
        samples[i] = nullptr;
        samplesRight[i] = nullptr;
        sampleLengths[i] = 0;
        sampleGen[i] = 0;
        sampleBackups[i] = nullptr;
        sampleBackupsRight[i] = nullptr;
        sampleBackupLengths[i] = 0;
        originalSamples[i] = nullptr;
        originalSamplesRight[i] = nullptr;
        originalSamplesF[i] = nullptr;
        originalSamplesRightF[i] = nullptr;
        originalSampleLengths[i] = 0;
        sampleBitDepth[i] = 16;
        sampleIsFloat[i]  = false;
    }
    resetTic00Cursors();
    globalFrameCounter.store(0, std::memory_order_relaxed);
    noteSeedEntropy = ((uint32_t)nowMs() * 2654435761u) | 1u;  // vary RND/DRNK per app session

    // Pre-size the per-block drain buffers: a block holds a handful of events, and 64 covers a dense
    // AUS/AUF ramp across all eight tracks. ⚠️ A typical bound, not a hard one — `drainUntil` also
    // takes anything overdue, so a stall can cross it and cost one `operator new` in the callback, once.
    noteBatch.reserve(64);
    killBatch.reserve(64);
    paramBatch.reserve(64);

    for (int i = 0; i < WAVEFORM_SIZE; i++) {
        waveformBuffer[i] = 0.0f;
    }
    waveformIndex = 0;
    waveformDownsampleCounter = 0;
    for (int i = 0; i < SPECTRUM_SIZE; i++) {
        spectrumBuffer[i] = 0.0f;
        delaySpectrumBuffer[i] = 0.0f;
        reverbSpectrumBuffer[i] = 0.0f;
        instrSpectrumBuffer[i] = 0.0f;
    }
    spectrumWriteIdx = delaySpectrumWriteIdx = reverbSpectrumWriteIdx = instrSpectrumWriteIdx = 0;
    // The buses need valid coefficients before any audio, and there is no device yet to ask — so they
    // are built at the fallback rate and REBUILT by setDeviceSampleRate the moment the shell learns
    // the real one. `effectsSampleRate` records which rate this was, so that call knows what is owed.
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
    resetEffectState();     // reads getSampleRate(), i.e. the value just stored
    // The buses are at factory defaults now; the next block puts the song's own values back.
    prepareReverb();   // …and the reverb's engine, rebuilt at the new rate, is waiting for it
    busReplayRequested.store(true, std::memory_order_release);
}

AudioEngine::~AudioEngine() {
    // ⚠️ FIRST. A background preset load holds a `this` capture, so nothing below may free anything
    // while it is still running. It is at most one preset's decode.
    discardSoundfontLoad();

    // The platform backend owns and closes the output stream; the core frees its buffers. The owner
    // destroys the backend first, so no callback can run during this teardown.
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
    // Stop all soundfont notes on all tracks (incl. the preview lane)
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        sfVoices[t].hardStop();
    }
    // Transport stop rewinds every TIC00 table, router or sounding: PLAY starts at row 0.
    resetTic00Cursors();
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
    // The deadline the audio thread reclaims the slots at — see processAudioBlock, which is also
    // where it says why a fade counter alone does not get there.
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
    // ⚠️ THE SOUNDFONT POOL IS A SECOND VOICE POOL, AND THE MONITOR READS BOTH (`voices[]` holds
    // samplers only). Sampler first: a sampler note supersedes an SF note still releasing on the track.
    for (int t = 0; t < SF_VOICE_COUNT && t < trackCount; t++) {
        if (out[t] == -1 && view.sf[t].active) {
            out[t] = view.sf[t].note;
        }
    }
}

int AudioEngine::getSampleRate() {
    // Cached from the backend at stream-open (setDeviceSampleRate); 44100 until then, matching
    // every other fallback in the engine.
    return deviceSampleRate.load(std::memory_order_relaxed);
}

// Flush-to-Zero eliminates denormal CPU stalls (10-100x slowdowns in reverb/delay/EQ feedback
// tails). See the declaration in audio-engine.h for why it is a member rather than a file-static:
// the offline sample-editor paths run the same DSP on the UI thread and must arm it too.
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
    // x86/x86_64 (emulators, desktop hosts): FTZ (outputs) + DAZ (inputs) via MXCSR; SSE3+ is given.
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
    note.instrumentId = instrumentId;   // >= 0 is what marks this note as still undecided
    note.noteOn       = noteOn;
    note.tempo        = tempo;
    note.rootAudition = rootAudition;
    noteQueue.schedule(note);
}

bool AudioEngine::resolveScheduledNote(ScheduledNote& note) {
    // Follow any INS cells the hit passes through. With no INS anywhere this returns the instrument
    // it was given and the table it would have used, so the ordinary note path is unchanged.
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

    note.isSoundfont = d.isSoundfont;
    if (d.isSoundfont) {
        const songcore::SoundfontNoteArgs& a = d.soundfont;
        note.sfSlot             = a.sfSlot;
        note.midiNote           = a.midiNote;
        note.midiVelocity       = a.midiVelocity;
        note.volume             = a.vol;
        note.phraseVolume       = a.phraseVol;
        note.pan                = a.pan;
        note.sfBank             = a.bank;
        note.sfPreset           = a.preset;
        note.sampleId           = a.sampleId;
        note.frequency          = 440.0f;
        note.baseFrequency      = 440.0f;
        note.startPointOverride = -1;
        note.tableId            = a.tableId;
        note.tableTicRate       = a.tableTicRate;
        note.noteOctave         = a.noteOctave;
        note.notePitch          = a.notePitch;
        note.pslInitialOffset   = a.pslInitialOffset;
        note.pslDuration        = a.pslDuration;
        note.pbnRate            = a.pbnRate;
        note.vibratoSpeed       = a.vibratoSpeed;
        note.vibratoDepth       = a.vibratoDepth;
        note.tableStartRow      = a.tableStartRow;
        note.detuneSemitones    = a.detuneSemitones;
    } else {
        const songcore::SamplerNoteArgs& a = d.sampler;
        note.sampleId           = a.sampleId;
        note.frequency          = a.frequency;
        note.baseFrequency      = a.baseFrequency;
        note.volume             = a.volume;
        note.phraseVolume       = a.phraseVolume;
        note.pan                = a.pan;
        note.startPointOverride = a.startPointOverride;
        note.endPointOverride   = a.endPointOverride;
        note.tableId            = a.tableId;
        note.tableTicRate       = a.tableTicRate;
        note.noteOctave         = a.noteOctave;
        note.notePitch          = a.notePitch;
        note.pslInitialOffset   = a.pslInitialOffset;
        note.pslDuration        = a.pslDuration;
        note.pbnRate            = a.pbnRate;
        note.vibratoSpeed       = a.vibratoSpeed;
        note.vibratoDepth       = a.vibratoDepth;
        note.tableStartRow      = a.tableStartRow;
    }
    return true;
}

// Audio thread. The stored row's `sliceMarkers` is null — a self-pointer would not survive the
// copy between the two sides — so it is re-pointed here, at this thread's own copy of the markers.
songcore::Program AudioEngine::programView(int id) const {
    if (id < 0 || id >= songcore::PROGRAM_SLOTS) return songcore::Program{};
    songcore::Program p = programs[id].program;
    p.sliceMarkers = programs[id].markers;
    return p;
}

// Audio thread, once per block: take in every per-instrument record the control thread published.
void AudioEngine::syncInstrumentData() {
    instrumentParams.sync();
    instrumentModSlots.sync();
    programs.sync();
    sfEnvOverrides.sync();
    eqPresets.sync();
    eqPresetHex.sync();
}

void AudioEngine::scheduleKill(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleNoteOff(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    kill.mode        = KILL_SOFT;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleKeyRelease(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    kill.mode        = KILL_KEY_OFF;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleCut(int64_t targetFrame, int trackId) {
    ScheduledKill kill{};
    kill.targetFrame = targetFrame;
    kill.trackId     = trackId;
    kill.mode        = KILL_CUT;
    killQueue.schedule(kill);
}

void AudioEngine::scheduleNoteOffAll(int64_t targetFrame) {
    // Every lane, the preview one included — a render owns the whole engine, and a lane with nothing
    // sounding on it costs one queue entry that finds no voice.
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
    // Enqueue; the audio thread applies it to voices[] in the drain loop (see processAudioBlock).
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, (float)row, PARAM_UPDATE_TABLE_ROW, 0.0f });
}

void AudioEngine::scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol) {
    paramUpdateQueue.schedule({ targetFrame, trackId, (int)MOD_SRC_PHRASE_VOL, phraseVol });
}

// ── Live per-note / mixer FX — all onto the sample-accurate paramUpdateQueue, so the mutation happens
// on the audio thread at the exact step frame and replays identically in an offline render. ──────

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

// Convert one band from authored hex to the Hz/dB/Q the filters run on.
// ⚠️ The three curves are setEqBand's and must have no second copy: a morph converting even slightly
// differently would miss the preset it names at the ends of a fade. Change both together.
static EqBandData eqBandFromHex(int type, int freqHex, int gainHex, int qHex) {
    EqBandData b;
    b.type   = type;
    b.freqHz = 20.0f * powf(1000.0f, freqHex / 255.0f);
    b.gainDb = gainHex / 10.0f - 12.0f;
    b.q      = 0.1f  * powf(100.0f,  qHex   / 255.0f);
    return b;
}

// Apply a morph tick's bands to an EQ (a live voice's inline EQ, or the master chain's). Mirrors
// applyEqPresetToModule, but the bands arrive as values rather than as a slot to look up.
void AudioEngine::applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands) {
    bool any = false;
    for (int i = 0; i < 3; i++) {
        EqBandData d = eqBandFromHex(bands.type[i], bands.freq[i], bands.gain[i], bands.q[i]);
        eq.bands[i].setParams(d.type, d.freqHz, d.gainDb, d.q);
        if (d.type != 0) any = true;
    }
    eq.active = any;
}

// Apply a global EQ preset (slot 0-127, <0 = bypass) to any EQ: a live voice's inline EQ (EQN), the
// master bus (EQM), or either send's input EQ. The target was already sp_pareq_init'd by its owner's
// reset, so only the band params are re-set here.
//
// `active` follows "some band is not type 0" rather than "the slot index is in range". The two are
// audibly identical — a type-0 band sets its own `bypass` and returns the input untouched
// (eq-module.h) — but the derived form skips three bypassed bands per sample on an all-off preset.
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

// Shared FFT helper — takes FFT_SIZE samples already copied from the circular buffer by the caller
// (under mutex), applies Hann window + FFT, maps to numBins log-spaced magnitude values [0,1].
// ⚠️ The two tables below are FUNCTION-LOCAL STATICS WITH NO LOCK, like the FFT config: every caller
// is the single UI poll thread (a second thread would need its own slot — the bin map is per call).
// Cached because recomputing them cost more than the transform itself.

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

// Which FFT bin each output bin reads. Depends only on (numBins, sampleRate), and the live consumers
// ask for different counts — the EQ panel and the visualizer strip — so a single slot would thrash
// between them and rebuild on every call. Four slots, replaced round-robin on a miss.
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

    // Cache the config across calls: kiss_fftr_alloc does a malloc + twiddle-table trig init
    // every time. FFT size is constant, so the cfg lives for the process (never freed).
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

// Read SPECTRUM_FFT_SIZE contiguous samples from a circular buffer of size bufSize.
static void readCircularBuffer(const float* buf, int writeIdx, int bufSize, kiss_fft_scalar* input) {
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        int idx = (writeIdx - SPECTRUM_FFT_SIZE + i + bufSize) % bufSize;
        input[i] = buf[idx];
    }
}

void AudioEngine::getSpectrumMagnitudes(int numBins, float* out) {
    lastSpectrumReadMs.store(nowMs(), std::memory_order_relaxed);  // demand signal for the capture gate
    kiss_fft_scalar input[SPECTRUM_FFT_SIZE];
    {
        std::lock_guard<std::mutex> lock(spectrumMutex);
        readCircularBuffer(spectrumBuffer, spectrumWriteIdx, SPECTRUM_SIZE, input);
    }
    computeSpectrumFFT(input, numBins, out, (float)getSampleRate());
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
    const float MANUAL_DECAY = 0.92f;

    for (int t = 0; t < 8; t++) {
        trackPeaksL[t] *= MANUAL_DECAY;
        trackPeaksR[t] *= MANUAL_DECAY;
        if (trackPeaksL[t] < 0.001f) trackPeaksL[t] = 0.0f;
        if (trackPeaksR[t] < 0.001f) trackPeaksR[t] = 0.0f;
    }
    masterPeakL *= MANUAL_DECAY;
    masterPeakR *= MANUAL_DECAY;
    if (masterPeakL < 0.001f) masterPeakL = 0.0f;
    if (masterPeakR < 0.001f) masterPeakR = 0.0f;
    sendPeakRevL *= MANUAL_DECAY; sendPeakRevR *= MANUAL_DECAY;
    sendPeakDelL *= MANUAL_DECAY; sendPeakDelR *= MANUAL_DECAY;
    if (sendPeakRevL < 0.001f) sendPeakRevL = 0.0f;
    if (sendPeakRevR < 0.001f) sendPeakRevR = 0.0f;
    if (sendPeakDelL < 0.001f) sendPeakDelL = 0.0f;
    if (sendPeakDelR < 0.001f) sendPeakDelR = 0.0f;
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

// ⚠️ THE apply* HELPERS EXIST BECAUSE VTR/VMV REACH THE SAME FADERS FROM THE AUDIO THREAD, where no
// log call may run (audio-defs.h). The setters are the helpers plus a LOGD; the queue arms call the
// helpers. Setting the value is all there is to do — both mix paths ramp to it every block.
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
    // Nothing else is needed to silence what is ringing: the next block picks the new target up and
    // both mix paths walk their gate to it over MUTE_GATE_SAMPLES, so a mute lands in ~5.8 ms rather
    // than in one sample. Voices keep running underneath — a mute is a gate, never a stop.
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

// The sample-accurate faces of the same two faders — what a VTR / VMV effect schedules. VMV is global
// and carries no track, so it borrows the queue's `trackId` field as -1 the way EQM does.
void AudioEngine::scheduleTrackVolume(int64_t targetFrame, int trackId, float volume) {
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, volume, PARAM_UPDATE_TRACK_VOL, 0.0f });
}

void AudioEngine::scheduleMasterVolume(int64_t targetFrame, float volume) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, volume, PARAM_UPDATE_MASTER_VOL, 0.0f });
}

// TIM. Global like VMV above, and carries no track for the same reason.
void AudioEngine::scheduleDelayTime(int64_t targetFrame, float time) {
    paramUpdateQueue.schedule({ targetFrame, -1, 0, time, PARAM_UPDATE_DELAY_TIME, 0.0f });
}

void AudioEngine::refreshSoundingInstrument(int instrumentId) {
    if (instrumentId < 0 || instrumentId >= 256) return;
    // ⚠️ NAMED RATHER THAN POSITIONAL: this is the one record that carries the instrument, and the
    // field sits past `eqBands` at the end of the struct where nothing else initialises.
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
    // The track's CURRENT note, for mid-note param updates (PBN/PVB/PAN). A releasing SF voice or a
    // fading (stolen) sampler voice is the previous note's tail, never the target — a naturally
    // decayed SF note stays isActive, and would otherwise capture every later update.
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
    // Convert the per-step bend rate to per-frame here (sample-rate/tempo known on this thread),
    // then enqueue the raw rate; the audio thread applies it to the active voice. 0 = stop.
    float ratePerFrame = 0.0f;
    if (fabsf(semitonesPerStep) >= 0.0001f) {
        float sr = (float)getSampleRate();
        float framesPerStep = sr / (tempo / 60.0f * 4.0f * 12.0f) * 12.0f;
        ratePerFrame = semitonesPerStep / framesPerStep;
    }
    paramUpdateQueue.schedule({ targetFrame, trackId, 0, ratePerFrame, PARAM_UPDATE_PITCH_BEND, 0.0f });
}

void AudioEngine::scheduleVibrato(int64_t targetFrame, int trackId, float speed, float depth) {
    // Enqueue speed+depth; the audio thread applies setVibratoRaw in the drain loop. depth=0 stops.
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

        // Per-slot RNG for the RND/DRNK LFO shapes, seeded per note (frame), per slot, and per
        // session/render (noteSeedEntropy), so two renders differ.
        dst.lfoRngState  = (((uint32_t)(uint64_t)currentFrame) * 747796405u
                            ^ (uint32_t)(m + 1) * 2891336453u
                            ^ noteSeedEntropy) | 1u;
        dst.lfoRandValue = (src.type == 3 && src.oscShape >= 8)
                           ? xorshift32Bipolar(dst.lfoRngState) : 0.0f;

        // LFO trigger mode: RETG/ONCE restart the cycle at note-on; FREE and HOLD align the
        // phase to the global frame clock (a stateless free-running LFO). HOLD additionally
        // freezes the clock-aligned value for the note's lifetime (tickLFO never advances it).
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
    // modDestValues[PARAM_PITCH] accumulates: TABLE_PITCH + PITCH_SLIDE + VIBRATO + user mod slots.
    // params.base[PARAM_PITCH] is FIN's fine tune — the same slot, its other half, cleared by every
    // trigger. Read as a pair here so a fine tune and a table transpose add rather than replace.
    // voice.playbackRate has no transpose baked in; arpeggio adjusts it via setMidiNote().
    float rateMod = powf(2.0f,
                         (voice.modDestValues[PARAM_PITCH] + voice.params.base[PARAM_PITCH]) / 12.0f);
    const float rate = voice.playbackRate * rateMod;

    // ── OSCILLATOR loop mode: one trip round the loop is one cycle of the played note ───────────
    //
    // ⭐⭐ THE WHOLE MODE IS THIS ONE FACTOR, AND IT INHERITS EVERY PITCH SOURCE: `rate × baseFrequency`
    // is what the voice sounds at now (transpose, FIN, slides, vibrato, arps); traversals per second
    // are `rate × sampleRate / loopLength`, so scaling by `loopLength × baseFrequency / sampleRate`
    // makes them equal. The factor is 1.0 when the loop is one cycle at the base pitch — the check.
    // ⚠️ So here the loop LENGTH is TIMBRE, not pitch — which is why LPO (never changing the length)
    // is the command this mode is played with.
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

    // The same granularity live playback uses — the two must not drift apart, or the export stops
    // matching what you heard. See PROCESS_SUBBLOCK.
    int rendered = 0;
    while (rendered < numFrames) {
        int chunk = std::min(PROCESS_SUBBLOCK, numFrames - rendered);
        processAudioBlock(output + rendered * 2, chunk, 2, (float)sampleRate);
        rendered += chunk;
    }
}

void AudioEngine::resetFrameCounter() {
    globalFrameCounter.store(0, std::memory_order_relaxed);
    // Fresh randomness per render — see noteSeedEntropy in the header.
    noteSeedEntropy = ((uint32_t)nowMs() * 2654435761u) | 1u;
}

void AudioEngine::resetEffectState() {
    const float sr = (float)getSampleRate();
    reverbSend.reset(sr);   // zeroes the delay lines AND reseeds ReverbSc's random-lineseg LCG
    delaySend.reset(sr);    // zeroes both delay lines
    masterChain.reset(sr);  // OTT bands, DUST, limiter envelope, master EQ
    // Everything above is at FACTORY DEFAULTS now. A render pushes the project next; a device reopen
    // replays `busSettings`. Not replayed here: a render must be a function of the project alone.
    LOGD("🎬 Effect chains reset to clean state");
}

// Control thread: build the reverb's engine for what the record now says, BEFORE the record is
// published — so the block that applies the new ALGO or SIZE finds the engine already waiting.
void AudioEngine::prepareReverb() {
    const BusSettings& s = busSettings;
    reverbSend.dragonfly.prepare(s.reverbAlgo, s.reverbDecay, s.reverbSize, s.reverbDamp, s.reverbMod,
                                 static_cast<float>(getSampleRate()));
}

// Audio thread, top of the block: apply every bus group whose sequence number moved since this
// thread last applied it — or every group, after setDeviceSampleRate rebuilt the buses.
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

int64_t AudioEngine::getFrameCounter() {
    return globalFrameCounter.load(std::memory_order_relaxed);
}

void AudioEngine::setOfflineRendering(bool offline) {
    isOfflineRendering.store(offline);
    LOGD("🎬 Offline rendering: %s", offline ? "ON" : "OFF");
}

void AudioEngine::setTempo(int tempo) {
    // Clamp to a sane musical range; the table-advance divides by this so it must be > 0.
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
    // An export carries the SONG, never the click that was helping the user write it. Also drops the
    // click in flight, so a render started mid-beat cannot leak its tail into the first block back.
    if (offlineRender) { metroClickPos_ = -1; return; }

    const int64_t epoch = metronomeEpoch.load(std::memory_order_relaxed);
    const int64_t beat  = metronomeBeatFrames.load(std::memory_order_relaxed);

    if (epoch != metroEpoch_) {
        // A new take. The grid is re-pinned to the transport's own start frame, which is what makes
        // beat 0 the downbeat rather than "wherever the click happened to be".
        metroEpoch_      = epoch;
        metroBeatFrames_ = beat;
        metroIndex_      = 0;
        metroCount_      = 0;
        metroClickPos_   = -1;
    } else if (beat > 0 && beat != metroBeatFrames_) {
        // TEMPO was turned mid-take. The epoch moves to where the NEXT beat was already going to
        // fall and the index restarts, so that beat keeps its slot — no beat jumps, doubles or is
        // lost — and every one after it takes the new spacing. songcore::MidiClock::rebase, same
        // arithmetic and same reason.
        metroEpoch_      = metroEpoch_ + metroIndex_ * metroBeatFrames_;
        metroIndex_      = 0;
        metroBeatFrames_ = beat;
    }

    if (metroEpoch_ < 0 || metroBeatFrames_ <= 0) return;   // nothing is playing

    // ⚠️ The grid walks while the click is silent, so turning it on mid-take waits for the next beat
    // instead of clicking at once for the one it passed while off.
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

    // ⚠️ The audio device can STALL AND RESUME — an Android suspend, a CFW power menu — and the frame
    // counter then jumps by the whole stall at once. Walking the backlog one beat at a time would fire
    // a click per block until it caught up; snap the grid to where the song actually is instead. The
    // index still counts every beat the song passed, so the accent stays on the bar.
    const int64_t behind = blockStartFrame - (metroEpoch_ + metroIndex_ * metroBeatFrames_);
    if (behind > metroBeatFrames_) {
        const int64_t skipped = behind / metroBeatFrames_;
        metroIndex_ += skipped;
        metroCount_ += skipped;
    }

    const int clickFrames = std::max(1, static_cast<int>(sampleRate * METRONOME_CLICK_SEC));

    for (int i = 0; i < numFrames; i++) {
        // A beat always restarts the click, rather than being dropped while one is still sounding:
        // at 999 BPM a beat is shorter than the click, and a metronome that skips beats when it is
        // pushed is worse than one that cuts its own tail.
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

        // A cubic decay from the first sample, which reaches exactly zero at the end of the window —
        // an envelope that merely got small would step at the cut. The sine starts at phase 0, so
        // there is no discontinuity at the onset either.
        const float t   = static_cast<float>(metroClickPos_) / static_cast<float>(clickFrames);
        const float env = (1.0f - t) * (1.0f - t) * (1.0f - t);
        const float s   = sinf(metroClickPhase_) * env * gain;
        metroClickPhase_ += metroClickStep_;
        if (++metroClickPos_ >= clickFrames) metroClickPos_ = -1;

        // Clamped, because this is summed BELOW the limiter: a loud mix plus a click can ask for more
        // than the DAC has, and a hard clip on a 30 ms transient is inaudible where the wrap is not.
        for (int ch = 0; ch < channelCount; ch++) {
            float& out = output[i * channelCount + ch];
            out = fmaxf(-1.0f, fminf(1.0f, out + s));
        }
    }
}
