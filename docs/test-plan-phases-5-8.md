# Testing Plan — SF2 Module System Phases 5–8

Covers the four phases of the per-track SF2 audio architecture.  
Run sections in order — each builds on the previous.

---

## Prerequisites

- A loaded SF2 file (e.g. a General MIDI font with piano, strings, drums)
- At least two SF2 instruments on different tracks
- One sampler instrument on a separate track (for isolation tests)
- A phrase with 4–8 notes spanning at least one octave

---

## 1. Smoke Tests (run first)

| Test | Expected |
|------|----------|
| Load app, open SF2 instrument, preview a note (A button on ROOT row) | Sound plays, correct pitch |
| Play a phrase with an SF2 instrument | Audio, no crash |
| Play phrase, stop, play again | Clean restart, no stuck notes |
| Switch between SF2 and sampler instruments while phrase is playing | No crash, both sound correct |

---

## 2. Phase 5 — SF Voices in the Modulation Engine

SF voices now share the same modulation infrastructure as sampler voices.  
Test that modulation slots work on SF instruments.

### 2a. VOL Envelope on SF

| Test | Setup | Expected |
|------|-------|----------|
| AHD envelope shapes volume | MODS screen: slot 1 TYPE=AHD, DEST=VOL, ATK=40, HLD=80, DEC=C0, AMT=FF | Note fades in, holds, then decays — audible fade |
| ADSR sustain holds | MODS: slot 1 TYPE=ADSR, DEST=VOL, ATK=20, DEC=40, SUS=80, REL=C0 | Note fades in, decays to ~50%, holds while phrase step lasts, releases on step end |
| LFO tremolo | MODS: slot 1 TYPE=LFO, DEST=VOL, RATE=20, AMT=80 | Volume pulses rhythmically (tremolo effect) |
| No modulation = no change | All MODS slots cleared | Flat volume at instrument VOL level |

### 2b. PITCH Modulation on SF

| Test | Setup | Expected |
|------|-------|----------|
| LFO vibrato via MODS | slot 1 TYPE=LFO, DEST=PITCH, RATE=10, AMT=30 | Pitch wavers (vibrato) |
| PSL pitch slide effect | Phrase step: PSL effect with value | Pitch slides from start to target |
| PBN continuous bend | Phrase step: PBN effect | Continuous pitch drift over steps |
| PVB vibrato effect | Phrase step: PVB effect | Vibrato audible on SF voice |

### 2c. SF vs. Sampler Isolation

| Test | Setup | Expected |
|------|-------|----------|
| VOL LFO on SF track does not affect sampler track | Tracks 1=SF+LFO, Track 2=sampler, both playing | Only SF track trembles; sampler is flat |
| VOL envelope on sampler does not leak to SF | ADSR on sampler instrument | SF voice plays flat; sampler fades as configured |

---

## 3. Phase 6 — Per-Channel TSF Rendering

Each SF track now gets an independent render channel.  
Test that tracks are isolated and simultaneous playback works correctly.

### 3a. Track Isolation

| Test | Setup | Expected |
|------|-------|----------|
| Two SF tracks play simultaneously | Track 1: piano, Track 2: strings, both in same phrase | Both sounds audible simultaneously, no dropout |
| Track 1 volume = 0, Track 2 playing | Mixer: Track 1 vol to 00 | Track 2 audible, Track 1 silent |
| Mute Track 2 | Mixer: Track 2 vol to 00 | Track 1 audible, Track 2 silent |
| 4 SF tracks simultaneously | Tracks 1–4 all SF2, all in phrase | All 4 voices audible, no crash |

### 3b. Pan Independence

| Test | Setup | Expected |
|------|-------|----------|
| Track 1 pan=00 (full left), Track 2 pan=FF (full right) | Both SF tracks playing | Track 1 audio in left channel only, Track 2 in right only (use headphones) |
| Center pan = 80 | Single SF track, pan=80 | Equal L/R output |

### 3c. Voice Stealing

| Test | Setup | Expected |
|------|-------|----------|
| New note on same SF track steals previous | Rapid notes on single SF track | Each new note cuts previous cleanly, no click |
| Note on different track does not steal | Track 1 playing, trigger Track 2 | Both coexist |

---

## 4. Phase 7 — Per-Instrument Effects on SF Voices

Drive, bitcrush, downsample, and biquad filter now apply post-TSF-render, per instrument.

### 4a. Drive

| Test | Setup | Expected |
|------|-------|----------|
| Drive = 00 | Instrument DRIVE=00 | Clean output |
| Drive = 80 | Instrument DRIVE=80 | Audibly warmer / louder / slight clipping |
| Drive = FF | Instrument DRIVE=FF | Heavy saturation/clipping |
| Drive on SF does not affect sampler track | Track 1=SF+drive, Track 2=sampler | Only SF track sounds distorted |

### 4b. Bitcrusher

| Test | Setup | Expected |
|------|-------|----------|
| Crush = 0 | CRUSH=0 | No effect |
| Crush = 8 | CRUSH=8 | Noticeable bit-reduction grunge |
| Crush = F | CRUSH=F | Extreme lo-fi / 1-bit sound |

### 4c. Biquad Filter

| Test | Setup | Expected |
|------|-------|----------|
| Filter off | FILTER=off | Full spectrum |
| Low-pass, CUT=40 | FILTER=lp, CUT=40 | High frequencies removed; muffled sound |
| Low-pass, CUT=C0 | FILTER=lp, CUT=C0 | Most spectrum passes; slight roll-off |
| High-pass, CUT=80 | FILTER=hp, CUT=80 | Bass removed; thin/airy sound |
| Band-pass, CUT=80, RES=80 | FILTER=bp | Honky/mid-focused sound |
| High resonance | FILTER=lp, CUT=60, RES=E0 | Audible resonant peak/squeal near cutoff |
| Filter on SF does not affect sampler | Track 1=SF+filter, Track 2=sampler | Only SF track is filtered |

### 4d. Downsample

| Test | Setup | Expected |
|------|-------|----------|
| Downsample = 0 | DWNSMPL=0 | No effect |
| Downsample = 4 | DWNSMPL=4 | Audible sample-rate reduction (gritty) |
| Downsample = F | DWNSMPL=F | Extreme aliasing / lo-fi |

---

## 5. Phase 8 — SF2 Preset Parameter Editing

Envelope and filter overrides on the instrument screen (rows 9–11 for SF instruments).

### 5a. UI Navigation

| Test | Expected |
|------|----------|
| Open SF2 instrument screen | Rows 0–11 visible; rows 9–11 show ATK, DEC, SUS, REL, CUT, RES |
| Navigate down past PRESET NAME (row 8) | Cursor reaches ATK+DEC row (row 9) |
| Navigate down through rows 9→10→11 | Lands on SUS+REL then CUT+RES |
| Navigate up from row 9 | Returns to PRESET NAME (row 8) |
| Navigate down from row 11 | Wraps to row 0 (TYPE) |
| Move cursor left/right on row 9 | Switches between ATK (col 1) and DEC (col 3) |

### 5b. Default State

| Test | Expected |
|------|----------|
| Fresh SF2 instrument, view rows 9–11 | All six fields show "--" |
| Play note with all "--" | Sound matches SF2 preset's built-in envelope exactly |

### 5c. Envelope Override — Attack

| Test | Setup | Expected |
|------|-------|----------|
| Set ATK = 00 | A+UP/DOWN until 00 | Note attack is near-instant (~0.001s) |
| Set ATK = 80 | ATK = 80 | Noticeable fade-in (~0.3s) |
| Set ATK = FF | ATK = FF | Very slow fade-in (~10s) |
| Reset ATK to "--" | Cursor on ATK, press A+B | Shows "--"; playback uses SF2's built-in attack |

### 5d. Envelope Override — Decay & Sustain

| Test | Setup | Expected |
|------|-------|----------|
| Set DEC = 40, SUS = 80 | Row 9 col 3: DEC=40; row 10 col 1: SUS=80 | Audible decay to ~50% sustain level |
| Set SUS = 00 | SUS = 00 | Decays to silence (no sustain) |
| Set SUS = FF | SUS = FF | Sustain at full volume (no decay audible) |
| Set SUS = "--" | A+B on SUS | Reverts to SF2 preset sustain level |

### 5e. Envelope Override — Release

| Test | Setup | Expected |
|------|-------|----------|
| Set REL = 00 | REL = 00 | Note cuts off immediately when step ends |
| Set REL = 80 | REL = 80 | Note trails off over ~0.3s after step ends |
| Set REL = FF | REL = FF | Very long release (~10s tail) |

### 5f. Filter Override (Phase-7 Biquad, Post-Render)

| Test | Setup | Expected |
|------|-------|----------|
| CUT = "--" | Default | No Phase-7 filter applied; SF2 internal filter only |
| Set CUT = 40 | CUT = 40 | Low-pass filter audible on SF voice (high freqs roll off) |
| Set CUT = FF | CUT = FF | Filter at max; near-bypass |
| Set RES = C0 with CUT = 60 | Both set | Resonant peak audible near cutoff |
| Reset CUT to "--" | A+B on CUT | Filter bypassed again |
| RES = value while CUT = "--" | Set RES but leave CUT at "--" | Filter still bypassed (CUT is the gate) |

### 5g. Persistence

| Test | Setup | Expected |
|------|-------|----------|
| Set ATK=40, DEC=80, SUS=C0, REL=20, CUT=60, RES=40 | Edit all fields | Values display correctly |
| Save project, reload project | Save → load | All six override fields restored to same values |
| Re-load SF2 (press LOAD on same instrument) | Trigger LOAD | Overrides immediately re-applied to fresh TSF handle; sound matches before reload |
| Copy instrument to another slot (SAVE → LOAD .pti) | Save preset, load into another slot | sfOverrides copied; overrides applied on load |

### 5h. Multi-Track Override Isolation

| Test | Setup | Expected |
|------|-------|----------|
| Track 1 SF: ATK=FF (slow attack), Track 2 SF: ATK=00 (instant) | Both in phrase | Track 1 fades in slowly, Track 2 plays instantly |
| Track 1 SF: CUT=30 (dark), Track 2 SF: CUT="--" (default) | Both in phrase | Track 1 sounds muffled; Track 2 sounds like preset |

---

## 6. Regression — Sampler Unaffected

After all SF changes, verify the sampler instrument pipeline is intact.

| Test | Expected |
|------|----------|
| Sampler instrument: preview note | Plays correctly |
| Sampler phrase plays with drive + filter | Effects audible, correct |
| Sampler modulation (AHD VOL envelope) | Volume envelope works |
| Sampler + SF2 instrument in same song | Both play without interference |
| WAV export with SF + sampler tracks | Exported file contains both |

---

## 7. Edge Cases & Stress

| Test | Expected |
|------|----------|
| Rapid preset switching (navigate PRESET col fast) | No crash, last preset plays |
| All 8 tracks: SF2 + overrides | No crash, all voices render |
| Set all envelope fields to FF (max), play | Very slow attack/release, no crash |
| Set all envelope fields to 00 (min), play | Near-instant envelope, no click |
| Change CUT while note is playing | Next note picks up new filter value |
| Change ATK while note is playing | Current voice not affected; next note uses new value |
