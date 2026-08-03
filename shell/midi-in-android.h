#ifndef POCKETTRACKER_SHELL_MIDI_IN_ANDROID_H
#define POCKETTRACKER_SHELL_MIDI_IN_ANDROID_H

// The ANDROID songcore::IMidiIn — `MidiManager`, reached by JNI. MIDI plan phase E5.
// See midi-in-android.cpp for the direction gotcha, and for why this backend is POLLED where the other
// two push. Compiles to nothing off Android; android-main.cpp is the only thing that names it, exactly
// as it is the only thing that names AndroidMidiOut.

#include "midi-in-base.h"

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
    /** The one reusable byte[] `midiInRead` fills. A GLOBAL ref — see the note in the .cpp. */
    jbyteArray buffer_ = nullptr;
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_IN_ANDROID_H
