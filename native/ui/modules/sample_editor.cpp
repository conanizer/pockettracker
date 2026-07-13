#include "ui/modules/sample_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

/**
 * A bounds-safe `list[i]`. Every index here comes from a `toggle_ternary` whose max is the list's own
 * size, so it cannot go out of range — but Kotlin would THROW if one ever did, and a C++ `operator[]`
 * would read off the end of the vector instead. Clamping is the one of the two that stays a bug you
 * can see rather than one you cannot. (`instrument_row_kind` is bounds-safe for the same reason, as
 * Kotlin's own `getOrElse` is there.)
 */
const std::string& at(const std::vector<std::string>& v, int i) {
    static const std::string kEmpty;
    if (v.empty()) return kEmpty;
    return v[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(v.size()) - 1))];
}

/** `IntArray.getOrElse(i) { fallback }`. */
int64_t marker_or(const std::vector<int>& v, int i, int64_t fallback) {
    if (i < 0 || i >= static_cast<int>(v.size())) return fallback;
    return static_cast<int64_t>(v[static_cast<size_t>(i)]);
}

/** A vertical 1px rule — Compose's `drawLine` from (x, y0) to (x, y1) with a butt cap. */
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
const std::vector<std::string>& SampleEditorModule::duration_values() {
    static const std::vector<std::string> v{"4 BAR", "2 BAR", "1 BAR", "1/2",
                                            "1/4",   "1/8",   "1/16",  "1/32"};
    return v;
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
    static const std::vector<std::string> v{"TRANSIENT", "DIVIDE", "OFF"};
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
    if (row == 10) return (slice_method == SLICE_OFF) ? 0 : 1;
    if (row == 11) return 1;
    if (row == 13 || row == 14) return 5;        // six ops per row
    if (row == 16) return 2;                     // TYPE / VALUE / APPLY
    if (row == 18) return 0;                     // NAME — the whole row is one cell
    if (row == 19) return (slice_method == SLICE_OFF) ? 2 : 3;   // CHOP only exists with slices
    return 0;
}

// ─── SampleEditorState: the derived values ───────────────────────────────────────────────────────

void SampleEditorState::slice_bounds(int idx, int64_t& start, int64_t& end) const {
    const int64_t total = static_cast<int64_t>(totalFrames);
    switch (sliceMethod) {
        case SampleEditorModule::SLICE_TRANSIENT: {
            // Marker `idx − 1` is the left edge of slice `idx`, marker `idx` its right edge — so N
            // markers make N + 1 slices, and the first and last are bounded by the sample itself.
            start = (idx == 0) ? 0 : marker_or(transientMarkers, idx - 1, 0);
            end   = marker_or(transientMarkers, idx, total);
            break;
        }
        case SampleEditorModule::SLICE_DIVIDE: {
            const int64_t div = std::max(sliceDivisions, 1);
            start = (static_cast<int64_t>(idx) * total) / div;
            end   = std::min((static_cast<int64_t>(idx + 1) * total) / div, total);
            break;
        }
        default:
            start = 0;
            end   = total;
            break;
    }
}

int64_t SampleEditorState::effective_slice_position() const {
    int64_t start = 0, end = 0;
    slice_bounds(sliceIndex, start, end);
    return start;
}

int64_t SampleEditorState::view_start() const {
    if (zoomLevel == 0 || totalFrames <= 0) return 0;

    // What the window is centred on: the playhead while it runs (so a zoomed view follows the audio
    // instead of watching it leave), otherwise whichever marker the cursor is actually pointing at.
    int64_t center;
    if (playbackPosition >= 0.0f) {
        center = static_cast<int64_t>(playbackPosition * static_cast<float>(totalFrames));
    } else if (cursorRow == 8 && cursorCol == 1) {
        center = selectionEnd;
    } else if (cursorRow == 11 && sliceMethod != SampleEditorModule::SLICE_OFF) {
        center = effective_slice_position();
    } else {
        center = selectionStart;   // row 8 col 0, and every other row
    }

    const int64_t total   = static_cast<int64_t>(totalFrames);
    const int64_t visible = std::max<int64_t>(total >> zoomLevel, 1);
    const int64_t start   = center - visible / 2;
    return std::clamp<int64_t>(start, 0, std::max<int64_t>(total - visible, 0));
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

// ─── The draw ────────────────────────────────────────────────────────────────────────────────────

namespace {

/** The cursor row's full-width band. 640 wide — this screen has no right bar to stay clear of. */
void row_bg(Canvas& c, int x, int y, const Theme& t) {
    c.fill_rect(x, y, SampleEditorModule::WIDTH, ROW_HEIGHT, t.rowCursor);
}

/** Three label/value pairs across one row — rows 1 and 2 are both built from this. */
void draw_label_3val(Canvas& c, int x, int ty, bool is_cur_row, int cur_col, const Theme& t,
                     const std::string& l1, const std::string& v1, int c1,
                     const std::string& l2, const std::string& v2, int c2,
                     const std::string& l3, const std::string& v3, int c3) {
    auto label_color = [&](int col) { return (is_cur_row && cur_col == col) ? t.textCursor : t.textParam; };
    auto value_color = [&](int col) { return (is_cur_row && cur_col == col) ? t.textCursor : t.textValue; };

    c.draw_text(l1, x + 10,  ty, label_color(c1), CHAR_SPACING, FONT_SCALE);
    c.draw_text(v1, x + 110, ty, value_color(c1), CHAR_SPACING, FONT_SCALE);
    c.draw_text(l2, x + 180, ty, label_color(c2), CHAR_SPACING, FONT_SCALE);
    c.draw_text(v2, x + 335, ty, value_color(c2), CHAR_SPACING, FONT_SCALE);
    c.draw_text(l3, x + 445, ty, label_color(c3), CHAR_SPACING, FONT_SCALE);
    c.draw_text(v3, x + 535, ty, value_color(c3), CHAR_SPACING, FONT_SCALE);
}

/** One of the two op rows: six evenly-spaced buttons, the one under the cursor lit. */
void draw_ops_row(Canvas& c, int x, int y, const std::vector<std::string>& ops, bool is_cur,
                  int cur_col, const Theme& t) {
    if (is_cur) row_bg(c, x, y, t);
    const int ty   = y + TEXT_PADDING;
    const int colW = SampleEditorModule::WIDTH / static_cast<int>(ops.size());   // ~106px
    for (size_t i = 0; i < ops.size(); ++i) {
        const int lx = x + static_cast<int>(i) * colW + 15;
        c.draw_text(ops[i], lx, ty,
                    (is_cur && cur_col == static_cast<int>(i)) ? t.textCursor : t.textValue,
                    CHAR_SPACING, FONT_SCALE);
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
        c.draw_text(std::to_string(s.sampleRate) + "Hz", x + 360, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(s.duration_display(), x + 492, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
    }

    {   // Row 1 — ZOOM / SOURCE / RATE
        const int  ry  = y + 26;
        const bool cur = (s.cursorRow == 1);
        if (cur) row_bg(c, x, ry, t);
        // SOURCE reads MONO on a mono sample whatever the mode says — there is no second channel to
        // pick from, and the cell is read-only there (see cursor_context).
        const std::string source = s.hasStereoData ? at(source_values(), s.sourceMode) : "MONO";
        draw_label_3val(c, x, ry + TEXT_PADDING, cur, s.cursorCol, t,
                        "ZOOM",   std::to_string(1 << s.zoomLevel) + "X",  0,
                        "SOURCE", source,                                  1,
                        "RATE",   at(rate_values(), s.rateMode),           2);
    }

    {   // Row 2 — PITCH / DURATION / SNAP
        const int  ry  = y + 47;
        const bool cur = (s.cursorRow == 2);
        if (cur) row_bg(c, x, ry, t);
        const std::string pitch = (s.pitchSemitones >= 0 ? "+" : "") + std::to_string(s.pitchSemitones);
        draw_label_3val(c, x, ry + TEXT_PADDING, cur, s.cursorCol, t,
                        "PITCH",    pitch,                                 0,
                        "DURATION", at(duration_values(), s.durationIndex), 1,
                        "SNAP",     s.snapEnabled ? "ON" : "OFF",          2);
    }

    draw_waveform(c, x, y + WAVEFORM_Y, s, t);

    {   // Row 8 — the selection, in frames
        const int  ry  = y + content_y(8);
        const bool cur = (s.cursorRow == 8);
        if (cur) row_bg(c, x, ry, t);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("SELECTION", x + 10, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(hex8(s.selectionStart), x + 180, ty,
                    (cur && s.cursorCol == 0) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        c.draw_text(hex8(s.selectionEnd), x + 335, ty,
                    (cur && s.cursorCol == 1) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    }

    {   // Row 10 — the slice METHOD, and its one parameter (SENS or BY, depending)
        const int  ry  = y + content_y(10);
        const bool cur = (s.cursorRow == 10);
        if (cur) row_bg(c, x, ry, t);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("SLICE", x + 10, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(at(slice_methods(), s.sliceMethod), x + 175, ty,
                    (cur && s.cursorCol == 0) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        if (s.sliceMethod != SLICE_OFF) {
            const bool        onVal = (cur && s.cursorCol == 1);
            const std::string lbl   = (s.sliceMethod == SLICE_TRANSIENT) ? "SENS" : "BY";
            const std::string val   = hex2(s.sliceMethod == SLICE_TRANSIENT ? s.sliceSensitivity
                                                                            : s.sliceDivisions);
            c.draw_text(lbl, x + 335, ty, onVal ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
            c.draw_text(val, x + 410, ty, onVal ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        }
    }

    if (s.sliceMethod != SLICE_OFF) {   // Row 11 — which slice, and where it starts
        const int  ry  = y + content_y(11);
        const bool cur = (s.cursorRow == 11);
        if (cur) row_bg(c, x, ry, t);
        const int  ty       = ry + TEXT_PADDING;
        const Argb idxColor = (cur && s.cursorCol == 0) ? t.textCursor : t.textValue;
        const Argb posColor = (cur && s.cursorCol == 1) ? t.textCursor : t.textValue;

        c.draw_text(hex2(s.sliceIndex), x + 90, ty, idxColor, CHAR_SPACING, FONT_SCALE);
        if (s.sliceMethod == SLICE_TRANSIENT) {
            // N markers → N + 1 slices, so the total is one more than the marker count.
            const int total = static_cast<int>(s.transientMarkers.size()) + 1;
            c.draw_text("/" + hex2(total), x + 120, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        }
        c.draw_text(hex8(s.effective_slice_position()), x + 175, ty, posColor, CHAR_SPACING, FONT_SCALE);
    }

    draw_ops_row(c, x, y + content_y(13), ops_row1(), s.cursorRow == 13, s.cursorCol, t);
    draw_ops_row(c, x, y + content_y(14), ops_row2(), s.cursorRow == 14, s.cursorCol, t);

    {   // Row 16 — the FX row. Its VALUE cell is the one cell in the app with three vocabularies.
        const int  ry  = y + content_y(16);
        const bool cur = (s.cursorRow == 16);
        if (cur) row_bg(c, x, ry, t);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("EFFECT", x + 10, ty, t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(at(fx_types(), s.fxType), x + 180, ty,
                    (cur && s.cursorCol == 0) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);

        const bool onVal = (cur && s.cursorCol == 1);
        if (s.fxType == FX_EQ) {
            // An EQ slot, drawn as every other EQ cell in the app is — the number plus the ">" that
            // says an editor sits behind it. (That editor is not ported yet; see input_dispatcher.h.)
            draw_eq_cell(c, x + 290, ty, s.fxValue, onVal, t);
        } else {
            const std::string val = (s.fxType == FX_SYNC) ? at(sync_types(), s.syncType)
                                                          : hex2(s.fxValue);
            c.draw_text(val, x + 290, ty, onVal ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        }
        c.draw_text("APPLY", x + 440, ty,
                    (cur && s.cursorCol == 2) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    }

    {   // Row 18 — NAME. One cell: the whole row lights, there is no column.
        const int  ry  = y + content_y(18);
        const bool cur = (s.cursorRow == 18);
        if (cur) row_bg(c, x, ry, t);
        const int ty = ry + TEXT_PADDING;
        c.draw_text("NAME", x + 10, ty, cur ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(s.sampleName, x + 120, ty, cur ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    }

    {   // Row 19 — LOAD / SAVE / OVERWRITE, and CHOP when there are slices to chop
        const int  ry  = y + content_y(19);
        const bool cur = (s.cursorRow == 19);
        if (cur) row_bg(c, x, ry, t);
        const int ty = ry + TEXT_PADDING;
        auto col = [&](int i) { return (cur && s.cursorCol == i) ? t.textCursor : t.textValue; };
        c.draw_text("LOAD",      x + 120, ty, col(0), CHAR_SPACING, FONT_SCALE);
        c.draw_text("SAVE",      x + 230, ty, col(1), CHAR_SPACING, FONT_SCALE);
        c.draw_text("OVERWRITE", x + 335, ty, col(2), CHAR_SPACING, FONT_SCALE);
        if (s.sliceMethod != SLICE_OFF)
            c.draw_text("CHOP",  x + 510, ty, col(3), CHAR_SPACING, FONT_SCALE);
    }

    if (s.showConfirmClose) draw_confirm_dialog(c, x, y, t);
}

void SampleEditorModule::draw_waveform(Canvas& c, int x, int y, const SampleEditorState& s,
                                       const Theme& t) const {
    const int wfLeft  = x + 10;
    const int wfRight = x + 630;
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
            const int sX = std::clamp(static_cast<int>(sXf), wfLeft, wfRight);
            v_line(c, sX, y, WAVEFORM_H, t.textTitle);
            c.draw_text("S", sX + 2, y + 3, t.textTitle, CHAR_SPACING, FONT_SCALE);
        }
        if (eXf >= static_cast<float>(wfLeft) && eXf <= static_cast<float>(wfRight)) {
            const int eX = std::clamp(static_cast<int>(eXf), wfLeft, wfRight);
            v_line(c, eX, y, WAVEFORM_H, t.textTitle);
            c.draw_text("E", eX - 17, y + 3, t.textTitle, CHAR_SPACING, FONT_SCALE);
        }
    }

    const bool onSliceRow = (s.cursorRow == 11);

    /** The band behind the slice under the cursor — 10% of the cursor colour, so the wave shows through. */
    auto highlight_slice = [&](int64_t start, int64_t end) {
        const int sX = std::clamp(static_cast<int>(frame_x(static_cast<float>(start))), wfLeft, wfRight);
        const int eX = std::clamp(static_cast<int>(frame_x(static_cast<float>(end))),   wfLeft, wfRight);
        if (eX > sX) c.fill_rect(sX, y, eX - sX, WAVEFORM_H, with_alpha(t.textCursor, 0.1f));
    };

    /** A slice boundary. The two bounding the CURRENT slice are lit; the rest are dim. */
    auto boundary = [&](int64_t frame, bool active) {
        const float mXf = frame_x(static_cast<float>(frame));
        if (mXf < static_cast<float>(wfLeft) || mXf > static_cast<float>(wfRight)) return;
        v_line(c, static_cast<int>(mXf), y, WAVEFORM_H, active ? t.textEmpty : t.textParam);
    };

    // ── The slice boundaries ─────────────────────────────────────────────────────────────────────
    //
    // TRANSIENT (0) and OFF (2) both draw the MARKERS — in OFF they are read-only, straight from the
    // WAV's `cue ` chunk, and showing them is the whole reason a chopped file looks chopped when you
    // reopen it. DIVIDE (1) ignores them and computes its own N − 1 cuts.
    if ((s.sliceMethod == SLICE_TRANSIENT || s.sliceMethod == SLICE_OFF) &&
        !s.transientMarkers.empty()) {
        if (onSliceRow) {
            int64_t start = 0, end = 0;
            s.slice_bounds(s.sliceIndex, start, end);
            highlight_slice(start, end);
        }
        for (size_t i = 0; i < s.transientMarkers.size(); ++i) {
            const int idx = static_cast<int>(i);
            // Marker `idx` is the RIGHT edge of slice `idx` and the LEFT edge of slice `idx + 1`, so
            // the two bounding the current slice are `sliceIndex − 1` and `sliceIndex`.
            const bool active = onSliceRow && (idx == s.sliceIndex - 1 || idx == s.sliceIndex);
            boundary(s.transientMarkers[i], active);
        }
    }

    if (s.sliceMethod == SLICE_DIVIDE && s.sliceDivisions > 0) {
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
            v_line(c, static_cast<int>(mXf), y, WAVEFORM_H, t.vizWave, /*thickness=*/2);
    }
}

void SampleEditorModule::draw_confirm_dialog(Canvas& c, int x, int y, const Theme& t) const {
    // It covers the editor completely — an unsaved sample is not something to decide about while the
    // waveform is still inviting you to keep editing it.
    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int dX = x + 160, dY = y + 200, dW = 320, dH = 80;
    c.fill_rect(dX, dY, dW, dH, t.meterBackground);
    c.fill_rect(dX, dY, dW, 2, t.textEmpty);   // the 2px rule along its top edge
    c.draw_text("ARE YOU SURE?", dX + 55, dY + 15, t.textValue,  CHAR_SPACING, FONT_SCALE);
    c.draw_text("A=YES  B=NO",   dX + 65, dY + 45, t.textCursor, CHAR_SPACING, FONT_SCALE);
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
                // PITCH is ±24 semitones, carried as 0..48 with a +24 bias — the generic hex-byte
                // handler only knows how to step an unsigned range, so the module biases on the way in
                // and un-biases on the way out (handle_input). Kotlin does exactly this.
                case 0: return cc::hex_byte(s.pitchSemitones + 24, 0, 48);
                case 1: return cc::toggle_ternary(at(duration_values(), s.durationIndex), duration_values());
                case 2: return cc::toggle_binary(s.snapEnabled);
                default: return cc::none();
            }

        // Rows 3..8 are the WAVEFORM and the SELECTION. There is no cell here to increment: A+DPAD
        // DRAGS an edge (by a frame count that depends on the zoom, and snaps to a zero crossing),
        // which no CursorContext can express. The dispatcher handles it directly — see nudge_selection_edge.
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
            // The index's ceiling is the slice COUNT, and the two methods count differently: N markers
            // make N + 1 slices (so the top index is N), while DIVIDE into N makes N (top index N − 1).
            if (s.sliceMethod == SLICE_TRANSIENT && s.cursorCol == 0)
                return cc::hex_byte(s.sliceIndex, 0, static_cast<int>(s.transientMarkers.size()));
            if (s.sliceMethod == SLICE_DIVIDE && s.cursorCol == 0)
                return cc::hex_byte(s.sliceIndex, 0, std::max(s.sliceDivisions - 1, 0));
            return cc::none();

        case 13: case 14:
            return cc::none();   // the op buttons: A fires them, there is nothing to step

        case 16:
            switch (s.cursorCol) {
                case 0: return cc::toggle_ternary(at(fx_types(), s.fxType), fx_types());
                case 1:
                    if (s.fxType == FX_EQ)   return cc::hex_byte(s.fxValue, 0, 127);   // an EQ SLOT
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
    if (action.type != ActionType::SET_VALUE) return r;   // every cell here is a SET_VALUE cell
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
                case 2: s.snapEnabled    = (v == 1); r.modified = true; break;
                default: break;
            }
            break;

        case 10:
            switch (s.cursorCol) {
                case 0:
                    s.sliceMethod = v;
                    // Switching TO transient clears the markers, which is what makes the feed re-run the
                    // detector (it fires on "transient mode with no markers"). Switching to DIVIDE or OFF
                    // KEEPS them — in OFF they become the read-only display of the file's own cue points,
                    // and throwing them away would lose the only copy the editor has.
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

}  // namespace pt::ui
