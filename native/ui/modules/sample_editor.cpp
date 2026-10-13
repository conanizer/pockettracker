#include "ui/modules/sample_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/helpers.h"
#include "ui/modules/confirm_dialog.h"

namespace pt::ui {

namespace {

/**
 * A bounds-safe `list[i]`. Every index comes from a cycle sized to the list, but a clamp keeps a
 * future slip a visible wrong label rather than a read past the end.
 */
const std::string& at(const std::vector<std::string>& v, int i) {
    static const std::string kEmpty;
    if (v.empty()) return kEmpty;
    return v[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(v.size()) - 1))];
}

/** A vertical 1px rule from (x, y0), `h` tall. */
void v_line(Canvas& c, int x, int y0, int h, Argb color, int thickness = 1) {
    if (h <= 0) return;   // a zero-length butt-capped line draws nothing; neither does this
    c.fill_rect(x, y0, thickness, h, color);
}

}  // namespace

// ─── The vocabularies ────────────────────────────────────────────────────────────────────────────

const std::vector<std::string>& SampleEditorModule::source_values() {
    static const std::vector<std::string> v{"LEFT", "RIGHT", "STEREO", "MONO"};
    return v;
}
const std::vector<std::string>& SampleEditorModule::rate_values() {
    static const std::vector<std::string> v{"HIGH", "NORM", "LOFI"};
    return v;
}
std::vector<std::string> SampleEditorModule::bit_depth_choices(int source_bits) {
    std::vector<std::string> v;
    for (const int bits : {32, 24, 16, 8})
        if (bits <= source_bits) v.push_back(std::to_string(bits));
    if (v.empty()) v.push_back("8");
    return v;
}
const std::vector<std::string>& SampleEditorModule::duration_values() {
    static const std::vector<std::string> v{"4 BAR", "2 BAR", "1 BAR", "1/2",
                                            "1/4",   "1/8",   "1/16",  "1/32"};
    return v;
}
double SampleEditorModule::duration_beats(int duration_index) {
    const int i = std::clamp(duration_index, 0, static_cast<int>(duration_values().size()) - 1);
    return 16.0 / static_cast<double>(1 << i);   // 4 BAR = 16 beats, halving to 1/32 = 0.125
}
const std::vector<std::string>& SampleEditorModule::fx_types() {
    static const std::vector<std::string> v{"OTT", "DUST", "DRIVE", "EQ", "SYNC"};
    return v;
}
const std::vector<std::string>& SampleEditorModule::sync_types() {
    static const std::vector<std::string> v{"RPITCH", "TSTRETCH"};
    return v;
}
const std::vector<std::string>& SampleEditorModule::slice_methods() {
    static const std::vector<std::string> v{"TRANSIENT", "DIVIDE", "OFF", "MANUAL"};
    return v;
}
const std::vector<std::string>& SampleEditorModule::ops_row1() {
    static const std::vector<std::string> v{"CROP", "COPY", "CUT", "DUPL", "PASTE", "DEL"};
    return v;
}
const std::vector<std::string>& SampleEditorModule::ops_row2() {
    static const std::vector<std::string> v{"NORM", "FADE+", "FADE-", "SLNC", "REV", "UNDO"};
    return v;
}

int SampleEditorModule::content_y(int row) { return (row - 8) * ROW_HEIGHT + 228; }

// ─── The sparse row map ──────────────────────────────────────────────────────────────────────────

int SampleEditorModule::row_above(int row, int slice_method) {
    switch (row) {
        case 0:  return 0;
        case 1:  return 19;   // wrap to the last row
        case 8:  return 2;
        case 10: return 8;
        case 13: return (slice_method == SLICE_OFF) ? 10 : 11;
        case 16: return 14;
        case 18: return 16;
        default: return row - 1;
    }
}

int SampleEditorModule::row_below(int row, int slice_method) {
    switch (row) {
        case 2:  return 8;
        case 8:  return 10;
        case 10: return (slice_method == SLICE_OFF) ? 13 : 11;
        case 11: return 13;
        case 14: return 16;
        case 16: return 18;
        case 19: return 1;    // wrap to the first row
        default: return row + 1;
    }
}

int SampleEditorModule::max_col_for_row(int row, int slice_method) {
    if (row == 1 || row == 2) return 2;
    if (row >= 3 && row <= 8) return 1;          // the selection's two edges (START / END)
    if (row == 10) return slice_has_parameter(slice_method) ? 1 : 0;
    if (row == 11) return 1;
    if (row == 13 || row == 14) return 5;        // six ops per row
    if (row == 16) return 2;                     // TYPE / VALUE / APPLY
    if (row == 18) return 0;                     // NAME — the whole row is one cell
    if (row == 19) return (slice_method == SLICE_OFF) ? 2 : 3;   // CHOP only exists with slices
    return 0;
}

// ─── SampleEditorState: the derived values ───────────────────────────────────────────────────────

int SampleEditorState::manual_key_param() const {
    if (sliceMethod == SampleEditorModule::SLICE_TRANSIENT) return sliceSensitivity;
    if (sliceMethod == SampleEditorModule::SLICE_DIVIDE)    return sliceDivisions;
    return 0;   // MANUAL and OFF have no parameter for the markers to be keyed to
}

bool SampleEditorState::manual_markers_live() const {
    // ⚠️ The STAMP alone, not "and the list is not empty": an EMPTY live list (MANUAL with every
    // boundary deleted) must keep overriding the file's cue points. No method is −1, so the stamp
    // is the whole test.
    return manualKeyMethod == sliceMethod && manualKeyParam == manual_key_param();
}

const std::vector<int>& SampleEditorState::method_markers() const {
    // For a method that computes rather than stores: one function-local empty list, nothing to
    // write into.
    static const std::vector<int> computed;

    switch (sliceMethod) {
        case SampleEditorModule::SLICE_TRANSIENT: return transientMarkers;
        // ⭐ OFF and MANUAL both answer with the FILE's cue points — so MANUAL edits the slicing a
        // sample already carries. No `cue ` chunk: an empty list, a from-scratch session.
        case SampleEditorModule::SLICE_OFF:
        case SampleEditorModule::SLICE_MANUAL:    return fileMarkers;
        // DIVIDE derives its boundaries from `sliceDivisions` on every read, so there is no list.
        default:                                  return computed;
    }
}

int SampleEditorState::marker_count() const {
    return static_cast<int>(manual_markers_live() ? manualMarkers.size() : method_markers().size());
}

int64_t SampleEditorState::marker_position(int k) const {
    const int n = marker_count();
    if (k < 0 || n == 0) return 0;
    const int i = std::min(k, n - 1);   // past the end: the boundary an unplaced one is born on
    return manual_markers_live() ? static_cast<int64_t>(manualMarkers[static_cast<size_t>(i)].frame)
                                 : static_cast<int64_t>(method_markers()[static_cast<size_t>(i)]);
}

int SampleEditorState::slice_marker_index() const {
    if (sliceMethod == SampleEditorModule::SLICE_OFF) return -1;
    return sliceIndex - 1;   // −1 at index 0: slice 00's left edge is the sample's own start
}

int SampleEditorState::slice_index_ceiling() const {
    const int markers = marker_count();
    switch (sliceMethod) {
        // N markers make N + 1 slices, so the top slice number is N …
        case SampleEditorModule::SLICE_TRANSIENT: return markers;
        // … and MANUAL reaches ONE FURTHER, onto a slice that does not exist yet; it is born when
        // dragged off the boundary to its left. ⭐ So the list's length IS the rule — no
        // unlocked-count, no duplicate check.
        case SampleEditorModule::SLICE_MANUAL:    return markers + 1;
        case SampleEditorModule::SLICE_DIVIDE:
            return markers > 0 ? markers : std::max(sliceDivisions - 1, 0);
        default:                                  return 0;
    }
}

void SampleEditorState::slice_bounds(int idx, int64_t& start, int64_t& end) const {
    const int64_t total = static_cast<int64_t>(totalFrames);
    if (sliceMethod == SampleEditorModule::SLICE_OFF) {
        start = 0;
        end   = total;
        return;
    }

    // Marker `idx − 1` is slice `idx`'s left edge, marker `idx` its right. A nudged DIVIDE answers
    // with a list too: div − 1 interior markers are div slices.
    const int n = marker_count();
    if (n > 0) {
        // ⚠️ `marker_position` clamps past the end on purpose: MANUAL's free slot is one slice past the
        // last real one, and the boundary it is born on is the last in the list. The RIGHT edge does
        // not clamp — past the last boundary the slice runs to the end of the sample.
        start = (idx == 0) ? 0 : marker_position(idx - 1);
        end   = (idx < n) ? marker_position(idx) : total;
        return;
    }

    if (sliceMethod == SampleEditorModule::SLICE_DIVIDE) {
        const int64_t div = std::max(sliceDivisions, 1);
        start = (static_cast<int64_t>(idx) * total) / div;
        end   = std::min((static_cast<int64_t>(idx + 1) * total) / div, total);
        return;
    }

    start = 0;   // TRANSIENT before the detector has run, and MANUAL on a sample with no boundaries
    end   = total;
}

int64_t SampleEditorState::effective_slice_position() const {
    int64_t start = 0, end = 0;
    slice_bounds(sliceIndex, start, end);
    return start;
}

int64_t SampleEditorState::view_start() const {
    if (zoomLevel == 0 || totalFrames <= 0) return 0;

    const int64_t total   = static_cast<int64_t>(totalFrames);
    const int64_t visible = std::max<int64_t>(total >> zoomLevel, 1);

    const auto window_for = [&](int64_t center) {
        return std::clamp<int64_t>(center - visible / 2, 0, std::max<int64_t>(total - visible, 0));
    };

    // Where the window sits when nothing is sounding: on whichever marker the cursor is pointing at.
    int64_t center;
    if (cursorRow == 8 && cursorCol == 1) {
        center = selectionEnd;
    } else if (cursorRow == 11 && sliceMethod != SampleEditorModule::SLICE_OFF) {
        center = effective_slice_position();
    } else {
        center = selectionStart;   // row 8 col 0, and every other row
    }
    const int64_t anchored = window_for(center);

    // A running playhead pulls the window along only when the selection is too wide to fit;
    // otherwise re-centring every frame would shake a view with nothing to reveal (worst under LOOP).
    if (playbackPosition >= 0.0f &&
        !(selectionStart >= anchored && selectionEnd <= anchored + visible)) {
        return window_for(static_cast<int64_t>(playbackPosition * static_cast<float>(total)));
    }
    return anchored;
}

int64_t SampleEditorState::view_end() const {
    const int64_t total = static_cast<int64_t>(totalFrames);
    if (zoomLevel == 0 || totalFrames <= 0) return total;
    const int64_t visible = std::max<int64_t>(total >> zoomLevel, 1);
    return std::min<int64_t>(view_start() + visible, total);
}

std::string SampleEditorState::duration_display() const {
    if (sampleRate <= 0 || totalFrames <= 0) return "--:--.--";

    // The PENDING pitch shift is previewed here: pitching up shortens the sample, and DURATION should
    // say what SAVE is about to bake rather than what the buffer holds now.
    const double  ratio      = std::pow(2.0, pitchSemitones / 12.0);
    const int64_t effFrames  = std::max<int64_t>(static_cast<int64_t>(totalFrames / ratio), 1);
    const int64_t ms         = (effFrames * 1000) / sampleRate;

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", static_cast<int>(ms / 60000),
                  static_cast<int>((ms % 60000) / 1000), static_cast<int>((ms % 1000) / 10));
    return std::string(buf);
}

int SampleEditorState::source_bpm() const {
    if (sampleRate <= 0 || totalFrames <= 0) return 0;

    const double ratio   = std::pow(2.0, pitchSemitones / 12.0);
    const double seconds = static_cast<double>(totalFrames) / (ratio * sampleRate);
    if (seconds <= 0.0) return 0;

    const long bpm = std::lround(SampleEditorModule::duration_beats(durationIndex) * 60.0 / seconds);
    return (bpm < 1 || bpm > 999) ? 0 : static_cast<int>(bpm);
}

std::string SampleEditorState::bpm_display() const {
    const int bpm = source_bpm();
    return bpm > 0 ? std::to_string(bpm) + "BPM" : "---BPM";
}

void SampleEditorState::reset_stretch() {
    const SampleEditorState fresh;
    durationIndex  = fresh.durationIndex;
    stretchBpm     = fresh.stretchBpm;
    pitchSemitones = fresh.pitchSemitones;
    stretchGrainMs = fresh.stretchGrainMs;
}

bool SampleEditorState::fx_armed() const {
    if (fxType == SampleEditorModule::FX_EQ) return fxEqSlot >= 0;
    return fxType <= SampleEditorModule::FX_DRIVE && fxValue > 0;
}

int SampleEditorState::fx_param() const {
    return (fxType == SampleEditorModule::FX_EQ) ? fxEqSlot : fxValue;
}

// ─── The draw ────────────────────────────────────────────────────────────────────────────────────

namespace {

// ⚠️ NO ROW BACKGROUND ANYWHERE ON THIS SCREEN. The cursor is the CELL it is on, as it is on every
// grid; the label beside a cell is what says which row, and it takes `cursor_mark_ink` with it.

/** Three label/value pairs across one row — rows 1 and 2 are both built from this. */
void draw_label_3val(Canvas& c, int x, int ty, bool is_cur_row, int cur_col, const Theme& t,
                     const std::string& l1, const std::string& v1, int c1,
                     const std::string& l2, const std::string& v2, int c2,
                     const std::string& l3, const std::string& v3, int c3) {
    auto on          = [&](int col) { return is_cur_row && cur_col == col; };
    auto label_color = [&](int col) { return on(col) ? cursor_mark_ink(t) : t.textParam; };

    c.draw_text(l1, x + 10,  ty, label_color(c1), CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v1, x + 110, ty, on(c1), t.textValue, t);
    c.draw_text(l2, x + 180, ty, label_color(c2), CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v2, x + 335, ty, on(c2), t.textValue, t);
    c.draw_text(l3, x + 445, ty, label_color(c3), CHAR_SPACING, FONT_SCALE);
    draw_cursor_cell(c, v3, x + 535, ty, on(c3), t.textValue, t);
}

/** One of the two op rows: six evenly-spaced buttons, the one under the cursor lit. */
void draw_ops_row(Canvas& c, int x, int y, const std::vector<std::string>& ops, bool is_cur,
                  int cur_col, const Theme& t) {
    const int ty   = y + TEXT_PADDING;
    const int colW = SampleEditorModule::WIDTH / static_cast<int>(ops.size());   // ~106px
    for (size_t i = 0; i < ops.size(); ++i) {
        const int lx = x + static_cast<int>(i) * colW + 15;
        draw_cursor_cell(c, ops[i], lx, ty, is_cur && cur_col == static_cast<int>(i), t.textValue, t);
    }
}

}  // namespace

void SampleEditorModule::draw(Canvas& c, int x, int y, const SampleEditorState& s,
                              const Theme& t) const {
    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // ── Rows 0/1/2: a 5px gap, then three 21px rows, then another gap, then the waveform ─────────
    {
        const int ty = y + 5 + TEXT_PADDING;
        c.draw_text("SAMPLE EDITOR", x + 10, ty, t.textTitle, CHAR_SPACING, FONT_SCALE);
        // BPM sits in the gap the title leaves, left of the two facts it is derived from: the rate
        // and the length. Six columns is what fits there, which is what caps the reading at 999.
        c.draw_text(s.bpm_display(), x + 245, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(std::to_string(s.sampleRate) + "Hz", x + 360, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(s.duration_display(), x + 492, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    {   // Row 1 — ZOOM / SOURCE / RATE
        const int  ry  = y + 26;
        const bool cur = (s.cursorRow == 1);
        // SOURCE reads MONO on a mono sample whatever the mode says — there is no second channel to
        // pick from, and the cell is read-only there (see cursor_context).
        const std::string source = s.hasStereoData ? at(source_values(), s.sourceMode) : "MONO";
        draw_label_3val(c, x, ry + TEXT_PADDING, cur, s.cursorCol, t,
                        "ZOOM",   std::to_string(1 << s.zoomLevel) + "X",  0,
                        "SOURCE", source,                                  1,
                        "RATE",   at(rate_values(), s.rateMode),           2);
    }

    {   // Row 2 — PITCH / DURATION / BIT
        const int  ry  = y + 47;
        const bool cur = (s.cursorRow == 2);
        const std::string pitch = (s.pitchSemitones >= 0 ? "+" : "") + std::to_string(s.pitchSemitones);
        draw_label_3val(c, x, ry + TEXT_PADDING, cur, s.cursorCol, t,
                        "PITCH",    pitch,                                  0,
                        "DURATION", at(duration_values(), s.durationIndex), 1,
                        "BIT",      std::to_string(s.bitDepth),             2);
    }

    draw_waveform(c, x, y + WAVEFORM_Y, s, t);

    {   // Row 8 — the selection, in frames
        const int  ry  = y + content_y(8);
        const bool cur = (s.cursorRow == 8);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("SELECTION", x + 10, ty, cur ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                    FONT_SCALE);
        draw_cursor_cell(c, hex8(s.selectionStart), x + 180, ty, cur && s.cursorCol == 0,
                         t.textValue, t);
        draw_cursor_cell(c, hex8(s.selectionEnd), x + 335, ty, cur && s.cursorCol == 1,
                         t.textValue, t);
    }

    {   // Row 10 — the slice METHOD, and its one parameter (SENS or BY, depending)
        const int  ry  = y + content_y(10);
        const bool cur = (s.cursorRow == 10);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("SLICE", x + 10, ty, cur ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, at(slice_methods(), s.sliceMethod), x + 175, ty,
                         cur && s.cursorCol == 0, t.textValue, t);
        if (slice_has_parameter(s.sliceMethod)) {
            const bool        onVal = (cur && s.cursorCol == 1);
            const std::string lbl   = (s.sliceMethod == SLICE_TRANSIENT) ? "SENS" : "BY";
            const std::string val   = hex2(s.sliceMethod == SLICE_TRANSIENT ? s.sliceSensitivity
                                                                            : s.sliceDivisions);
            c.draw_text(lbl, x + 335, ty, onVal ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
            draw_cursor_cell(c, val, x + 410, ty, onVal, t.textValue, t);
        }
    }

    if (s.sliceMethod != SLICE_OFF) {   // Row 11 — which slice, and where it starts
        const int  ry  = y + content_y(11);
        const bool cur = (s.cursorRow == 11);
        const int ty = ry + TEXT_PADDING;

        // The index sits under row 10's METHOD value, the position under its SENS/BY label; `/total`
        // follows the index directly. ⚠️ The row has no label, so the cursor's fill is the only sign
        // the cursor is here.
        draw_cursor_cell(c, hex2(s.sliceIndex), x + 175, ty, cur && s.cursorCol == 0, t.textValue, t);
        if (s.sliceMethod == SLICE_TRANSIENT || s.sliceMethod == SLICE_MANUAL) {
            // How many the index can reach, which is one more than its ceiling. DIVIDE draws none: its
            // count is the BY cell directly above, and saying it twice invites the two to disagree.
            const int total = s.slice_index_ceiling() + 1;
            c.draw_text("/" + hex2(total), x + 175 + 2 * CHAR_W, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        }
        draw_cursor_cell(c, hex8(s.effective_slice_position()), x + 335, ty, cur && s.cursorCol == 1,
                         t.textValue, t);
    }

    draw_ops_row(c, x, y + content_y(13), ops_row1(), s.cursorRow == 13, s.cursorCol, t);
    draw_ops_row(c, x, y + content_y(14), ops_row2(), s.cursorRow == 14, s.cursorCol, t);

    {   // Row 16 — the FX row. Its VALUE cell is the one cell in the app with three vocabularies.
        const int  ry  = y + content_y(16);
        const bool cur = (s.cursorRow == 16);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("EFFECT", x + 10, ty, cur ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, at(fx_types(), s.fxType), x + 180, ty, cur && s.cursorCol == 0,
                         t.textValue, t);

        const bool onVal = (cur && s.cursorCol == 1);
        if (s.fxType == FX_EQ) {
            // An EQ slot, drawn as every other EQ cell is: the number plus the ">" for its editor.
            draw_eq_cell(c, x + 290, ty, s.fxEqSlot, onVal, t);
        } else {
            const std::string val = (s.fxType == FX_SYNC) ? at(sync_types(), s.syncType)
                                                          : hex2(s.fxValue);
            draw_cursor_cell(c, val, x + 290, ty, onVal, t.textValue, t);
        }
        draw_cursor_cell(c, "APPLY", x + 440, ty, cur && s.cursorCol == 2, t.textValue, t);
    }

    {   // Row 18 — NAME. One cell, and no column: the row and the cell are the same thing.
        const int  ry  = y + content_y(18);
        const bool cur = (s.cursorRow == 18);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("NAME", x + 10, ty, cur ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, s.sampleName, x + 120, ty, cur, t.textValue, t);
    }

    {   // Row 19 — LOAD / SAVE / OVERWRITE, and CHOP when there are slices to chop
        const int  ry  = y + content_y(19);
        const bool cur = (s.cursorRow == 19);
        const int ty = ry + TEXT_PADDING;
        auto btn = [&](const char* text, int bx, int i) {
            draw_cursor_cell(c, text, bx, ty, cur && s.cursorCol == i, t.textValue, t);
        };
        btn("LOAD",      x + 120, 0);
        btn("SAVE",      x + 230, 1);
        btn("OVERWRITE", x + 335, 2);
        if (s.sliceMethod != SLICE_OFF) btn("CHOP", x + 510, 3);
    }

    if (s.showConfirmClose) draw_confirm_dialog(c, x, y, t);
}

void SampleEditorModule::draw_waveform(Canvas& c, int x, int y, const SampleEditorState& s,
                                       const Theme& t) const {
    const int wfLeft  = x + 10;
    const int wfRight = wfLeft + WAVEFORM_W;
    /**
     * The last column INSIDE the panel; `wfRight` is one past it. Marks test against the EDGE (the
     * default selection puts E exactly there) and draw at `wfLast`, so an end-of-view mark shows.
     */
    const int wfLast  = wfRight - 1;
    const int midY    = y + WAVEFORM_H / 2;

    c.fill_rect(wfLeft, y, WAVEFORM_W, WAVEFORM_H, t.vizBackground);
    c.fill_rect(wfLeft, midY, WAVEFORM_W, 1, t.vizCenterLine);

    const int64_t viewStart = s.view_start();
    const int64_t viewLen   = std::max<int64_t>(s.view_end() - viewStart, 1);
    const float   viewLenF  = static_cast<float>(viewLen);

    // Frame → pixel, the one mapping every mark on this panel goes through.
    auto frame_x = [&](float frame) {
        return static_cast<float>(wfLeft) +
               ((frame - static_cast<float>(viewStart)) / viewLenF) * static_cast<float>(WAVEFORM_W);
    };

    // ── The wave itself: one vertical rule per bin, from its min to its max ──────────────────────
    if (s.waveformData.size() >= 2) {
        const int   bins  = static_cast<int>(s.waveformData.size()) / 2;
        const float halfH = WAVEFORM_H / 2.0f;
        for (int i = 0; i < bins; ++i) {
            const float minVal = s.waveformData[static_cast<size_t>(i) * 2];
            const float maxVal = s.waveformData[static_cast<size_t>(i) * 2 + 1];

            const int cx  = wfLeft + static_cast<int>(static_cast<float>(i) *
                                                      static_cast<float>(WAVEFORM_W) /
                                                      static_cast<float>(bins));
            const int top = midY - static_cast<int>(maxVal * halfH);
            const int bot = midY - static_cast<int>(minVal * halfH);

            // Bins inside the selection are drawn lit; the rest are dim. That IS the selection —
            // the S/E rules below only mark its edges.
            const int64_t binFrame = viewStart + (static_cast<int64_t>(i) * viewLen) / bins;
            const bool    inSel    = (binFrame >= s.selectionStart && binFrame < s.selectionEnd);
            v_line(c, cx, top, bot - top, inSel ? t.textValue : t.textEmpty);
        }
    } else {
        c.draw_text("WAVEFORM", x + 265, midY - 6, t.textEmpty, CHAR_SPACING, FONT_SCALE);
    }

    if (s.totalFrames <= 0) return;

    // ── The selection's two edges ────────────────────────────────────────────────────────────────
    {
        const float sXf = frame_x(static_cast<float>(s.selectionStart));
        const float eXf = frame_x(static_cast<float>(s.selectionEnd));
        // Test the UNCLAMPED position, draw the clamped one: an edge scrolled out of the window must
        // vanish, not pile up against the panel's border pretending to be there.
        if (sXf >= static_cast<float>(wfLeft) && sXf <= static_cast<float>(wfRight)) {
            const int sX = std::clamp(static_cast<int>(sXf), wfLeft, wfLast);
            v_line(c, sX, y, WAVEFORM_H, t.textTitle);
            c.draw_text("S", sX + 2, y + 3, t.textTitle, CHAR_SPACING, FONT_SCALE);
        }
        if (eXf >= static_cast<float>(wfLeft) && eXf <= static_cast<float>(wfRight)) {
            const int eX = std::clamp(static_cast<int>(eXf), wfLeft, wfLast);
            v_line(c, eX, y, WAVEFORM_H, t.textTitle);
            c.draw_text("E", eX - 17, y + 3, t.textTitle, CHAR_SPACING, FONT_SCALE);
        }
    }

    const bool onSliceRow = (s.cursorRow == 11);

    /** The band behind the slice under the cursor — 10% of the cursor colour, so the wave shows through. */
    auto highlight_slice = [&](int64_t start, int64_t end) {
        const int sX = std::clamp(static_cast<int>(frame_x(static_cast<float>(start))), wfLeft, wfRight);
        const int eX = std::clamp(static_cast<int>(frame_x(static_cast<float>(end))),   wfLeft, wfRight);
        if (eX > sX) c.fill_rect(sX, y, eX - sX, WAVEFORM_H, with_alpha(t.rowCursor, 0.1f));
    };

    /**
     * A slice boundary. ⚠️ The two bounding the CURRENT slice draw DIMMER (`textEmpty`) than the
     * rest — it reads backwards, but it is the established look; swapping it is a UX change.
     */
    auto boundary = [&](int64_t frame, bool active) {
        const float mXf = frame_x(static_cast<float>(frame));
        if (mXf < static_cast<float>(wfLeft) || mXf > static_cast<float>(wfRight)) return;
        v_line(c, std::min(static_cast<int>(mXf), wfLast), y, WAVEFORM_H,
               active ? t.textEmpty : t.textParam);
    };

    // ── The slice boundaries ─────────────────────────────────────────────────────────────────────
    //
    // Whichever list the method answers with: the detector's under TRANSIENT, the file's `cue `
    // chunk read-only under OFF (why a chopped file looks chopped when reopened). DIVIDE computes below.
    const int markerCount = s.marker_count();
    if (markerCount > 0) {
        if (onSliceRow) {
            int64_t start = 0, end = 0;
            s.slice_bounds(s.sliceIndex, start, end);
            highlight_slice(start, end);
        }
        for (int idx = 0; idx < markerCount; ++idx) {
            // Marker `idx` is the RIGHT edge of slice `idx` and the LEFT edge of slice `idx + 1`, so
            // the two bounding the current slice are `sliceIndex − 1` and `sliceIndex`.
            const bool active = onSliceRow && (idx == s.sliceIndex - 1 || idx == s.sliceIndex);
            boundary(s.marker_position(idx), active);
        }
    }

    // ⚠️ Only while DIVIDE is still arithmetic. Once a marker has been nudged the list above IS the
    // boundaries, and computing them a second time would draw the originals over the moved ones.
    if (s.sliceMethod == SLICE_DIVIDE && s.sliceDivisions > 0 && markerCount == 0) {
        const int64_t div   = s.sliceDivisions;
        const int64_t total = static_cast<int64_t>(s.totalFrames);
        if (onSliceRow) {
            highlight_slice((static_cast<int64_t>(s.sliceIndex) * total) / div,
                            std::min((static_cast<int64_t>(s.sliceIndex + 1) * total) / div, total));
        }
        for (int64_t i = 1; i < div; ++i) {
            const bool active = onSliceRow && (i == s.sliceIndex || i == s.sliceIndex + 1);
            boundary((i * total) / div, active);
        }
    }

    // ── The playhead ────────────────────────────────────────────────────────────────────────────
    if (s.playbackPosition >= 0.0f) {
        const float playFrame = s.playbackPosition * static_cast<float>(s.totalFrames);
        const float mXf       = frame_x(playFrame);
        if (mXf >= static_cast<float>(wfLeft) && mXf <= static_cast<float>(wfRight))
            v_line(c, std::min(static_cast<int>(mXf), wfLast - 1), y, WAVEFORM_H, t.vizWave,
                   /*thickness=*/2);
    }
}

void SampleEditorModule::draw_confirm_dialog(Canvas& c, int x, int y, const Theme& t) const {
    // It covers the editor completely. ⚠️ That cover is why it is not in `modal_backdrop_active`: it
    // does not dim, so the shell must not dim the bars either.
    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // THE app's confirm box, asking this screen's question.
    draw_confirm_box(c, "ARE YOU SURE?", t);
}

// ─── The cursor ──────────────────────────────────────────────────────────────────────────────────

CursorContext SampleEditorModule::cursor_context(const SampleEditorState& s) const {
    // The confirm dialog owns every button while it is up. Nothing under it is editable.
    if (s.showConfirmClose) return cc::none();

    switch (s.cursorRow) {
        case 0: return cc::read_only();   // the title bar

        case 1:
            switch (s.cursorCol) {
                case 0: return cc::hex_byte(s.zoomLevel, 0, 4);
                // A mono sample has no channel to choose. The cell still EXISTS (it reads "MONO") —
                // it just cannot be changed, which is what read_only means and none() would not.
                case 1: return s.hasStereoData
                                   ? cc::toggle_ternary(at(source_values(), s.sourceMode), source_values())
                                   : cc::read_only();
                case 2: return cc::toggle_ternary(at(rate_values(), s.rateMode), rate_values());
                default: return cc::none();
            }

        case 2:
            switch (s.cursorCol) {
                // PITCH is ±24 semitones carried as 0..48 with a +24 bias (hex_byte steps an
                // unsigned range); handle_input un-biases. A+B takes it back to +0.
                case 0: return cc::hex_byte(s.pitchSemitones + 24, 0, 48, -1, false, false, false,
                                            /*def=*/24);
                case 1: return cc::toggle_ternary(at(duration_values(), s.durationIndex), duration_values());
                case 2: {
                    // An 8-bit sample has nowhere lower to go: the cell reads 8 and cannot move, as
                    // SOURCE on a mono sample cannot.
                    const std::vector<std::string> choices = bit_depth_choices(s.sourceBitDepth);
                    if (choices.size() < 2) return cc::read_only();
                    return cc::toggle_ternary(std::to_string(s.bitDepth), choices);
                }
                default: return cc::none();
            }

        // Rows 3..8 are the WAVEFORM and SELECTION: A+DPAD DRAGS an edge (zoom-scaled, snapped),
        // which no CursorContext can express — the dispatcher's nudge_selection_edge.
        case 3: case 4: case 5: case 6: case 7: case 8:
            return cc::none();

        case 10:
            switch (s.cursorCol) {
                case 0: return cc::toggle_ternary(at(slice_methods(), s.sliceMethod), slice_methods());
                case 1:
                    if (s.sliceMethod == SLICE_TRANSIENT) return cc::hex_byte(s.sliceSensitivity, 0, 255);
                    if (s.sliceMethod == SLICE_DIVIDE)    return cc::hex_byte(s.sliceDivisions, 0, 255);
                    return cc::none();   // OFF has no parameter
                default: return cc::none();
            }

        case 11:
            // The ceiling is the method's own — see slice_index_ceiling(), which is where the three
            // ways of counting live. Col 1 is the POSITION, and it has no cell to step: A+DPAD DRAGS
            // the marker, exactly as it drags a selection edge on row 8 (nudge_slice_marker).
            if (s.cursorCol == 0 && s.sliceMethod != SLICE_OFF)
                return cc::hex_byte(s.sliceIndex, 0, s.slice_index_ceiling());
            return cc::none();

        case 13: case 14:
            return cc::none();   // the op buttons: A fires them, there is nothing to step

        case 16:
            switch (s.cursorCol) {
                case 0: return cc::toggle_ternary(at(fx_types(), s.fxType), fx_types());
                case 1:
                    // An EQ SLOT, as every EQ cell: "--" is empty, A+RIGHT takes slot 00, A+B clears.
                    if (s.fxType == FX_EQ)
                        return cc::hex_byte(s.fxEqSlot < 0 ? -1 : s.fxEqSlot, 0, 127,
                                            /*empty_value=*/-1, /*can_delete=*/true, /*can_insert=*/true);
                    if (s.fxType == FX_SYNC) return cc::toggle_ternary(at(sync_types(), s.syncType), sync_types());
                    return cc::hex_byte(s.fxValue, 0, 255);                            // an AMOUNT
                default: return cc::none();   // col 2 = APPLY, an action
            }

        case 18: case 19:
            return cc::none();   // NAME opens the keyboard; the save buttons are actions

        default:
            return cc::none();
    }
}

// ─── The edit ────────────────────────────────────────────────────────────────────────────────────

SampleEditorInputResult SampleEditorModule::handle_input(SampleEditorState& s,
                                                         const InputAction& action) const {
    SampleEditorInputResult r;

    // The EQ slot is the one cell here that DELETEs and INSERTs rather than only taking a value.
    if (s.cursorRow == 16 && s.cursorCol == 1 && s.fxType == FX_EQ) {
        switch (action.type) {
            case ActionType::SET_VALUE:      s.fxEqSlot = std::clamp(action.value, 0, 127); break;
            case ActionType::DELETE:         s.fxEqSlot = -1;                               break;
            case ActionType::INSERT_DEFAULT: s.fxEqSlot = 0;                                break;
            default: return r;
        }
        r.modified = true;
        return r;
    }

    if (action.type != ActionType::SET_VALUE) return r;   // every other cell is a SET_VALUE cell
    const int v = action.value;

    switch (s.cursorRow) {
        case 1:
            switch (s.cursorCol) {
                case 0: s.zoomLevel  = std::clamp(v, 0, 4); r.modified = true; break;
                case 1: s.sourceMode = v;                   r.modified = true; break;
                case 2:
                    // The one row on this screen whose edit is DESTRUCTIVE — it re-decimates the audio.
                    // The module records the transition; the dispatcher makes the engine calls.
                    if (v != s.rateMode) r.rateModeChanged = true;
                    s.rateMode = v;
                    r.modified = true;
                    break;
                default: break;
            }
            break;

        case 2:
            switch (s.cursorCol) {
                case 0: s.pitchSemitones = v - 24;  r.modified = true; break;   // un-bias (see cursor_context)
                case 1: s.durationIndex  = v;       r.modified = true; break;
                case 2: {
                    // RATE's twin: destructive, so the dispatcher rebuilds the buffer (applyRateAndBits).
                    const int bits = std::atoi(at(bit_depth_choices(s.sourceBitDepth), v).c_str());
                    if (bits != s.bitDepth) r.bitDepthChanged = true;
                    s.bitDepth = bits;
                    r.modified = true;
                    break;
                }
                default: break;
            }
            break;

        case 10:
            switch (s.cursorCol) {
                case 0:
                    s.sliceMethod = v;
                    // Switching TO transient clears the detector's list, which makes the feed re-run
                    // it. DIVIDE and OFF do not read it, so it is kept for the way back.
                    if (v == SLICE_TRANSIENT) s.transientMarkers.clear();
                    r.modified = true;
                    break;
                case 1:
                    if (s.sliceMethod == SLICE_TRANSIENT) {
                        s.sliceSensitivity = v;
                        s.transientMarkers.clear();   // re-detect at the new threshold
                    } else {
                        s.sliceDivisions = v;
                    }
                    r.modified = true;
                    break;
                default: break;
            }
            break;

        case 11:
            // Stepping the slice index SELECTS that slice — which is the whole gesture: dial through the
            // slices and the selection follows, so CROP/CHOP/preview all act on the one you are looking at.
            if (s.cursorCol == 0 && s.sliceMethod != SLICE_OFF) {
                int64_t start = 0, end = 0;
                s.slice_bounds(v, start, end);
                s.sliceIndex      = v;
                s.selectionStart  = start;
                s.selectionEnd    = end;
                r.modified        = true;
            }
            break;

        case 16:
            switch (s.cursorCol) {
                case 0: s.fxType = v; r.modified = true; break;
                case 1:
                    if (s.fxType == FX_SYNC) s.syncType = v;
                    else                     s.fxValue  = v;
                    r.modified = true;
                    break;
                default: break;
            }
            break;

        default:
            break;
    }

    return r;
}

// ─── The TSTRETCH box ────────────────────────────────────────────────────────────────────────────

namespace {

// Under the waveform, so the playhead of a preview runs in view above the box.
constexpr int STRETCH_BOX_W   = 320;
constexpr int STRETCH_BOX_X   = (DESIGN_W - STRETCH_BOX_W) / 2;
constexpr int STRETCH_BOX_Y   = SampleEditorModule::WAVEFORM_Y + SampleEditorModule::WAVEFORM_H + 8;
constexpr int STRETCH_PAD     = 8;
constexpr int STRETCH_GAP     = 8;    // between the title and the lines
constexpr int STRETCH_LABEL_X = STRETCH_BOX_X + 14;
constexpr int STRETCH_VALUE_X = STRETCH_LABEL_X + 10 * CHAR_W;

/** One line of the box: a label and the cell beside it. No cell = a reading, no label = a blank line. */
struct StretchLine {
    const char* label;
    int         cell;   // a StretchCell, or −1
};
constexpr StretchLine STRETCH_LINES[] = {
    {"ORIG BPM", -1},
    {"DURATION", static_cast<int>(StretchCell::DURATION)},
    {"NEW BPM",  static_cast<int>(StretchCell::BPM)},
    {"PITCH",    static_cast<int>(StretchCell::PITCH)},
    {nullptr,    -1},
    {"GRAIN",    static_cast<int>(StretchCell::GRAIN)},
    {nullptr,    -1},
};
constexpr int STRETCH_LINE_COUNT = static_cast<int>(sizeof(STRETCH_LINES) / sizeof(STRETCH_LINES[0]));

// The title, air, the lines, then the PREVIEW / APPLY line.
constexpr int STRETCH_BOX_H =
    ROW_HEIGHT + STRETCH_GAP + (STRETCH_LINE_COUNT + 1) * ROW_HEIGHT + 2 * STRETCH_PAD;
static_assert(STRETCH_BOX_Y + STRETCH_BOX_H + MODAL_BORDER - 1 <= DESIGN_H,
              "the TSTRETCH box runs off the bottom of the screen");

std::string pad3(int v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%03d", std::clamp(v, 0, 999));
    return std::string(buf);
}

/** PITCH as row 2 shows it: "+0", "+7", "-12". */
std::string signed_semitones(int v) { return (v >= 0 ? "+" : "") + std::to_string(v); }

std::string stretch_value(const SampleEditorState& s, StretchCell cell, int song_tempo) {
    switch (cell) {
        case StretchCell::DURATION: return at(SampleEditorModule::duration_values(), s.durationIndex);
        case StretchCell::BPM:      return pad3(s.stretch_bpm(song_tempo));
        case StretchCell::PITCH:    return signed_semitones(s.pitchSemitones);
        case StretchCell::GRAIN:    return hex2(s.stretchGrainMs);
        default:                    return "";
    }
}

}  // namespace

StretchCell stretch_step(StretchCell from, int d_row, int d_col) {
    const bool onButtons = (from == StretchCell::PREVIEW || from == StretchCell::APPLY);
    if (d_col != 0 && onButtons) return (d_col > 0) ? StretchCell::APPLY : StretchCell::PREVIEW;
    if (d_row == 0) return from;
    if (onButtons) return (d_row < 0) ? StretchCell::GRAIN : from;
    return static_cast<StretchCell>(std::clamp(static_cast<int>(from) + d_row,
                                               static_cast<int>(StretchCell::DURATION),
                                               static_cast<int>(StretchCell::PREVIEW)));
}

CursorContext stretch_cursor_context(const SampleEditorState& s, int song_tempo) {
    using M = SampleEditorModule;
    switch (s.stretchCursor) {
        case StretchCell::DURATION:
            return cc::toggle_ternary(at(M::duration_values(), s.durationIndex), M::duration_values());
        case StretchCell::BPM: {
            // The song TEMPO's own range and steps; A+B goes back to following it.
            CursorContext c = cc::hex_byte(s.stretch_bpm(song_tempo), 20, 999, -1, false, false, false,
                                           /*def=*/song_tempo);
            c.largeStep = 10;
            return c;
        }
        case StretchCell::PITCH:
            // Row 2's own cell and bias; here A+B takes it back to +0.
            return cc::hex_byte(s.pitchSemitones + 24, 0, 48, -1, false, false, false, /*def=*/24);
        case StretchCell::GRAIN:
            return cc::hex_byte(s.stretchGrainMs, 5, 255, -1, false, false, false, /*def=*/40);
        default:
            return cc::none();   // PREVIEW and APPLY are actions
    }
}

void stretch_handle_input(SampleEditorState& s, const InputAction& action, int song_tempo) {
    if (action.type != ActionType::SET_VALUE) return;
    const int v = action.value;
    switch (s.stretchCursor) {
        case StretchCell::DURATION: s.durationIndex = v; break;
        // ⚠️ The song's own tempo is stored as 0, so the cell follows the song rather than remembering it.
        case StretchCell::BPM:      s.stretchBpm = (v == song_tempo) ? 0 : v; break;
        case StretchCell::PITCH:    s.pitchSemitones = v - 24; break;
        case StretchCell::GRAIN:    s.stretchGrainMs = v; break;
        default: break;
    }
}

void draw_stretch_editor(Canvas& c, const SampleEditorState& s, int song_tempo, const Theme& t) {
    if (!s.stretchOpen) return;

    c.reset_clip();
    draw_modal_backdrop(c);
    draw_modal_box(c, STRETCH_BOX_X, STRETCH_BOX_Y, STRETCH_BOX_W, STRETCH_BOX_H, t);

    const std::string title = "TIMESTRETCH";
    int ty = STRETCH_BOX_Y + STRETCH_PAD + TEXT_PADDING;
    c.draw_text(title, STRETCH_BOX_X + (STRETCH_BOX_W - static_cast<int>(title.size()) * CHAR_W) / 2,
                ty, t.textTitle, CHAR_SPACING, FONT_SCALE);
    ty += ROW_HEIGHT + STRETCH_GAP;

    auto on = [&](StretchCell cell) { return s.stretchCursor == cell; };

    for (const StretchLine& line : STRETCH_LINES) {
        if (line.label != nullptr) {
            const bool        isCell = (line.cell >= 0);
            const StretchCell cell   = static_cast<StretchCell>(std::max(line.cell, 0));
            const bool        cur    = isCell && on(cell);
            c.draw_text(line.label, STRETCH_LABEL_X, ty, cur ? cursor_mark_ink(t) : t.textParam,
                        CHAR_SPACING, FONT_SCALE);
            if (isCell) {
                draw_cursor_cell(c, stretch_value(s, cell, song_tempo), STRETCH_VALUE_X, ty, cur,
                                 t.textValue, t);
            } else {
                // The header's reading: the sample's tempo at this DURATION, the PITCH below counted in.
                const int bpm = s.source_bpm();
                c.draw_text(bpm > 0 ? pad3(bpm) : "---", STRETCH_VALUE_X, ty, t.textParam, CHAR_SPACING,
                            FONT_SCALE);
            }
        }
        ty += ROW_HEIGHT;
    }

    draw_cursor_cell(c, "PREVIEW", STRETCH_LABEL_X, ty, on(StretchCell::PREVIEW), t.textValue, t);
    draw_cursor_cell(c, "APPLY",   STRETCH_VALUE_X, ty, on(StretchCell::APPLY),   t.textValue, t);
}

}  // namespace pt::ui
