package com.conanizer.pockettracker.trace

import com.conanizer.pockettracker.core.data.InstrumentType
import com.conanizer.pockettracker.core.data.ModDest
import com.conanizer.pockettracker.core.data.ModType
import com.conanizer.pockettracker.core.data.Note
import com.conanizer.pockettracker.core.data.Phrase
import com.conanizer.pockettracker.core.data.Project
import com.conanizer.pockettracker.core.logic.EffectProcessor as FX

/**
 * The golden project suite — synthetic, random-free (SC-1: no CHA/RND/RNL/ARP-RANDOM), each
 * project aimed at a cluster of ledger behaviors from zone-c-behavior-sweep.md. Built in code,
 * serialized to .ptp files in /testdata through FileController's exact Json config, and traced via
 * the EventTrace tap in render + live modes at 44100 and 48000 Hz.
 *
 * These projects are THE conformance corpus: C++ songcore (Phase 1 S4) loads the .ptp files and
 * must reproduce every trace byte-for-byte after canonical sort. Editing a project here without
 * regenerating goldens fails GoldenTraceTest.
 */
object GoldenProjects {

    /** One live-mode trace to record for a project. */
    data class LiveMode(
        val kind: String,        // SONG | CHAIN | PHRASE
        val arg: Int,            // start row / chain id / phrase id
        val horizonPhrases: Int  // drive the fake clock this many phrase-lengths, then stop()
    )

    /**
     * [name] is BOTH the `.ptp` filename and the project's stored `name` field, and they must stay
     * equal. Loading a project through the file browser renames it to the file's basename
     * (`AppInputDispatcher`: `result.project.name = item.file.nameWithoutExtension.take(20)`), so a
     * golden whose stored name differs from its filename comes back from a device load as *different
     * bytes* — and the trace header's `project=` sha, taken over the whole serialized project, then
     * matches no golden and the device cross-check refuses to compare anything. The events are
     * unaffected (nothing reads `name`), which makes it a maddening failure: byte-perfect events,
     * rejected on identity. `goldenNameMatchesFilename` below enforces the invariant.
     */
    data class Spec(
        val name: String,
        val renderRows: IntRange,
        val liveModes: List<LiveMode>,
        val build: () -> Project
    )

    val all: List<Spec> = listOf(
        Spec("g1-basics", 0..0, listOf(LiveMode("SONG", 0, 8), LiveMode("PHRASE", 0, 4), LiveMode("CHAIN", 0, 6)), ::g1Basics),
        Spec("g2-timing", 0..0, listOf(LiveMode("SONG", 0, 8), LiveMode("PHRASE", 0, 4)), ::g2Timing),
        Spec("g3-retrig", 0..0, listOf(LiveMode("SONG", 0, 8), LiveMode("PHRASE", 0, 4)), ::g3Retrig),
        Spec("g4-pitch", 0..0, listOf(LiveMode("PHRASE", 0, 4)), ::g4Pitch),
        Spec("g5-structure", 0..2, listOf(LiveMode("SONG", 0, 12)), ::g5Structure),
        Spec("g6-params", 0..0, listOf(LiveMode("PHRASE", 0, 4)), ::g6Params),
        Spec("g7-audio", 0..0, listOf(LiveMode("SONG", 0, 4)), ::g7Audio),
    )

    /**
     * g8 is deliberately NOT in [all] — it is the only golden project that is *random*, so it can
     * never own a byte-exact trace, and generating one for it would produce a file that fails on its
     * own second run. It is driven instead by S7RandomGoldenTest / tools/ptrandom, which run it a few
     * hundred times and compare DISTRIBUTIONS. Its `.ptp` is still written to /testdata (both engines
     * must load the same bytes), and ptroundtrip still round-trips it — the FX codes CHA/RND/RNL had
     * never appeared in a serialized golden before.
     */
    val random: Spec = Spec("g8-random", 0..0, emptyList(), ::g8Random)

    // ── Builder helpers ──────────────────────────────────────────────────────────────────────

    private fun Phrase.step(
        row: Int, note: String? = null, inst: Int = 0, vel: Int = 0x7F,
        fx1: Pair<Int, Int>? = null, fx2: Pair<Int, Int>? = null, fx3: Pair<Int, Int>? = null
    ) {
        val s = steps[row]
        if (note != null) {
            s.note = Note.fromString(note)
            s.instrument = inst
            s.volume = vel
        }
        fx1?.let { s.fx1Type = it.first; s.fx1Value = it.second }
        fx2?.let { s.fx2Type = it.first; s.fx2Value = it.second }
        fx3?.let { s.fx3Type = it.first; s.fx3Value = it.second }
    }

    private fun Project.chainRow(chainId: Int, row: Int, phraseId: Int, transpose: Int = 0) {
        chains[chainId].phraseRefs[row] = phraseId
        chains[chainId].transposeValues[row] = transpose and 0xFF
    }

    /** Instruments 0/1 sampler with distinct vol/pan, 2 SoundFont, 3 empty slot. */
    private fun Project.standardInstruments() {
        instruments[0].apply { volume = 0xFF; pan = 0x80; sampleFilePath = "golden/kick.wav" }
        instruments[1].apply { volume = 0xC0; pan = 0x40; sampleFilePath = "golden/pad.wav" }
        instruments[2].apply {
            instrumentType = InstrumentType.SOUNDFONT
            soundfontPath = "golden/test.sf2"; sfBank = 0; sfPreset = 5; volume = 0xE0
        }
        // instrument 3 stays empty (sampleFilePath == null): NoteOns on it are still events —
        // the tap sits above the validity checks; consumers drop them (frozen 2026-07-10).
    }

    // ── g1: notes, velocity curve, Vxx, KIL(+offset), PAN-with-note, muted-track divergence ──
    // Live SONG schedules the muted track 1, render skips it (IB-10) — the two goldens differ
    // by exactly those events, on purpose.
    private fun g1Basics(): Project {
        val p = Project(version = 1, name = "g1-basics", tempo = 128)
        p.standardInstruments()
        p.phrases[0].apply {
            step(0, "C-4", inst = 0, vel = 0x7F)
            step(2, "E-4", inst = 0, vel = 0x40)
            step(4, "G-4", inst = 1, vel = 0x20, fx1 = FX.FX_PAN to 0x20)          // PAN baked into trigger
            step(6, "C-5", inst = 0, vel = 0x00)                                    // velocity 0 edge
            step(8, null, fx1 = FX.FX_KILL to 0x00)                                 // KIL at row
            step(10, null, fx1 = FX.FX_VOLUME to 0x80)                              // Vxx on empty step → CC 7
            step(11, "D-4", inst = 0, vel = 0x60, fx1 = FX.FX_VOLUME to 0x30)       // Vxx with note → volGain
            step(12, null, fx1 = FX.FX_KILL to 0x06)                                // KIL +6 ticks offset
            step(14, "A-4", inst = 2, vel = 0x50)                                   // SoundFont note
            step(15, "C-3", inst = 3, vel = 0x7F)                                   // empty-instrument note
        }
        p.phrases[1].apply {
            step(0, "C-2", inst = 1, vel = 0x70)
            step(8, "G-2", inst = 1, vel = 0x35)
        }
        p.chainRow(0, 0, 0)
        p.chainRow(0, 1, 0, transpose = 0x0C)   // +12 semitones
        p.chainRow(1, 0, 1)
        p.tracks[0].chainRefs.add(0)
        p.tracks[1].chainRefs.add(1)
        p.tracks[1].mute = true
        return p
    }

    // ── g2: grooves (swing, 0-tic swallow IB-3, groove-00-is-real IB-13, GRV last-wins FIX-2,
    //        cross-phrase carry IB-4), LAT, LAT+KIL, odd tempo 137 (floor arithmetic) ──
    private fun g2Timing(): Project {
        val p = Project(version = 1, name = "g2-timing", tempo = 137)
        p.standardInstruments()
        p.grooves[1].steps.fill(-1); p.grooves[1].steps[0] = 8; p.grooves[1].steps[1] = 4
        p.grooves[2].steps.fill(-1)
        p.grooves[2].steps[0] = 6; p.grooves[2].steps[1] = 6; p.grooves[2].steps[2] = 0; p.grooves[2].steps[3] = 12
        // Groove 00 is a real, editable groove used by default (IB-13) — edit it on purpose.
        p.grooves[0].steps.fill(-1)
        p.grooves[0].steps[0] = 12; p.grooves[0].steps[1] = 12; p.grooves[0].steps[2] = 11; p.grooves[0].steps[3] = 13
        p.phrases[0].apply {
            step(0, "C-4", inst = 0, fx1 = FX.FX_GRV to 0x01)                       // swing from this step
            step(2, "E-4", inst = 0)
            step(4, "G-4", inst = 0, fx1 = FX.FX_GRV to 0x01, fx3 = FX.FX_GRV to 0x02) // FIX-2: last GRV wins
            step(5, "A-4", inst = 0)                                                // rides groove 02
            step(6, "B-4", inst = 0)                                                // 0-tic slot swallows a row near here
            step(7, "C-5", inst = 0)
            step(10, "D-4", inst = 1, fx1 = FX.FX_LAT to 0x06)                      // LAT: half-step late
            step(12, "E-4", inst = 1, fx1 = FX.FX_LAT to 0x03, fx2 = FX.FX_KILL to 0x02) // LAT shifts row incl. its KIL
        }
        p.phrases[1].apply {
            step(0, "C-3", inst = 0)   // groove phase carried across the phrase boundary (IB-4)
            step(4, "G-3", inst = 0, fx1 = FX.FX_GRV to 0x00)  // back to (edited) groove 00
            step(8, "C-4", inst = 0)
        }
        p.chainRow(0, 0, 0)
        p.chainRow(0, 1, 1)
        p.tracks[0].chainRefs.add(0)
        return p
    }

    // ── g3: RPT grids (phase-continuous, ramps, persistence, R00 cancel, LAT+RPT FIX-3 skip),
    //        ARP (UP/DOWN/PINGPONG via ARC, persistence), SF retrig velocity=-1 (IB-19) ──
    private fun g3Retrig(): Project {
        val p = Project(version = 1, name = "g3-retrig", tempo = 120)
        p.standardInstruments()
        p.phrases[0].apply {
            step(0, "C-4", inst = 0, fx1 = FX.FX_REPEAT to 0x03)   // R03: every 3 ticks, persistent
            step(4, "E-4", inst = 0, fx1 = FX.FX_REPEAT to 0x23)   // R23: every 3 ticks, ramp down 2
            step(8, null, fx1 = FX.FX_REPEAT to 0x00)              // R00 in the active column: cancel
            step(10, "G-4", inst = 0, fx1 = FX.FX_LAT to 0x06, fx2 = FX.FX_REPEAT to 0x06) // FIX-3
            step(12, "C-5", inst = 2, fx1 = FX.FX_REPEAT to 0x04)  // SF retrigs: velocity=-1 legacy path
        }
        p.phrases[1].apply {
            step(0, "C-4", inst = 0, fx1 = FX.FX_ARPEGGIO to 0x37) // ARP UP (default config), +3/+7
            step(4, null, fx1 = FX.FX_ARC to 0x24)                 // config: PINGPONG, speed 4
            step(5, "E-4", inst = 0, fx2 = FX.FX_ARPEGGIO to 0x47)
            step(9, null, fx1 = FX.FX_ARC to 0x13)                 // config: DOWN, speed 3
            step(10, "G-3", inst = 0, fx2 = FX.FX_ARPEGGIO to 0x25)
            step(14, null, fx2 = FX.FX_ARPEGGIO to 0x00)           // ARP00 cancel
        }
        p.chainRow(0, 0, 0)
        p.chainRow(0, 1, 1)
        p.tracks[0].chainRefs.add(0)
        return p
    }

    // ── g4: PSL (incl. no-previous-note FIX-1 case), PBN with-note + empty-step + stop,
    //        PVB/PVX incl. same-step last-wins (IB-21), PIT ±, OFF, SLI, transposes ──
    private fun g4Pitch(): Project {
        val p = Project(version = 1, name = "g4-pitch", tempo = 128)
        p.standardInstruments()
        p.transpose = 0x0C  // project transpose +12 — rides the NoteOn transpose field
        p.instruments[0].apply { slicingMode = 1; sliceMarkers = listOf(1000L, 9000L, 20000L) }
        p.phrases[0].apply {
            step(0, "C-4", inst = 1, fx1 = FX.FX_PSL to 0x08)      // PSL with no previous note: no slide
            step(2, "G-4", inst = 1, fx1 = FX.FX_PSL to 0x0C)      // slides from C-4
            step(4, "C-4", inst = 1, fx1 = FX.FX_PBN to 0x20)      // bend up, with note
            step(6, null, fx1 = FX.FX_PBN to 0x90)                 // bend down, mid-note → ExtPitchRate
            step(7, null, fx1 = FX.FX_PBN to 0x00)                 // stop bend
            step(8, "E-4", inst = 1, fx1 = FX.FX_PVB to 0x24)      // vibrato with note
            step(10, null, fx1 = FX.FX_PVB to 0x33, fx2 = FX.FX_PVX to 0x22) // both: PVX wins by order (IB-21)
            step(12, "C-4", inst = 0, fx1 = FX.FX_PIT to 0x03, fx2 = FX.FX_OFFSET to 0x40)
            step(13, "C-4", inst = 0, fx1 = FX.FX_PIT to 0xF8)     // PIT -8
            step(14, "C-4", inst = 0, fx1 = FX.FX_SLI to 0x02)     // explicit slice
        }
        p.chainRow(0, 0, 0, transpose = 0xF4)  // chain transpose -12 (cancels project +12)
        p.chainRow(0, 1, 0, transpose = 0x07)
        p.tracks[0].chainRefs.add(0)
        return p
    }

    // ── g5: SONG row-lock + padding (IB-5), HOP row-jump + HOP FF (IB-6), empty song row (IB-7),
    //        gapped chain rows, THO with/without note, TBL override, TIC-in-phrase no-op (IB-15) ──
    private fun g5Structure(): Project {
        val p = Project(version = 1, name = "g5-structure", tempo = 140)
        p.standardInstruments()
        p.tables[3].rows[0].transpose = 0x0C
        p.phrases[0].apply {   // long phrase, notes to the end
            for (r in 0..15 step 2) step(r, "C-4", inst = 0, vel = 0x70 - r)
        }
        p.phrases[1].apply {   // short phrase: HOP 04 at row 11 → next phrase starts at row 4
            step(0, "C-3", inst = 1)
            step(8, "G-3", inst = 1)
            step(11, null, fx1 = FX.FX_HOP to 0x04)
        }
        p.phrases[2].apply {   // HOP FF: track stops until the next song row (IB-6)
            step(0, "E-3", inst = 1)
            step(4, null, fx1 = FX.FX_HOP to 0xFF)
        }
        p.phrases[3].apply {   // tables: TBL override + THO with note + THO empty + TIC no-op
            step(0, "C-4", inst = 0, fx1 = FX.FX_TBL to 0x03)
            step(4, "E-4", inst = 0, fx1 = FX.FX_THO to 0x05)      // rides the NoteOn tableRow field
            step(6, null, fx1 = FX.FX_THO to 0x02)                 // ExtTableRow on the live voice
            step(8, "G-4", inst = 0, fx1 = FX.FX_TIC to 0x03)      // silent no-op in a phrase (IB-15)
        }
        p.chainRow(0, 0, 0); p.chainRow(0, 1, 0)
        p.chainRow(1, 0, 1); p.chainRow(1, 1, 1)                   // HOP'd short rows pad in SONG mode
        p.chainRow(2, 0, 2); p.chainRow(2, 2, 0)                   // gap at row 1 (enshrines gap semantics)
        p.chainRow(3, 0, 3); p.chainRow(3, 1, 3)                   // the table phrase, on its own track
        // Song rows: 0 = four tracks; 1 = empty (zero time, IB-7); 2 = track 0 only.
        p.tracks[0].chainRefs.addAll(listOf(0, -1, 0))
        p.tracks[1].chainRefs.addAll(listOf(1, -1, -1))
        p.tracks[2].chainRefs.addAll(listOf(2, -1, -1))
        p.tracks[3].chainRefs.addAll(listOf(3, -1, -1))
        return p
    }

    // ── g6: the param-queue FX on note steps (frame+1 rule, IB-12) and empty steps; BCK restart
    //        semantics; EQN/EQM; KIL 00 + note on one row (same-frame drain-order case) ──
    private fun g6Params(): Project {
        val p = Project(version = 1, name = "g6-params", tempo = 128)
        p.standardInstruments()
        p.instruments[0].apply { reverbSend = 0x30; delaySend = 0x20; eqSlot = 2 }
        p.eqPresets[5].bands[0].apply { type = 3; freq = 0x90; gain = 180; q = 0x70 }
        p.phrases[0].apply {
            step(0, "C-4", inst = 0, fx1 = FX.FX_RSEND to 0x80, fx2 = FX.FX_DSEND to 0x40) // frame+1
            step(2, null, fx1 = FX.FX_RSEND to 0x10)                // empty step: at frame
            step(3, null, fx2 = FX.FX_DSEND to 0xFF)
            step(4, "E-4", inst = 0, fx1 = FX.FX_BCK to 0x00)       // reverse, restart=1 (with note)
            step(6, null, fx1 = FX.FX_BCK to 0x01)                  // forward, restart=0 (scratch)
            step(8, "G-4", inst = 0, fx1 = FX.FX_EQN to 0x05)       // per-note EQ, frame+1
            step(10, null, fx1 = FX.FX_EQM to 0x05)                 // master EQ → track 255 event
            step(11, null, fx1 = FX.FX_PAN to 0xC0)                 // PAN empty step → CC 10
            step(12, "C-5", inst = 2, vel = 0x48, fx1 = FX.FX_KILL to 0x00) // KIL 00 + note, same frame
            step(14, null, fx1 = FX.FX_VOLUME to 0x22)              // Vxx empty → CC 7
        }
        p.chainRow(0, 0, 0)
        p.tracks[0].chainRefs.add(0)
        return p
    }

    // ── g7: the AUDIO golden — the only one that is judged on its SOUND ──────────────────────
    //
    // g1..g6 are judged on their EVENTS, so they never needed to make a noise (their sample paths
    // point at files that did not exist). tools/ptrender renders THIS one through the real engine,
    // so it is built around the DSP instead of the sequencer: both send buses (reverb and delay —
    // the two whose state used to leak from one render into the next), the master bus (OTT, limiter,
    // master EQ), a per-instrument EQ, a drive, a resonant filter under an LFO, a SoundFont voice,
    // and a stereo sample at HALF the render rate so the resampler runs. testdata/golden/ holds the
    // media (see make-golden-media.cpp — every source decays to silence, and nothing loops).
    //
    // The last step of the pad phrase carries a note ON PURPOSE: it is still ringing — sample,
    // reverb tail and delay repeats — when the song's last step ends. A render that stops at the
    // scheduler's span (as every render did before S6b) cuts that note dead at full amplitude, so
    // this project makes the truncation bug visible rather than theoretical.
    //
    // TWO HARD CONSTRAINTS, both because ptrender asserts that two renders of this project are
    // BYTE-IDENTICAL. Anything with a wall-clock seed in it breaks that:
    //   • no CHA / RND / RNL / random ARP — SC-1, and the C++ scheduler's rng_* are still stubs;
    //   • no oscShape 8 or 9 (RND / DRNK). Those LFO shapes draw from the engine's noteSeedEntropy,
    //     which is deliberately re-seeded from the clock at every render start, so a project using
    //     them renders differently every time BY DESIGN.
    // The master FX is OTT (masterBusFx = 0), not DUST, for the same reason: DUST's random-walk
    // drift is not something ptrender can make a determinism claim about.
    private fun g7Audio(): Project {
        val p = Project(version = 1, name = "g7-audio", tempo = 120)
        p.standardInstruments()

        // The two send buses. Wet and feedback are pushed high enough that the tail is unmistakable
        // — but not so high that it takes the render anywhere near render.h's 30-second tail cap.
        p.reverbFeedback = 0x70; p.reverbDamp = 0x60; p.reverbWet = 0xA0
        p.delayTime = 0x30; p.delayFeedback = 0x60; p.delayWet = 0x90; p.delayReverbSend = 0x40

        // The master bus, and the master EQ the render must restore afterwards.
        p.masterBusFx = 0; p.ottDepth = 0x50; p.limiterPreGain = 0x20
        p.eqPresets[3].bands[0].apply { type = 1; freq = 0x30; gain = 150; q = 0x80 }  // low shelf +3dB
        p.eqPresets[3].bands[1].apply { type = 3; freq = 0xB0; gain = 100; q = 0x60 }  // bell −2dB
        p.masterEqSlot = 3
        p.eqPresets[6].bands[0].apply { type = 4; freq = 0xC0; gain = 145; q = 0x80 }  // the pad's own EQ

        p.instruments[0].apply {                       // kick: dry-ish, driven, with an amp envelope
            reverbSend = 0x20; drive = 0x40
            modSlots[0].apply {
                type = ModType.AHD; dest = ModDest.VOLUME; amount = 0xFF
                attack = 0x00; hold = 0x08; decay = 0x40
            }
        }
        p.instruments[1].apply {                       // pad: soaked in both buses — the tail's source
            reverbSend = 0x90; delaySend = 0x70; eqSlot = 6
            filterType = "lp"; filterCut = 0xA0; filterRes = 0x40
            modSlots[0].apply {                        // a slow filter sweep — the modulation engine
                type = ModType.LFO; dest = ModDest.FILTER_CUTOFF; amount = 0x60
                oscShape = 1; lfoTrigMode = 1; lfoFreq = 0x30   // SIN, RETRIG — never 8/9 (RND/DRNK)
            }
        }
        p.instruments[2].apply { reverbSend = 0x60; delaySend = 0x40 }   // the SoundFont voice

        p.phrases[0].apply {                           // kick
            step(0, "C-4", inst = 0, vel = 0x7F)
            step(4, "C-4", inst = 0, vel = 0x60)
            step(6, "C-4", inst = 0, vel = 0x40)
            step(8, "C-4", inst = 0, vel = 0x7F)
            step(12, "C-4", inst = 0, vel = 0x50)
            step(14, "C-4", inst = 0, vel = 0x35, fx1 = FX.FX_RSEND to 0xA0)  // a live send move
        }
        p.phrases[1].apply {                           // pad — stereo, resampled from 22050
            step(0, "C-3", inst = 1, vel = 0x70)
            step(8, "G-3", inst = 1, vel = 0x60, fx1 = FX.FX_PAN to 0x30)
            step(15, "E-3", inst = 1, vel = 0x7F)      // still ringing when the song ends → the tail
        }
        p.phrases[2].apply {                           // SoundFont
            step(2, "C-4", inst = 2, vel = 0x60)
            step(6, "E-4", inst = 2, vel = 0x50)
            step(10, "G-4", inst = 2, vel = 0x55)
            step(13, "C-5", inst = 2, vel = 0x45)
        }
        // Two chain rows each, so the song is 32 steps = 4.0 s at 120 BPM before the tail.
        p.chainRow(0, 0, 0); p.chainRow(0, 1, 0)
        p.chainRow(1, 0, 1); p.chainRow(1, 1, 1)
        p.chainRow(2, 0, 2); p.chainRow(2, 1, 2)
        p.tracks[0].chainRefs.add(0)
        p.tracks[1].chainRefs.add(1)
        p.tracks[2].chainRefs.add(2)
        return p
    }

    // ── g8: the RANDOM golden — the only one judged on its DISTRIBUTION ────────────────────────
    //
    // The four random FX (CHA, RND, RNL, ARP mode RANDOM) are the one corner of the sequencer no
    // golden can reach: kotlin.random.Random is seeded from the platform, so the KOTLIN engine does
    // not repeat itself either. SC-1 kept them out of g1..g7 for exactly that reason — which left
    // songcore free to ship a stub (rng_int() → 0) that always took the LOWEST value, for four
    // sessions, with every check green. This project is what finally aims something at them.
    //
    // Built so that every random draw arrives in the schema-v1 trace as an EXACT INTEGER, which is
    // what lets the JVM measure Kotlin and the C++ tool measure songcore with the same arithmetic:
    //
    //   • PIT is the carrier. resolveStepParams maps its byte to a signed semitone bijectively
    //     (`value < 0x80 ? value : value - 256` — no clamp, no rounding) and it rides the NoteOn's
    //     `pit=` field, so a randomized FX byte lands in the trace as a plain integer. No float to
    //     invert, unlike PAN or Vxx.
    //   • ONE FX per track, so no two random draws can interact (a CHA that clears an FX column
    //     would change what a RND in that column later recalls).
    //   • Every band is chosen to stay clear of every clamp. RNL's note offset is ±5 around C-4 and
    //     its instrument offset ±3 around 4: let either saturate at 0 or 127 and probability would
    //     pile up on the boundary value, making the "uniform" claim in the golden simply false.
    //   • ARP uses A37 (+3/+7) — three DISTINCT chord tones. Kotlin's `listOf(n0,n1,n2).random()`
    //     draws a SLOT, so a chord with colliding semitones would be uniform over slots but not over
    //     pitches, and the trace only shows pitches. Distinct semitones make the two the same claim.
    //
    // Chains are eight rows of the same phrase, so one render walks each phrase eight times: the
    // draws-per-render is a constant, which is what makes the golden's `n=` counts byte-comparable.
    private fun g8Random(): Project {
        val p = Project(version = 1, name = "g8-random", tempo = 120)
        p.standardInstruments()

        // Track 0 — RND: recall the column's previous FX with a random value in [x0, yF].
        // Row 0 establishes the column (RND/RNL/CHA are never themselves recorded as the "previous"
        // FX, so without it every RND here would find prevType == 0 and quietly do nothing).
        p.phrases[0].apply {
            step(0, "C-4", inst = 0, fx1 = FX.FX_PIT to 0x10)              // seed the column: PIT
            for (r in 2..14 step 2) step(r, "C-4", inst = 0, fx1 = FX.FX_RND to 0x37)
        }                                                                   // → pit uniform over 48..127

        // Track 1 — RNL on FX1: randomize the NOTE and the INSTRUMENT, not an FX value.
        p.phrases[1].apply {
            for (r in 0..14 step 2) step(r, "C-4", inst = 4, fx1 = FX.FX_RNL to 0x53)
        }                                                                   // → note 55..65, inst 1..7

        // Track 2 — CHA target 0: the gate on the note itself. The two ends are DETERMINISTIC and
        // assert exactly: probability 0 never fires, probability F always does (the roll is
        // nextInt(15) → 0..14, so 15 can never lose). Row 12 carries no CHA at all — the control that
        // proves an absent NoteOn means "gated", not "the harness lost the step".
        p.phrases[2].apply {
            step(0,  "C-4", inst = 0, fx1 = FX.FX_CHA to 0x00)             // p=0  → never
            step(2,  "C-4", inst = 0, fx1 = FX.FX_CHA to 0x40)             // p=4  → 4/15
            step(4,  "C-4", inst = 0, fx1 = FX.FX_CHA to 0x80)             // p=8  → 8/15
            step(6,  "C-4", inst = 0, fx1 = FX.FX_CHA to 0x80)             // p=8  again (more trials)
            step(8,  "C-4", inst = 0, fx1 = FX.FX_CHA to 0xC0)             // p=12 → 12/15
            step(10, "C-4", inst = 0, fx1 = FX.FX_CHA to 0xF0)             // p=15 → always
            step(12, "C-4", inst = 0)                                       // control: no CHA
        }

        // Track 3 — CHA target 2: the OTHER arm of the gate. A losing roll clears FX2 instead of the
        // note, so the note always fires and its `pit=` reads 5 when the gate passed and 0 when it did
        // not — the same Bernoulli, observed through an FX column rather than a missing event.
        p.phrases[3].apply {
            for (r in 0..14 step 2) step(r, "C-4", inst = 0,
                fx1 = FX.FX_CHA to 0x82, fx2 = FX.FX_PIT to 0x05)
        }

        // Track 4 — ARP mode RANDOM. ARC configures on its own row (a note on the same row as the ARP
        // is fine, but ARC-then-ARP is how g3 does it and keeps the two effects unambiguous). The
        // arpeggio is persistent, and rows 2..15 stay EMPTY on purpose: any note would clearArpeggio()
        // and the draws would stop after one step.
        p.phrases[4].apply {
            step(0, null, fx1 = FX.FX_ARC to 0x38)                          // mode 3 = RANDOM, speed 8
            step(1, "C-4", inst = 0, fx2 = FX.FX_ARPEGGIO to 0x37)          // +3 / +7 → arp ∈ {0,3,7}
        }

        // Track 5 — RNL on FX2: randomize the column to the LEFT, i.e. FX1's VALUE, keeping FX1's type.
        p.phrases[5].apply {
            for (r in 0..14 step 2) step(r, "C-4", inst = 0,
                fx1 = FX.FX_PIT to 0x00, fx2 = FX.FX_RNL to 0x24)
        }                                                                   // → pit uniform over 32..79

        for (t in 0..5) {
            for (row in 0..7) p.chainRow(t, row, t)   // eight walks of the phrase per render
            p.tracks[t].chainRefs.add(t)
        }
        return p
    }
}
