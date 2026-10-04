#include "ui/modules/eq_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr float kPi         = 3.14159265358979323846f;
constexpr double kPiD       = 3.14159265358979323846;
// The fallback rate when none is given. ⚠️ Only a fallback: the curve must use the rate the ENGINE's
// bands were built at — bilinear warping is worst near Nyquist and the axis runs to 20 kHz.
constexpr float kDefaultSampleRate = 44100.0f;

/**
 * ⚠️ THE CANVAS HAS NO LINE PRIMITIVE (canvas.h). The curve is one sample per pixel column, so each
 * column is a vertical SPAN from this sample to the next — a `fill_rect`. Joining column i to i+1
 * matters: a steep filter skirt drawn as dots would read as a dotted staircase.
 */
void stroke_column_curve(Canvas& c, int x0, int y_top, const int* y, int n, Argb color,
                         int thickness, int panel_h) {
    // ⚠️ A stroke grows DOWNWARD from its sample, so a curve on the floor would hang below the panel
    // under the separator. Lifting the sample keeps it full width, sitting on the floor.
    const int floorY = panel_h - thickness;
    for (int i = 0; i < n; ++i) {
        const int y0 = std::min(y[i], floorY);
        const int y1 = std::min((i + 1 < n) ? y[i + 1] : y[i], floorY);
        const int lo = std::min(y0, y1);
        const int hi = std::max(y0, y1);
        c.fill_rect(x0 + i, y_top + lo, 1, (hi - lo) + thickness, color);
    }
}

/** The area between a per-column curve and the panel's floor. */
void fill_under_curve(Canvas& c, int x0, int y_top, const int* y, int n, int bottom, Argb color) {
    for (int i = 0; i < n; ++i) {
        const int top = y[i];
        if (top < bottom) c.fill_rect(x0 + i, y_top + top, 1, bottom - top, color);
    }
}

/**
 * The Z-domain transfer function of the DaisySP double-pass Chamberlin SVF. LOWCUT and HICUT are
 * the SVF in this engine, not biquads, and the curve must plot what the audio does. `Process()`
 * runs two passes over the same input and averages them — hence not the textbook expression.
 */
double svf_gain_db(int type, float fc, float q, float viz_freq, float sample_rate) {
    const float  fcC = std::min(fc, sample_rate * 0.45f);
    const double f   = 2.0 * std::sin(kPiD * std::min(0.25, static_cast<double>(fcC) /
                                                                (static_cast<double>(sample_rate) * 2.0)));
    const double res = std::min(0.99, std::max(0.0, 1.0 - 1.0 / (2.0 * static_cast<double>(q))));
    const double d   = std::min(2.0 * (1.0 - std::pow(res, 0.25)), std::min(2.0, 2.0 / f - f * 0.5));

    const double k     = 1.0 - f * d - f * f;
    const double alpha = f * (1.0 + k);
    const double beta  = 1.0 - f * f;
    const double gamma = k * k - f * f;
    const double mu    = f + (d + f) * k;

    const double w    = 2.0 * kPiD * static_cast<double>(viz_freq) / static_cast<double>(sample_rate);
    const double cosW = std::cos(w);
    const double sinW = std::sin(w);

    // D = z² − (β+γ)z + (βγ+α²), at z = e^{jω}
    const double dRe  = std::cos(2.0 * w) - (beta + gamma) * cosW + (beta * gamma + alpha * alpha);
    const double dIm  = std::sin(2.0 * w) - (beta + gamma) * sinW;
    const double dMSq = dRe * dRe + dIm * dIm + 1e-30;

    // B̂ = α(z−1)/D
    const double bNumRe = alpha * (cosW - 1.0);
    const double bNumIm = alpha * sinW;
    const double bRe    = (bNumRe * dRe + bNumIm * dIm) / dMSq;
    const double bIm    = (bNumIm * dRe - bNumRe * dIm) / dMSq;

    // L̂ = (αB̂ + f²) / (z−β)
    const double lNumRe = alpha * bRe + f * f;
    const double lNumIm = alpha * bIm;
    const double zbRe   = cosW - beta;
    const double zbIm   = sinW;
    const double zbMSq  = zbRe * zbRe + zbIm * zbIm + 1e-30;
    const double lRe    = (lNumRe * zbRe + lNumIm * zbIm) / zbMSq;
    const double lIm    = (lNumIm * zbRe - lNumRe * zbIm) / zbMSq;

    const double emjwL_Re = cosW * lRe + sinW * lIm;
    const double emjwL_Im = cosW * lIm - sinW * lRe;
    const double emjwB_Re = cosW * bRe + sinW * bIm;
    const double emjwB_Im = cosW * bIm - sinW * bRe;

    double magSq;
    if (type == 5) {  // HICUT — the low-pass output
        const double hRe = 0.5 * (lRe + emjwL_Re + f * emjwB_Re);
        const double hIm = 0.5 * (lIm + emjwL_Im + f * emjwB_Im);
        magSq            = hRe * hRe + hIm * hIm;
    } else {  // LOWCUT — the high-pass output
        const double onePlusK = 1.0 + k;
        const double dfm      = d + f + mu;
        const double hRe      = 0.5 * onePlusK - 0.5 * (onePlusK * emjwL_Re + dfm * emjwB_Re);
        const double hIm      = -0.5 * (onePlusK * emjwL_Im + dfm * emjwB_Im);
        magSq                 = hRe * hRe + hIm * hIm;
    }

    const double db = 10.0 * std::log10(magSq + 1e-30);
    return std::min(static_cast<double>(EqModule::VIS_DB),
                    std::max(-static_cast<double>(EqModule::VIS_DB), db));
}

/** One band's gain in dB at one frequency. Type 0 (OFF) contributes nothing. */
float band_gain_db(const songcore::EqBand& band, float freq, float sample_rate) {
    const float fc     = 20.0f * std::pow(1000.0f, static_cast<float>(band.freq) / 255.0f);
    const float gainDb = static_cast<float>(band.gain) / 10.0f - 12.0f;
    const float q      = 0.1f * std::pow(100.0f, static_cast<float>(band.q) / 255.0f);

    // LOWCUT / HICUT run through the SVF, not a biquad — see svf_gain_db.
    if (band.type == 2 || band.type == 5)
        return static_cast<float>(svf_gain_db(band.type, fc, q, freq, sample_rate));

    const float w0    = 2.0f * kPi * fc / sample_rate;
    const float w     = 2.0f * kPi * freq / sample_rate;
    const float cosW0 = std::cos(w0);
    const float sinW0 = std::sin(w0);
    const float alpha = sinW0 / (2.0f * q);
    const float cosW  = std::cos(w);
    const float cos2W = std::cos(2.0f * w);
    const float sinW  = std::sin(w);
    const float sin2W = std::sin(2.0f * w);

    const float A = std::pow(10.0f, gainDb / 40.0f);

    float b0, b1, b2, a0, a1, a2;
    switch (band.type) {
        case 1: {  // LOSHELF
            const float sqA = std::sqrt(A);
            b0 = A * ((A + 1) - (A - 1) * cosW0 + 2 * sqA * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cosW0);
            b2 = A * ((A + 1) - (A - 1) * cosW0 - 2 * sqA * alpha);
            a0 = (A + 1) + (A - 1) * cosW0 + 2 * sqA * alpha;
            a1 = -2.0f * ((A - 1) + (A + 1) * cosW0);
            a2 = (A + 1) + (A - 1) * cosW0 - 2 * sqA * alpha;
            break;
        }
        case 3: {  // BELL (peaking)
            b0 = 1 + alpha * A;  b1 = -2 * cosW0;  b2 = 1 - alpha * A;
            a0 = 1 + alpha / A;  a1 = -2 * cosW0;  a2 = 1 - alpha / A;
            break;
        }
        case 4: {  // HISHELF
            const float sqA = std::sqrt(A);
            b0 = A * ((A + 1) + (A - 1) * cosW0 + 2 * sqA * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cosW0);
            b2 = A * ((A + 1) + (A - 1) * cosW0 - 2 * sqA * alpha);
            a0 = (A + 1) - (A - 1) * cosW0 + 2 * sqA * alpha;
            a1 = 2.0f * ((A - 1) - (A + 1) * cosW0);
            a2 = (A + 1) - (A - 1) * cosW0 - 2 * sqA * alpha;
            break;
        }
        default:
            return 0.0f;
    }

    // |H(e^{jω})|, from the complex numerator and denominator.
    const float numRe = b0 + b1 * cosW + b2 * cos2W;
    const float numIm = -(b1 * sinW + b2 * sin2W);
    const float denRe = a0 + a1 * cosW + a2 * cos2W;
    const float denIm = -(a1 * sinW + a2 * sin2W);

    const float magSq =
        (numRe * numRe + numIm * numIm) / (denRe * denRe + denIm * denIm + 1e-30f);
    return 10.0f * std::log10(magSq + 1e-30f);
}

/** The cache key: an EqBand is mutated in place, so only its CONTENTS can say the curve is stale. */
long long bands_content_hash(const std::vector<songcore::EqBand>& bands) {
    long long h = 1;
    for (const songcore::EqBand& b : bands) {
        h = h * 31 + b.type;
        h = h * 31 + b.freq;
        h = h * 31 + b.gain;
        h = h * 31 + b.q;
    }
    return h;
}

}  // namespace

const std::vector<std::string>& eq_band_type_names() {
    static const std::vector<std::string> names = {"OFF", "LOSHELF", "LOWCUT", "BELL", "HISHELF", "HICUT"};
    return names;
}

// ─── The math the header exposes ─────────────────────────────────────────────────────────────────

float EqModule::freq_hz_from_hex(int hex) {
    return 20.0f * std::pow(1000.0f, static_cast<float>(hex) / 255.0f);
}

std::string EqModule::format_freq_hz(float hz) {
    const int rounded = static_cast<int>(hz);
    char      buf[32];
    if (rounded < 1000) {
        std::snprintf(buf, sizeof(buf), "%dHz", rounded);
    } else if (rounded < 10000) {
        // ⚠️ Divided in FLOAT, then widened: double rounds differently at the fourth decimal and can
        // cross a display boundary — and this string is what `step_freq_display_aware` stops on.
        std::snprintf(buf, sizeof(buf), "%.1fkHz", static_cast<double>(hz / 1000.0f));
    } else {
        std::snprintf(buf, sizeof(buf), "%dkHz", static_cast<int>(hz / 1000.0f + 0.5f));
    }
    return buf;
}

std::string EqModule::format_gain_db(float db) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%.1f", (db >= 0.0f ? "+" : ""), static_cast<double>(db));
    return buf;
}

int EqModule::step_freq_display_aware(int old_value, int target) {
    if (std::abs(target - old_value) != 1) return target;  // only a SINGLE step nudges
    const int         dir       = target - old_value;      // +1 or −1
    const std::string old_label = format_freq_hz(freq_hz_from_hex(old_value));

    int v = target;
    while (format_freq_hz(freq_hz_from_hex(v)) == old_label) {
        const int next = v + dir;
        if (next < 0 || next > 255) break;
        v = next;
    }
    return v;
}

int EqModule::db_to_pixel(float db) {
    const int   center = VIS_H / 2;
    const float p      = static_cast<float>(center) - db * (static_cast<float>(VIS_H) / 2.0f) / VIS_DB;
    return std::min(VIS_H - 1, std::max(0, static_cast<int>(p)));
}

int EqModule::freq_to_pixel(float freq) {
    const float t = std::log(freq / 20.0f) / std::log(1000.0f);  // log base-1000 of (freq / 20)
    const int   p = static_cast<int>(t * static_cast<float>(WIDTH - 1));
    return std::min(WIDTH - 1, std::max(0, p));
}

float EqModule::combined_gain_db(const std::vector<songcore::EqBand>& bands, float freq,
                                 float sampleRate) {
    if (sampleRate <= 0.0f) sampleRate = kDefaultSampleRate;
    float total = 0.0f;
    for (const songcore::EqBand& b : bands) {
        if (b.type == 0) continue;  // OFF
        total += band_gain_db(b, freq, sampleRate);
    }
    return std::min(VIS_DB, std::max(-VIS_DB, total));
}

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void EqModule::draw(Canvas& c, int x, int y, const EqState& s) {
    c.fill_rect(x, y, WIDTH, HEIGHT, s.theme.background);
    draw_header(c, x, y, s);
    draw_visualization(c, x, y, s);
    draw_editor(c, x, y, s);
}

void EqModule::draw_header(Canvas& c, int x, int y, const EqState& s) const {
    const Theme& t  = s.theme;
    const int    hY = y + 3;

    c.draw_text("EQ " + songcore::hex2(s.slotIndex), x + 10, hY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    std::string caller;
    switch (s.caller.kind) {
        case EqCallerContext::Kind::MASTER:           caller = "MASTER"; break;
        case EqCallerContext::Kind::REVERB_IN:        caller = "REV IN"; break;
        case EqCallerContext::Kind::DELAY_IN:         caller = "DLY IN"; break;
        case EqCallerContext::Kind::INSTRUMENT:       caller = "INST " + songcore::hex2(s.caller.instrId); break;
        case EqCallerContext::Kind::SAMPLE_EDITOR_FX: caller = "SAMPLE"; break;
    }
    c.draw_text(caller, x + 10 + 8 * CHAR_W, hY, t.textParam, CHAR_SPACING, FONT_SCALE);
}

void EqModule::draw_visualization(Canvas& c, int x, int y, const EqState& s) {
    const Theme& t      = s.theme;
    const int    vy     = y + HEADER_H + ROW_H;
    const int    bottom = VIS_H;  // panel-relative

    // `eqBg`, not `background`: the panel has its own ground so a generated palette can choose; it
    // derives from `vizBackground`.
    c.fill_rect(x, vy, WIDTH, VIS_H, t.eqBg);

    // ── The spectrum, one sample per pixel column ────────────────────────────────────────────────
    // Log-mapped bins come from the engine, so bin → pixel is a straight rescale. Fewer than two
    // bins, or null, is silence.
    int  specY[WIDTH];
    bool haveSpectrum = (s.spectrum != nullptr && s.spectrumCount >= 2);
    // What the idle gate reads back, set from the heights actually drawn — a sub-pixel bar counts as
    // none, so the answer can reach true.
    spectrumAtRest_ = true;
    if (haveSpectrum) {
        const int n = s.spectrumCount;
        for (int xi = 0; xi < WIDTH; ++xi) {
            const int bin0 = std::min(n - 1, std::max(0, static_cast<int>(static_cast<float>(xi) /
                                                                          static_cast<float>(WIDTH - 1) *
                                                                          static_cast<float>(n - 1))));
            const int bin1 = std::min(bin0 + 1, n - 1);
            const float mag = std::max(s.spectrum[bin0], s.spectrum[bin1]);
            const int   h   = std::min(VIS_H, std::max(0, static_cast<int>(mag * static_cast<float>(VIS_H))));
            specY[xi]       = VIS_H - h;
            if (h > 0) spectrumAtRest_ = false;
        }
        // ⚠️ Fill AND outline go down before the grid, so the grid renders over the whole spectrum
        // rather than splitting one shape across it.
        fill_under_curve(c, x, vy, specY, WIDTH, bottom, t.eqFill);
        stroke_column_curve(c, x, vy, specY, WIDTH, t.eqBorder, 1, VIS_H);
    }

    // ── The dB grid ─────────────────────────────────────────────────────────────────────────────
    // Axis-aligned, so a "line" is a 1px (2px at 0 dB) rect.
    // ⚠️ 0 dB takes the spectrum outline's colour on purpose: both are reference marks, and the
    // response curve — the reading — must never be mistaken for one.
    const int dbLevels[] = {-12, -6, 0, 6, 12};
    for (int db : dbLevels) {
        const int  lineY = db_to_pixel(static_cast<float>(db));
        const Argb col   = (db == 0) ? t.eqBorder : t.rowEvery4th;
        c.fill_rect(x, vy + lineY, WIDTH, (db == 0) ? 2 : 1, col);
    }

    // ── The frequency grid, and its labels ──────────────────────────────────────────────────────
    struct Marker { float hz; const char* label; };
    static const Marker markers[] = {
        {20.0f, "20"},    {100.0f, "100"},  {200.0f, "200"},   {500.0f, "500"}, {1000.0f, "1K"},
        {2000.0f, "2K"},  {5000.0f, "5K"},  {10000.0f, "10K"}, {20000.0f, "20K"},
    };
    // ⚠️ A label sits to the LEFT of its own grid line, because the panel's right edge runs under the
    // side bar and a right-hand "20K" is covered by it. 20 Hz is the exception, and the reason the
    // rule cannot be blanket: its line IS the left edge, so its label goes right or off the panel.
    constexpr int LABEL_GAP = 2;
    for (const Marker& m : markers) {
        const int fx = freq_to_pixel(m.hz);
        // A grid line is meant to be a hair off the panel (as ROW 4TH is off BACKGROUND): a guide,
        // not an ink, so it is not held to a contrast floor.
        c.fill_rect(x + fx, vy, 1, VIS_H, t.rowEvery4th);
        const int w  = Canvas::text_width(m.label, CHAR_SPACING, 2);
        const int tx = (fx <= 0) ? x + fx + LABEL_GAP : x + fx - LABEL_GAP - w;
        c.draw_text(m.label, tx, vy + 3, t.eqTxt, CHAR_SPACING, 2);
    }

    // ── The response curve — the point of the whole screen ───────────────────────────────────────
    if (s.slotIndex >= 0 && s.slotIndex < static_cast<int>(s.project.eqPresets.size())) {
        const songcore::EqPreset& preset = s.project.eqPresets[static_cast<size_t>(s.slotIndex)];

        // The cache is keyed on slot and band content only — not on the sample rate — because the
        // audio device opens exactly once, so `s.sampleRate` cannot change under a cached curve.
        const long long hash = bands_content_hash(preset.bands);
        if (curveCacheSlot_ != s.slotIndex || curveCacheHash_ != hash) {
            for (int xi = 0; xi < WIDTH; ++xi) {
                const float normX = static_cast<float>(xi) / static_cast<float>(WIDTH - 1);
                const float freq  = 20.0f * std::pow(1000.0f, normX);
                curveCacheDb_[xi] = combined_gain_db(preset.bands, freq, s.sampleRate);
            }
            curveCacheSlot_ = s.slotIndex;
            curveCacheHash_ = hash;
        }

        int curveY[WIDTH];
        for (int xi = 0; xi < WIDTH; ++xi) curveY[xi] = db_to_pixel(curveCacheDb_[xi]);
        // ⚠️ THE PALETTE'S ACCENT, AND THICKER THAN EVERY REFERENCE MARK ON THE PANEL. It is the one
        // thing on this screen the user is editing, and it crosses the 0 dB line and the spectrum
        // outline constantly — both of which it has to stay legible ON TOP OF, not merely beside.
        stroke_column_curve(c, x, vy, curveY, WIDTH, t.rowCursor, 3, VIS_H);
    }

    c.fill_rect(x, vy + VIS_H, WIDTH, 1, t.vizCenterLine);  // separator
}

void EqModule::draw_editor(Canvas& c, int x, int y, const EqState& s) const {
    const Theme& t        = s.theme;
    const int    edY      = y + EDITOR_Y;
    const int    curBand  = s.cursorRow / 4;
    const int    curParam = s.cursorRow % 4;

    const bool haveSlot =
        (s.slotIndex >= 0 && s.slotIndex < static_cast<int>(s.project.eqPresets.size()));

    const int bandX[3] = {x + LABEL_COL_W, x + LABEL_COL_W + BAND_COL_W,
                          x + LABEL_COL_W + 2 * BAND_COL_W};

    // The band headers: the one under the cursor is a title, the other two are empty-toned.
    for (int bi = 0; bi < 3; ++bi) {
        const Argb col = (bi == curBand) ? t.textTitle : t.textEmpty;
        c.draw_text("BAND " + std::to_string(bi + 1), bandX[bi] + 6, edY + 3, col, CHAR_SPACING,
                    FONT_SCALE);
    }

    static const char* kParamLabels[4] = {"TYPE", "FREQ", "GAIN", "Q"};

    for (int pi = 0; pi < 4; ++pi) {
        const int  rowY     = edY + ROW_H + pi * ROW_H;
        const bool isParSel = (pi == curParam);

        // The cursor is one cell: the parameter label says which row, the band header which column.
        // The other bands print `textEmpty`; the cursor row's label prints `cursor_mark_ink`.
        c.draw_text(kParamLabels[pi], x + 6, rowY + 3, isParSel ? cursor_mark_ink(t) : t.textEmpty,
                    CHAR_SPACING, FONT_SCALE);

        for (int bi = 0; bi < 3; ++bi) {
            const bool isCursor = (bi == curBand && isParSel);
            // The cursor's own cell is not a case: the painter inverts its ink against the bar.
            const Argb col      = (bi == curBand) ? t.textValue : t.textEmpty;

            std::string text = "--";
            if (haveSlot) {
                const songcore::EqPreset& preset = s.project.eqPresets[static_cast<size_t>(s.slotIndex)];
                if (bi < static_cast<int>(preset.bands.size())) {
                    const songcore::EqBand& band = preset.bands[static_cast<size_t>(bi)];
                    switch (pi) {
                        case 0:
                            text = (band.type >= 0 && band.type < static_cast<int>(eq_band_type_names().size()))
                                       ? eq_band_type_names()[static_cast<size_t>(band.type)]
                                       : "???";
                            break;
                        case 1: text = format_freq_hz(freq_hz_from_hex(band.freq)); break;
                        case 2: text = format_gain_db(static_cast<float>(band.gain) / 10.0f - 12.0f); break;
                        default: text = songcore::hex2(band.q); break;
                    }
                }
            }
            draw_cursor_cell(c, text, bandX[bi] + 6, rowY + 3, isCursor, col, t);
        }
    }
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

CursorContext EqModule::cursor_context(const EqState& s) const {
    if (s.slotIndex < 0 || s.slotIndex >= static_cast<int>(s.project.eqPresets.size()))
        return cc::none();

    const songcore::EqPreset& preset   = s.project.eqPresets[static_cast<size_t>(s.slotIndex)];
    const int                 bandIdx  = s.cursorRow / 4;
    if (bandIdx < 0 || bandIdx >= static_cast<int>(preset.bands.size())) return cc::none();
    const songcore::EqBand& band = preset.bands[static_cast<size_t>(bandIdx)];

    switch (s.cursorRow % 4) {
        case 0: {
            // ⚠️ NOT `cc::hex_byte`: largeStep 1 (six types; a fast step of 16 means nothing) and
            // the struct's own 0xFF empty value. Behaviour is nearly identical, but folding them
            // together changes the context.
            CursorContext c;
            c.valueType                     = CursorValueType::HEX_BYTE;
            c.capabilities.canIncrement     = true;
            c.capabilities.canDecrement     = true;
            c.capabilities.canIncrementFast = true;
            c.capabilities.canDecrementFast = true;
            c.currentValue                  = band.type;
            c.minValue                      = 0;
            c.maxValue                      = static_cast<int>(eq_band_type_names().size()) - 1;
            c.smallStep                     = 1;
            c.largeStep                     = 1;
            return c;
        }
        case 1: return cc::freq(band.freq, 0x80);
        case 2: return cc::gain_db(band.gain, 120);  // 0..240 = −12.0..+12.0 dB
        case 3: return cc::hex_byte(band.q, 0, 255, -1, false, false, false, 0x80);
        default: return cc::none();
    }
}

EqInputResult EqModule::handle_input(songcore::Project& project, int slot_index, int cursor_row,
                                     const InputAction& action) const {
    if (action.type != ActionType::SET_VALUE) return {};
    if (slot_index < 0 || slot_index >= static_cast<int>(project.eqPresets.size())) return {};

    songcore::EqPreset& preset  = project.eqPresets[static_cast<size_t>(slot_index)];
    const int           bandIdx = cursor_row / 4;
    if (bandIdx < 0 || bandIdx >= static_cast<int>(preset.bands.size())) return {};
    songcore::EqBand& band = preset.bands[static_cast<size_t>(bandIdx)];

    const int v = action.value;
    switch (cursor_row % 4) {
        case 0:
            band.type = std::min(static_cast<int>(eq_band_type_names().size()) - 1, std::max(0, v));
            break;
        case 1:
            band.freq = step_freq_display_aware(band.freq, std::min(255, std::max(0, v)));
            break;
        case 2:
            band.gain = std::min(240, std::max(0, v));  // 0..240 = −12.0..+12.0 dB, 0.1 dB a step
            break;
        case 3:
            band.q = std::min(255, std::max(0, v));
            break;
        default:
            break;
    }

    EqInputResult r;
    r.modified      = true;
    r.eqBandChanged = true;
    return r;
}

}  // namespace pt::ui
