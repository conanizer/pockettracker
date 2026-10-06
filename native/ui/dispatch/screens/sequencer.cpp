// SONG, CHAIN, PHRASE and TABLE: their buttons — A on an empty cell inserts, A,A inserts the next
// unused item, L+B+A clones, B+DPAD changes what is on screen — and the selection's clipboard they share.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"
#include "ui/song_pointer.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace pt::ui {

namespace {

/** A phrase nobody has written a note into. */
bool phrase_is_blank(const Phrase& p) {
    for (const songcore::PhraseStep& s : p.steps)
        if (!songcore::step_is_empty(s)) return false;
    return true;
}

/** A chain that references no phrase. */
bool chain_is_blank(const Chain& c) {
    for (const int ref : c.phraseRefs)
        if (ref != -1) return false;
    return true;
}

/**
 * Search forward from `start`, wrapping once; −1 when the pool is full. Starting after the last edited
 * item hands you a free one near it, not slot 0 every time.
 */
template <typename Pred>
int first_from_wrapping(int start, int count, Pred pred) {
    for (int i = start; i < count; ++i)
        if (pred(i)) return i;
    for (int i = 0; i < start && i < count; ++i)
        if (pred(i)) return i;
    return -1;
}

/** Phrase IDs any chain references — "used" even when blank (a silent spacer inside a pad chain). */
std::set<int> used_phrase_ids(const Project& p) {
    std::set<int> used;
    for (const Chain& c : p.chains)
        for (const int ref : c.phraseRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

/** Chain IDs any song track references — same "used even if blank" reasoning. */
std::set<int> used_chain_ids(const Project& p) {
    std::set<int> used;
    for (const songcore::Track& t : p.tracks)
        for (const int ref : t.chainRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

}  // namespace

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::song_screen(Gesture g) {
    switch (g) {
        case Gesture::A:
            if (open_sub_screen_at_cursor(/*peek=*/false)) return GestureResult::TAKEN;
            song_insert();
            return GestureResult::TAKEN;

        case Gesture::A_A:
            // ⚠️ RESAMPLE goes before the double-tap gate: under a selection the first A copied rather
            // than inserting, so the gate would turn it away.
            if (s_.selection.active) {
                open_qwerty(QwertyContext::RESAMPLE, resample_base_name(fs_), "SAMPLE NAME:", "",
                            /*max_length=*/20, /*clear_on_first_b=*/true);
                return GestureResult::TAKEN;
            }
            if (take_double_tap()) song_insert_next();
            return GestureResult::TAKEN;

        // ⭐ B+LEFT/RIGHT toggles the transport mode; both directions toggle, since there are two modes.
        case Gesture::B_LEFT:
        case Gesture::B_RIGHT:
            host_.set_live_mode(!host_.live_mode());
            return GestureResult::TAKEN;

        // Pages by 16 rows.
        case Gesture::B_UP:
            s_.cursorRow = std::max(0, s_.cursorRow - 16);
            scroll_song_to_row(s_, s_.cursorRow);
            return GestureResult::TAKEN;
        case Gesture::B_DOWN:
            s_.cursorRow = std::min(255, s_.cursorRow + 16);
            scroll_song_to_row(s_, s_.cursorRow);
            return GestureResult::TAKEN;

        case Gesture::L_B_A:
            song_clone_chain();
            return GestureResult::PASS;   // the body ends the selection

        // LIVE mode's three launches. Outside LIVE, START is the transport and the two chords do nothing.
        case Gesture::START:
            if (!live_song_gesture()) return GestureResult::PASS;
            live_launch_cell();
            return GestureResult::TAKEN;
        case Gesture::L_START:
            if (live_song_gesture()) live_launch_row();
            return GestureResult::TAKEN;
        case Gesture::R_START:
            if (live_song_gesture()) live_stop_track();
            return GestureResult::TAKEN;

        default:
            return grid_clipboard(g, &InputDispatcher::song_clip, /*pasteTarget=*/0);
    }
}

GestureResult InputDispatcher::chain_screen(Gesture g) {
    switch (g) {
        case Gesture::A:
            if (open_sub_screen_at_cursor(/*peek=*/false)) return GestureResult::TAKEN;
            chain_insert();
            return GestureResult::TAKEN;

        case Gesture::A_A:
            if (take_double_tap()) chain_insert_next();
            return GestureResult::TAKEN;

        // Under NAV = SONG, B+DPAD walks the song instead of the pool (ui/song_pointer.h).
        case Gesture::B_LEFT:
        case Gesture::B_RIGHT: {
            const int delta = (g == Gesture::B_LEFT) ? -1 : +1;
            if (song_relative_b_horizontal(delta)) return GestureResult::TAKEN;
            s_.currentChain    = step_wrapping(s_.currentChain, delta, 256);
            s_.lastEditedChain = s_.currentChain;
            return GestureResult::TAKEN;
        }
        case Gesture::B_UP:   song_relative_b_vertical(-1); return GestureResult::TAKEN;
        case Gesture::B_DOWN: song_relative_b_vertical(+1); return GestureResult::TAKEN;

        case Gesture::L_B_A:
            chain_clone_phrase();
            return GestureResult::PASS;   // the body ends the selection

        default:
            return grid_clipboard(g, &InputDispatcher::chain_clip, s_.currentChain);
    }
}

GestureResult InputDispatcher::phrase_screen(Gesture g) {
    switch (g) {
        case Gesture::A:
            if (open_sub_screen_at_cursor(/*peek=*/false)) return GestureResult::TAKEN;
            phrase_insert();
            return GestureResult::TAKEN;

        // ⚠️ The audition is owed on every exit — a quick second A never reaches `phrase_insert`.
        case Gesture::A_A:
            if (take_double_tap()) phrase_next_instrument();
            preview_held_note();
            return GestureResult::TAKEN;

        // An FX-TYPE cell opens the picker instead of stepping through 60 effects.
        case Gesture::A_UP:
        case Gesture::A_DOWN:
            if (!on_fx_type_column()) return GestureResult::PASS;
            open_fx_helper();
            return GestureResult::TAKEN;

        case Gesture::B_LEFT:
        case Gesture::B_RIGHT: {
            const int delta = (g == Gesture::B_LEFT) ? -1 : +1;
            if (song_relative_b_horizontal(delta)) return GestureResult::TAKEN;
            s_.currentPhrase    = step_wrapping(s_.currentPhrase, delta, 256);
            s_.lastEditedPhrase = s_.currentPhrase;
            return GestureResult::TAKEN;
        }
        case Gesture::B_UP:   song_relative_b_vertical(-1); return GestureResult::TAKEN;
        case Gesture::B_DOWN: song_relative_b_vertical(+1); return GestureResult::TAKEN;

        case Gesture::L_B_A:
            phrase_clone();
            return GestureResult::PASS;   // the body ends the selection

        // Only stops what it can start: a held-A preview, when the setting is on.
        case Gesture::STOP_PREVIEW:
            if (s_.settings.notePreviewEnabled) host_.stop_preview();
            return GestureResult::TAKEN;

        default:
            return grid_clipboard(g, &InputDispatcher::phrase_clip, s_.currentPhrase);
    }
}

GestureResult InputDispatcher::table_screen(Gesture g) {
    switch (g) {
        case Gesture::A_UP:
        case Gesture::A_DOWN:
            if (!on_fx_type_column()) return GestureResult::PASS;
            open_fx_helper();
            return GestureResult::TAKEN;

        case Gesture::B_LEFT:
        case Gesture::B_RIGHT:
            s_.currentTable    = step_wrapping(s_.currentTable, (g == Gesture::B_LEFT) ? -1 : +1, 128);
            s_.lastEditedTable = s_.currentTable;
            return GestureResult::TAKEN;

        // ⚠️ START is not the transport here: it auditions on the preview lane THROUGH the table on
        // screen (instrument N owns table N), or you would hear the instrument's own table instead.
        case Gesture::START:
            host_.set_preview_track(audition_track());
            host_.preview_instrument(s_.currentTable, /*tableIdOverride=*/s_.currentTable);
            return GestureResult::TAKEN;
        case Gesture::STOP_PREVIEW:
            host_.stop_preview();
            return GestureResult::TAKEN;

        default:
            return grid_clipboard(g, &InputDispatcher::table_clip, s_.currentTable);
    }
}

// ─── The selection's clipboard ───────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::grid_clipboard(Gesture g, ClipFn clip, int pasteTarget) {
    const GridCursor& cursor = *grid();  // only the four grid screens call this
    switch (g) {
        case Gesture::L_B:
            s_.selection.handle_select_b(now_ms_, s_.*cursor.row, s_.*cursor.column,
                                         cursor.maxColumn, cursor.maxRow);
            return GestureResult::TAKEN;

        // Inside a selection: B copies, A+B deletes, L+A cuts — and each ends it. Outside one, B does
        // nothing, A+B is the cell's own delete, and L+A pastes.
        case Gesture::B:
            if (!s_.selection.active) return GestureResult::TAKEN;
            (this->*clip)(ClipOp::COPY, s_.selection.bounds());
            s_.selection.exit();
            return GestureResult::TAKEN;

        case Gesture::A_B:
            if (!s_.selection.active) return GestureResult::PASS;
            (this->*clip)(ClipOp::DELETE, s_.selection.bounds());
            mark_modified();
            s_.selection.exit();
            return GestureResult::TAKEN;

        case Gesture::L_A: {
            if (s_.selection.active) {
                (this->*clip)(ClipOp::CUT, s_.selection.bounds());
                mark_modified();
                s_.selection.exit();
                return GestureResult::TAKEN;
            }
            const PasteResult r = clip_.paste(host_.edit_project(), s_.currentScreen, pasteTarget,
                                              s_.*cursor.row, s_.*cursor.column);
            if (r.kind == PasteResult::Kind::SUCCESS && r.itemsPasted > 0) mark_modified();
            return GestureResult::TAKEN;
        }

        default:
            return GestureResult::PASS;
    }
}

void InputDispatcher::song_clip(ClipOp op, const SelectionBounds& b) {
    Project& p = host_.edit_project();
    switch (op) {
        case ClipOp::COPY:
            clip_.copy_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::CUT:
            clip_.cut_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::DELETE:
            clip_.delete_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
    }
}

void InputDispatcher::chain_clip(ClipOp op, const SelectionBounds& b) {
    Project&  p  = host_.edit_project();
    const int id = s_.currentChain;
    switch (op) {
        case ClipOp::COPY:
            clip_.copy_chain_rows(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::CUT:
            clip_.cut_chain_rows(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::DELETE:
            clip_.delete_chain_rows(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
    }
}

void InputDispatcher::phrase_clip(ClipOp op, const SelectionBounds& b) {
    Project&  p  = host_.edit_project();
    const int id = s_.currentPhrase;
    switch (op) {
        case ClipOp::COPY:
            clip_.copy_phrase_steps(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::CUT:
            clip_.cut_phrase_steps(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::DELETE:
            clip_.delete_phrase_steps(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
    }
}

void InputDispatcher::table_clip(ClipOp op, const SelectionBounds& b) {
    Project&  p  = host_.edit_project();
    const int id = s_.currentTable;
    switch (op) {
        case ClipOp::COPY:
            clip_.copy_table_rows(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::CUT:
            clip_.cut_table_rows(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
        case ClipOp::DELETE:
            clip_.delete_table_rows(p, id, b.topLeftRow, b.topLeftColumn, b.bottomRightRow, b.bottomRightColumn);
            break;
    }
}

// ─── A on an empty cell, and A,A ─────────────────────────────────────────────────────────────────
//
// A on an EMPTY cell inserts the item last edited — so A lays down the last chain again, A,A a fresh one.

void InputDispatcher::arm_double_tap() {
    hasInsertPos_ = true;
    insertScreen_ = s_.currentScreen;
    insertRow_    = s_.cursorRow;
    insertCol_    = s_.cursorColumn;
}

bool InputDispatcher::take_double_tap() {
    const bool armed = hasInsertPos_ && insertScreen_ == s_.currentScreen &&
                       insertRow_ == s_.cursorRow && insertCol_ == s_.cursorColumn;
    if (armed) hasInsertPos_ = false;
    return armed;
}

void InputDispatcher::song_insert() {
    hasInsertPos_ = false;
    if (s_.selection.active) return;
    if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
    songcore::Track& track = host_.edit_project().tracks[static_cast<size_t>(s_.cursorColumn - 1)];
    while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow) track.chainRefs.push_back(-1);

    if (track.chainRefs[static_cast<size_t>(s_.cursorRow)] != -1) return;
    track.chainRefs[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedChain;
    mark_modified();
    arm_double_tap();   // a second press inserts the next UNUSED chain
}

void InputDispatcher::song_insert_next() {
    if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
    Project&         p     = host_.edit_project();
    songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];

    const int next = first_from_wrapping(s_.lastEditedChain + 1, 256, [&](int i) {
        return chain_is_blank(p.chains[static_cast<size_t>(i)]);
    });
    if (next < 0) return;

    while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow) track.chainRefs.push_back(-1);
    track.chainRefs[static_cast<size_t>(s_.cursorRow)] = next;
    s_.lastEditedChain                                 = next;
    mark_modified();
}

void InputDispatcher::chain_insert() {
    hasInsertPos_ = false;
    Chain& chain = host_.edit_project().chains[static_cast<size_t>(s_.currentChain)];
    if (chain.phraseRefs[static_cast<size_t>(s_.cursorRow)] != -1) return;
    chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = s_.lastEditedPhrase;
    chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
    mark_modified();
    arm_double_tap();   // a second press inserts the next UNUSED phrase
}

void InputDispatcher::chain_insert_next() {
    Project& p     = host_.edit_project();
    Chain&   chain = p.chains[static_cast<size_t>(s_.currentChain)];

    const int next = first_from_wrapping(s_.lastEditedPhrase + 1, 256, [&](int i) {
        return phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
    });
    if (next < 0) return;

    chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = next;
    chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
    s_.lastEditedPhrase                                      = next;
    mark_modified();
}

void InputDispatcher::phrase_insert() {
    hasInsertPos_ = false;
    if (s_.cursorColumn != 1 || s_.selection.active) return;   // the NOTE column only

    Phrase& ph = host_.edit_project().phrases[static_cast<size_t>(s_.currentPhrase)];
    PhraseEditorState ps{ph};
    ps.cursorRow    = s_.cursorRow;
    ps.cursorColumn = s_.cursorColumn;
    // A on an existing note inserts nothing, but holding it is how you listen.
    if (phrase_.cursor_context(ps).capabilities.isEmpty) {
        songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
        step.note       = s_.lastEditedNote;
        step.instrument = s_.lastEditedInstrument;
        step.volume     = s_.lastEditedVolume;
        mark_modified();
        arm_double_tap();   // a second press keeps the note and re-points it at the next FREE instrument
    }
    preview_held_note();
}

void InputDispatcher::phrase_next_instrument() {
    if (s_.cursorColumn != 1) return;   // the NOTE column only
    Project&              p    = host_.edit_project();
    songcore::PhraseStep& step = p.phrases[static_cast<size_t>(s_.currentPhrase)]
                                     .steps[static_cast<size_t>(s_.cursorRow)];

    // `instrument_is_free` also skips configured SoundFonts, which have a null sampleFilePath too.
    const int count = static_cast<int>(p.instruments.size());
    const int next  = first_from_wrapping(s_.lastEditedInstrument + 1, count, [&](int i) {
        return songcore::instrument_is_free(p.instruments[static_cast<size_t>(i)]);
    });
    if (next < 0) return;
    step.instrument         = next;
    s_.lastEditedInstrument = next;
    mark_modified();
}

// ─── L+B+A: clone ────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::song_clone_chain() {
    if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
    Project&         p     = host_.edit_project();
    songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
    const int currentChainId = (s_.cursorRow < static_cast<int>(track.chainRefs.size()))
                                   ? track.chainRefs[static_cast<size_t>(s_.cursorRow)]
                                   : -1;
    if (currentChainId == -1) return;

    const Chain&        src         = p.chains[static_cast<size_t>(currentChainId)];
    const std::set<int> usedChains  = used_chain_ids(p);
    const std::set<int> usedPhrases = used_phrase_ids(p);

    // The destination must be a FREE chain: blank AND unreferenced.
    const int dstChainId = first_from_wrapping(currentChainId + 1, 256, [&](int i) {
        return usedChains.count(i) == 0 && chain_is_blank(p.chains[static_cast<size_t>(i)]);
    });

    // A DEEP clone: every phrase the chain references gets its own free slot, so the copy is fully
    // independent. `reserved` stops two source phrases claiming the same destination; duplicate refs
    // inside the chain map to the SAME clone, which is what keeps a chain that plays phrase 5 twice
    // still playing one phrase twice.
    std::vector<int> srcPhraseIds;
    for (const int ref : src.phraseRefs)
        if (ref != -1 && std::find(srcPhraseIds.begin(), srcPhraseIds.end(), ref) == srcPhraseIds.end())
            srcPhraseIds.push_back(ref);

    std::set<int>      reserved;
    std::map<int, int> phraseMap;
    bool               enoughPhrases = true;
    for (const int pid : srcPhraseIds) {
        const int slot = first_from_wrapping(0, 256, [&](int i) {
            return reserved.count(i) == 0 && usedPhrases.count(i) == 0 &&
                   phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (slot < 0) { enoughPhrases = false; break; }
        reserved.insert(slot);
        phraseMap[pid] = slot;
    }

    // Capacity is checked in FULL before anything is written. Abort, never half-clone: a partial clone
    // leaves a chain pointing at phrases that were never copied.
    if (dstChainId < 0) {
        s_.statusMessage = "NO FREE CHAINS";
        s_.statusSuccess = false;
        return;
    }
    if (!enoughPhrases) {
        s_.statusMessage = "NO FREE PHRASES";
        s_.statusSuccess = false;
        return;
    }

    for (const auto& kv : phraseMap)
        p.phrases[static_cast<size_t>(kv.second)].steps = p.phrases[static_cast<size_t>(kv.first)].steps;

    Chain& dst = p.chains[static_cast<size_t>(dstChainId)];
    for (size_t i = 0; i < src.phraseRefs.size(); ++i) {
        const int ref     = src.phraseRefs[i];
        dst.phraseRefs[i] = (ref == -1) ? -1 : phraseMap[ref];
    }
    dst.transposeValues = src.transposeValues;

    track.chainRefs[static_cast<size_t>(s_.cursorRow)] = dstChainId;
    s_.lastEditedChain = dstChainId;
    s_.statusMessage   = "CHAIN CLONED";
    s_.statusSuccess   = true;
    mark_modified();
}

void InputDispatcher::chain_clone_phrase() {
    Project&  p               = host_.edit_project();
    Chain&    chain           = p.chains[static_cast<size_t>(s_.currentChain)];
    const int currentPhraseId = chain.phraseRefs[static_cast<size_t>(s_.cursorRow)];
    if (currentPhraseId == -1) return;

    const std::set<int> usedPhrases = used_phrase_ids(p);
    const int next = first_from_wrapping(currentPhraseId + 1, 256, [&](int i) {
        return usedPhrases.count(i) == 0 && phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
    });
    if (next < 0) return;

    p.phrases[static_cast<size_t>(next)].steps = p.phrases[static_cast<size_t>(currentPhraseId)].steps;
    chain.phraseRefs[static_cast<size_t>(s_.cursorRow)] = next;
    s_.lastEditedPhrase                                 = next;
    mark_modified();
}

void InputDispatcher::phrase_clone() {
    Project&            p           = host_.edit_project();
    const int           srcPhraseId = s_.currentPhrase;
    const std::set<int> usedPhrases = used_phrase_ids(p);
    const int next = first_from_wrapping(srcPhraseId + 1, 256, [&](int i) {
        return i != srcPhraseId && usedPhrases.count(i) == 0 &&
               phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
    });
    if (next < 0) return;

    p.phrases[static_cast<size_t>(next)].steps = p.phrases[static_cast<size_t>(srcPhraseId)].steps;
    s_.currentPhrase = next;   // …and follow the clone, so you are editing the copy
    mark_modified();
}

// ─── B+DPAD under NAV = SONG ─────────────────────────────────────────────────────────────────────

// CHAIN steps to the nearest filled cell either side; PHRASE to the nearest whose chain also holds a
// phrase at this chain row (songcore/traversal.h). Both clamp. `chainRow` is not reset — the
// CHAIN→PHRASE gate already refuses an empty row.
bool InputDispatcher::song_relative_b_horizontal(int delta) {
    if (!s_.settings.navSongRelative) return false;
    const int requireRow = (s_.currentScreen == ScreenType::PHRASE) ? pointer_chain_row(s_) : -1;
    const int songRow    = pointer_song_row(s_);
    const int track      = songcore::next_song_cell_h(*s_.project, songRow, pointer_track(s_), delta,
                                                      requireRow);
    set_pointer_song_cell(s_, songRow, track);
    refresh_song_relative_refs(s_);
    return true;
}

// On CHAIN it walks the track column to the nearest filled song row, skipping gaps. On PHRASE it walks
// the chain's own filled rows and never leaves the chain. Off NAV = SONG it does nothing.
void InputDispatcher::song_relative_b_vertical(int delta) {
    if (!s_.settings.navSongRelative) return;

    if (s_.currentScreen == ScreenType::CHAIN) {
        const int track = pointer_track(s_);
        const int row   = songcore::next_song_cell_v(*s_.project, pointer_song_row(s_), track, delta);
        set_pointer_song_cell(s_, row, track);
    } else {
        const int row = songcore::next_chain_row(*s_.project, s_.currentChain, pointer_chain_row(s_),
                                                 delta, /*wrap=*/false);
        set_pointer_chain_row(s_, row);
    }
    refresh_song_relative_refs(s_);
}

// ─── LIVE mode ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ In LIVE mode START QUEUES the cursor's cell rather than toggling the transport. Stopping is
// R+START (one channel) or STOP (everything).
void InputDispatcher::live_launch_cell() {
    const int track = s_.cursorColumn - 1;   // on SONG the column IS the track, 1-based
    if (!host_.is_playing()) {
        // Nothing running: this channel starts NOW, with the transport; the other seven begin silent.
        host_.play_song_live(s_.cursorRow, 1 << track);
        return;
    }
    // ⭐ The two-press launch: the first press queues for the end of the playing chain, a second on the
    // SAME cell promotes it to the next phrase boundary — the slot says which press this is.
    // ⚠️ Only while still ARMED: promoting a committed launch would cut short a chain still being heard.
    const songcore::LiveSlot q = host_.live_queue(track);
    const bool               immediate = q.armed() && !q.stop && q.targetRow == s_.cursorRow;
    host_.queue_live(track, s_.cursorRow, immediate);
}

void InputDispatcher::live_launch_row() {
    if (!host_.is_playing()) {
        // From a standing start the row launches together on one downbeat; an empty cell starts silent.
        host_.play_song_live(s_.cursorRow, live_row_mask(s_.cursorRow));
        return;
    }
    host_.queue_live_row(s_.cursorRow, live_row_armed(s_.cursorRow));
}

void InputDispatcher::live_stop_track() {
    if (!host_.is_playing()) return;   // nothing sounding, nothing to queue a stop for
    const int track = s_.cursorColumn - 1;
    // The launch's two-press promotion: chain end, then next phrase boundary.
    const songcore::LiveSlot sq = host_.live_queue(track);
    host_.queue_live_stop(track, sq.armed() && sq.stop);
}

int InputDispatcher::live_row_mask(int songRow) const {
    int mask = 0;
    for (int t = 0; t < 8; ++t) {
        // chainRefs is a growing list: a row past a track's end is empty.
        const std::vector<int>& refs = s_.project->tracks[static_cast<size_t>(t)].chainRefs;
        const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                ? refs[static_cast<size_t>(songRow)] : -1;
        if (chainId >= 0 && chainId < 256) mask |= 1 << t;
    }
    return mask;
}

bool InputDispatcher::live_row_armed(int songRow) const {
    for (int t = 0; t < 8; ++t) {
        const songcore::LiveSlot q = host_.live_queue(t);
        if (!q.stop && q.targetRow == songRow) return true;
    }
    return false;
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::song_context() const {
    const Project& p = *s_.project;
    SongEditorState ss{p};
    ss.cursorRow   = s_.cursorRow;
    ss.cursorTrack = s_.cursorColumn;  // on SONG the column IS the track
    return song_.cursor_context(ss);
}

bool InputDispatcher::song_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    const SongInputResult r = song_.handle_input(p, s_.cursorRow, s_.cursorColumn, action);
    if (r.hasChain) s_.lastEditedChain = r.lastEditedChain;
    return r.modified;
}

CursorContext InputDispatcher::chain_context() const {
    const Project& p = *s_.project;
    ChainEditorState cs{p.chains[static_cast<size_t>(s_.currentChain)]};
    cs.cursorRow    = s_.cursorRow;
    cs.cursorColumn = s_.cursorColumn;
    return chain_.cursor_context(cs);
}

bool InputDispatcher::chain_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    const ChainInputResult r = chain_.handle_input(
        p.chains[static_cast<size_t>(s_.currentChain)], s_.cursorRow, s_.cursorColumn, action);
    if (r.hasPhrase)    s_.lastEditedPhrase    = r.lastEditedPhrase;
    if (r.hasTranspose) s_.lastEditedTranspose = r.lastEditedTranspose;
    return r.modified;
}

CursorContext InputDispatcher::phrase_context() const {
    const Project& p = *s_.project;
    PhraseEditorState ps{p.phrases[static_cast<size_t>(s_.currentPhrase)]};
    ps.cursorRow        = s_.cursorRow;
    ps.cursorColumn     = s_.cursorColumn;
    ps.effectTypeCount  = visible_effect_type_count();
    // ⚠️ The CURSOR needs the project too: the NOTE cell's scale comes from it, and null reads as
    // chromatic with no error. `layout.cpp` sets its own, separate PhraseEditorState.
    ps.project          = &p;
    ps.insertInstrument = s_.lastEditedInstrument;
    return phrase_.cursor_context(ps);
}

bool InputDispatcher::phrase_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
    const PhraseInputResult r = phrase_.handle_input(ph, s_.cursorRow, s_.cursorColumn, action);
    if (!r.modified) return false;

    // "Last edited" and the audition: only a step WITH a note is remembered, and only a NOTE
    // edit auditions — dialling a velocity must not retrigger the voice.
    const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
    if ((r.hasNote || r.hasVolume || r.hasInstrument) && step.note != Note::EMPTY()) {
        s_.lastEditedNote       = step.note;
        s_.lastEditedVolume     = step.volume;
        s_.lastEditedInstrument = step.instrument;
        if (r.hasNote) preview_held_note();
    }
    // A+B under a held audition: the note is gone, so is the sound.
    if (heldNotePreview_ && step.note == Note::EMPTY()) {
        heldNotePreview_ = false;
        host_.stop_preview(/*cut=*/true);
    }
    return true;
}

CursorContext InputDispatcher::table_context() const {
    const Project& p = *s_.project;
    TableState ts{p.tables[static_cast<size_t>(s_.currentTable)]};
    ts.cursorRow       = s_.tableCursorRow;
    ts.cursorColumn    = s_.tableCursorColumn;
    ts.effectTypeCount = visible_effect_type_count();
    return table_.cursor_context(ts);
}

bool InputDispatcher::table_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    return table_
        .handle_input(p.tables[static_cast<size_t>(s_.currentTable)], s_.tableCursorRow,
                      s_.tableCursorColumn, action)
        .modified;
}

}  // namespace pt::ui
