#ifndef POCKETTRACKER_SHELL_MIDI_IN_ANDROID_H
#define POCKETTRACKER_SHELL_MIDI_IN_ANDROID_H

// The Android songcore::IMidiIn — `MidiManager` over JNI, polled once a frame (the .cpp says why).
// Nothing off Android.

#include "midi/midi-in-base.h"

#ifdef __ANDROID__

#include <jni.h>

#include <string>

namespace ptshell {

class AndroidMidiIn : public MidiInBase {
  public:
    ~AndroidMidiIn() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return open_; }

    /** The frame loop's call: ask the Kotlin side for whatever arrived since last frame. */
    void pump() override;
    bool polled() const override { return true; }

  private:
    struct Hooks {
        bool      resolved = false;
        bool      ok       = false;
        jmethodID count    = nullptr;
        jmethodID name     = nullptr;
        jmethodID open     = nullptr;
        jmethodID close    = nullptr;
        jmethodID read     = nullptr;
    };

    Hooks& hooks(JNIEnv* env, jobject activity);

    Hooks     hooks_{};
    bool      open_ = false;
    // The one reusable byte[] `midiInRead` fills; a global ref.
    jbyteArray buffer_ = nullptr;
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_IN_ANDROID_H
