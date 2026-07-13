#ifndef POCKETTRACKER_SONGCORE_ENGINE_SETUP_H
#define POCKETTRACKER_SONGCORE_ENGINE_SETUP_H

// ─── The project → engine push ───────────────────────────────────────────────────────────────────
//
// Everything the app does to get a Project *into* the engine: the per-instrument playback params, the
// modulation slots, the EQ/send routing, the mixer, and the global FX state. Ported from
// RenderController.setupInstrumentParams + applyMasterBusForRender and AppInputDispatcher's
// syncVolumesToAudioBackend / pushGlobalEffectsToBackend.
//
// WHY IT HAS TO EXIST (S6b). The engine keeps mixer / master-bus / EQ-bank / send state across project
// swaps — none of it is per-voice — so "play this project" is not just scheduling notes: someone must
// push all of that down first. On Android that someone was Kotlin, which meant a host renderer
// (tools/ptrender) or the SDL shell could not make a sound at all, no matter how correct the
// scheduler was. S7 (deleting the Kotlin path) and Linux Phase 2 need exactly this file.
//
// Two halves, split by lifetime:
//
//   push_project_params  — pure param pushes, no I/O. Cheap, idempotent, safe to repeat. A render
//                          prepares with it; the SDL shell calls it after every load and edit.
//   load_project_media   — opens FILES (samples, SF2s) and *produces* the Routing. Used by ptrender
//                          and the SDL shell. Android still loads media in Kotlin, whose loader also
//                          drives MediaCodec for m4a and reads WAV cue points — see the note there.
//
// Float exactness: every derived value goes through voice_derive.h, whose derivations are byte-
// goldened against the real Kotlin code by tools/ptvoice (77 cases). Nothing is re-derived here.
//
// Template over the engine, like voice_derive.h — AudioEngine satisfies it as-is, and a recorder can
// be substituted in a host test without an interface or a virtual call.

#include <string>
#include <vector>

#include "model.h"
#include "scheduler.h"    // hex_to_float (VolumeUtils.hexToFloat)
#include "traversal.h"    // collect_used_instruments
#include "voice_derive.h" // Routing, push_instrument_mod_eq_sends, push_instrument_playback_params

namespace songcore {

// ─── params: no I/O, idempotent ──────────────────────────────────────────────────────────────────

// RenderController.setupInstrumentParams, for one instrument. Kotlin's SF branch and sampler branch
// push the *same* three things — applySoundfontFilterOverrides is just updateInstrumentPlaybackParams
// under another name — so there is deliberately one path here, not two.
template <typename Engine>
void push_instrument_params(Engine& engine, const Instrument& ins, int tempo, int sampleRate) {
    push_instrument_playback_params(engine, ins);
    push_instrument_mod_eq_sends(engine, ins, tempo, sampleRate);
}

// The pre-render sweep: every instrument any non-muted step in rows [startRow, endRow] plays.
template <typename Engine>
void push_used_instrument_params(Engine& engine, const Project& project, int startRow, int endRow) {
    const int sampleRate = engine.getSampleRate();
    const int count      = static_cast<int>(project.instruments.size());
    for (const int id : collect_used_instruments(project, startRow, endRow)) {
        if (id < 0 || id >= count) continue;
        push_instrument_params(engine, project.instruments[id], project.tempo, sampleRate);
    }
}

// The LIVE sweep: every instrument in the pool, played or not.
//
// A render only needs the instruments the rows it is exporting actually use. An interactive app cannot
// make that assumption for a second: you can sit on the INSTRUMENT screen and audition slot 7F while no
// step in the song refers to it, and its filter and drive must already be in the engine when you do.
template <typename Engine>
void push_all_instrument_params(Engine& engine, const Project& project) {
    const int sampleRate = engine.getSampleRate();
    for (const Instrument& ins : project.instruments) {
        push_instrument_params(engine, ins, project.tempo, sampleRate);
    }
}

// AppInputDispatcher.pushGlobalEffectsToBackend — the state that lives ONLY in the engine and so
// survives a project swap: the 128-slot EQ preset bank, the reverb and delay buses (+ their input EQ
// and the delay→reverb send), and the master EQ. Without it a loaded project's reverb/delay keep
// sounding like the previous project's until the user nudges each control.
//
// Every slot and band is pushed, including cleared (type = 0) ones, so a previous project's presets
// are fully overwritten rather than partially.
template <typename Engine>
void push_global_effects(Engine& engine, const Project& project) {
    const int presets = static_cast<int>(project.eqPresets.size());
    for (int slot = 0; slot < presets; ++slot) {
        const std::vector<EqBand>& bands = project.eqPresets[slot].bands;
        const int bandCount = static_cast<int>(bands.size());
        for (int band = 0; band < 3 && band < bandCount; ++band) {
            const EqBand& b = bands[band];
            engine.setEqBand(slot, band, b.type, b.freq, b.gain, b.q);
        }
    }
    engine.setReverbParams(project.reverbFeedback, project.reverbDamp, project.reverbWet);
    engine.setReverbInputEq(project.reverbInputEq);
    engine.setDelayParams(project.delayTime, project.delayFeedback, project.delaySync,
                          static_cast<float>(project.tempo), project.delayWet);
    engine.setDelayInputEq(project.delayInputEq);
    engine.setDelayReverbSend(project.delayReverbSend);
    engine.setMasterEqSlot(project.masterEqSlot);   // -1 = bypass
}

// AppInputDispatcher.syncVolumesToAudioBackend — the mixer and master bus, then the globals above.
template <typename Engine>
void push_mixer(Engine& engine, const Project& project) {
    const int tracks = static_cast<int>(project.tracks.size());
    for (int i = 0; i < 8 && i < tracks; ++i) {
        engine.setTrackVolume(i, hex_to_float(project.tracks[i].volume));
    }
    engine.setMasterVolume(hex_to_float(project.masterVolume));
    engine.setOttDepth(project.ottDepth);
    engine.setMasterFx(project.masterBusFx);
    engine.setDustDepth(project.dustDepth);
    engine.setLimiterPreGain(project.limiterPreGain);
    push_global_effects(engine, project);
}

// RenderController.applyMasterBusForRender. The *ForRender variants reset the module rather than
// fading it in, so the export matches playback from frame 0 — and the master EQ is put back to the
// project's slot so an EQM effect in the song animates from the right baseline (and a previous
// render's EQM override cannot bleed into this one).
template <typename Engine>
void apply_master_bus_for_render(Engine& engine, const Project& project) {
    engine.setMasterFx(project.masterBusFx);
    if (project.masterBusFx == 0) engine.setOttDepthForRender(project.ottDepth);
    else                          engine.setDustDepthForRender(project.dustDepth);
    engine.setLimiterPreGain(project.limiterPreGain);
    engine.setMasterEqSlot(project.masterEqSlot);
}

// The whole param half in one call: the mixer + globals, then every instrument the given song range
// uses. This is what makes a render a pure function of the project — see songcore::prepare_render,
// which calls it right after AudioEngine::resetEffectState() has wiped the chains back to defaults.
template <typename Engine>
void push_project_params(Engine& engine, const Project& project, int startRow, int endRow) {
    engine.setTempo(project.tempo);
    push_mixer(engine, project);
    push_used_instrument_params(engine, project, startRow, endRow);
}

/**
 * The same thing, for an app that is going to PLAY the project rather than export it — every
 * instrument rather than the used ones, and no `resetEffectState()` first (that would cut off whatever
 * is currently ringing).
 *
 * ⚠️ **This closes a real hole, and it is worth being precise about what the hole was.** Until Phase 3
 * S4 the ONLY caller of push_project_params in the whole tree was `prepare_render`. So a rendered WAV
 * carried the project's mixer, master bus, reverb, delay, EQ bank and every sampler's drive / filter /
 * crush / loop / sample window — and the SDL shell PLAYING that same project carried none of it. Live
 * playback ran on whatever the engine happened to hold: its own defaults at startup, or the previous
 * project's settings after a load. It was not audible on the default project (whose values happen to be
 * the engine's own), which is exactly why it survived Phase 2 and three Phase-3 sessions.
 *
 * The reason it could survive at all is that it is invisible to the conformance ladder: ptplay compares
 * EVENTS, and none of this is an event; ptvoice compares the calls a NOTE makes, and none of this is
 * made by a note; ptrender compares audio, and ptrender renders — so it goes through the one path that
 * was correct. Nothing in seven tools looks at what the engine holds while the app is merely running.
 *
 * Call it after a project is loaded, and again whenever a screen edits something in it that the engine
 * keeps on its own (the mixer, the master bus, an instrument's params — SongcoreHost::push_params /
 * push_instrument).
 */
template <typename Engine>
void push_live_params(Engine& engine, const Project& project) {
    engine.setTempo(project.tempo);
    push_mixer(engine, project);
    push_all_instrument_params(engine, project);
}

// ─── media: opens files, produces the Routing ────────────────────────────────────────────────────

struct MediaLoadResult {
    int loaded = 0;
    int failed = 0;
};

// Project media paths are absolute on device, but a portable project (the /testdata goldens, anything
// the Linux build ships) stores them RELATIVE to the project file. Absolute wins; relative resolves
// against base_dir. Deliberately not <filesystem>: it drags in a separate link library on some
// toolchains, for a job that is one string concat.
inline std::string resolve_media_path(const std::string& path, const std::string& base_dir) {
    if (path.empty() || base_dir.empty()) return path;
    const bool absolute = path[0] == '/' || path[0] == '\\' ||
                          (path.size() > 1 && path[1] == ':');   // C:\… on Windows
    return absolute ? path : base_dir + "/" + path;
}

inline std::string path_extension_lower(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return ext;
}

// AudioFormats.NATIVE_EXTENSIONS — the formats the bundled decoders handle (dr_mp3 / dr_flac /
// stb_vorbis / libopus). m4a/aac is the one format with no good native decoder; on Android it goes
// through MediaCodec, and there is no host equivalent.
inline bool is_native_compressed(const std::string& ext) {
    return ext == "mp3" || ext == "flac" || ext == "ogg" || ext == "opus";
}

// Load one instrument's sample into its engine slot. Returns the FILE's sample rate (> 0) on success,
// which is what the rate-compensation ratio is derived from, or 0 on failure / unsupported format.
template <typename Engine>
int load_sample_file(Engine& engine, int instrumentId, const std::string& path) {
    const std::string ext = path_extension_lower(path);
    if (ext == "wav") return engine.loadSampleFromWavFile(instrumentId, path.c_str());
    if (is_native_compressed(ext)) return engine.loadSampleFromCompressed(instrumentId, path.c_str());
    return 0;   // m4a/aac (MediaCodec-only) or an unknown extension
}

// AppInputDispatcher.reloadProjectSamples, without Android: load every instrument's media into the
// engine and record what the note path cannot derive for itself.
//
// The Routing is an OUTPUT here, not an input. songcore never opens a file, so the two facts it can't
// know — a sample's rate ratio (deviceRate / fileRate) and the SF2 slot a soundfontPath resolved to —
// are learned exactly here, where the files are opened. On Android the Kotlin loader learns the same
// two and pushes them in via SongcoreHost::push_routing: one struct, one producer per platform.
//
// Indexing note, preserved from Kotlin bug-for-bug: the loaders key the rate ratio by `instrument.id`
// while the note path reads it by `instrument.sampleId`. The two coincide for every project the
// factory builds (sampleId = slot index), and "fixing" it here would silently diverge from the Kotlin
// engine that tools/ptvoice goldens.
//
// NOT ported from the Kotlin loader, deliberately:
//   • m4a/aac — needs MediaCodec; no native decoder exists (AudioFormats.kt).
//   • WAV cue points → instrument.sliceMarkers. The engine's WAV decoder does not read the `cue `
//     chunk, so an instrument whose markers came from the file loads here without them. Markers
//     stored in the .ptp are unaffected. (Lands with the Linux Phase 1.5 media unification.)
template <typename Engine>
MediaLoadResult load_project_media(Engine& engine, const Project& project,
                                   const std::string& base_dir, Routing& routing) {
    // Start from a clean native slate so a previous project's PCM and SoundFonts don't accumulate —
    // the same reason reloadProjectSamples opens with clearAllSamples + clearAllSoundfonts.
    engine.clearAllSamples();
    engine.clearAllSoundfonts();
    routing.reset();

    MediaLoadResult result;
    const float deviceRate = static_cast<float>(engine.getSampleRate());

    for (const Instrument& ins : project.instruments) {
        if (ins.id < 0 || ins.id >= POOL_INSTRUMENTS) continue;

        if (ins.instrumentType == InstrumentType::SOUNDFONT && ins.soundfontPath.has_value()) {
            const std::string path = resolve_media_path(*ins.soundfontPath, base_dir);
            const int slot = engine.loadSoundfont(ins.id, path.c_str());
            if (slot >= 0) {
                routing.sfSlot[ins.id] = slot;
                result.loaded++;
            } else {
                result.failed++;
            }
        } else if (ins.sampleFilePath.has_value()) {
            // sampleFilePath == null is the single "empty slot" signal — an instrument with no path
            // loads nothing and its note is dropped at the seam, exactly as on Android.
            const std::string path = resolve_media_path(*ins.sampleFilePath, base_dir);
            const int fileRate = load_sample_file(engine, ins.id, path);
            if (fileRate > 0) {
                routing.sampleRateRatio[ins.id] = deviceRate / static_cast<float>(fileRate);
                result.loaded++;
            } else {
                result.failed++;
            }
        }
    }
    return result;
}

// ─── the instrument operations (core/logic/InstrumentController.kt) ──────────────────────────────
//
// The three verbs the INSTRUMENT screen and the pool need that are NOT a plain parameter edit, because
// they own a SOURCE and freeing it is the engine's business. Kotlin's InstrumentController holds them,
// together with its ~25 `updateXxx(instrument, value)` setters — and those setters are deliberately NOT
// ported: they are `instrument.field = v.coerceIn(...)` plus an engine push, and in C++ the module's
// own `handle_input` does the assignment (as every other screen module already does) and the dispatcher
// makes ONE push afterwards. A controller class whose entire content is "assign, then push" is a layer
// that exists only to be a layer, and porting it would put the model mutation somewhere no golden could
// see it — the pool and MODS modules would then be untestable by ptinput.
//
// ⚠️ The SoundFont path→slot map is NOT ported either, and must not be: it lives in the ENGINE now
// (S6b moved the whole SF bank out of jni-bridge.cpp), which already de-dups by path and evicts LRU.
// Kotlin's `sfSlotMap` is the Kotlin-side shadow of that map. A second copy here would be a second
// truth about which slot a file is in.

/** SF2 preset count for an instrument, or 0 when it has no SoundFont loaded. */
template <typename Engine>
int soundfont_preset_count(Engine& engine, const Instrument& ins, const Routing& routing) {
    if (!ins.soundfontPath.has_value()) return 0;
    const int slot = routing.sfSlot[ins.id];
    return (slot < 0) ? 0 : engine.getSoundfontPresetCount(slot);
}

/** The list INDEX of the instrument's current bank+preset, or 0 when not found. */
template <typename Engine>
int soundfont_preset_index(Engine& engine, const Instrument& ins, const Routing& routing) {
    if (!ins.soundfontPath.has_value()) return 0;
    const int slot = routing.sfSlot[ins.id];
    if (slot < 0) return 0;
    const int count = engine.getSoundfontPresetCount(slot);
    for (int i = 0; i < count; ++i) {
        int bank = -1, preset = -1;
        if (!engine.getSoundfontPresetAt(slot, i, &bank, &preset)) continue;
        if (bank == ins.sfBank && preset == ins.sfPreset) return i;
    }
    return 0;
}

/** The display name of the instrument's current preset — "---" when there is no SoundFont. */
template <typename Engine>
std::string soundfont_preset_name(Engine& engine, const Instrument& ins, const Routing& routing) {
    if (!ins.soundfontPath.has_value()) return "---";
    const int slot = routing.sfSlot[ins.id];
    if (slot < 0) return "---";
    return engine.getSoundfontPresetName(slot, ins.sfBank, ins.sfPreset);
}

/**
 * Move to the preset at `index` in the SF2's list — the INSTRUMENT screen's PRESET row.
 *
 * It writes the instrument's bank+preset and nothing else. Kotlin also calls
 * `backend.setSoundfontPreset(slot, bank, preset)` here so that previews use the new sound; the C++
 * engine's twin of that call is a LOG LINE ("applied per-note") — the bank and preset ride with every
 * scheduled note, from `derive_soundfont_note`, so writing them on the instrument IS the whole of it.
 */
template <typename Engine>
bool set_soundfont_preset_by_index(Engine& engine, Instrument& ins, const Routing& routing, int index) {
    if (!ins.soundfontPath.has_value()) return false;
    const int slot = routing.sfSlot[ins.id];
    if (slot < 0) return false;
    int bank = -1, preset = -1;
    if (!engine.getSoundfontPresetAt(slot, index, &bank, &preset) || bank < 0) return false;
    ins.sfBank   = bank;
    ins.sfPreset = preset;
    return true;
}

/**
 * Change an instrument's TYPE, freeing the source the old type owned.
 *
 * The free is the point. Without it a slot toggled SAMPLER→SOUNDFONT keeps its PCM resident (and a
 * SoundFont toggled the other way keeps ~2× its file size in RAM) for a source the UI no longer shows
 * and nothing can ever play again.
 *
 * ⚠️ The SoundFont unload is guarded on SHARING: engine slots are keyed by PATH, so two instruments
 * pointing at one .sf2 hold ONE slot between them. Unloading it because one of them changed type would
 * silence the other.
 *
 * ⚠️ **`engine` is a POINTER and may be null, and that is not defensive padding — it is the contract.**
 * These are MODEL edits that happen to also free engine resources, and gating the model edit on an
 * engine being present would make the whole editing path require an audio device. It does not: the S4
 * harness drives every one of these verbs against a null engine, and `tools/ptshot` renders the screens
 * that show them with no engine in the process at all. Guard the ENGINE CALLS, never the document.
 */
template <typename Engine>
void set_instrument_type(Engine* engine, Project& project, int id, InstrumentType newType,
                         Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return;
    Instrument& ins = project.instruments[id];
    ins.instrumentType = newType;

    if (newType == InstrumentType::SOUNDFONT) {
        ins.sampleFilePath.reset();
        if (engine) engine->clearSample(id);
        routing.sampleRateRatio[id] = 1.0f;
    } else {
        const std::optional<std::string> sfPath = ins.soundfontPath;
        ins.soundfontPath.reset();
        if (sfPath.has_value()) {
            bool sharedWithAnother = false;
            for (const Instrument& other : project.instruments)
                if (other.soundfontPath == sfPath) { sharedWithAnother = true; break; }
            if (engine && !sharedWithAnother && routing.sfSlot[id] >= 0)
                engine->unloadSoundfont(routing.sfSlot[id]);
        }
        routing.sfSlot[id] = -1;
    }
}

/**
 * Reset a slot to empty — the pool's A+B. The instrument TYPE is KEPT, so a SoundFont slot stays a
 * (now empty) SoundFont slot rather than silently becoming a sampler under the user's cursor.
 * `engine` may be null; see set_instrument_type.
 */
template <typename Engine>
void clear_instrument(Engine* engine, Project& project, int id, Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return;

    const std::optional<std::string> sfPath  = project.instruments[id].soundfontPath;
    const InstrumentType             keepType = project.instruments[id].instrumentType;

    Instrument fresh(id);
    fresh.sampleId       = id;   // the factory value — Project's Array(128) initializer
    fresh.instrumentType = keepType;
    project.instruments[id] = std::move(fresh);

    if (engine) engine->clearSample(id);
    routing.sampleRateRatio[id] = 1.0f;

    // …and the SF2, if this was its last user. Same sharing guard as set_instrument_type.
    if (sfPath.has_value()) {
        bool stillUsed = false;
        for (const Instrument& other : project.instruments)
            if (other.soundfontPath == sfPath) { stillUsed = true; break; }
        if (engine && !stillUsed && routing.sfSlot[id] >= 0)
            engine->unloadSoundfont(routing.sfSlot[id]);
    }
    routing.sfSlot[id] = -1;
}

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_ENGINE_SETUP_H
