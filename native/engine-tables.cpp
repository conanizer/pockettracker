// The table walk: the TIC00 bookmarks, the INS chain a hit is routed through, a voice's per-block row
// tick and AUS/AUF ramps, and the playing-row readout the TABLE screen draws.
#include "audio-engine.h"
#include "engine-voice-ops.h"
#include "songcore/table_automation.h"  // AUS/AUF pairing over a table's rows — shared with the table editor
#include <cstdint>

// A TIC in the LAST row sets its column's rate (FX1 → lane 0 …); a column without one keeps the
// instrument's. ⚠️ Only this row's TIC is read at the trigger, so it holds from row 0; a TIC anywhere
// else acts when its lane reaches it.
void AudioEngine::resetTic00Cursors() {
    for (int t = 0; t < SF_VOICE_COUNT; ++t) {
        for (int s = 0; s < TIC00_SLOTS; ++s) tic00Cursor[t][s] = Tic00Cursor();
        tic00Sounding[t] = -1;
    }
}

// Out of slots (a chain deeper than the cap), the oldest-numbered one is reused: a rotation restarts
// rather than the switch failing.
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

// RND re-fires the nearest command ABOVE it in its column with 0..xx added. The base is always the
// typed cell, never an earlier roll, so rolls spread instead of wandering. False when nothing is above.
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

// The row as ONE column plays it after the dice, in the phrase's order: CHA, RND, RNL.
// CHA XY: X gates the nearest filled cell on the LEFT (none → the row's N and V), Y the one on the
// RIGHT; 0 never, F always. Its rolls are made once per row per tick and shared by the lanes (`rolls`).
// `nvPlays` is false when a CHA with nothing to its left lost that roll. An RNL in the next column adds
// 0..xx to this one.
static TableRow rollTableRow(const TableRow rows[16], int row, int lane, uint32_t& rng,
                             int (&rolls)[3][2], bool& nvPlays) {
    TableRow played = rows[row];
    uint8_t& fxT = (lane == 0) ? played.fx1Type  : (lane == 1) ? played.fx2Type  : played.fx3Type;
    uint8_t& fxV = (lane == 0) ? played.fx1Value : (lane == 1) ? played.fx2Value : played.fx3Value;
    int t3[3], v3[3];
    rowFx(played, t3, v3);

    bool alive[3] = {t3[0] != 0, t3[1] != 0, t3[2] != 0};
    nvPlays = true;
    for (int s = 0; s < 3; ++s) {
        if (t3[s] != FX_CHA || !alive[s]) continue;
        for (int& r : rolls[s]) if (r < 0) r = static_cast<int>(xorshift32(rng) % 15u);
        int left = s - 1, right = s + 1;
        while (left >= 0 && !alive[left]) --left;
        while (right <= 2 && !alive[right]) ++right;
        if (rolls[s][0] >= ((v3[s] >> 4) & 0x0F)) {
            if (left >= 0) alive[left] = false;
            else           nvPlays = false;
        }
        if (right <= 2 && rolls[s][1] >= (v3[s] & 0x0F)) alive[right] = false;
    }
    if (!alive[lane]) fxT = 0;

    if (fxT == FX_RND) {
        int t = 0, v = 0;
        if (tableRandomize(rows, row, lane, fxV, rng, t, v)) {
            fxT = static_cast<uint8_t>(t);
            fxV = static_cast<uint8_t>(v);
        } else {
            fxT = 0;
        }
    }
    if (lane < 2 && fxT != 0 && alive[lane + 1] && t3[lane + 1] == FX_RNL && v3[lane + 1] > 0) {
        const int added = static_cast<int>(xorshift32(rng) % static_cast<uint32_t>(v3[lane + 1] + 1));
        fxV = static_cast<uint8_t>(std::min(fxV + added, tableFxCeiling(fxT)));
    }
    return played;
}

int AudioEngine::resolveChain(int trackId, int instrumentId, int tableIdOverride, int* outTableId,
                              TableCarry* carry) {
    int tableId = (tableIdOverride >= 0) ? tableIdOverride : instrumentId;
    int visited[CHAIN_MAX_LINKS];
    int visitedCount = 0;

    for (int link = 0; link < CHAIN_MAX_LINKS; ++link) {
        if (outTableId) *outTableId = tableId;

        // An empty slot or external gear sounds nothing rather than falling back. Link 0 the
        // scheduler already checked.
        if (instrumentId < 0 || instrumentId >= songcore::PROGRAM_SLOTS) return -1;
        const songcore::Program& p = programs[instrumentId].program;
        if (p.type == songcore::PROGRAM_EXTERNAL) return -1;   // until the cable is a program too
        if (link > 0 && !p.hasSample && !p.hasSoundfont) return -1;

        // A table reached twice on one hit: stop and sound here.
        for (int i = 0; i < visitedCount; ++i) if (visited[i] == tableId) return instrumentId;
        if (tableId < 0 || tableId >= 256) return instrumentId;

        TableRow rows[16];
        if (!tables.read(tableId, rows)) return instrumentId;

        // A table with no INS cannot route: it is left alone, no place kept.
        bool routes = false;
        for (int r = 0; r < 16 && !routes; ++r) {
            int t3[3], v3[3];
            rowFx(rows[r], t3, v3);
            for (int s = 0; s < 3; ++s) routes |= (t3[s] == FX_INS);
        }
        if (!routes) return instrumentId;

        // ⚠️ The row the voice would start at: at TIC00 one row further per trigger (the rotation),
        // otherwise row 0. With no switch, the voice starts on it. Rates per column as effectiveTicRatesFor.
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

        // Follow HOPs per column before reading the row. A counted HOP jumps on every hit:
        // `reset_table_lanes` places each lane fresh at the trigger, so no count carries.
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

        // Each column is read at its own row (at TIC00 they coincide).
        int type[3], value[3], slotRow[3];
        for (int s = 0; s < 3; ++s) {
            int t3[3], v3[3];
            rowFx(rows[laneRow[s]], t3, v3);
            type[s] = t3[s]; value[s] = v3[s]; slotRow[s] = laneRow[s];
        }

        // CHA as on a played row (rollTableRow). A cleared INS is a row with no switch; a lost N/V
        // stays out of what the hit carries.
        bool nvPlays = true;
        for (int s = 0; s < 3; ++s) {
            if (type[s] != FX_CHA) continue;
            const int v = value[s];
            int left = s - 1, right = s + 1;
            while (left >= 0 && type[left] == 0) --left;
            while (right <= 2 && type[right] == 0) ++right;
            if (static_cast<int>(xorshift32(chainRngState) % 15u) >= ((v >> 4) & 0x0F)) {
                if (left >= 0) type[left] = 0;
                else           nvPlays = false;
            }
            if (right <= 2 && static_cast<int>(xorshift32(chainRngState) % 15u) >= (v & 0x0F)) type[right] = 0;
        }

        // RND below an INS picks a nearby instrument per hit; below a CUT it moves the carried cutoff.
        for (int s = 0; s < 3; ++s) {
            if (type[s] != FX_RND) continue;
            if (!tableRandomize(rows, slotRow[s], s, value[s], chainRngState, type[s], value[s]))
                type[s] = 0;
        }

        // RNL adds 0..xx to the slot on its LEFT — beside an INS, a pool-picker. Read wherever it sits.
        for (int s = 1; s < 3; ++s) {
            if (type[s] != FX_RNL || type[s - 1] != FX_INS) continue;
            const int range = value[s] & 0xFF;
            if (range > 0) value[s - 1] += static_cast<int>(xorshift32(chainRngState) % (uint32_t)(range + 1));
            if (value[s - 1] > 127) value[s - 1] = 127;   // INS's own ceiling
        }

        // ⚠️ The rotation moves on every hit, switch or not — written first, or a row without an INS
        // would freeze the table on it. The leftmost switch wins; what is right of it is not in the path.
        int next = -1, switchSlot = -1;
        for (int s = 0; s < 3; ++s) {
            if (type[s] == FX_INS) { next = value[s] & 0x7F; switchSlot = s; break; }
        }
        // No switch: this instrument sounds, its voice starting where it would anyway.
        if (next < 0) return instrumentId;

        // What is left of the switch travels with the hit — no voice plays this row. The VOL column
        // and a VOL cell are one write, the cell winning.
        if (carry) {
            const TableRow& r0 = rows[laneRow[0]];
            float vol = (!nvPlays || r0.volume == 0xFF) ? 1.0f : r0.volume / 255.0f;
            for (int s = 0; s < switchSlot; ++s) {
                const int t = type[s];
                if (t == FX_VOLUME) { vol = static_cast<float>(value[s]) / 255.0f; continue; }
                if (t == 0 || t == FX_HOP || t == FX_THO || t == FX_TIC || t == FX_KILL ||
                    t == FX_RNL || t == FX_CHA)
                    continue;
                if (carry->fxCount < TABLE_CARRY_FX_MAX) {
                    carry->fxType[carry->fxCount]  = static_cast<uint8_t>(t);
                    carry->fxValue[carry->fxCount] = static_cast<uint8_t>(value[s]);
                    carry->fxCount++;
                }
            }
            if (nvPlays) carry->semitones += static_cast<float>(transposeToSemitones(r0.transpose));
            carry->volume    *= vol;
        }

        // ⚠️ No voice runs this table, so its step is made here, or the next trigger reads the same row
        // for ever. Only a per-trigger column keeps a place.
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

    // Depth cap: sound here.
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

// TIC00: a row per note, no advance · TICFC: row = the note's octave · TICFE: row = its pitch class ·
// TICFF: 200 rows a second.
// Frames one row lasts on a lane that advances (0 for the three that hold); 12 tics a step.
static double framesPerTableRow(int ticRate, int tempo, float sampleRate) {
    if (ticRate == 0x00 || ticRate == 0xFC || ticRate == 0xFE) return 0.0;
    if (ticRate == 0xFF) return sampleRate / 200.0;
    return sampleRate / (tempo / 60.0 * 4.0 * 12.0) * ticRate;
}

// A lane's clock landing a hair under its row length after `ceil` is the same frame, not one more.
static constexpr double TIC_EPSILON = 1e-6;

template <typename V>
int AudioEngine::processTableTick(V& voice, int from, int maxFrames, float sampleRate) {
    // The whole table: AUS/AUF pairing is re-derived from every row on every call, so nothing per voice.
    TableRow rows[16];
    if (!tables.read(voice.tableId, rows)) return maxFrames;
    const int tempo = currentTempo.load(std::memory_order_relaxed);

    // ⚠️ A blank table is never cut — every note runs its instrument's table, and a blank row changes
    // nothing.
    bool blank = true;
    for (const TableRow& r : rows)
        blank = blank && r.transpose == 0 && r.volume == 0xFF && !r.fx1Type && !r.fx2Type && !r.fx3Type;

    // Each CHA's two rolls per row, shared by the lanes — see rollTableRow.
    int rolls[16][3][2];
    for (auto& row : rolls) for (auto& slot : row) slot[0] = slot[1] = -1;

    // Each lane has its own clock: one shared would drag a TICFF lane to its TIC 06 neighbour.
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
            // Row 0 plays at the note's first sample; the clock starts at its onset.
            L.frameAccum = (voice.startDelayFrames > from) ? -(double)(voice.startDelayFrames - from) : 0.0;
            shouldProcessRow = shouldAdvance = true;
        } else if (L.frameAccum >= perRow - TIC_EPSILON) {
            L.frameAccum -= perRow;
            // The row shrank under the clock (tempo, a TIC row): drop the backlog, or rows run away.
            if (L.frameAccum >= perRow) L.frameAccum = 0.0;
            shouldProcessRow = shouldAdvance = true;
        }

        // ⚠️ A HOP or THO takes no tic: the row it lands on plays in this one. Bounded at a table's
        // rows, so a ring of jumps cannot spin the audio thread.
        for (int steered = 0; shouldProcessRow && steered <= 16; ++steered) {
            bool nvPlays = true;
            const TableRow played = rollTableRow(rows, L.row, lane, chainRngState, rolls[L.row], nvPlays);
            // A CHA that ate this column's AUS turns its ramp off until the row plays again.
            int a3[3], av3[3], p3[3], pv3[3];
            rowFx(rows[L.row], a3, av3);
            rowFx(played, p3, pv3);
            const uint16_t bit = static_cast<uint16_t>(1u << L.row);
            if (a3[lane] == table_automation::FX_AUS_CODE && p3[lane] != table_automation::FX_AUS_CODE) L.ausEaten = static_cast<uint16_t>(L.ausEaten | bit);
            else                                          L.ausEaten = static_cast<uint16_t>(L.ausEaten & ~bit);
            if (!processTableRow(voice, played, lane, shouldAdvance, from, sampleRate, nvPlays)) break;
        }
        // ⚠️ A HOP FF in an earlier lane can have cleared tableId: stop here.
        if (voice.tableId < 0) return maxFrames;
    }

    // The segment ends where any lane's next row begins, so a row starts on its own frame, not a
    // block edge. The fraction carries in the lane's clock.
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

    // A pan or volume glide keeps the pieces short.
    if ((voice.panGlideLeft > 0 || voice.volGlideLeft > 0) && frames > PAN_GLIDE_STEP) frames = PAN_GLIDE_STEP;

    // Each lane's progress through its row at the segment's END — the ramp's target. 0 when held.
    double rowFraction[TABLE_LANES] = {0.0, 0.0, 0.0};
    for (int lane = 0; lane < TABLE_LANES; ++lane) {
        TableLane& L = voice.lanes[lane];
        const double perRow = framesPerTableRow(L.ticRate, tempo, sampleRate);
        if (!L.active || perRow <= 0.0) continue;
        L.frameAccum += frames;
        rowFraction[lane] = std::max(0.0, std::min(1.0, L.frameAccum / perRow));
    }

    // ⚠️ Against `lastProcessed`, not the lane's row: the cursor already points at the NEXT row, and
    // reading it would run every fade a row early.
    applyTableRamps(voice, rows, rowFraction, sampleRate);
    return frames;
}

// One lane consuming one row. Transpose and volume belong to lane 0 (FX1's playhead) only, and a lane
// reads only its own FX slot; HOP, TIC and THO steer the lane they are in.
// ⚠️ A HOP or THO row is never heard — playing it would put a tic of table defaults before every loop.
// The caller re-enters on `true` until a row plays.
template <typename V>
bool AudioEngine::processTableRow(V& voice, const TableRow& row, int lane, bool shouldAdvance,
                                  int atFrame, float sampleRate, bool applyNV) {
    TableLane& L = voice.lanes[lane];

    const uint8_t laneFxType = (lane == 0) ? row.fx1Type : (lane == 1) ? row.fx2Type : row.fx3Type;
    const bool steers = (laneFxType == FX_HOP || laneFxType == FX_THO);

    if (lane == 0 && !steers && applyNV) {
        // Pitch reaches the voice through TABLE_PITCH → modDestValues[PARAM_PITCH].
        int semitones = transposeToSemitones(row.transpose);
        voice.tableTranspose = (float)semitones;  // kept for debug log
        voice.modSourceValues[MOD_SRC_TABLE_PITCH] = (float)semitones + voice.carrySemitones;

        // The mix reads modDestValues[PARAM_VOL], not tableVolume.
        const float wasVolume = voice.tableVolume;
        if (row.volume == 0xFF) {
            voice.tableVolume = 1.0f;  // kept for debug log
        } else {
            voice.tableVolume = row.volume / 255.0f;
        }
        if (voice.tableVolume != wasVolume) voiceGlideVol(voice);
        voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume * voice.carryVolume;
    }

    bool hopExecuted = false;
    int hopTarget = -1;

    auto processEffect = [&](uint8_t fxType, uint8_t fxValue) {
        switch (fxType) {
            case FX_KILL:
                if (fxValue == 0x00) {
                    voiceCut(voice, atFrame);
                    LOGT("📋 Table effect: KILL track %d", voice.getTrackId());
                }
                break;

            case FX_HOP:
                // HOP XY: X=repeat count (0=infinite), Y=target row; FF=stop THIS COLUMN
                if (fxValue == 0xFF) {
                    // Stops this lane only; the table ends when all three have.
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
                            // Count spent: no jump, reset, carry on.
                            L.hopTarget = -1;
                            LOGT("📋 Table HOP: counter exhausted, continuing past row, track %d", voice.getTrackId());
                        }
                    }
                }
                break;

            case FX_VOLUME:
                if (voice.tableVolume != fxValue / 255.0f) voiceGlideVol(voice);
                voice.tableVolume = fxValue / 255.0f;
                voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume * voice.carryVolume;
                break;

            case FX_TIC:
                // Sets this column's rate. (Row 15's TIC is read at the trigger — effectiveTicRatesFor.)
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
    // A spent HOP still does not play: step past it. ⚠️ Not under `shouldAdvance`, or a lane that
    // holds (TIC00, the map modes) would sit on a row that never sounds.
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

// The table commands that write something, for a played row and for a hit's carry through INS rows
// (applyTableCarry). Unknown codes do nothing.
template <typename V>
void AudioEngine::applyTableWrite(V& voice, uint8_t fxType, uint8_t fxValue, float sampleRate) {
    switch (fxType) {
        case FX_OFFSET:
            tableOffset(voice, fxValue);
            break;

        // 00 left, 80 centre, FF right.
        case FX_PAN:
            voiceGlidePan(voice, fxValue / 255.0f);
            break;

        // The FX column's per-voice writes, once per tic.
        case FX_CUT:
            voiceSetFilterCut(voice, fxValue, sampleRate);
            break;

        case FX_RES:
            voiceSetFilterRes(voice, fxValue, sampleRate);
            break;

        // A filter on every note the instrument plays — CUT and RES are inert without one.
        case FX_LPF: voiceSetFilterMode(voice, 1, fxValue, sampleRate); break;
        case FX_HPF: voiceSetFilterMode(voice, 2, fxValue, sampleRate); break;
        case FX_BPF: voiceSetFilterMode(voice, 3, fxValue, sampleRate); break;

        case FX_DRV: voiceSetDrive(voice, fxValue);     break;
        case FX_CRU: voiceSetCrush(voice, fxValue);     break;

        // ⚠️ Writes the pitch BASE; the TRANSPOSE column drives the MOD half — the two add.
        case FX_FIN: voiceSetFineTune(voice, fxValue);  break;

        // ⚠️ Accumulates, unlike every other arm: under a HOP the loop window walks a step per tic.
        case FX_LPO: voiceSlideLoop(voice, fxValue);    break;

        // ⚠️ EQN dies with the note; EQM writes the master bus, which outlives it, so it arms the
        // restore on stop() as the phrase's EQM does.
        case FX_EQN:
            applyEqPresetToModule(voice.chain.eq, fxValue);
            break;

        case FX_EQM:
            applyEqPresetToModule(masterChain.masterEq, fxValue);   // not setMasterEqSlot: a table's
            tableMasterEqTouched.store(true, std::memory_order_relaxed);   // override is not the song's
            break;

        // The delay's time per tic. ⚠️ Global: the last voice in the block wins. Latched like EQM.
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

// AUS / AUF on a table row: pairing from `table_automation.h`, curve from `automation_curve.h` (both
// shared with the phrase path); only the apply is here. Re-evaluated every call, not every row, or a
// slow morph would stair.
template <typename V>
void AudioEngine::applyTableRamps(V& voice, const TableRow* rows,
                                  const double (&rowFraction)[TABLE_LANES], float sampleRate) {
    const table_automation::TableRampSet ramps = table_automation::find_table_ramps(rows, 16);

    for (int i = 0; i < ramps.count; ++i) {
        const table_automation::TableRamp& r = ramps.items[i];
        // ⚠️ A ramp runs on its parameter's column, not its AUS's, or one destination moves at two rates.
        const TableLane& L = voice.lanes[r.paramSlot - 1];
        if (!L.active || L.lastProcessed < 0) continue;
        if (voice.lanes[r.ausSlot - 1].ausEaten & (1u << r.ausRow)) continue;   // a CHA ate its AUS
        const double t = table_automation::table_ramp_position(r, L.lastProcessed,
                                                               rowFraction[r.paramSlot - 1]);
        if (t < 0.0) continue;   // this ramp does not cover the row that column is standing on

        if (r.eqPreset) {
            // Clamped: a hand-edited file can name a slot past the bank.
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
                // The latch the per-row EQM arms.
                tableMasterEqTouched.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        const int value = songcore::automation_value_byte(r.startByte, r.destByte, r.curveByte, t);
        switch (r.fxCode) {
            case FX_VOLUME:
                if (voice.tableVolume != static_cast<float>(value) / 255.0f) voiceGlideVol(voice);
                voice.tableVolume = value / 255.0f;
                voice.modSourceValues[MOD_SRC_TABLE_VOL] = voice.tableVolume * voice.carryVolume;
                break;
            case FX_CUT: voiceSetFilterCut(voice, value, sampleRate); break;
            case FX_RES: voiceSetFilterRes(voice, value, sampleRate); break;
            case FX_PAN: voiceGlidePan(voice, static_cast<float>(value) / 255.0f); break;
            // Moves the cutoff and re-asserts the type each block.
            case FX_LPF: voiceSetFilterMode(voice, 1, value, sampleRate); break;
            case FX_HPF: voiceSetFilterMode(voice, 2, value, sampleRate); break;
            case FX_BPF: voiceSetFilterMode(voice, 3, value, sampleRate); break;
            case FX_DRV: voiceSetDrive(voice, value);     break;
            // A glide: two semitones end to end.
            case FX_FIN: voiceSetFineTune(voice, value);  break;
            // The tape swoop; latched like the per-row arm.
            case FX_TIM:
                delaySend.setTimeFree(value);
                tableDelayTimeTouched.store(true, std::memory_order_relaxed);
                break;
            // No CRU arm: two packed nibbles are not a quantity (`rampable = false` in effects.h).
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

// The TABLE screen's playing-row marker reads these two at 60 Hz. They answer "where is this track's
// table", not "is a voice sounding". Order: the live voice, the SF voice, the track's bookmark (a
// TIC00 table outlives its voices), a fading voice last.
static int findTrackVoice(Voice* voices, int trackId, bool fading) {
    for (int v = 0; v < MAX_VOICES; v++)
        if (voices[v].isActive && voices[v].isFadingOut == fading && voices[v].trackId == trackId) return v;
    return -1;
}

// ⚠️ The row in force is `lastProcessed`, not `row` — `row` already points at the next one.
static inline int laneMarker(int row, int lastProcessed) {
    return lastProcessed >= 0 ? lastProcessed : row;
}

// A column stopped by `HOP FF` reads −1: its marker goes, the others keep moving.
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
        // The table the voice last ran — a chain's routers have bookmarks too.
        if (const VoiceView::Bookmark* b = v.bookmark(trackId, v.tic00Sounding[trackId].tableId)) {
            copyLanes(b->lanes, out);
            return;
        }
    }
    const int fading = v.trackVoice(trackId, /*fadingOne=*/true);
    if (fading >= 0) copyLanes(v.sampler[fading].lanes, out);
}

// As getVoiceTableRows, but every source must be in the table asked for.
bool AudioEngine::getTableRowsFor(int trackId, int tableId, int out[TABLE_LANES]) {
    for (int l = 0; l < TABLE_LANES; ++l) out[l] = -1;
    if (tableId < 0) return false;
    const VoiceView& v = voiceView();

    const int live = v.trackVoice(trackId, /*fadingOne=*/false);
    if (live >= 0 && v.sampler[live].tableId == tableId) { copyLanes(v.sampler[live].lanes, out); return true; }

    if (trackId >= 0 && trackId < SF_VOICE_COUNT) {
        const VoiceView::Sf& sv = v.sf[trackId];
        if (sv.active && sv.tableId == tableId) { copyLanes(sv.lanes, out); return true; }

        // No voice carries it: a TIC00 table between notes, or one a hit only routed through.
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
