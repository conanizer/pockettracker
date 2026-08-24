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

/**
 * ⚠️ **THERE IS NO SIZE LIMIT ON A SOUNDFONT, AND A LIMIT ON `.sf3` ALONE WOULD BE THE WRONG SHAPE.**
 * This is written down because it looks like an obvious thing to add.
 *
 * The two formats are the same font with the sample data compressed or not, and they end at the same
 * float buffer. Measured on a 3000-sample font decoding to 410 MB of float:
 *
 *   | | file | load | peak RSS |
 *   |---|---|---|---|
 *   | `.sf2` | 205 MB | 1.24 s | **825 MB** |
 *   | `.sf3` |  14 MB | 3.60 s | **443 MB** |
 *
 * SF3 costs about 3× the load time and **half the peak memory**, because the SF2 path holds the whole
 * raw `smpl` chunk alongside the float buffer while SF3's raw chunk is a fourteenth of the size. So
 * capping SF3 by file size refuses the cheaper of the two and admits the more expensive one — and it
 * would do it at a threshold nothing can derive, since **nothing in an SF3 header states the decoded
 * size**: tsf only learns it by decoding.
 *
 * ⚠️⚠️ **AND NOTHING BOUNDS IT AT THE ALLOCATOR EITHER — DO NOT ASSUME A FAILED LOAD FAILS CLEANLY.**
 * tsf does null-check every allocation it makes, so the unwind path is real; what is missing is the
 * null. Measured: **bionic granted a 256 GB request on a 7.36 GB device**, so on Android there is no
 * size at which `malloc` refuses and `tsf_load` cannot return null for want of memory — the process
 * is killed on the write instead, with no message and no autosave. glibc does refuse, but only above
 * `MemTotal + SwapTotal`: it refuses against the size of the whole machine, never against what is
 * free. Only Windows (real commit accounting) and 32-bit armhf (3 GB of address space) fail cleanly.
 *
 * What bounds it instead is a check against the DEVICE's free memory, made where the memory actually
 * grows: a counting `TSF_MALLOC`/`TSF_REALLOC` for both font formats (`soundfont-voice.cpp`),
 * `decode_has_room()` for compressed samples (`audio-decoders.cpp`), and `load_budget_bytes()` up
 * front for WAV — the one source whose decoded size a header states. ⚠️ Deliberately NOT an estimate
 * made before opening the file: that would re-parse what the loaders already parse, and **no SF3
 * header carries the decoded size at all** — tsf only learns it by decoding. The limit is therefore
 * derived per-device from the file as it loads, which is why a fixed cap on one format is still the
 * wrong shape.
 *
 * ⚠️ **Also unsolved: the load is SYNCHRONOUS on the thread the UI is drawn from**, for both formats.
 * A big font freezes the picture for its load — over a second even for SF2 — and that is a threading
 * problem, not a format problem. A cap on one format was never the fix for that either.
 */

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    /**
     * Cache the backend's device sample rate. Set by the platform shell when the stream opens, then
     * read by getSampleRate() and the scheduler-thread pitch/tic math — so the core never has to
     * reach into a platform stream object for it.
     *
     * ⚠️ **It also re-initialises the send and master buses when the rate is not the one they were
     * built at**, and that is the whole reason this is not a one-line store. The reverb, delay,
     * master EQ, OTT and DUST all bake the sample rate into their coefficients at `reset(sr)`, and
     * the constructor has no device to ask — so a 48 kHz output would otherwise run five effect
     * modules computed for 44.1 kHz, with a synced delay writing 22050 samples where a beat is
     * 24000. (The render path escaped it: `prepare_render` resets at the real rate.)
     *
     * ⚠️ **The buses are at FACTORY defaults afterwards — the caller must re-push the project's FX.**
     * At boot the shell already does: the stream opens before the project is pushed, and
     * `push_params()` follows. Call this BEFORE the stream is unpaused; the audio callback must not
     * be running.
     */
    void setDeviceSampleRate(int sr);

    // False when the destination could not be allocated. The slot is left exactly as it was — the
    // buffers are taken before anything is freed — so a caller that reports LOAD FAILED is telling
    // the truth about a slot that still holds whatever it held before.
    bool loadSample(int id, const float* data, int length);
    bool loadSampleStereo(int id, const float* left, const float* right, int length);

    /**
     * Why the last media load failed. Every loader reports failure the same way — 0 or -1 — and a
     * file that is not a soundfont wants a different message from one the device cannot hold.
     *
     * ⚠️ Set by every load PATH, including the successful one (which clears it), so a stale value
     * cannot be read after a load that worked. Read immediately after the load that set it.
     */
    enum class LoadFailure { NONE = 0, PARSE, OUT_OF_MEMORY };
    LoadFailure lastLoadFailure() const { return lastLoadFailure_; }

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
    // Decode a compressed audio file (mp3/flac/ogg/opus, plus AAC in an ISO-BMFF container:
    // m4a/mp4/m4b/mov/3gp) natively into native sample memory — no Java heap, no MediaCodec. Dispatches
    // by file extension to dr_mp3 / dr_flac / stb_vorbis / libopus / (minimp4+FAAD2), then publishes via
    // the same slot path as loadSampleStereo. Returns the source sample rate (>0) on success, 0 on
    // failure (incl. unsupported extension). AAC containers are handled here now — nothing needs MediaCodec.
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

    // The playback window in FRAMES, for the one caller that has frames: the sample editor's audition.
    // Overrides the 0-255 start/end of the call above until the next setInstrumentParams, which clears
    // it. −1 (or an inverted pair) disarms. See InstrumentParams::startFrame.
    void setInstrumentFrameWindow(int instrumentId, int startFrame, int endFrame);

    void stopTrack(int trackId);
    void stopAll();

    // Platform hook: the audio shell installs a callback that restarts the output stream if the
    // platform paused it (Oboe today; ALSA/SDL on Linux). The Kotlin path called
    // backend.resumeStream() before every scheduled note; songcore's consumer calls requestResume()
    // for the same reason, without knowing what a stream is. Unset = no-op.
    std::function<void()> onResumeRequested;
    void requestResume() { if (onResumeRequested) onResumeRequested(); }

    // The ninth voice: every audition — sampler, sample, note, SF instrument — plays HERE rather than
    // on one of the 8 song tracks, which is what lets you hear a note you are dialling in without
    // stealing a voice from the song under it. Kotlin names the same lane PREVIEW_TRACK_ID.
    //
    // Public because a preview is scheduled from OUTSIDE the engine: SongcoreHost::preview_note has to
    // name the lane it plays on, and stop_preview the lane it kills. It was private while previews
    // were built in Kotlin and only ever *arrived* as a track id.
    static const int PREVIEW_LANE = 8;

    int getActiveVoiceCount();

    /**
     * For each of the 8 tracks, encode the active note as (octave * 12 + pitch), or -1 if no
     * voice is currently playing on that track. The caller passes a pre-allocated int[8] array.
     *
     * ⚠️ BOTH voice pools — the sampler `voices[]` AND `sfVoices[]`. A track playing a SoundFont
     * instrument has no entry in the first one at all.
     */
    void getTrackActiveNotes(int* out, int trackCount);

    int getSampleRate();

    // ===================================
    // SAMPLE EDITOR OPERATIONS
    // ===================================
    int   getSampleLength(int id);

    /**
     * Every byte of audio the engine is holding, for the USED RAM readout.
     *
     * ⚠️ IT LIVES HERE BECAUSE THE BUFFERS DO. A caller outside the engine can only reach the playable
     * pool, through getSampleLength() — it cannot see the undo buffers, the RATE-HIGH originals, the
     * FX-preview copy, the sample clipboard or a SoundFont's PCM, which together can be several times
     * the pool while a sample is being edited. Summing it at the call site meant reporting a quarter of
     * the truth and calling it a total. **A buffer added to this class must be added to this function**,
     * and it is the only place that can be true of.
     *
     * ⚠️ Deliberately takes no lock. It reads pointers and lengths the edit path swaps under
     * `sampleEditMutex`, so a value can be one frame stale — which is the correct trade for a number
     * drawn 60 times a second, and the reason it must never be used for an allocation decision.
     */
    int64_t audio_memory_bytes() const;

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
    // Destructive whole-sample DSP: fxType 0=OTT, 1=DUST, 2=DRIVE, 3=EQ.
    // ⚠️ `fxValue` is 0-255 for the first three (the effect's amount) but an EQ PRESET SLOT for type 3
    // — a different quantity in the same parameter, which is why the type has to be read first.
    void applySampleFx(int id, int fxType, int fxValue, float sampleRate, int limiterPreGain = 0);
    // Zero-crossing search near `frame`. dir>0 = forward only, dir<0 = backward only, dir==0 = nearest
    // (both ways); returns `frame` if none within searchRadius. Directional keeps marker snapping
    // monotonic so a small move can't snap back and stick.
    // `sourceMode` is the sample editor's SOURCE (0 LEFT, 1 RIGHT, 2 STEREO, 3 MONO) — the signal the
    // cut will actually be made in. STEREO scores candidates by their worst channel; see the body.
    int  findZeroCrossing(int id, int frame, int dir = 0, int searchRadius = 512, int sourceMode = 0);
    // Spectral-flux transient detection. Returns count; outMarkers[] filled with frame positions.
    // sensitivity 0x00 = few markers (high threshold), 0xFF = many markers (low threshold).
    int  detectTransients(int id, int sensitivity, int* outMarkers, int maxMarkers);

    // ===================================
    // CORE AUDIO PROCESSING BLOCK
    // ===================================
    // ALL REAL-TIME audio DSP lives here. processLiveBlock and renderOffline are thin wrappers.
    // Rule: NEVER add audio processing logic directly to processLiveBlock or renderOffline.
    //
    // ⚠️ "Real-time" is the whole of the claim, and the word is load-bearing. There is OFFLINE DSP
    // elsewhere — sample-editor.cpp runs OTT, DUST, DRIVE, the EQ, the limiter, a SOLA time-stretch
    // and a resampler on the UI thread, and transient-detector.cpp and computeSpectrumFFT run FFTs.
    // Read as "all DSP, anywhere", this line hides them: anything that protects the audio thread
    // (flush-to-zero, for one) has to be armed on those paths separately, and they do arm it.
    void processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // ===================================
    // LIVE BLOCK ENTRY (called by the platform backend's audio callback)
    // ===================================
    // The platform shell (OboeAudioEngine::onAudioReady) hands its raw output buffer here. This does
    // everything the old onAudioReady did MINUS the Oboe glue: flush-to-zero, clear the buffer, bail to
    // silence during offline render, chunk into PROCESS_SUBBLOCK processAudioBlock calls, then capture the
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

    // Schedule a KEY release — a live MIDI-in note let go of (MIDI plan §4.1, phase E4).
    // ⚠️ NOT the same thing as scheduleNoteOff, and the difference is the whole reason it exists: a
    // one-shot sample with no release envelope IGNORES this and plays out (a drum hit finishes), where
    // a KIL fades it. ADSR/TRIG release and the looping soft-kill are identical in both.
    void scheduleKeyRelease(int64_t targetFrame, int trackId);

    // Clear all scheduled notes
    void clearScheduledNotes();

    // Clear only notes/kills at or after fromFrame (leaves the current phrase intact).
    // ⚠️ `trackId >= 0` clears ONE track's — the eight song cursors roll back independently, so a
    // live edit must drop only what the track being rolled back is going to schedule again.
    void clearScheduledNotesFrom(int64_t fromFrame, int trackId = -1);

    // Load table data from Kotlin
    // rowData format: 16 rows × 8 bytes = 128 bytes
    // Each row: [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value, fx3Type, fx3Value]
    void loadTable(int tableId, const uint8_t* rowData);

    // Get current table row for a voice (for UI feedback)
    // Where each of the table's three column playheads stands on this track, or −1 for a column that
    // is not running one. Three markers on the TABLE screen; see ui/engine_feed.h.
    void getVoiceTableRows(int trackId, int out[TABLE_LANES]);

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
    void scheduleVoiceFilterCut(int64_t targetFrame, int trackId, float cut);          // CUT xx
    void scheduleVoiceFilterRes(int64_t targetFrame, int trackId, float res);          // RES xx
    void scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot);              // EQN xx
    void scheduleMasterEqSlot(int64_t targetFrame, int slot);                          // EQM xx
    // An AUS/AUF EQ morph tick. Not a slot: the bands are carried verbatim, because the setting a
    // morph names is between two presets and need not be one any slot holds.
    void scheduleVoiceEqBands(int64_t targetFrame, int trackId, const EqBandsHex& bands);
    void scheduleMasterEqBands(int64_t targetFrame, const EqBandsHex& bands);
    void scheduleTrackVolume(int64_t targetFrame, int trackId, float volume);          // VTR xx
    void scheduleMasterVolume(int64_t targetFrame, float volume);                      // VMV xx

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

    // MUTE the track, independently of its fader — voices already ringing go silent within one block.
    // "Inaudible" is the caller's word: songcore folds solo into it (songcore/model.h track_audible).
    void setTrackMuted(int trackId, bool muted);

    // Set real-time master volume (affects playback immediately)
    void setMasterVolume(float volume);

    // Route the preview lane through a mixer channel's fader: 0..7, or -1 for unity gain.
    void setPreviewTrack(int trackId);

    // The same two writes with no log line — what the VTR/VMV queue arms call from the audio thread.
    // See the comment above their definitions for why the split exists.
    void applyTrackVolume(int trackId, float volume);
    void applyMasterVolume(float volume);

    // ===================================
    // EQ METHODS
    // ===================================

    // Set one band of an EQ preset slot (hex params converted to Hz/dB/Q internally).
    // slot: 0-127, band: 0-2, type: 0=OFF 1=LOSHELF 2=LOWCUT 3=BELL 4=HISHELF 5=HICUT
    // ⚠️ A band type's NUMBER is its identity — it is stored in the project file, passed here verbatim
    // and branched on by EqBandModule::setParams. LOWCUT and HICUT were appended, so the members are
    // NOT in the order a reader would guess from the names. The one definition is eq-module.h; the
    // UI's list at eq_editor.cpp agrees with it. Append, never insert.
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

    /**
     * Did a TABLE row's EQM move the master bus since this was last asked? Reading CLEARS it.
     *
     * ⚠️ THE HOST'S "PUT THE MIXER'S EQ BACK ON STOP" IS DRIVEN BY THE SCHEDULER, WHICH A TABLE ROW
     * NEVER REACHES — a table runs inside this class, per voice, off the audio thread. So the two
     * ways an EQM can be authored arm the same restore from opposite sides of the seam, and this is
     * the engine's half (songcore/host.h stop(), beside `Sequencer::eqm_active()`).
     */
    bool takeTableMasterEqTouched() { return tableMasterEqTouched.exchange(false, std::memory_order_relaxed); }

    // The same question WITHOUT consuming the answer, for a caller that wants to know whether the bus
    // is currently overridden rather than to discharge the restore. ⚠️ Anyone asking mid-take must use
    // this one: `take` would disarm stop(), and the bus would then keep the table's preset forever.
    bool tableMasterEqTouchedPeek() const { return tableMasterEqTouched.load(std::memory_order_relaxed); }

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
    // note trigger — per COLUMN, one rate per playhead. Shared by the sampler and SF dispatch
    // paths (audio thread only).
    void effectiveTicRatesFor(int tableId, int fallback, int out[TABLE_LANES]);

    // Unified per-voice table tick (tic advance + row FX processing) for sampler AND SF
    // voices — was two drifted ~90-line copies. Duck-typed template over the identical
    // table-state fields; the two per-type differences (KIL semantics, OFFSET) resolve at
    // compile time via the tableKill/tableOffset overloads in audio-engine.cpp, where the
    // template is defined and (implicitly) instantiated.
    template <typename V> void processTableTick(V& voice, int numFrames, float sampleRate);

    // The row half of the above, for ONE column's playhead: lane 0 also carries transpose and
    // volume, and every lane carries its own FX slot. Applied on the blocks where that lane
    // actually consumes a row. Split out so that processTableTick has ONE exit and the AUS/AUF ramp
    // below it runs on EVERY block — the row work returning early is what used to end the tick, and
    // a ramp that only moved on a row change would be sixteen values instead of a fade.
    template <typename V> void processTableRow(V& voice, const TableRow& row, int lane,
                                               bool shouldAdvance, float sampleRate);

    // The AUS/AUF ramps a table declares, applied to one voice at the position it is standing on.
    // ⚠️ AFTER the row's own effects, never before: on the AUS row the ramp is at t=0, so it writes
    // the same value the cell to its left just wrote, and on every later row the fade is the thing
    // that should win over a stale per-row cell.
    // ⚠️ `rowFraction` is per COLUMN, and each ramp reads the column its PARAMETER cell is in — see
    // the derivation at the definition.
    template <typename V> void applyTableRamps(V& voice, const TableRow* rows,
                                               const double (&rowFraction)[TABLE_LANES],
                                               float sampleRate);

    // Smart note-off: trigger ADSR/TRIG release if available, otherwise hard-stop.
    void triggerNoteOff(int trackId);

    // The same, for a live KEY that was let go of — a one-shot ignores it (MIDI plan §4.1).
    void triggerKeyRelease(int trackId);

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
    /** Backs lastLoadFailure(). Written by every load path, including the ones that succeed. */
    LoadFailure lastLoadFailure_ = LoadFailure::NONE;

    /**
     * Flush-to-Zero: the engine's denormal protection, and not any compile flag.
     *
     * ⚠️ **PER-THREAD, because FPCR/FPSCR/MXCSR are thread-local registers** — so it is not enough
     * that "the audio thread arms it". EVERY entry point that runs DSP must call it first, on
     * whatever thread it happens to be on. That includes the offline sample-editor operations, which
     * run the same OTT, DUST, EQ and limiter code on the UI thread: a fade-out tail decays smoothly
     * into the denormal range and stays there, at 10-100x the per-sample cost, which on a handheld is
     * a frozen screen with no progress indication.
     *
     * Repeat calls are free — a `thread_local` flag makes it a no-op after the first.
     */
    static void setFlushToZeroForCurrentThread();

    // ⚠️ THE GRANULARITY AT WHICH EVENTS ARE RESOLVED, and the size of every per-block buffer.
    // The two are ONE constant on purpose: the correctness limit is the tighter of the pair, so a
    // separate, larger buffer cap could only ever describe a call nothing is allowed to make.
    //
    // The correctness half. processAudioBlock applies a block's note-ons inside one pass: each retrigger
    // fades the previous same-track voice and takes a new slot, but a faded voice only frees up as
    // it is MIXED. So when several same-track retriggers land in ONE block they pile up slots,
    // exhaust the voice pool, and get recycled — i.e. notes are silently DROPPED.
    //
    // That makes the audible result a function of the caller's block size, which is exactly the
    // Phase-4 "retrig sounds different on the device" bug: the Flip's ALSA period is 940 frames and
    // an RPT 01 at 128 BPM retriggers every ~430, so 2+ retriggers shared every block. Measured on
    // the user's REPEAT_TEST over 20 s (tools/blocktest): 128 → 4454 onsets/44 gaps, 256 →
    // 4457/53, 512 → 4158/277, 940 → 3680/421 (17% of the retriggers gone, gaps up to 26 ms).
    // renderOffline always chunked at 256, which is why the EXPORT was correct while live playback
    // pulsed. Both paths now chunk here, so what you hear is what you export.
    //
    // The buffer half. Every per-block scratch member below is a fixed array of this many frames,
    // indexed by `numFrames`, so processAudioBlock rejects a larger block rather than overrun them.
    // Both shipped wrappers chunk here and nothing else calls it — the reader who will is the future
    // ALSA/JACK backend this header invites, and that reader calls processAudioBlock directly.
    static constexpr int PROCESS_SUBBLOCK = 256;

    // Device output sample rate, cached from the platform backend (see setDeviceSampleRate). Defaults to
    // 44100 so getSampleRate()/pitch math stay correct if read before the stream opens — matches every
    // other 44100 fallback in the engine. Atomic: written by the shell thread, read by the scheduler.
    std::atomic<int> deviceSampleRate{44100};

    // The rate the send and master buses were last built at. Only setDeviceSampleRate touches it, and
    // only to notice that a re-init is owed — the coefficients are the buses' own, not readable back.
    int effectsSampleRate = 44100;

    Voice voices[MAX_VOICES];

    // ⚠️ The TIC00 table cursor, per TRACK — where the track's table stands once no voice is left to
    // carry it. At TIC00 the table advances per NOTE, so the cursor has to outlive any one voice: a
    // one-shot whose sample runs out before the next note leaves nothing to read the row off. Both the
    // audible row and the TABLE screen's indicator then depend on the instrument's ROOT note — root
    // sets playback rate, rate sets how long the sample lasts — which is not a property of the table.
    //
    // It mirrors the VOICE's pair rather than any one answer derived from it, so the two consumers
    // (the next retrigger's start row; the row the indicator shows) derive what they need the same way
    // they do from a live voice. Written in ONE place — the sampler table-tick loop, below the row
    // logic. stopAll() clears it, so PLAY, and every render, begins at row 0.
    //
    // ⚠️ **PER COLUMN, because the columns advance separately.** Only a column actually at TIC00
    // carries over; the entry is written for all three so the two consumers read the same shape they
    // would from a live voice, and each of them checks that column's own rate before using it.
    struct Tic00Cursor {
        int tableId       = -1;  // -1 = no TIC00 table running on this track; the next note starts at row 0
        int  row[TABLE_LANES]           = {0, 0, 0};    // each column's row — where its table stands
        int  lastProcessed[TABLE_LANES] = {-1, -1, -1}; // …and whether that row has been applied yet
        int  ticRate[TABLE_LANES]       = {6, 6, 6};    // which columns were at TIC00 and may carry over
        // ⚠️ …and which had already executed HOP FF. Without it a column that stopped before its voice
        // ended would both draw a marker on the row it stopped at and resume there on the next note.
        bool active[TABLE_LANES]        = {true, true, true};
    };
    Tic00Cursor tic00Cursor[SF_VOICE_COUNT];

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
    // Apply a global EQ preset (0-127, <0 = bypass) to any EQ — a live voice's inline EQ (EQN), the
    // master bus (EQM), or a send's input EQ. The one place a slot becomes band params.
    void applyEqPresetToModule(EqModule& eq, int slot);
    // The same write, from band VALUES rather than a slot — an AUS/AUF morph tick (EqBandsHex).
    void applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands);
    // Release one SoundFont slot. Order matters — detach the per-track voices FIRST (so the render
    // pass stops touching the slot), then close the handle under the slot mutex. The only place a
    // slot is ever freed: LRU eviction, unloadSoundfont and clearAllSoundfonts all route through it.
    void freeSoundfontSlot(int slot);
    InstrumentParams instrumentParams[256];
    InstrumentModSlot instrumentModSlots[256][4]; // [sampleId][slotIndex]
    // Per-instrument SF2 ADSR envelope override: stored keyed by instrument id
    // (always unique) and applied atomically in fireArmedNote, so two instruments sharing one de-duplicated
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
    // Set by a table row's EQM, consumed by takeTableMasterEqTouched(). Audio thread writes,
    // UI thread reads — atomic for that reason and no other; it is a one-way latch.
    std::atomic<bool> tableMasterEqTouched{false};
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
    // ⚠️ A SEPARATE GATE, NOT A FADER VALUE. Folding the mute into trackVolumes would make a VTR
    // ramp — which writes the fader from the audio thread — un-mute the track it lands on. The two
    // stay independent and are multiplied where the block snapshots them.
    bool  trackMuted[8] = {false, false, false, false, false, false, false, false};
    // Where the gate has actually GOT TO, chasing trackMuted at MUTE_GATE_SAMPLES per full swing.
    // ⚠️ AUDIO THREAD ONLY — it is advanced once per block inside processAudioBlock and read nowhere
    // else, which is why it needs no atomic and no lock of its own. Starts open: an engine that has
    // never been told about a mute must not fade its first block in.
    float trackGate[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // Which of those eight the preview lane borrows, or -1 for unity. An INDEX, not a gain: the
    // snapshot below re-reads the live fader every block, so a VTR or a mixer move is heard in the
    // audition it is aimed at. Written by the UI thread, read once per block under volumeMutex.
    int   previewLaneTrack = -1;
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
    // ⚠️ THE SAME 128 PRESETS AS AUTHORED HEX, and the bank above cannot answer for them. A morph
    // interpolates the AUTHORED bytes — that is what makes a frequency sweep linear in log-frequency
    // — and Hz/dB/Q is a one-way conversion: interpolating those instead walks a different path
    // between the same two presets. The phrase path never needed this because the morph happens
    // above the seam, where the model still has the hex; a TABLE morph happens here.
    //
    // Both arrays are written in exactly one place, `setEqBand`, and always together.
    EqBandsHex eqPresetHex[128];

    // Per-track waveform buffers for OCTA visualizer.
    // 8 song tracks + 1 dedicated preview lane (index PREVIEW_LANE, declared public above): all
    // previews — sampler, sample, note, and SF instrument — play outside tracks 0-7, so without their
    // own lane they would never appear on the per-track scopes.
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
    float revSendBufL[PROCESS_SUBBLOCK], revSendBufR[PROCESS_SUBBLOCK];   // panned reverb-send sum
    float dlySendBufL[PROCESS_SUBBLOCK], dlySendBufR[PROCESS_SUBBLOCK];   // panned delay-send sum
    float revWetL[PROCESS_SUBBLOCK], revWetR[PROCESS_SUBBLOCK];           // reverb wet output
    float dlyWetL[PROCESS_SUBBLOCK], dlyWetR[PROCESS_SUBBLOCK];           // delay wet output
    float instrSpectrumTempL[PROCESS_SUBBLOCK];                           // mono sum of a monitored instrument's voices
    float sfBuf[PROCESS_SUBBLOCK * 2];                                    // per-track SF render (interleaved stereo)
    float trackWaveAccumL[TRACK_WAVEFORM_COUNT][PROCESS_SUBBLOCK];        // OCTA per-track accumulators
    float trackWaveAccumR[TRACK_WAVEFORM_COUNT][PROCESS_SUBBLOCK];
    bool  trackWasActive[TRACK_WAVEFORM_COUNT];             // OCTA: lane had a non-fading voice this block

    // Downsampling for oscilloscope (capture every Nth sample)
    // Lower = faster scrolling (more zoomed in), Higher = slower scrolling (more time visible)
    // Adjust this value to control oscilloscope speed:
    //   1 = 14ms visible (super fast), 10 = 140ms, 20 = 280ms, 50 = 700ms, etc.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // 1 = capture every sample (no downsampling)
    int waveformDownsampleCounter = 0;
};
