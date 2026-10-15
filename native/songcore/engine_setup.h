#ifndef POCKETTRACKER_SONGCORE_ENGINE_SETUP_H
#define POCKETTRACKER_SONGCORE_ENGINE_SETUP_H

// The project → engine push. The engine keeps instrument params, the mixer and the global FX across
// project swaps, so a project is pushed before it plays. Params are pushed without I/O; media
// (samples, SoundFonts) are opened by `load_project_media`, which produces the Routing.
// Derived values come from voice_derive.h. Templated so a recorder can stand in for the engine.

#include <cstdio>
#include <string>
#include <vector>

#include "../common/byte_source.h"     // pt_fopen
#include "../common/load_progress.h"   // LoadSpan
#include "../common/sample_formats.h"  // SAMPLE_FORMATS
#include "media_path.h"
#include "midi_map.h"     // MapDestId
#include "model.h"
#include "scheduler.h"    // hex_to_float
#include "traversal.h"    // collect_used_instruments
#include "voice_derive.h" // Routing, push_instrument_mod_eq_sends, push_instrument_playback_params
#include "wav_writer.h"   // read_cue_points

namespace songcore {

// ─── params: no I/O, idempotent ──────────────────────────────────────────────────────────────────

// One instrument's params.
// ⚠️ THE "instrument N changed" seam: the program snapshot the engine plays notes from rides on it.
template <typename Engine>
void push_instrument_params(Engine& engine, const Instrument& ins, const Routing& routing,
                            int tempo, int sampleRate) {
    push_instrument_playback_params(engine, ins);
    push_instrument_mod_eq_sends(engine, ins, tempo, sampleRate);
    // The SF ADSR override — a live key is scheduled with no push, so the engine's copy must be current.
    const SFOverrides& ov = ins.sfOverrides;
    engine.setSoundfontEnvelopeOverride(ins.id, ov.ampAttack, ov.ampDecay, ov.ampSustain, ov.ampRelease);

    const int   sid   = ins.sampleId;
    const float ratio = (sid >= 0 && sid < POOL_INSTRUMENTS) ? routing.sampleRateRatio[sid] : 1.0f;
    const int   slot  = (ins.id >= 0 && ins.id < POOL_INSTRUMENTS) ? routing.sfSlot[ins.id] : -1;
    const Program p = make_program(ins, ratio, slot);
    engine.setProgram(ins.id, p, p.sliceMarkers, p.sliceCount);
}

// The pre-render sweep: every instrument an audible track plays in [startRow, endRow], plus every one
// a table's `INS` can hand a hit to — an instrument with no program is silent.
template <typename Engine>
void push_used_instrument_params(Engine& engine, const Project& project, const Routing& routing,
                                 int startRow, int endRow) {
    const int sampleRate = engine.getSampleRate();
    const int count      = static_cast<int>(project.instruments.size());
    const int tableCount = static_cast<int>(project.tables.size());

    bool wanted[POOL_INSTRUMENTS] = {false};
    std::vector<int> pending;
    for (const int id : collect_used_instruments(project, startRow, endRow)) {
        if (id < 0 || id >= POOL_INSTRUMENTS || wanted[id]) continue;
        wanted[id] = true;
        pending.push_back(id);
    }

    // A table id defaults to its instrument's id; each instrument is visited once.
    while (!pending.empty()) {
        const int id = pending.back();
        pending.pop_back();
        if (id >= tableCount) continue;
        for (const TableRow& row : project.tables[id].rows) {
            const int fxType[3]  = {row.fx1Type,  row.fx2Type,  row.fx3Type};
            const int fxValue[3] = {row.fx1Value, row.fx2Value, row.fx3Value};
            for (int s = 0; s < 3; ++s) {
                if (fxType[s] != FX_INS) continue;
                const int next = fxValue[s] & 0x7F;
                if (next >= POOL_INSTRUMENTS || wanted[next]) continue;
                wanted[next] = true;
                pending.push_back(next);
            }
        }
    }

    for (int id = 0; id < count && id < POOL_INSTRUMENTS; ++id) {
        if (!wanted[id]) continue;
        push_instrument_params(engine, project.instruments[id], routing, project.tempo, sampleRate);
    }
}

// The LIVE sweep: every instrument in the pool, since the user can audition one no step refers to.
template <typename Engine>
void push_all_instrument_params(Engine& engine, const Project& project, const Routing& routing) {
    const int sampleRate = engine.getSampleRate();
    for (const Instrument& ins : project.instruments) {
        push_instrument_params(engine, ins, routing, project.tempo, sampleRate);
    }
}

// What the running song has taken over, so its authored value is NOT pushed on top.
// ⚠️ VTR/VMV, EQM and TIM replace engine state until STOP, so a mid-take push would wipe the fade.
// Per fader, not a blanket skip, or every fader edit would go unheard once a VTR had run.
struct MixerHeld {
    int  faderTracks = 0;       // bit N: a VTR has moved track N's fader this take
    bool masterFader = false;   // a VMV has moved the master fader
    bool masterEq    = false;   // an EQM has moved the master bus off the project's slot
    bool delayTime   = false;   // a TIM has taken the delay's echo time off the DELAY screen's
};

// The EQ bank, the reverb and delay buses and the master EQ. Every slot is pushed, cleared ones too,
// so a previous project's presets are overwritten.
template <typename Engine>
void push_global_effects(Engine& engine, const Project& project, MixerHeld held = {}) {
    const int presets = static_cast<int>(project.eqPresets.size());
    for (int slot = 0; slot < presets; ++slot) {
        const std::vector<EqBand>& bands = project.eqPresets[slot].bands;
        const int bandCount = static_cast<int>(bands.size());
        for (int band = 0; band < 3 && band < bandCount; ++band) {
            const EqBand& b = bands[band];
            engine.setEqBand(slot, band, b.type, b.freq, b.gain, b.q);
        }
    }
    engine.setReverbParams(project.reverbFeedback, project.reverbDamp, project.reverbWet,
                           project.reverbSize);
    engine.setReverbAlgo(project.reverbAlgo);
    engine.setReverbCharacter(project.reverbPreDelay, project.reverbWidth, project.reverbMod);
    engine.setReverbInputEq(project.reverbInputEq);
    // ⚠️ Only the TIME is skipped while a TIM owns it — FDBK and WET edits must still be heard.
    if (held.delayTime) {
        engine.setDelayFeedbackWet(project.delayFeedback, project.delayWet);
    } else {
        engine.setDelayParams(project.delayTime, project.delayFeedback, project.delaySync,
                              static_cast<float>(project.tempo), project.delayWet);
    }
    engine.setDelayCharacter(project.delayPong, project.delayTone, project.delayWobble);
    engine.setDelayInputEq(project.delayInputEq);
    engine.setDelayReverbSend(project.delayReverbSend);
    if (!held.masterEq) engine.setMasterEqSlot(project.masterEqSlot);   // -1 = bypass
}

// The mixer and master bus, then the globals above.
template <typename Engine>
void push_mixer(Engine& engine, const Project& project, MixerHeld held = {}) {
    const int tracks = static_cast<int>(project.tracks.size());
    for (int i = 0; i < 8 && i < tracks; ++i) {
        if (!(held.faderTracks & (1 << i)))
            engine.setTrackVolume(i, hex_to_float(project.tracks[i].volume));
        // ⚠️ All eight, ungated: SOLO ties each track to the other seven, and this line IS the press.
        engine.setTrackMuted(i, !track_audible(project, i));
    }
    // The other channels mute/solo can name; ungated for the same reason.
    engine.setBusMutes(!reverb_return_audible(project), !delay_return_audible(project),
                       !dry_audible(project));
    if (!held.masterFader) engine.setMasterVolume(hex_to_float(project.masterVolume));
    engine.setOttDepth(project.ottDepth);
    engine.setMasterFx(project.masterBusFx);
    engine.setDustDepth(project.dustDepth);
    engine.setLimiterPreGain(project.limiterPreGain);
    push_global_effects(engine, project, held);
}

// A mapped knob's push: only the setter that moved (`push_mixer` is ~140 calls, a knob ~30 a second).
// ⚠️ Never gated by `MixerHeld`: a knob is a press, and the hand wins.
// False for an instrument parameter — that is `push_instrument(id)`, on the host.
template <typename Engine>
bool push_mapped_dest(Engine& engine, const Project& p, MapDestId dest, int scopeIndex) {
    switch (dest) {
        case MapDestId::TRACK_VOL:
            if (scopeIndex < 0 || scopeIndex >= static_cast<int>(p.tracks.size())) return false;
            engine.setTrackVolume(scopeIndex, hex_to_float(p.tracks[scopeIndex].volume));
            return true;
        case MapDestId::MASTER_VOL:
            engine.setMasterVolume(hex_to_float(p.masterVolume));
            return true;

        // Grouped setters: the neighbours go along at their project values.
        case MapDestId::REV_DCAY:
        case MapDestId::REV_DAMP:
        case MapDestId::REV_WET:
        case MapDestId::REV_SIZE:
            engine.setReverbParams(p.reverbFeedback, p.reverbDamp, p.reverbWet, p.reverbSize);
            return true;
        case MapDestId::REV_PRE:
        case MapDestId::REV_WIDE:
        case MapDestId::REV_MOD:
            engine.setReverbCharacter(p.reverbPreDelay, p.reverbWidth, p.reverbMod);
            return true;

        case MapDestId::DLY_TIME:
        case MapDestId::DLY_FDBK:
        case MapDestId::DLY_WET:
            engine.setDelayParams(p.delayTime, p.delayFeedback, p.delaySync,
                                  static_cast<float>(p.tempo), p.delayWet);
            return true;
        case MapDestId::DLY_TONE:
        case MapDestId::DLY_WOBL:
            engine.setDelayCharacter(p.delayPong, p.delayTone, p.delayWobble);
            return true;
        case MapDestId::DLY_SEND:
            engine.setDelayReverbSend(p.delayReverbSend);
            return true;

        case MapDestId::OTT_DEPTH:  engine.setOttDepth(p.ottDepth);             return true;
        case MapDestId::DUST_DEPTH: engine.setDustDepth(p.dustDepth);           return true;
        case MapDestId::LIMIT_PRE:  engine.setLimiterPreGain(p.limiterPreGain); return true;

        default: return false;   // NONE, and every INSTRUMENT-scoped id
    }
}

// The master bus for an export: the *ForRender setters reset rather than fade in, and the master EQ
// goes back to the project's slot so a previous render's EQM cannot bleed in.
template <typename Engine>
void apply_master_bus_for_render(Engine& engine, const Project& project) {
    engine.setMasterFx(project.masterBusFx);
    if (project.masterBusFx == 0) engine.setOttDepthForRender(project.ottDepth);
    else                          engine.setDustDepthForRender(project.dustDepth);
    engine.setLimiterPreGain(project.limiterPreGain);
    engine.setMasterEqSlot(project.masterEqSlot);
}

// The render's param push: mixer, globals, and the instruments the range uses.
template <typename Engine>
void push_project_params(Engine& engine, const Project& project, const Routing& routing,
                         int startRow, int endRow) {
    engine.setTempo(project.tempo);
    push_mixer(engine, project);
    push_used_instrument_params(engine, project, routing, startRow, endRow);
}

// The same for playing: every instrument.
// ⚠️ Call it after a load and after any edit to what the engine keeps; no tool sees a missed call.
template <typename Engine>
void push_live_params(Engine& engine, const Project& project, const Routing& routing) {
    engine.setTempo(project.tempo);
    push_mixer(engine, project);
    push_all_instrument_params(engine, project, routing);
}

// ─── media: opens files, produces the Routing ────────────────────────────────────────────────────

struct MediaLoadResult {
    int loaded = 0;
    int failed = 0;
};

inline std::string path_extension_lower(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return ext;
}

// A sample format other than WAV — one the engine decodes whole (`common/sample_formats.h`).
inline bool is_native_compressed(const std::string& ext) {
    const pt::SampleDecoder d = pt::sample_decoder_for(ext.c_str());
    return d != pt::SampleDecoder::NONE && d != pt::SampleDecoder::WAV;
}

// A WAV's cue points as `Instrument::sliceMarkers` wants them (int64).
inline std::vector<int64_t> read_cue_markers(const std::string& path) {
    const std::vector<int> cues = read_cue_points(path);
    return std::vector<int64_t>(cues.begin(), cues.end());
}

// Load one instrument's sample. Returns the file's rate, or 0 on failure.
template <typename Engine>
int load_sample_file(Engine& engine, int instrumentId, const std::string& path) {
    const std::string ext = path_extension_lower(path);
    if (pt::sample_decoder_for(ext.c_str()) == pt::SampleDecoder::WAV)
        return engine.loadSampleFromWavFile(instrumentId, path.c_str());
    if (is_native_compressed(ext)) return engine.loadSampleFromCompressed(instrumentId, path.c_str());
    return 0;   // unknown or unsupported (raw .aac ADTS, video-only container, …)
}

// Load every instrument's media and fill the Routing: each sample's rate ratio and SoundFont slot.
// ⚠️ The ratio is keyed by `instrument.id` but read by `sampleId`; they coincide in every factory project.
// ⚠️ A WAV's `cue ` chunk REPLACES the .ptp's slice markers (hence the non-const `project`).
template <typename Engine>
MediaLoadResult load_project_media(Engine& engine, Project& project,
                                   const std::string& base_dir, const std::string& app_root,
                                   Routing& routing) {
    // A clean slate, so a previous project's PCM and SoundFonts do not accumulate.
    engine.clearAllSamples();
    engine.clearAllSoundfonts();
    routing.reset();

    MediaLoadResult result;
    const float deviceRate = static_cast<float>(engine.getSampleRate());

    // One progress bar: each instrument with a source gets an equal slice of 0..1.
    int sources = 0;
    for (const Instrument& ins : project.instruments) {
        if (ins.id < 0 || ins.id >= POOL_INSTRUMENTS) continue;
        if ((ins.instrumentType == InstrumentType::SOUNDFONT && ins.soundfontPath.has_value()) ||
            ins.sampleFilePath.has_value())
            sources++;
    }
    int loadedSoFar = 0;
    const auto slice = [&sources, &loadedSoFar]() {
        const float n  = static_cast<float>(sources > 0 ? sources : 1);
        const float lo = static_cast<float>(loadedSoFar) / n;
        return pt::LoadSpan(lo, lo + 1.0f / n);
    };

    for (Instrument& ins : project.instruments) {
        if (ins.id < 0 || ins.id >= POOL_INSTRUMENTS) continue;

        if (ins.instrumentType == InstrumentType::SOUNDFONT && ins.soundfontPath.has_value()) {
            const std::string path = resolve_media_path(*ins.soundfontPath, base_dir, app_root);
            const auto span = slice();
            const int slot = engine.loadSoundfont(ins.id, path.c_str(), ins.sfBank, ins.sfPreset);
            loadedSoFar++;
            if (slot >= 0) {
                routing.sfSlot[ins.id] = slot;
                result.loaded++;
            } else {
                result.failed++;
            }
            if (pt::load_cancelled()) break;
        } else if (ins.sampleFilePath.has_value()) {
            const std::string path = resolve_media_path(*ins.sampleFilePath, base_dir, app_root);
            const auto span = slice();
            const int fileRate = load_sample_file(engine, ins.id, path);
            loadedSoFar++;
            if (fileRate > 0) {
                routing.sampleRateRatio[ins.id] = deviceRate / static_cast<float>(fileRate);
                if (!is_native_compressed(path_extension_lower(path)))
                    ins.sliceMarkers = read_cue_markers(path);
                result.loaded++;
            } else {
                result.failed++;
            }
            // ⚠️ A cancel leaves the document naming sources the engine lacks; the caller restores it.
            if (pt::load_cancelled()) break;
        }
    }
    return result;
}

// ─── The instrument operations: the verbs that own a SOURCE ──────────────────────────────────────
// The SoundFont path→slot map lives in the engine; keep no second copy here. The preset queries read
// the file's index, not a loaded slot.

// The instrument's SoundFont path as it opens on this install, or empty.
// ⚠️ Every engine call goes through this, never `ins.soundfontPath`: slot compares need this spelling.
inline std::string instrument_soundfont_path(const Instrument& ins, const MediaRoots& roots) {
    if (!ins.soundfontPath.has_value()) return std::string();
    return resolve_media_path(*ins.soundfontPath, roots);
}


template <typename Engine>
int soundfont_preset_count(Engine& engine, const Instrument& ins, const MediaRoots& roots) {
    const std::string path = instrument_soundfont_path(ins, roots);
    if (path.empty()) return 0;
    return engine.getSoundfontFilePresetCount(path.c_str());
}

// The list index of the current bank+preset, or 0.
template <typename Engine>
int soundfont_preset_index(Engine& engine, const Instrument& ins, const MediaRoots& roots) {
    const std::string path = instrument_soundfont_path(ins, roots);
    if (path.empty()) return 0;
    const int count = engine.getSoundfontFilePresetCount(path.c_str());
    for (int i = 0; i < count; ++i) {
        int bank = -1, preset = -1;
        if (!engine.getSoundfontFilePresetAt(path.c_str(), i, &bank, &preset)) continue;
        if (bank == ins.sfBank && preset == ins.sfPreset) return i;
    }
    return 0;
}


template <typename Engine>
std::string soundfont_preset_name(Engine& engine, const Instrument& ins, const MediaRoots& roots) {
    const std::string path = instrument_soundfont_path(ins, roots);
    if (path.empty()) return "---";
    return engine.getSoundfontFilePresetName(path.c_str(), ins.sfBank, ins.sfPreset);
}

// The PATCH row: bank+preset only, so scrolling is free; `sync_instrument_soundfont` loads later.
template <typename Engine>
bool set_soundfont_preset_by_index(Engine& engine, Instrument& ins, int index,
                                   const MediaRoots& roots) {
    const std::string path = instrument_soundfont_path(ins, roots);
    if (path.empty()) return false;
    int bank = -1, preset = -1;
    if (!engine.getSoundfontFilePresetAt(path.c_str(), index, &bank, &preset) || bank < 0)
        return false;
    ins.sfBank   = bank;
    ins.sfPreset = preset;
    return true;
}

// Free every SoundFont slot no instrument is routed to (moving PATCH orphans the old preset).
// ⚠️ "Referenced" means `routing.sfSlot`, NOT the path: a project from another install holds re-rooted
// paths in its slots, and a path compare would free — and silence — every one of them.
template <typename Engine>
void release_unreferenced_soundfonts(Engine& engine, const Routing& routing) {
    const int slots = engine.soundfontSlotCount();
    for (int s = 0; s < slots; ++s) {
        bool inUse = false;
        for (int i = 0; i < POOL_INSTRUMENTS; ++i)
            if (routing.sfSlot[i] == s) { inUse = true; break; }
        if (!inUse) engine.unloadSoundfont(s);
    }
}

// Make the instrument's slot hold the sound it names, loading if needed.
template <typename Engine>
bool sync_instrument_soundfont(Engine& engine, const Instrument& ins, Routing& routing,
                               const MediaRoots& roots) {
    if (ins.instrumentType != InstrumentType::SOUNDFONT) return false;
    const std::string resolved = instrument_soundfont_path(ins, roots);
    if (resolved.empty()) return false;
    const char* path = resolved.c_str();
    if (engine.soundfontSlotHolds(routing.sfSlot[ins.id], path, ins.sfBank, ins.sfPreset)) return true;

    const int slot = engine.loadSoundfont(ins.id, path, ins.sfBank, ins.sfPreset);
    if (slot < 0) return false;
    routing.sfSlot[ins.id] = slot;
    release_unreferenced_soundfonts(engine, routing);
    return true;
}

// The same on a worker (a decode can take ~300 ms); `collect` installs it, the old sound plays until
// then. False when the engine is busy — nothing is queued, so the caller asks again.
template <typename Engine>
bool request_instrument_soundfont(Engine& engine, const Instrument& ins, Routing& routing,
                                  const MediaRoots& roots) {
    if (ins.instrumentType != InstrumentType::SOUNDFONT) return true;
    const std::string resolved = instrument_soundfont_path(ins, roots);
    if (resolved.empty()) return true;
    const char* path = resolved.c_str();
    if (engine.soundfontSlotHolds(routing.sfSlot[ins.id], path, ins.sfBank, ins.sfPreset)) return true;

    int ready = -1;
    const auto answer = engine.requestSoundfontLoad(ins.id, path, ins.sfBank, ins.sfPreset, &ready);
    if (answer == Engine::SfRequest::BUSY) return false;
    if (answer == Engine::SfRequest::READY && ready >= 0) {
        routing.sfSlot[ins.id] = ready;
        release_unreferenced_soundfonts(engine, routing);
    }
    return true;
}

// Install a finished background load; call once a frame. Returns the instrument whose slot moved, or -1.
// ⚠️ A note's slot is read when it is scheduled, so the caller must reschedule to hear it mid-take.
template <typename Engine>
int collect_instrument_soundfont(Engine& engine, const Project& project, Routing& routing,
                                 const MediaRoots& roots) {
    int id = -1, slot = -1;
    if (!engine.collectSoundfontLoad(&id, &slot)) return -1;
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return -1;

    // ⚠️ The row may have moved on while this decoded: check the instrument as it is NOW.
    const Instrument& ins = project.instruments[static_cast<size_t>(id)];
    const std::string resolved = instrument_soundfont_path(ins, roots);
    bool moved = false;
    if (slot >= 0 && ins.instrumentType == InstrumentType::SOUNDFONT && !resolved.empty() &&
        engine.soundfontSlotHolds(slot, resolved.c_str(), ins.sfBank, ins.sfPreset)) {
        moved = routing.sfSlot[id] != slot;
        routing.sfSlot[id] = slot;
    }
    release_unreferenced_soundfonts(engine, routing);
    return moved ? id : -1;
}

// Change an instrument's TYPE, freeing the source the old type owned.
// ⚠️ `engine` may be null (headless tests): guard the engine calls, never the document.
template <typename Engine>
void set_instrument_type(Engine* engine, Project& project, int id, InstrumentType newType,
                         Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return;
    Instrument& ins = project.instruments[id];
    ins.instrumentType = newType;

    // ⚠️ Two independent tests, not an if/else: EXTERNAL owns neither source and must free both.
    if (newType != InstrumentType::SAMPLER) {
        ins.sampleFilePath.reset();
        if (engine) engine->clearSample(id);
        routing.sampleRateRatio[id] = 1.0f;
    }
    if (newType != InstrumentType::SOUNDFONT) {
        ins.soundfontPath.reset();
        routing.sfSlot[id] = -1;
        if (engine) release_unreferenced_soundfonts(*engine, routing);
    }
}

// Reset a slot to empty, KEEPING its type. `engine` may be null.
template <typename Engine>
void clear_instrument(Engine* engine, Project& project, int id, Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return;

    const InstrumentType keepType = project.instruments[id].instrumentType;

    Instrument fresh(id);
    fresh.sampleId       = id;   // the factory value
    fresh.instrumentType = keepType;
    project.instruments[id] = std::move(fresh);

    if (engine) engine->clearSample(id);
    routing.sampleRateRatio[id] = 1.0f;

    routing.sfSlot[id] = -1;
    if (engine) release_unreferenced_soundfonts(*engine, routing);
}

// Two sample slots above the pool, owned by no project: the browser's audition (255) and the sample
// editor's source preview (254). A real load frees them.

inline constexpr int PREVIEW_SAMPLE_SLOT = 255;
inline constexpr int SOURCE_PREVIEW_SLOT = 254;

template <typename Engine>
void clear_preview_slots(Engine& engine) {
    engine.clearSample(SOURCE_PREVIEW_SLOT);
    engine.clearSample(PREVIEW_SAMPLE_SLOT);
}

// The browser's audition: decoded into slot 255, played at C-4 on the preview lane, rate ratio
// applied. The one note that skips `plan_note_on` — there is no instrument. Returns the file's rate or 0.
template <typename Engine>
int preview_sample_file(Engine& engine, const std::string& path) {
    engine.scheduleKill(engine.getCurrentFrame(), Engine::PREVIEW_LANE);   // the previous audition

    const int fileRate = load_sample_file(engine, PREVIEW_SAMPLE_SLOT, path);
    if (fileRate <= 0) return 0;

    engine.requestResume();
    const float deviceRate = static_cast<float>(engine.getSampleRate());
    const float baseFreq   = C4_HZ * (deviceRate / static_cast<float>(fileRate));

    engine.scheduleNote(engine.getCurrentFrame() + 100, PREVIEW_SAMPLE_SLOT, Engine::PREVIEW_LANE,
                        /*frequency=*/C4_HZ, /*baseFrequency=*/baseFreq, /*volume=*/1.0f,
                        /*phraseVolume=*/1.0f, /*pan=*/0.5f);
    return fileRate;
}

// ─── The browser's loads into one instrument: these also write the path into the document ────────

// Load a sample into instrument `id`. A compressed source keeps its own path (no WAV is written).
// Slice markers come from a WAV's `cue ` chunk; otherwise they are cleared.
template <typename Engine>
bool load_instrument_sample(Engine* engine, Project& project, int id, const std::string& path,
                            Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return false;

    // No engine, no decode — and a path the engine never opened must not enter the document.
    if (!engine) return false;

    const int fileRate = load_sample_file(*engine, id, path);
    if (fileRate <= 0) return false;

    Instrument& ins   = project.instruments[static_cast<size_t>(id)];
    ins.sampleFilePath = path;
    ins.sampleId       = id;
    ins.sliceMarkers   = is_native_compressed(path_extension_lower(path))
                             ? std::vector<int64_t>{}
                             : read_cue_markers(path);

    const float deviceRate = static_cast<float>(engine->getSampleRate());
    routing.sampleRateRatio[id] = deviceRate / static_cast<float>(fileRate);

    clear_preview_slots(*engine);
    return true;
}

// Load a SoundFont into instrument `id` on its FIRST listed preset (0/0 is not in every file).
template <typename Engine>
bool load_instrument_soundfont(Engine* engine, Project& project, int id, const std::string& path,
                               Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return false;
    if (!engine) return false;

    int bank = -1, preset = -1;
    if (!engine->getSoundfontFilePresetAt(path.c_str(), 0, &bank, &preset) || bank < 0) return false;

    const int slot = engine->loadSoundfont(id, path.c_str(), bank, preset);
    if (slot < 0) return false;

    Instrument& ins    = project.instruments[static_cast<size_t>(id)];
    ins.soundfontPath  = path;
    ins.instrumentType = InstrumentType::SOUNDFONT;
    ins.sampleFilePath.reset();   // the slot's old sampler source is gone with the type change
    routing.sfSlot[id] = slot;
    ins.sfBank   = bank;
    ins.sfPreset = preset;

    release_unreferenced_soundfonts(*engine, routing);

    clear_preview_slots(*engine);
    return true;
}

// Apply a .pti to instrument `id`: its parameters, its table (into `id`'s own) and its source.
// False only if the source failed to load; the parameters apply regardless.
template <typename Engine>
bool apply_instrument_preset(Engine* engine, Project& project, int id, const InstrumentPreset& preset,
                             Routing& routing, const MediaRoots& roots) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return false;

    Instrument&       dst = project.instruments[static_cast<size_t>(id)];
    const Instrument& src = preset.instrument;

    const int keepId = dst.id;
    dst = src;
    dst.id       = keepId;
    dst.sampleId = keepId;
    dst.sampleFilePath.reset();
    dst.soundfontPath.reset();

    if (preset.tableRows.has_value() && id < POOL_TABLES) {
        Table& table = project.tables[static_cast<size_t>(id)];
        const std::vector<TableRow>& rows = *preset.tableRows;
        for (size_t i = 0; i < rows.size() && i < table.rows.size(); ++i) table.rows[i] = rows[i];
        dst.tableId = id;
    }

    if (src.instrumentType == InstrumentType::SOUNDFONT) {
        if (!src.soundfontPath.has_value()) return true;   // params-only preset
        // A .pti authored on another install names its .sf2 under THAT install's root.
        const std::string sfPath = resolve_media_path(*src.soundfontPath, roots);
        if (!load_instrument_soundfont(engine, project, id, sfPath, routing)) return false;

        // The .pti's own preset, if the file still has it.
        Instrument& ins = project.instruments[static_cast<size_t>(id)];
        if (engine && ins.soundfontPath.has_value() &&
            engine->getSoundfontFilePresetName(sfPath.c_str(), src.sfBank, src.sfPreset) != "---") {
            ins.sfBank   = src.sfBank;
            ins.sfPreset = src.sfPreset;
            sync_instrument_soundfont(*engine, ins, routing, roots);
        }
        return true;
    }

    if (!src.sampleFilePath.has_value()) return true;      // params-only preset
    if (!load_instrument_sample(engine, project, id, *src.sampleFilePath, routing)) return false;

    // The preset's markers win over the file's.
    project.instruments[static_cast<size_t>(id)].sliceMarkers = src.sliceMarkers;
    return true;
}

// The .pti for instrument `id`; its table travels only if it has content.
inline InstrumentPreset make_instrument_preset(const Project& project, int id) {
    InstrumentPreset ip;
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return ip;

    ip.instrument = project.instruments[static_cast<size_t>(id)];

    const Instrument& ins = ip.instrument;
    const int tableId = (ins.tableId >= 0 && ins.tableId < static_cast<int>(project.tables.size()))
                            ? ins.tableId
                            : ins.id;
    if (tableId < 0 || tableId >= static_cast<int>(project.tables.size())) return ip;

    const std::vector<TableRow>& rows = project.tables[static_cast<size_t>(tableId)].rows;
    bool hasContent = false;
    for (const TableRow& r : rows)
        if (r.transpose != 0 || r.volume != -1 || r.fx1Type != 0) { hasContent = true; break; }
    if (hasContent) ip.tableRows = rows;

    return ip;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_ENGINE_SETUP_H
