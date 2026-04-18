#pragma once
#include <oboe/Oboe.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include "sampler-voice.h"
#include "soundfont-voice.h"

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
                      int startPointOverride = -1, int tableId = -1, int tableTicRate = 6,
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

    // Get per-track peak levels for mixer meters
    void getTrackPeaks(float* outBuffer);

    // Get master peak levels (stereo) for mixer meters
    void getMasterPeaks(float* outBuffer);

    // Decay peaks manually (call when audio stream is not running)
    void decayPeaks();

    // Decay waveform buffer (call when audio stream is not running)
    void decayWaveform();

    // Set real-time track volume (affects playback immediately, including SF channels).
    void setTrackVolume(int trackId, float volume);

    // Set real-time master volume (affects playback immediately)
    void setMasterVolume(float volume);


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
    int sampleLengths[256];
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

    // Per-block per-track peaks: written by processAudioBlock, read by onAudioReady for meters
    float framePeaksPerTrack[8] = {0};

    // Peak level tracking for mixer meters
    float trackPeaks[8] = {0};      // Per-track peak levels (0.0 - 1.0)
    float masterPeakL = 0;          // Master left channel peak
    float masterPeakR = 0;          // Master right channel peak
    std::mutex peakMutex;
    static constexpr float PEAK_DECAY = 0.95f;  // Decay rate per callback (smooth falloff)

    // Real-time volume control (can be changed without rescheduling notes)
    float trackVolumes[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float masterVolume = 1.0f;
    std::mutex volumeMutex;

    // Downsampling for oscilloscope (capture every Nth sample)
    // Lower = faster scrolling (more zoomed in), Higher = slower scrolling (more time visible)
    // Adjust this value to control oscilloscope speed:
    //   1 = 14ms visible (super fast), 10 = 140ms, 20 = 280ms, 50 = 700ms, etc.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // Capture every 10th sample
    int waveformDownsampleCounter = 0;
};
