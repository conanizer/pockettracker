#include <jni.h>
#include <oboe/Oboe.h>
#include <android/log.h>
#include <vector>

#define LOG_TAG "NativeAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

const int MAX_VOICES = 8;  // Reduced for testing

struct Voice {
    bool isActive;
    float* sampleData;
    int sampleLength;
    float position;
    int trackId;
    float playbackRate;
    float volume;

    Voice() : isActive(false), sampleData(nullptr), sampleLength(0),
              position(0), trackId(-1), playbackRate(1.0f), volume(1.0f) {}

    void trigger(float* sample, int length, int track, float rate, float vol) {
        sampleData = sample;
        sampleLength = length;
        position = 0.0f;
        trackId = track;
        playbackRate = rate;
        volume = vol;
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
        builder.setPerformanceMode(oboe::PerformanceMode::None);
        builder.setSharingMode(oboe::SharingMode::Shared);
        builder.setFormat(oboe::AudioFormat::Float);
        builder.setChannelCount(oboe::ChannelCount::Stereo);


        oboe::Result result = builder.openStream(stream);
        if (result != oboe::Result::OK) {
            LOGE("Failed to create stream: %s", oboe::convertToText(result));
            return false;
        }

        LOGD("Stream: %d Hz, buffer: %d",
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
                voices[i].trigger(samples[sampleId], sampleLengths[sampleId], trackId, rate, vol);
                LOGD("Note: track=%d, sampleId=%d", trackId, sampleId);
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

                if (idx >= voice.sampleLength - 1) {
                    voice.isActive = false;
                    break;
                }

                float sample = voice.sampleData[idx] * voice.volume * 0.25f;

                // Write to both channels (stereo)
                output[i * channelCount] += sample;      // Left
                output[i * channelCount + 1] += sample;  // Right

                voice.position += voice.playbackRate;
            }
        }

        return oboe::DataCallbackResult::Continue;
    }

private:
    std::shared_ptr<oboe::AudioStream> stream;
    Voice voices[MAX_VOICES];
    float* samples[256];
    int sampleLengths[256];
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

}