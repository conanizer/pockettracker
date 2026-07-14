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
constexpr float kSampleRate = 44100.0f;

/**
 * ⚠️ THE CANVAS HAS FOUR PRIMITIVES AND NONE OF THEM IS A LINE (canvas.h), and the EQ editor is the
 * first screen in the port that draws one. This is how, and it is not a workaround.
 *
 * Kotlin strokes a `Path`, but look at how it BUILDS that path: `FloatArray(width) { xi -> … }`, then
 * `for (xi in 1 until width) lineTo(…)`. It is already exactly one sample per pixel column — a
 * single-valued function of x, not a general path. Such a curve needs no line rasterizer: each column
 * is a vertical SPAN from this sample to the next, and a span is a `fill_rect`. That is what a stroked
 * per-pixel polyline rasterizes to anyway, minus Compose's antialiasing.
 *
 * Joining column i to column i+1 (rather than plotting a dot per column) is the part that matters:
 * a 24 dB/oct filter skirt crosses 60 pixels of height in 20 of width, and dots would draw it as a
 * dotted staircase.
 */
void stroke_column_curve(Canvas& c, int x0, int y_top, const int* y, int n, Argb color,
                         int thickness) {
    for (int i = 0; i < n; ++i) {
        const int y0 = y[i];
        const int y1 = (i + 1 < n) ? y[i + 1] : y[i];
        const int lo = std::min(y0, y1);
        const int hi = std::max(y0, y1);
        c.fill_rect(x0 + i, y_top + lo, 1, (hi - lo) + thickness, color);
    }
}

/** The area between a per-column curve and the panel's floor — Kotlin's closed, filled `Path`. */
void fill_under_curve(Canvas& c, int x0, int y_top, const int* y, int n, int bottom, Argb color) {
    for (int i = 0; i < n; ++i) {
        const int top = y[i];
        if (top < bottom) c.fill_rect(x0 + i, y_top + top, 1, bottom - top, color);
    }
}

/**
 * The Z-domain transfer function of the DaisySP double-pass Chamberlin SVF, ported verbatim from
 * `EqModule.svfGainDb`.
 *
 * LOWCUT and HICUT are NOT biquads in this engine — they are the SVF, and the curve has to plot what
 * the audio actually does. DaisySP's `Process()` runs two sequential passes over the same input and
 * averages them, which is why this is not the textbook one-pass expression. Kotlin's derivation is
 * written out above its own copy; it is not repeated here, because the two must not drift and one
 * canonical statement of it is safer than two.
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
        // ⚠️ The division is done in FLOAT and then widened, which is Kotlin's (`hz / 1000f`, boxed to
        // a Float and handed to the formatter). Dividing in double would round differently at the
        // fourth decimal and could land on the other side of a display boundary — and this string is
        // what `step_freq_display_aware` stops on, so it is the CELL, not just the picture.
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

float EqModule::combined_gain_db(const std::vector<songcore::EqBand>& bands, float freq) {
    float total = 0.0f;
    for (const songcore::EqBand& b : bands) {
        if (b.type == 0) continue;  // OFF
        total += band_gain_db(b, freq, kSampleRate);
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

    c.fill_rect(x, vy, WIDTH, VIS_H, t.vizBackground);

    // ── The spectrum, one sample per pixel column ────────────────────────────────────────────────
    // Log-mapped bins come out of the engine already, so bin → pixel is a straight rescale. Fewer than
    // two bins is no spectrum at all (Kotlin's `takeIf { it.size >= 2 }`), and a null one is silence —
    // which is what ptshot draws, and the reason this screen renders with no engine in the process.
    int  specY[WIDTH];
    bool haveSpectrum = (s.spectrum != nullptr && s.spectrumCount >= 2);
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
        }
        // The FILL goes down first, so the grid lines below render on top of it — Kotlin's order.
        fill_under_curve(c, x, vy, specY, WIDTH, bottom, darken(t.textParam, 0.27f));
    }

    // ── The dB grid ─────────────────────────────────────────────────────────────────────────────
    // Axis-aligned, so a "line" here is simply a 1px (or 2px, at 0 dB) rect.
    const int dbLevels[] = {-12, -6, 0, 6, 12};
    for (int db : dbLevels) {
        const int  lineY = db_to_pixel(static_cast<float>(db));
        const Argb col   = (db == 0) ? t.vizCenterLine : t.rowEvery4th;
        c.fill_rect(x, vy + lineY, WIDTH, (db == 0) ? 2 : 1, col);
    }

    // ── The frequency grid, and its labels ──────────────────────────────────────────────────────
    struct Marker { float hz; const char* label; };
    static const Marker markers[] = {
        {20.0f, "20"},    {100.0f, "100"},  {200.0f, "200"},   {500.0f, "500"}, {1000.0f, "1K"},
        {2000.0f, "2K"},  {5000.0f, "5K"},  {10000.0f, "10K"}, {20000.0f, "20K"},
    };
    for (const Marker& m : markers) {
        const int fx = freq_to_pixel(m.hz);
        c.fill_rect(x + fx, vy, 1, VIS_H, t.rowEvery4th);
        c.draw_text(m.label, x + fx + 2, vy + 3, t.vizCenterLine, CHAR_SPACING, 2);
    }

    // The spectrum's own outline, over the grid.
    if (haveSpectrum) stroke_column_curve(c, x, vy, specY, WIDTH, t.textParam, 1);

    // ── The response curve — the point of the whole screen ───────────────────────────────────────
    if (s.slotIndex >= 0 && s.slotIndex < static_cast<int>(s.project.eqPresets.size())) {
        const songcore::EqPreset& preset = s.project.eqPresets[static_cast<size_t>(s.slotIndex)];

        const long long hash = bands_content_hash(preset.bands);
        if (curveCacheSlot_ != s.slotIndex || curveCacheHash_ != hash) {
            for (int xi = 0; xi < WIDTH; ++xi) {
                const float normX = static_cast<float>(xi) / static_cast<float>(WIDTH - 1);
                const float freq  = 20.0f * std::pow(1000.0f, normX);
                curveCacheDb_[xi] = combined_gain_db(preset.bands, freq);
            }
            curveCacheSlot_ = s.slotIndex;
            curveCacheHash_ = hash;
        }

        int curveY[WIDTH];
        for (int xi = 0; xi < WIDTH; ++xi) curveY[xi] = db_to_pixel(curveCacheDb_[xi]);
        stroke_column_curve(c, x, vy, curveY, WIDTH, t.textCursor, 2);
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

        // The cursor highlights the whole ROW — all three bands — because the D-pad's LEFT/RIGHT moves
        // along it, and the row is what you are about to sweep.
        if (isParSel) c.fill_rect(x, rowY, WIDTH, ROW_H, t.rowCursor);

        c.draw_text(kParamLabels[pi], x + 6, rowY + 3, isParSel ? t.textCursor : t.textEmpty,
                    CHAR_SPACING, FONT_SCALE);

        for (int bi = 0; bi < 3; ++bi) {
            const bool isCursor = (bi == curBand && isParSel);
            const Argb col      = isCursor      ? t.textCursor
                                  : (bi == curBand) ? t.textValue
                                                    : t.textEmpty;

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
            c.draw_text(text, bandX[bi] + 6, rowY + 3, col, CHAR_SPACING, FONT_SCALE);
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
            // ⚠️ NOT `cc::hex_byte`, and that is S7's `enum_cycle` trap wearing a different hat. Kotlin
            // builds this context INLINE rather than through its own hexByte factory, and the two are
            // not the same object: hexByte would give largeStep 16 and emptyValue −1, where this has
            // largeStep 1 (there are six types — a "fast" step of 16 is meaningless) and keeps the
            // struct's own 0xFF. Both WRAP and neither deletes, so the behaviour is nearly identical
            // and folding them together is the most natural cleanup imaginable. ptinput byte-compares
            // the CONTEXT, so it is red the moment you do.
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
