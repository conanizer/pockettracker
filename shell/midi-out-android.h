#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H

// The ANDROID songcore::IMidiOut — five JNI up-calls into MainActivity. MIDI plan phase B2b.
// See midi-out-android.cpp for why the port lives in Kotlin at all, and for the direction gotcha
// (to SEND you open the device's INPUT port) that is the single easiest thing to get backwards here.
// Compiles to nothing off Android.

#include "midi-out-base.h"

#ifdef __ANDROID__

#include <jni.h>

#include <string>
#include <vector>

namespace ptshell {

class AndroidMidiOut : public MidiOutBase {
  public:
    ~AndroidMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return open_; }
    void        send(const uint8_t* data, int len) override;

  private:
    /** The five method ids, resolved together on first use. `ok_` false = the Kotlin side is absent. */
    struct Hooks {
        jmethodID count = nullptr, name = nullptr, open = nullptr, close = nullptr, send = nullptr;
        bool      resolved = false;
        bool      ok       = false;
    };

    Hooks& hooks(JNIEnv* env, jobject activity);

    Hooks hooks_;
    /**
     * Whether OUR last `open` succeeded — not a question asked of Java.
     *
     * ⚠️ `is_open()` is called from the frame loop (the OUTPUT row draws from it, and songcore's
     * consumer checks it before every message), and a JNI round trip per frame to answer a question
     * we already know the answer to would be absurd. The two can only disagree if the device is
     * yanked, which surfaces as `send` failing — counted, and visible in `error_count()`.
     */
    bool open_ = false;
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H
