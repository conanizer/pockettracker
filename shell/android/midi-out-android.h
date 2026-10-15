#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H

// The Android songcore::IMidiOut — five JNI up-calls into MainActivity. Nothing off Android.

#include "midi/midi-out-base.h"

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
    // Whether our last `open` succeeded, so the frame loop never asks Java. A yanked device shows
    // up as `send` failing — counted and marked in the MIDI trace.
    bool open_ = false;
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H
