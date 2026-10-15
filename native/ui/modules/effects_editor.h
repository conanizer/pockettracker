#pragma once

// EFFECTS: the master FX type, the reverb and delay, and their input EQ slots (MIXER owns the send
// levels). A form: the cursor walks editable cells and `effects_row_layout.h` places them among the
// headers. TIME reads as a byte or a note division per `delaySync`. Each send opens with a preset line
// (name, SAVE, LOAD), laid out like SCALE's name row.

#include <string>
#include <vector>

#include "songcore/midi_map.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/effects_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

struct EffectState {
    const songcore::Project& project;
    int   cursorRow = 0;   // see the ROW_* constants
    Theme theme     = theme_classic();
};

// The send editor: one send's section in a box over INSTRUMENT, INST.POOL or MIXER, raised from a
// REV or DEL cell. ⚠️ A modal — `currentScreen` still names the screen underneath.
struct SendEditorState {
    bool isOpen    = false;
    int  cursorRow = 0;   // an EffectsRow, never leaving the section drawn
};

struct EffectInputResult {
    bool modified = false;
};

class EffectModule {
public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 392;

    // ⚠️ The values live in effects_row_layout.h: append, never renumber.
    static constexpr int ROW_MASTER_TYPE = static_cast<int>(EffectsRow::MASTER_TYPE);  // OTT / DUST
    static constexpr int ROW_REV_DECAY   = static_cast<int>(EffectsRow::REV_DECAY);    // the tail's length
    static constexpr int ROW_REV_DAMP    = static_cast<int>(EffectsRow::REV_DAMP);
    static constexpr int ROW_REV_EQ      = static_cast<int>(EffectsRow::REV_EQ);       // −1 = off, else a slot
    static constexpr int ROW_DLY_TIME    = static_cast<int>(EffectsRow::DLY_TIME);     // 00..FF, or 0..B synced
    static constexpr int ROW_DLY_FDBK    = static_cast<int>(EffectsRow::DLY_FDBK);
    static constexpr int ROW_DLY_REV     = static_cast<int>(EffectsRow::DLY_REV);      // delay → reverb send
    static constexpr int ROW_DLY_EQ      = static_cast<int>(EffectsRow::DLY_EQ);
    static constexpr int ROW_DLY_TYPE    = static_cast<int>(EffectsRow::DLY_TYPE);     // the delay preset's name
    static constexpr int ROW_DLY_TONE    = static_cast<int>(EffectsRow::DLY_TONE);
    static constexpr int ROW_DLY_WOBBLE  = static_cast<int>(EffectsRow::DLY_WOBBLE);
    static constexpr int ROW_DLY_PONG    = static_cast<int>(EffectsRow::DLY_PONG);
    static constexpr int ROW_REV_TYPE    = static_cast<int>(EffectsRow::REV_TYPE);     // the reverb preset's name
    static constexpr int ROW_REV_PRE     = static_cast<int>(EffectsRow::REV_PRE);      // pre-delay
    static constexpr int ROW_REV_WIDE    = static_cast<int>(EffectsRow::REV_WIDE);     // 80 = untouched
    static constexpr int ROW_REV_MOD     = static_cast<int>(EffectsRow::REV_MOD);      // 40 = as it shipped
    static constexpr int ROW_REV_SIZE    = static_cast<int>(EffectsRow::REV_SIZE);     // the room; 60 = as shipped
    static constexpr int ROW_REV_ALGO    = static_cast<int>(EffectsRow::REV_ALGO);     // 0 = as it shipped
    static constexpr int ROW_REV_SAVE    = static_cast<int>(EffectsRow::REV_SAVE);     // a bare A, no value
    static constexpr int ROW_REV_LOAD    = static_cast<int>(EffectsRow::REV_LOAD);
    static constexpr int ROW_DLY_SAVE    = static_cast<int>(EffectsRow::DLY_SAVE);
    static constexpr int ROW_DLY_LOAD    = static_cast<int>(EffectsRow::DLY_LOAD);
    static constexpr int MAX_CURSOR_ROW  = EFFECTS_ROW_COUNT - 1;

    // In delay-module.h's kDelaySyncBeats[] order.
    static const std::vector<std::string>& delay_sync_names();

    void draw(Canvas& c, int x, int y, const EffectState& s) const;

    CursorContext cursor_context(const EffectState& s) const;

    // For MIDI learn. Preset and mode rows decline: a knob turning pages is not a parameter.
    songcore::MapTarget map_target(const EffectState& s) const;

    EffectInputResult handle_input(songcore::Project& project, int cursor_row,
                                   const InputAction& action) const;
};

// The send editor's box over the finished frame; no-op when closed.
void draw_send_editor(Canvas& c, const SendEditorState& s, const songcore::Project& project,
                      const Theme& t);

}  // namespace pt::ui
