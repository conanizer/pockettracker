#pragma once
#include <oboe/Oboe.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>
#include <algorithm>
#include "sampler-voice.h"
#include "soundfont-voice.h"
#include "effects/send-chain.h"
#include "effects/master-chain.h"

// Per-track soundfont voice state (shares soundfonts[sfSlot].handle via MIDI channels)
// Declared here so audio-engine.cpp and jni-bridge.cpp can reference sfVoices[].
extern SoundfontVoice sfVoices[8];

class AudioEngine : public oboe::AudioStreamDataCallback {
public:
    AudioEngine();
    ~AudioEngine();

    bool openStream();
    void closeStream();

    void loadSample(int id, const float* data, int length);
    void loadSampleStereo(int id, const float* left, const float* right, int length);
    bool hasStereoData(int id);
    void clearAllSamples();

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes);

    void triggerNote(int sampleId, int trackId, float freq, float baseFreq, float vol, float pan = 0.5f);

    void stopTrack(int trackId);
    void stopAll();

    int getActiveVoiceCount();

    /**
     * For each of the 8 tracks, encode the active note as (octave * 12 + pitch), or -1 if no
     * voice is currently playing on that track. The caller passes a pre-allocated int[8] array.
     */
    void getTrackActiveNotes(int* out, int trackCount);

    int getSampleRate();

    void resumeStream();

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
    // Non-destructive FX preview: saves a clean copy separate from the undo slot.
    // Call saveFxPreviewBackup before applySampleFx for preview; restoreFxPreviewBackup to revert.
    void saveFxPreviewBackup(int id);
    void restoreFxPreviewBackup();
    // Destructive resize operations
    void cropSample(int id, int startFrame, int endFrame);
    void deleteSampleRegion(int id, int startFrame, int endFrame);
    void copyRegion(int id, int startFrame, int endFrame);
    void pasteRegion(int id, int insertAt);
    int  getClipboardLength();
    void downsampleSample(int id, int factor);
    // Non-destructive rate mode: derives buffer from cached original (factor 1=HIGH,2=NORM,4=LOFI).
    void applyRateMode(int id, int factor);
    // Destructive pitch shift by semitones (applied to buffer in-place; clears original cache).
    void pitchShiftSample(int id, float semitones);
    // Destructive time-stretch: ratio > 1 = longer/slower, < 1 = shorter/faster. SOLA algorithm.
    void timeStretchSample(int id, float ratio);
    // Destructive whole-sample DSP: fxType 0=OTT, 1=DUST, 2=DRIVE. fxValue 0-255.
    void applySampleFx(int id, int fxType, int fxValue, float sampleRate);
    // Returns the nearest zero-crossing frame within ±searchRadius of `frame`, or `frame` if none found.
    int  findZeroCrossing(int id, int frame, int searchRadius = 512);
    // Spectral-flux transient detection. Returns count; outMarkers[] filled with frame positions.
    // sensitivity 0x00 = few markers (high threshold), 0xFF = many markers (low threshold).
    int  detectTransients(int id, int sensitivity, int* outMarkers, int maxMarkers);

    // ===================================
    // CORE AUDIO PROCESSING BLOCK
    // ===================================
    // ALL audio DSP lives here. onAudioReady and renderOffline are thin wrappers.
    // Rule: NEVER add audio processing logic directly to onAudioReady or renderOffline.
    void processAudioBlock(float* output, int numFrames, int channelCount, float sampleRate);

    // ===================================
    // LIVE AUDIO CALLBACK (thin wrapper — no DSP here!)
    // ===================================
    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override;

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

    // Schedule a soundfont note (public method — called from JNI)
    void scheduleSoundfontNote(int64_t targetFrame, int trackId, int sfSlot,
                               int midiNote, int midiVelocity, float vol, float pan,
                               int bank, int preset,
                               float pslInitialOffset, float pslDuration,
                               float pbnRate, float vibratoSpeed, float vibratoDepth,
                               float phraseVol = 1.0f, int sampleId = -1,
                               int tableId = -1, int tableTicRate = 6,
                               int noteOctave = 4, int notePitch = 0,
                               int tableStartRow = -1);

    // Schedule a kill event (for Kill effect K00)
    void scheduleKill(int64_t targetFrame, int trackId);

    // Schedule a soft note-off (triggers ADSR release instead of hard stop)
    void scheduleNoteOff(int64_t targetFrame, int trackId);

    // Clear all scheduled notes
    void clearScheduledNotes();

    // Clear only notes/kills at or after fromFrame (leaves the current phrase intact)
    void clearScheduledNotesFrom(int64_t fromFrame);

    // ===================================
    // TABLE METHODS (Phase 3.5)
    // ===================================

    // Load table data from Kotlin
    // rowData format: 16 rows × 8 bytes = 128 bytes
    // Each row: [transpose, volume, fx1Type, fx1Value, fx2Type, fx2Value, fx3Type, fx3Value]
    void loadTable(int tableId, const uint8_t* rowData);

    // Get current table row for a voice (for UI feedback)
    int getVoiceTableRow(int trackId);

    // Get table ID for a voice
    int getVoiceTableId(int trackId);

    // Set table row for a voice (THO effect from phrase on empty step)
    void setVoiceTableRow(int trackId, int row);

    // Schedule a phraseVol update at exact frame (Vxx effect on empty steps)
    void scheduleTrackPhraseVol(int64_t targetFrame, int trackId, float phraseVol);

    // Get waveform data for oscilloscope display
    void getWaveform(float* outBuffer, int bufferSize);

    // Get log-spaced frequency-domain magnitude spectrum for EQ visualizer (0-1 per bin)
    void getSpectrumMagnitudes(int numBins, float* out);

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
    // freqHex: 00-FF → 20–20kHz log, gainHex: 00-FF → −12..+12 dB, qHex: 00-FF → 0.1–10 log
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


    // ===================================
    // PITCH MODULATION METHODS (Phase 6)
    // ===================================

    // Returns the active voice for a given track, checking SF voices first.
    IAudioVoice* findActiveVoiceForTrack(int trackId);

    // Set pitch slide for a voice (PSL effect).
    void setPitchSlide(int trackId, float targetSemitones, float durationTicks, int tempo);

    // Set continuous pitch bend (PBN effect).
    void setPitchBend(int trackId, float semitonesPerStep, int tempo);

    // Set vibrato (PVB/PVX effect). depth=0 stops vibrato.
    void setVibrato(int trackId, float speed, float depth);

    // Clear all pitch modulation for a voice (PSL/PBN/PVB/PVX reset).
    void clearPitchMod(int trackId);

    // Set initial pitch offset (PSL setup: call before setPitchSlide).
    void setInitialPitchOffset(int trackId, float semitones);

    // ===================================
    // MODULATION METHODS (Phase 4 — AHD)
    // ===================================

    // Set per-instrument modulation slot (called from Kotlin before scheduling each note)
    void setInstrumentModulation(int sampleId, int slotIndex,
                                 int type, int dest, float amount,
                                 int attackSamples, int holdSamples, int decaySamples,
                                 float sustainLevel, float lfoHz, int oscShape,
                                 int releaseSamples = 0);

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

    // Get current frame counter
    int64_t getFrameCounter();

    // Offline rendering flag: when true, onAudioReady outputs silence instead of audio.
    void setOfflineRendering(bool offline);

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    float* samplesRight[256];          // right channel for stereo samples (null = mono)
    int    sampleLengths[256];
    float* sampleBackups[256];        // single-level undo buffers
    int    sampleBackupLengths[256];
    float* fxPreviewBackup    = nullptr; // separate clean-sample copy for FX preview (doesn't clobber undo)
    int    fxPreviewBackupLen = 0;
    int    fxPreviewBackupId  = -1;
    float* originalSamples[256];      // cached HIGH-rate original for non-destructive RATE mode
    int    originalSampleLengths[256];
    std::mutex sampleEditMutex;       // held during buffer swap; try-locked in voice mix loop
    float* sampleClipboard = nullptr; // cross-operation copy/paste buffer
    int    sampleClipboardLength = 0;
    InstrumentParams instrumentParams[256];
    InstrumentModSlot instrumentModSlots[256][4]; // [sampleId][slotIndex]

    // Table data (Phase 3.5)
    Table tables[256];             // 256 tables, each with 16 rows
    std::mutex tableMutex;         // Protect table data during load/access

    // PHASE 1: Sample-accurate timing infrastructure
    NoteQueue noteQueue;             // Thread-safe queue of scheduled notes
    KillQueue killQueue;             // Thread-safe queue of scheduled kill events
    ParamUpdateQueue paramUpdateQueue; // Thread-safe queue of scheduled parameter updates
    int64_t globalFrameCounter;    // Total frames processed since start
    std::atomic<bool> isOfflineRendering{false};  // True during WAV export → onAudioReady outputs silence

    // Oscilloscope waveform buffer (circular buffer for recent output)
    static const int WAVEFORM_SIZE = 620;
    float waveformBuffer[WAVEFORM_SIZE];
    int waveformIndex = 0;
    std::mutex waveformMutex;

    // Spectrum capture buffer for EQ visualizer (undownsampled, master left channel)
    static const int SPECTRUM_SIZE = 4096;
    float spectrumBuffer[SPECTRUM_SIZE];
    int spectrumWriteIdx = 0;
    std::mutex spectrumMutex;

    // Per-block per-track peaks: written by processAudioBlock, read by onAudioReady for meters
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

    // Downsampling for oscilloscope (capture every Nth sample)
    // Lower = faster scrolling (more zoomed in), Higher = slower scrolling (more time visible)
    // Adjust this value to control oscilloscope speed:
    //   1 = 14ms visible (super fast), 10 = 140ms, 20 = 280ms, 50 = 700ms, etc.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // Capture every 10th sample
    int waveformDownsampleCounter = 0;
};
