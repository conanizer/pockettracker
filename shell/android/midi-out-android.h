#ifndef POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H
#define POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H

// The Android songcore::IMidiOut — five JNI up-calls into MainActivity. Nothing off Android.

#include "midi/midi-out-base.h"

#ifdef __ANDROID__

#include <jni.h>

#include "android/midi-android-devices.h"

#include <string>
#include <vector>

namespace ptshell {

class AndroidMidiOut : public MidiOutBase {
  public:
    AndroidMidiOut();
    ~AndroidMidiOut() override;

    int         device_count() override;
    std::string device_name(int index) override;
    bool        open(int index) override;
    void        close() override;
    bool        is_open() const override { return open_; }
    void        send(const uint8_t* data, int len) override;

  private:
    AndroidMidiDevices devices_;
    // Whether our last `open` succeeded, so the frame loop never asks Java. A yanked device shows
    // up as `send` failing — counted and marked in the MIDI trace.
    bool open_ = false;
    // A SysEx goes out of one array kept for it, allocated at open: none per message.
    jbyteArray longBuf_ = nullptr;
};

}  // namespace ptshell

#endif  // __ANDROID__
#endif  // POCKETTRACKER_SHELL_MIDI_OUT_ANDROID_H
