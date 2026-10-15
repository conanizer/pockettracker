// The file browser — its buttons, opening, the cursor, loading, the file clipboard.

#include "ui/dispatch/dispatch_common.h"

#include "ui/fx_preset_io.h"
#include "ui/groove_io.h"
#include "ui/scale_io.h"
#include "ui/std_filesystem.h"   // path_name / path_stem / path_extension / to_lower
#include "ui/theme_io.h"
#include "common/load_progress.h"       // load_cancelled

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

std::string file_count(size_t n) { return std::to_string(n) + (n == 1 ? " FILE" : " FILES"); }

void end_selection(FileBrowserState& b) {
    b.selectionMode   = false;
    b.selectionAnchor = -1;
}

}  // namespace

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::file_browser_screen(Gesture g) {
    FileBrowserState& fb = s_.fileBrowser;
    switch (g) {
        // UP/DOWN move a row; LEFT/RIGHT page by a screenful.
        case Gesture::DPAD_UP:    browser_move_cursor(-1, /*page=*/false); break;
        case Gesture::DPAD_DOWN:  browser_move_cursor(+1, /*page=*/false); break;
        case Gesture::DPAD_LEFT:  browser_move_cursor(-BROWSER_VISIBLE_ROWS, /*page=*/true); break;
        case Gesture::DPAD_RIGHT: browser_move_cursor(+BROWSER_VISIBLE_ROWS, /*page=*/true); break;

        case Gesture::A:     browser_confirm(); break;
        case Gesture::B:     browser_back(); break;
        case Gesture::START: browser_audition(); break;

        // An audition rings until stopped; scrolling must not stack them.
        case Gesture::STOP_PREVIEW: host_.stop_preview(); break;

        // Help is the shared path's, which knows the panel has no room here.
        case Gesture::SELECT: return GestureResult::PASS;

        // ⚠️ Not navigation: the browser is not a grid cell. Down a directory is A on a folder.
        case Gesture::R_UP:   browser_cycle_sort(+1); break;
        case Gesture::R_DOWN: browser_cycle_sort(-1); break;
        case Gesture::R_LEFT: navigate_to_parent(fb, fs_); break;

        case Gesture::L_B: browser_select(); break;
        case Gesture::L_A: browser_cut_or_paste(); break;
        case Gesture::L_R: end_selection(fb); break;

        case Gesture::SELECT_A: browser_rename(); break;
        case Gesture::SELECT_B: browser_arm_delete(); break;
        case Gesture::SELECT_R: browser_new_folder(); break;

        default: break;   // every other gesture means nothing here
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::browser_back() {
    FileBrowserState& fb = s_.fileBrowser;

    // B is the NO: it disarms any mode rather than leaving, so an accidental SELECT+A/B is harmless.
    if (fb.mode != BrowserMode::NORMAL) { fb.mode = BrowserMode::NORMAL; return; }

    // Inside a file selection, B COPIES it — as over a grid.
    if (fb.selectionMode) {
        std::vector<std::string> files = browser_selected_paths();
        if (!files.empty()) {
            const size_t n = files.size();
            fb.fileClipboard      = std::move(files);
            fb.fileClipboardIsCut = false;
            set_status(fb, "CPY " + file_count(n), true);
        }
        end_selection(fb);
        return;
    }

    close_file_browser();
}

void InputDispatcher::browser_audition() {
    // Decoded into slot 255 on the preview lane; every audible extension, from every context.
    const BrowserItem* item = s_.fileBrowser.current();
    if (!item || item->kind != BrowserItem::Kind::FILE) return;

    const std::string ext = to_lower(item->extension);
    const bool audible = std::find(sample_extensions().begin(), sample_extensions().end(), ext) !=
                         sample_extensions().end();
    if (!audible) return;   // a .pti or an .sf2 has no waveform

    // No song cell behind a file: neutral gain.
    host_.set_preview_track(-1);
    // ⚠️ A full decode (a long mp3 is a real load), so the loading strip and B.
    const LoadScope previewScope(*this, now_ms_, item->displayName);
    if (!host_.preview_file(item->path)) {
        if (host_.last_load_cancelled()) return;   // stopped on purpose
        set_status(s_.fileBrowser, "PREVIEW FAILED", false);
    }
}

// ─── Opening and closing the browser ─────────────────────────────────────────────────────────────

std::string InputDispatcher::browser_dir(BrowserDir cat) {
    // The config.json override per category, else the built-in default.
    const std::optional<std::string>* ov = nullptr;
    std::string                       def;
    switch (cat) {
        case BrowserDir::SAMPLES:     ov = &s_.folderConfig.samples;     def = fs_.samples_directory();     break;
        case BrowserDir::SOUNDFONTS:  ov = &s_.folderConfig.soundfonts;  def = fs_.soundfonts_directory();  break;
        case BrowserDir::INSTRUMENTS: ov = &s_.folderConfig.instruments; def = fs_.instruments_directory(); break;
        case BrowserDir::PROJECTS:    ov = &s_.folderConfig.projects;    def = fs_.projects_directory();    break;
        case BrowserDir::THEMES:      ov = &s_.folderConfig.themes;      def = fs_.themes_directory();      break;
    }
    // ⚠️ The whole rule is resolve_browse_dir; inline none of it, or config and sample paths disagree.
    return ov ? resolve_browse_dir(fs_, *ov, def) : def;
}

void InputDispatcher::open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                                        const std::vector<std::string>& extensions) {
    s_.previousScreen = s_.currentScreen;
    s_.browserPurpose = purpose;

    s_.fileBrowser.fileExtensions = extensions;
    s_.fileBrowser.mode           = BrowserMode::NORMAL;

    // FOLDER = REMEMBER: a sample load starts in the last sample's folder, if it still exists.
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
    end_selection(s_.fileBrowser);
    s_.currentScreen = s_.previousScreen;
}

void InputDispatcher::refresh_browser() {
    FileBrowserState& b = s_.fileBrowser;

    // ⚠️ SafFileSystem caches listings and drops them only on this app's writes; forget it here, or files
    // added from outside stay invisible.
    fs_.forget_listing(b.currentDirectory);

    // Re-list keeping the cursor (a refresh is not a navigation), clamped to the shorter list.
    rebuild_items(b, fs_);

    const int last = static_cast<int>(b.items.size()) - 1;
    b.cursor = std::clamp(b.cursor, 0, std::max(last, 0));

    if (b.cursor < b.scroll) b.scroll = b.cursor;
    if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    b.scroll = std::clamp(b.scroll, 0, std::max(0, last - BROWSER_VISIBLE_ROWS + 1));
}

void InputDispatcher::refresh_browser_on_foreground() {
    // ⚠️ Only on the browser: its state outlives the screen.
    if (s_.currentScreen != ScreenType::FILE_BROWSER) return;

    // ⚠️ Not under a modal: a refresh can move the cursor, leaving "DELETE X?" with A deleting Y.
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    refresh_browser();
}

// ─── The browser's cursor ────────────────────────────────────────────────────────────────────────

void InputDispatcher::browser_move_cursor(int delta, bool page) {
    FileBrowserState& b     = s_.fileBrowser;
    const int         total = static_cast<int>(b.items.size());
    if (total == 0) return;

    // ⚠️ UP/DOWN wrap; a page clamps (wrapping would fling you across a 400-file folder).
    if (page) {
        b.cursor = std::clamp(b.cursor + delta, 0, total - 1);
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

    // DELETE mode: A is the YES, on what the question named — the selection, or the cursor's entry.
    if (b.mode == BrowserMode::DELETE) {
        b.mode = BrowserMode::NORMAL;
        // Copied out first: the refresh below rebuilds the list the targets point into.
        const std::vector<const BrowserItem*> targets = b.delete_targets();
        if (targets.empty()) return;
        const std::string        name = targets.front()->displayName;
        std::vector<std::string> paths;
        for (const BrowserItem* item : targets) paths.push_back(item->path);

        int done = 0, failed = 0;
        for (const std::string& path : paths) {
            if (fs_.delete_path(path)) ++done; else ++failed;
        }
        end_selection(b);
        if (done > 0) refresh_browser();

        if (paths.size() == 1)
            set_status(b, done ? "DELETED: " + name : "DELETE FAILED", failed == 0);
        else
            set_status(b, "DELETED " + file_count(static_cast<size_t>(done)) +
                              (failed ? ", FAILED " + std::to_string(failed) : ""), failed == 0);
        return;
    }

    // SET_HOME / FORGET_ROOT: A is the YES, armed by SELECT+A / SELECT+B on a granted tree.
    if (b.mode == BrowserMode::SET_HOME || b.mode == BrowserMode::FORGET_ROOT) {
        const bool         home = (b.mode == BrowserMode::SET_HOME);
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (!(home ? fs_.set_home_directory(item->path) : fs_.revoke_access(item->path))) {
            set_status(b, home ? "COULD NOT SET HOME FOLDER" : "COULD NOT FORGET IT", false);
            return;
        }
        // ⚠️ The derived roots were read at boot and must follow the home (a forgotten tree may have been
        // it), or media resolves against the old tree.
        const std::string root = fs_.parent_path(fs_.samples_directory());
        set_media_base_dir(root);
        host_.set_app_root(root);

        refresh_browser();
        set_status(b, (home ? "HOME FOLDER: " : "FORGOT: ") + name, true);
        return;
    }

    const BrowserItem* item = b.current();
    if (!item) return;

    if (item->is_parent()) { navigate_to_parent(b, fs_); return; }

    // An ACTION is the filesystem's to perform. ⚠️ No refresh: activate does not wait (Android's picker);
    // refresh_browser_on_foreground catches up.
    if (item->kind == BrowserItem::Kind::ACTION) {
        const std::string label = item->displayName;
        if (!fs_.activate(item->path)) {
            set_status(b, label + " FAILED", false);
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
    // The name the slot is about to stop deserving, read before the load.
    const std::string previousAutoName = instrument_auto_name(host_.project(), sourceId);

    // ⚠️ Every arm: slowness is the file's and the device's. A fast load draws nothing.
    const LoadScope loadScope(*this, now_ms_, stem);

    bool ok = false;
    switch (s_.browserPurpose) {
        case AppState::BrowserPurpose::LOAD_PRESET:
            ok = host_.load_instrument_preset(id, path);
            break;

        case AppState::BrowserPurpose::LOAD_SOURCE:
            // The extension decides, not the slot's type: an .sf2/.sf3 turns a sampler slot into a
            // SoundFont.
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
            // ⚠️ The whole document, returning early: load_project_file is parse, push, media and params.
            if (!host_.load_project_file(path, fs_.samples_directory())) {
                set_status(b, "LOAD FAILED", false);
                return;
            }
            // ⚠️ A cancelled project load cannot stay half-done (later instruments would play silence):
            // NEW PROJECT's blank document, and the message says so.
            if (pt::load_cancelled()) {
                host_.new_project();
                host_.push_params();
                // No source path, and the autosave cleared.
                load_project_done("");
                set_status(s_, "LOAD CANCELLED", true);
                return;
            }
            load_project_done(path);
            return;

        case AppState::BrowserPurpose::LOAD_THEME:
            // ⚠️ Pixels only, returning early. The extension is re-checked (the D-pad leaves the folder),
            // and a failed parse must not blank the palette.
            if (ext != "ptt" || !load_theme_file(fs_, path, s_.theme)) {
                set_status(b, "LOAD FAILED", false);
                return;
            }
            // Back to the editor that raised the browser (`theme_row_action` closed it on the way out).
            close_file_browser();
            open_theme_editor();
            set_status(s_, "THEME LOADED", true);
            return;

        case AppState::BrowserPurpose::LOAD_SCALE: {
            // ⚠️ Into a copy that keeps the slot's id, committed only once it parses. The extension is
            // re-checked: the D-pad walks out of the folder.
            songcore::Scale loaded = host_.project().scales[static_cast<size_t>(s_.currentScale)];
            if (ext != SCALE_FILE_EXT || !load_scale_file(fs_, path, loaded)) {
                set_status(b, "LOAD FAILED", false);
                return;
            }
            host_.edit_project().scales[static_cast<size_t>(s_.currentScale)] = loaded;
            mark_modified();
            close_file_browser();
            set_status(s_, "SCALE LOADED", true);
            return;
        }

        case AppState::BrowserPurpose::LOAD_GROOVE: {
            // The scale load's two guards, for the same reasons (the slot's `id` is what `GRV` names).
            songcore::Groove loaded = host_.project().grooves[static_cast<size_t>(s_.currentGroove)];
            if (ext != GROOVE_FILE_EXT || !load_groove_file(fs_, path, loaded)) {
                set_status(b, "LOAD FAILED", false);
                return;
            }
            host_.edit_project().grooves[static_cast<size_t>(s_.currentGroove)] = loaded;
            mark_modified();
            close_file_browser();
            set_status(s_, "GROOVE LOADED", true);
            return;
        }

        case AppState::BrowserPurpose::LOAD_REVERB: {
            // Read into a COPY of the cells, committed only once the file parses.
            ReverbCells cells = reverb_cells_of(host_.project());
            if (ext != REVERB_FILE_EXT || !load_reverb_file(fs_, path, cells)) {
                set_status(b, "LOAD FAILED", false);
                return;
            }
            apply_reverb_cells(host_.edit_project(), cells);
            // ⚠️ Closed first: mark_modified pushes only from EFFECTS, so the preset would show and not sound.
            close_file_browser();
            mark_modified();
            set_status(s_, "REVERB LOADED", true);
            return;
        }

        case AppState::BrowserPurpose::LOAD_DELAY: {
            // As the reverb's, including the order of the last two calls.
            DelayCells cells = delay_cells_of(host_.project());
            if (ext != DELAY_FILE_EXT || !load_delay_file(fs_, path, cells)) {
                set_status(b, "LOAD FAILED", false);
                return;
            }
            apply_delay_cells(host_.edit_project(), cells);
            close_file_browser();
            mark_modified();
            set_status(s_, "DELAY LOADED", true);
            return;
        }
    }

    if (!ok) {
        // ⚠️ A cancel is not a failure: no red line.
        if (host_.last_load_cancelled()) {
            set_status(b, "CANCELLED", true);
            return;
        }
        // ⚠️ A file too big for the device is not corrupt; say so.
        set_status(b, host_.last_load_ran_out_of_memory() ? "FILE TOO BIG" : "LOAD FAILED", false);
        return;
    }

    // The slot adopts the file's name unless the user TYPED its current one (not the default, not the
    // name it took from the replaced source). Both source loads apply it.
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

    // FOLDER = REMEMBER: sample loads only (not an SF from a sampler slot); saved on exit.
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        !is_soundfont_extension(ext)) {
        s_.settings.lastSampleFolder = b.currentDirectory;
    }

    if (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) {
        // Back into the editor on the new audio (previousScreen is the editor's return target), rebuilt.
        close_file_browser();
        s_.currentScreen = ScreenType::SAMPLE_EDITOR;
        init_sample_editor_state();
        return;
    }

    close_file_browser();
}

// ─── The multi-select and the file clipboard ─────────────────────────────────────────────────────

void InputDispatcher::browser_select() {
    // ⚠️ A plain anchor..cursor range (a second tap within 500 ms selects all) — not the grids' widener.
    FileBrowserState& b = s_.fileBrowser;
    if (b.mode != BrowserMode::NORMAL) return;

    if (!b.selectionMode) {
        b.selectionMode   = true;
        b.selectionAnchor = b.cursor;
        b.lastSelectTapMs = now_ms_;
    } else if (now_ms_ - b.lastSelectTapMs <= 500) {
        // Tap again inside the window: select everything, skipping the ".." row.
        const int first = b.first_selectable();
        const int last  = std::max(static_cast<int>(b.items.size()) - 1, first);
        b.selectionAnchor = first;
        b.cursor          = last;
        b.scroll          = std::max(0, last - BROWSER_VISIBLE_ROWS + 1);
        b.lastSelectTapMs = 0;   // …so a third tap re-anchors rather than re-selecting all
    } else {
        b.selectionAnchor = b.cursor;   // the window lapsed — start a fresh range here
        b.lastSelectTapMs = now_ms_;
    }
}

void InputDispatcher::browser_cut_or_paste() {
    FileBrowserState& b = s_.fileBrowser;
    if (b.mode != BrowserMode::NORMAL) return;

    if (b.selectionMode) {
        std::vector<std::string> files = browser_selected_paths();
        if (files.empty()) return;
        const size_t n = files.size();

        b.fileClipboard      = std::move(files);
        b.fileClipboardIsCut = true;
        end_selection(b);
        set_status(b, "CUT " + file_count(n), true);
    } else if (!b.fileClipboard.empty()) {
        browser_paste();
    }
}

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
    set_status(b,
               std::string(verb) + " " +
                   (failed == 0 ? file_count(static_cast<size_t>(done))
                                : std::to_string(done) + ", FAILED " + std::to_string(failed)),
               failed == 0);
}

// ─── SELECT + A / B / R — the browser's file-management chords ───────────────────────────────────

void InputDispatcher::browser_rename() {
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // On a granted tree SELECT+A offers to make it the home folder. Armed: A confirms, B cancels.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::SET_HOME;
        set_status(s_.fileBrowser, "", true);
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

void InputDispatcher::browser_arm_delete() {
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⚠️ On a granted tree this is FORGET: deleting "pt://<id>" would remove the whole directory. Handing
    // back the permission touches no file.
    if (!s_.fileBrowser.selectionMode && item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::FORGET_ROOT;
        set_status(s_.fileBrowser, "", true);
        return;
    }

    if (s_.fileBrowser.delete_targets().empty()) return;

    // ARM the confirm; never delete on this press ("DELETE <name>? A=YES B=NO", or "DELETE 3 FILES?").
    s_.fileBrowser.mode          = BrowserMode::DELETE;
    set_status(s_.fileBrowser, "", true);
}

void InputDispatcher::browser_new_folder() {
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;
    open_qwerty(QwertyContext::FOLDER_CREATE, "NEW FOLDER", "FOLDER NAME:",
                s_.fileBrowser.currentDirectory);
}

// ─── Sorting ─────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::browser_cycle_sort(int delta) {
    FileBrowserState& b = s_.fileBrowser;

    // Steps the modes by index, so the enum's order is behaviour (ui/filesystem.h).
    const int next = (static_cast<int>(b.sortMode) + delta + FILE_SORT_MODE_COUNT) % FILE_SORT_MODE_COUNT;
    b.sortMode = static_cast<FileSortMode>(next);

    // ⚠️ Rebuild rather than re-sort in place, or the tie-break depends on the previous sort mode.
    rebuild_items(b, fs_);

    // The cursor stays put, so the row under it now holds a different file — the list is re-ordered.
    set_status(b, file_sort_label(b.sortMode), true);
}

}  // namespace pt::ui
