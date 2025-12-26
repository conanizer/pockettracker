#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <vector>

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

const int MAX_VOICES = 8;  // Reduced for testing

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

        // Mix voices
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& voice = voices[v];
            if (!voice.isActive || !voice.sampleData) continue;

            for (int i = 0; i < numFrames; i++) {
                int idx = (int)voice.position;

                // Bounds check
                if (idx < 0 || idx >= voice.sampleLength) {
                    voice.isActive = false;
                    break;
                }

                float sample = voice.sampleData[idx] * voice.volume * 0.25f;

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

        return oboe::DataCallbackResult::Continue;
    }

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    int sampleLengths[256];
    InstrumentParams instrumentParams[256];
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

}