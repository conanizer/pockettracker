#include "ui/modules/mixer.h"

#include <algorithm>
#include <cmath>

#include "ui/helpers.h"
#include "ui/mixer_cell_layout.h"

namespace pt::ui {

namespace {

// ─── Geometry ────────────────────────────────────────────────────────────────────────────────────
constexpr int METER_SPACING  = 53;
constexpr int FIRST_METER_X  = 10;

constexpr int TRACK_METER_TOP = 24;
constexpr int TRACK_METER_H   = 155;

constexpr int MASTER_METER_H  = 200;   // taller than a track's; same top

// Background between a meter's outline and the text beside it — the 3px a row pads its glyphs with.
// Every text row bordering a meter takes its baseline from the two helpers below, so all clear it alike.
constexpr int METER_TEXT_GAP = 3;

/** Text baseline for a row sitting UNDER a meter of height `meter_h` that starts at `meter_top`. */
constexpr int text_y_under(int meter_top, int meter_h) {
    return meter_top + meter_h + 1 + METER_TEXT_GAP + TEXT_PADDING;
}

/** Text baseline for a row sitting ABOVE a meter that starts at `meter_top`. */
constexpr int text_y_above(int meter_top) {
    return meter_top - 1 - METER_TEXT_GAP - ROW_HEIGHT + TEXT_PADDING;
}

constexpr int TRACK_VOL_Y = text_y_under(TRACK_METER_TOP, TRACK_METER_H);   // 186

// The send returns sit under the tracks: headers ABOVE the meters, wet values BELOW them.
constexpr int SEND_METER_TOP = 234;
constexpr int SEND_METER_H   = 112;
constexpr int SEND_HEADER_Y  = text_y_above(SEND_METER_TOP);                 // 212
constexpr int SEND_VALUE_Y   = text_y_under(SEND_METER_TOP, SEND_METER_H);   // 353

// The outline grows OUT sideways but IN top and bottom, where the text rows clear the meter's outer
// edge by a derived gap that growing outward would eat.
constexpr int METER_BORDER = 2;
// What the inward half costs the trough: one row off the top, one off the bottom.
constexpr int METER_INSET = METER_BORDER - 1;

// Every stereo pair is two slim bars; the gutter between is left unpainted out of the outline's
// rect, so the pair reads as one framed object with a wall down it.
constexpr int BAR_W   = 20;
constexpr int BAR_SEP = METER_BORDER;

constexpr int MASTER_X = FIRST_METER_X + 8 * METER_SPACING;   // 434

// The master strip's four value rows, hanging off the bottom of its own (taller) meter.
constexpr int MROW0_Y = text_y_under(TRACK_METER_TOP, MASTER_METER_H);  // 231
constexpr int MROW1_Y = MROW0_Y + ROW_HEIGHT;                           // 252
constexpr int MROW2_Y = MROW1_Y + ROW_HEIGHT;                           // 273
constexpr int MROW3_Y = MROW2_Y + ROW_HEIGHT;                           // 294

constexpr int MSTR_LABEL_X = MASTER_X - 65;   // 369
constexpr int MSTR_VALUE_X = MASTER_X + 5;    // 439

// LED segments. Deliberately chunkier than the visualizer's 2px, or they read as a solid bar.
constexpr int SEG_H    = 4;
constexpr int SEG_GAP  = 1;
constexpr int SEG_STEP = SEG_H + SEG_GAP;

/** How many peak refreshes the marker hangs before it starts to fall. See mixer.h on what a "frame" is. */
constexpr int PEAK_HOLD_FRAMES = 45;

// The longest fall there is: the hold, then one segment per refresh down the tallest meter. Replaying
// more refreshes than this in one draw cannot move a marker that is already on the trough.
constexpr unsigned MAX_HOLD_STEPS =
    static_cast<unsigned>(PEAK_HOLD_FRAMES + MASTER_METER_H / SEG_STEP);

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

/** A peak array the feed has not filled is silence. */
float peak_at(const float* peaks, int index) { return peaks ? peaks[index] : 0.0f; }

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void MixerModule::draw(Canvas& c, int x, int y, const MixerState& s) {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    // ⚠️ The hold ages once per PEAK REFRESH, not per draw (see the header), and by as many refreshes
    // as the version moved. Unsigned wrap makes the `-1` sentinel give one step on the first draw.
    const unsigned steps = std::min(s.peaksVersion - lastPeaksVersion_, MAX_HOLD_STEPS);
    lastPeaksVersion_    = s.peaksVersion;

    const MixerCell cur = mixer_cell_at(s.mixerMasterRow, s.cursorColumn);

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);
    c.draw_text("MIXER", x + 10, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── The eight track meters, with their volumes underneath ────────────────────────────────────
    for (int i = 0; i < 8; ++i) {
        const int  mX    = x + FIRST_METER_X + i * METER_SPACING;
        const bool isSel = (cur == MixerCell::TRACK_VOL && s.cursorColumn == i);

        // Muted, unsoloed under another solo, or under a soloed send return: meter and value say so.
        const bool audible = track_audible(p, i) && dry_audible(p);

        draw_stereo_meter(c, mX, y + TRACK_METER_TOP, TRACK_METER_H, peak_at(s.trackPeaks, i * 2),
                          peak_at(s.trackPeaks, i * 2 + 1), isSel, /*is_muted=*/!audible,
                          t, i * 2, i * 2 + 1, steps);

        draw_cursor_cell(c, hex2(p.tracks[static_cast<size_t>(i)].volume), mX + 5, y + TRACK_VOL_Y,
                         isSel, audible ? t.textValue : t.textEmpty, t);
    }

    // ── The master meter ─────────────────────────────────────────────────────────────────────────
    // ⚠️ Selected on COLUMN ALONE: all four master rows live in column 8, so the meter stays lit and
    // says which strip you are editing while the cursor is down on LIM.
    const bool masterSel = (s.cursorColumn == 8);
    draw_stereo_meter(c, x + MASTER_X, y + TRACK_METER_TOP, MASTER_METER_H,
                      peak_at(s.masterPeaks, 0), peak_at(s.masterPeaks, 1), masterSel,
                      /*is_muted=*/false, t, 16, 17, steps);

    // ── The two send returns, side by side under the tracks ──────────────────────────────────────
    const bool revSendSel = (cur == MixerCell::REV_WET);
    const bool delSendSel = (cur == MixerCell::DLY_WET);

    // The returns answer the same question the tracks do, with their own solo set: muted, or unsoloed
    // while the other return is soloed.
    const bool revAudible = reverb_return_audible(p);
    const bool delAudible = delay_return_audible(p);

    draw_stereo_meter(c, x + FIRST_METER_X, y + SEND_METER_TOP, SEND_METER_H,
                      peak_at(s.reverbPeaks, 0), peak_at(s.reverbPeaks, 1), revSendSel,
                      /*is_muted=*/!revAudible, t, 18, 19, steps);
    draw_stereo_meter(c, x + FIRST_METER_X + METER_SPACING, y + SEND_METER_TOP, SEND_METER_H,
                      peak_at(s.delayPeaks, 0), peak_at(s.delayPeaks, 1), delSendSel,
                      /*is_muted=*/!delAudible, t, 20, 21, steps);

    // Both labels are centred on their meter pair: half the pair's width, less half the text's.
    const int revCX = x + FIRST_METER_X + (BAR_W + BAR_SEP + BAR_W) / 2;
    const int delCX = x + FIRST_METER_X + METER_SPACING + (BAR_W + BAR_SEP + BAR_W) / 2;

    // The two headers are these cells' LABELS — they sit above rather than beside, but they do the
    // same job the label does on every other screen: say which of the two the cursor is on.
    c.draw_text("REV", revCX - (3 * CHAR_W) / 2, y + SEND_HEADER_Y,
                revSendSel ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("DEL", delCX - (3 * CHAR_W) / 2, y + SEND_HEADER_Y,
                delSendSel ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);

    draw_cursor_cell(c, hex2(p.reverbWet), revCX - (2 * CHAR_W) / 2, y + SEND_VALUE_Y, revSendSel,
                     revAudible ? t.textValue : t.textEmpty, t);
    draw_cursor_cell(c, hex2(p.delayWet), delCX - (2 * CHAR_W) / 2, y + SEND_VALUE_Y, delSendSel,
                     delAudible ? t.textValue : t.textEmpty, t);

    // ── The master strip ─────────────────────────────────────────────────────────────────────────
    // Row 2 shows OTT *or* DUST — one control, two destinations, chosen by the EFFECTS screen's TYPE.
    // The label changes with it, so the cell always says which of the two you are turning.
    const bool  isOtt      = (p.masterBusFx == 0);
    const char* depthLabel = isOtt ? "OTT" : "DST";
    const int   depthValue = isOtt ? p.ottDepth : p.dustDepth;

    const bool mixSel   = (cur == MixerCell::MASTER_VOL);
    const bool eqSel    = (cur == MixerCell::MASTER_EQ);
    const bool depthSel = (cur == MixerCell::MASTER_FX);
    const bool limSel   = (cur == MixerCell::LIMITER);

    // Label/value rows as on every other screen. ⚠️ The meter stays lit for all four (`masterSel`):
    // which strip is a different question from which row.
    const auto master_row = [&](const char* label, int row_y, const std::string& value, bool sel) {
        c.draw_text(label, x + MSTR_LABEL_X, y + row_y, sel ? cursor_mark_ink(t) : t.textParam,
                    CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, x + MSTR_VALUE_X, y + row_y, sel, t.textValue, t);
    };

    master_row("MIX", MROW0_Y, hex2(p.masterVolume), mixSel);

    c.draw_text("EQ", x + MSTR_LABEL_X, y + MROW1_Y, eqSel ? cursor_mark_ink(t) : t.textParam,
                CHAR_SPACING, FONT_SCALE);
    draw_eq_cell(c, x + MSTR_VALUE_X, y + MROW1_Y, p.masterEqSlot, eqSel, t);

    master_row(depthLabel, MROW2_Y, hex2(depthValue), depthSel);
    master_row("LIM",      MROW3_Y, hex2(p.limiterPreGain), limSel);
}

void MixerModule::draw_stereo_meter(Canvas& c, int x, int y, int h, float level_l, float level_r,
                                    bool is_selected, bool is_muted, const Theme& t, int peak_idx_l,
                                    int peak_idx_r, unsigned steps) {
    const Argb border = is_selected ? t.rowCursor : t.meterBorder;
    const int  rX     = x + BAR_W + BAR_SEP;

    // ⚠️ `y`/`h` stay the meter's OUTER box — the text rows either side are placed off it — and the
    // trough is what everything drawn inside measures against.
    const int inY = y + METER_INSET;
    const int inH = h - 2 * METER_INSET;

    // One border around the pair (both bars + the gutter between them), then each channel's trough.
    c.fill_rect(x - METER_BORDER, y - 1, BAR_W + BAR_SEP + BAR_W + 2 * METER_BORDER, h + 2, border);
    c.fill_rect(x, inY, BAR_W, inH, t.meterBackground);
    c.fill_rect(rX, inY, BAR_W, inH, t.meterBackground);

    const int lhPx = level_to_height_px(level_l, inH);
    const int rhPx = level_to_height_px(level_r, inH);

    // Replaying a step with the level held is what the missed refreshes did anyway: past the first, the
    // fall is a function of the counter alone.
    for (unsigned i = 0; i < steps; ++i) {
        update_peak(peak_idx_l, lhPx, is_muted);
        update_peak(peak_idx_r, rhPx, is_muted);
    }

    // A muted track shows an empty trough — its peaks are forced to zero above, so the marker falls too.
    if (!is_muted) {
        draw_segmented_bar(c, x, inY, inH, lhPx, t);
        draw_segmented_bar(c, rX, inY, inH, rhPx, t);
    }

    // After the bars, so a marker resting on the trough is still visible.
    draw_peak_marker(c, x, inY, inH, peak_idx_l, t);
    draw_peak_marker(c, rX, inY, inH, peak_idx_r, t);
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

bool MixerModule::peaks_at_rest() const {
    for (float px : peakHoldPx_) {
        if (px > 0.0f) return false;
    }
    return true;
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

    switch (mixer_cell_at(s.mixerMasterRow, s.cursorColumn)) {
        case MixerCell::TRACK_VOL:
            return cc::hex_byte(p.tracks[static_cast<size_t>(s.cursorColumn)].volume, 0, 255, -1, false,
                                false, false, /*def=*/0xFF);
        case MixerCell::MASTER_VOL:
            return cc::hex_byte(p.masterVolume, 0, 255, -1, false, false, false, /*def=*/0xFF);

        // The send returns. A+B resets to 0x80 (unity-ish), not to silence — a send you cannot hear is
        // not a useful default for a control whose whole job is to be dialled in by ear.
        case MixerCell::REV_WET:
            return cc::hex_byte(p.reverbWet, 0, 255, -1, false, false, false, /*def=*/0x80);
        case MixerCell::DLY_WET:
            return cc::hex_byte(p.delayWet, 0, 255, -1, false, false, false, /*def=*/0x80);

        case MixerCell::MASTER_EQ:
            // ⚠️ The −1 is passed THROUGH, so an unassigned master EQ is `isEmpty` and A inserts slot 0.
            // (INSTRUMENT's EQ cell substitutes 0 first, so A there jumps to slot 1.)
            return cc::hex_byte(p.masterEqSlot < 0 ? -1 : p.masterEqSlot, 0, 127,
                                /*empty_value=*/-1, /*can_delete=*/true, /*can_insert=*/true);
        case MixerCell::MASTER_FX:
            return cc::hex_byte(p.masterBusFx == 0 ? p.ottDepth : p.dustDepth, 0, 255, -1, false, false,
                                false, /*def=*/0x00);
        case MixerCell::LIMITER:
            return cc::hex_byte(p.limiterPreGain, 0, 255, -1, false, false, false, /*def=*/0x00);

        // Between cells: answered honestly rather than guessed at, which is what keeps the (row,
        // column) pair safe as two independent ints.
        case MixerCell::NONE:
            break;
    }
    return cc::none();
}

// ─── What the cursor is standing on, by NAME ─────────────────────────────────────────────────────

songcore::MapTarget MixerModule::map_target(const MixerState& s) const {
    using songcore::MapDestId;
    const int col = s.cursorColumn;

    switch (mixer_cell_at(s.mixerMasterRow, col)) {
        case MixerCell::TRACK_VOL:  return {MapDestId::TRACK_VOL, static_cast<uint8_t>(col)};
        case MixerCell::MASTER_VOL: return {MapDestId::MASTER_VOL, 0};
        // The two send returns. Their WET is the mixer's; everything else about those buses is EFFECTS'.
        case MixerCell::REV_WET:    return {MapDestId::REV_WET, 0};
        case MixerCell::DLY_WET:    return {MapDestId::DLY_WET, 0};
        // ⚠️ ONE cell drawing whichever of the two master effects is switched on, so the name it
        // answers with depends on the project — not on the cursor.
        case MixerCell::MASTER_FX:
            return {s.project.masterBusFx == 0 ? MapDestId::OTT_DEPTH : MapDestId::DUST_DEPTH, 0};
        case MixerCell::LIMITER:    return {MapDestId::LIMIT_PRE, 0};
        case MixerCell::MASTER_EQ:  // a choice of preset, not a value to sweep
        case MixerCell::NONE:
            break;
    }
    return {};
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

MixerInputResult MixerModule::handle_input(songcore::Project& p, int cursor_row, int cursor_column,
                                           const InputAction& action) const {
    const auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const MixerCell cell = mixer_cell_at(cursor_row, cursor_column);

    // The master EQ slot is the one cell here that DELETEs and INSERTs rather than only taking a value.
    if (cell == MixerCell::MASTER_EQ) {
        switch (action.type) {
            case ActionType::SET_VALUE:      p.masterEqSlot = clamp(action.value, 0, 127); return {true};
            case ActionType::DELETE:         p.masterEqSlot = -1;                          return {true};
            case ActionType::INSERT_DEFAULT: p.masterEqSlot = 0;                           return {true};
            default:                         return {false};
        }
    }

    if (action.type != ActionType::SET_VALUE) return {false};
    const int v = clamp(action.value, 0, 255);

    switch (cell) {
        case MixerCell::TRACK_VOL:  p.tracks[static_cast<size_t>(cursor_column)].volume = v; return {true};
        case MixerCell::MASTER_VOL: p.masterVolume = v;                                     return {true};
        case MixerCell::REV_WET:    p.reverbWet = v;                                        return {true};
        case MixerCell::DLY_WET:    p.delayWet = v;                                         return {true};
        case MixerCell::MASTER_FX:
            if (p.masterBusFx == 0) p.ottDepth = v;
            else                    p.dustDepth = v;
            return {true};
        case MixerCell::LIMITER:    p.limiterPreGain = v;                                   return {true};
        case MixerCell::MASTER_EQ:  // above
        case MixerCell::NONE:
            break;
    }
    return {false};
}

}  // namespace pt::ui
