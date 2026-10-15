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

// The padding between a meter's outline and the text beside it; every bordering row is placed by
// the two helpers below.
constexpr int METER_TEXT_GAP = 3;

// Baseline for a row under a meter.
constexpr int text_y_under(int meter_top, int meter_h) {
    return meter_top + meter_h + 1 + METER_TEXT_GAP + TEXT_PADDING;
}

// Baseline for a row above a meter.
constexpr int text_y_above(int meter_top) {
    return meter_top - 1 - METER_TEXT_GAP - ROW_HEIGHT + TEXT_PADDING;
}

constexpr int TRACK_VOL_Y = text_y_under(TRACK_METER_TOP, TRACK_METER_H);   // 186

// The send returns: headers above the meters, wet values below.
constexpr int SEND_METER_TOP = 234;
constexpr int SEND_METER_H   = 112;
constexpr int SEND_HEADER_Y  = text_y_above(SEND_METER_TOP);                 // 212
constexpr int SEND_VALUE_Y   = text_y_under(SEND_METER_TOP, SEND_METER_H);   // 353

// The outline grows out sideways but in at top and bottom, where the text rows sit close.
constexpr int METER_BORDER = 2;
// What the inward half costs the trough.
constexpr int METER_INSET = METER_BORDER - 1;

// A stereo pair is two slim bars in one outline, the gutter between left unpainted.
constexpr int BAR_W   = 20;
constexpr int BAR_SEP = METER_BORDER;

constexpr int MASTER_X = FIRST_METER_X + 8 * METER_SPACING;   // 434

// The master strip's four value rows, under its taller meter.
constexpr int MROW0_Y = text_y_under(TRACK_METER_TOP, MASTER_METER_H);  // 231
constexpr int MROW1_Y = MROW0_Y + ROW_HEIGHT;                           // 252
constexpr int MROW2_Y = MROW1_Y + ROW_HEIGHT;                           // 273
constexpr int MROW3_Y = MROW2_Y + ROW_HEIGHT;                           // 294

constexpr int MSTR_LABEL_X = MASTER_X - 65;   // 369
constexpr int MSTR_VALUE_X = MASTER_X + 5;    // 439

// LED segments, chunkier than the visualizer's so they do not read as a solid bar.
constexpr int SEG_H    = 4;
constexpr int SEG_GAP  = 1;
constexpr int SEG_STEP = SEG_H + SEG_GAP;

// Peak refreshes the marker hangs before it falls (mixer.h says what a refresh is).
constexpr int PEAK_HOLD_FRAMES = 45;

// The longest fall: the hold, then one segment per refresh down the tallest meter.
constexpr unsigned MAX_HOLD_STEPS =
    static_cast<unsigned>(PEAK_HOLD_FRAMES + MASTER_METER_H / SEG_STEP);

// Zone boundaries as a fraction of height: the meter spans −42..+6 dBFS, so −12 dB is 30/48 up and
// 0 dB 42/48. Fixed to the meter, so colour alone reads the level.
constexpr float LOW_TO_MID_FRAC  = 30.0f / 48.0f;
constexpr float MID_TO_HIGH_FRAC = 42.0f / 48.0f;

// Log, clamped to the meter's dB window.
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

// An unfilled peak array is silence.
float peak_at(const float* peaks, int index) { return peaks ? peaks[index] : 0.0f; }

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void MixerModule::draw(Canvas& c, int x, int y, const MixerState& s) {
    const Theme&             t = s.theme;
    const songcore::Project& p = s.project;

    // ⚠️ The hold ages per peak refresh, not per draw. Unsigned wrap makes the −1 sentinel one step.
    const unsigned steps = std::min(s.peaksVersion - lastPeaksVersion_, MAX_HOLD_STEPS);
    lastPeaksVersion_    = s.peaksVersion;

    const MixerCell cur = mixer_cell_at(s.mixerMasterRow, s.cursorColumn);

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);
    c.draw_text("MIXER", x + 10, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // ── The eight track meters, with their volumes underneath ────────────────────────────────────
    for (int i = 0; i < 8; ++i) {
        const int  mX    = x + FIRST_METER_X + i * METER_SPACING;
        const bool isSel = (cur == MixerCell::TRACK_VOL && s.cursorColumn == i);

        // Muted, unsoloed under another solo, or under a soloed send: meter and value say so.
        const bool audible = track_audible(p, i) && dry_audible(p);

        draw_stereo_meter(c, mX, y + TRACK_METER_TOP, TRACK_METER_H, peak_at(s.trackPeaks, i * 2),
                          peak_at(s.trackPeaks, i * 2 + 1), isSel, /*is_muted=*/!audible,
                          t, i * 2, i * 2 + 1, steps);

        draw_cursor_cell(c, hex2(p.tracks[static_cast<size_t>(i)].volume), mX + 5, y + TRACK_VOL_Y,
                         isSel, audible ? t.textValue : t.textEmpty, t);
    }

    // ── The master meter ─────────────────────────────────────────────────────────────────────────
    // ⚠️ Selected on column alone, so the meter stays lit while the cursor is down on LIM.
    const bool masterSel = (s.cursorColumn == 8);
    draw_stereo_meter(c, x + MASTER_X, y + TRACK_METER_TOP, MASTER_METER_H,
                      peak_at(s.masterPeaks, 0), peak_at(s.masterPeaks, 1), masterSel,
                      /*is_muted=*/false, t, 16, 17, steps);

    // ── The two send returns, side by side under the tracks ──────────────────────────────────────
    const bool revSendSel = (cur == MixerCell::REV_WET);
    const bool delSendSel = (cur == MixerCell::DLY_WET);

    // Muted, or unsoloed while the other return is soloed.
    const bool revAudible = reverb_return_audible(p);
    const bool delAudible = delay_return_audible(p);

    draw_stereo_meter(c, x + FIRST_METER_X, y + SEND_METER_TOP, SEND_METER_H,
                      peak_at(s.reverbPeaks, 0), peak_at(s.reverbPeaks, 1), revSendSel,
                      /*is_muted=*/!revAudible, t, 18, 19, steps);
    draw_stereo_meter(c, x + FIRST_METER_X + METER_SPACING, y + SEND_METER_TOP, SEND_METER_H,
                      peak_at(s.delayPeaks, 0), peak_at(s.delayPeaks, 1), delSendSel,
                      /*is_muted=*/!delAudible, t, 20, 21, steps);

    // Labels centred on their meter pair.
    const int revCX = x + FIRST_METER_X + (BAR_W + BAR_SEP + BAR_W) / 2;
    const int delCX = x + FIRST_METER_X + METER_SPACING + (BAR_W + BAR_SEP + BAR_W) / 2;

    // The headers are these cells' labels, above rather than beside.
    c.draw_text("REV", revCX - (3 * CHAR_W) / 2, y + SEND_HEADER_Y,
                revSendSel ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
    c.draw_text("DEL", delCX - (3 * CHAR_W) / 2, y + SEND_HEADER_Y,
                delSendSel ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);

    draw_cursor_cell(c, hex2(p.reverbWet), revCX - (2 * CHAR_W) / 2, y + SEND_VALUE_Y, revSendSel,
                     revAudible ? t.textValue : t.textEmpty, t);
    draw_cursor_cell(c, hex2(p.delayWet), delCX - (2 * CHAR_W) / 2, y + SEND_VALUE_Y, delSendSel,
                     delAudible ? t.textValue : t.textEmpty, t);

    // ── The master strip ─────────────────────────────────────────────────────────────────────────
    // Row 2 shows OTT or DUST (EFFECTS' TYPE picks); the label says which.
    const bool  isOtt      = (p.masterBusFx == 0);
    const char* depthLabel = isOtt ? "OTT" : "DST";
    const int   depthValue = isOtt ? p.ottDepth : p.dustDepth;

    const bool mixSel   = (cur == MixerCell::MASTER_VOL);
    const bool eqSel    = (cur == MixerCell::MASTER_EQ);
    const bool depthSel = (cur == MixerCell::MASTER_FX);
    const bool limSel   = (cur == MixerCell::LIMITER);

    // Label/value rows; the meter stays lit for all four.
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

    // ⚠️ `y`/`h` stay the outer box the text rows are placed off; the trough is what the inside uses.
    const int inY = y + METER_INSET;
    const int inH = h - 2 * METER_INSET;

    // One border around the pair, then each channel's trough.
    c.fill_rect(x - METER_BORDER, y - 1, BAR_W + BAR_SEP + BAR_W + 2 * METER_BORDER, h + 2, border);
    c.fill_rect(x, inY, BAR_W, inH, t.meterBackground);
    c.fill_rect(rX, inY, BAR_W, inH, t.meterBackground);

    const int lhPx = level_to_height_px(level_l, inH);
    const int rhPx = level_to_height_px(level_r, inH);

    // Past the first missed refresh the fall depends on the counter alone.
    for (unsigned i = 0; i < steps; ++i) {
        update_peak(peak_idx_l, lhPx, is_muted);
        update_peak(peak_idx_r, rhPx, is_muted);
    }

    // A muted track's peaks are forced to zero, so the trough is empty and the marker falls.
    if (!is_muted) {
        draw_segmented_bar(c, x, inY, inH, lhPx, t);
        draw_segmented_bar(c, rX, inY, inH, rhPx, t);
    }

    // After the bars, so a marker on the trough shows.
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

    // Snapped to the LED grid.
    const int peakDy  = (static_cast<int>(peakPx) / SEG_STEP) * SEG_STEP;
    const int peakTop = y + h - peakDy - SEG_H;
    if (peakTop < y) return;   // full scale would draw above the meter

    c.fill_rect(x, peakTop, BAR_W, SEG_H, segment_color(peakDy, h, t));
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MixerModule::cursor_context(const MixerState& s) const {
    const songcore::Project& p = s.project;

    switch (mixer_cell_at(s.mixerMasterRow, s.cursorColumn)) {
        case MixerCell::TRACK_VOL:
            return cc::plain_byte(p.tracks[static_cast<size_t>(s.cursorColumn)].volume, 0xFF);
        case MixerCell::MASTER_VOL:
            return cc::plain_byte(p.masterVolume, 0xFF);

        // The send returns: A+B resets to 0x80 — a silent send is no useful default.
        case MixerCell::REV_WET:
            return cc::plain_byte(p.reverbWet, 0x80);
        case MixerCell::DLY_WET:
            return cc::plain_byte(p.delayWet, 0x80);

        case MixerCell::MASTER_EQ:
            return cc::eq_slot(p.masterEqSlot);
        case MixerCell::MASTER_FX:
            return cc::plain_byte(p.masterBusFx == 0 ? p.ottDepth : p.dustDepth, 0x00);
        case MixerCell::LIMITER:
            return cc::plain_byte(p.limiterPreGain, 0x00);

        // Between cells.
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
        // The returns' WET is the mixer's; the rest of those buses is EFFECTS'.
        case MixerCell::REV_WET:    return {MapDestId::REV_WET, 0};
        case MixerCell::DLY_WET:    return {MapDestId::DLY_WET, 0};
        // ⚠️ One cell for whichever master effect is on, so its name depends on the project.
        case MixerCell::MASTER_FX:
            return {s.project.masterBusFx == 0 ? MapDestId::OTT_DEPTH : MapDestId::DUST_DEPTH, 0};
        case MixerCell::LIMITER:    return {MapDestId::LIMIT_PRE, 0};
        case MixerCell::MASTER_EQ:  // a preset choice, nothing to sweep
        case MixerCell::NONE:
            break;
    }
    return {};
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

MixerInputResult MixerModule::handle_input(songcore::Project& p, int cursor_row, int cursor_column,
                                           const InputAction& action) const {
    const MixerCell cell = mixer_cell_at(cursor_row, cursor_column);

    // The master EQ slot is the one cell here that deletes and inserts.
    if (cell == MixerCell::MASTER_EQ) return {apply_optional_byte(p.masterEqSlot, 127, action)};

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
