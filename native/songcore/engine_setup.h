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

#include <filesystem>   // resolve_case_insensitive — Android storage is case-insensitive, the SD card is not
#include <fstream>      // path_exists — an <fstream> probe (see resolve_media_path)
#include <string>
#include <vector>

#include "model.h"
#include "scheduler.h"    // hex_to_float (VolumeUtils.hexToFloat)
#include "traversal.h"    // collect_used_instruments
#include "voice_derive.h" // Routing, push_instrument_mod_eq_sends, push_instrument_playback_params
#include "wav_writer.h"   // read_cue_points — a WAV's slice boundaries live in its `cue ` chunk

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

// A cheap "is this file actually here?" — an <fstream> open probe, NOT <filesystem>, so it stays inside
// the no-extra-link-library rule the path helpers below keep. Used only to decide whether an absolute
// path needs relocating; a false negative (a file that exists but cannot be opened) at worst re-roots to
// the same-or-a-worse guess, and the load fails either way, so it costs nothing it did not already cost.
inline bool path_exists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// The app-root-relative tail of an absolute media path authored under ANOTHER install — the portable
// part naming where UNDER the app root a file lives ("Samples/Pads/kick.wav"), with the foreign root
// stripped. Empty when the path is under no recognisable app sub-tree (a sample the user kept elsewhere).
inline std::string app_root_relative_tail(const std::string& path) {
    // 1) An Android phone hard-codes its root to ".../PocketTracker" (AndroidFileSystem), so everything
    //    after the LAST "/PocketTracker/" is exactly the tail. This is the case a user copying a project
    //    off a phone hits, and the anchor the user themselves named. rfind, so a user sub-folder that
    //    happens to be called "PocketTracker" loses to the real root above it.
    static const std::string kPtAnchor = "/PocketTracker/";
    const size_t pt = path.rfind(kPtAnchor);
    if (pt != std::string::npos) return path.substr(pt + kPtAnchor.size());

    // 2) A root NOT named PocketTracker (another handheld whose $POCKETTRACKER_HOME is e.g. ".../data")
    //    has no such anchor — fall back to the media sub-trees themselves, keeping the sub-dir IN the
    //    tail so it re-roots whole ("Samples/x.wav" onto <root> → <root>/Samples/x.wav).
    static const std::string kSubtrees[] = { "/Samples/", "/Soundfonts/", "/Renders/" };
    size_t best = std::string::npos;
    for (const std::string& sub : kSubtrees) {
        const size_t at = path.rfind(sub);
        if (at != std::string::npos && (best == std::string::npos || at > best)) best = at;
    }
    if (best == std::string::npos) return "";
    return path.substr(best + 1);   // drop the leading '/', keep "Samples/…"
}

inline std::string to_lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

// Resolve a path whose stored CASE may not match the disk. A project authored on Android references
// "Samples/Breaks/x.wav", but Android storage is case-INsensitive (FAT/sdcardfs) while a Linux SD card is
// case-SENSITIVE — so the real file is "Samples/breaks/x.wav" and the exact path is dead. Walk the path a
// component at a time from the longest existing prefix; where an exact child is missing, take the entry
// whose name matches case-insensitively. Returns the ORIGINAL path when no such chain exists, so a
// genuinely-absent file (a sample the user never copied) still fails naming what the project asked for.
//
// ⭐ It returns IMMEDIATELY when the exact path exists — which is every host-tool/golden case (their paths
// match the disk exactly) — so nothing there moves, and the only directory listing ever done is on a real
// miss. This is the one place songcore reaches for <filesystem>; it is never instantiated on Android
// (load_project_media has no caller there), so the APK neither runs nor links it.
inline std::string resolve_case_insensitive(const std::string& path) {
    if (path.empty()) return path;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(fs::path(path), ec)) return path;   // exact hit — the overwhelmingly common case

    // Split on both separators OURSELVES rather than via fs::path's relative_path(): the real device app
    // root is "//mnt/SDCARD/Roms/PORTS//ports/…" and fs::path treats a leading "//" in an
    // implementation-defined way. Dropping empty components collapses every redundant slash cleanly.
    const bool absolute = path[0] == '/' || path[0] == '\\';
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else                       { cur.push_back(c); }
    }
    if (!cur.empty()) parts.push_back(cur);

    fs::path have = absolute ? fs::path("/") : fs::path(".");
    size_t start = 0;
    if (!absolute && !parts.empty() && parts[0].size() == 2 && parts[0][1] == ':') {
        have = fs::path(parts[0] + "/");   // a Windows drive ("C:") anchors the walk (dev box)
        start = 1;
    }
    if (!fs::exists(have, ec)) return path;   // nothing to anchor the walk on

    for (size_t i = start; i < parts.size(); ++i) {
        const fs::path exact = have / parts[i];
        if (fs::exists(exact, ec)) { have = exact; continue; }
        bool matched = false;
        if (fs::is_directory(have, ec)) {
            const std::string target = to_lower_ascii(parts[i]);
            for (const auto& entry : fs::directory_iterator(have, ec)) {
                if (to_lower_ascii(entry.path().filename().string()) == target) {
                    have = entry.path();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return path;   // give up; let the failure name the intended path
    }
    return have.string();
}

// Project media paths are absolute on device, but a portable project (the /testdata goldens, anything
// the Linux build ships) stores them RELATIVE to the project file. Absolute wins; relative resolves
// against base_dir. Deliberately not <filesystem>: it drags in a separate link library on some
// toolchains, for a job that is one string concat.
//
// ⚠️ THE ONE EXCEPTION — a project MOVED between installs. Its paths were written absolute under the
// AUTHORING install's root (an Android phone's ".../Documents/PocketTracker/", another handheld's own
// $POCKETTRACKER_HOME); copied onto THIS device they point nowhere. `app_root` — the folder THIS install
// keeps Samples/, Soundfonts/… directly under — is the anchor that fixes it: an absolute path that does
// not exist here but carries an app-root-relative tail is re-rooted onto our own root. Bounded to the app
// tree on purpose (app_root_relative_tail returns "" otherwise) — a sample parked OUTSIDE PocketTracker/
// is genuinely unfindable and is left as-authored so the failure names the real path. Resolution only:
// the Project's stored string is NOT rewritten, so a re-save stays portable back to the phone.
//
// ⭐ app_root EMPTY ⇒ the whole exception is skipped ⇒ byte-for-byte the old two-line behaviour. Every
// host TOOL leaves it empty (SongcoreHost::appRoot_ defaults to ""), which is why not one golden moves;
// only the SDL shell, which calls set_app_root() at boot, ever re-roots.
inline std::string resolve_media_path(const std::string& path, const std::string& base_dir,
                                      const std::string& app_root) {
    if (path.empty()) return path;
    const bool absolute = path[0] == '/' || path[0] == '\\' ||
                          (path.size() > 1 && path[1] == ':');   // C:\… on Windows

    std::string resolved = (!absolute && !base_dir.empty()) ? base_dir + "/" + path : path;

    // Absolute wins — UNLESS it points nowhere here and was authored under another install's app tree.
    if (absolute && !app_root.empty() && !path_exists(resolved)) {
        const std::string tail = app_root_relative_tail(resolved);
        if (!tail.empty()) resolved = app_root + "/" + tail;
    }

    // Last: fix any case drift (an Android-authored "Breaks/" vs the card's "breaks/"). A no-op when the
    // path already exists exactly, so goldens/tools — whose paths match the disk — are byte-for-byte
    // unchanged; it only ever lists a directory on a real miss.
    return resolve_case_insensitive(resolved);
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

// A WAV's cue points as the model wants them: `Instrument::sliceMarkers` is int64 (Kotlin's
// `List<Long>`, and Kotlin maps `readCuePoints(path).map { it.toLong() }` at every call site).
inline std::vector<int64_t> read_cue_markers(const std::string& path) {
    const std::vector<int> cues = read_cue_points(path);
    return std::vector<int64_t>(cues.begin(), cues.end());
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
//
// ⚠️ **WAV cue points → `instrument.sliceMarkers` IS ported now (S6b), and the FILE WINS.** S6a could
// not port it — there was no cue-point reader — and said so. There is one now (`wav_writer.h`), so this
// reads them, exactly where Kotlin reads them (`reloadProjectSamples`), and with Kotlin's precedence:
// for a WAV the file's cue chunk REPLACES whatever markers the .ptp carried, because the audio and its
// slice boundaries are one artifact and the file is the newer of the two (the editor's CHOP/SAVE writes
// both, but only the file survives being loaded into a different slot or project). A COMPRESSED source
// keeps the .ptp's markers untouched — it has no cue chunk to read, and clearing them would delete
// markers nothing else can restore. Hence the non-const `project`.
template <typename Engine>
MediaLoadResult load_project_media(Engine& engine, Project& project,
                                   const std::string& base_dir, const std::string& app_root,
                                   Routing& routing) {
    // Start from a clean native slate so a previous project's PCM and SoundFonts don't accumulate —
    // the same reason reloadProjectSamples opens with clearAllSamples + clearAllSoundfonts.
    engine.clearAllSamples();
    engine.clearAllSoundfonts();
    routing.reset();

    MediaLoadResult result;
    const float deviceRate = static_cast<float>(engine.getSampleRate());

    for (Instrument& ins : project.instruments) {
        if (ins.id < 0 || ins.id >= POOL_INSTRUMENTS) continue;

        if (ins.instrumentType == InstrumentType::SOUNDFONT && ins.soundfontPath.has_value()) {
            const std::string path = resolve_media_path(*ins.soundfontPath, base_dir, app_root);
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
            const std::string path = resolve_media_path(*ins.sampleFilePath, base_dir, app_root);
            const int fileRate = load_sample_file(engine, ins.id, path);
            if (fileRate > 0) {
                routing.sampleRateRatio[ins.id] = deviceRate / static_cast<float>(fileRate);
                // The file's slice boundaries win over the project's — but only a WAV has any.
                if (!is_native_compressed(path_extension_lower(path)))
                    ins.sliceMarkers = read_cue_markers(path);
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

// ─── the preview slots (AudioEngine.clearPreviewSlots) ──────────────────────────────────────────
//
// Two sample slots above the 128-instrument pool are scratch, and neither belongs to a project:
//   255 — the FILE BROWSER's audition. The file under the cursor, decoded so it can be heard BEFORE
//         it is committed to a slot, which is the entire reason to browse samples rather than guess.
//   254 — the SAMPLE EDITOR's source preview (S6b).
// A real load frees them, because the audition is stale the moment the file it auditioned is loaded.

inline constexpr int PREVIEW_SAMPLE_SLOT = 255;
inline constexpr int SOURCE_PREVIEW_SLOT = 254;

template <typename Engine>
void clear_preview_slots(Engine& engine) {
    engine.clearSample(SOURCE_PREVIEW_SLOT);
    engine.clearSample(PREVIEW_SAMPLE_SLOT);
}

/**
 * Audition the file at `path` — the browser's START. It decodes into slot 255 and plays it at C-4 on
 * the preview lane, so it steals nothing from a song playing underneath.
 *
 * ⚠️ This is the ONE note in the port that does NOT go through `plan_note_on`, and the exception is
 * principled rather than convenient: `plan_note_on` derives a note from an INSTRUMENT, and a file
 * being auditioned in a browser has no instrument behind it — no root, no detune, no filter, no mod
 * slots, not even a pool slot. There is nothing to derive from. What it plays is the file, flat, at
 * C-4, with the sample-rate ratio applied so a 22 kHz file is not auditioned an octave low.
 *
 * Returns the file's sample rate (> 0) on success, 0 if it could not be decoded.
 */
template <typename Engine>
int preview_sample_file(Engine& engine, const std::string& path) {
    constexpr float C4_HZ = 261.63f;

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

// ─── loading a SOURCE into one instrument (Phase 3 S6a — the file browser's whole point) ─────────
//
// `load_project_media` above loads every instrument's source at once, which is what a project LOAD
// does. These two are the single-slot verbs the file browser needs: the user picked one file, and it
// goes into one slot. Same engine calls, same Routing writes, same "sampleFilePath is the empty
// signal" convention — differing only in that they also update the DOCUMENT, because a project load
// has already read the paths from the file whereas a browser pick is what CREATES them.

/**
 * Load a sample (wav / mp3 / flac / ogg / opus) into instrument `id`. True on success.
 *
 * ⚠️ The instrument keeps the ORIGINAL path even for a compressed source — no WAV is written, and the
 * decode is repeated on the next project load. That is Kotlin's contract (`loadSampleFromCompressed`:
 * "the instrument keeps its original path"), and `load_project_media` is the code that honours it.
 *
 * ⚠️ The new source's SLICE MARKERS come from the file, and only a WAV has any — its `cue ` chunk,
 * which is where the sample editor's CHOP and SAVE put them (S6b). A compressed source has no cue
 * chunk, so its markers are CLEARED rather than left behind: the slot now points at different audio,
 * and boundaries measured against the previous sample are worse than none. Kotlin does exactly this
 * (`InstrumentController`: `if (isCompressed) emptyList() else readCuePoints(path)`).
 */
template <typename Engine>
bool load_instrument_sample(Engine* engine, Project& project, int id, const std::string& path,
                            Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return false;

    // No engine → no decode, and therefore nothing true to write into the document. Unlike
    // set_instrument_type (which edits the document and merely also frees engine resources), a LOAD
    // *is* the engine call: claiming a path we never opened would leave a slot that points at audio
    // the engine does not have.
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

    // The audition the browser was playing while the user scrolled is now stale — a real load has
    // committed. Kotlin drops it here too (`audioEngine.clearPreviewSlots()`).
    clear_preview_slots(*engine);
    return true;
}

/**
 * Load an SF2/SF3 into instrument `id`, make the slot a SOUNDFONT, and select the first preset that
 * actually EXISTS in the file — a bank/preset pair the .sf2 does not contain plays silence, and 0/0 is
 * not present in every soundfont.
 */
template <typename Engine>
bool load_instrument_soundfont(Engine* engine, Project& project, int id, const std::string& path,
                               Routing& routing) {
    if (id < 0 || id >= static_cast<int>(project.instruments.size())) return false;
    if (!engine) return false;

    const int slot = engine->loadSoundfont(id, path.c_str());
    if (slot < 0) return false;

    Instrument& ins    = project.instruments[static_cast<size_t>(id)];
    ins.soundfontPath  = path;
    ins.instrumentType = InstrumentType::SOUNDFONT;
    ins.sampleFilePath.reset();   // the slot's old sampler source is gone with the type change
    routing.sfSlot[id] = slot;

    // The FIRST preset in the file's list — `getSoundfontFirstBankPreset` on Android, which is
    // `getSoundfontPresetAt(slot, 0)` here. Not 0/0: plenty of SF2s do not contain bank 0 preset 0, and
    // a bank/preset pair the file lacks plays silence.
    int bank = -1, preset = -1;
    if (engine->getSoundfontPresetAt(slot, 0, &bank, &preset) && bank >= 0) {
        ins.sfBank   = bank;
        ins.sfPreset = preset;
    }

    clear_preview_slots(*engine);
    return true;
}

/**
 * Apply a loaded .pti to instrument `id` — every parameter, the embedded table if there is one, and
 * the source file the preset names.
 *
 * `id` is preserved (a preset saved from slot 3 loads into whichever slot you are standing on), and so
 * is the TABLE it lands in: the embedded rows always go into the DESTINATION instrument's own table,
 * because instrument index == table index is the app's rule (INST01 owns TABLE01) and honouring the
 * preset's stored tableId would have it stomp a table belonging to a different instrument.
 *
 * Returns false only if the SOURCE could not be loaded — the parameters are applied either way, which
 * is Kotlin's behaviour and the useful one: a preset whose sample has been moved should still give you
 * back its filter, its envelope and its mod slots.
 */
template <typename Engine>
bool apply_instrument_preset(Engine* engine, Project& project, int id, const InstrumentPreset& preset,
                             Routing& routing) {
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
        if (!load_instrument_soundfont(engine, project, id, *src.soundfontPath, routing)) return false;

        // load_instrument_soundfont selected the SF2's FIRST preset. The one the .pti saved wins —
        // but only if this file still has it: a preset validated against a different .sf2 (or an .sf2
        // that has been edited since) would play silence, and falling back to the first is Kotlin's
        // behaviour and the recoverable one.
        Instrument& ins = project.instruments[static_cast<size_t>(id)];
        if (engine && routing.sfSlot[id] >= 0 &&
            engine->getSoundfontPresetName(routing.sfSlot[id], src.sfBank, src.sfPreset) != "---") {
            ins.sfBank   = src.sfBank;
            ins.sfPreset = src.sfPreset;
            // No engine call: the bank and preset ride with every scheduled note (derive_soundfont_note),
            // so writing them on the instrument IS the whole of it. Kotlin's `setSoundfontPreset` here
            // reaches an engine function whose body is a log line — see set_soundfont_preset_by_index.
        }
        return true;
    }

    if (!src.sampleFilePath.has_value()) return true;      // params-only preset
    if (!load_instrument_sample(engine, project, id, *src.sampleFilePath, routing)) return false;

    // load_instrument_sample cleared the markers (a fresh source has none); the preset's win.
    project.instruments[static_cast<size_t>(id)].sliceMarkers = src.sliceMarkers;
    return true;
}

/**
 * Build the .pti for instrument `id`. The table travels WITH the preset — but only if it has content,
 * because embedding 16 empty rows in every preset is bloat that says nothing.
 */
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
