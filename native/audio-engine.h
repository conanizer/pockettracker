#pragma once
// ───────────────────────────────────────────────────────────────────────────────────────────────
// PORTABLE AUDIO CORE — no platform/Oboe/Android dependencies.
// This translation unit holds the whole engine: voices, note scheduling, the sample-accurate queues
// and ALL DSP (processAudioBlock). It must stay backend-agnostic so the Linux port is a drop-in: the
// Oboe glue (stream open/close, the audio callback) lives ONLY in oboe-audio-engine.{h,cpp}; a future
// ALSA/JACK/SDL2 backend is a parallel file that calls processLiveBlock() the same way. Do NOT add
// <oboe/*> or <android/*> includes here — logging already goes through the audio-defs.h shim.
// ───────────────────────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include "sampler-voice.h"
#include "soundfont-voice.h"
#include "effects/send-chain.h"
#include "effects/master-chain.h"

// Per-track soundfont voice state (shares soundfonts[sfSlot].handle via MIDI channels).
// 9 voices: song tracks 0-7 plus the dedicated preview lane (track 8 == AudioEngine::PREVIEW_LANE
// == Kotlin PREVIEW_TRACK_ID), so SF instrument previews never touch song tracks.
// Declared here so audio-engine.cpp and jni-bridge.cpp can reference sfVoices[].
static const int SF_VOICE_COUNT = 9;
extern SoundfontVoice sfVoices[SF_VOICE_COUNT];

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Cache the backend's device sample rate. Set once by the platform shell (OboeAudioEngine) when the
    // stream opens, then read by getSampleRate() and the scheduler-thread pitch/tic math — so the core
    // never has to reach into a platform stream object for it.
    void setDeviceSampleRate(int sr) { deviceSampleRate.store(sr, std::memory_order_relaxed); }

    void loadSample(int id, const float* data, int length);
    void loadSampleStereo(int id, const float* left, const float* right, int length);

    // Streaming sample load — decode a compressed file (e.g. MP3) chunk-by-chunk straight into native
    // memory so the whole PCM never has to live on the Java heap. begin allocates the slot from an
    // (over-)estimated frame count; fillSampleChunk writes interleaved 16-bit chunks in place; finalize
    // publishes the real length; cancel frees a partial load on decode failure. One load at a time.
    bool beginSampleLoad(int id, int channels, int estimatedFrames);
    void fillSampleChunk(int id, const int16_t* interleaved, int frameCount, int channels);
    int  finalizeSampleLoad(int id);
    void cancelSampleLoad(int id);
    // Decode a WAV file straight into native sample memory (no Java-heap round trip). Handles the
    // same formats as the Kotlin parser it replaces for file loads — 16/24/32-bit PCM, 32-bit float,
    // mono/stereo, WAVE_FORMAT_EXTENSIBLE. Returns the WAV sample rate (>0) on success, 0 on failure.
    // Lets multi-MB samples load without OOM on the capped Java heap.
    int loadSampleFromWavFile(int id, const char* path);
    // Decode a compressed audio file (mp3/flac/ogg) natively into native sample memory — no Java heap,
    // no MediaCodec. Dispatches by file extension to dr_mp3 / dr_flac / stb_vorbis, then publishes via
    // the same slot path as loadSampleStereo. Returns the source sample rate (>0) on success, 0 on
    // failure (incl. unsupported extension). M4A/AAC is NOT handled here — it stays on MediaCodec.
    int loadSampleFromCompressed(int id, const char* path);
    bool hasStereoData(int id);
    void clearAllSamples();
    // Free all buffers for a single slot (used when a slot is repurposed, e.g. sampler → SoundFont).
    void clearSample(int id);

    // ===================================
    // SOUNDFONT BANK — SF2 files → the shared tsf handles the SF voices render through
    // ===================================
    // Loading a SoundFont is engine work, not platform work: it opens a file, parses it, and owns the
    // handle every SF voice reads. It lived in jni-bridge.cpp until S6b, which put it out of reach of
    // any non-Android build — tools/ptrender could not render an SF2 project, and the SDL shell would
    // have had to reimplement the slot cache. The JNI functions are now thin forwards to these.
    //
    // MAX_SOUNDFONTS slots, shared by file: an already-loaded path de-duplicates onto its existing slot
    // (instruments sharing a handle play on distinct MIDI channels and apply their ADSR override
    // per-note, so their state stays isolated), and one more distinct SF2 than there are slots evicts
    // the least-recently-used one.
    int  loadSoundfont(int instrumentId, const char* path);   // → slot index, or -1 on failure
    void unloadSoundfont(int slot);
    void clearAllSoundfonts();

    // Preset metadata for the UI. Each takes the slot mutex before touching the handle: a concurrent
    // load can evict and tsf_close a slot at any moment, and TSF's getters dereference without a null
    // check. Empty slot → "---" / false / 0.
    std::string getSoundfontPresetName(int slot, int bank, int preset);
    bool getSoundfontPresetAt(int slot, int index, int* bank, int* presetNumber);
    int  getSoundfontPresetCount(int slot);

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes);

    void stopTrack(int trackId);
    void stopAll();

    // Platform hook: the audio shell installs a callback that restarts the output stream if the
    // platform paused it (Oboe today; ALSA/SDL on Linux). The Kotlin path called
    // backend.resumeStream() before every scheduled note; songcore's consumer calls requestResume()
    // for the same reason, without knowing what a stream is. Unset = no-op.
    std::function<void()> onResumeRequested;
    void requestResume() { if (onResumeRequested) onResumeRequested(); }

    int getActiveVoiceCount();

    /**
     * For each of the 8 tracks, encode the active note as (octave * 12 + pitch), or -1 if no
     * voice is currently playing on that track. The caller passes a pre-allocated int[8] array.
     */
    void getTrackActiveNotes(int* out, int trackCount);

    int getSampleRate();

    // ===================================
    // SAMPLE EDITOR OPERATIONS
    // ===================================
    int   getSampleLength(int id);
    void  getSampleWaveform(int id, float* out, int numBins);
    void  getSampleWaveformRange(int id, int startFrame, int endFrame, float* out, int numBins);
    // channel: 0=left, 1=right, 2=averaged (for STEREO/MONO source views)
    void  getSampleWaveformRangeSource(int id, int startFrame, int endFrame, float* out, int numBins, int channel);
    void  getSampleData(int id, float* out);  // raw float copy for WAV export (left channel)
    void  getSampleDataRight(int id, float* out);  // right channel copy (for SOURCE=RIGHT or STEREO save)
    float getSamplePlaybackPosition(int id);  // 0.0-1.0 fraction of active voice, or -1 if silent
    void normalizeSample(int id, int startFrame, int endFrame);
    void fadeInSample(int id, int startFrame, int endFrame);
    void fadeOutSample(int id, int startFrame, int endFrame);
    void silenceRegion(int id, int startFrame, int endFrame);
    void reverseSample(int id, int startFrame, int endFrame);
    void backupSample(int id);
    void undoSample(int id);
    // Free the single-level undo backup for a slot. Called when the sample editor closes: undo is
    // unreachable once the editor is gone, so the backup is otherwise dead weight — a full-length copy
    // (×2 for stereo) sitting in RAM until the slot is reloaded.
    void freeSampleUndo(int id);
    // Non-destructive FX preview: saves a clean copy separate from the undo slot.
    // Call saveFxPreviewBackup before applySampleFx for preview; restoreFxPreviewBackup to revert.
    void saveFxPreviewBackup(int id);
    void restoreFxPreviewBackup();
    // Destructive resize operations
    void cropSample(int id, int startFrame, int endFrame);
    void deleteSampleRegion(int id, int startFrame, int endFrame);
    void copyRegion(int id, int startFrame, int endFrame);
    void pasteRegion(int id, int insertAt);
    // Sample-editor LEFT/RIGHT/MONO source preview: copy the selected channel (or the L/R
    // average) of srcId into dstId's slot, entirely in native memory. mode: 0=L, 1=R, 3=avg.
    void prepareSourcePreview(int dstId, int srcId, int mode);
    int  getClipboardLength();
    void downsampleSample(int id, int factor);
    // Non-destructive rate mode: derives buffer from cached original (factor 1=HIGH,2=NORM,4=LOFI).
    void applyRateMode(int id, int factor);
    // Destructive pitch shift by semitones (applied to buffer in-place; clears original cache).
    void pitchShiftSample(int id, float semitones);
    // Destructive time-stretch: ratio > 1 = longer/slower, < 1 = shorter/faster. SOLA algorithm.
    void timeStretchSample(int id, float ratio);
    // Destructive whole-sample DSP: fxType 0=OTT, 1=DUST, 2=DRIVE. fxValue 0-255.
    void applySampleFx(int id, int fxType, int fxValue, float sampleRate, int limiterPreGain = 0);
    // Zero-crossing search near `frame`. dir>0 = forward only, dir<0 = backward only, dir==0 = nearest
    // (both ways); returns `frame` if none within searchRadius. Directional keeps marker snapping
    // monotonic so a small move can't snap back and stick.
    int  findZeroCrossing(int id, int frame, int dir = 0, int searchRadius = 512);
    // Spectral-flux transient detection. Returns count; outMarkers[] filled with frame positions.
    // sensitivity 0x00 = few markers (high threshold), 0xFF = many markers (low threshold).
    int  detectTransients(int id, int sensitivity, int* outMarkers, int maxMarkers);

    // ===================================
    // CORE AUDIO PROCESSING BLOCK
    // ===================================
    // ALL audio DSP lives here. processLiveBlock and renderOffline are thin wrappers.
    // Rule: NEVER add audio processing logic directly to processLiveBlock or renderOffline.
    void processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // ===================================
    // LIVE BLOCK ENTRY (called by the platform backend's audio callback)
    // ===================================
    // The platform shell (OboeAudioEngine::onAudioReady) hands its raw output buffer here. This does
    // everything the old onAudioReady did MINUS the Oboe glue: flush-to-zero, clear the buffer, bail to
    // silence during offline render, chunk into MAX_BLOCK processAudioBlock calls, then capture the
    // oscilloscope/spectrum/peak data. Backend-agnostic — no DSP lives in the callback shell.
    void processLiveBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // Get current global frame counter (for scheduling notes from Kotlin)
    int64_t getCurrentFrame();

    // Schedule a note to be played at exact frame
    void scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                      float frequency, float baseFrequency, float volume, float phraseVolume = 1.0f, float pan = 0.5f,
                      int startPointOverride = -1, int endPointOverride = -1,
                      int tableId = -1, int tableTicRate = 6,
                      int noteOctave = 4, int notePitch = 0,
                      float pslInitialOffset = 0.0f, float pslDuration = 0.0f,
                      float pbnRate = 0.0f, float vibratoSpeed = 0.0f, float vibratoDepth = 0.0f,
                      int tableStartRow = -1);

    // Store a per-instrument SF2 ADSR override. Keyed by instrument id and
    // applied atomically at note trigger, so instruments sharing a de-duplicated handle don't clash.
    void setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel);

    // Schedule a soundfont note (public method — called from JNI)
    void scheduleSoundfontNote(int64_t targetFrame, int trackId, int sfSlot,
                               int midiNote, int midiVelocity, float vol, float pan,
                               int bank, int preset,
                               float pslInitialOffset, float pslDuration,
                               float pbnRate, float vibratoSpeed, float vibratoDepth,
                               float phraseVol = 1.0f, int sampleId = -1,
                               int tableId = -1, int tableTicRate = 6,
                               int noteOctave = 4, int notePitch = 0,
                               int tableStartRow = -1, float detuneSemitones = 0.0f);

    // Schedule a kill event (for Kill effect K00)
    void scheduleKill(int64_t targetFrame, int trackId);

    // Schedule a soft note-off (triggers ADSR release instead of hard stop)
    void scheduleNoteOff(int64_t targetFrame, int trackId);

    // Clear all scheduled notes
    void clearScheduledNotes();

    // Clear only notes/kills at or after fromFrame (leaves the current phrase intact)
    void clearScheduledNotesFrom(int64_t fromFrame);

    // Load table data from Kotlin
    // rowData format: 16 rows × 8 bytes = 128 bytes
    // Each row: [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value, fx3Type, fx3Value]
    void loadTable(int tableId, const uint8_t* rowData);

    // Get current table row for a voice (for UI feedback)
    int getVoiceTableRow(int trackId);

    // Get table ID for a voice
    int getVoiceTableId(int trackId);

    // Schedule a table-row jump (THO on an empty step) for the active sampler voice at targetFrame.
    void scheduleVoiceTableRow(int64_t targetFrame, int trackId, int row);

    // Schedule a phraseVol update at exact frame (Vxx effect on empty steps)
    void scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol);

    // ── Live per-note / mixer FX (all routed through the sample-accurate param queue) ──
    void scheduleVoicePan(int64_t targetFrame, int trackId, float pan);                // PAN xx
    void scheduleVoiceReverbSend(int64_t targetFrame, int trackId, float send);        // REV xx
    void scheduleVoiceDelaySend(int64_t targetFrame, int trackId, float send);         // DEL xx
    void scheduleVoiceReverse(int64_t targetFrame, int trackId, bool reverse, bool restart);  // BCK
    void scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot);              // EQN xx
    void scheduleMasterEqSlot(int64_t targetFrame, int slot);                          // EQM xx

    // Get waveform data for oscilloscope display
    void getWaveform(float* outBuffer, int bufferSize);

    // Get per-track waveform data for OCTA visualizer.
    // outBuffer: TRACK_WAVEFORM_COUNT * WAVEFORM_SIZE floats (track0[0..619], ... track7, preview).
    // activeFlags: bool[TRACK_WAVEFORM_COUNT] — true if that lane had active (non-fading) voices last block.
    void getTrackWaveforms(float* outBuffer, bool* activeFlags);

    // Get log-spaced frequency-domain magnitude spectrum for EQ visualizer (0-1 per bin)
    void getSpectrumMagnitudes(int numBins, float* out);

    // Per-context spectrum for EQ visualizer.
    // source: 0=master, 1=delay-wet, 2=reverb-wet, 3=instrument (instrId used when source==3)
    void getSpectrumMagnitudesForSource(int source, int instrId, int numBins, float* out);

    // Get per-track peak levels for mixer meters
    void getTrackPeaks(float* outBuffer);

    // Get master peak levels (stereo) for mixer meters
    void getMasterPeaks(float* outBuffer);

    // Get send bus peak levels [revL, revR, delL, delR] for mixer meters
    void getSendPeaks(float* outBuffer);

    // Decay peaks manually (call when audio stream is not running)
    void decayPeaks();

    // Decay waveform buffer (call when audio stream is not running)
    void decayWaveform();

    // Set real-time track volume (affects playback immediately, including SF channels).
    void setTrackVolume(int trackId, float volume);

    // Set real-time master volume (affects playback immediately)
    void setMasterVolume(float volume);

    // ===================================
    // EQ METHODS
    // ===================================

    // Set one band of an EQ preset slot (hex params converted to Hz/dB/Q internally).
    // slot: 0-127, band: 0-2, type: 0=off 1=loShelf 2=bell 3=hiShelf
    // freqHex: 00-FF → 20–20kHz log, gainHex: 0-240 → −12.0..+12.0 dB (0.1 dB/step), qHex: 00-FF → 0.1–10 log
    void setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex);

    // Map an instrument to an EQ preset slot (-1 = off).
    // Copies the preset into instrumentParams[instrId] for use at next note trigger.
    void setInstrumentEqSlot(int instrId, int slot);

    // ===================================
    // SEND LEVEL METHODS
    // ===================================

    // Set reverb/delay send levels for an instrument (00-FF each, converted to float).
    void setInstrumentSendLevels(int instrId, int reverbSend, int delaySend);

    // ===================================
    // REVERB / DELAY SEND METHODS
    // ===================================

    // Set reverb params. feedbackHex/dampHex/wetHex: 00-FF. wetHex controls return gain.
    void setReverbParams(int feedbackHex, int dampHex, int wetHex = 0x80);

    // Set delay params. syncMode false: timeOrSubdiv is hex 00-FF (0-2s).
    //                   syncMode true:  timeOrSubdiv is subdivision index 0-11, bpm used.
    //                   wetHex: 00-FF return gain.
    void setDelayParams(int timeOrSubdiv, int feedbackHex, bool syncMode, float bpm = 120.0f, int wetHex = 0x80);

    // Set delay→reverb send level. sendHex 00-FF: how much delay output feeds into reverb.
    void setDelayReverbSend(int sendHex);

    // Set reverb/delay input EQ from the global preset bank (-1 = off).
    void setReverbInputEq(int slot);
    void setDelayInputEq(int slot);

    // Set master EQ from the global preset bank (-1 = off).
    void setMasterEqSlot(int slot);

    // Set OTT depth (0=bypass, 255=full wet). Enables/disables OTT module.
    void setOttDepth(int depth);
    // Reset OTT for offline render: clean state, no warmup fade.
    void setOttDepthForRender(int depth);
    // Select active master bus effect (0=OTT, 1=DUST).
    void setMasterFx(int fx);
    // Set DUST amount (0=bypass, 255=full). No-op when masterFx != 1.
    void setDustDepth(int depth);
    // Reset DUST for offline render: clears delay/envelope state before export.
    void setDustDepthForRender(int depth);
    // Set limiter pre-gain (0=unity 1.0x, 255=max 4.0x drive into limiter).
    void setLimiterPreGain(int depth);


    // Returns the active voice for a given track, checking SF voices first.
    IAudioVoice* findActiveVoiceForTrack(int trackId);

    // Schedule a continuous pitch bend (PBN on an empty step) at targetFrame — applied on the
    // audio thread via paramUpdateQueue (no off-thread voices[] write). ~0 stops the bend.
    void schedulePitchBend(int64_t targetFrame, int trackId, float semitonesPerStep, int tempo);

    // Schedule vibrato (PVB/PVX on an empty step) at targetFrame. depth=0 stops vibrato.
    void scheduleVibrato(int64_t targetFrame, int trackId, float speed, float depth);

    // Set per-instrument modulation slot (called from Kotlin before scheduling each note)
    void setInstrumentModulation(int sampleId, int slotIndex,
                                 int type, int dest, float amount,
                                 int attackSamples, int holdSamples, int decaySamples,
                                 float sustainLevel, float lfoHz, int oscShape,
                                 int releaseSamples = 0, int lfoTrigMode = 1);

    // Copy an instrument's mod-slot config onto a voice at note trigger: resets all per-note
    // state, seeds the RND/DRNK RNG, and applies the LFO trigger mode's initial phase.
    // Shared by the sampler and SF dispatch paths (audio thread only).
    void initVoiceModSlots(IAudioVoice& voice, int sampleId, int64_t currentFrame, float sampleRate);

    // M8-style: a TIC FX in the table's LAST row overrides the instrument's tic rate at
    // note trigger. Shared by the sampler and SF dispatch paths (audio thread only).
    int effectiveTicRateFor(int tableId, int fallback);

    // Unified per-voice table tick (tic advance + row FX processing) for sampler AND SF
    // voices — was two drifted ~90-line copies. Duck-typed template over the identical
    // table-state fields; the two per-type differences (KIL semantics, OFFSET) resolve at
    // compile time via the tableKill/tableOffset overloads in audio-engine.cpp, where the
    // template is defined and (implicitly) instantiated.
    template <typename V> void processTableTick(V& voice, int numFrames, float sampleRate);

    // Smart note-off: trigger ADSR/TRIG release if available, otherwise hard-stop.
    void triggerNoteOff(int trackId);

    // Clear all modulation slots for an instrument
    void clearInstrumentModulation(int sampleId);

    // Advance modulation stages for one voice (called once per audio callback).
    void updateVoiceModulation(IAudioVoice& voice, int numFrames, float sampleRate = 44100.0f);

    // Update pitch modulation for a single voice (called per frame in audio callback)
    void updateVoicePitchMod(Voice& voice, int numFrames, float sampleRate);

    // Get modulated playback rate including pitch offset, vibrato, and mod-slot pitch.
    float getModulatedPlaybackRate(Voice& voice);

    // ===================================
    // OFFLINE RENDER (for WAV export — thin wrapper)
    // ===================================
    void renderOffline(int numFrames, float* output, int sampleRate);

    // Reset frame counter (for starting a new render)
    void resetFrameCounter();

    // Clear every effect chain's INTERNAL STATE: the reverb's delay lines and its random-lineseg LCG,
    // the delay buffers, and the OTT/DUST/limiter envelopes. stopAll() stops voices but leaves all of
    // this running, so before S6b a render began inside the previous render's reverb tail — and since
    // ReverbSc's LCG kept walking, the same song rendered differently every time. A render must be a
    // function of the project, not of playback history.
    //
    // ⚠️ This is NOT a state-only reset: the module reset()s also re-apply their factory DEFAULTS
    // (reverb feedback 0x60, delay 500 ms, master EQ bypassed). The caller MUST re-push the project's
    // FX afterwards or it silently renders with default reverb/delay — songcore::prepare_render does
    // exactly that, via engine_setup.h. Live playback never calls this.
    void resetEffectState();

    // Get current frame counter
    int64_t getFrameCounter();

    // Offline rendering flag: when true, processLiveBlock outputs silence instead of audio.
    void setOfflineRendering(bool offline);

    // Current song tempo (BPM). Used by the standard-mode table advance to compute a
    // frame-accurate, tempo-locked tic so table speed matches the sequencer (and render==live,
    // device-independent). Set by the Kotlin scheduler before any note fires.
    void setTempo(int tempo);

    // Stems render mode: 0=normal full mix, 1-8=track N (0-indexed N-1),
    // 9=reverb-return-only, 10=delay-return-only. OTT/DUST/masterEQ are bypassed for non-zero modes.
    void setStemsMode(int mode) { stemsMode = mode; }

private:
    // Maximum frames processAudioBlock can handle in one call — all its per-block buffers
    // (send buses, OCTA accumulators, sfBuf) are sized to this. Callers with potentially
    // larger blocks (processLiveBlock, renderOffline) must chunk.
    static constexpr int MAX_BLOCK = 1024;

    // Device output sample rate, cached from the platform backend (see setDeviceSampleRate). Defaults to
    // 44100 so getSampleRate()/pitch math stay correct if read before the stream opens — matches every
    // other 44100 fallback in the engine. Atomic: written by the shell thread, read by the scheduler.
    std::atomic<int> deviceSampleRate{44100};

    Voice voices[MAX_VOICES];
    float* samples[256];
    float* samplesRight[256];          // right channel for stereo samples (null = mono)
    int    sampleLengths[256];         // ONE length for both channels — samplesRight[id], when non-null,
                                       // always has exactly this length (kept in lockstep by every edit op)
    // Undo + RATE-HIGH caches exist only to RESTORE the working buffer, never to play directly, so
    // they are stored as int16 to halve their RAM. Bit-exact for the 16-bit-sourced
    // WAVs that dominate (decoder reads those as v/32768); ~-96 dBFS requantization otherwise.
    int16_t* sampleBackups[256];      // single-level undo buffers (left channel)
    int16_t* sampleBackupsRight[256]; // single-level undo buffers (right channel; null = backup was mono)
    int      sampleBackupLengths[256];// length of both backup channels
    float* fxPreviewBackup      = nullptr; // separate clean-sample copy for FX preview (doesn't clobber undo)
    float* fxPreviewBackupRight = nullptr; // right channel of the FX-preview backup (null = mono)
    int    fxPreviewBackupLen   = 0;
    int    fxPreviewBackupId    = -1;
    int16_t* originalSamples[256];      // cached HIGH-rate original for non-destructive RATE mode (left, int16 — see above)
    int16_t* originalSamplesRight[256]; // cached HIGH-rate original (right channel; null = mono)
    int      originalSampleLengths[256];
    std::mutex sampleEditMutex;       // held during buffer swap; try-locked in voice mix loop
    float* sampleClipboard      = nullptr; // cross-operation copy/paste buffer (left)
    float* sampleClipboardRight = nullptr; // copy/paste buffer (right channel; null = mono clip)
    int    sampleClipboardLength = 0;

    // Streaming-load cursor (see beginSampleLoad). Touched only on the decode thread; the audio thread
    // never reads these — it sees the slot via sampleLengths[id], which stays 0 until finalize.
    int streamLoadId       = -1;  // slot currently being streamed into, or -1
    int streamLoadChannels = 0;   // 1 or 2
    int streamLoadCapacity = 0;   // allocated frames in samples[streamLoadId]
    int streamLoadFilled   = 0;   // frames written so far (becomes sampleLengths on finalize)

    // Replace the working buffers for `id` with a new left + optional right of length newLen, freeing the
    // old buffers. Keeps left/right and their shared length in lockstep so the stereo mix path can never
    // read a stale or short right channel. Pass newR=nullptr for a mono result.
    void setSampleBuffers(int id, float* newL, float* newR, int newLen);
    // Stop voices reading slot `id`'s buffers, then acquire sampleEditMutex. EVERY destructive
    // sample-editor op must hold the returned lock while mutating/freeing the slot's buffers so
    // the audio thread's try_lock fails (one silent block) instead of reading freed memory.
    std::unique_lock<std::mutex> beginSampleEdit(int id);
    // Apply a global EQ preset (0-127, <0 = bypass) to a live voice's inline EQ (EQN effect).
    void applyEqPresetToChain(InstrumentChain& chain, int slot);
    // Release one SoundFont slot. Order matters — detach the per-track voices FIRST (so the render
    // pass stops touching the slot), then close the handle under the slot mutex. The only place a
    // slot is ever freed: LRU eviction, unloadSoundfont and clearAllSoundfonts all route through it.
    void freeSoundfontSlot(int slot);
    InstrumentParams instrumentParams[256];
    InstrumentModSlot instrumentModSlots[256][4]; // [sampleId][slotIndex]
    // Per-instrument SF2 ADSR envelope override: stored keyed by instrument id
    // (always unique) and applied atomically in triggerNote, so two instruments sharing one de-duplicated
    // tsf handle never collide on the shared preset-region patch. -1 = keep the SF2 preset's own value.
    struct SfEnvOverride { int atk = -1, dec = -1, sus = -1, rel = -1; };
    SfEnvOverride sfEnvOverrides[256];

    Table tables[256];             // 256 tables, each with 16 rows
    std::mutex tableMutex;         // Protect table data during load/access

    NoteQueue noteQueue;             // Thread-safe queue of scheduled notes
    KillQueue killQueue;             // Thread-safe queue of scheduled kill events
    ParamUpdateQueue paramUpdateQueue; // Thread-safe queue of scheduled parameter updates
    // Per-block drain buffers: the audio callback empties each queue ONCE per block into
    // these (one lock each) instead of taking the queue mutex every frame. Reused across blocks so
    // the backing allocation persists (no per-block heap churn after warmup). Audio-thread-only.
    std::vector<ScheduledNote>        noteBatch;
    std::vector<ScheduledKill>        killBatch;
    std::vector<ScheduledParamUpdate> paramBatch;

    // Demand-driven visualizer capture (1.2 / 1.10): the UI read methods (getTrackWaveforms /
    // getSpectrumMagnitudes*) stamp these with the wall clock; the audio callback only does the
    // (expensive) OCTA accumulation / spectrum-ring writes when a read happened recently. No
    // Kotlin→C++ enable flag to keep in sync — capture simply follows actual demand, and stops
    // ~CAPTURE_IDLE_MS after the visualizer/EQ stops polling.
    static const int64_t CAPTURE_IDLE_MS = 250;
    std::atomic<int64_t> lastTrackWaveformReadMs{-CAPTURE_IDLE_MS};
    std::atomic<int64_t> lastSpectrumReadMs{-CAPTURE_IDLE_MS};
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // Written by the audio/render thread (processAudioBlock), read by the Kotlin scheduler via
    // getCurrentFrame() JNI — atomic (relaxed) makes that formally race-free at zero cost on arm64
    // and keeps the planned Linux port correct on unknown hardware.
    std::atomic<int64_t> globalFrameCounter{0};  // Total frames processed since start

    // Session entropy mixed into per-note RNG seeds (RND/DRNK LFO). Reseeded from the wall
    // clock at construction and at every resetFrameCounter() (= offline-render start): seeds
    // derived from the frame counter alone made repeated renders of the same song
    // bit-identical, because the counter resets to 0 for each render. Plain (non-atomic) is
    // fine — a torn read would just be different entropy.
    uint32_t noteSeedEntropy = 0x9E3779B9u;
    std::atomic<bool> isOfflineRendering{false};  // True during WAV export → processLiveBlock outputs silence
    std::atomic<int> currentTempo{120};  // Song BPM; read by the table-advance to derive framesPerTic
    int stemsMode = 0;  // 0=normal, 1-8=track stem, 9=reverb, 10=delay

    // Oscilloscope waveform buffer (circular buffer for recent output)
    static const int WAVEFORM_SIZE = 620;
    float waveformBuffer[WAVEFORM_SIZE];
    int waveformIndex = 0;
    std::mutex waveformMutex;

    // Spectrum capture buffers for EQ visualizer (per-context)
    static const int SPECTRUM_SIZE = 4096;
    float spectrumBuffer[SPECTRUM_SIZE];       // master left channel
    int   spectrumWriteIdx = 0;
    float delaySpectrumBuffer[SPECTRUM_SIZE];  // delay wet left
    int   delaySpectrumWriteIdx = 0;
    float reverbSpectrumBuffer[SPECTRUM_SIZE]; // reverb wet left
    int   reverbSpectrumWriteIdx = 0;
    float instrSpectrumBuffer[SPECTRUM_SIZE];  // single instrument (mono sum of all its voices)
    int   instrSpectrumWriteIdx = 0;
    std::atomic<int> instrSpectrumInstrId{-1}; // which instrId to monitor (-1 = none)
    std::mutex spectrumMutex;

    // Per-block per-track peaks: written by processAudioBlock, read by processLiveBlock for meters
    float framePeaksPerTrackL[8] = {0};
    float framePeaksPerTrackR[8] = {0};
    float frameSendPeakRevL = 0.0f, frameSendPeakRevR = 0.0f;
    float frameSendPeakDelL = 0.0f, frameSendPeakDelR = 0.0f;

    // Peak level tracking for mixer meters (stereo L/R per track)
    float trackPeaksL[8] = {0};
    float trackPeaksR[8] = {0};
    float masterPeakL = 0.0f;
    float masterPeakR = 0.0f;
    float sendPeakRevL = 0.0f, sendPeakRevR = 0.0f;
    float sendPeakDelL = 0.0f, sendPeakDelR = 0.0f;
    std::mutex peakMutex;
    static constexpr float PEAK_DECAY = 0.95f;  // Decay rate per callback (smooth falloff)

    // Real-time volume control (can be changed without rescheduling notes)
    float trackVolumes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float masterVolume = 1.0f;
    float reverbReturnGain  = 0.5f;
    float delayReturnGain   = 0.5f;
    float delayToReverbSend = 0.0f;
    std::mutex volumeMutex;

    // Send buses (reverb and delay)
    ReverbModule reverbSend;
    DelayModule  delaySend;
    MasterChain  masterChain;  // final output bus

    // EQ preset bank (128 slots; pre-converted from hex to Hz/dB/Q)
    struct EqPresetBank {
        EqBandData bands[3];
    } eqPresets[128];

    // Per-track waveform buffers for OCTA visualizer.
    // 8 song tracks + 1 dedicated preview lane (index PREVIEW_LANE): all previews — sampler,
    // sample, note, and SF instrument — play on PREVIEW_TRACK_ID (outside tracks 0-7), so
    // without their own lane they never appear on the per-track scopes.
    static const int PREVIEW_LANE = 8;
    static const int TRACK_WAVEFORM_COUNT = 9;  // 8 tracks + preview lane
    float trackWaveformBuffer[TRACK_WAVEFORM_COUNT][WAVEFORM_SIZE] = {};
    int   trackWaveformIndex = 0;
    bool  trackHasVoice[TRACK_WAVEFORM_COUNT] = {};

    // ── Per-block scratch for processAudioBlock (audio-thread-only) ──────────────────────────────
    // Engine members rather than audio-thread-stack locals (~116 KB per call) so a small-stack
    // real-time audio thread — e.g. a Linux ALSA/JACK callback — can't overflow. Safe as shared
    // members because processAudioBlock is never concurrent: processLiveBlock skips it during offline
    // render (isOfflineRendering gate) and the render thread is then its sole caller — the same
    // single-caller invariant the voices[]/framePeaks members rely on. Each is (re)initialised every
    // block; nothing persists across blocks.
    float revSendBufL[MAX_BLOCK], revSendBufR[MAX_BLOCK];   // panned reverb-send sum
    float dlySendBufL[MAX_BLOCK], dlySendBufR[MAX_BLOCK];   // panned delay-send sum
    float revWetL[MAX_BLOCK], revWetR[MAX_BLOCK];           // reverb wet output
    float dlyWetL[MAX_BLOCK], dlyWetR[MAX_BLOCK];           // delay wet output
    float instrSpectrumTempL[MAX_BLOCK];                   // mono sum of a monitored instrument's voices
    float sfBuf[MAX_BLOCK * 2];                             // per-track SF render (interleaved stereo)
    float trackWaveAccumL[TRACK_WAVEFORM_COUNT][MAX_BLOCK]; // OCTA per-track accumulators
    float trackWaveAccumR[TRACK_WAVEFORM_COUNT][MAX_BLOCK];
    bool  trackWasActive[TRACK_WAVEFORM_COUNT];             // OCTA: lane had a non-fading voice this block

    // Downsampling for oscilloscope (capture every Nth sample)
    // Lower = faster scrolling (more zoomed in), Higher = slower scrolling (more time visible)
    // Adjust this value to control oscilloscope speed:
    //   1 = 14ms visible (super fast), 10 = 140ms, 20 = 280ms, 50 = 700ms, etc.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // 1 = capture every sample (no downsampling)
    int waveformDownsampleCounter = 0;
};
