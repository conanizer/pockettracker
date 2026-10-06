// audio-outputs-win.cpp — see the header.

#include "windows/audio-outputs-win.h"

#ifdef _WIN32

#include <cstdio>

#include "audio-engine.h"
#include "sdl/sdl-audio-engine.h"

namespace ptshell {

WindowsAudioOutputs::WindowsAudioOutputs(AudioEngine* core, SdlAudioEngine& system)
    : core_(core), system_(system), asio_(core) {
    names_.push_back("SYSTEM");
    for (const std::string& n : AsioAudioEngine::driver_names()) names_.push_back(n);
    std::printf("audio:   %zu ASIO driver(s)", names_.size() - 1);
    for (size_t i = 1; i < names_.size(); ++i) std::printf("%s %s", i == 1 ? ":" : ",", names_[i].c_str());
    std::printf("\n");
}

AudioBackend& WindowsAudioOutputs::current() {
    return active_ == 0 ? static_cast<AudioBackend&>(system_) : asio_;
}

const AudioBackend& WindowsAudioOutputs::current() const {
    return active_ == 0 ? static_cast<const AudioBackend&>(system_) : asio_;
}

// ⚠️ Always at the rate already in force. Loaded samples are pitched for it, so an output that cannot
// run at it is refused rather than allowed to move it.
bool WindowsAudioOutputs::open(int index, int rate) {
    if (index == 0) {
        system_.pin_rate(rate);
        return system_.openStream();
    }
    asio_.set_driver(names_[static_cast<size_t>(index)]);
    asio_.set_rate(rate);
    return asio_.openStream();
}

bool WindowsAudioOutputs::select(int index, std::string& error) {
    if (index < 0 || index >= static_cast<int>(names_.size())) {
        error = "NO SUCH OUTPUT";
        return false;
    }
    if (index == active_) return true;

    const int rate = core_->getSampleRate();
    current().closeStream();
    if (open(index, rate)) {
        active_ = index;
        return true;
    }
    error = index == 0 ? "SYSTEM OUTPUT WILL NOT OPEN" : asio_.last_error();
    if (!open(active_, rate) && active_ != 0) {
        active_ = 0;
        open(0, rate);
    }
    return false;
}

// The app's reopen after `deviceLost`. A driver that asked to be reset is reopened; one that went
// silent, or will not come back, leaves the system output playing rather than silence.
//
// ⚠️ A silent driver is NOT tried again: one whose interface is unplugged can still open and then
// never call back, which would be a second of silence and a reopen, for ever.
bool WindowsAudioOutputs::openStream() {
    const int rate = core_->getSampleRate();
    if (!(active_ != 0 && asio_.closed_silent()) && open(active_, rate)) return true;
    if (active_ == 0) return false;
    std::printf("audio:   %s did not come back - playing through the system output\n",
                names_[static_cast<size_t>(active_)].c_str());
    active_ = 0;
    return open(0, rate);
}

}  // namespace ptshell

#endif  // _WIN32
