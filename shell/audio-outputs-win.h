// audio-outputs-win.{h,cpp} — SETTINGS > AUDIO OUT on Windows: the system output or an ASIO driver.
//
// One AudioBackend to the app, delegating to whichever output is playing. Windows only; an empty TU
// elsewhere.
#ifndef POCKETTRACKER_AUDIO_OUTPUTS_WIN_H
#define POCKETTRACKER_AUDIO_OUTPUTS_WIN_H

#ifdef _WIN32

#include <string>
#include <vector>

#include "app.h"
#include "asio-audio-engine.h"
#include "audio-backend.h"

class AudioEngine;
class SdlAudioEngine;

namespace ptshell {

class WindowsAudioOutputs : public AudioBackend, public AudioOutputSelector {
  public:
    /** `system` must already be open. The ASIO drivers are listed once, here. */
    WindowsAudioOutputs(AudioEngine* core, SdlAudioEngine& system);

    const std::vector<std::string>& names() const override { return names_; }
    int  active() const override { return active_; }
    bool select(int index, std::string& error) override;

    bool openStream() override;
    void closeStream() override { current().closeStream(); }
    void resumeStream() override { current().resumeStream(); }
    void setPaused(bool paused) override { current().setPaused(paused); }
    int  sampleRate() const override { return current().sampleRate(); }
    OutputLatency outputLatency() const override { return current().outputLatency(); }
    bool deviceLost() const override { return current().deviceLost(); }

  private:
    AudioBackend&       current();
    const AudioBackend& current() const;
    bool open(int index, int rate);

    AudioEngine*             core_;
    SdlAudioEngine&          system_;
    AsioAudioEngine          asio_;
    std::vector<std::string> names_;
    int                      active_ = 0;
};

}  // namespace ptshell

#endif  // _WIN32
#endif  // POCKETTRACKER_AUDIO_OUTPUTS_WIN_H
