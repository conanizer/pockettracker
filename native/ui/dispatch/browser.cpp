// The file browser — opening, the cursor, loading, the file clipboard — and the QWERTY keyboard.

#include "ui/dispatch/dispatch_common.h"

#include "ui/groove_io.h"
#include "ui/scale_io.h"
#include "ui/std_filesystem.h"   // path_name / path_stem / path_extension / to_lower
#include "ui/theme_io.h"
#include "common/load_progress.h"       // load_cancelled

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── Opening and closing the browser ─────────────────────────────────────────────────────────────

std::string InputDispatcher::browser_dir(BrowserDir cat) {
    // A config.json override per category, else the built-in default — created on first use, so always
    // a real directory to fall back on.
    const std::optional<std::string>* ov = nullptr;
    std::string                       def;
    switch (cat) {
        case BrowserDir::SAMPLES:     ov = &s_.folderConfig.samples;     def = fs_.samples_directory();     break;
        case BrowserDir::SOUNDFONTS:  ov = &s_.folderConfig.soundfonts;  def = fs_.soundfonts_directory();  break;
        case BrowserDir::INSTRUMENTS: ov = &s_.folderConfig.instruments; def = fs_.instruments_directory(); break;
        case BrowserDir::PROJECTS:    ov = &s_.folderConfig.projects;    def = fs_.projects_directory();    break;
        case BrowserDir::THEMES:      ov = &s_.folderConfig.themes;      def = fs_.themes_directory();      break;
    }
    // ⚠️ The whole rule is `resolve_browse_dir` (root-relative unless absolute, re-rooting, the fallback);
    // inlining any of it here would let the config and project sample paths disagree.
    return ov ? resolve_browse_dir(fs_, *ov, def) : def;
}

void InputDispatcher::open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                                        const std::vector<std::string>& extensions) {
    s_.previousScreen = s_.currentScreen;
    s_.browserPurpose = purpose;

    s_.fileBrowser.fileExtensions = extensions;
    s_.fileBrowser.mode           = BrowserMode::NORMAL;

    // FOLDER = REMEMBER: a SAMPLE load starts in the folder the last sample came from. Keyed off the start
    // being the samples dir, which is exactly the two sample-load purposes. A remembered folder that is
    // gone falls back to `directory`.
    std::string start = directory;
    if (s_.settings.rememberFolder && directory == browser_dir(BrowserDir::SAMPLES) &&
        !s_.settings.lastSampleFolder.empty() && fs_.is_directory(s_.settings.lastSampleFolder)) {
        start = s_.settings.lastSampleFolder;
    }
    navigate_to_folder(s_.fileBrowser, fs_, start);

    s_.currentScreen = ScreenType::FILE_BROWSER;
}

void InputDispatcher::close_file_browser() {
    // The audition is over; a preview left resident is PCM nothing will play again.
    host_.clear_previews();
    s_.fileBrowser.selectionMode   = false;
    s_.fileBrowser.selectionAnchor = -1;
    s_.currentScreen = s_.previousScreen;
}

void InputDispatcher::refresh_browser() {
    FileBrowserState& b = s_.fileBrowser;

    // ⚠️ A re-list is not a re-read unless the filesystem forgets: `SafFileSystem` caches and drops the
    // cache only on this app's writes, so a file from a PC or download would stay invisible. Here, below
    // every call site. A no-op where nothing is cached.
    fs_.forget_listing(b.currentDirectory);

    // Re-list in place and KEEP THE CURSOR — a refresh is not a navigation — CLAMPED, since the list may
    // have got shorter.
    rebuild_items(b, fs_);

    const int last = static_cast<int>(b.items.size()) - 1;
    b.cursor = std::min(std::max(b.cursor, 0), std::max(last, 0));

    if (b.cursor < b.scroll) b.scroll = b.cursor;
    if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    b.scroll = std::max(0, std::min(b.scroll, std::max(0, last - BROWSER_VISIBLE_ROWS + 1)));
}

void InputDispatcher::refresh_browser_on_foreground() {
    // ⚠️ Guarded on the SCREEN: `fileBrowser` keeps its state after closing, and re-listing on every
    // return to the app would query a directory nobody is looking at.
    if (s_.currentScreen != ScreenType::FILE_BROWSER) return;

    // ⚠️ NOT while a modal is up: a refresh can move what is under the cursor, leaving "DELETE X?" on
    // screen with A deleting Y. A stale listing is cosmetic; a mislabelled confirm is not.
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    refresh_browser();
}

// ─── The browser's cursor ────────────────────────────────────────────────────────────────────────

void InputDispatcher::browser_move_cursor(int delta, bool page) {
    FileBrowserState& b     = s_.fileBrowser;
    const int         total = static_cast<int>(b.items.size());
    if (total == 0) return;

    // ⚠️ UP/DOWN WRAP; the LEFT/RIGHT page jump CLAMPS — a wrapping page would fling you from the top of a
    // 400-file directory to the bottom.
    if (page) {
        b.cursor = std::min(std::max(b.cursor + delta, 0), total - 1);
    } else {
        // ⚠️ Modulo TWICE: a page step can exceed the list length.
        b.cursor = ((b.cursor + delta) % total + total) % total;
    }

    // Keep the 19-row window around the cursor.
    if (b.cursor < b.scroll) {
        b.scroll = b.cursor;
    } else if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) {
        b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    }
}

// ─── A: open a folder, or LOAD the file ──────────────────────────────────────────────────────────

void InputDispatcher::browser_confirm() {
    FileBrowserState& b = s_.fileBrowser;

    // DELETE mode: A is the YES. The only place the browser removes anything — SELECT+B to arm, A to
    // confirm.
    if (b.mode == BrowserMode::DELETE) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || item->is_pseudo()) return;

        const std::string name = item->displayName;
        if (fs_.delete_path(item->path)) {
            refresh_browser();
            b.statusMessage = "DELETED: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "DELETE FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    // SET_HOME mode: A is the YES, armed by SELECT+A on a granted tree.
    if (b.mode == BrowserMode::SET_HOME) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (fs_.set_home_directory(item->path)) {
            // ⚠️ The two derived roots (`AppConfig::mediaBaseDir`, `SongcoreHost::set_app_root`) were read
            // at boot and must move WITH the home, or media resolves against the old tree. Derived from an
            // accessor, so pt-ui never learns what a root string looks like.
            const std::string root = fs_.parent_path(fs_.samples_directory());
            set_media_base_dir(root);
            host_.set_app_root(root);

            refresh_browser();
            b.statusMessage = "HOME FOLDER: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "COULD NOT SET HOME FOLDER";
            b.statusSuccess = false;
        }
        return;
    }

    // FORGET_ROOT mode: A is the YES, armed by SELECT+B on a granted tree.
    if (b.mode == BrowserMode::FORGET_ROOT) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (fs_.revoke_access(item->path)) {
            // ⚠️ The home may have been this tree, and the filesystem has picked another — re-ask the
            // derived roots exactly as after a home change.
            const std::string root = fs_.parent_path(fs_.samples_directory());
            set_media_base_dir(root);
            host_.set_app_root(root);

            refresh_browser();
            b.statusMessage = "FORGOT: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "COULD NOT FORGET IT";
            b.statusSuccess = false;
        }
        return;
    }

    const BrowserItem* item = b.current();
    if (!item) return;

    if (item->is_parent()) { navigate_to_parent(b, fs_); return; }

    // An ACTION is a thing to do; the filesystem owns what it means.
    // ⚠️ No refresh afterwards: `activate` does not wait (Android's folder picker), so the world has not
    // changed yet. `refresh_browser_on_foreground()` catches up when the app returns.
    if (item->kind == BrowserItem::Kind::ACTION) {
        const std::string label = item->displayName;
        if (!fs_.activate(item->path)) {
            b.statusMessage = label + " FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    if (item->kind == BrowserItem::Kind::FOLDER) { navigate_to_folder(b, fs_, item->path); return; }

    // ── It is a FILE — the reason the browser was opened ─────────────────────────────────────────
    const int         id   = s_.currentInstrument;
    const std::string ext  = to_lower(item->extension);
    const std::string path = item->path;
    const std::string stem = item->displayName;

    // The SAMPLE EDITOR's LOAD targets the editor's slot, which need not be INSTRUMENT's last one.
    const int sourceId = (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR)
                             ? s_.sampleEditor.instrumentId
                             : id;
    // The name the slot is about to stop deserving — read before the load replaces its source (see the
    // adopt rule below).
    const std::string previousAutoName = instrument_auto_name(host_.project(), sourceId);

    // ⚠️ EVERY arm: slowness is a fact about the FILE and DEVICE, not the menu item. Below the delay
    // nothing is drawn, so a fast load costs nothing.
    const LoadScope loadScope(*this, now_ms_, stem);

    bool ok = false;
    switch (s_.browserPurpose) {
        case AppState::BrowserPurpose::LOAD_PRESET:
            ok = host_.load_instrument_preset(id, path);
            break;

        case AppState::BrowserPurpose::LOAD_SOURCE:
            // The extension decides, not the slot's type: picking an .sf2 from a sampler slot TURNS it
            // into a SoundFont slot — the filter is not a guarantee, the user can navigate anywhere.
            // `.sf3` is on the same footing as `.sf2` (and cheaper in peak memory, audio-engine.h).
            if (is_soundfont_extension(ext)) {
                ok = host_.load_soundfont(id, path);
            } else {
                ok = host_.load_sample(id, path);
            }
            break;

        case AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR:
            // The editor's own LOAD: the same load, returning to the EDITOR.
            ok = host_.load_sample(s_.sampleEditor.instrumentId, path);
            break;

        case AppState::BrowserPurpose::LOAD_PROJECT:
            // ⚠️ The WHOLE DOCUMENT, returning early — nothing below applies. `load_project_file` is parse →
            // push → load_media → push_params in one call, so none can be skipped.
            if (!host_.load_project_file(path, fs_.samples_directory())) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            // ⚠️⚠️ A cancelled PROJECT load cannot stay where it stopped: the document is already swapped
            // in, and instruments past the stop would look loaded and play silence. The honest state is
            // NEW PROJECT's blank document, and the message says so.
            if (pt::load_cancelled()) {
                host_.new_project();
                host_.push_params();
                // `load_project_done`'s settling: no source path, and the autosave cleared — its work
                // belonged to the project just left.
                load_project_done("");
                s_.statusMessage = "LOAD CANCELLED";
                s_.statusSuccess = true;
                return;
            }
            load_project_done(path);
            return;

        case AppState::BrowserPurpose::LOAD_THEME:
            // ⚠️ A theme is pixels — no engine, sample or slot — so this returns early too.
            // ⚠️ The extension is re-checked: the filter is not a guarantee (the D-pad walks out of the
            // Themes folder), and a failed parse must not blank the palette.
            if (ext != "ptt" || !load_theme_file(fs_, path, s_.theme)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            // Back to the editor that raised the browser (`theme_row_action` closed it on the way out).
            close_file_browser();
            open_theme_editor();
            s_.statusMessage = "THEME LOADED";
            s_.statusSuccess = true;
            return;

        case AppState::BrowserPurpose::LOAD_SCALE: {
            // ⚠️ Loaded into a COPY, committed only once it parses — no half-overwritten slot — and the copy
            // keeps the slot's `id`. The extension is re-checked: the D-pad walks out of the Scales folder.
            songcore::Scale loaded = host_.project().scales[static_cast<size_t>(s_.currentScale)];
            if (ext != SCALE_FILE_EXT || !load_scale_file(fs_, path, loaded)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            host_.edit_project().scales[static_cast<size_t>(s_.currentScale)] = loaded;
            mark_modified();
            close_file_browser();
            s_.statusMessage = "SCALE LOADED";
            s_.statusSuccess = true;
            return;
        }

        case AppState::BrowserPurpose::LOAD_GROOVE: {
            // The scale load's two guards, for the same reasons (the slot's `id` is what `GRV` names).
            songcore::Groove loaded = host_.project().grooves[static_cast<size_t>(s_.currentGroove)];
            if (ext != GROOVE_FILE_EXT || !load_groove_file(fs_, path, loaded)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)] = loaded;
            mark_modified();
            close_file_browser();
            s_.statusMessage = "GROOVE LOADED";
            s_.statusSuccess = true;
            return;
        }
    }

    if (!ok) {
        // ⚠️ A CANCEL is not a failure and gets no red line — "LOAD FAILED" would claim something about
        // the file nobody knows.
        if (host_.last_load_cancelled()) {
            b.statusMessage = "CANCELLED";
            b.statusSuccess = true;
            return;
        }
        // ⚠️ "LOAD FAILED" for a file the DEVICE cannot hold sends the user hunting a corruption that is not
        // there. The file is sound; pick a smaller one.
        b.statusMessage = host_.last_load_ran_out_of_memory() ? "FILE TOO BIG" : "LOAD FAILED";
        b.statusSuccess = false;
        return;
    }

    // The slot adopts the file's name — unless the user TYPED the one it has. Not typed: the default
    // ("INST07"), or the name it took from the source being replaced (`previousAutoName`, captured
    // before the load) — otherwise a slot keeps reading RHODES C4 after a different sample lands.
    // Both source loads apply it, or a name set through one door could never be corrected through the
    // other.
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        sourceId >= 0 && static_cast<size_t>(sourceId) < host_.project().instruments.size()) {
        Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(sourceId)];
        if (songcore::instrument_has_default_name(ins) ||
            (!previousAutoName.empty() && ins.name == previousAutoName)) {
            ins.name = adopted_name(stem);
        }
    }

    mark_modified();

    // FOLDER = REMEMBER: `b.currentDirectory` is the folder this SAMPLE came from. Sample loads only, not
    // an SF picked from a sampler slot. Persisted on exit (save_settings_if_changed).
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        !is_soundfont_extension(ext)) {
        s_.settings.lastSampleFolder = b.currentDirectory;
    }

    if (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) {
        // Re-enter the EDITOR on the new audio (not `previousScreen`, still INSTRUMENT, the editor's own
        // return target). The old session's state is rebuilt, not patched.
        host_.clear_previews();
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        s_.currentScreen = ScreenType::SAMPLE_EDITOR;
        init_sample_editor_state();
        return;
    }

    close_file_browser();
}

// ─── The multi-select and the file clipboard ─────────────────────────────────────────────────────

std::vector<std::string> InputDispatcher::browser_selected_paths() const {
    const FileBrowserState& b = s_.fileBrowser;
    std::vector<std::string> out;
    if (!b.selectionMode || b.selectionAnchor < 0) return out;

    const int lo = std::max(std::min(b.selectionAnchor, b.cursor), b.first_selectable());
    const int hi = std::max(b.selectionAnchor, b.cursor);
    for (int i = lo; i <= hi; ++i) {
        const BrowserItem* item = b.item_at(i);
        if (item && !item->is_pseudo()) out.push_back(item->path);
    }
    return out;
}

void InputDispatcher::browser_paste() {
    FileBrowserState& b = s_.fileBrowser;
    if (b.fileClipboard.empty()) return;

    const std::string& dest = b.currentDirectory;
    int done = 0, failed = 0;

    for (const std::string& src : b.fileClipboard) {
        if (!fs_.file_exists(src)) { ++failed; continue; }

        const std::string name = path_name(src);
        std::string       target = dest + "/" + name;
        if (target == src) { ++done; continue; }   // pasted back into the source folder

        // De-duplicate: "kick.wav" → "kick_2.wav" → "kick_3.wav". Never overwrite.
        if (fs_.file_exists(target)) {
            const std::string ext  = path_extension(name);
            const std::string base = path_stem(name);
            for (int n = 2;; ++n) {
                target = dest + "/" + base + "_" + std::to_string(n) + (ext.empty() ? "" : "." + ext);
                if (!fs_.file_exists(target)) break;
            }
        }

        const bool ok = b.fileClipboardIsCut ? fs_.move_file(src, target) : fs_.copy_file(src, target);
        if (ok) ++done; else ++failed;
    }

    const bool  cut  = b.fileClipboardIsCut;
    const char* verb = cut ? "MOVED" : "COPIED";

    // A CUT clipboard is spent once pasted; a COPY can be pasted into several folders.
    if (cut) b.fileClipboard.clear();

    refresh_browser();
    b.statusMessage = failed == 0
                          ? std::string(verb) + " " + std::to_string(done) +
                                (done == 1 ? " FILE" : " FILES")
                          : std::string(verb) + " " + std::to_string(done) + ", FAILED " +
                                std::to_string(failed);
    b.statusSuccess = (failed == 0);
}

// ─── SELECT + A / B / R — the browser's file-management chords ───────────────────────────────────

void InputDispatcher::on_select_a() {
    if (layer_takes(Gesture::SELECT_A)) return;
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⭐ On a granted TREE the file chords mean nothing, so SELECT+A offers the one thing it can: make this
    // the folder the app keeps its directories in. Armed — A confirms, B cancels.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::SET_HOME;
        s_.fileBrowser.statusMessage.clear();
        s_.fileBrowser.statusSuccess = true;
        return;
    }

    if (!item || item->is_pseudo()) return;   // ".." and ADD FOLDER… are not files

    const bool  dir   = (item->kind == BrowserItem::Kind::FOLDER);
    const std::string ext = to_lower(item->extension);
    const char* label = dir              ? "FOLDER NAME:"
                        : (ext == "wav") ? "SAMPLE NAME:"
                        : (ext == "ptp") ? "PROJECT NAME:"
                                         : "FILE NAME:";

    // A FOLDER's displayName is "[name]"; the brackets must not reach the rename box.
    const std::string base = dir ? path_name(item->path) : item->displayName;
    open_qwerty(QwertyContext::FILE_RENAME, base, label, item->path);
}

void InputDispatcher::on_select_b() {
    if (layer_takes(Gesture::SELECT_B)) return;
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⚠️ On a granted TREE this is FORGET, not DELETE: `delete_path("pt://<id>")` would remove the user's
    // whole PocketTracker directory. Handing back the PERMISSION removes the row and touches no file — the
    // only way to clear a grant whose folder was deleted.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::FORGET_ROOT;
        s_.fileBrowser.statusMessage.clear();
        s_.fileBrowser.statusSuccess = true;
        return;
    }

    if (!item || item->is_pseudo()) return;

    // ARM the confirm; never delete on this press ("DELETE <name>? A=YES B=NO").
    s_.fileBrowser.mode          = BrowserMode::DELETE;
    s_.fileBrowser.statusMessage.clear();
    s_.fileBrowser.statusSuccess = true;
}

void InputDispatcher::on_select_r() {
    if (layer_takes(Gesture::SELECT_R)) return;
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;
    open_qwerty(QwertyContext::FOLDER_CREATE, "NEW FOLDER", "FOLDER NAME:",
                s_.fileBrowser.currentDirectory);
}

// ─── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_qwerty(QwertyContext context, const std::string& initial_text,
                                  const std::string& field_label, const std::string& context_extra,
                                  int max_length, bool clear_on_first_b) {
    QwertyKeyboardState k{};
    k.isOpen        = true;
    k.text          = initial_text.substr(0, static_cast<size_t>(max_length));
    k.maxLength     = max_length;
    k.textCursor    = static_cast<int>(k.text.size());
    k.fieldLabel    = field_label;
    k.contextExtra  = context_extra;
    k.context       = context;
    k.clearOnFirstB = clear_on_first_b;
    k.insertBefore  = s_.settings.insertBefore;   // read at OPEN, so flipping the setting cannot change
    s_.qwerty       = k;                 // what the buttons mean mid-word
}

void InputDispatcher::qwerty_apply() {
    const QwertyKeyboardState k    = s_.qwerty;   // by value: every arm below closes the keyboard
    const std::string         text = trimmed_text(k);
    s_.qwerty = QwertyKeyboardState{};

    switch (k.context) {
        case QwertyContext::FILE_RENAME: {
            // An empty field means "leave it alone", not "name it nothing".
            const std::string name = text.empty() ? path_stem(k.contextExtra) : text;
            if (fs_.rename_file(k.contextExtra, name)) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "RENAMED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "RENAME FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::FOLDER_CREATE: {
            const std::string name = text.empty() ? "NewFolder" : text;
            if (!fs_.create_folder(k.contextExtra, name).empty()) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "CREATED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "CREATE FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::INSTRUMENT_NAME: {
            Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(s_.currentInstrument)];
            // A cleared name reverts to "INSTxx": an unnamed instrument must still be identifiable.
            ins.name = text.empty() ? songcore::default_instrument_name(ins.id) : text;
            mark_modified();
            break;
        }

        case QwertyContext::PROJECT_NAME:
            // An empty name is allowed; SAVE supplies a fallback filename (save_project).
            host_.edit_project().name = text;
            mark_modified();
            break;

        case QwertyContext::INSTRUMENT_SAVE: {
            const std::string name = text.empty() ? "PRESET" : text;
            const std::string path = k.contextExtra + "/" + name + ".pti";
            if (save_instrument_preset(host_, fs_, s_.currentInstrument, path)) {
                s_.statusMessage = "SAVED: " + name;
                s_.statusSuccess = true;
            } else {
                s_.statusMessage = "SAVE FAILED";
                s_.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::THEME_SAVE:
            // See `save_theme_as`. ⚠️ `k.contextExtra`, not `s_.qwerty.contextExtra`: the live keyboard was
            // cleared at the top of this function.
            save_theme_as(k.contextExtra, text);
            break;

        case QwertyContext::SCALE_SAVE:
            // ⚠️ `k.contextExtra` — the live keyboard is already cleared.
            save_scale_as(k.contextExtra, text);
            break;

        case QwertyContext::GROOVE_SAVE:
            save_groove_as(k.contextExtra, text);
            break;

        case QwertyContext::SAMPLE_NAME: {
            // Renames BOTH the editor's sample and the INSTRUMENT holding it — one thing to the user. An
            // empty field keeps the current name.
            SampleEditorState& se = s_.sampleEditor;
            const std::string  name = text.empty() ? se.sampleName : text;
            se.sampleName = name;
            host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)].name = name;
            mark_modified();
            break;
        }

        case QwertyContext::SAMPLE_SAVE: {
            // SAVE-AS DE-DUPLICATES (`SNARE.wav`, `SNARE_0001.wav`, …) — OVERWRITE is its own button, and a
            // silent replace would be a destructive act with no confirm.
            const std::string base = text.empty() ? "SAMPLE" : text;
            std::string       path = k.contextExtra + "/" + base + ".wav";
            for (int n = 1; fs_.file_exists(path); ++n) {
                char suffix[16];   // "_%04d" of an int can be 12 bytes (-Wformat-truncation)
                std::snprintf(suffix, sizeof(suffix), "_%04d", n);
                path = k.contextExtra + "/" + base + suffix + ".wav";
            }
            save_sample_to(path, /*adopt_name=*/true);
            break;
        }

        case QwertyContext::RESAMPLE:
            // Empty → auto Resample_NNNN; anything typed is the base name. The selection is still live
            // (the keyboard never touched it).
            resample_selection(text);
            break;
    }
}

}  // namespace pt::ui
