#pragma once

// The preset folders the stepped rows walk — SCALE, GROOVE, the reverb, the delay, THEME — each read
// from disk once and kept until asked again (`force`) or forgotten.

#include "ui/filesystem.h"
#include "ui/preset_folder.h"

#include <string>
#include <vector>

namespace pt::ui {

class PresetLists {
  public:
    explicit PresetLists(FileSystem& fs) : fs_(fs) {}

    const std::vector<FolderPreset<songcore::Scale>>& scale(bool force) {
        return cached(scale_, force, scale_preset_list);
    }
    const std::vector<FolderPreset<songcore::Groove>>& groove(bool force) {
        return cached(groove_, force, groove_preset_list);
    }
    const std::vector<FolderPreset<NamedReverb>>& reverb(bool force) {
        return cached(reverb_, force, reverb_preset_list);
    }
    const std::vector<FolderPreset<NamedDelay>>& delay(bool force) {
        return cached(delay_, force, delay_preset_list);
    }
    const std::vector<FolderPreset<Theme>>& theme(bool force) {
        return cached(theme_, force, theme_preset_list);
    }

    // Every folder read again on its next use: after a save, a file may have joined.
    void forget() {
        scale_.read = groove_.read = reverb_.read = delay_.read = theme_.read = false;
    }

    // The same when the screen (`screen`, any number that names it) is not the one last seen: a file
    // may have come or gone while another screen was up.
    void forget_on_move(int screen) {
        if (screen == screen_) return;
        forget();
        screen_ = screen;
    }

  private:
    template <typename T, typename Read>
    const std::vector<FolderPreset<T>>& cached(PresetFolderCache<T>& c, bool force, Read read) {
        if (force || !c.read) {
            c.list = read(fs_);
            c.read = true;
        }
        return c.list;
    }

    FileSystem& fs_;

    PresetFolderCache<songcore::Scale>  scale_;
    PresetFolderCache<songcore::Groove> groove_;
    PresetFolderCache<NamedReverb>      reverb_;
    PresetFolderCache<NamedDelay>       delay_;
    PresetFolderCache<Theme>            theme_;
    int                                 screen_ = -1;
};

}  // namespace pt::ui
