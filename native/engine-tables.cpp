// The table walk: the TIC00 bookmarks, the INS chain a hit is routed through, a voice's per-block row
// tick and AUS/AUF ramps, and the playing-row readout the TABLE screen draws.
#include "audio-engine.h"
#include "engine-voice-ops.h"
#include "table_automation.h"  // AUS/AUF pairing over a table's rows — shared with the table editor
#include <cstdint>

// A TIC in the table's LAST ROW overrides the instrument's rate — per COLUMN, because each column
// has its own playhead. FX1 sets lane 0's rate, FX2 lane 1's, FX3 lane 2's, and a column with no TIC
// there keeps the instrument's.
//
// ⚠️ This is the one row where a TIC does not act when the playhead reaches it: it is read at TRIGGER,
// so the rate is in force from row 0. A TIC anywhere else takes effect when its lane arrives.
void AudioEngine::resetTic00Cursors() {
    for (int t = 0; t < SF_VOICE_COUNT; ++t) {
        for (int s = 0; s < TIC00_SLOTS; ++s) tic00Cursor[t][s] = Tic00Cursor();
        tic00Sounding[t] = -1;
    }
}

// ⚠️ A track runs out of slots only with a chain deeper than the cap plus a table per link, and then
// the OLDEST-numbered slot is reused rather than the lookup failing: one table forgetting its place
// costs a rotation restarting, where refusing the bookmark would cost the switch itself.
AudioEngine::Tic00Cursor* AudioEngine::tic00Slot(int trackId, int tableId, bool create) {
    if (trackId < 0 || trackId >= SF_VOICE_COUNT || tableId < 0) return nullptr;
    Tic00Cursor* row = tic00Cursor[trackId];
    for (int s = 0; s < TIC00_SLOTS; ++s)
        if (row[s].tableId == tableId) return &row[s];
    if (!create) return nullptr;
    for (int s = 0; s < TIC00_SLOTS; ++s) {
        if (row[s].tableId < 0) { row[s] = Tic00Cursor(); row[s].tableId = tableId; return &row[s]; }
    }
    row[0] = Tic00Cursor();
    row[0].tableId = tableId;
    return &row[0];
}

// Defined below, beside the playing-row readout.
static int findTrackVoice(Voice* voices, int trackId, bool fading);

// A table row's three FX slots, as (type, value) pairs — slot 1 is index 0.
static inline void rowFx(const TableRow& r, int type[3], int value[3]) {
    type[0] = r.fx1Type; value[0] = r.fx1Value;
    type[1] = r.fx2Type; value[1] = r.fx2Value;
    type[2] = r.fx3Type; value[2] = r.fx3Value;
}

// RND re-fires the nearest real command ABOVE it in its own column with 0..xx added — the phrase's
// rule, one row up instead of one step back. ⚠️ The base is always the TYPED cell, never an earlier
// roll, so repeated rolls spread around it instead of wandering off. False when there is nothing above.
static bool tableRandomize(const TableRow rows[16], int row, int slot, int range, uint32_t& rng,
                           int& type, int& value) {
    for (int up = 1; up < 16; ++up) {
        int t3[3], v3[3];
        rowFx(rows[(row - up + 16) % 16], t3, v3);
        const int t = t3[slot];
        if (t == 0 || t == FX_RND || t == FX_RNL || t == FX_CHA) continue;
        const int added = range > 0 ? static_cast<int>(xorshift32(rng) % (uint32_t)(range + 1)) : 0;
        type  = t;
        value = std::min(v3[slot] + added, tableFxCeiling(t));
        return true;
    }
    return false;
}

int AudioEngine::resolveChain(int trackId, int instrumentId, int tableIdOverride, int* outTableId,
                              TableCarry* carry) {
    int tableId = (tableIdOverride >= 0) ? tableIdOverride : instrumentId;
    int visited[CHAIN_MAX_LINKS];
    int visitedCount = 0;

    for (int link = 0; link < CHAIN_MAX_LINKS; ++link) {
        if (outTableId) *outTableId = tableId;

        // Silence beats a fallback: an empty slot and external gear both mean "this instrument
        // cannot answer", and a hit that quietly played something else is the bug that started this.
        // Link 0 is exempt from the empty test — the scheduler already made it, before any pushes.
        if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return -1;
        const songcore::Program& p = programs[instrumentId].program;
        if (p.type == songcore::PROGRAM_EXTERNAL) return -1;   // until the cable is a program too
        if (link > 0 && !p.hasSample && !p.hasSoundfont) return -1;

        // A table reached twice on one hit would loop forever; stop and sound where we stand.
        for (int i = 0; i < visitedCount; ++i) if (visited[i] == tableId) return instrumentId;
        if (tableId < 0 || tableId >= 256) return instrumentId;

        TableRow rows[16];
        if (!tables.read(tableId, rows)) return instrumentId;

        // A table with no INS anywhere in it cannot route, so it is left completely alone — no place
        // is kept for it and nothing below can touch how its voice plays it.
        bool routes = false;
        for (int r = 0; r < 16 && !routes; ++r) {
            int t3[3], v3[3];
            rowFx(rows[r], t3, v3);
            for (int s = 0; s < 3; ++s) routes |= (t3[s] == FX_INS);
        }
        if (!routes) return instrumentId;

        // ⚠️ **THE ROW IS THE ONE THE TABLE IS STANDING ON FOR THIS TRIGGER — THE SAME ROW THE VOICE
        // WOULD START AT, BY THE SAME RULE.** There is ONE place per table, not a separate one for
        // routing: at TIC00 it steps on a row per trigger (which is the rotation) and at every other
        // rate it starts at row 0 each note, exactly as a table with no INS in it does. So when no
        // switch is found, the voice below picks up this same row and that row's transpose, volume
        // and FX apply to the note — they are in the path, not part of the switch.
        // Row 15's TIC overrides the instrument's rate per column — `effectiveTicRatesFor`'s rule,
        // read off the copy already in hand rather than taking the table lock a second time.
        int rates[TABLE_LANES] = {p.tableTicRate, p.tableTicRate, p.tableTicRate};
        if (rows[15].fx1Type == FX_TIC) rates[0] = rows[15].fx1Value;
        if (rows[15].fx2Type == FX_TIC) rates[1] = rows[15].fx2Value;
        if (rows[15].fx3Type == FX_TIC) rates[2] = rows[15].fx3Value;

        const Tic00Cursor* cur = tic00Slot(trackId, tableId, /*create=*/false);
        const int liveVoice = findTrackVoice(voices, trackId, /*fading=*/false);

        int laneRow[TABLE_LANES] = {0, 0, 0};
        for (int l = 0; l < TABLE_LANES; ++l) {
            if (rates[l] != 0x00) continue;   // not a per-trigger column: it starts at the top
            if (liveVoice >= 0 && voices[liveVoice].tableId == tableId &&
                voices[liveVoice].lanes[l].ticRate == 0x00) {
                laneRow[l] = tic00RowAfter(voices[liveVoice].lanes[l]) & 0x0F;
            } else if (cur && cur->ticRate[l] == 0x00 && cur->active[l]) {
                laneRow[l] = tic00RowAfter(cur->row[l], cur->lastProcessed[l]) & 0x0F;
            }
        }

        // HOP steers the rotation, so a router follows it before it reads anything on the row —
        // per COLUMN, like everywhere else, to the row in its low nibble. `HOP FF` stops its column
        // and needs nothing here: the row it stands on carries no switch. The repeat count does not
        // carry between hits, because `reset_table_lanes` places a lane fresh at every trigger, so a
        // counted HOP jumps on every hit exactly as one in a TIC00 table does.
        for (int l = 0; l < TABLE_LANES; ++l) {
            uint16_t seen = 0;
            for (;;) {
                if (seen & (uint16_t)(1u << laneRow[l])) break;   // round again: stand where we are
                seen |= (uint16_t)(1u << laneRow[l]);
                int t3[3], v3[3];
                rowFx(rows[laneRow[l]], t3, v3);
                if (t3[l] != FX_HOP || v3[l] == 0xFF) break;
                laneRow[l] = v3[l] & 0x0F;
            }
        }

        // ⚠️ **EACH COLUMN IS READ AT ITS OWN ROW**, the rule the rest of the table already follows.
        // At TIC 00 — the mode this feature is for — all three stand on the same row anyway.
        int type[3], value[3], slotRow[3];
        for (int s = 0; s < 3; ++s) {
            int t3[3], v3[3];
            rowFx(rows[laneRow[s]], t3, v3);
            type[s] = t3[s]; value[s] = v3[s]; slotRow[s] = laneRow[s];
        }

        // CHA decides whether the switch fires at all: on a failed roll its low nibble clears that
        // slot, and a cleared INS is simply a row with no switch on it.
        for (int s = 0; s < 3; ++s) {
            if (type[s] != FX_CHA) continue;
            const int odds  = (value[s] >> 4) & 0x0F;
            const int target = value[s] & 0x0F;
            if (static_cast<int>(xorshift32(chainRngState) % 15u) < odds) continue;   // passed
            if (target >= 1 && target <= 3) type[target - 1] = 0;
        }

        // RND re-fires the command above it in its column — so an RND below an INS picks a nearby
        // instrument on every hit, and one below a CUT moves the cutoff the hit carries.
        for (int s = 0; s < 3; ++s) {
            if (type[s] != FX_RND) continue;
            if (!tableRandomize(rows, slotRow[s], s, value[s], chainRngState, type[s], value[s]))
                type[s] = 0;
        }

        // RNL adds a random 0..xx to the slot on its LEFT, so an RNL beside an INS IS the instrument
        // number — the pool-picker. Read wherever it sits: it configures the switch, it is not in
        // the path through the row.
        for (int s = 1; s < 3; ++s) {
            if (type[s] != FX_RNL || type[s - 1] != FX_INS) continue;
            const int range = value[s] & 0xFF;
            if (range > 0) value[s - 1] += static_cast<int>(xorshift32(chainRngState) % (uint32_t)(range + 1));
            if (value[s - 1] > 127) value[s - 1] = 127;   // INS's own ceiling
        }

        // ⚠️ **THE ROTATION MOVES ON EVERY HIT, SWITCH OR NO SWITCH**, and it is written before the
        // row is judged for exactly that reason: a row with no INS on it would otherwise freeze the
        // table on that row, and every later hit would read it again and never reach the rows past it.
        // ⚠️ **LEFTMOST WINS** — the signal leaves at the first switch it meets, so anything past it
        // on the row is not part of the path.
        int next = -1, switchSlot = -1;
        for (int s = 0; s < 3; ++s) {
            if (type[s] == FX_INS) { next = value[s] & 0x7F; switchSlot = s; break; }
        }
        // No switch here, so this instrument sounds and its voice runs this table from `laneRow` —
        // which is where the voice would have started anyway, so nothing has to be handed down.
        if (next < 0) return instrumentId;

        // ⚠️ **WHAT IS LEFT OF THE SWITCH IS IN THE PATH, SO IT TRAVELS WITH THE HIT.** No voice will
        // play this row, so its transpose, volume and FX have to be handed to the one that sounds.
        // The VOL column and a VOL cell are one write, the cell winning, as on a played row.
        if (carry) {
            const TableRow& r0 = rows[laneRow[0]];
            float vol = (r0.volume == 0xFF) ? 1.0f : r0.volume / 255.0f;
            for (int s = 0; s < switchSlot; ++s) {
                const int t = type[s];
                if (t == FX_VOLUME) { vol = value[s] / 255.0f; continue; }
                if (t == 0 || t == FX_HOP || t == FX_THO || t == FX_TIC || t == FX_KILL ||
                    t == FX_RNL || t == FX_CHA)
                    continue;
                if (carry->fxCount < TABLE_CARRY_FX_MAX) {
                    carry->fxType[carry->fxCount]  = static_cast<uint8_t>(t);
                    carry->fxValue[carry->fxCount] = static_cast<uint8_t>(value[s]);
                    carry->fxCount++;
                }
            }
            carry->semitones += static_cast<float>(transposeToSemitones(r0.transpose));
            carry->volume    *= vol;
        }

        // ⚠️ **THE HIT LEAVES, SO NOTHING ELSE WILL MOVE THIS TABLE ON.** No voice runs it, so the
        // step the voice would have made has to be made here or the next trigger reads the same row
        // for ever. Only a per-trigger column has a place to keep; the others start at the top anyway.
        for (int l = 0; l < TABLE_LANES; ++l) {
            if (rates[l] != 0x00) continue;
            Tic00Cursor* c = tic00Slot(trackId, tableId, /*create=*/true);
            if (!c) break;
            c->row[l]           = slotRow[l];
            c->lastProcessed[l] = slotRow[l];   // "this row has been used" — the next trigger steps on
            c->ticRate[l]       = 0x00;
            c->active[l]        = true;
        }
        if (visitedCount < CHAIN_MAX_LINKS) visited[visitedCount++] = tableId;

        instrumentId = next;
        tableId      = next;   // the next link brings its OWN table
    }

    // The depth cap was hit. Sound where we stand rather than following further.
    if (outTableId) *outTableId = tableId;
    if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return -1;
    const songcore::Program& last = programs[instrumentId].program;
    if (last.type == songcore::PROGRAM_EXTERNAL) return -1;
    if (!last.hasSample && !last.hasSoundfont) return -1;
    return instrumentId;
}

void AudioEngine::effectiveTicRatesFor(int tableId, int fallback, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = fallback;
    if (tableId < 0 || tableId >= 256) return;
    TableRow rows[16];
    if (!tables.read(tableId, rows)) return;
    const TableRow& lastRow = rows[15];
    if (lastRow.fx1Type == FX_TIC) out[0] = lastRow.fx1Value;
    if (lastRow.fx2Type == FX_TIC) out[1] = lastRow.fx2Value;
    if (lastRow.fx3Type == FX_TIC) out[2] = lastRow.fx3Value;
}

// Special TIC modes:
//   TIC00 (0x00): Trigger mode — table row set by note, doesn't advance automatically
//   TICFC (0xFC): Octave map — row = triggered note's octave (0-9)
//   TICFE (0xFE): Note map — row = triggered note's pitch (0-11)
//   TICFF (0xFF): 200Hz mode — one row every sampleRate / 200 frames

// Frames one row lasts on a lane that advances by itself; 0 for the three modes that hold the row.
// framesPerTic = sr / (BPM/60 · 4 steps/beat · 12 tics/step), so table speed tracks the sequencer.
static double framesPerTableRow(int ticRate, int tempo, float sampleRate) {
    if (ticRate == 0x00 || ticRate == 0xFC || ticRate == 0xFE) return 0.0;
    if (ticRate == 0xFF) return sampleRate / 200.0;
    return sampleRate / (tempo / 60.0 * 4.0 * 12.0) * ticRate;
}

// A lane's clock landing a hair under its row length after `ceil` is the same frame, not one more.
static constexpr double TIC_EPSILON = 1e-6;

template <typename V>
int AudioEngine::processTableTick(V& voice, int from, int maxFrames, float sampleRate) {
    // ⚠️ THE WHOLE TABLE, not just the current row, because the AUS/AUF pairing below is re-derived
    // from every row on every call — that is what lets a backwards HOP resume a ramp mid-span with
    // nothing stored per voice.
    TableRow rows[16];
    if (!tables.read(voice.tableId, rows)) return maxFrames;
    const int tempo = currentTempo.load(std::memory_order_relaxed);

    // ⚠️ **A BLANK TABLE IS NEVER CUT.** Every note runs its instrument's table, written or not, and a
    // blank row changes nothing — cutting the voice there would only re-derive it for the same values.
    bool blank = true;
    for (const TableRow& r : rows)
        blank = blank && r.transpose == 0 && r.volume == 0xFF && !r.fx1Type && !r.fx2Type && !r.fx3Type;

    // ⚠️ **THE RATE MACHINE RUNS ONCE PER LANE, AND NOTHING IN IT IS SHARED.** A lane at TICFF next
    // to a lane at TIC 06 is the point of the feature; a single accumulator would make the faster
    // one drag the slower.
    for (int lane = 0; lane < TABLE_LANES; ++lane) {
        TableLane& L = voice.lanes[lane];
        if (!L.active) continue;   // this column executed HOP FF; the others carry on

        bool shouldProcessRow = false;
        bool shouldAdvance = false;
        const double perRow = framesPerTableRow(L.ticRate, tempo, sampleRate);

        if (perRow <= 0.0) {
            // TIC00 / TICFC / TICFE: the row is placed by the note and applied once.
            shouldProcessRow = (L.row != L.lastProcessed);
        } else if (L.lastProcessed == -1) {
            // The first row plays AT the note, so row 0's transpose/vol/FX apply from its first
            // sample. The clock starts at the note's onset, which may lie later in this block.
            L.frameAccum = (voice.startDelayFrames > from) ? -(double)(voice.startDelayFrames - from) : 0.0;
            shouldProcessRow = shouldAdvance = true;
        } else if (L.frameAccum >= perRow - TIC_EPSILON) {
            L.frameAccum -= perRow;
            // Only reachable when the row got shorter under the clock (tempo, a TIC row) or on the
            // block-rate path: drop the backlog rather than bank it, or rows would run away.
            if (L.frameAccum >= perRow) L.frameAccum = 0.0;
            shouldProcessRow = shouldAdvance = true;
        }

        // ⚠️ A HOP or THO does not consume the tic — it moves the lane and the row it lands on plays
        // here, in this same tic. Bounded at one table's worth of rows so a HOP onto itself, or a
        // ring of them, cannot spin the audio thread; a ring with no playable row simply sounds
        // nothing, which is what a table of pure jumps deserves.
        for (int steered = 0; shouldProcessRow && steered <= 16; ++steered) {
            TableRow played = rows[L.row];
            uint8_t& fxT = (lane == 0) ? played.fx1Type  : (lane == 1) ? played.fx2Type  : played.fx3Type;
            uint8_t& fxV = (lane == 0) ? played.fx1Value : (lane == 1) ? played.fx2Value : played.fx3Value;
            if (fxT == FX_RND) {
                int t = 0, v = 0;
                if (tableRandomize(rows, L.row, lane, fxV, chainRngState, t, v)) {
                    fxT = static_cast<uint8_t>(t);
                    fxV = static_cast<uint8_t>(v);
                } else {
                    fxT = 0;
                }
            }
            if (!processTableRow(voice, played, lane, shouldAdvance, from, sampleRate)) break;
        }
        // ⚠️ A HOP FF in an EARLIER lane can have cleared tableId. Stop reading the table copy the
        // moment it does — the remaining lanes are already down.
        if (voice.tableId < 0) return maxFrames;
    }

    // ⚠️ **THE SEGMENT ENDS WHERE THE NEXT ROW OF ANY LANE BEGINS**, so the caller renders up to it and
    // calls again: a row starts on its own frame, not on a block edge. Measured in whole frames — the
    // fraction carries in the lane's clock, so the rate is exact over time.
    int frames = maxFrames;
    if (!blank) {
        for (int lane = 0; lane < TABLE_LANES; ++lane) {
            const TableLane& L = voice.lanes[lane];
            const double perRow = framesPerTableRow(L.ticRate, tempo, sampleRate);
            if (!L.active || perRow <= 0.0) continue;
            const double left = std::ceil(perRow - L.frameAccum - TIC_EPSILON);
            frames = std::min(frames, left < 1.0 ? 1 : (left < (double)maxFrames ? (int)left : maxFrames));
        }
    }

    // How far each lane is through the row in force, at the END of this segment — the ramp's target,
    // which the mix interpolates towards across it. 0 in the three modes that hold the row still.
    double rowFraction[TABLE_LANES] = {0.0, 0.0, 0.0};
    for (int lane = 0; lane < TABLE_LANES; ++lane) {
        TableLane& L = voice.lanes[lane];
        const double perRow = framesPerTableRow(L.ticRate, tempo, sampleRate);
        if (!L.active || perRow <= 0.0) continue;
        L.frameAccum += frames;
        rowFraction[lane] = std::max(0.0, std::min(1.0, L.frameAccum / perRow));
    }

    // ⚠️ THE RAMP IS EVALUATED AGAINST `lastProcessed`, NOT the lane's current row. The row whose
    // effects are in force is the one that was last consumed — the cursor has already been advanced
    // (or HOPped) to the one that comes NEXT, and reading it would run every fade a whole row ahead
    // of what is being heard. `frameAccum` is the progress through that same consumed row, so the
    // pair is consistent by construction.
    applyTableRamps(voice, rows, rowFraction, sampleRate);
    return frames;
}

// One lane consuming one row.
//
// ⚠️ **THE TRANSPOSE AND VOLUME COLUMNS BELONG TO LANE 0, and only lane 0 applies them.** They share
// FX1's playhead by definition — that is what "lane A" means — so running them from any other lane
// would rewrite the note's pitch at FX2's rate.
//
// ⚠️ And a lane reads **exactly one** FX slot: its own. `KIL VOL OFFSET CUT RES EQN EQM` are global
// effects any column may carry, but `HOP`, `TIC` and `THO` steer the lane they are written in.
//
// ⚠️⚠️ **A ROW THAT STEERS THE LANE IS NEVER HEARD.** `HOP` and `THO` have no tic of their own: the
// lane resolves them and the row they land on is the row that plays. Letting one play would put a
// tic of the table's DEFAULTS in front of every loop — no transpose, no filter, full volume — which
// is an untransposed grace note before the transposed one, and a burst of raw sample under a closed
// filter. The caller re-enters on `true` until a row actually plays.
template <typename V>
bool AudioEngine::processTableRow(V& voice, const TableRow& row, int lane, bool shouldAdvance,
                                  int atFrame, float sampleRate) {
    TableLane& L = voice.lanes[lane];

    const uint8_t laneFxType = (lane == 0) ? row.fx1Type : (lane == 1) ? row.fx2Type : row.fx3Type;
    const bool steers = (laneFxType == FX_HOP || laneFxType == FX_THO);

    if (lane == 0 && !steers) {
        // playbackRate does not include transpose; getModulatedPlaybackRate reads
        // modDestValues[PARAM_PITCH] which processRoutes accumulates from TABLE_PITCH.
        int semitones = transposeToSemitones(row.transpose);
        voice.tableTranspose = (float)semitones;  // kept for debug log
        voice.modSourceValues[MOD_SRC_TABLE_PITCH] = (float)semitones + voice.carrySemitones;

        // Mix loop reads modDestValues[PARAM_VOL] instead of voice.tableVolume.
        if (row.volume == 0xFF) {
            voice.tableVolume = 1.0f;  // kept for debug log
        } else {
            voice.tableVolume = row.volume / 255.0f;
        }
        voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume * voice.carryVolume;
    }

    bool hopExecuted = false;
    int hopTarget = -1;

    auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
        switch (fxType) {
            case FX_KILL:
                if (fxValue == 0x00) {
                    tableKill(voice, atFrame);
                    LOGT("📋 Table effect: KILL track %d", voice.getTrackId());
                }
                break;

            case FX_HOP:
                // HOP XY: X=repeat count (0=infinite), Y=target row; FF=stop THIS COLUMN
                if (fxValue == 0xFF) {
                    // ⚠️ **THE LANE, NOT THE TABLE.** The voice's table only ends once all three
                    // columns have stopped — a HOP FF typed in FX3 must not silence the note and
                    // volume columns, which is exactly the coupling per-column playback removes.
                    L.active = false;
                    L.hopTarget = -1;
                    L.hopRepeat = 0;
                    bool anyRunning = false;
                    for (int o = 0; o < TABLE_LANES; ++o) anyRunning |= voice.lanes[o].active;
                    if (!anyRunning) voice.tableId = -1;
                    LOGT("📋 Table HOP FF: stopped column %d for track %d%s", lane + 1,
                         voice.getTrackId(), anyRunning ? "" : " (table ended)");
                } else {
                    int repeatCount = (fxValue >> 4) & 0x0F;  // High nibble = X
                    int targetRow = fxValue & 0x0F;           // Low nibble = Y

                    if (repeatCount == 0) {
                        // HOP 0Y = Infinite loop to row Y
                        hopExecuted = true;
                        hopTarget = targetRow;
                        LOGT("📋 Table HOP %02X: infinite loop to row %d, track %d", fxValue, targetRow, voice.getTrackId());
                    } else {
                        // HOP XY (X>0) = Jump X times, then continue
                        if (L.hopTarget == -1 || L.hopTarget != targetRow) {
                            L.hopRepeat = repeatCount;
                            L.hopTarget = targetRow;
                            LOGT("📋 Table HOP %02X: initialized counter=%d, target=%d, track %d",
                                 fxValue, repeatCount, targetRow, voice.getTrackId());
                        }

                        if (L.hopRepeat > 0) {
                            L.hopRepeat--;
                            hopExecuted = true;
                            hopTarget = targetRow;
                            LOGT("📋 Table HOP: jump to row %d, %d jumps remaining, track %d",
                                 targetRow, L.hopRepeat, voice.getTrackId());
                        } else {
                            // Counter exhausted, don't jump, reset state and continue normally
                            L.hopTarget = -1;
                            LOGT("📋 Table HOP: counter exhausted, continuing past row, track %d", voice.getTrackId());
                        }
                    }
                }
                break;

            case FX_VOLUME:
                voice.tableVolume = fxValue / 255.0f;
                voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume * voice.carryVolume;
                break;

            case FX_TIC:
                // The rate of the COLUMN it is written in — that is what lets one table carry two
                // speeds at once. (Row 15's TIC is read at trigger instead; effectiveTicRatesFor.)
                if (fxValue >= 0x01 && fxValue <= 0xFB) {
                    L.ticRate = fxValue;
                    LOGT("📋 Table effect: TIC %02X - column %d now advances every %d tics",
                         fxValue, lane + 1, fxValue);
                }
                break;

            case FX_THO:
                hopExecuted = true;
                hopTarget = fxValue & 0x0F;
                LOGT("📋 Table THO %02X: hop to row %d, track %d", fxValue, hopTarget, voice.getTrackId());
                break;

            default:
                applyTableWrite(voice, fxType, fxValue, sampleRate);
                break;
        }
    };

    if (lane == 0)      processEffect(row.fx1Type, row.fx1Value);
    else if (lane == 1) processEffect(row.fx2Type, row.fx2Value);
    else                processEffect(row.fx3Type, row.fx3Value);

    L.lastProcessed = L.row;

    if (hopExecuted && hopTarget >= 0) {
        L.row = hopTarget % 16;
        LOGT("📋 Table HOP: track %d column %d jumped to row %d", voice.getTrackId(), lane + 1, L.row);
        return L.active && voice.tableId >= 0;
    }
    // A HOP whose repeat count is spent still does not play: it is stepped past, and the row after
    // it is the one that plays. ⚠️ Unconditionally, NOT under `shouldAdvance` — a lane that does not
    // advance on its own (TIC 00 and the two map modes) would otherwise sit on a row that can never
    // sound.
    if (steers && L.active) {
        L.row = (L.row + 1) % 16;
        return voice.tableId >= 0;
    }
    if (shouldAdvance) {
        L.row = (L.row + 1) % 16;
    }

    if (lane == 0 && shouldAdvance && L.row == 0) {
        LOGT("📋 Table %d loop: track=%d, transpose=%.0f, vol=%.2f",
             voice.tableId, voice.getTrackId(), voice.tableTranspose, voice.tableVolume);
    }
    return false;
}

// ─── The table commands that WRITE something ─────────────────────────────────────────────────────
//
// Shared by a played row and by the carry a hit brings through INS rows (applyTableCarry), so a cutoff
// handed on by a switch is the same write a row makes. Unknown codes do nothing.
template <typename V>
void AudioEngine::applyTableWrite(V& voice, int fxType, int fxValue, float sampleRate) {
    switch (fxType) {
        case FX_OFFSET:
            tableOffset(voice, fxValue);
            break;

        // CUT / RES on a table row: the same per-voice write the FX column makes, once per tic —
        // a sweep that follows every note the instrument plays without being written per phrase.
        case FX_CUT:
            voiceSetFilterCut(voice, fxValue, sampleRate);
            break;

        case FX_RES:
            voiceSetFilterRes(voice, fxValue, sampleRate);
            break;

        // LPF / HPF / BPF on a table row. The reason they belong here as much as in a phrase: a
        // table follows the INSTRUMENT, so one row gives every note that instrument ever plays a
        // filter — including the CUT and RES rows above it, which without one are inert.
        case FX_LPF: voiceSetFilterMode(voice, 1, fxValue, sampleRate); break;
        case FX_HPF: voiceSetFilterMode(voice, 2, fxValue, sampleRate); break;
        case FX_BPF: voiceSetFilterMode(voice, 3, fxValue, sampleRate); break;

        // DRV / CRU on a table row — the same per-voice writes the FX column makes, once per
        // tic. A table is where a dirt that rises while the note holds is actually written,
        // because it wants a value per tic rather than one per step.
        case FX_DRV: voiceSetDrive(voice, fxValue);     break;
        case FX_CRU: voiceSetCrush(voice, fxValue);     break;

        // FIN on a table row — a tuning per tic, which is where a chorus or a drifting detune is
        // actually written. ⚠️ It shares no state with the table's TRANSPOSE column: that column
        // drives the MOD half of the same bus slot and this writes the BASE, so the two add.
        case FX_FIN: voiceSetFineTune(voice, fxValue);  break;

        // LPO on a table row, which is where the slide is most of the point: a row under a HOP
        // walks the loop window a step per tic for as long as the note holds, and that walk is
        // the drone, the timestretch and the wavetable scan. ⚠️ It ACCUMULATES — a row that fires
        // 100 tics has moved the window 100 steps, unlike every other arm here.
        case FX_LPO: voiceSlideLoop(voice, fxValue);    break;

        // EQN / EQM on a table row: the same two writes the FX column's EQN and EQM make, once
        // per tic, reached directly rather than through the param queue because the voice is
        // already in hand — the queue's only job on that path is finding it.
        //
        // ⚠️ THE TWO HAVE DIFFERENT LIFETIMES, and only one of them cleans up after itself. EQN
        // writes THIS voice's chain and dies with the note, because a note-on rebuilds the chain
        // from the instrument. EQM writes the MASTER BUS, which outlives every voice and the
        // table with them — so it is armed for the restore on stop() the phrase-level EQM gets
        // from the scheduler's own flag (songcore/scheduler.h eqm_active, host.h stop). Without
        // the arming the bus keeps the table's preset after the transport stops, and the FX
        // helper's "resets to mixer EQ on stop" would be false for exactly this one way in.
        case FX_EQN:
            applyEqPresetToModule(voice.chain.eq, fxValue);
            break;

        case FX_EQM:
            applyEqPresetToModule(masterChain.masterEq, fxValue);   // not setMasterEqSlot: a table's
            tableMasterEqTouched.store(true, std::memory_order_relaxed);   // override is not the song's
            break;

        // TIM on a table row — the delay's echo time, once per tic, which is where the command is
        // most of the fun: a row under a HOP walks the time a step per tic, and the head glides to
        // each one, so the repeats bend continuously.
        //
        // ⚠️ IT IS GLOBAL AND THE VOICES ARE NOT. Two voices standing on different rows of the same
        // table write it in turn and the last one in the block wins — exactly what EQM above does,
        // and for the same reason: a shared bus reached from a per-voice walk. A TIM belongs in a
        // table one instrument drives, not in one eight tracks share.
        //
        // ⚠️ Latched like EQM, and for the identical reason: the send outlives every voice, so
        // stop() has to put the DELAY screen's own time back (host.h).
        case FX_TIM:
            delaySend.setTimeFree(fxValue);
            tableDelayTimeTouched.store(true, std::memory_order_relaxed);
            break;

        default:
            break;
    }
}

template <typename V>
void AudioEngine::applyTableCarry(V& voice, const TableCarry& carry, float sampleRate) {
    voice.carrySemitones = carry.semitones;
    voice.carryVolume    = carry.volume;
    voice.modSourceValues[MOD_SRC_TABLE_PITCH] = carry.semitones;
    voice.modSourceValues[MOD_SRC_TABLE_VOL]   = carry.volume;
    for (int i = 0; i < carry.fxCount; ++i)
        applyTableWrite(voice, carry.fxType[i], carry.fxValue[i], sampleRate);
}

// ─── AUS / AUF on a table row ────────────────────────────────────────────────────────────────────
//
// The pairing and the curve are both shared — `table_automation.h` runs the same walk the TABLE
// editor dims cells from, and `automation_curve.h` is the same polynomial the phrase path emits
// through, so a table morph and a phrase morph over the same two presets land on the same bytes.
// What is here is only the apply: the five effects a table row can both carry and ramp.
//
// ⚠️ **EVERY CALL, NOT EVERY ROW** — that is the whole reason processTableTick was split. A ramp
// re-evaluated only on a row change would be sixteen values, the sub-row interpolation would be dead
// code, and a slow morph would step audibly. The cost is one 48-slot walk plus, for an EQ ramp, six
// `powf` per voice per call (a block, or less where a row begins inside it); at eight voices that is
// a fraction of a percent of a core, and it buys a fade instead of a staircase.
template <typename V>
void AudioEngine::applyTableRamps(V& voice, const TableRow* rows,
                                  const double (&rowFraction)[TABLE_LANES], float sampleRate) {
    const table_automation::TableRampSet ramps = table_automation::find_table_ramps(rows, 16);

    for (int i = 0; i < ramps.count; ++i) {
        const table_automation::TableRamp& r = ramps.items[i];
        // ⚠️⚠️ **A RAMP RUNS ON THE COLUMN ITS PARAMETER IS IN, NOT THE ONE ITS AUS IS IN.** AUS pairs
        // by looking LEFT along its own row, so the value being faded always sits in a lower slot
        // than the curve — every ramp spans at least two columns, and with three playheads there are
        // two candidate clocks. The parameter's is the only one that cannot fight itself: that same
        // cell is re-applied on its own column's tic, and driving the fade from the AUS's column
        // would have the two writing one destination at two rates. (In practice the parameter is in
        // FX1, so a fade written before per-column playback existed keeps running on lane 0.)
        const TableLane& L = voice.lanes[r.paramSlot - 1];
        if (!L.active || L.lastProcessed < 0) continue;
        const double t = table_automation::table_ramp_position(r, L.lastProcessed,
                                                               rowFraction[r.paramSlot - 1]);
        if (t < 0.0) continue;   // this ramp does not cover the row that column is standing on

        if (r.eqPreset) {
            // ⚠️ Clamped rather than trusted, as the phrase morph is: pairing refuses an endpoint
            // that is not a slot, so the authored path cannot produce one — but a hand-edited
            // project file is not the authored path, and an unchecked index reads off the bank.
            if (!table_automation::is_eq_slot(r.startByte) ||
                !table_automation::is_eq_slot(r.destByte)) continue;
            const EqBandsHex& from = eqPresetHex[r.startByte];
            const EqBandsHex& to   = eqPresetHex[r.destByte];
            EqBandsHex m;
            for (int b = 0; b < 3; ++b) {
                const songcore::AutomationEqBand v = songcore::automation_eq_band_at(
                        { from.type[b], from.freq[b], from.gain[b], from.q[b] },
                        { to.type[b],   to.freq[b],   to.gain[b],   to.q[b]   }, r.curveByte, t);
                m.type[b] = v.type;
                m.freq[b] = v.freq;
                m.gain[b] = v.gain;
                m.q[b]    = v.q;
            }
            if (r.fxCode == FX_EQN) {
                applyEqBandsToModule(voice.chain.eq, m);
            } else {
                applyEqBandsToModule(masterChain.masterEq, m);
                // Same one-way latch the per-row EQM arms: the master bus outlives the voice, so
                // stop() has to put the project's own preset back.
                tableMasterEqTouched.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        const int value = songcore::automation_value_byte(r.startByte, r.destByte, r.curveByte, t);
        switch (r.fxCode) {
            case FX_VOLUME:
                voice.tableVolume = value / 255.0f;
                voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume * voice.carryVolume;
                break;
            case FX_CUT: voiceSetFilterCut(voice, value, sampleRate); break;
            case FX_RES: voiceSetFilterRes(voice, value, sampleRate); break;
            // A ramp over one of these moves the CUTOFF and re-asserts the same type every block, so
            // a sweep cannot lose the filter it opened with half way through.
            case FX_LPF: voiceSetFilterMode(voice, 1, value, sampleRate); break;
            case FX_HPF: voiceSetFilterMode(voice, 2, value, sampleRate); break;
            case FX_BPF: voiceSetFilterMode(voice, 3, value, sampleRate); break;
            case FX_DRV: voiceSetDrive(voice, value);     break;
            // A ramp over FIN is a glide: end to end is two semitones, spread over the AUS window.
            case FX_FIN: voiceSetFineTune(voice, value);  break;
            // …and a ramp over TIM is the tape swoop — see the per-row arm for what it writes and why
            // it is latched.
            case FX_TIM:
                delaySend.setTimeFree(value);
                tableDelayTimeTouched.store(true, std::memory_order_relaxed);
                break;
            // ⚠️ No FX_CRU arm, and its ARMS row says `rampable = false` — a packed pair of nibbles
            // is not a quantity to interpolate (songcore/effects.h). The default below drops it.
            default: break;   // the registry admits nothing else the table has an arm for
        }
    }
}

void AudioEngine::loadTable(int tableId, const uint8_t* rowData) {
    if (tableId < 0 || tableId >= 256) return;

    TableRow rows[16];
    for (int row = 0; row < 16; row++) {
        int offset = row * 8;
        rows[row].transpose = (int8_t)rowData[offset + 0];
        rows[row].volume = rowData[offset + 1];
        rows[row].fx1Type = rowData[offset + 2];
        rows[row].fx1Value = rowData[offset + 3];
        rows[row].fx2Type = rowData[offset + 4];
        rows[row].fx2Value = rowData[offset + 5];
        rows[row].fx3Type = rowData[offset + 6];
        rows[row].fx3Value = rowData[offset + 7];
    }
    std::lock_guard<std::mutex> lock(tableWriteMutex);   // writers only; the audio thread never takes it
    tables.write(tableId, rows);

    LOGD("📋 Loaded table %d", tableId);
}

// The TABLE screen's playing-row indicator (ui/engine_feed.h) reads these two, at 60 Hz.
//
// ⚠️ They answer "where is this track's table", NOT "is a voice sounding" — the two diverge, and the
// indicator is the thing that shows it. A retrigger leaves the OLD voice fading beside the new one for
// the length of its declick, so a plain first-active-slot scan can report the previous note's row; and
// a one-shot that ends before the next note leaves no voice at all, which read as "no table running"
// and blanked the indicator for the rest of the note. Both are set by the instrument's ROOT note —
// root sets playback rate, rate sets how long the sample lasts — so the same table on the same phrase
// stepped smoothly at one root and skipped and stalled at another.
//
// Order: the live voice, then the SF voice, then the track cursor (the TIC00 table outlives its
// voices), and a fading voice only as a last resort — it goes on ticking its own table after the note
// that replaced it has moved on, so it is the stalest source here, not the freshest.
static int findTrackVoice(Voice* voices, int trackId, bool fading) {
    for (int v = 0; v < MAX_VOICES; v++)
        if (voices[v].isActive && voices[v].isFadingOut == fading && voices[v].trackId == trackId) return v;
    return -1;
}

// ⚠️⚠️ **THE ROW IN FORCE IS `lastProcessed`, NOT `row`.** The lane's cursor has already been moved
// on — advanced, or HOPped — to the row that comes NEXT, so drawing it puts the marker a row ahead of
// what is being heard, and parks it on rows that never sound: a `HOP` row is resolved without ever
// playing, so `row` rests on it while the row it jumped to is the one making the sound. `row` is the
// honest answer only before the lane has consumed anything, where it is where the lane will start.
// Same pairing the AUS/AUF ramp reads, and for the same reason.
static inline int laneMarker(int row, int lastProcessed) {
    return lastProcessed >= 0 ? lastProcessed : row;
}

// ⚠️ A column that has executed `HOP FF` reads −1, the same "no position" the whole call answers with
// — the marker for that column disappears while its neighbours keep moving, which is the only honest
// drawing of a table with one column stopped.
static void lanesOf(const TableLane (&lanes)[TABLE_LANES], int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l)
        out[l] = lanes[l].active ? laneMarker(lanes[l].row, lanes[l].lastProcessed) : -1;
}

// Audio thread, end of every block: what the voice getters below answer from.
void AudioEngine::publishVoiceView() {
    VoiceView v;
    for (int i = 0; i < MAX_VOICES; i++) {
        const Voice& src = voices[i];
        VoiceView::Sampler& d = v.sampler[i];
        d.active    = src.isActive;
        d.fading    = src.isFadingOut;
        d.trackId   = src.trackId;
        d.instrId   = src.instrId;
        d.tableId   = src.tableId;
        d.note      = src.noteOctave * 12 + src.notePitch;
        d.loopStart = src.actualLoopStart;
        d.loopEnd   = src.actualLoopEnd;
        d.sampleGen = src.sampleGen;
        d.position  = src.position;
        lanesOf(src.lanes, d.lanes);
    }
    for (int t = 0; t < SF_VOICE_COUNT; t++) {
        const SoundfontVoice& src = sfVoices[t];
        v.sf[t].active  = src.isActive;
        v.sf[t].tableId = src.tableId;
        v.sf[t].note    = src.noteOctave * 12 + src.notePitch;
        lanesOf(src.lanes, v.sf[t].lanes);
        v.tic00Sounding[t].tableId = tic00Sounding[t];
        for (int s = 0; s < TIC00_SLOTS; ++s) {
            const Tic00Cursor& c = tic00Cursor[t][s];
            v.tic00[t][s].tableId = c.tableId;
            for (int l = 0; l < TABLE_LANES; ++l)
                v.tic00[t][s].lanes[l] = c.active[l] ? laneMarker(c.row[l], c.lastProcessed[l]) : -1;
        }
    }
    voiceViewPublisher.publish(v);
}

static void copyLanes(const int (&from)[TABLE_LANES], int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = from[l];
}

void AudioEngine::getVoiceTableRows(int trackId, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = -1;
    const VoiceView& v = voiceView();

    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0) { copyLanes(v.sampler[live].lanes, out); return; }

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const VoiceView::Sf& sv = v.sf[trackId];
        if (sv.active && sv.tableId >= 0) { copyLanes(sv.lanes, out); return; }
        // The table the voice last RAN — a chain's routers have bookmarks too, and the screen is
        // showing the one that made the sound.
        if (const VoiceView::Bookmark* b = v.bookmark(trackId, v.tic00Sounding[trackId].tableId)) {
            copyLanes(b->lanes, out);
            return;
        }
    }
    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    if (fading >= 0) copyLanes(v.sampler[fading].lanes, out);
}

// Same precedence as getVoiceTableRows — live voice, SF voice, the table's bookmark, a fading voice
// last — but every source has to be in the table that was ASKED for.
bool AudioEngine::getTableRowsFor(int trackId, int tableId, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = -1;
    if (tableId < 0) return false;
    const VoiceView& v = voiceView();

    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0 && v.sampler[live].tableId == tableId) { copyLanes(v.sampler[live].lanes, out); return true; }

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const VoiceView::Sf& sv = v.sf[trackId];
        if (sv.active && sv.tableId == tableId) { copyLanes(sv.lanes, out); return true; }

        // No voice is carrying it: a TIC00 table between notes, or one a hit only ROUTED through,
        // which is the same place kept the same way.
        if (const VoiceView::Bookmark* b = v.bookmark(trackId, tableId)) {
            copyLanes(b->lanes, out);
            return true;
        }
    }

    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    if (fading >= 0 && v.sampler[fading].tableId == tableId) { copyLanes(v.sampler[fading].lanes, out); return true; }
    return false;
}

bool AudioEngine::getVoiceLoopWindow(int trackId, int* startFrame, int* endFrame) {
    const VoiceView& v = voiceView();
    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live < 0) return false;
    if (startFrame) *startFrame = v.sampler[live].loopStart;
    if (endFrame)   *endFrame   = v.sampler[live].loopEnd;
    return true;
}

int AudioEngine::getVoiceTableId(int trackId) {
    const VoiceView& v = voiceView();
    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0) return v.sampler[live].tableId;

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        if (v.sf[trackId].active) return v.sf[trackId].tableId;
        if (v.tic00Sounding[trackId].tableId >= 0) return v.tic00Sounding[trackId].tableId;
    }
    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    return fading >= 0 ? v.sampler[fading].tableId : -1;
}

// processAudioBlock (engine-mix.cpp) ticks both voice pools.
template int AudioEngine::processTableTick<Voice>(Voice&, int, int, float);
template int AudioEngine::processTableTick<SoundfontVoice>(SoundfontVoice&, int, int, float);
template void AudioEngine::applyTableCarry<Voice>(Voice&, const TableCarry&, float);
template void AudioEngine::applyTableCarry<SoundfontVoice>(SoundfontVoice&, const TableCarry&, float);
