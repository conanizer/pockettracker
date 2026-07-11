#ifndef POCKETTRACKER_SONGCORE_TRACE_WRITER_H
#define POCKETTRACKER_SONGCORE_TRACE_WRITER_H

// ─── Conformance-trace writer (encoding 2) ──────────────────────────────────────────────────────
//
// The C++ twin of core/trace/EventTrace.kt: an IMidiConsumer that serializes the router's bus
// records into the frozen schema-v1 trace text (event.h / event-schema.md §6). This is the C++ side
// of the "measuring stick" — the text it produces must equal the Kotlin golden byte-for-byte (after
// the §4 canonical sort, applied by the comparator, not here).
//
// Freezes reproduced verbatim (see EventTrace.kt):
//   * frames are session-relative — the base latches at T PLAY (render = 0; live subtracts the
//     transport-start frame) so traces are position-independent and device↔host comparable;
//   * every payload field always renders, in struct order (fixed line shape);
//   * ints decimal, floats "0x" + exactly 8 uppercase hex digits of the binary32 bits, bools 0/1,
//     instrument 2-hex or -1; '\n' endings; no date/wall-clock anywhere;
//   * events outside a PLAY..STOP session are dropped; T STOP is a no-op with no session open
//     (mirrors stop() running before every play()).

#include <cstdint>
#include <cstdio>
#include <string>
#include "event.h"
#include "router.h"

namespace songcore {

class TraceWriter : public IMidiConsumer {
  public:
    // Attach a sink (an owned std::string) and identify the project in session headers.
    void begin(std::string* out, const std::string& project_sha) {
        out_ = out;
        project_sha_ = project_sha;
        in_session_ = false;
    }
    void end() { out_ = nullptr; in_session_ = false; }

    void on_play(const std::string& kind, const std::string& detail,
                 int64_t start_frame, int tempo, int sample_rate) override {
        if (!out_) return;
        session_start_frame_ = start_frame;
        in_session_ = true;
        const char* mode = (kind == "RENDER") ? "render" : "live";
        *out_ += "# schema=";
        *out_ += std::to_string(SCHEMA_VERSION);
        *out_ += " sr=";     *out_ += std::to_string(sample_rate);
        *out_ += " tempo=";  *out_ += std::to_string(tempo);
        *out_ += " mode=";   *out_ += mode;
        *out_ += " project="; *out_ += project_sha_;
        *out_ += '\n';
        *out_ += "T PLAY "; *out_ += kind; *out_ += ' '; *out_ += detail; *out_ += '\n';
    }

    void on_stop() override {
        if (!in_session_ || !out_) return;
        *out_ += "T STOP\n";
        in_session_ = false;
    }

    void consume(const Event& ev) override {
        if (!out_ || !in_session_) return;
        std::string& s = *out_;
        // prefix: <frame> <track> <instr> <TT>
        s += std::to_string(ev.frame - session_start_frame_); s += ' ';
        s += std::to_string(static_cast<int>(ev.track));      s += ' ';
        s += (ev.instrument < 0 ? std::string("-1") : hex2(ev.instrument)); s += ' ';
        s += hex2(ev.type);

        switch (ev.type) {
            case EV_NOTE_ON: {
                const NoteOnPayload& n = ev.noteOn;
                s += " note=";      s += std::to_string(static_cast<int>(n.note));
                s += " vel=";       s += std::to_string(static_cast<int>(n.velocity));
                s += " velGain=";   s += fbits(n.velGainBits);
                s += " volGain=";   s += fbits(n.volGainBits);
                s += " pan=";       s += fbits(n.panBits);
                s += " start=";     s += std::to_string(n.start);
                s += " slice=";     s += std::to_string(n.slice);
                s += " transpose="; s += std::to_string(n.transpose);
                s += " pit=";       s += std::to_string(n.pit);
                s += " arp=";       s += std::to_string(n.arp);
                s += " tableId=";   s += std::to_string(n.tableId);
                s += " tableRow=";  s += std::to_string(n.tableRow);
                s += " pslOff=";    s += fbits(n.pslOffBits);
                s += " pslDur=";    s += fbits(n.pslDurBits);
                s += " pbnRate=";   s += fbits(n.pbnRateBits);
                s += " vibSpd=";    s += fbits(n.vibSpdBits);
                s += " vibDep=";    s += fbits(n.vibDepBits);
                break;
            }
            case EV_NOTE_OFF:
                s += " mode="; s += std::to_string(static_cast<int>(ev.noteOff.mode));
                break;
            case EV_CC:
                s += " param="; s += std::to_string(static_cast<int>(ev.cc.param));
                s += " value="; s += fbits(ev.cc.valueBits);
                break;
            case EV_EXT_PITCH_RATE:
                s += " rate=";  s += fbits(ev.extPitchRate.rateBits);
                s += " tempo="; s += std::to_string(static_cast<int>(ev.extPitchRate.tempo));
                break;
            case EV_EXT_VIBRATO:
                s += " speed="; s += fbits(ev.extVibrato.speedBits);
                s += " depth="; s += fbits(ev.extVibrato.depthBits);
                break;
            case EV_EXT_TABLE_ROW:
                s += " row="; s += std::to_string(static_cast<int>(ev.extTableRow.row));
                break;
            case EV_EXT_REVERSE:
                s += " reverse="; s += (ev.extReverse.reverse ? "1" : "0");
                s += " restart="; s += (ev.extReverse.restart ? "1" : "0");
                break;
            case EV_EXT_EQ_SLOT:
                s += " slot="; s += std::to_string(static_cast<int>(ev.extEqSlot.slot));
                break;
            case EV_EXT_MASTER_EQ:
                s += " slot="; s += std::to_string(static_cast<int>(ev.extMasterEq.slot));
                break;
            default: break;  // schema-complete: no other emitters exist (event-schema §3)
        }
        s += '\n';
    }

  private:
    // lower 8 bits as 2-digit UPPERCASE hex — mirrors EventTrace.hex2 / model.h hex2.
    static std::string hex2(int v) {
        static const char* H = "0123456789ABCDEF";
        unsigned b = static_cast<unsigned>(v) & 0xFFu;
        std::string s(2, '0');
        s[0] = H[(b >> 4) & 0xF];
        s[1] = H[b & 0xF];
        return s;
    }
    // raw binary32 bits as "0x" + exactly 8 uppercase hex digits — mirrors EventTrace.fbits.
    static std::string fbits(uint32_t bits) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08X", bits);
        return buf;
    }

    std::string* out_ = nullptr;
    std::string project_sha_ = "-";
    int64_t session_start_frame_ = 0;
    bool in_session_ = false;
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_TRACE_WRITER_H
