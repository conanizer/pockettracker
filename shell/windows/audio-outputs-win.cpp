
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

// ⚠️ Always at the rate in force (loaded samples are pitched for it); an output that cannot run it is
// refused.
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

// The reopen after `deviceLost`: a driver that asked for a reset is reopened; one that went silent or
// will not come back leaves the system output playing. ⚠️ A silent driver is not retried — an
// unplugged interface can open and never call back, a reopen every second for ever.
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
