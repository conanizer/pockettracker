#pragma once
// ───────────────────────────────────────────────────────────────────────────────────────────────
// PORTABLE AUDIO CORE — no platform dependencies: voices, note scheduling, the sample-accurate
// queues and ALL DSP (processAudioBlock). Backends (oboe-audio-engine, shell/sdl-audio-engine) call
// processLiveBlock(); do NOT add <oboe/*> or <android/*> here — logging goes through audio-defs.h.
// ───────────────────────────────────────────────────────────────────────────────────────────────
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
#include "songcore/program.h"   // Program / ProgramTable — an instrument as the numbers a note needs
#include "songcore/seqlock.h"   // SeqPublisher — the bus settings, published to the audio thread
#include "sampler-voice.h"
#include "soundfont-voice.h"
#include "soundfont-trim.h"
#include "effects/send-chain.h"
#include "effects/master-chain.h"

// Per-track soundfont voice state (shares soundfonts[sfSlot].handle via MIDI channels): song tracks
// 0-7 plus the preview lane (8 == AudioEngine::PREVIEW_LANE). Defined in audio-engine.cpp.
static const int SF_VOICE_COUNT = 9;
extern SoundfontVoice sfVoices[SF_VOICE_COUNT];

/**
 * ⚠️ THERE IS NO SIZE LIMIT ON A SOUNDFONT, AND A LIMIT ON `.sf3` ALONE WOULD BE THE WRONG SHAPE.
 * The two formats decode to the same float buffer; SF3 loads ~3× slower but peaks at about HALF the
 * memory (SF2 holds its raw `smpl` chunk beside the floats). And no SF3 header states its decoded
 * size, so no threshold could be derived.
 *
 * ⚠️⚠️ NOR DOES THE ALLOCATOR BOUND IT (platform_memory.h): on Android a too-big load is killed on
 * the write, not refused. What bounds it is a check against the DEVICE's free memory where memory
 * grows: the counting TSF_MALLOC/TSF_REALLOC (soundfont-voice.cpp), `PcmSink::append` for compressed
 * samples (audio-decoders.cpp), and `load_budget_bytes()` up front for WAV, whose header states it.
 *
 * ⚠️ A load the user is WATCHING (opening a project, applying a preset) is synchronous on the
 * drawing thread and reports through `load_progress.h`. The PATCH row's is not; see
 * `requestSoundfontLoad`.
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
     * ⚠️ It also RE-INITIALISES the send and master buses when the rate differs from the one they
     * were built at: reverb, delay, master EQ, OTT and DUST bake the rate in at `reset(sr)`, and the
     * constructor has no device to ask.
     *
     * ⚠️ The buses are at FACTORY defaults afterwards — the caller must re-push the project's FX (at
     * boot `push_params()` follows the stream open). Call BEFORE the stream is unpaused.
     */
    void setDeviceSampleRate(int sr);

    // False when the destination could not be allocated. The slot is left exactly as it was — the
    // buffers are taken before anything is freed — so a caller that reports LOAD FAILED is telling
    // the truth about a slot that still holds whatever it held before.
    bool loadSample(int id, const float* data, int length);

    /**
     * Why the last media load failed. Every loader reports failure the same way — 0 or -1 — and a
     * file that is not a soundfont wants a different message from one the device cannot hold.
     *
     * ⚠️ Set by every load PATH, including the successful one (which clears it), so a stale value
     * cannot be read after a load that worked. Read immediately after the load that set it.
     */
    // ⚠️ APPEND ONLY. CANCELLED is not an error and the UI must not draw it as one — the user stopped
    // the load on purpose and already knows why nothing arrived.
    enum class LoadFailure { NONE = 0, PARSE, OUT_OF_MEMORY, CANCELLED };
    LoadFailure lastLoadFailure() const { return lastLoadFailure_; }

    // Decode a WAV file straight into native sample memory: 8/16/24/32-bit PCM, 32-bit float,
    // mono/stereo, WAVE_FORMAT_EXTENSIBLE. Returns the WAV sample rate (>0), 0 on failure.
    int loadSampleFromWavFile(int id, const char* path);
    // Decode a compressed audio file (mp3/flac/ogg/opus, AAC in m4a/mp4/m4b/mov/3gp) into native
    // sample memory, by extension (audio-decoders.h). Returns the source rate (>0), 0 on failure or
    // an unsupported extension.
    int loadSampleFromCompressed(int id, const char* path);
    bool hasStereoData(int id);
    // The bit depth the slot's audio came in at: 8/16/24/32 for a WAV, the STREAMINFO depth for a FLAC,
    // and 16 for everything else (a lossy decode has no depth of its own). The sample editor's BIT cell
    // offers this and lower, and SAVE writes it. `isSampleFloat` is true only for a 32-bit float WAV.
    int  getSampleBitDepth(int id) const;
    bool isSampleFloat(int id) const;
    // After SAVE: the buffer IS the file now. Record the depth it was written at, and drop the RATE/BIT
    // original — it describes audio that is no longer the sample's starting point.
    void adoptSavedSampleFormat(int id, int bits, bool isFloat);
    void clearAllSamples();
    // Free all buffers for a single slot (used when a slot is repurposed, e.g. sampler → SoundFont).
    void clearSample(int id);

    // ===================================
    // SOUNDFONT BANK — SF2 files → the shared tsf handles the SF voices render through
    // ===================================
    // ⭐ ONE PRESET IS LOADED, NOT THE WHOLE FILE: it is cut out into a small in-memory SoundFont
    // (soundfont-trim.h), so a 200 MB bank costs the megabytes of the one sound in use. A file that
    // cannot be cut apart loads whole.
    //
    // MAX_SOUNDFONTS slots, keyed by (path, bank, preset): the same sound twice shares a slot
    // (instruments play on distinct MIDI channels with per-note ADSR overrides), and one sound too
    // many evicts the least-recently-used.
    int  loadSoundfont(int instrumentId, const char* path, int bank, int preset);   // → slot, or -1
    void unloadSoundfont(int slot);
    void clearAllSoundfonts();

    // ── the same load, off the drawing thread ─────────────────────────────────────────────────────
    //
    // ⚠️⚠️ A PRESET IS A DECODE, AND A DECODE DOES NOT FIT IN A FRAME (a compressed preset can take
    // hundreds of ms). So the decode runs on a worker; choosing the slot and publishing the pointer
    // stay on the thread that owns the slot table. The instrument keeps playing its previous sound.
    //
    // ⚠️ ONE worker, no queue: a request while one is in flight is REFUSED and the polling caller
    // asks again next frame — a queue would only keep presets already scrolled past.
    enum class SfRequest {
        READY,      ///< already resident — `readySlot` is it, and nothing was started
        STARTED,    ///< the worker has it; call collectSoundfontLoad until it comes back
        BUSY,       ///< a different load is in flight; ask again
    };
    SfRequest requestSoundfontLoad(int instrumentId, const char* path, int bank, int preset,
                                   int* readySlot);

    /**
     * Install a finished background load, if there is one.
     *
     * Returns false when nothing has finished. On true, `instrumentId` is who asked and `slot` is
     * where it landed — or −1 if the load failed, which the caller reports exactly as a synchronous
     * failure. ⚠️ **The caller must re-check that the instrument still wants this sound**: the row
     * may have moved on while it decoded, and then the slot is a spare nobody names.
     */
    bool collectSoundfontLoad(int* instrumentId, int* slot);

    /** True while a background load is in flight. */
    bool soundfontLoadPending() const;

    /** True when `slot` is loaded and holds exactly this sound — what a caller checks before reloading. */
    bool soundfontSlotHolds(int slot, const char* path, int bank, int preset);

    /**
     * What `slot` is holding — false, and the outputs untouched, when it is empty.
     *
     * The only way to ask whether a slot is OCCUPIED, and by what, without naming a sound first:
     * `soundfontSlotHolds` can only confirm a guess. That question has no answer on screen and none
     * in the audio, which is what a slot silently left resident depends on.
     */
    bool soundfontSlotSound(int slot, std::string& path, int& bank, int& preset);

    /** How many SoundFont slots there are, so a caller can walk them without knowing the constant. */
    int soundfontSlotCount() const;


    // ── the FILE's preset list, which a loaded slot can no longer answer ──────────────────────────
    //
    // ⚠️ A slot holds ONE preset now, so "what else is in this file?" cannot be asked of the handle.
    // These read the file's index directly — a few kilobytes, no sample data — which is also what lets
    // the PATCH row list a bank far too large to load, and lets it list one before anything is loaded.
    // Cached by path, because the answer only changes when the file does.
    int  getSoundfontFilePresetCount(const char* path);
    bool getSoundfontFilePresetAt(const char* path, int index, int* bank, int* presetNumber);
    std::string getSoundfontFilePresetName(const char* path, int bank, int preset);

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt, int loopEn,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes);

    // The playback window in FRAMES, for the one caller that has frames: the sample editor's audition.
    // Overrides the 0-255 start/end of the call above until the next setInstrumentParams, which clears
    // it. −1 (or an inverted pair) disarms. See InstrumentParams::startFrame.
    void setInstrumentFrameWindow(int instrumentId, int startFrame, int endFrame);

    /**
     * End every sounding voice IMMEDIATELY. Nobody is listening when this is called: the render path
     * runs it to clear the previous take out of the engine before it schedules, and the whole point
     * there is that no audio from before the call may reach the output. The transport uses
     * stopAllRamped() instead.
     *
     * ⚠️ It writes the voices directly, so only the thread that owns them may call it: the audio
     * thread, or a render while the device is paused.
     */
    void stopAll();

    /**
     * The STOP BUTTON's version — the same end state, reached over KILL_FADE_SAMPLES instead of in
     * one sample. A sustained note ended where its waveform happened to be is a full-scale step, and
     * the master bus obligingly carries it: OTT and DUST both compress, which lifts it further.
     *
     * Both pools ramp, and each ramp sits where that pool's audio is already fully formed — after the
     * instrument's filter and above the reverb/delay send tap — so the tails are fed a signal that
     * fades rather than one that stops mid-cycle and rings the click on for seconds.
     *
     * ⚠️ A REQUEST: the audio thread starts the ramp at the top of its next block and finishes it, so
     * the voices are still sounding when this returns. It touches no voice, so the UI may call it
     * while a block runs. Anything that must be silent now wants stopAll().
     */
    void stopAllRamped();

    /**
     * A take is starting — every play verb calls this BEFORE it schedules, so the audio thread sees
     * the request no later than the take's first note. A take that follows a stop starts OTT afresh
     * (OttModule::restart): what OTT holds after a stop is the previous take's fading tail.
     * A request, taken at the top of the next block, like stopAllRamped().
     */
    void startTake();

    // Platform hook: restarts the output stream if the platform paused it. songcore's consumer
    // calls requestResume() before every scheduled note. Unset = no-op.
    std::function<void()> onResumeRequested;
    void requestResume() { if (onResumeRequested) onResumeRequested(); }

    /**
     * Live input — a MIDI keyboard — drained on the AUDIO thread.
     *
     * The source is asked once per live block, at the top of processAudioBlock, before the queues
     * drain: what it schedules at the block's first frame is picked up by that same drain and sounds
     * in the block it arrived in. While an export owns the engine it is asked to discard instead,
     * so bytes that arrive during a render are not saved up to fire as a burst afterwards.
     *
     * ⚠️ Both calls run on the audio thread and must obey its rules (the host's implementation is
     * songcore's MidiInPipeline, which was written for it). ⚠️ Set before the stream opens and
     * cleared after it closes: the pointer is read by a callback that a close waits for, and by
     * nothing else.
     */
    struct LiveInputSource {
        virtual ~LiveInputSource() = default;
        virtual void drainLiveInput(int64_t blockStartFrame) = 0;
        virtual void discardLiveInput() = 0;
    };
    void setLiveInput(LiveInputSource* source) { liveInput.store(source, std::memory_order_release); }

    /** The tempo the engine is running at (setTempo). A live note is scheduled at this one. */
    int tempo() const { return currentTempo.load(std::memory_order_relaxed); }

    // The ninth voice: every audition plays HERE rather than on a song track, so a note being
    // dialled in never steals a voice from the song. Public because SongcoreHost::preview_note and
    // stop_preview name the lane.
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
     * ⚠️ IT LIVES HERE BECAUSE THE BUFFERS DO: undo buffers, RATE-HIGH originals, the FX-preview
     * copy, the clipboard and SoundFont PCM are invisible outside, and can be several times the pool
     * while editing. A buffer added to this class must be added to this function.
     *
     * ⚠️ No lock: it can be a frame stale — fine for a readout, never for an allocation decision.
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
    // The sample editor's RATE and BIT cells: derives the buffer from the cached original, decimated by
    // `factor` (1=HIGH, 2=NORM, 4=LOFI) and then quantised to `bits` (32, 24, 16 or 8, never above the
    // sample's own depth). One derive for both, so changing either never discards the other. (1, the
    // sample's own depth) restores the original.
    void applyRateAndBits(int id, int factor, int bits);
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
    // ⚠️ REAL-TIME DSP: sample-editor.cpp, transient-detector.cpp and computeSpectrumFFT run offline
    // DSP on the UI thread, and anything protecting the audio thread (flush-to-zero) must be armed
    // there too.
    void processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // ===================================
    // LIVE BLOCK ENTRY (called by the platform backend's audio callback)
    // ===================================
    // The backend's callback hands its output buffer here: flush-to-zero, clear, silence during an
    // offline render, chunk into PROCESS_SUBBLOCK processAudioBlock calls, capture scopes and peaks.
    void processLiveBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // What the live callback costs, over the last second of audio. A load is the time processLiveBlock
    // took over the time its block of sound lasts, in tenths of a percent: 1000 means the callback
    // needed as long as the sound it made, and the device is about to run dry. All zero until the
    // device has played a second. Any thread; the four fields may come from adjacent seconds.
    struct BlockTiming { int blockFrames = 0; int sampleRate = 0; int meanLoad = 0; int worstLoad = 0; };
    BlockTiming getBlockTiming() const;

    // The global frame counter, for scheduling.
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

    /**
     * Schedule a note by INSTRUMENT NUMBER, leaving what it sounds like undecided.
     *
     * ⚠️ The sequencer's path; the difference from scheduleNote() is WHEN. That one is handed a
     * sound; this one a number, resolved against the program table at the trigger — the only moment
     * a table row could have changed it. `tempo` is the tempo at SCHEDULE time (PSL/vibrato scale).
     */
    void scheduleProgramNote(int64_t targetFrame, int trackId, int instrumentId,
                             const songcore::NoteOnPayload& noteOn, int tempo,
                             bool rootAudition = false);

    // Store a per-instrument SF2 ADSR override. Keyed by instrument id and
    // applied atomically at note trigger, so instruments sharing a de-duplicated handle don't clash.
    void setSoundfontEnvelopeOverride(int instrumentId, int atk, int dec, int sus, int rel);

    // Schedule a kill event (for Kill effect K00)
    void scheduleKill(int64_t targetFrame, int trackId);

    // Schedule a soft note-off (triggers ADSR release instead of hard stop)
    void scheduleNoteOff(int64_t targetFrame, int trackId);

    // Schedule a KEY release — a live MIDI-in note let go of. ⚠️ Unlike scheduleNoteOff, a one-shot
    // with no release envelope IGNORES it and plays out; ADSR/TRIG release and the loop soft-kill match.
    void scheduleKeyRelease(int64_t targetFrame, int trackId);

    // Schedule a CUT: every voice on the track fades out over KILL_FADE_SAMPLES and ends, whatever
    // release the instrument has — a SoundFont's REL tail and an ADSR release included.
    void scheduleCut(int64_t targetFrame, int trackId);

    // End every track at once — what the END OF A RENDER'S RANGE does to the notes still sounding
    // (songcore::render_to_wav).
    // ⚠️ A KIL, NOT A KEY RELEASE: after the last row must come the TAILS only. A release envelope
    // (sampler ADSR/TRIG, every SoundFont preset) fades by its own release; everything else, a
    // one-shot included, gets the declicked fade.
    void scheduleNoteOffAll(int64_t targetFrame);

    // Clear all scheduled notes
    void clearScheduledNotes();

    // Clear only notes/kills at or after fromFrame (leaves the current phrase intact).
    // ⚠️ `trackId >= 0` clears ONE track's — the eight song cursors roll back independently, so a
    // live edit must drop only what the track being rolled back is going to schedule again.
    void clearScheduledNotesFrom(int64_t fromFrame, int trackId = -1);

    // Load a table: 16 rows × 8 bytes = [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value,
    // fx3Type, fx3Value].
    void loadTable(int tableId, const uint8_t* rowData);

    // Where each of the table's three column playheads stands on this track, or −1 for a column that
    // is not running one. Three markers on the TABLE screen; see ui/engine_feed.h.
    void getVoiceTableRows(int trackId, int out[TABLE_LANES]);

    // Get table ID for a voice
    int getVoiceTableId(int trackId);

    // Where this track stands in ONE NAMED table, false when it is not in that table at all.
    //
    // ⚠️ A table a hit only ROUTED through has no voice of its own, so asking a voice what it is
    // running cannot see it — and the screen would draw no marker on a table that is being read on
    // every hit. The table's own bookmark is the answer whenever no voice is carrying it.
    bool getTableRowsFor(int trackId, int tableId, int out[TABLE_LANES]);

    // Where this track's sampler voice is looping RIGHT NOW, in sample frames — after LPO has slid
    // it, which is the whole point. ⚠️ **THIS IS THE ONLY WAY TO SEE THE LOOP WINDOW AT ALL**: it is
    // engine state that no event carries, it is re-derived every block, and neither the pixels nor
    // the audio show it directly. Returns false when the track has no sampler voice.
    bool getVoiceLoopWindow(int trackId, int* startFrame, int* endFrame);

    // Schedule a table-row jump (THO on an empty step) for the track's voices at targetFrame.
    void scheduleVoiceTableRow(int64_t targetFrame, int trackId, int row);

    // Schedule a phraseVol update at exact frame (Vxx effect on empty steps)
    void scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol);

    // ── Live per-note / mixer FX (all routed through the sample-accurate param queue) ──
    // A per-voice controller — PAN, REV, DEL, CUT, RES, LPF/HPF/BPF, DRV, CRU, FIN, LPO — by its CC id
    // (songcore/event.h), `value` the 0-1 CC value. What each one does is `applyVoiceCc`.
    void scheduleVoiceCc(int64_t targetFrame, int trackId, int cc, float value);
    void scheduleVoiceReverse(int64_t targetFrame, int trackId, bool reverse, bool restart);  // BCK
    void scheduleVoiceEqSlot(int64_t targetFrame, int trackId, int slot);              // EQN xx
    void scheduleMasterEqSlot(int64_t targetFrame, int slot);                          // EQM xx
    // An AUS/AUF EQ morph tick. Not a slot: the bands are carried verbatim, because the setting a
    // morph names is between two presets and need not be one any slot holds.
    void scheduleVoiceEqBands(int64_t targetFrame, int trackId, const EqBandsHex& bands);
    void scheduleMasterEqBands(int64_t targetFrame, const EqBandsHex& bands);
    void scheduleTrackVolume(int64_t targetFrame, int trackId, float volume);          // VTR xx
    void scheduleMasterVolume(int64_t targetFrame, float volume);                      // VMV xx
    void scheduleDelayTime(int64_t targetFrame, float time);                           // TIM xx

    /**
     * An instrument's parameters were edited — make the notes ALREADY SOUNDING on it hear that.
     *
     * ⚠️ A TRIGGER COPIES THE INSTRUMENT INTO THE VOICE, so a filter, drive, crush or send edit is
     * otherwise silent until the next note. Every live edit path calls this after pushing the
     * instrument. No frame: it lands at the next drain — a hand's edit has no step to align to.
     */
    void refreshSoundingInstrument(int instrumentId);

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

    // The MIXER's other three gates: the reverb return, the delay return, and the DRY sum of all eight
    // tracks. One call because they move together — a solo is a statement about the set, so soloing the
    // reverb return silences the delay return and the dry sum in the same moment.
    //
    // ⚠️ The dry gate is NOT eight track mutes. It sits below every send tap, so the notes feeding a
    // soloed return go on playing and go on feeding it; muting the tracks instead would solo the
    // reverb into silence. songcore/model.h derives all three (reverb_return_audible, dry_audible).
    void setBusMutes(bool revMuted, bool dlyMuted, bool dryBusMuted);

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
    // ⚠️ A band type's NUMBER is its identity (stored in the project file); LOWCUT and HICUT were
    // appended, so the order is not the names'. Defined once in eq-module.h; append, never insert.
    // freqHex: 00-FF → 20–20kHz log, gainHex: 0-240 → −12.0..+12.0 dB (0.1 dB/step), qHex: 00-FF → 0.1–10 log
    void setEqBand(int slot, int band, int type, int freqHex, int gainHex, int qHex);

    // Map an instrument to an EQ preset slot (-1 = off).
    // Copies the preset into instrumentParams[instrId] for use at next note trigger.
    void setInstrumentEqSlot(int instrId, int slot);

    /**
     * One instrument's PROGRAM — the flat facts a note is derived from (type, sample, root, detune,
     * SF slot/bank/preset, slice markers). Pushed from the UI thread whenever the instrument changes;
     * read on the audio thread when a note actually fires.
     *
     * ⚠️ `markers`/`count` are COPIED. The Program's own `sliceMarkers` pointer is not kept — it
     * points into the caller's project, which the audio thread must never follow.
     */
    void setProgram(int instrumentId, const songcore::Program& program,
                    const int64_t* markers, int count);

    // ===================================
    // SEND LEVEL METHODS
    // ===================================

    // Set reverb/delay send levels for an instrument (00-FF each, converted to float).
    void setInstrumentSendLevels(int instrId, int reverbHex, int delayHex);

    // ===================================
    // REVERB / DELAY SEND METHODS
    // ===================================

    // Set reverb params, all 00-FF. feedbackHex is DCAY, the tail's length; sizeHex is SIZE, the room.
    // wetHex controls return gain. sizeHex's default 0x60 is the default room.
    void setReverbParams(int feedbackHex, int dampHex, int wetHex = 0x80, int sizeHex = 0x60);

    // Set the reverb's character: the three cells that place and colour the tail. Neutral at PRE 00,
    // WIDE 80, MOD 40.
    void setReverbCharacter(int preHex, int widthHex, int modHex);

    // Which reverb algorithm reads the cells above, 0 = the default. ⚠️ It rewrites none of them.
    // Any thread; the audio thread switches at its next block.
    void setReverbAlgo(int algo);

    // Set delay params. syncMode false: timeOrSubdiv is hex 00-FF (0-2s).
    //                   syncMode true:  timeOrSubdiv is subdivision index 0-11, bpm used.
    //                   wetHex: 00-FF return gain.
    void setDelayParams(int timeOrSubdiv, int feedbackHex, bool syncMode, float bpm = 120.0f, int wetHex = 0x80);

    // The same, split in two — for the one caller that must push a delay whose TIME the running song
    // has taken over with a TIM (songcore/engine_setup.h).
    void setDelayTime(int timeOrSubdiv, bool syncMode, float bpm = 120.0f);
    void setDelayFeedbackWet(int feedbackHex, int wetHex);

    // Set the delay's character: the three cells that shape the repeats. Off at pong off, TONE FF,
    // WOBL 00.
    void setDelayCharacter(bool pong, int toneHex, int wobbleHex);

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
     * ⚠️ The host's "restore the mixer's EQ on stop" is driven by the SCHEDULER, which a table row
     * never reaches (tables run in here, per voice). So the two ways to author an EQM arm the same
     * restore from opposite sides of the seam; this is the engine's half (songcore/host.h stop()).
     */
    bool takeTableMasterEqTouched() { return tableMasterEqTouched.exchange(false, std::memory_order_relaxed); }

    // The same question WITHOUT consuming the answer, for a caller that wants to know whether the bus
    // is currently overridden rather than to discharge the restore. ⚠️ Anyone asking mid-take must use
    // this one: `take` would disarm stop(), and the bus would then keep the table's preset forever.
    bool tableMasterEqTouchedPeek() const { return tableMasterEqTouched.load(std::memory_order_relaxed); }

    // The same pair for a TABLE row's TIM, which takes the delay's echo time over the same way an EQM
    // takes the master bus and needs the same restore from the same two sides of the seam.
    bool takeTableDelayTimeTouched() { return tableDelayTimeTouched.exchange(false, std::memory_order_relaxed); }
    bool tableDelayTimeTouchedPeek() const { return tableDelayTimeTouched.load(std::memory_order_relaxed); }

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

    // Set a per-instrument modulation slot.
    void setInstrumentModulation(int sampleId, int slotIndex,
                                 int type, int dest, float amount,
                                 int attackSamples, int holdSamples, int decaySamples,
                                 float sustainLevel, float lfoHz, int oscShape,
                                 int releaseSamples = 0, int lfoTrigMode = 1);

    // Copy an instrument's mod-slot config onto a voice at note trigger: resets all per-note
    // state, seeds the RND/DRNK RNG, and applies the LFO trigger mode's initial phase.
    // Shared by the sampler and SF dispatch paths (audio thread only).
    void initVoiceModSlots(IAudioVoice& voice, int sampleId, int64_t currentFrame, float sampleRate);

    // A resolved note onto its voice type's pool, at `frame` inside the current block. The dispatch
    // loop picks one per note; a new voice type is a third, never an inline arm (audio thread only).
    void triggerSoundfontNote(const ScheduledNote& note, int frame, int64_t currentFrame, float sampleRate);
    void triggerSamplerNote(const ScheduledNote& note, int frame, int64_t currentFrame, float sampleRate);

    // What chainTrackPiece and mixTrackBuffer need from the block — processAudioBlock's per-block values.
    struct TrackBufferMix {
        float*       output;          // interleaved stereo, the block being summed into
        int          numFrames;
        const float* trackVolStart;   // per track: the fader's ramp across this block
        const float* trackVolEnd;
        const float* gateStart;       // per track: the mute gate's ramp across this block
        const float* gateEnd;
        bool         octaWanted;      // the OCTA scopes are being drawn
        int          monitoredInstrId;
    };
    template <typename V>
    void chainTrackPiece(V& v, int t, float* buf, const TrackBufferMix& c, int from, int to);
    template <typename V>
    float mixTrackBuffer(V& v, int t, float* buf, const TrackBufferMix& c, bool& stopFadeDone);

    // The voices of one track, for the code that changes or ends them. `fn` is called with each voice
    // as its own type, so a per-type overload decides what it does. A new voice type adds its pool
    // HERE, and every such site reaches it. (Audio thread only.)
    //
    // What a live command reaches: the track's sampler voice unless it is fading out under the next
    // note, and its SoundFont voice while it is active — releasing included. A track plays one note at
    // a time across the pools, so this is that note, plus at most a SoundFont tail.
    template <typename F> void forEachTrackVoice(int track, F&& fn) {
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].isActive && !voices[v].isFadingOut && voices[v].trackId == track) fn(voices[v]);
        if (track >= 0 && track < SF_VOICE_COUNT && sfVoices[track].isActive) fn(sfVoices[track]);
    }
    // What a kill reaches: every sampler voice still sounding on the track, fading ones included, and
    // its SoundFont voice whatever its state.
    template <typename F> void forEachVoiceOnTrack(int track, F&& fn) {
        for (int v = 0; v < MAX_VOICES; v++)
            if (voices[v].isActive && voices[v].trackId == track) fn(voices[v]);
        if (track >= 0 && track < SF_VOICE_COUNT) fn(sfVoices[track]);
    }

    // A TIC FX in the table's LAST row overrides the instrument's tic rate at
    // note trigger — per COLUMN, one rate per playhead. Shared by the sampler and SF dispatch
    // paths (audio thread only).
    void effectiveTicRatesFor(int tableId, int fallback, int out[TABLE_LANES]);

    // Unified per-voice table tick (tic advance + row FX processing) for sampler AND SF voices.
    // Duck-typed template over the identical table-state fields; the two per-type differences (KIL
    // semantics, OFFSET) resolve at compile time via the tableKill/tableOffset overloads
    // (engine-voice-ops.h). Defined in engine-tables.cpp and instantiated there for both voice types.
    //
    // Plays every row due at frame `from` of the block, then returns how many frames the caller may
    // render before the next one is due (at most `maxFrames`) and moves the clocks by that much.
    template <typename V> int processTableTick(V& voice, int from, int maxFrames, float sampleRate);

    // The row half of the above, for ONE column's playhead: lane 0 also carries transpose and volume,
    // every lane its own FX slot. Split out so processTableTick has ONE exit and the AUS/AUF ramp runs
    // EVERY block, not only on a row change. Returns true when the row only STEERED the lane (HOP /
    // THO) and the lane now stands on the row that must play in this same tic.
    template <typename V> bool processTableRow(V& voice, const TableRow& row, int lane,
                                               bool shouldAdvance, int atFrame, float sampleRate,
                                               bool applyNV = true);

    // One table FX that WRITES something — a filter, the drive, the sample offset, an EQ — the arms
    // a row and a hit's carry (TableCarry) share. Steering, KIL and VOL are the caller's.
    template <typename V> void applyTableWrite(V& voice, uint8_t fxType, uint8_t fxValue, float sampleRate);

    // Start a voice on what its hit picked up passing through INS rows. Call after the voice is set
    // up from its instrument and before its table's first row, which may overwrite any of it.
    template <typename V> void applyTableCarry(V& voice, const TableCarry& carry, float sampleRate);

    // The AUS/AUF ramps a table declares, applied to one voice at the position it is standing on.
    // ⚠️ AFTER the row's own effects, never before: on the AUS row the ramp is at t=0, so it writes
    // the same value the cell to its left just wrote, and on every later row the fade is the thing
    // that should win over a stale per-row cell.
    // ⚠️ `rowFraction` is per COLUMN, and each ramp reads the column its PARAMETER cell is in — see
    // the derivation at the definition.
    template <typename V> void applyTableRamps(V& voice, const TableRow* rows,
                                               const double (&rowFraction)[TABLE_LANES],
                                               float sampleRate);

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

    // Clear every effect chain's INTERNAL STATE (reverb delay lines and its LCG, delay buffers, OTT /
    // DUST / limiter envelopes), so a render is a function of the project, not of playback history.
    // ⚠️ Not state-only: the module resets put their factory DEFAULTS back. A render pushes the
    // project right after (songcore::prepare_render); a device reopen replays `busSettings`. Never
    // from the audio thread.
    void resetEffectState();

    // Get current frame counter
    int64_t getFrameCounter();

    // Offline rendering flag: when true, processLiveBlock outputs silence instead of audio.
    void setOfflineRendering(bool offline);

    // Current song tempo (BPM), for the tempo-locked table tic (table speed matches the sequencer,
    // render == live). Set before any note fires.
    void setTempo(int tempo);

    // Stems render mode: 0=normal full mix, 1-8=track N (0-indexed N-1),
    // 9=reverb-return-only, 10=delay-return-only. OTT/DUST/masterEQ are bypassed for non-zero modes.
    void setStemsMode(int mode) { stemsMode = mode; }

    // ── The METRONOME ────────────────────────────────────────────────────────────────────────────
    //
    // A monitoring aid, and deliberately NOT part of the song: it carries no event, it is summed in
    // AFTER the master chain rather than through it, and it is silent while isOfflineRendering — an
    // export of a song written with the metronome on must not have a click baked into it.
    //
    // The grid is a QUARTER NOTE (four phrase steps) measured from the transport's own start frame,
    // which is the same grid songcore::MidiClock puts its 24 PPQN ticks on, pinned the same way: a
    // beat is `epoch + k * framesPerBeat`, never `next += period`, so there is no accumulator to drift.
    // GROOVE is per track and cannot move it — a swung track swings against a steady click, which is
    // what a metronome is for.

    /** SETTINGS > METRONOME. `gain` is the click's peak amplitude; 0 silences it, as does `enabled`. */
    void setMetronome(bool enabled, float gain);

    /** The transport started at `startFrame` — pin beat 0, the accented one, to it. */
    void startMetronome(int64_t startFrame, int64_t framesPerBeat);

    /** The LIVE tempo, pushed once a poll. A change respaces the grid from the next beat onward. */
    void setMetronomeBeat(int64_t framesPerBeat);

    /** The transport ended. */
    void stopMetronome();

private:
    /** Backs lastLoadFailure(). Written by every load path, including the ones that succeed. */
    LoadFailure lastLoadFailure_ = LoadFailure::NONE;

    /**
     * Flush-to-Zero: the engine's denormal protection, and not any compile flag.
     *
     * ⚠️ PER-THREAD (FPCR/FPSCR/MXCSR are thread-local), so EVERY entry point that runs DSP calls it
     * first — including the sample editor's offline ops on the UI thread, where a fade-out tail would
     * otherwise sit in denormals at 10-100× the cost. Repeat calls are free.
     */
    static void setFlushToZeroForCurrentThread();

    /**
     * Sum the metronome click into an already-finished block. Called from processAudioBlock, below
     * the master chain, so the click is neither compressed by the limiter nor coloured by the bus FX
     * — it is a monitor, not a part of the mix. Silent during an offline render.
     */
    void renderMetronome(float* output, int numFrames, int channelCount, float sampleRate,
                         int64_t blockStartFrame, bool offlineRender);

    // ⚠️ THE GRANULARITY AT WHICH EVENTS ARE RESOLVED, and the size of every per-block buffer — one
    // constant, since the correctness limit is the tighter of the two.
    //
    // Correctness: a block's note-ons are applied in one pass, and a faded voice frees its slot only
    // as it is MIXED, so several same-track retriggers in one block exhaust the pool and notes are
    // silently DROPPED (a 940-frame ALSA period lost ~17% of an RPT 01's retriggers). Live and
    // offline both chunk here, so what you hear is what you export.
    //
    // Buffers: every per-block scratch member is a fixed array of this many frames, so
    // processAudioBlock rejects a larger block rather than overrun them.
    static constexpr int PROCESS_SUBBLOCK = 256;

    // Device output sample rate, cached from the platform backend (see setDeviceSampleRate). Defaults to
    // 44100 so getSampleRate()/pitch math stay correct if read before the stream opens — matches every
    // other 44100 fallback in the engine. Atomic: written by the shell thread, read by the scheduler.
    std::atomic<int> deviceSampleRate{44100};

    // The rate the send and master buses were last built at. Only setDeviceSampleRate touches it, and
    // only to notice that a re-init is owed — the coefficients are the buses' own, not readable back.
    int effectsSampleRate = 44100;

    // ⚠️ **A BUS SETTER NEVER TOUCHES ITS MODULE.** It records the value here, bumps its group's
    // sequence number and publishes the record; the audio thread applies each group whose number moved
    // at the top of its next block (applyBusSettings). One setter call is one apply, even with an
    // unchanged value — the STOP restores of a TIM's echo time and a table's EQM depend on that.
    // The record is also what setDeviceSampleRate replays after rebuilding the buses at a new rate.
    // Control thread only (the UI, or a render while the device is paused) — a table row's EQM and
    // TIM reach their modules on the audio thread without passing through.
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

    // ⚠️ The TIC00 table cursor, per TRACK — where the track's table stands once no voice is left to
    // carry it (a one-shot can run out before the next note, and the row would then depend on the
    // root note). It mirrors the voice's pair, so the next retrigger and the TABLE screen derive from
    // it as from a live voice. Written in ONE place (the sampler table-tick loop); stopAll() clears it.
    // ⚠️ PER COLUMN: only a column at TIC00 carries over; each consumer checks the column's rate.
    struct Tic00Cursor {
        int tableId       = -1;  // -1 = no TIC00 table running on this track; the next note starts at row 0
        int  row[TABLE_LANES]           = {0, 0, 0};    // each column's row — where its table stands
        int  lastProcessed[TABLE_LANES] = {-1, -1, -1}; // …and whether that row has been applied yet
        int  ticRate[TABLE_LANES]       = {6, 6, 6};    // which columns were at TIC00 and may carry over
        // ⚠️ …and which had already executed HOP FF. Without it a column that stopped before its voice
        // ended would both draw a marker on the row it stopped at and resume there on the next note.
        bool active[TABLE_LANES]        = {true, true, true};
    };
    // ⚠️ **ONE BOOKMARK PER TABLE, NOT PER TRACK**, and that is what makes an INS on a table row work.
    // A hit can pass THROUGH one table on its way to another instrument: the table it passed through
    // keeps its place so the next hit reads the next row down, while the table the voice actually runs
    // keeps its own. Keyed by tableId within the track — a free slot is claimed on first use.
    static constexpr int TIC00_SLOTS = 8;
    Tic00Cursor tic00Cursor[SF_VOICE_COUNT][TIC00_SLOTS];
    // Which table the track's voice last RAN, so the TABLE screen still has one answer to draw.
    // -1 = none; cleared with the cursors above, never left at 0 (which is a real table id).
    int tic00Sounding[SF_VOICE_COUNT];

    /** This track's bookmark for `tableId`; null when there is none and `create` is false. */
    Tic00Cursor* tic00Slot(int trackId, int tableId, bool create);

    // ⚠️ **WHAT THE SCREEN READS OF THE VOICES.** The audio thread publishes this at the end of every
    // block; every voice getter the UI calls answers from its copy and never from the voices, which
    // change under it. A table column's `lanes` entry is the marker it draws, −1 for none.
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
        /** The bookmark this track keeps for `tableId`, or null. */
        const Bookmark* bookmark(int trackId, int tableId) const {
            if (trackId < 0 || trackId >= SF_VOICE_COUNT || tableId < 0) return nullptr;
            for (const Bookmark& b : tic00[trackId]) if (b.tableId == tableId) return &b;
            return nullptr;
        }
        /** The first sounding sampler voice on the track that is (or is not) fading; −1 for none. */
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
    /** Rewind every track's bookmarks — what a transport stop does. */
    void resetTic00Cursors();

    /**
     * Follow the INS cells on the tables a hit passes through, and answer which instrument sounds.
     *
     * A hit is a PATH: the step names an instrument, that instrument's table is consulted, and if the
     * row it is standing on says INS then the hit is handed to that instrument, which brings its own
     * table, which may hand it on again. Every table it passes through keeps its place, so the next
     * hit reads the next row down — that is what makes a sixteen-row rotation work.
     *
     * ⚠️ **The tables passed through do not tick on the note** — the voice runs the LAST table only,
     * and an INS met later, while the note holds, is not read. What a switching row holds to the LEFT
     * of its INS is in the hit's path, so it goes into `carry` for the voice to start with.
     *
     * Returns the instrument that sounds, or -1 for silence — an empty slot, or external gear, which
     * has no way to answer yet. `outTableId` is left holding the table the voice should run.
     *
     * ⚠️ Runs on the AUDIO THREAD, at the trigger. Bounded by CHAIN_MAX_LINKS and allocation-free.
     */
    static constexpr int CHAIN_MAX_LINKS = 4;
    int resolveChain(int trackId, int instrumentId, int tableIdOverride, int* outTableId,
                     TableCarry* carry);

    // Chain rolls (a CHA gating a switch, an RNL picking the instrument) happen on the audio thread,
    // so they use the engine's lock-free PRNG rather than the sequencer's.
    uint32_t chainRngState = 0x9E3779B9u;

    // Written by the UI under sampleEditMutex; read by the trigger with no lock, hence atomic.
    //
    // ⚠️ A voice keeps its own copy of the pointer and length, so what keeps it off a freed buffer is
    // `sampleGen`: every change of a slot's buffers bumps it AFTER the new pointers are stored, and the
    // mix — under the same mutex — ends any voice triggered on an older generation before reading it.
    // Go through setSampleBuffers / touchSample, which do the bump; the UI never stops a voice.
    std::atomic<float*>   samples[256];
    std::atomic<float*>   samplesRight[256];    // right channel for stereo samples (null = mono)
    std::atomic<int>      sampleLengths[256];   // ONE length for both channels — samplesRight[id], when
                                                // non-null, always has exactly this length
    std::atomic<uint32_t> sampleGen[256];
    void touchSample(int id) { sampleGen[id].fetch_add(1); }   // caller holds sampleEditMutex
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
    // ⚠️ The same cache in FLOAT, used INSTEAD of the int16 pair when the sample is deeper than 16 bits —
    // an int16 copy of a 24-bit sample would make "back to 24" hand back 16 bits. At most one pair is
    // ever allocated for a slot; `freeRateCache` is the one place that frees both.
    float*   originalSamplesF[256];
    float*   originalSamplesRightF[256];
    int      originalSampleLengths[256];
    uint8_t  sampleBitDepth[256];       // see getSampleBitDepth
    bool     sampleIsFloat[256];
    void     freeRateCache(int id);
    // Every "a new file replaced this slot" site: its depth, and no RATE/BIT original left over.
    void     setSampleSourceFormat(int id, int bits, bool isFloat);
    std::mutex sampleEditMutex;       // held during buffer swap; try-locked in voice mix loop
    float* sampleClipboard      = nullptr; // cross-operation copy/paste buffer (left)
    float* sampleClipboardRight = nullptr; // copy/paste buffer (right channel; null = mono clip)
    int    sampleClipboardLength = 0;

    // Replace the working buffers for `id` with a new left + optional right of length newLen, freeing the
    // old buffers. Keeps left/right and their shared length in lockstep so the stereo mix path can never
    // read a stale or short right channel. Pass newR=nullptr for a mono result. Caller holds
    // sampleEditMutex; the voices playing the old buffers end at the next mix (sampleGen).
    void setSampleBuffers(int id, float* newL, float* newR, int newLen);
    // Acquire sampleEditMutex and end every voice playing slot `id` (touchSample). EVERY destructive
    // sample-editor op must hold the returned lock while mutating/freeing the slot's buffers so
    // the audio thread's try_lock fails (one silent block) instead of reading freed memory.
    std::unique_lock<std::mutex> beginSampleEdit(int id);
    // Apply a global EQ preset (0-127, <0 = bypass) to any EQ — a live voice's inline EQ (EQN), the
    // master bus (EQM), or a send's input EQ. The one place a slot becomes band params.
    void applyEqPresetToModule(EqModule& eq, int slot);
    // The same write, from band VALUES rather than a slot — an AUS/AUF morph tick (EqBandsHex).
    void applyEqBandsToModule(EqModule& eq, const EqBandsHex& bands);
    // Release one SoundFont slot: take the handle out, bump the slot's generation, wait for the audio
    // block in flight to end, close it. The only place a slot is ever freed: LRU eviction,
    // unloadSoundfont and clearAllSoundfonts all route through it. Never on the audio thread.
    void freeSoundfontSlot(int slot);

    // ⚠️ A block that is running when this is called may hold a pointer it loaded before the caller
    // unpublished it; this returns once that block has ended (at once if none is running). The caller
    // must have unpublished the pointer FIRST — the two seq_cst sides are what make that sufficient.
    // Never on the audio thread: it would wait for itself.
    void waitForAudioBlockBoundary() {
        const uint64_t seen = audioBlocksDone.load();
        while (audioInBlock.load() && audioBlocksDone.load() == seen) std::this_thread::yield();
    }
    std::atomic<bool>     audioInBlock{false};
    std::atomic<uint64_t> audioBlocksDone{0};

    // ── the two halves of a load, split so one of them can run somewhere else ─────────────────────
    //
    // `parseSoundfont` is everything expensive and file-touching (trim, decode, whole-bank fallback)
    // and touches no slot, so the worker runs it; `installSoundfont` touches the slot table and only
    // the owning thread runs it. `loadSoundfont` is the two back to back.
    // ⚠️ `parseSoundfont` MUST BE THE ONLY tsf PARSE RUNNING: two would be two decodes bidding for
    // the memory the guard measures one at a time. `waitForSoundfontLoad` keeps that true.
    tsf* parseSoundfont(const char* path, int bank, int preset, LoadFailure* failure);
    int  installSoundfont(tsf* handle, int instrumentId, const char* path, int bank, int preset);

    /**
     * Block until any background load has finished, and reap its thread — but KEEP its result, which
     * the next `collectSoundfontLoad` installs as normal. What the caller wanted was to be the only
     * parse running, not to cancel anything.
     */
    void waitForSoundfontLoad();

    /** The same wait, then throw the answer away. For a project being torn down, and for teardown. */
    void discardSoundfontLoad();

    // The background preset load. `sfLoadDone` is the handshake: the worker writes the result fields
    // and then sets it, the owning thread reads it and then reads the fields, so nothing else needs a
    // lock. `sfLoadBusy` says a thread object exists and has still to be joined.
    std::thread       sfLoadThread;
    std::atomic<bool> sfLoadBusy{false};
    std::atomic<bool> sfLoadDone{false};
    int               sfLoadInstrument = -1;
    std::string       sfLoadPath;
    int               sfLoadBank = -1, sfLoadPreset = -1;
    tsf*              sfLoadHandle  = nullptr;
    LoadFailure       sfLoadFailure = LoadFailure::NONE;

    // The preset list of the last few SoundFont FILES asked about, so scrolling the PATCH row does not
    // re-read the index every frame. Keyed by path and capped: the answer is a few kilobytes each, and
    // nothing here holds a sample. Guarded by its own mutex — the UI thread is the only caller today,
    // but this sits beside data the audio thread reads.
    struct SfFileIndex { std::string path; std::vector<pt::SfPreset> presets; };
    std::vector<SfFileIndex> sfFileIndexCache;
    std::mutex               sfFileIndexMutex;
    /**
     * Position of `path`'s preset list in the cache, reading the file only on a miss; -1 if the file
     * has no preset table. ⚠️ Call it with `sfFileIndexMutex` HELD and read the entry under the same
     * lock — a later miss can move the vector out from under a reference.
     */
    int soundfontFileIndexSlot(const char* path);

    // ⚠️ **EVERY PER-INSTRUMENT TABLE BELOW IS A StagedTable:** a setter edits the control thread's
    // copy and publishes it; the audio thread reads its own copy, pulled at one point in its block
    // (syncInstrumentData). A setter that forgets to publish is an edit nobody hears.
    StagedTable<InstrumentParams, 256>                        instrumentParams;
    StagedTable<std::array<InstrumentModSlot, 4>, 256>        instrumentModSlots;   // [sampleId][slot]
    // The engine's own copy of every instrument, keyed by INSTRUMENT id — not by sampleId, which two
    // instruments can share. Read at the trigger through programView(). See setProgram().
    struct ProgramRow {
        songcore::Program program;
        int64_t           markers[songcore::PROGRAM_SLICE_MARKERS];
    };
    StagedTable<ProgramRow, songcore::PROGRAM_SLOTS> programs;
    songcore::Program programView(int id) const;   // audio thread
    void syncInstrumentData();                      // audio thread

    /**
     * Fill in a deferred note's sound, at the moment it fires. Returns false when the instrument it
     * landed on has nothing to play — an empty slot, or a SoundFont that never loaded — and the note
     * is then dropped rather than sounded on whatever the voice held before.
     *
     * ⚠️ Runs on the AUDIO THREAD. Everything it reads is POD in the program table; nothing here may
     * allocate, lock or touch the project.
     */
    bool resolveScheduledNote(ScheduledNote& note);
    // Per-instrument SF2 ADSR envelope override: stored keyed by instrument id
    // (always unique) and applied atomically in fireArmedNote, so two instruments sharing one de-duplicated
    // tsf handle never collide on the shared preset-region patch. -1 = keep the SF2 preset's own value.
    struct SfEnvOverride { int atk = -1, dec = -1, sus = -1, rel = -1; };
    StagedTable<SfEnvOverride, 256> sfEnvOverrides;

    TableStore tables;             // 256 tables; the audio thread reads them without a lock
    std::mutex tableWriteMutex;

    NoteQueue noteQueue;             // Thread-safe queue of scheduled notes
    KillQueue killQueue;             // Thread-safe queue of scheduled kill events
    ParamUpdateQueue paramUpdateQueue; // Thread-safe queue of scheduled parameter updates
    // Per-block drain buffers: the audio callback empties each queue ONCE per block into
    // these (one lock each) instead of taking the queue mutex every frame. Reused across blocks so
    // the backing allocation persists (no per-block heap churn after warmup). Audio-thread-only.
    std::vector<ScheduledNote>        noteBatch;
    std::vector<ScheduledKill>        killBatch;
    std::vector<ScheduledParamUpdate> paramBatch;

    // Demand-driven visualizer capture: the UI read methods stamp these with the wall clock, and the
    // audio callback does the OCTA / spectrum writes only while a read happened recently (stopping
    // ~CAPTURE_IDLE_MS after the polling stops).
    static const int64_t CAPTURE_IDLE_MS = 250;
    std::atomic<int64_t> lastTrackWaveformReadMs{-CAPTURE_IDLE_MS};
    std::atomic<int64_t> lastSpectrumReadMs{-CAPTURE_IDLE_MS};
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // Written by the audio/render thread, read by the scheduler via getCurrentFrame() — relaxed atomic.
    std::atomic<int64_t> globalFrameCounter{0};  // Total frames processed since start

    // The frame the transport-stop ramp is over at, or −1 when no stop is in flight. Audio thread;
    // processAudioBlock is where the reason it has to exist at all is written down.
    std::atomic<int64_t> stopRampEndFrame{-1};
    std::atomic<bool>    stopRampRequested{false};   // stopAllRamped() → the next block's top
    std::atomic<bool>    stoppedSinceTake{false};     // stopAllRamped() → the next startTake()
    std::atomic<bool>    ottRestartRequested{false};  // startTake() → the next block's top
    void startStopRamp(int64_t blockStartFrame);

    // Session entropy mixed into per-note RNG seeds (RND/DRNK LFO), reseeded from the wall clock at
    // construction and every resetFrameCounter() (= render start): the frame counter alone resets to
    // 0 per render, which would make renders identical. Non-atomic: a torn read is other entropy.
    uint32_t noteSeedEntropy = 0x9E3779B9u;
    std::atomic<bool> isOfflineRendering{false};  // True during WAV export → processLiveBlock outputs silence

    // getBlockTiming's second: summed on the audio thread, published once it holds a second of audio.
    void recordBlockTiming(std::chrono::steady_clock::time_point start, int numFrames, float sampleRate);
    int64_t timingBusyNs = 0;    // audio thread only
    int64_t timingFrames = 0;    // audio thread only
    int     timingWorst  = 0;    // audio thread only
    std::atomic<int> timingPubFrames{0}, timingPubRate{0}, timingPubMean{0}, timingPubWorst{0};
    std::atomic<int> currentTempo{120};  // Song BPM; read by the table-advance to derive framesPerTic
    std::atomic<LiveInputSource*> liveInput{nullptr};   // see setLiveInput

    // ── The metronome, across the thread boundary ────────────────────────────────────────────────
    // Written by the UI/host thread, read once a block by the audio thread. Relaxed atomics for the
    // same reason globalFrameCounter is one: it makes the read formally race-free at zero cost, and a
    // block of slightly-stale tempo or volume is inaudible.
    std::atomic<bool>    metronomeOn{false};
    std::atomic<float>   metronomeGain{0.0f};       // the click's peak amplitude
    std::atomic<int64_t> metronomeEpoch{-1};        // the transport's start frame; -1 = stopped
    std::atomic<int64_t> metronomeBeatFrames{0};    // frames in a quarter note at the live tempo
    // ⚠️ AUDIO THREAD ONLY, like trackGate above — the grid the block walks, and the click in flight.
    // `metroIndex_` counts beats since the current epoch (it restarts when a tempo change rebases the
    // grid); `metroCount_` counts them since the transport started, which is what the bar accent is on
    // — reset the accent with the grid and every tempo nudge would move the downbeat.
    int64_t metroEpoch_      = -1;
    int64_t metroBeatFrames_ = 0;
    int64_t metroIndex_      = 0;
    int64_t metroCount_      = 0;
    int     metroClickPos_   = -1;    // frames into the click being rendered; -1 = idle
    float   metroClickPhase_ = 0.0f;  // the click oscillator's phase, in radians
    float   metroClickStep_  = 0.0f;  // …and its per-frame advance
    static constexpr float METRONOME_CLICK_SEC = 0.030f;  // one click, start to silence
    static constexpr float METRONOME_ACCENT_HZ = 2093.0f; // the downbeat  (C7)
    static constexpr float METRONOME_BEAT_HZ   = 1046.5f; // the other three (C6)
    static constexpr int   METRONOME_BEATS_PER_BAR = 4;   // 16 phrase steps — one phrase
    // Set by a table row's EQM, consumed by takeTableMasterEqTouched(). Audio thread writes,
    // UI thread reads — atomic for that reason and no other; it is a one-way latch.
    std::atomic<bool> tableMasterEqTouched{false};
    // …and by a table row's TIM, for the identical reason.
    std::atomic<bool> tableDelayTimeTouched{false};
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

    // The mixer's targets, written by the setters (UI thread, and the VTR/VMV arms on the audio
    // thread) and read ONCE per block where the ramps below are walked. Atomics, not a lock: the
    // audio thread must never wait on a fader move, and a value landing a block late is inaudible.
    // ⚠️ RELAXED everywhere — each is a single number nothing else is ordered against.
    static_assert(std::atomic<float>::is_always_lock_free, "a fader write must not take a lock");
    std::atomic<float> trackVolumes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // ⚠️ A SEPARATE GATE, NOT A FADER VALUE. Folding the mute into trackVolumes would make a VTR
    // ramp — which writes the fader from the audio thread — un-mute the track it lands on. The two
    // stay independent and are multiplied where the block snapshots them.
    std::atomic<bool>  trackMuted[8] = {false, false, false, false, false, false, false, false};
    // Where the gate has actually GOT TO, chasing trackMuted at MUTE_GATE_SAMPLES per full swing.
    // ⚠️ AUDIO THREAD ONLY — it is advanced once per block inside processAudioBlock and read nowhere
    // else, which is why it needs no atomic and no lock of its own. Starts open: an engine that has
    // never been told about a mute must not fade its first block in.
    float trackGate[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    // ⚠️ WHERE EACH FADER HAS GOT TO — a fader moves across a block, it does not jump at its edge.
    // A knob sending 0-127 steps a gain by ~0.8% per message, and a step in a gain is a step in the
    // waveform: audible as a tick per message. It walks toward the target at FADER_GLIDE_SAMPLES per
    // full swing, and both mix paths interpolate each block's piece of that walk per sample.
    // ⚠️ AUDIO THREAD ONLY, like the gates: advanced once per block inside processAudioBlock.
    float trackVolRamp[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float masterVolRamp   = 1.0f;
    // Set while a VTR / VMV glide is under way: an export snaps a fader to its target, but not one
    // the song itself is moving — that would turn the glide back into a step.
    bool trackVolSongMove[8] = {};
    bool masterVolSongMove   = false;
    // The global frame of the last note that started on each track — a VTR one frame behind it is
    // that note's own level. ⚠️ AUDIO THREAD ONLY.
    int64_t trackOnsetFrame[SF_VOICE_COUNT] = {INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2,
                                               INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2, INT64_MIN / 2,
                                               INT64_MIN / 2};
    // Which of those eight the preview lane borrows, or -1 for unity. An INDEX, not a gain: the
    // snapshot below re-reads the live fader every block, so a VTR or a mixer move is heard in the
    // audition it is aimed at. Written by the UI thread, read once per block.
    std::atomic<int>   previewLaneTrack{-1};
    // The three bus gates, and where each has got to. Same ramp as the tracks', for the same reason:
    // slamming a return or the whole dry mix to zero in one sample is a full-scale step in the output.
    // ⚠️ The `*Gate` floats are AUDIO THREAD ONLY — advanced once per block and read nowhere else.
    std::atomic<bool>  revReturnMuted{false}, delayReturnMuted{false}, dryMuted{false};
    float revReturnGate = 1.0f, delayReturnGate = 1.0f, dryGate = 1.0f;
    std::atomic<float> masterVolume{1.0f};
    // The two return levels and the delay→reverb feed: the EFFECTS screen writes them, the block
    // reads them once at the send mix. Atomic for the same reason as the faders above.
    std::atomic<float> reverbReturnGain{0.5f};
    std::atomic<float> delayReturnGain{0.5f};
    std::atomic<float> delayToReverbSend{0.0f};

    // Send buses (reverb and delay)
    ReverbModule reverbSend;
    DelayModule  delaySend;
    MasterChain  masterChain;  // final output bus

    // EQ preset bank (128 slots; pre-converted from hex to Hz/dB/Q)
    struct EqPresetBank {
        EqBandData bands[3];
    };
    StagedTable<EqPresetBank, 128> eqPresets;
    // ⚠️ THE SAME 128 PRESETS AS AUTHORED HEX, and the bank above cannot answer for them. A morph
    // interpolates the AUTHORED bytes — that is what makes a frequency sweep linear in log-frequency
    // — and Hz/dB/Q is a one-way conversion: interpolating those instead walks a different path
    // between the same two presets. The phrase path never needed this because the morph happens
    // above the seam, where the model still has the hex; a TABLE morph happens here.
    //
    // Both arrays are written in exactly one place, `setEqBand`, and always together.
    StagedTable<EqBandsHex, 128> eqPresetHex;

    // Per-track waveform buffers for OCTA visualizer.
    // 8 song tracks + 1 dedicated preview lane (index PREVIEW_LANE, declared public above): all
    // previews — sampler, sample, note, and SF instrument — play outside tracks 0-7, so without their
    // own lane they would never appear on the per-track scopes.
    static const int TRACK_WAVEFORM_COUNT = 9;  // 8 tracks + preview lane
    float trackWaveformBuffer[TRACK_WAVEFORM_COUNT][WAVEFORM_SIZE] = {};
    int   trackWaveformIndex = 0;
    bool  trackHasVoice[TRACK_WAVEFORM_COUNT] = {};

    // ── Per-block scratch for processAudioBlock (audio-thread-only) ──────────────────────────────
    // Engine members, not stack locals (~116 KB), so a small-stack real-time thread cannot overflow.
    // Safe shared: processAudioBlock is never concurrent (the live path skips it during a render).
    // Re-initialised every block.
    float revSendBufL[PROCESS_SUBBLOCK], revSendBufR[PROCESS_SUBBLOCK];   // panned reverb-send sum
    float dlySendBufL[PROCESS_SUBBLOCK], dlySendBufR[PROCESS_SUBBLOCK];   // panned delay-send sum
    float revWetL[PROCESS_SUBBLOCK], revWetR[PROCESS_SUBBLOCK];           // reverb wet output
    float dlyWetL[PROCESS_SUBBLOCK], dlyWetR[PROCESS_SUBBLOCK];           // delay wet output
    float instrSpectrumTempL[PROCESS_SUBBLOCK];                           // mono sum of a monitored instrument's voices
    float sfBuf[PROCESS_SUBBLOCK * 2];                                    // per-track SF render (interleaved stereo)
    float sfNoteBuf[PROCESS_SUBBLOCK * 2];                                // a stealing note's own pass, gained apart from the old one
    float trackWaveAccumL[TRACK_WAVEFORM_COUNT][PROCESS_SUBBLOCK];        // OCTA per-track accumulators
    float trackWaveAccumR[TRACK_WAVEFORM_COUNT][PROCESS_SUBBLOCK];
    bool  trackWasActive[TRACK_WAVEFORM_COUNT];             // OCTA: lane had a non-fading voice this block

    // Oscilloscope capture stride: 1 = every sample (~14 ms visible), larger = slower scrolling.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // 1 = capture every sample (no downsampling)
    int waveformDownsampleCounter = 0;
};
