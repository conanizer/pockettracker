#include "ui/modules/mixer.h"

#include <algorithm>
#include <cmath>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// ─── Geometry (MixerModule.kt, verbatim) ─────────────────────────────────────────────────────────
constexpr int METER_SPACING  = 53;
constexpr int FIRST_METER_X  = 10;

constexpr int TRACK_METER_TOP = 24;
constexpr int TRACK_METER_H   = 155;
constexpr int TRACK_VOL_Y     = 182;   // TRACK_METER_TOP + TRACK_METER_H + 3

constexpr int MASTER_METER_H  = 200;   // taller than a track's; same top

// The send returns sit under the tracks: headers ABOVE the meters, wet values BELOW them.
constexpr int SEND_METER_TOP = 234;
constexpr int SEND_METER_H   = 112;
constexpr int SEND_HEADER_Y  = 216;
constexpr int SEND_VALUE_Y   = 350;

// Every stereo pair — tracks, master and sends alike — is two slim bars with a 1px gutter.
constexpr int BAR_W   = 20;
constexpr int BAR_SEP = 1;

constexpr int MASTER_X = FIRST_METER_X + 8 * METER_SPACING;   // 434

// The master strip's four value rows, starting level with the send meters.
constexpr int MROW0_Y = TRACK_METER_TOP + MASTER_METER_H + 10;  // 234
constexpr int MROW1_Y = MROW0_Y + ROW_HEIGHT;                   // 255
constexpr int MROW2_Y = MROW1_Y + ROW_HEIGHT;                   // 276
constexpr int MROW3_Y = MROW2_Y + ROW_HEIGHT;                   // 297

constexpr int MSTR_LABEL_X = MASTER_X - 65;   // 369
constexpr int MSTR_VALUE_X = MASTER_X + 5;    // 439

// LED segments. Deliberately chunkier than the visualizer's 2px, or they read as a solid bar.
constexpr int SEG_H    = 4;
constexpr int SEG_GAP  = 1;
constexpr int SEG_STEP = SEG_H + SEG_GAP;

/** How many peak refreshes the marker hangs before it starts to fall. See mixer.h on what a "frame" is. */
constexpr int PEAK_HOLD_FRAMES = 45;

// Zone boundaries as a fraction of meter height from the bottom. The meter spans −42..+6 dBFS (48 dB),
// so −12 dB is 30/48 up it and 0 dB is 42/48. Fixed to the METER, not to the signal: a green segment is
// green because of where it sits, so the eye reads level off colour without measuring height.
constexpr float LOW_TO_MID_FRAC  = 30.0f / 48.0f;
constexpr float MID_TO_HIGH_FRAC = 42.0f / 48.0f;

/** One channel's level → a bar height in px. Log, clamped to the meter's own dB window. */
int level_to_height_px(float level, int meter_h) {
    const float db  = 20.0f * std::log10(std::max(level, 0.00001f));
    const float pos = (std::min(std::max(db, -42.0f), 6.0f) + 42.0f) / 48.0f;
    return static_cast<int>(static_cast<float>(meter_h) * pos);
}

Argb segment_color(int dy_from_bottom, int total_h, const Theme& t) {
    const float frac = static_cast<float>(dy_from_bottom) / static_cast<float>(total_h);
    if (frac >= MID_TO_HIGH_FRAC) return t.meterHigh;
    if (frac >= LOW_TO_MID_FRAC) return t.meterMid;
    return t.meterLow;
}

/** A peak array the feed has not filled is silence — which is what lets ptshot draw this screen. */
float peak_at(const float* peaks, int index) { return peaks ? peaks[index] : 0.0f; }

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void MixerModule::draw(Canvas& c, int x, int y, const MixerState& s) {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    // ⚠️ The hold advances once per PEAK REFRESH, not once per draw — see the header. A redraw caused by
    // a cursor move (which happens between polls) must not age the peak markers.
    const bool advance = (s.peaksVersion != lastPeaksVersion_);
    lastPeaksVersion_  = s.peaksVersion;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);
    c.draw_text("MIXER", x + 10, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── The eight track meters, with their volumes underneath ────────────────────────────────────
    for (int i = 0; i < 8; ++i) {
        const int  mX    = x + FIRST_METER_X + i * METER_SPACING;
        const bool isSel = (s.mixerMasterRow == 0 && s.cursorColumn == i);

        draw_stereo_meter(c, mX, y + TRACK_METER_TOP, TRACK_METER_H, peak_at(s.trackPeaks, i * 2),
                          peak_at(s.trackPeaks, i * 2 + 1), isSel, p.tracks[static_cast<size_t>(i)].mute,
                          t, i * 2, i * 2 + 1, advance);

        c.draw_text(hex2(p.tracks[static_cast<size_t>(i)].volume), mX + 5, y + TRACK_VOL_Y,
                    isSel ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    }

    // ── The master meter ─────────────────────────────────────────────────────────────────────────
    // ⚠️ Selected on COLUMN ALONE, not on (row, column): the master strip's four rows all live in
    // column 8, so the meter is lit for every one of them. That is Kotlin's `masterSel`, and it is what
    // tells you which strip you are editing while the cursor is down on LIM.
    const bool masterSel = (s.cursorColumn == 8);
    draw_stereo_meter(c, x + MASTER_X, y + TRACK_METER_TOP, MASTER_METER_H,
                      peak_at(s.masterPeaks, 0), peak_at(s.masterPeaks, 1), masterSel,
                      /*is_muted=*/false, t, 16, 17, advance);

    // ── The two send returns, side by side under the tracks ──────────────────────────────────────
    const bool revSendSel = (s.mixerMasterRow == 1 && s.cursorColumn == 0);
    const bool delSendSel = (s.mixerMasterRow == 1 && s.cursorColumn == 1);

    draw_stereo_meter(c, x + FIRST_METER_X, y + SEND_METER_TOP, SEND_METER_H,
                      peak_at(s.reverbPeaks, 0), peak_at(s.reverbPeaks, 1), revSendSel,
                      /*is_muted=*/false, t, 18, 19, advance);
    draw_stereo_meter(c, x + FIRST_METER_X + METER_SPACING, y + SEND_METER_TOP, SEND_METER_H,
                      peak_at(s.delayPeaks, 0), peak_at(s.delayPeaks, 1), delSendSel,
                      /*is_muted=*/false, t, 20, 21, advance);

    // Both labels are centred on their meter pair: half the pair's width, less half the text's.
    const int revCX = x + FIRST_METER_X + (BAR_W + BAR_SEP + BAR_W) / 2;
    const int delCX = x + FIRST_METER_X + METER_SPACING + (BAR_W + BAR_SEP + BAR_W) / 2;

    c.draw_text("REV", revCX - (3 * CHAR_W) / 2, y + SEND_HEADER_Y, t.textParam, CHAR_SPACING,
                FONT_SCALE);
    c.draw_text("DEL", delCX - (3 * CHAR_W) / 2, y + SEND_HEADER_Y, t.textParam, CHAR_SPACING,
                FONT_SCALE);

    c.draw_text(hex2(p.reverbWet), revCX - (2 * CHAR_W) / 2, y + SEND_VALUE_Y,
                revSendSel ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    c.draw_text(hex2(p.delayWet), delCX - (2 * CHAR_W) / 2, y + SEND_VALUE_Y,
                delSendSel ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);

    // ── The master strip ─────────────────────────────────────────────────────────────────────────
    // Row 2 shows OTT *or* DUST — one control, two destinations, chosen by the EFFECTS screen's TYPE.
    // The label changes with it, so the cell always says which of the two you are turning.
    const bool  isOtt      = (p.masterBusFx == 0);
    const char* depthLabel = isOtt ? "OTT" : "DST";
    const int   depthValue = isOtt ? p.ottDepth : p.dustDepth;

    const bool mixSel   = masterSel && s.mixerMasterRow == 0;
    const bool eqSel    = masterSel && s.mixerMasterRow == 1;
    const bool depthSel = masterSel && s.mixerMasterRow == 2;
    const bool limSel   = masterSel && s.mixerMasterRow == 3;

    c.draw_text("MIX", x + MSTR_LABEL_X, y + MROW0_Y, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(hex2(p.masterVolume), x + MSTR_VALUE_X, y + MROW0_Y,
                mixSel ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);

    c.draw_text("EQ", x + MSTR_LABEL_X, y + MROW1_Y, t.textParam, CHAR_SPACING, FONT_SCALE);
    draw_eq_cell(c, x + MSTR_VALUE_X, y + MROW1_Y, p.masterEqSlot, eqSel, t);

    c.draw_text(depthLabel, x + MSTR_LABEL_X, y + MROW2_Y, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(hex2(depthValue), x + MSTR_VALUE_X, y + MROW2_Y,
                depthSel ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);

    c.draw_text("LIM", x + MSTR_LABEL_X, y + MROW3_Y, t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text(hex2(p.limiterPreGain), x + MSTR_VALUE_X, y + MROW3_Y,
                limSel ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
}

void MixerModule::draw_stereo_meter(Canvas& c, int x, int y, int h, float level_l, float level_r,
                                    bool is_selected, bool is_muted, const Theme& t, int peak_idx_l,
                                    int peak_idx_r, bool advance) {
    const Argb border = is_selected ? t.textCursor : t.meterBorder;
    const int  rX     = x + BAR_W + BAR_SEP;

    // One border around the pair (both bars + the gutter between them), then each channel's trough.
    c.fill_rect(x - 1, y - 1, BAR_W + BAR_SEP + BAR_W + 2, h + 2, border);
    c.fill_rect(x, y, BAR_W, h, t.meterBackground);
    c.fill_rect(rX, y, BAR_W, h, t.meterBackground);

    const int lhPx = level_to_height_px(level_l, h);
    const int rhPx = level_to_height_px(level_r, h);

    if (advance) {
        update_peak(peak_idx_l, lhPx, is_muted);
        update_peak(peak_idx_r, rhPx, is_muted);
    }

    // A muted track shows an empty trough — its peaks are forced to zero above, so the marker falls too.
    if (!is_muted) {
        draw_segmented_bar(c, x, y, h, lhPx, t);
        draw_segmented_bar(c, rX, y, h, rhPx, t);
    }

    // After the bars, so a marker resting on the trough is still visible.
    draw_peak_marker(c, x, y, h, peak_idx_l, t);
    draw_peak_marker(c, rX, y, h, peak_idx_r, t);
}

void MixerModule::draw_segmented_bar(Canvas& c, int x, int y, int h, int bar_h_px,
                                     const Theme& t) const {
    const int barBottom = y + h;
    for (int dy = 0; dy + SEG_H <= bar_h_px; dy += SEG_STEP) {
        c.fill_rect(x, barBottom - dy - SEG_H, BAR_W, SEG_H, segment_color(dy, h, t));
    }
}

void MixerModule::update_peak(int idx, int level_px, bool is_muted) {
    const int px = is_muted ? 0 : level_px;
    if (static_cast<float>(px) > peakHoldPx_[idx]) {
        peakHoldPx_[idx]   = static_cast<float>(px);
        peakCounters_[idx] = 0;
        return;
    }
    peakCounters_[idx]++;
    if (peakCounters_[idx] > PEAK_HOLD_FRAMES) {
        peakHoldPx_[idx] = std::max(0.0f, peakHoldPx_[idx] - static_cast<float>(SEG_STEP));
    }
}

void MixerModule::draw_peak_marker(Canvas& c, int x, int y, int h, int peak_idx,
                                   const Theme& t) const {
    const float peakPx = peakHoldPx_[peak_idx];
    if (peakPx <= 0.0f) return;

    // Snapped to the LED grid, so the marker always lands ON a segment row rather than between two.
    const int peakDy  = (static_cast<int>(peakPx) / SEG_STEP) * SEG_STEP;
    const int peakTop = y + h - peakDy - SEG_H;
    if (peakTop < y) return;   // a peak at full scale would draw its marker above the meter

    c.fill_rect(x, peakTop, BAR_W, SEG_H, segment_color(peakDy, h, t));
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MixerModule::cursor_context(const MixerState& s) const {
    const songcore::Project& p = s.project;

    if (s.mixerMasterRow == 0) {
        return (s.cursorColumn < 8)
                   ? cc::hex_byte(p.tracks[static_cast<size_t>(s.cursorColumn)].volume, 0, 255, -1,
                                  false, false, false, /*def=*/0xFF)
                   : cc::hex_byte(p.masterVolume, 0, 255, -1, false, false, false, /*def=*/0xFF);
    }

    // The send returns. A+B resets to 0x80 (unity-ish), not to silence — a send you cannot hear is not
    // a useful default for a control whose whole job is to be dialled in by ear.
    if (s.mixerMasterRow == 1 && s.cursorColumn == 0)
        return cc::hex_byte(p.reverbWet, 0, 255, -1, false, false, false, /*def=*/0x80);
    if (s.mixerMasterRow == 1 && s.cursorColumn == 1)
        return cc::hex_byte(p.delayWet, 0, 255, -1, false, false, false, /*def=*/0x80);

    if (s.cursorColumn == 8) {
        switch (s.mixerMasterRow) {
            case 1:
                // ⚠️ The −1 is passed THROUGH here, so an unassigned master EQ really is `isEmpty` and A
                // inserts slot 0. (The INSTRUMENT screen's EQ cell substitutes 0 before the call and so
                // can never be empty — its `canInsert` is inert, and A there jumps to slot 1. Same cell,
                // two behaviours; both are Kotlin's, and ptinput pins both.)
                return cc::hex_byte(p.masterEqSlot < 0 ? -1 : p.masterEqSlot, 0, 127,
                                    /*empty_value=*/-1, /*can_delete=*/true, /*can_insert=*/true);
            case 2:
                return cc::hex_byte(p.masterBusFx == 0 ? p.ottDepth : p.dustDepth, 0, 255, -1, false,
                                    false, false, /*def=*/0x00);
            case 3:
                return cc::hex_byte(p.limiterPreGain, 0, 255, -1, false, false, false, /*def=*/0x00);
            default:
                return cc::none();
        }
    }

    // Rows 2 and 3 outside column 8: unreachable by navigation, and answered honestly rather than
    // guessed at. This is what keeps the (row, column) pair safe as two independent ints.
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

MixerInputResult MixerModule::handle_input(songcore::Project& p, int cursor_row, int cursor_column,
                                           const InputAction& action) const {
    const auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };

    // The master EQ slot is checked FIRST and on its own, because it is the one cell here that DELETEs
    // and INSERTs rather than only taking a value.
    if (cursor_column == 8 && cursor_row == 1) {
        switch (action.type) {
            case ActionType::SET_VALUE:
                p.masterEqSlot = clamp(action.value, 0, 127);
                return {true};
            case ActionType::DELETE:
                p.masterEqSlot = -1;
                return {true};
            case ActionType::INSERT_DEFAULT:
                p.masterEqSlot = 0;
                return {true};
            default:
                break;   // …and fall through, exactly as Kotlin's inner `when` does
        }
    }

    if (action.type != ActionType::SET_VALUE) return {false};
    const int v = clamp(action.value, 0, 255);

    // The arm ORDER is Kotlin's, and it is load-bearing: `row == 0` (master volume) must be tested
    // after `row == 0 && column < 8` (a track), or every track volume would write the master's.
    if (cursor_row == 1 && cursor_column == 0) { p.reverbWet = v; return {true}; }
    if (cursor_row == 1 && cursor_column == 1) { p.delayWet = v; return {true}; }
    if (cursor_row == 0 && cursor_column < 8) {
        p.tracks[static_cast<size_t>(cursor_column)].volume = v;
        return {true};
    }
    if (cursor_row == 0) { p.masterVolume = v; return {true}; }

    // Rows 2/3 are reachable only in column 8 (the cursor context is `none()` anywhere else, so no
    // SET_VALUE can ever be produced there) — which is why these two need no column test, as in Kotlin.
    if (cursor_row == 2) {
        if (p.masterBusFx == 0) p.ottDepth = v;
        else                    p.dustDepth = v;
        return {true};
    }
    if (cursor_row == 3) { p.limiterPreGain = v; return {true}; }

    return {false};
}

}  // namespace pt::ui
