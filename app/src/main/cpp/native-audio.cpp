#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <vector>
#include <queue>
#include <mutex>
#include <cmath>

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

const int MAX_VOICES = 8;  // Reduced for testing

// ===================================
// BIQUAD FILTER COEFFICIENT CALCULATION
// ===================================
// Calculates resonant low-pass, high-pass, and band-pass filter coefficients
// Using Robert Bristow-Johnson's Audio EQ Cookbook formulas

inline void calculateBiquadCoeffs(
        int filterType,     // 0=off, 1=lp, 2=hp, 3=bp
        int cutParam,       // 0-255 (cutoff frequency parameter)
        int resParam,       // 0-255 (resonance parameter)
        float sampleRate,   // Audio sample rate (e.g., 44100 Hz)
        float& b0, float& b1, float& b2,  // Output: feedforward coefficients
        float& a1, float& a2              // Output: feedback coefficients
) {
    if (filterType == 0) {
        // Filter off: pass-through (unity gain)
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
        return;
    }

    // Map cutoff parameter (0-255) to frequency (20 Hz - 20 kHz)
    // Use exponential curve for musical feel
    float cutoff = 20.0f * powf(1000.0f, cutParam / 255.0f);  // 20 Hz to 20 kHz
    cutoff = fminf(cutoff, sampleRate * 0.45f);  // Limit to below Nyquist

    // Map resonance parameter (0-255) to Q factor (0.5 - 20.0)
    // Higher Q = sharper resonance peak
    float Q = 0.5f + (resParam / 255.0f) * 19.5f;  // 0.5 to 20.0

    // Calculate intermediate values
    float w0 = 2.0f * M_PI * cutoff / sampleRate;  // Angular frequency
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * Q);  // Bandwidth parameter

    // Calculate coefficients based on filter type
    float a0;  // Normalization coefficient

    if (filterType == 1) {
        // LOW-PASS filter
        b0 = (1.0f - cosw0) / 2.0f;
        b1 = 1.0f - cosw0;
        b2 = (1.0f - cosw0) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else if (filterType == 2) {
        // HIGH-PASS filter
        b0 = (1.0f + cosw0) / 2.0f;
        b1 = -(1.0f + cosw0);
        b2 = (1.0f + cosw0) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else if (filterType == 3) {
        // BAND-PASS filter (constant skirt gain)
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
    } else {
        // Unknown type, pass-through
        b0 = 1.0f; b1 = 0.0f; b2 = 0.0f;
        a1 = 0.0f; a2 = 0.0f;
        return;
    }

    // Normalize coefficients by a0
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
}

// ===================================
// PHASE 1: NOTE QUEUE INFRASTRUCTURE
// ===================================
// Sample-accurate note scheduling system
// Notes are scheduled with exact target frame numbers
// Audio callback triggers notes at precise moments

struct ScheduledNote {
    int64_t targetFrame;     // Exact audio frame to trigger this note
    int sampleId;            // Which sample to play (0-255)
    int trackId;             // Which track/voice (0-7)
    float frequency;         // Target playback frequency
    float baseFrequency;     // Sample's base frequency
    float volume;            // Playback volume (0.0-1.0)
    float pan;               // Stereo pan position (0.0=left, 0.5=center, 1.0=right)
    int startPointOverride;  // Optional start point override (-1 = use instrument default)

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledNote& other) const {
        return targetFrame > other.targetFrame;
    }
};

// Scheduled kill event (for Kill effect K00)
struct ScheduledKill {
    int64_t targetFrame;     // Exact audio frame to trigger kill
    int trackId;             // Which track to kill (0-7)

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledKill& other) const {
        return targetFrame > other.targetFrame;
    }
};

// Thread-safe note queue
// Audio callback pops notes, Kotlin thread pushes notes
class NoteQueue {
private:
    // Min-heap: earliest targetFrame is always on top
    std::priority_queue<ScheduledNote, std::vector<ScheduledNote>, std::greater<ScheduledNote>> queue;
    std::mutex mutex;

public:
    // Schedule a note to be played at exact frame
    void schedule(const ScheduledNote& note) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(note);
        LOGD("📅 Scheduled note: frame=%lld, sample=%d, track=%d, freq=%.2f",
             (long long)note.targetFrame, note.sampleId, note.trackId, note.frequency);
    }

    // Check if any note should trigger at or before this frame
    bool hasNoteAt(int64_t currentFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) return false;
        return queue.top().targetFrame <= currentFrame;
    }

    // Pop the next note (call only if hasNoteAt returns true)
    ScheduledNote pop() {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledNote note = queue.top();
        queue.pop();
        return note;
    }

    // Clear all scheduled notes (for stop/reset)
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        LOGD("🗑️ Note queue cleared");
    }

    // Get queue size (for debugging)
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

// Thread-safe kill queue (for Kill effect K00)
class KillQueue {
private:
    std::priority_queue<ScheduledKill, std::vector<ScheduledKill>, std::greater<ScheduledKill>> queue;
    std::mutex mutex;

public:
    // Schedule a kill event at exact frame
    void schedule(const ScheduledKill& kill) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(kill);
        LOGD("🔪 Scheduled kill: frame=%lld, track=%d", (long long)kill.targetFrame, kill.trackId);
    }

    // Check if any kill should trigger at or before this frame
    bool hasKillAt(int64_t currentFrame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) return false;
        return queue.top().targetFrame <= currentFrame;
    }

    // Pop the next kill (call only if hasKillAt returns true)
    ScheduledKill pop() {
        std::lock_guard<std::mutex> lock(mutex);
        ScheduledKill kill = queue.top();
        queue.pop();
        return kill;
    }

    // Clear all scheduled kills
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        while (!queue.empty()) {
            queue.pop();
        }
        LOGD("🗑️ Kill queue cleared");
    }

    // Get queue size (for debugging)
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

// Instrument playback parameters
struct InstrumentParams {
    int startPoint;     // 0-255 (normalized position)
    int endPoint;       // 0-255 (normalized position)
    bool reverse;       // Play backwards
    int loopMode;       // 0=off, 1=forward, 2=ping-pong
    int loopStart;      // 0-255 (normalized position)

    // Distortion/bitcrusher parameters
    int drive;          // 0-255 (pre-gain boost)
    int crush;          // 0-15 (bit depth reduction, 0=off/16-bit, 15=1-bit)
    int downsample;     // 0-15 (sample rate reduction, 0=off, 1=÷2, 2=÷4, etc.)

    // Filter parameters
    int filterType;     // 0=off, 1=lp, 2=hp, 3=bp
    int filterCut;      // 0-255 (cutoff frequency)
    int filterRes;      // 0-255 (resonance)

    InstrumentParams() : startPoint(0), endPoint(255), reverse(false),
                         loopMode(0), loopStart(0), drive(0), crush(0), downsample(0),
                         filterType(0), filterCut(128), filterRes(0) {}
};

struct Voice {
    bool isActive;
    float* sampleData;
    int sampleLength;
    float position;
    int trackId;
    float playbackRate;
    float volume;
    float panLeft;           // Left channel gain (0.0-1.0)
    float panRight;          // Right channel gain (0.0-1.0)

    // Playback parameters (calculated from instrument params)
    int actualStart;     // Actual sample index to start from
    int actualEnd;       // Actual sample index to end at
    int actualLoopStart; // Actual sample index to loop from
    bool reverse;        // Play backwards
    int loopMode;        // 0=off, 1=forward, 2=ping-pong
    bool loopingBack;    // For ping-pong mode direction

    // Distortion/bitcrusher parameters
    int drive;           // 0-255 (pre-gain boost)
    int crush;           // 0-15 (bit depth reduction, 0=off, 15=1-bit)
    int downsample;      // 0-15 (sample rate reduction factor)

    // Filter parameters
    int filterType;      // 0=off, 1=lp, 2=hp, 3=bp
    int filterCut;       // 0-255 (cutoff frequency)
    int filterRes;       // 0-255 (resonance)

    // Biquad filter state (for resonant filters)
    float b0, b1, b2;    // Feedforward coefficients
    float a1, a2;        // Feedback coefficients (a0 is normalized to 1)
    float x1, x2;        // Input history
    float y1, y2;        // Output history

    Voice() : isActive(false), sampleData(nullptr), sampleLength(0),
              position(0), trackId(-1), playbackRate(1.0f), volume(1.0f),
              panLeft(0.707f), panRight(0.707f),  // Default to center
              actualStart(0), actualEnd(0), actualLoopStart(0),
              reverse(false), loopMode(0), loopingBack(false),
              drive(0), crush(0), downsample(0),
              filterType(0), filterCut(128), filterRes(0),
              b0(1.0f), b1(0.0f), b2(0.0f), a1(0.0f), a2(0.0f),
              x1(0.0f), x2(0.0f), y1(0.0f), y2(0.0f) {}

    void trigger(float* sample, int length, int track, float rate, float vol, float pan,
                 const InstrumentParams& params, float sampleRate, int startPointOverride = -1) {
        sampleData = sample;
        sampleLength = length;
        trackId = track;
        playbackRate = rate;
        volume = vol;

        // Calculate constant-power pan gains
        // pan: 0.0=left, 0.5=center, 1.0=right
        float panAngle = pan * (float)M_PI * 0.5f;  // 0 to π/2
        panLeft = cosf(panAngle);
        panRight = sinf(panAngle);

        // Convert normalized 0-255 values to actual sample positions
        // Use startPointOverride if provided (Offset effect), otherwise use instrument default
        int effectiveStartPoint = (startPointOverride >= 0) ? startPointOverride : params.startPoint;
        actualStart = (effectiveStartPoint * length) / 255;
        actualEnd = (params.endPoint * length) / 255;
        actualLoopStart = (params.loopStart * length) / 255;

        // Clamp to valid range
        actualStart = std::max(0, std::min(actualStart, length - 1));
        actualEnd = std::max(0, std::min(actualEnd, length - 1));
        actualLoopStart = std::max(actualStart, std::min(actualLoopStart, actualEnd));

        // Ensure start < end
        if (actualStart >= actualEnd) {
            actualStart = 0;
            actualEnd = length - 1;
        }

        // Set playback parameters
        reverse = params.reverse;
        loopMode = params.loopMode;
        loopingBack = false;

        // Set distortion/bitcrusher parameters
        drive = params.drive;
        crush = params.crush;
        downsample = params.downsample;

        // Set filter parameters and calculate coefficients
        filterType = params.filterType;
        filterCut = params.filterCut;
        filterRes = params.filterRes;
        calculateBiquadCoeffs(filterType, filterCut, filterRes, sampleRate,
                              b0, b1, b2, a1, a2);

        // Reset filter history (important to avoid clicks)
        x1 = 0.0f; x2 = 0.0f;
        y1 = 0.0f; y2 = 0.0f;

        // Set initial position based on direction
        // For reverse: start at actualEnd - 1 (not actualEnd) because we need to read idx+1 for interpolation
        if (reverse) {
            position = (float)(actualEnd > actualStart ? actualEnd - 1 : actualStart);
        } else {
            position = (float)actualStart;
        }

        isActive = true;
    }

    void stop() {
        isActive = false;
    }
};

class AudioEngine : public oboe::AudioStreamDataCallback {
public:
    AudioEngine() {
        for (int i = 0; i < 256; i++) {
            samples[i] = nullptr;
            sampleLengths[i] = 0;
            // Initialize with default parameters
            instrumentParams[i] = InstrumentParams();
        }
        globalFrameCounter = 0;

        // Initialize waveform buffer
        for (int i = 0; i < WAVEFORM_SIZE; i++) {
            waveformBuffer[i] = 0.0f;
        }
        waveformIndex = 0;
        waveformDownsampleCounter = 0;
    }

    ~AudioEngine() {
        closeStream();
        for (int i = 0; i < 256; i++) {
            if (samples[i]) {
                delete[] samples[i];
            }
        }
    }

    bool openStream() {
        oboe::AudioStreamBuilder builder;
        builder.setDataCallback(this);

        // Enable low-latency mode for precise timing
        // This enables MMAP (fast audio path) and reduces buffer size
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);

        // Try exclusive mode first for lowest latency, fallback to shared if not available
        builder.setSharingMode(oboe::SharingMode::Exclusive);

        builder.setFormat(oboe::AudioFormat::Float);
        builder.setChannelCount(oboe::ChannelCount::Stereo);

        // Set sample rate to 44100 Hz (most common for audio samples)
        // This eliminates compensation for 44100 Hz samples (perfect pitch!)
        // Samples at other rates (48000 Hz) will still be compensated
        builder.setSampleRate(44100);

        oboe::Result result = builder.openStream(stream);
        if (result != oboe::Result::OK) {
            LOGE("Failed to create stream: %s", oboe::convertToText(result));
            return false;
        }

        LOGD("Stream: %d Hz (requested 44100), buffer: %d",
             stream->getSampleRate(),
             stream->getBufferSizeInFrames());

        result = stream->requestStart();
        if (result != oboe::Result::OK) {
            LOGE("Failed to start: %s", oboe::convertToText(result));
            return false;
        }

        LOGD("Started OK");
        return true;
    }

    void closeStream() {
        if (stream) {
            stream->stop();
            stream->close();
            stream.reset();
        }
    }

    void loadSample(int id, const float* data, int length) {
        if (id < 0 || id >= 256) return;

        if (samples[id]) {
            delete[] samples[id];
        }

        samples[id] = new float[length];
        for (int i = 0; i < length; i++) {
            samples[id][i] = data[i];
        }
        sampleLengths[id] = length;

        LOGD("Sample %d: %d frames", id, length);
    }

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt,
                             int drv, int crsh, int dwn, int fType, int fCut, int fRes) {
        if (instrumentId < 0 || instrumentId >= 256) return;

        instrumentParams[instrumentId].startPoint = start;
        instrumentParams[instrumentId].endPoint = end;
        instrumentParams[instrumentId].reverse = rev;
        instrumentParams[instrumentId].loopMode = loop;
        instrumentParams[instrumentId].loopStart = loopSt;
        instrumentParams[instrumentId].drive = drv;
        instrumentParams[instrumentId].crush = crsh;
        instrumentParams[instrumentId].downsample = dwn;
        instrumentParams[instrumentId].filterType = fType;
        instrumentParams[instrumentId].filterCut = fCut;
        instrumentParams[instrumentId].filterRes = fRes;

        LOGD("Instrument %d params: start=%d, end=%d, rev=%d, loop=%d, loopStart=%d, drive=%d, crush=%d, downsample=%d, filter=%d, cut=%d, res=%d",
             instrumentId, start, end, rev, loop, loopSt, drv, crsh, dwn, fType, fCut, fRes);
    }

    void triggerNote(int sampleId, int trackId, float freq, float baseFreq, float vol, float pan = 0.5f) {
        if (sampleId < 0 || sampleId >= 256 || !samples[sampleId]) return;

        // Resume stream if paused (prevents hum when not playing)
        resumeStream();

        // Stop track
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].trackId == trackId) {
                voices[i].stop();
            }
        }

        // Find free voice
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!voices[i].isActive) {
                float rate = freq / baseFreq;
                float sampleRate = stream ? (float)stream->getSampleRate() : 44100.0f;
                // Use stored instrument parameters
                voices[i].trigger(samples[sampleId], sampleLengths[sampleId], trackId, rate, vol, pan,
                                  instrumentParams[sampleId], sampleRate);
                LOGD("Note: track=%d, sampleId=%d, rate=%.3f, pan=%.2f", trackId, sampleId, rate, pan);
                return;
            }
        }
    }

    void stopTrack(int trackId) {
        // Stop all voices on this track
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].trackId == trackId && voices[i].isActive) {
                voices[i].stop();
            }
        }
    }

    void stopAll() {
        for (int i = 0; i < MAX_VOICES; i++) {
            voices[i].stop();
        }
        // Pause stream to prevent background noise when idle
        if (stream && stream->getState() == oboe::StreamState::Started) {
            stream->pause();
            LOGD("Stream paused (stopAll)");
        }
    }

    int getActiveVoiceCount() {
        int count = 0;
        for (int i = 0; i < MAX_VOICES; i++) {
            if (voices[i].isActive) {
                count++;
            }
        }
        return count;
    }

    int getSampleRate() {
        if (stream) {
            return stream->getSampleRate();
        }
        return 48000; // Default fallback
    }

    void resumeStream() {
        // Resume stream when playback starts
        if (stream && stream->getState() == oboe::StreamState::Paused) {
            stream->start();
            LOGD("Stream resumed");
        }
    }

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream *audioStream,
            void *audioData,
            int32_t numFrames) override {

        float *output = static_cast<float*>(audioData);
        int channelCount = audioStream->getChannelCount();

        // Clear
        for (int i = 0; i < numFrames * channelCount; i++) {
            output[i] = 0.0f;
        }

        // PHASE 1: Process note queue at sample-accurate timing
        // Check each frame for scheduled notes
        for (int32_t frame = 0; frame < numFrames; frame++) {
            int64_t currentFrame = globalFrameCounter + frame;

            // Process all scheduled kill events for this exact frame (BEFORE notes)
            while (killQueue.hasKillAt(currentFrame)) {
                ScheduledKill kill = killQueue.pop();
                // Stop all voices on this track
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == kill.trackId && voices[v].isActive) {
                        voices[v].stop();
                        LOGD("🔪 Killed track %d at frame %lld", kill.trackId, (long long)currentFrame);
                    }
                }
            }

            // Trigger all notes scheduled for this exact frame
            while (noteQueue.hasNoteAt(currentFrame)) {
                ScheduledNote note = noteQueue.pop();

                // Find free voice (same logic as triggerNote)
                bool voiceFound = false;

                // First, stop any voice on this track (voice stealing)
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (voices[v].trackId == note.trackId) {
                        voices[v].stop();
                    }
                }

                // Find free voice slot
                bool foundInactiveVoice = false;
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (!voices[v].isActive) {
                        foundInactiveVoice = true;
                        if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                            float rate = note.frequency / note.baseFrequency;
                            float sampleRate = (float)audioStream->getSampleRate();
                            voices[v].trigger(samples[note.sampleId], sampleLengths[note.sampleId],
                                              note.trackId, rate, note.volume, note.pan, instrumentParams[note.sampleId],
                                              sampleRate, note.startPointOverride);
                            voiceFound = true;
                            LOGD("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f, pan=%.2f, startOverride=%d",
                                 (long long)currentFrame, note.sampleId, note.trackId, rate, note.pan, note.startPointOverride);
                        } else {
                            // Sample not loaded - log specific reason
                            if (note.sampleId < 0 || note.sampleId >= 256) {
                                LOGD("❌ Invalid sampleId=%d for note at frame %lld", note.sampleId, (long long)currentFrame);
                            } else {
                                LOGD("❌ Sample %d not loaded! Note at frame %lld cannot play", note.sampleId, (long long)currentFrame);
                            }
                        }
                        break;
                    }
                }

                if (!voiceFound) {
                    if (!foundInactiveVoice) {
                        LOGD("⚠️ No free voice (all 8 active) for note at frame %lld, sample=%d", (long long)currentFrame, note.sampleId);
                    }
                    // If foundInactiveVoice but !voiceFound, it means sample wasn't loaded (already logged above)
                }
            }
        }

        // Mix voices
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive || !voice.sampleData) continue;

            for (int i = 0; i < numFrames; i++) {
                int idx = (int)voice.position;
                float frac = voice.position - (float)idx;  // Fractional part for interpolation

                // Bounds check - need idx+1 for interpolation
                if (idx < 0 || idx >= voice.sampleLength - 1) {
                    // Handle edge case: exactly at last sample
                    if (idx == voice.sampleLength - 1 && frac == 0.0f) {
                        float sample = voice.sampleData[idx] * voice.volume * 0.25f;
                        output[i * channelCount] += sample * voice.panLeft;       // Left
                        output[i * channelCount + 1] += sample * voice.panRight;  // Right
                    }
                    voice.isActive = false;
                    break;
                }

                // ===================================
                // SIGNAL CHAIN: Downsample → Crush → Interpolate → Drive → Volume
                // Applying downsample/crush BEFORE interpolation creates artifacts
                // that interact with pitch-shifting for authentic lo-fi texture
                // ===================================

                // Read two adjacent samples for interpolation
                float sample1 = voice.sampleData[idx];
                float sample2 = voice.sampleData[idx + 1];

                // STEP 1: DOWNSAMPLE (sample rate reduction via sample-and-hold)
                // Apply to raw samples BEFORE interpolation to create aliasing
                if (voice.downsample > 0) {
                    int downsampleFactor = 1 << voice.downsample;  // 2^downsample
                    // Quantize position to downsample grid (creates sample-and-hold effect)
                    int quantizedIdx = (idx / downsampleFactor) * downsampleFactor;
                    sample1 = voice.sampleData[quantizedIdx];
                    sample2 = voice.sampleData[quantizedIdx];  // No interpolation between samples
                }

                // STEP 2: CRUSH (bit depth reduction)
                // Apply to raw samples BEFORE interpolation
                if (voice.crush > 0) {
                    int bits = 16 - voice.crush;  // 0=16-bit (off), 1=15-bit, ..., 15=1-bit
                    if (bits < 1) bits = 1;  // Minimum 1 bit
                    int levels = 1 << bits;  // 2^bits quantization levels
                    sample1 = floorf(sample1 * levels) / levels;
                    sample2 = floorf(sample2 * levels) / levels;
                }

                // STEP 3: LINEAR INTERPOLATION (pitch shifting)
                // Now interpolate the crushed/downsampled samples
                float interpolatedSample = sample1 + (sample2 - sample1) * frac;

                // STEP 4: DRIVE (pre-gain boost with soft clipping for overdrive character)
                float processedSample = interpolatedSample;
                if (voice.drive > 0) {
                    float driveGain = voice.drive / 128.0f;  // 00=0x, 80=1.0x, FF=2.0x
                    processedSample *= driveGain;
                    // Soft clip using tanh for smooth overdrive/distortion character
                    processedSample = tanhf(processedSample);
                }

                // STEP 5: FILTER (resonant biquad filter)
                // Apply only if filter is enabled (filterType != 0)
                if (voice.filterType != 0) {
                    // Biquad filter equation (Direct Form I):
                    // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
                    float x0 = processedSample;
                    float y0 = voice.b0 * x0 + voice.b1 * voice.x1 + voice.b2 * voice.x2
                               - voice.a1 * voice.y1 - voice.a2 * voice.y2;

                    // Update history buffers
                    voice.x2 = voice.x1;
                    voice.x1 = x0;
                    voice.y2 = voice.y1;
                    voice.y1 = y0;

                    processedSample = y0;
                }

                // STEP 6: Apply volume after effects
                float sample = processedSample * voice.volume * 0.25f;

                // Write to stereo channels with pan applied
                output[i * channelCount] += sample * voice.panLeft;       // Left
                output[i * channelCount + 1] += sample * voice.panRight;  // Right

                // Update position based on playback mode
                if (voice.loopMode == 2) {
                    // Ping-pong loop
                    if (voice.loopingBack) {
                        voice.position -= voice.playbackRate;
                        if (voice.position <= voice.actualLoopStart) {
                            voice.loopingBack = false;
                            voice.position = (float)voice.actualLoopStart;
                        }
                    } else {
                        voice.position += voice.playbackRate;
                        if (voice.position >= voice.actualEnd) {
                            voice.loopingBack = true;
                            voice.position = (float)voice.actualEnd;
                        }
                    }
                } else if (voice.reverse) {
                    // Reverse playback
                    voice.position -= voice.playbackRate;
                    if (voice.position <= voice.actualStart) {
                        if (voice.loopMode == 1) {
                            // Forward loop (restart from loop point)
                            voice.position = (float)voice.actualLoopStart;
                        } else {
                            // No loop, stop
                            voice.isActive = false;
                            break;
                        }
                    }
                } else {
                    // Forward playback
                    voice.position += voice.playbackRate;
                    if (voice.position >= voice.actualEnd) {
                        if (voice.loopMode == 1) {
                            // Forward loop (restart from loop point)
                            voice.position = (float)voice.actualLoopStart;
                        } else {
                            // No loop, stop
                            voice.isActive = false;
                            break;
                        }
                    }
                }
            }
        }

        // Capture waveform for oscilloscope (left channel only, with downsampling)
        {
            std::lock_guard<std::mutex> lock(waveformMutex);
            for (int i = 0; i < numFrames; i++) {
                // Downsample: only capture every Nth sample
                waveformDownsampleCounter++;
                if (waveformDownsampleCounter >= WAVEFORM_DOWNSAMPLE) {
                    waveformBuffer[waveformIndex] = output[i * channelCount];  // Left channel
                    waveformIndex = (waveformIndex + 1) % WAVEFORM_SIZE;
                    waveformDownsampleCounter = 0;
                }
            }
        }

        // Update global frame counter for next callback
        globalFrameCounter += numFrames;

        return oboe::DataCallbackResult::Continue;
    }

    // Get current global frame counter (for scheduling notes from Kotlin)
    int64_t getCurrentFrame() {
        return globalFrameCounter;
    }

    // Schedule a note to be played at exact frame
    void scheduleNote(int64_t targetFrame, int sampleId, int trackId,
                      float frequency, float baseFrequency, float volume, float pan = 0.5f, int startPointOverride = -1) {
        ScheduledNote note = {
                .targetFrame = targetFrame,
                .sampleId = sampleId,
                .trackId = trackId,
                .frequency = frequency,
                .baseFrequency = baseFrequency,
                .volume = volume,
                .pan = pan,
                .startPointOverride = startPointOverride
        };
        noteQueue.schedule(note);
    }

    // Schedule a kill event (for Kill effect K00)
    void scheduleKill(int64_t targetFrame, int trackId) {
        ScheduledKill kill = {
                .targetFrame = targetFrame,
                .trackId = trackId
        };
        killQueue.schedule(kill);
    }

    // Clear all scheduled notes
    void clearScheduledNotes() {
        noteQueue.clear();
        killQueue.clear();  // Also clear kill events
    }

    // Get waveform data for oscilloscope display
    void getWaveform(float* outBuffer, int bufferSize) {
        std::lock_guard<std::mutex> lock(waveformMutex);

        // Copy from circular buffer in the correct order
        // Start from current index and wrap around for scrolling effect
        for (int i = 0; i < bufferSize && i < WAVEFORM_SIZE; i++) {
            int readIndex = (waveformIndex + i) % WAVEFORM_SIZE;
            outBuffer[i] = waveformBuffer[readIndex];
        }
    }

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    int sampleLengths[256];
    InstrumentParams instrumentParams[256];

    // PHASE 1: Sample-accurate timing infrastructure
    NoteQueue noteQueue;           // Thread-safe queue of scheduled notes
    KillQueue killQueue;           // Thread-safe queue of scheduled kill events
    int64_t globalFrameCounter;    // Total frames processed since start

    // Oscilloscope waveform buffer (circular buffer for recent output)
    static const int WAVEFORM_SIZE = 620;
    float waveformBuffer[WAVEFORM_SIZE];
    int waveformIndex = 0;
    std::mutex waveformMutex;

    // Downsampling for oscilloscope (capture every Nth sample)
    // Lower = faster scrolling (more zoomed in), Higher = slower scrolling (more time visible)
    // Adjust this value to control oscilloscope speed:
    //   1 = 14ms visible (super fast), 10 = 140ms, 20 = 280ms, 50 = 700ms, etc.
    static const int WAVEFORM_DOWNSAMPLE = 1;  // Capture every 10th sample
    int waveformDownsampleCounter = 0;
};

static AudioEngine* engine = nullptr;

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1delete(JNIEnv *env, jobject thiz) {
    delete engine;
    engine = nullptr;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1triggerNote(
        JNIEnv *env, jobject thiz, jint sid, jint tid, jfloat f, jfloat bf, jfloat v) {
    if (engine) {
        engine->triggerNote(sid, tid, f, bf, v);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1stopTrack(JNIEnv *env, jobject thiz, jint tid) {
    if (engine) {
        engine->stopTrack(tid);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getActiveVoiceCount(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getActiveVoiceCount();
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 48000; // Default fallback
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

// ===================================
// PHASE 1: NOTE QUEUE JNI METHODS
// ===================================

JNIEXPORT jlong JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        jsize length = env->GetArrayLength(outArray);
        float* buffer = new float[length];

        // Get waveform from engine
        engine->getWaveform(buffer, length);

        // Copy to Java array
        env->SetFloatArrayRegion(outArray, 0, length, buffer);

        delete[] buffer;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// JNI METHODS FOR OboeAudioBackend (Refactoring Phase 1)
// ═══════════════════════════════════════════════════════════════════════════
// These are the new platform-agnostic JNI methods that match the OboeAudioBackend class
// in platform/android/OboeAudioBackend.kt

JNIEXPORT jboolean JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1create(JNIEnv *env, jobject thiz) {
    if (!engine) {
        engine = new AudioEngine();
        return engine->openStream() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1delete(JNIEnv *env, jobject thiz) {
    if (engine) {
        delete engine;
        engine = nullptr;
        LOGD("✅ Audio engine deleted");
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1loadSample(
        JNIEnv *env, jobject thiz, jint id, jfloatArray data) {
    if (!engine) return;

    jsize len = env->GetArrayLength(data);
    jfloat* arr = env->GetFloatArrayElements(data, nullptr);

    engine->loadSample(id, arr, len);

    env->ReleaseFloatArrayElements(data, arr, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1scheduleNote(
        JNIEnv *env, jobject thiz, jlong targetFrame, jint sampleId, jint trackId,
        jfloat frequency, jfloat baseFrequency, jfloat volume, jfloat pan, jint startPointOverride) {
    if (engine) {
        engine->scheduleNote(targetFrame, sampleId, trackId, frequency, baseFrequency, volume, pan, startPointOverride);
    }
}

JNIEXPORT jlong JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getCurrentFrame(JNIEnv *env, jobject thiz) {
    if (engine) {
        return (jlong)engine->getCurrentFrame();
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1clearScheduledNotes(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->clearScheduledNotes();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1resumeStream(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->resumeStream();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1killTrack(JNIEnv *env, jobject thiz, jint trackId) {
    if (engine) {
        engine->stopTrack(trackId);
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1scheduleKill(JNIEnv *env, jobject thiz, jlong targetFrame, jint trackId) {
    if (engine) {
        engine->scheduleKill(targetFrame, trackId);
    }
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getSampleRate(JNIEnv *env, jobject thiz) {
    if (engine) {
        return engine->getSampleRate();
    }
    return 44100; // Default fallback
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1getWaveform(JNIEnv *env, jobject thiz, jfloatArray outArray) {
    if (engine && outArray != nullptr) {
        jsize length = env->GetArrayLength(outArray);
        float* buffer = new float[length];

        // Get waveform from engine
        engine->getWaveform(buffer, length);

        // Copy to Java array
        env->SetFloatArrayRegion(outArray, 0, length, buffer);

        delete[] buffer;
    }
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_platform_android_OboeAudioBackend_native_1setInstrumentParams(
        JNIEnv *env, jobject thiz, jint instrumentId, jint start, jint end,
        jboolean reverse, jint loopMode, jint loopStart,
        jint drive, jint crush, jint downsample,
        jint filterType, jint filterCut, jint filterRes) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart,
                                    drive, crush, downsample, filterType, filterCut, filterRes);
    }
}

}