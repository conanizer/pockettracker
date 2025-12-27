#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <vector>
#include <queue>
#include <mutex>

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

const int MAX_VOICES = 8;  // Reduced for testing

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

    // For priority queue sorting (earliest frame first)
    bool operator>(const ScheduledNote& other) const {
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

// Instrument playback parameters
struct InstrumentParams {
    int startPoint;     // 0-255 (normalized position)
    int endPoint;       // 0-255 (normalized position)
    bool reverse;       // Play backwards
    int loopMode;       // 0=off, 1=forward, 2=ping-pong
    int loopStart;      // 0-255 (normalized position)

    InstrumentParams() : startPoint(0), endPoint(255), reverse(false),
                         loopMode(0), loopStart(0) {}
};

struct Voice {
    bool isActive;
    float* sampleData;
    int sampleLength;
    float position;
    int trackId;
    float playbackRate;
    float volume;

    // Playback parameters (calculated from instrument params)
    int actualStart;     // Actual sample index to start from
    int actualEnd;       // Actual sample index to end at
    int actualLoopStart; // Actual sample index to loop from
    bool reverse;        // Play backwards
    int loopMode;        // 0=off, 1=forward, 2=ping-pong
    bool loopingBack;    // For ping-pong mode direction

    Voice() : isActive(false), sampleData(nullptr), sampleLength(0),
              position(0), trackId(-1), playbackRate(1.0f), volume(1.0f),
              actualStart(0), actualEnd(0), actualLoopStart(0),
              reverse(false), loopMode(0), loopingBack(false) {}

    void trigger(float* sample, int length, int track, float rate, float vol,
                 const InstrumentParams& params) {
        sampleData = sample;
        sampleLength = length;
        trackId = track;
        playbackRate = rate;
        volume = vol;

        // Convert normalized 0-255 values to actual sample positions
        actualStart = (params.startPoint * length) / 255;
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

        // Set initial position based on direction
        if (reverse) {
            position = (float)actualEnd;
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

    void setInstrumentParams(int instrumentId, int start, int end, bool rev, int loop, int loopSt) {
        if (instrumentId < 0 || instrumentId >= 256) return;

        instrumentParams[instrumentId].startPoint = start;
        instrumentParams[instrumentId].endPoint = end;
        instrumentParams[instrumentId].reverse = rev;
        instrumentParams[instrumentId].loopMode = loop;
        instrumentParams[instrumentId].loopStart = loopSt;

        LOGD("Instrument %d params: start=%d, end=%d, rev=%d, loop=%d, loopStart=%d",
             instrumentId, start, end, rev, loop, loopSt);
    }

    void triggerNote(int sampleId, int trackId, float freq, float baseFreq, float vol) {
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
                // Use stored instrument parameters
                voices[i].trigger(samples[sampleId], sampleLengths[sampleId], trackId, rate, vol,
                                  instrumentParams[sampleId]);
                LOGD("Note: track=%d, sampleId=%d, rate=%.3f", trackId, sampleId, rate);
                return;
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
                for (int v = 0; v < MAX_VOICES; v++) {
                    if (!voices[v].isActive) {
                        if (note.sampleId >= 0 && note.sampleId < 256 && samples[note.sampleId]) {
                            float rate = note.frequency / note.baseFrequency;
                            voices[v].trigger(samples[note.sampleId], sampleLengths[note.sampleId],
                                            note.trackId, rate, note.volume, instrumentParams[note.sampleId]);
                            voiceFound = true;
                            LOGD("🎵 Triggered note at frame %lld: sample=%d, track=%d, rate=%.3f",
                                 (long long)currentFrame, note.sampleId, note.trackId, rate);
                        }
                        break;
                    }
                }

                if (!voiceFound) {
                    LOGD("⚠️ No free voice for scheduled note at frame %lld", (long long)currentFrame);
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
                        output[i * channelCount] += sample;      // Left
                        output[i * channelCount + 1] += sample;  // Right
                    }
                    voice.isActive = false;
                    break;
                }

                // Linear interpolation: blend between two adjacent samples
                // This eliminates aliasing artifacts when pitch-shifting
                float sample1 = voice.sampleData[idx];
                float sample2 = voice.sampleData[idx + 1];
                float interpolatedSample = sample1 + (sample2 - sample1) * frac;
                float sample = interpolatedSample * voice.volume * 0.25f;

                // Write to both channels (stereo)
                output[i * channelCount] += sample;      // Left
                output[i * channelCount + 1] += sample;  // Right

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
                     float frequency, float baseFrequency, float volume) {
        ScheduledNote note = {
            .targetFrame = targetFrame,
            .sampleId = sampleId,
            .trackId = trackId,
            .frequency = frequency,
            .baseFrequency = baseFrequency,
            .volume = volume
        };
        noteQueue.schedule(note);
    }

    // Clear all scheduled notes
    void clearScheduledNotes() {
        noteQueue.clear();
    }

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    int sampleLengths[256];
    InstrumentParams instrumentParams[256];

    // PHASE 1: Sample-accurate timing infrastructure
    NoteQueue noteQueue;           // Thread-safe queue of scheduled notes
    int64_t globalFrameCounter;    // Total frames processed since start
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
    // Not implemented
}

JNIEXPORT void JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1stopAll(JNIEnv *env, jobject thiz) {
    if (engine) {
        engine->stopAll();
    }
}

JNIEXPORT jint JNICALL
Java_com_example_pockettracker_TrackerAudioEngine_native_1getActiveVoiceCount(JNIEnv *env, jobject thiz) {
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
        jboolean reverse, jint loopMode, jint loopStart) {
    if (engine) {
        engine->setInstrumentParams(instrumentId, start, end, reverse, loopMode, loopStart);
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

}