#pragma once

// ONE HAND: A, B, L, R or SELECT held alone past the hold time and let go stays down until it is
// pressed again, so every chord can be played one button at a time. It sits between the input and the
// mapper, which never knows: a locked button is one whose release has not arrived yet.
//
// A lock outlives the chords that are repeated (any with the D-pad, MUTE/SOLO, audition) and lets go
// after one that is done in one go (delete, cut, paste, clone, selection, the browser's file chords) —
// on that chord's release, as a hand would. L+R lets go once it has nothing left to clear. L after L+B
// waits out the widen window, so B again widens the selection.

#include "ui/buttons.h"
#include "ui/selection.h"   // MULTI_TAP_WINDOW

#include <cstddef>
#include <cstdint>

namespace pt::ui {

class StickyButtons {
public:
    static constexpr uint64_t DEFAULT_HOLD_MS = 300;

    // Off gates only new locks; the caller lets go of the old ones with `release_all`.
    void set_mode(bool enabled, uint64_t holdMs) {
        enabled_ = enabled;
        holdMs_  = holdMs;
    }

    // Hands `emit` each event to pass on, with the locked buttons added to its modifiers; a lock's
    // release and an unlocking press go nowhere. `l_r_has_more()` is asked after an L+R made with a
    // lock: false lets the lock go.
    template <class Emit, class More>
    void feed(const ButtonEvent& e, uint64_t now, Emit&& emit, More&& l_r_has_more) {
        Slot& s = slot(e.button);

        if (e.action == ButtonAction::PRESSED) {
            // Something else went down: every button held meanwhile is part of a chord, not a lock.
            for (Slot& other : slots_)
                if (&other != &s) other.alone = false;
            if (s.locked) {
                s.unlocking = true;   // its release, below, is the one the mapper hears
                s.letGoAt   = 0;
                return;
            }
            // L waiting after L+B: B widens, A cuts — the chord goes on. Anything else lets L go
            // first, or a D-pad press would fill rows instead of moving.
            for (size_t i = 0; i < COUNT; ++i) {
                Slot& l = slots_[i];
                if (!l.letGoAt) continue;
                l.letGoAt = 0;
                if (e.button != Button::A && e.button != Button::B) let_go(i, emit);
            }
            s.downAt = now;
            s.alone  = lockable(e.button);
            const ButtonEvent out = with_locks(e);
            emit(out);
            for (size_t i = 0; i < COUNT; ++i) {
                Slot& l = slots_[i];
                if (!l.locked || l.unlocking) continue;
                const After a = after_chord(static_cast<Button>(i), e.button, out.mods);
                if (a > l.after) {
                    l.after   = a;
                    l.afterBy = e.button;
                }
            }
            return;
        }

        if (s.unlocking) {
            s.unlocking = s.locked = s.alone = false;
            s.after = After::KEEP;
            emit(with_locks(e));
            return;
        }
        if (enabled_ && s.alone && now - s.downAt >= holdMs_) {
            s.alone  = false;
            s.locked = true;
            // A partner that locks too is a bigger chord on the way (L, B, then A clones): keep both.
            for (Slot& l : slots_)
                if (l.after != After::KEEP && l.afterBy == e.button) l.after = After::KEEP;
            return;
        }
        s.alone = false;
        emit(with_locks(e));

        // The chord this button made is over: the locks it used up let go, after it.
        for (size_t i = 0; i < COUNT; ++i) {
            Slot& l = slots_[i];
            if (l.after == After::KEEP || l.afterBy != e.button) continue;
            const After a = l.after;
            l.after = After::KEEP;
            if (a == After::WIDEN) {
                // The window runs from B's press, as the selection's own does.
                const uint64_t until = s.downAt + static_cast<uint64_t>(MULTI_TAP_WINDOW);
                if (until > now) { l.letGoAt = until; continue; }
            }
            if (a != After::ASK_L_R || !l_r_has_more()) let_go(i, emit);
        }
    }

    // Every frame: L lets go once the widen window after L+B has passed with no B.
    template <class Emit>
    void tick(uint64_t now, Emit&& emit) {
        for (size_t i = 0; i < COUNT; ++i) {
            Slot& l = slots_[i];
            if (!l.letGoAt || now < l.letGoAt) continue;
            l.letGoAt = 0;
            let_go(i, emit);
        }
    }

    // The releases the locks owe, so turning the mode off leaves nothing held.
    template <class Emit>
    void release_all(Emit&& emit) {
        for (size_t i = 0; i < COUNT; ++i) {
            Slot& s = slots_[i];
            s.after   = After::KEEP;
            s.letGoAt = 0;
            if (!s.locked || s.unlocking) continue;   // an unlocking one is still down: its release comes
            s.locked = false;
            emit(with_locks({static_cast<Button>(i), ButtonAction::RELEASED, ButtonMods{}}));
        }
    }

    bool locked(Button b) const { return slots_[index(b)].locked; }

    // Held alone long enough that letting go now locks it — drawn so the hand knows when to let go.
    bool will_lock(Button b, uint64_t now) const {
        const Slot& s = slots_[index(b)];
        return enabled_ && s.alone && now - s.downAt >= holdMs_;
    }

    // An on-screen button: down while held or locked; up from the press that lets a lock go.
    bool drawn_down(Button b, bool held) const {
        const Slot& s = slots_[index(b)];
        return (held || s.locked) && !s.unlocking;
    }

    static bool lockable(Button b) {
        return b == Button::A || b == Button::B || b == Button::L_SHIFT || b == Button::R_SHIFT ||
               b == Button::SELECT;
    }

private:
    // What a press does to a lock it chords with. Ordered: a later press only raises it.
    enum class After { KEEP, ASK_L_R, WIDEN, LET_GO };

    // ⚠️ Mirrors the mapper's matrix (button_mapper.h): a new one-shot chord there needs its case here,
    // or its lock stays on and has to be pressed off.
    static After after_chord(Button lock, Button partner, const ButtonMods& m) {
        if (is_dpad(partner)) return After::KEEP;   // edits, scrolling, screens, fills: repeated
        switch (lock) {
            case Button::A:   // A+B deletes; A+START auditions, again and again
                return partner == Button::B ? After::LET_GO : After::KEEP;
            case Button::B:   // L+B+A clones; B then A is a plain A
                return partner == Button::A && m.l ? After::LET_GO : After::KEEP;
            case Button::L_SHIFT:   // selection, cut/paste, LIVE's queue
                if (partner == Button::R_SHIFT) return After::ASK_L_R;
                if (partner == Button::B) return After::WIDEN;
                return partner == Button::A || partner == Button::START ? After::LET_GO : After::KEEP;
            case Button::R_SHIFT:   // MUTE and SOLO stay; R+START is one go
                if (partner == Button::L_SHIFT) return After::ASK_L_R;
                return partner == Button::START ? After::LET_GO : After::KEEP;
            case Button::SELECT:    // the browser's rename, delete, new folder
                return partner == Button::A || partner == Button::B || partner == Button::R_SHIFT
                           ? After::LET_GO : After::KEEP;
            default:
                return After::KEEP;
        }
    }

    struct Slot {
        uint64_t downAt    = 0;
        bool     alone     = false;          // down with nothing pressed since
        bool     locked    = false;
        bool     unlocking = false;          // locked, and pressed again: up on its release
        After    after     = After::KEEP;    // what the chord in progress does to this lock…
        Button   afterBy   = Button::COUNT;  // …settled on this button's release
        uint64_t letGoAt   = 0;              // L after L+B: lets go then, unless B comes first
    };
    static constexpr size_t COUNT = static_cast<size_t>(Button::COUNT);
    Slot     slots_[COUNT];
    bool     enabled_ = false;
    uint64_t holdMs_  = DEFAULT_HOLD_MS;

    static size_t index(Button b) { return static_cast<size_t>(b); }
    Slot&         slot(Button b) { return slots_[index(b)]; }

    template <class Emit>
    void let_go(size_t i, Emit&& emit) {
        Slot& l = slots_[i];
        if (!l.locked || l.unlocking) return;   // an unlocking one is still down: its release comes
        l.locked = false;
        emit(with_locks({static_cast<Button>(i), ButtonAction::RELEASED, ButtonMods{}}));
    }

    ButtonEvent with_locks(ButtonEvent e) const {
        e.mods.a      = e.mods.a      || locked(Button::A);
        e.mods.b      = e.mods.b      || locked(Button::B);
        e.mods.l      = e.mods.l      || locked(Button::L_SHIFT);
        e.mods.r      = e.mods.r      || locked(Button::R_SHIFT);
        e.mods.select = e.mods.select || locked(Button::SELECT);
        return e;
    }
};

}  // namespace pt::ui
