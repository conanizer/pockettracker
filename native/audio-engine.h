#pragma once
// The portable audio core: voices, the sample-accurate queues and all DSP (processAudioBlock).
// No platform headers here — backends call processLiveBlock(), logging goes through audio-defs.h.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <algorithm>
#include <array>
#include "songcore/device_clock.h"
#include "songcore/program.h"
#include "songcore/seqlock.h"
#include "sampler-voice.h"
#include "soundfont-voice.h"
#include "soundfont-trim.h"
#include "effects/send-chain.h"
#include "effects/master-chain.h"

// One SoundFont voice per song track (0-7) plus the preview lane (8). Defined in audio-engine.cpp.
static const int SF_VOICE_COUNT = 9;
extern SoundfontVoice sfVoices[SF_VOICE_COUNT];

// ⚠️ A SoundFont has no size limit. What bounds a load is a check against the device's free memory
// where memory grows (TSF_MALLOC/TSF_REALLOC, PcmSink::append, load_budget_bytes) — on Android an
// oversized allocation is killed on the write, not refused.

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // The rate every platform opens its stream at; where the hardware runs another, the system
    // converts. One rate everywhere, so a project and its export sound the same on every device.
    static constexpr int SAMPLE_RATE = 48000;

    // The device's rate, set by the shell when the stream opens. Call before the stream is unpaused.
    // ⚠️ A new rate rebuilds the send and master buses at factory defaults; the caller re-pushes the
    // project's FX.
    void setDeviceSampleRate(int sr);

    // False when the buffers could not be allocated; the slot then still holds what it held.
    bool loadSample(int id, const float* data, int length);

    // Why the last media load failed; every load path sets it, a success clears it.
    // ⚠️ Append only. CANCELLED is the user's own stop and must not be drawn as an error.
    enum class LoadFailure { NONE = 0, PARSE, OUT_OF_MEMORY, CANCELLED };
    LoadFailure lastLoadFailure() const { return lastLoadFailure_; }

    // WAV (8/16/24/32-bit PCM, float, extensible) into slot `id`. Returns the file's rate, 0 on failure.
    int loadSampleFromWavFile(int id, const char* path);
    // mp3/flac/ogg/opus/AAC by extension (audio-decoders.h). Returns the source rate, 0 on failure.
    int loadSampleFromCompressed(int id, const char* path);
    bool hasStereoData(int id);
    // The depth the audio came in at (16 for a lossy decode). The sample editor's BIT offers this and
    // lower; SAVE writes it.
    int  getSampleBitDepth(int id) const;
    bool isSampleFloat(int id) const;
    // After SAVE: the buffer is the file now — record its depth and drop the RATE/BIT original.
    void adoptSavedSampleFormat(int id, int bits, bool isFloat);
    void clearAllSamples();
    void clearSample(int id);

    // ===================================
    // SOUNDFONT BANK
    // ===================================
    // One preset is cut out of the file and loaded, not the whole bank (a file that cannot be cut
    // loads whole). Slots are keyed by (path, bank, preset); the same sound twice shares one, and
    // one too many evicts the least recently used.
    int  loadSoundfont(int instrumentId, const char* path, int bank, int preset);   // → slot, or -1
    void unloadSoundfont(int slot);
    void clearAllSoundfonts();

    // The same load on a worker thread, for the PATCH row: a decode can take hundreds of ms.
    // One worker and no queue — a request while one runs answers BUSY, and the caller asks again.
    enum class SfRequest {
        READY,      ///< already resident — `readySlot` is it
        STARTED,    ///< poll collectSoundfontLoad until it comes back
        BUSY,       ///< a different load is in flight; ask again
    };
    SfRequest requestSoundfontLoad(int instrumentId, const char* path, int bank, int preset,
                                   int* readySlot);

    // Install a finished background load; false when none has finished. `slot` is −1 on failure.
    // ⚠️ The caller must check the instrument still wants this sound — the row may have moved on.
    bool collectSoundfontLoad(int* instrumentId, int* slot);

    bool soundfontLoadPending() const;

    bool soundfontSlotHolds(int slot, const char* path, int bank, int preset);

    // What `slot` holds; false (outputs untouched) when it is empty.
    bool soundfontSlotSound(int slot, std::string& path, int& bank, int& preset);

    int soundfontSlotCount() const;


    // A file's preset list, read from its index (no sample data) and cached by path — a slot holds one
    // preset and cannot answer it, and a bank too big to load can still be listed.
    int  getSoundfontFilePresetCount(const char* path);
    bool getSoundfontFilePresetAt(const char* path, int index, int* bank, int* presetNumber);
    std::string getSoundfontFilePresetName(const char* path, int bank, int preset);

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes);

    // The playback window in frames, for the sample editor's audition. Overrides start/end until the
    // next setInstrumentParams; −1 or an inverted pair disarms.
    void setInstrumentFrameWindow(int instrumentId, int startFrame, int endFrame);

    // End every voice at once, for a render clearing the previous take.
    // ⚠️ Writes the voices directly: audio thread only, or a render while the device is paused.
    void stopAll();

    // The STOP button: the same end, faded over KILL_FADE_SAMPLES so a cut waveform does not click.
    // A request — the next block runs the fade; safe from the UI. Use stopAll() for silence now.
    void stopAllRamped();

    // Every play verb calls this before it schedules. After a stop it restarts OTT, which would
    // otherwise carry the previous take's tail. Taken at the next block, like stopAllRamped().
    void startTake();

    // Restarts the output stream if the platform paused it; called before every scheduled note.
    std::function<void()> onResumeRequested;
    void requestResume() { if (onResumeRequested) onResumeRequested(); }

    // Live input (a MIDI keyboard), drained on the audio thread at the top of each block, so a note
    // sounds in the block it arrived in. During an export it is discarded instead.
    // ⚠️ Both calls run on the audio thread. Set before the stream opens, clear after it closes.
    // `LiveClock`: a point on the device's line (`DeviceClock`) — `frame` was being taken at `us`
    // (steady_clock µs) — so a byte's arrival stamp can be turned into a frame.
    struct LiveClock {
        int64_t frame      = 0;
        int64_t us         = 0;
        int     sampleRate = 0;
    };
    struct LiveInputSource {
        virtual ~LiveInputSource() = default;
        virtual void drainLiveInput(int64_t blockStartFrame, const LiveClock& clock) = 0;
        virtual void discardLiveInput() = 0;
    };
    void setLiveInput(LiveInputSource* source) { liveInput.store(source, std::memory_order_release); }

    int tempo() const { return currentTempo.load(std::memory_order_relaxed); }

    // Auditions play on this ninth lane, so they never steal a song track's voice.
    static const int PREVIEW_LANE = 8;

    int getActiveVoiceCount();

    // Each track's sounding note as octave*12+pitch, or -1. Reads both voice pools.
    void getTrackActiveNotes(int* out, int trackCount);

    int getSampleRate();

    // ===================================
    // SAMPLE EDITOR OPERATIONS
    // ===================================
    int   getSampleLength(int id);

    // Every byte of audio held here, for the USED RAM readout (undo, RATE originals, the clipboard,
    // SoundFont PCM). ⚠️ A new buffer must be counted here. Unlocked — fine for a readout only.
    int64_t audio_memory_bytes() const;

    // channel: 0=left, 1=right, 2=averaged; a mono sample always draws its one channel
    void  getSampleWaveformRangeSource(int id, int startFrame, int endFrame, float* out, int numBins, int channel);
    void  getSampleData(int id, float* out);       // left channel copy
    void  getSampleDataRight(int id, float* out);  // right channel copy
    float getSamplePlaybackPosition(int id);  // 0.0-1.0 fraction of active voice, or -1 if silent
    void normalizeSample(int id, int startFrame, int endFrame);
    void fadeInSample(int id, int startFrame, int endFrame);
    void fadeOutSample(int id, int startFrame, int endFrame);
    void silenceRegion(int id, int startFrame, int endFrame);
    void reverseSample(int id, int startFrame, int endFrame);
    void backupSample(int id);
    void undoSample(int id);
    // Free the undo copy when the editor closes — nothing can reach it after that.
    void freeSampleUndo(int id);
    // FX preview keeps its own clean copy, apart from undo.
    void saveFxPreviewBackup(int id);
    void restoreFxPreviewBackup();
    void cropSample(int id, int startFrame, int endFrame);
    void deleteSampleRegion(int id, int startFrame, int endFrame);
    void copyRegion(int id, int startFrame, int endFrame);
    void pasteRegion(int id, int insertAt);
    // The editor's audition copy of srcId into dstId. mode: 0=L, 1=R, 2=both, 3=avg.
    void prepareSourcePreview(int dstId, int srcId, int mode);
    int  getClipboardLength();
    // RATE and BIT: rebuilt from the cached original, decimated by `factor` (1/2/4) and quantised to
    // `bits`. One derive for both, so changing one keeps the other. (1, own depth) restores it.
    void applyRateAndBits(int id, int factor, int bits);
    // Destructive; clears the RATE original.
    void pitchShiftSample(int id, float semitones);
    // SOLA stretch: ratio > 1 is longer. `grainMs` is in the buffer's own `sampleRate`.
    void timeStretchSample(int id, float ratio, float grainMs, float sampleRate);
    // fxType 0=OTT, 1=DUST, 2=DRIVE, 3=EQ.
    // ⚠️ `fxValue` is the amount (0-255) for the first three but an EQ preset slot for EQ.
    void applySampleFx(int id, int fxType, int fxValue, float sampleRate, int limiterPreGain = 0);
    // Nearest zero crossing to `frame`: dir>0 forward only, <0 backward only, 0 either; `frame` if none.
    // One direction keeps marker snapping monotonic. `sourceMode` is the editor's SOURCE.
    int  findZeroCrossing(int id, int frame, int dir = 0, int searchRadius = 512, int sourceMode = 0);
    // Spectral-flux transients into outMarkers; returns the count. Higher sensitivity, more markers.
    int  detectTransients(int id, int sensitivity, int* outMarkers, int maxMarkers);

    // ===================================
    // CORE AUDIO PROCESSING BLOCK
    // ===================================
    // All real-time DSP lives here; processLiveBlock and renderOffline only wrap it.
    // ⚠️ The sample editor's offline DSP runs on the UI thread and needs flush-to-zero armed there too.
    void processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // The backend's audio callback: flush-to-zero, silence during a render, chunk into
    // PROCESS_SUBBLOCK calls, capture scopes and peaks.
    void processLiveBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // The callback's cost over the last second, in tenths of a percent of the block's duration
    // (1000 = about to run dry). `lateBlocks` counts every overrun since launch. Any thread.
    struct BlockTiming {
        int blockFrames = 0; int sampleRate = 0; int meanLoad = 0; int worstLoad = 0;
        int64_t lateBlocks = 0;
    };
    BlockTiming getBlockTiming() const;

    int64_t getCurrentFrame();

    // The frame the audio device is taking at `steadyUs` — what MIDI is timed against. The counter
    // until the device has called back (a host with no device).
    int64_t getDeviceFrame(int64_t steadyUs) const {
        int64_t f = 0;
        return deviceClock_.frame_at(steadyUs, f) ? f : globalFrameCounter.load(std::memory_order_relaxed);
    }
    // The most frames the device takes at once lately; 0 before it has called back.
    int getDeviceBlockFrames() const { return deviceClock_.block_frames(); }

    void scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                      float frequency, float baseFrequency, float volume, float phraseVolume = 1.0f, float pan = 0.5f,
                      int startPointOverride = -1, int endPointOverride = -1,
                      int tableId = -1, int tableTicRate = 6,
                      int noteOctave = 4, int notePitch = 0,
                      float pslInitialOffset = 0.0f, float pslDuration = 0.0f,
                      float pbnRate = 0.0f, float vibratoSpeed = 0.0f, float vibratoDepth = 0.0f,
                      int tableStartRow = -1);

    // The sequencer's path: a note by instrument number, resolved against the program table when it
    // fires — the only moment a table row could have changed it. `tempo` is the schedule-time tempo.
    void scheduleProgramNote(int64_t targetFrame, int trackId, int instrumentId,
                             const songcore::NoteOnPayload& noteOn, int tempo,
                             bool rootAudition = false);

    // Per-instrument SF2 ADSR, applied at the trigger, so instruments sharing a handle don't clash.
    void setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel);

    // End what sounds on a track: a sampler voice fades, a SoundFont plays its release.
    void scheduleKill(int64_t targetFrame, int trackId);

    // An OFF note: the ADSR or SoundFont release, or a declick fade where there is none.
    void scheduleNoteOff(int64_t targetFrame, int trackId);

    // A live MIDI key let go. ⚠️ Unlike scheduleNoteOff, a one-shot with no release plays on.
    void scheduleKeyRelease(int64_t targetFrame, int trackId);

    // KIL: every voice on the track fades over KILL_FADE_SAMPLES, whatever its release.
    void scheduleCut(int64_t targetFrame, int trackId);

    // The end of a render's range: every track gets a KIL, so only tails follow the last row.
    void scheduleNoteOffAll(int64_t targetFrame);

    void clearScheduledNotes();

    // Clear notes and kills at or after fromFrame. `trackId >= 0` clears one track — a LIVE launch
    // rewinds only the track it relaunches.
    void clearScheduledNotesFrom(int64_t fromFrame, int trackId = -1);

    // 16 rows × 8 bytes: transpose, volume, then three (fxType, fxValue) pairs.
    void loadTable(int tableId, const uint8_t* rowData);

    // Each table column's playhead row on this track, −1 for a column not running.
    void getVoiceTableRows(int trackId, int out[TABLE_LANES]);

    int getVoiceTableId(int trackId);

    // Where this track stands in one named table; false if not in it. Also answers for a table a hit
    // only passed through, which has no voice of its own.
    bool getTableRowsFor(int trackId, int tableId, int out[TABLE_LANES]);

    // The track's live loop window in frames, after LPO. False with no sampler voice.
    // ⚠️ The only way to observe it: no event, pixel or audio carries it directly.
    bool getVoiceLoopWindow(int trackId, int* startFrame, int* endFrame);

    // THO on an empty step.
    void scheduleVoiceTableRow(int64_t targetFrame, int trackId, int row);

    // Vxx on an empty step.
    void scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol);

    // ── Live per-note and mixer FX, through the sample-accurate param queue ──
    // A per-voice controller (PAN, REV, DEL, CUT, …) by its CC id (songcore/event.h); `value` is 0-1.
    void scheduleVoiceCc(int64_t targetFrame, int trackId, int cc, float value);
    void scheduleVoiceReverse(int64_t targetFrame, int trackId, bool reverse, bool restart);  // BCK
    void scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot);              // EQN xx
    void scheduleMasterEqSlot(int64_t targetFrame, int slot);                          // EQM xx
    // An AUS/AUF morph tick carries band values, not a slot: a point between two presets.
    void scheduleVoiceEqBands(int64_t targetFrame, int trackId, const EqBandsHex& bands);
    void scheduleMasterEqBands(int64_t targetFrame, const EqBandsHex& bands);
    void scheduleTrackVolume(int64_t targetFrame, int trackId, float volume);          // VTR xx
    void scheduleMasterVolume(int64_t targetFrame, float volume);                      // VMV xx
    void scheduleDelayTime(int64_t targetFrame, float time);                           // TIM xx

    // Make notes already sounding on an instrument hear its edited parameters — a trigger copies the
    // instrument into the voice. Every live edit path calls this; it lands at the next drain.
    void refreshSoundingInstrument(int instrumentId);

    void getWaveform(float* outBuffer, int bufferSize);

    // The OCTA scopes: TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE floats, and which lanes sounded last block.
    void getTrackWaveforms(float* outBuffer, bool* activeFlags);

    void getSpectrumMagnitudes(int numBins, float* out);

    // source: 0=master, 1=delay wet, 2=reverb wet, 3=instrument `instrId`.
    void getSpectrumMagnitudesForSource(int source, int instrId, int numBins, float* out);

    void getTrackPeaks(float* outBuffer);
    void getMasterPeaks(float* outBuffer);
    // [revL, revR, delL, delR]
    void getSendPeaks(float* outBuffer);

    // For when the stream is not running.
    void decayPeaks();
    void decayWaveform();

    void setTrackVolume(int trackId, float volume);

    // Mute, apart from the fader; ringing voices go silent within a block, and so does their send feed.
    void setTrackMuted(int trackId, bool muted);

    // The reverb return, delay return and dry-sum gates, set together because a solo is about the set.
    // ⚠️ The dry gate sits below every send tap: muting the eight tracks instead would starve a soloed
    // return. songcore/model.h derives all three.
    void setBusMutes(bool revMuted, bool dlyMuted, bool dryBusMuted);

    void setMasterVolume(float volume);

    // Route the preview lane through a track's fader (0..7), or -1 for unity.
    void setPreviewTrack(int trackId);

    // The same writes without a log line, for the VTR/VMV arms on the audio thread.
    void applyTrackVolume(int trackId, float volume);
    void applyMasterVolume(float volume);

    // ===================================
    // EQ METHODS
    // ===================================

    // One band of preset `slot` (0-127), band 0-2. type: 0=OFF 1=LOSHELF 2=LOWCUT 3=BELL 4=HISHELF 5=HICUT.
    // ⚠️ The type number is stored in the project — append in eq-module.h, never insert.
    // freqHex 00-FF → 20 Hz–20 kHz log; gainHex 0-240 → −12..+12 dB; qHex 00-FF → 0.1–10 log.
    void setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex);

    // -1 = off. Taken at the instrument's next note.
    void setInstrumentEqSlot(int instrId, int slot);

    // An instrument as the numbers a note needs, pushed on every change and read when a note fires.
    // ⚠️ The slice markers are copied; Program::sliceMarkers points into the project and is not kept.
    void setProgram(int instrumentId, const songcore::Program& program,
                    const int64_t* markers, int count);

    // ===================================
    // SEND LEVEL METHODS
    // ===================================

    void setInstrumentSendLevels(int instrId, int reverbHex, int delayHex);

    // ===================================
    // REVERB / DELAY SEND METHODS
    // ===================================

    // All 00-FF: DCAY, DAMP, the return gain, SIZE.
    void setReverbParams(int feedbackHex, int dampHex, int wetHex = 0x80, int sizeHex = 0x60);

    // PRE, WIDE, MOD; neutral at 00, 80, 40.
    void setReverbCharacter(int preHex, int widthHex, int modHex);

    // 0 = the default algorithm. Rewrites no other cell. Taken at the next block.
    void setReverbAlgo(int algo);

    // timeOrSubdiv is 00-FF (0-2 s) free, or a subdivision index 0-11 synced to `bpm`.
    void setDelayParams(int timeOrSubdiv, int feedbackHex, bool syncMode, float bpm = 120.0f, int wetHex = 0x80);

    // The same split in two, for pushing a delay whose TIME a running TIM has taken over.
    void setDelayTime(int timeOrSubdiv, bool syncMode, float bpm = 120.0f);
    void setDelayFeedbackWet(int feedbackHex, int wetHex);

    // PONG, TONE, WOBL; neutral at off, FF, 00.
    void setDelayCharacter(bool pong, int toneHex, int wobbleHex);

    // How much delay output feeds the reverb, 00-FF.
    void setDelayReverbSend(int sendHex);

    // EQ preset slot, -1 = off.
    void setReverbInputEq(int slot);
    void setDelayInputEq(int slot);
    void setMasterEqSlot(int slot);

    // Did a table row's EQM move the master bus? Reading clears it. This is the engine's half of the
    // restore on stop (songcore/host.h stop()) — the scheduler never sees a table row.
    bool takeTableMasterEqTouched() { return tableMasterEqTouched.exchange(false, std::memory_order_relaxed); }

    // ⚠️ Mid-take, ask with this: `take` would disarm the restore and the table's preset would stick.
    bool tableMasterEqTouchedPeek() const { return tableMasterEqTouched.load(std::memory_order_relaxed); }

    // The same pair for a table row's TIM.
    bool takeTableDelayTimeTouched() { return tableDelayTimeTouched.exchange(false, std::memory_order_relaxed); }
    bool tableDelayTimeTouchedPeek() const { return tableDelayTimeTouched.load(std::memory_order_relaxed); }

    // 0 = bypass, 255 = full.
    void setOttDepth(int depth);
    // For a render: clean state, no warm-up fade.
    void setOttDepthForRender(int depth);
    // 0=OTT, 1=DUST.
    void setMasterFx(int fx);
    // 0 = bypass, 255 = full. No-op unless DUST is the master FX.
    void setDustDepth(int depth);
    void setDustDepthForRender(int depth);
    // 0 = unity, 255 = 4× into the limiter.
    void setLimiterPreGain(int depth);


    // SoundFont voice first.
    IAudioVoice* findActiveVoiceForTrack(int trackId);

    // PBN on an empty step; ~0 stops the bend.
    void schedulePitchBend(int64_t targetFrame, int trackId, float semitonesPerStep, int tempo);

    // PVB/PVX on an empty step; depth 0 stops it.
    void scheduleVibrato(int64_t targetFrame, int trackId, float speed, float depth);

    // A MIDI-in pitch wheel on the track's voices, in semitones; the next trigger starts from 0.
    void scheduleVoiceWheel(int64_t targetFrame, int trackId, float semitones);

    void setInstrumentModulation(int sampleId, int slotIndex,
                                 int type, int dest, float amount,
                                 int attackSamples, int holdSamples, int decaySamples,
                                 float sustainLevel, float lfoHz, int oscShape,
                                 int releaseSamples = 0, int lfoTrigMode = 1);

    // At a trigger: copy the instrument's mod slots onto the voice, seed RND/DRNK, set the LFO phase.
    void initVoiceModSlots(IAudioVoice& voice, int sampleId, int64_t currentFrame, float sampleRate);

    // Start a resolved note in its voice type's pool. A new voice type gets its own, never an inline arm.
    void triggerSoundfontNote(const ScheduledNote& note, int frame, int64_t currentFrame, float sampleRate);
    void triggerSamplerNote(const ScheduledNote& note, int frame, int64_t currentFrame, float sampleRate);

    // What one processAudioBlock call's stages share, built per block. Each ramp is a (start, end)
    // pair interpolated per sample across the block.
    struct BlockMix {
        float*  output;               // interleaved stereo, the block being summed into
        int     numFrames;
        int     channelCount;
        float   sampleRate;
        int64_t startFrame;           // the block's first frame on the engine clock
        bool    offlineRender;
        bool    octaWanted;           // the OCTA scopes are being drawn
        bool    spectrumWanted;       // the send spectra are being drawn
        int     monitoredInstrId;     // the EQ screen's instrument, or -1
        float   faderStep;            // how far a fader may move in this block
        // Per track, the preview lane included: the fader and the mute gate.
        float   trackVolStart[SF_VOICE_COUNT], trackVolEnd[SF_VOICE_COUNT];
        float   gateStart[SF_VOICE_COUNT],     gateEnd[SF_VOICE_COUNT];
        float   revGateStart, revGateEnd, dlyGateStart, dlyGateEnd, dryGateStart, dryGateEnd;
        float   revReturnStart, revReturnEnd, dlyReturnStart, dlyReturnEnd;   // the returns' WET
        float   dlyToRevStart, dlyToRevEnd;                                   // the delay's feed into the reverb
        float   masterVolStart, masterVolEnd;
        int     previewTrack;         // the track the preview lane borrows its fader from, or -1
        bool    previewBorrows;
    };
    // processAudioBlock's stages, in call order (engine-mix.cpp).
    void walkMixerRamps(BlockMix& b);
    void clearBlockScratch(BlockMix& b);
    void takeQueuedWork(BlockMix& b);
    void dispatchEvents(BlockMix& b);
    void applyParamUpdate(const ScheduledParamUpdate& upd, BlockMix& b);
    void applyKill(const ScheduledKill& kill, int frame);
    void mixSamplerVoices(BlockMix& b);
    void storeTic00Cursor(int trackId, int tableId, const TableLane (&lanes)[TABLE_LANES]);
    void tableStartRows(const ScheduledNote& note, const TableLane* live, const int (&rates)[TABLE_LANES],
                        int (&out)[TABLE_LANES]);
    void prepareSamplerPiece(Voice& voice, int frames, float sampleRate);
    void mixSamplerPiece(Voice& voice, const BlockMix& b, int from, int to);
    // ⚠️ Compiled twice: a single note must not pay for a chord's playheads in its loop.
    template <bool Chord> void mixSamplerPieceAs(Voice& voice, const BlockMix& b, int from, int to);
    void mixSoundfontVoices(BlockMix& b);
    void prepareSoundfontPiece(SoundfontVoice& sv, int t, int from, int frames, float sampleRate);
    void renderSoundfontPiece(SoundfontVoice& sv, int t, tsf* h, const BlockMix& b, int from, int to);
    void renderSoundfontKeys(SoundfontVoice& sv, tsf* h, int t, float* buf, int from, int to);
    void carryFadesToNextBlock();
    void captureTrackScopes(const BlockMix& b);
    void mixBuses(const BlockMix& b);
    void mixMaster(const BlockMix& b);
    template <typename V>
    float mixTrackBuffer(V& v, int t, float* buf, const BlockMix& c, bool& stopFadeDone);

    // A track's voices, each passed as its own type so an overload decides. A new voice type adds its
    // pool here. Audio thread only.
    //
    // What a live command reaches: the sampler voice unless it is fading under the next note, and the
    // SoundFont voice while active (releasing included).
    template <typename F> void forEachTrackVoice(int track, F&& fn) {
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == track) fn(voices[v]);
        if (track >= 0 && track < SF_VOICE_COUNT && sfVoices[track].isActive) fn(sfVoices[track]);
    }
    // What a kill reaches: every sampler voice on the track, fading ones too, and its SoundFont voice.
    template <typename F> void forEachVoiceOnTrack(int track, F&& fn) {
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].isActive && voices[v].trackId == track) fn(voices[v]);
        if (track >= 0 && track < SF_VOICE_COUNT) fn(sfVoices[track]);
    }

    // A TIC in the table's last row overrides the instrument's tic rate, per column. Audio thread.
    void effectiveTicRatesFor(int tableId, int fallback, int out[TABLE_LANES]);

    // One table tick for a sampler or SoundFont voice (engine-tables.cpp). Plays every row due at
    // `from`, then returns how many frames may render before the next one (at most `maxFrames`).
    template <typename V> int processTableTick(V& voice, int from, int maxFrames, float sampleRate);

    // One column's row: lane 0 also carries transpose and volume. Returns true when the row only
    // steered the lane (HOP/THO) and the row it now stands on must play in this same tic.
    template <typename V> bool processTableRow(V& voice, const TableRow (&rows)[16], const TableRow& row, int lane,
                                               bool shouldAdvance, int atFrame, float sampleRate,
                                               bool applyNV = true);

    // A table FX that writes something (filter, drive, offset, EQ), shared by a row and a hit's carry.
    // Steering, KIL and VOL are the caller's.
    template <typename V> void applyTableWrite(V& voice, uint8_t fxType, uint8_t fxValue, float sampleRate);

    // Start a voice on what its hit picked up passing INS rows: after the instrument setup, before
    // the table's first row.
    template <typename V> void applyTableCarry(V& voice, const TableCarry& carry, float sampleRate);

    // A table's AUS/AUF ramps on one voice. ⚠️ After the row's own effects, so the fade wins over a
    // stale cell. `rowFraction` is per column; each ramp reads its parameter cell's column.
    template <typename V> void applyTableRamps(V& voice, const TableRow* rows,
                                               const double (&rowFraction)[TABLE_LANES],
                                               float sampleRate);

    void clearInstrumentModulation(int sampleId);

    // Once per block per voice.
    void updateVoiceModulation(IAudioVoice& voice, int numFrames, float sampleRate = 44100.0f);

    void updateVoicePitchMod(Voice& voice, int numFrames, float sampleRate);

    // Playback rate with pitch offset, vibrato and mod-slot pitch.
    float getModulatedPlaybackRate(Voice& voice);

    // ===================================
    // OFFLINE RENDER
    // ===================================
    void renderOffline(int numFrames, float* output, int sampleRate);

    void resetFrameCounter();

    // Clear every chain's internal state, so a render depends only on the project.
    // ⚠️ The modules also go back to factory defaults — the caller re-pushes the project. Never from
    // the audio thread.
    void resetEffectState();

    // While set, processLiveBlock outputs silence.
    void setOfflineRendering(bool offline);

    // The song BPM the table tic is locked to. Set before any note fires.
    void setTempo(int tempo);

    // 0 = full mix, 1-8 = track N-1, 9 = reverb return, 10 = delay return. Non-zero bypasses OTT,
    // DUST and the master EQ.
    void setStemsMode(int mode) { stemsMode = mode; }

    // ── The metronome ────────────────────────────────────────────────────────────────────────────
    // Not part of the song: summed after the master chain, silent in an export. Quarter notes from
    // the transport's start frame (epoch + k·framesPerBeat, so nothing drifts); a groove does not move it.

    // `gain` is the click's peak; 0 silences it.
    void setMetronome(bool enabled, float gain);

    // Pin beat 0, the accented one, to the transport's start.
    void startMetronome(int64_t startFrame, int64_t framesPerBeat);

    // A tempo change respaces the grid from the next beat.
    void setMetronomeBeat(int64_t framesPerBeat);

    void stopMetronome();

private:
    LoadFailure lastLoadFailure_ = LoadFailure::NONE;

    // The engine's denormal protection. ⚠️ Per thread, so every entry point that runs DSP calls it,
    // the sample editor's UI-thread ops included. Repeat calls are free.
    static void setFlushToZeroForCurrentThread();

    // Sum the click into a finished block, below the master chain. Silent during a render.
    void renderMetronome(float* output, int numFrames, int channelCount, float sampleRate,
                         int64_t blockStartFrame, bool offlineRender);

    // The event granularity and the size of every per-block buffer.
    // ⚠️ A faded voice frees its slot only as it is mixed, so a larger block drops same-track
    // retriggers. Live and offline both chunk here, so the export matches what is heard.
    static constexpr int PROCESS_SUBBLOCK = 256;

    // The device rate. Written by the shell, read by the scheduler.
    std::atomic<int> deviceSampleRate{SAMPLE_RATE};

    // The rate the buses were last built at, so setDeviceSampleRate knows a rebuild is owed.
    int effectsSampleRate = SAMPLE_RATE;

    // ⚠️ A bus setter never touches its module. It records the value here, bumps its group and
    // publishes; the audio thread applies each changed group at its next block (applyBusSettings).
    // Every call applies, even with an unchanged value — the STOP restores of TIM and EQM rely on it.
    // setDeviceSampleRate replays the record. Control thread only.
    enum BusGroup {
        BUS_REVERB_PARAMS, BUS_REVERB_ALGO, BUS_REVERB_CHAR, BUS_REVERB_INEQ,
        BUS_DELAY_TIME, BUS_DELAY_FEEDBACK, BUS_DELAY_CHAR, BUS_DELAY_INEQ,
        BUS_MASTER_EQ, BUS_OTT, BUS_MASTER_FX, BUS_DUST, BUS_LIMITER,
        BUS_GROUPS
    };
    struct BusSettings {
        int   reverbDecay = 0x60, reverbDamp = 0x80, reverbWet = 0x80, reverbSize = 0x60;
        int   reverbPre = 0x00, reverbWidth = 0x80, reverbMod = 0x10;
        int   reverbAlgo = 0, reverbInputEq = -1;
        int   delayTime = 2;   bool delaySync = true;   float delayBpm = 120.0f;   // 1/4 at 120 = 500 ms
        int   delayFeedback = 0x60, delayWet = 0x80;
        bool  delayPong = false;   int delayTone = 0xFF, delayWobble = 0x00;
        int   delayInputEq = -1, delayReverbSend = 0;
        int   masterEqSlot = -1;
        int   ottDepth = 0, masterFx = 0, dustDepth = 0, limiterPreGain = 0;
        bool  ottForRender = false, dustForRender = false;   // snap rather than glide (…ForRender)
        bool  pushed = false;   // nothing is replayed until a setter has run
        uint32_t seq[BUS_GROUPS] = {};
    };
    BusSettings               busSettings;       // the control thread's record
    SeqPublisher<BusSettings> busPublisher;
    void recordBus(BusGroup g) {
        ++busSettings.seq[g];
        busSettings.pushed = true;
        busPublisher.publish(busSettings);
    }
    // Audio thread: its copy, and the sequence number of each group it last applied.
    BusSettings       busLive;
    uint32_t          busSeen = 0;
    uint32_t          busApplied[BUS_GROUPS] = {};
    std::atomic<bool> busReplayRequested{false};   // setDeviceSampleRate: apply every group again
    void applyBusSettings();
    void prepareReverb();   // control thread — the one place the reverb's engines are allocated

    Voice voices[MAX_VOICES];

    // Where a TIC00 table stands once no voice is left to carry it, so the next note and the TABLE
    // screen continue from there. Written only in the sampler table-tick loop; per column.
    struct Tic00Cursor {
        int tableId       = -1;  // -1 = none; the next note starts at row 0
        int  row[TABLE_LANES]           = {0, 0, 0};
        int  lastProcessed[TABLE_LANES] = {-1, -1, -1}; // whether that row has been applied yet
        int  ticRate[TABLE_LANES]       = {6, 6, 6};    // only a column at TIC00 carries over
        // False once the column ran HOP FF — it must neither draw a marker nor resume.
        bool active[TABLE_LANES]        = {true, true, true};
    };
    // One bookmark per table within a track, not one per track: a hit passing through a table (INS)
    // leaves it on the next row while the voice runs another. A free slot is claimed on first use.
    static constexpr int TIC00_SLOTS = 8;
    Tic00Cursor tic00Cursor[SF_VOICE_COUNT][TIC00_SLOTS];
    // The table the track's voice last ran, for the TABLE screen. -1 = none (0 is a real table).
    int tic00Sounding[SF_VOICE_COUNT];
    // Table strums so far per track: a PP strum goes up on the even ones. Zeroed with the cursors.
    int tableStrumPass[SF_VOICE_COUNT];

    Tic00Cursor* tic00Slot(int trackId, int tableId, bool create);

    // ⚠️ What the UI reads of the voices. Published by the audio thread each block; every voice
    // getter answers from this copy, never from the live voices. A `lanes` entry is a marker, −1 none.
    struct VoiceView {
        struct Sampler {
            bool     active = false, fading = false;
            int      trackId = -1, instrId = -1, tableId = -1, note = -1, loopStart = 0, loopEnd = 0;
            uint32_t sampleGen = 0;
            double   position  = 0.0;
            int      lanes[TABLE_LANES] = {-1, -1, -1};
        } sampler[MAX_VOICES];
        struct Sf {
            bool active = false;
            int  tableId = -1, note = -1;
            int  lanes[TABLE_LANES] = {-1, -1, -1};
        } sf[SF_VOICE_COUNT];
        struct Bookmark {
            int tableId = -1;
            int lanes[TABLE_LANES] = {-1, -1, -1};
        } tic00[SF_VOICE_COUNT][TIC00_SLOTS];
        struct Sounding { int tableId = -1; } tic00Sounding[SF_VOICE_COUNT];
        const Bookmark* bookmark(int trackId, int tableId) const {
            if (trackId < 0 || trackId >= SF_VOICE_COUNT || tableId < 0) return nullptr;
            for (const Bookmark& b : tic00[trackId]) if (b.tableId == tableId) return &b;
            return nullptr;
        }
        // The first sounding sampler voice on the track that is (or is not) fading; −1 for none.
        int trackVoice(int trackId, bool fadingOne) const {
            for (int v = 0; v < MAX_VOICES; v++)
                if (sampler[v].active && sampler[v].fading == fadingOne && sampler[v].trackId == trackId) return v;
            return -1;
        }
    };
    SeqPublisher<VoiceView> voiceViewPublisher;
    void publishVoiceView();              // audio thread
    VoiceView voiceView_;                 // the UI's copy
    uint32_t  voiceViewSeen_ = 0;
    const VoiceView& voiceView() { voiceViewPublisher.read(voiceView_, voiceViewSeen_); return voiceView_; }
    // What a transport stop does.
    void resetTic00Cursors();

    // Follow the INS cells a hit passes through and return the instrument that sounds, or -1 for
    // silence. Each table passed keeps its place, so the next hit reads the next row (a rotation).
    // ⚠️ Only the last table ticks on the note; what a switching row holds left of its INS goes into
    // `carry`. `outTableId` is the table the voice runs. Audio thread, allocation-free.
    static constexpr int CHAIN_MAX_LINKS = 4;
    int resolveChain(int trackId, int instrumentId, int tableIdOverride, int* outTableId,
                     TableCarry* carry);

    // CHA and RNL roll on the audio thread, so they use their own lock-free PRNG.
    uint32_t chainRngState = 0x9E3779B9u;

    // Written by the UI under sampleEditMutex, read by the trigger without a lock.
    // ⚠️ A voice keeps its own pointer; `sampleGen` keeps it off a freed buffer — bumped after the new
    // pointers are stored, and the mix ends any voice on an older generation. Change buffers only
    // through setSampleBuffers / touchSample.
    std::atomic<float*>   samples[256];
    std::atomic<float*>   samplesRight[256];    // null = mono
    std::atomic<int>      sampleLengths[256];   // ONE length for both channels — samplesRight[id], when
                                                // non-null, always has exactly this length
    std::atomic<uint32_t> sampleGen[256];
    void touchSample(int id) { sampleGen[id].fetch_add(1); }   // caller holds sampleEditMutex
    // Undo and RATE caches only restore the buffer, so they are int16 — bit-exact for 16-bit sources.
    int16_t* sampleBackups[256] = {};
    int16_t* sampleBackupsRight[256] = {}; // null = the backup was mono
    int      sampleBackupLengths[256] = {};
    float* fxPreviewBackup      = nullptr;
    float* fxPreviewBackupRight = nullptr; // null = mono
    int    fxPreviewBackupLen   = 0;
    int    fxPreviewBackupId    = -1;
    int16_t* originalSamples[256] = {};      // the RATE original
    int16_t* originalSamplesRight[256] = {}; // null = mono
    // ⚠️ The same original in float, used instead of the int16 pair above 16 bits. At most one pair
    // exists per slot; freeRateCache frees both.
    float*   originalSamplesF[256] = {};
    float*   originalSamplesRightF[256] = {};
    int      originalSampleLengths[256] = {};
    uint8_t  sampleBitDepth[256];
    bool     sampleIsFloat[256] = {};
    void     freeRateCache(int id);
    // Wherever a new file replaces a slot: its depth, and no RATE/BIT original left over.
    void     setSampleSourceFormat(int id, int bits, bool isFloat);
    std::mutex sampleEditMutex;       // held during buffer swap; try-locked in voice mix loop
    float* sampleClipboard      = nullptr;
    float* sampleClipboardRight = nullptr; // null = mono clip
    int    sampleClipboardLength = 0;

    // Replace slot `id`'s buffers (newR null for mono), keeping both channels one length. Caller holds
    // sampleEditMutex; voices on the old buffers end at the next mix.
    void setSampleBuffers(int id, float* newL, float* newR, int newLen);
    // Lock sampleEditMutex and end every voice on slot `id`. ⚠️ Every destructive edit holds this lock
    // while it changes or frees the buffers; the audio thread's try_lock then skips one block.
    std::unique_lock<std::mutex> beginSampleEdit(int id);
    void fadeSample(int id, int startFrame, int endFrame, bool out);
    // An EQ preset slot (<0 = bypass) onto any EQ: a voice's, the master's, a send's input.
    void applyEqPresetToModule(EqModule& eq, int slot);
    // The same from band values — an AUS/AUF morph tick.
    void applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands);
    // The only place a SoundFont slot is freed: unpublish, bump the generation, wait out the block in
    // flight, close. Never on the audio thread.
    void freeSoundfontSlot(int slot);

    // Returns once any block that may hold an already-unpublished pointer has ended.
    // ⚠️ Unpublish first. Never on the audio thread — it would wait for itself.
    void waitForAudioBlockBoundary() {
        const uint64_t seen = audioBlocksDone.load();
        while (audioInBlock.load() && audioBlocksDone.load() == seen) std::this_thread::yield();
    }
    std::atomic<bool>     audioInBlock{false};
    std::atomic<uint64_t> audioBlocksDone{0};

    // A load in two halves: parse (file, trim, decode — no slot; the worker may run it) and install
    // (the slot table; owning thread only).
    // ⚠️ Only one parse may run at a time, or two decodes bid for memory the guard checks one at a time.
    tsf* parseSoundfont(const char* path, int bank, int preset, LoadFailure* failure);
    int  installSoundfont(tsf* handle, int instrumentId, const char* path, int bank, int preset);

    // Wait for a background load and reap its thread, keeping the result for collectSoundfontLoad.
    void waitForSoundfontLoad();

    // The same wait, discarding the result — for teardown.
    void discardSoundfontLoad();

    // The background load. The worker writes the result fields, then sets `sfLoadDone`; the owner
    // reads it, then the fields. `sfLoadBusy`: a thread exists and must be joined.
    std::thread       sfLoadThread;
    std::atomic<bool> sfLoadBusy{false};
    std::atomic<bool> sfLoadDone{false};
    int               sfLoadInstrument = -1;
    std::string       sfLoadPath;
    int               sfLoadBank = -1, sfLoadPreset = -1;
    tsf*              sfLoadHandle  = nullptr;
    LoadFailure       sfLoadFailure = LoadFailure::NONE;

    // Preset lists of the last few files, so scrolling PATCH does not re-read an index every frame.
    struct SfFileIndex { std::string path; std::vector<pt::SfPreset> presets; };
    std::vector<SfFileIndex> sfFileIndexCache;
    std::mutex               sfFileIndexMutex;
    // The cache position of `path`'s list, reading the file on a miss; -1 if it has no preset table.
    // ⚠️ Hold sfFileIndexMutex across the call and the read — a miss can move the vector.
    int soundfontFileIndexSlot(const char* path);

    // ⚠️ Every per-instrument table below is a StagedTable: a setter edits its copy and publishes;
    // the audio thread takes it in syncInstrumentData. An unpublished edit is never heard.
    StagedTable<InstrumentParams, 256>                        instrumentParams;
    StagedTable<std::array<InstrumentModSlot, 4>, 256>        instrumentModSlots;   // [sampleId][slot]
    // Keyed by instrument id, not sampleId (two instruments can share a sample).
    struct ProgramRow {
        songcore::Program program;
        int64_t           markers[songcore::PROGRAM_SLICE_MARKERS];
    };
    StagedTable<ProgramRow, songcore::PROGRAM_SLOTS> programs;
    songcore::Program programView(int id) const;   // audio thread
    void syncInstrumentData();                      // audio thread

    // Fill in a deferred note's sound as it fires; false (the note is dropped) when its instrument
    // has nothing to play. ⚠️ Audio thread: no allocation, no lock, no project access.
    bool resolveScheduledNote(ScheduledNote& note);
    // -1 = keep the preset's own value.
    struct SfEnvOverride { int atk = -1, dec = -1, sus = -1, rel = -1; };
    StagedTable<SfEnvOverride, 256> sfEnvOverrides;

    TableStore tables;             // 256 tables; the audio thread reads them without a lock
    std::mutex tableWriteMutex;

    NoteQueue noteQueue;
    KillQueue killQueue;
    ParamUpdateQueue paramUpdateQueue;
    // Each queue is drained once per block into these; reused, so no allocation after warm-up.
    std::vector<ScheduledNote>        noteBatch;
    std::vector<ScheduledKill>        killBatch;
    std::vector<ScheduledParamUpdate> paramBatch;

    // The OCTA and spectrum captures run only while the UI has read them within CAPTURE_IDLE_MS.
    static const int64_t CAPTURE_IDLE_MS = 250;
    std::atomic<int64_t> lastTrackWaveformReadMs{-CAPTURE_IDLE_MS};
    std::atomic<int64_t> lastSpectrumReadMs{-CAPTURE_IDLE_MS};
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::atomic<int64_t> globalFrameCounter{0};  // frames since start

    // The frame the stop ramp ends at, −1 when none. See processAudioBlock.
    std::atomic<int64_t> stopRampEndFrame{-1};
    std::atomic<bool>    stopRampRequested{false};   // stopAllRamped() → the next block's top
    std::atomic<bool>    stoppedSinceTake{false};     // stopAllRamped() → the next startTake()
    std::atomic<bool>    ottRestartRequested{false};  // startTake() → the next block's top
    void startStopRamp(int64_t blockStartFrame);

    // Mixed into per-note RND/DRNK seeds; reseeded from the clock at every render start so renders
    // differ. A torn read is just other entropy.
    uint32_t noteSeedEntropy = 0x9E3779B9u;
    std::atomic<bool> isOfflineRendering{false};

    // getBlockTiming's second: summed on the audio thread, published once a second of audio is in.
    void recordBlockTiming(std::chrono::steady_clock::time_point start, int numFrames, float sampleRate);
    int64_t timingBusyNs = 0;    // audio thread only
    int64_t timingFrames = 0;    // audio thread only
    int     timingWorst  = 0;    // audio thread only
    std::atomic<int> timingPubFrames{0}, timingPubRate{0}, timingPubMean{0}, timingPubWorst{0};
    std::atomic<int64_t> timingLateBlocks{0};
    std::atomic<int> currentTempo{120};
    std::atomic<LiveInputSource*> liveInput{nullptr};
    songcore::DeviceClock deviceClock_;   // fed by every live callback
    LiveClock liveClock_;   // audio thread only: the callback the sub-blocks belong to

    // ── The metronome ────────────────────────────────────────────────────────────────────────────
    // Written by the UI, read once a block by the audio thread.
    std::atomic<bool>    metronomeOn{false};
    std::atomic<float>   metronomeGain{0.0f};
    std::atomic<int64_t> metronomeEpoch{-1};        // the transport's start frame; -1 = stopped
    std::atomic<int64_t> metronomeBeatFrames{0};    // frames in a quarter note at the live tempo
    // Audio thread only. `metroIndex_` counts beats since the epoch (a tempo change rebases it);
    // `metroCount_` since the transport started — the bar accent follows it, so a nudge keeps the downbeat.
    // ⚠️ `metroTake_` is the take's start as set; `metroEpoch_` moves away from it on every rebase.
    int64_t metroTake_       = -1;
    int64_t metroEpoch_      = -1;
    int64_t metroBeatFrames_ = 0;
    int64_t metroIndex_      = 0;
    int64_t metroCount_      = 0;
    int     metroClickPos_   = -1;    // frames into the click being rendered; -1 = idle
    float   metroClickPhase_ = 0.0f;  // radians
    float   metroClickStep_  = 0.0f;
    static constexpr float METRONOME_CLICK_SEC = 0.030f;
    static constexpr float METRONOME_ACCENT_HZ = 2093.0f; // the downbeat  (C7)
    static constexpr float METRONOME_BEAT_HZ   = 1046.5f; // the other three (C6)
    static constexpr int   METRONOME_BEATS_PER_BAR = 4;
    // Latches set by a table row's EQM / TIM on the audio thread, read by the take…Touched calls.
    std::atomic<bool> tableMasterEqTouched{false};
    std::atomic<bool> tableDelayTimeTouched{false};
    int stemsMode = 0;  // see setStemsMode

    // The oscilloscope's ring of recent output.
    static const int WAVEFORM_SIZE = 620;
    float waveformBuffer[WAVEFORM_SIZE] = {};
    int waveformIndex = 0;
    std::mutex waveformMutex;

    // Spectrum captures for the EQ screen.
    static const int SPECTRUM_SIZE = 4096;
    float spectrumBuffer[SPECTRUM_SIZE] = {};       // master left channel
    int   spectrumWriteIdx = 0;
    float delaySpectrumBuffer[SPECTRUM_SIZE] = {};  // delay wet left
    int   delaySpectrumWriteIdx = 0;
    float reverbSpectrumBuffer[SPECTRUM_SIZE] = {}; // reverb wet left
    int   reverbSpectrumWriteIdx = 0;
    float instrSpectrumBuffer[SPECTRUM_SIZE] = {};  // one instrument, mono sum of its voices
    int   instrSpectrumWriteIdx = 0;
    std::atomic<int> instrSpectrumInstrId{-1}; // -1 = none
    std::mutex spectrumMutex;

    // This block's peaks, written by processAudioBlock and read by processLiveBlock.
    float framePeaksPerTrackL[8] = {0};
    float framePeaksPerTrackR[8] = {0};
    float frameSendPeakRevL = 0.0f, frameSendPeakRevR = 0.0f;
    float frameSendPeakDelL = 0.0f, frameSendPeakDelR = 0.0f;

    // The meters.
    float trackPeaksL[8] = {0};
    float trackPeaksR[8] = {0};
    float masterPeakL = 0.0f;
    float masterPeakR = 0.0f;
    float sendPeakRevL = 0.0f, sendPeakRevR = 0.0f;
    float sendPeakDelL = 0.0f, sendPeakDelR = 0.0f;
    std::mutex peakMutex;
    static constexpr float PEAK_DECAY = 0.95f;  // per callback

    // The mixer's targets: written by the setters (and the VTR/VMV arms), read once per block.
    // Relaxed atomics — the audio thread never waits on a fader.
    static_assert(std::atomic<float>::is_always_lock_free, "a fader write must not take a lock");
    std::atomic<float> trackVolumes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // ⚠️ Kept apart from the fader: a VTR writes the fader from the audio thread and would un-mute.
    std::atomic<bool>  trackMuted[8] = {false, false, false, false, false, false, false, false};
    // Where each gate has got to, chasing trackMuted. Audio thread only. Starts open.
    float trackGate[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // Where each fader has got to: it glides at FADER_GLIDE_SAMPLES per full swing, since a stepped
    // gain ticks. Audio thread only.
    float trackVolRamp[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float masterVolRamp   = 1.0f;
    // A VTR/VMV glide is under way: an export snaps a fader, but not one the song is moving.
    bool trackVolSongMove[8] = {};
    bool masterVolSongMove   = false;
    // Each track's last note-on frame — a VTR one frame behind it is that note's level. Audio thread.
    int64_t trackOnsetFrame[SF_VOICE_COUNT] = {INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2,
                                               INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2,
                                               INT64_MIN / 2};
    // The track whose fader the preview lane borrows, -1 for unity. An index, so a live fader move
    // is heard in the audition.
    std::atomic<int>   previewLaneTrack{-1};
    // The bus gates and where each has got to (the `*Gate` floats are audio thread only).
    std::atomic<bool>  revReturnMuted{false}, delayReturnMuted{false}, dryMuted{false};
    float revReturnGate = 1.0f, delayReturnGate = 1.0f, dryGate = 1.0f;
    std::atomic<float> masterVolume{1.0f};
    // The returns' WET and the delay's feed into the reverb: targets, walked once a block like a fader,
    // and where each has got to (audio thread only).
    std::atomic<float> reverbReturnGain{0.5f};
    std::atomic<float> delayReturnGain{0.5f};
    std::atomic<float> delayToReverbSend{0.0f};
    float reverbReturnRamp = 0.5f, delayReturnRamp = 0.5f, delayToReverbRamp = 0.0f;

    ReverbModule reverbSend;
    DelayModule  delaySend;
    MasterChain  masterChain;

    // 128 EQ presets, converted to Hz/dB/Q.
    struct EqPresetBank {
        EqBandData bands[3];
    };
    StagedTable<EqPresetBank, 128> eqPresets;
    // ⚠️ The same presets as authored hex. A table morph interpolates the hex (linear in log
    // frequency); Hz/dB/Q cannot be converted back. Both are written only in setEqBand, together.
    StagedTable<EqBandsHex, 128> eqPresetHex;

    // The OCTA scopes: 8 tracks plus the preview lane.
    static const int TRACK_WAVEFORM_COUNT = 9;
    float trackWaveformBuffer[TRACK_WAVEFORM_COUNT][WAVEFORM_SIZE] = {};
    int   trackWaveformIndex = 0;
    bool  trackHasVoice[TRACK_WAVEFORM_COUNT] = {};

    // ── processAudioBlock's scratch (audio thread only) ──────────────────────────────────────────
    // Members rather than locals (~116 KB) so a small real-time stack cannot overflow.
    float revSendBufL[PROCESS_SUBBLOCK], revSendBufR[PROCESS_SUBBLOCK];
    float dlySendBufL[PROCESS_SUBBLOCK], dlySendBufR[PROCESS_SUBBLOCK];
    float revWetL[PROCESS_SUBBLOCK], revWetR[PROCESS_SUBBLOCK];
    float dlyWetL[PROCESS_SUBBLOCK], dlyWetR[PROCESS_SUBBLOCK];
    float instrSpectrumTempL[PROCESS_SUBBLOCK];                           // the monitored instrument, mono
    float sfBuf[PROCESS_SUBBLOCK * 2];                                    // a track's SF render, interleaved
    float sfNoteBuf[PROCESS_SUBBLOCK * 2];                                // a stealing note's own pass
    float trackWaveAccumL[TRACK_WAVEFORM_COUNT][PROCESS_SUBBLOCK];
    float trackWaveAccumR[TRACK_WAVEFORM_COUNT][PROCESS_SUBBLOCK];
    bool  trackWasActive[TRACK_WAVEFORM_COUNT];

    // The oscilloscope's capture stride; 1 = every sample.
    static const int WAVEFORM_DOWNSAMPLE = 1;
    int waveformDownsampleCounter = 0;
};
