#ifndef POCKETTRACKER_SONGCORE_MIDI_IN_H
#define POCKETTRACKER_SONGCORE_MIDI_IN_H

// MIDI in: bytes from a cable become bus events. `IMidiIn` is the platform seam. Above it:
// `MidiInQueue` (the thread boundary), `MidiParser` (the protocol, no policy), `MidiInputRouter`
// (which track and instrument a key plays) and `MidiInPipeline` (the drain that runs them in turn).
// ⚠️ The drain runs on the AUDIO thread, so everything it touches is real-time safe; whatever needs
// the project happens on the UI thread one poll later. Frames are arguments — testable with no device.
// SYNC IN is not built: real-time bytes are parsed, counted and dropped.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "event.h"
#include "midi_map.h"   // the knob gate
#include "model.h"
#include "router.h"
#include "seqlock.h"

namespace songcore {

// Where a backend puts received bytes.
// ⚠️ Called from a thread you do not own: be quick, never block, never call back into the backend.
struct IMidiInSink {
    virtual ~IMidiInSink() = default;
    virtual void on_bytes(const uint8_t* data, int len) = 0;
};

// A MIDI input port, one per platform, shaped like `IMidiOut`.
// ⚠️ `set_sink(nullptr)` must be safe at any moment and honoured before `close()` returns, or a backend
// thread outlives the object it writes into.
struct IMidiIn {
    virtual ~IMidiIn() = default;

    // Re-enumerated on each call; hot-plug changes it.
    virtual int device_count() = 0;
    virtual std::string device_name(int index) = 0;

    virtual bool open(int index) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    // The open port failed in a way only a reopen cures (the cable was pulled).
    virtual bool broken() const { return false; }

    // Set BEFORE `open`, cleared before the sink dies.
    virtual void set_sink(IMidiInSink* sink) = 0;
};

// A byte ring between the backend's thread and the drain — bytes, since a backend read can split a
// message. ⚠️ One producer, one consumer (the audio thread), no lock; each side owns one index.
// Overflow is counted, never silent.
class MidiInQueue : public IMidiInSink {
  public:
    // ~341 three-byte messages, drained every few ms; a flat-out cable sends ~1 040 a second.
    static constexpr int CAPACITY = 1024;   // a power of two: the slot is `index & MASK`

    // When full, drops the NEWEST bytes — dropping the oldest would orphan note-offs still coming.
    void on_bytes(const uint8_t* data, int len) override {
        if (!data || len <= 0) return;
        const uint32_t head = head_.load(std::memory_order_acquire);
        uint32_t       tail = tail_.load(std::memory_order_relaxed);
        for (int i = 0; i < len; ++i) {
            if (tail - head >= static_cast<uint32_t>(CAPACITY)) {
                dropped_.fetch_add(static_cast<uint64_t>(len - i), std::memory_order_relaxed);
                break;
            }
            buf_[tail & MASK] = data[i];
            ++tail;
        }
        tail_.store(tail, std::memory_order_release);
    }

    // Move up to `max` bytes into `out`; returns how many.
    int drain(uint8_t* out, int max) {
        if (!out || max <= 0) return 0;
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        uint32_t       head = head_.load(std::memory_order_relaxed);
        int n = 0;
        while (head != tail && n < max) { out[n++] = buf_[head & MASK]; ++head; }
        head_.store(head, std::memory_order_release);
        return n;
    }

    // Bytes lost to a full ring, ever.
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    int pending() const {
        return static_cast<int>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
    }

    // ⚠️ The consumer's call — it moves the consumer's index.
    void clear() { head_.store(tail_.load(std::memory_order_acquire), std::memory_order_release); }

    // A mark for `discard_up_to`. Any thread.
    uint32_t written() const { return tail_.load(std::memory_order_acquire); }

    // The consumer's: drop everything written before `mark`.
    void discard_up_to(uint32_t mark) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (static_cast<int32_t>(mark - head) > 0) head_.store(mark, std::memory_order_release);
    }

  private:
    static constexpr uint32_t MASK = CAPACITY - 1;
    static_assert((CAPACITY & MASK) == 0, "the ring's capacity must be a power of two");

    uint8_t               buf_[CAPACITY] = {0};
    std::atomic<uint32_t> head_{0};      // consumer's
    std::atomic<uint32_t> tail_{0};      // producer's
    std::atomic<uint64_t> dropped_{0};
};

// How many bytes a message with this status byte occupies — 1, 2 or 3.
// ⚠️ Backends need it too: winmm packs a message into a DWORD without its length, and assuming three
// turns a program change into two.
inline int midi_message_length(uint8_t status) {
    if (status >= 0xF8) return 1;
    if (status < 0xF0) {
        const uint8_t hi = status & 0xF0;
        return (hi == 0xC0 || hi == 0xD0) ? 2 : 3;   // program change / channel pressure carry one
    }
    switch (status) {
        case 0xF1: return 2;   // MTC quarter frame
        case 0xF2: return 3;   // song position pointer
        case 0xF3: return 2;   // song select
        default:   return 1;   // 0xF4 0xF5 undefined, 0xF6 tune request
    }
}

// One complete message. `status` is the nibble for a channel message, the whole byte for a system one.
struct MidiInMessage {
    uint8_t status  = 0;   // 0x80-0xE0 channel (nibble) | 0xF1-0xFF system (whole byte)
    uint8_t channel = 0;   // 0-15; meaningless for system messages
    uint8_t data1   = 0;
    uint8_t data2   = 0;
    uint8_t len     = 0;   // 1-3, as it appeared on the wire (running status excluded)

    bool is_channel()  const { return status >= 0x80 && status <= 0xE0; }
    bool is_realtime() const { return status >= 0xF8; }

    // ⚠️ A note-on with velocity 0 IS a note-off. Ask these, never `status`.
    bool is_note_on()  const { return status == EV_NOTE_ON  && data2 > 0; }
    bool is_note_off() const { return status == EV_NOTE_OFF || (status == EV_NOTE_ON && data2 == 0); }
};

// A streaming parser: running status; real-time bytes mid-message change NOTHING (resetting would eat
// notes while a clock runs); System Common and SysEx cancel running status; orphan data bytes are
// dropped and counted.
class MidiParser {
  public:
    // Feed one byte; true when `message()` holds a newly completed message.
    bool feed(uint8_t b) {
        // ── real time: complete in one byte, and interrupts NOTHING ──
        if (b >= 0xF8) {
            msg_ = MidiInMessage{};
            msg_.status = b;
            msg_.len    = 1;
            return true;                      // status_, pending_, running status: all untouched
        }

        if (b >= 0x80) {                      // a status byte
            if (b == 0xF7) { inSysex_ = false; return false; }          // end of SysEx
            pendingCount_ = 0;
            if (b == 0xF0) {                  // start of SysEx — skip its body
                inSysex_ = true;
                status_  = 0;
                return false;
            }
            inSysex_ = false;
            status_  = b;
            expect_  = data_bytes_for(b);
            if (expect_ == 0) {               // 0xF6 tune request, 0xF4/0xF5 undefined
                emit();
                status_ = 0;                  // system messages never establish running status
                return true;
            }
            return false;
        }

        // ── a data byte ──
        if (inSysex_) return false;
        if (status_ == 0) { orphans_.fetch_add(1, std::memory_order_relaxed); return false; }   // no running status to belong to

        pending_[pendingCount_++] = b;
        if (pendingCount_ < expect_) return false;

        emit();
        pendingCount_ = 0;
        if (status_ >= 0xF0) status_ = 0;     // System Common is one-shot; channel status runs on
        return true;
    }

    const MidiInMessage& message() const { return msg_; }

    // Data bytes with no status to belong to: a resync, or — if many — a bug.
    uint64_t orphan_bytes() const { return orphans_.load(std::memory_order_relaxed); }

    // Forget everything mid-flight, when a port closes.
    void reset() {
        status_ = 0;
        expect_ = pendingCount_ = 0;
        inSysex_ = false;
        msg_ = MidiInMessage{};
    }

  private:
    static int data_bytes_for(uint8_t status) { return midi_message_length(status) - 1; }

    void emit() {
        msg_ = MidiInMessage{};
        if (status_ < 0xF0) {
            msg_.status  = status_ & 0xF0;
            msg_.channel = status_ & 0x0F;
        } else {
            msg_.status = status_;
        }
        msg_.data1 = expect_ > 0 ? pending_[0] : 0;
        msg_.data2 = expect_ > 1 ? pending_[1] : 0;
        msg_.len   = static_cast<uint8_t>(1 + expect_);
    }

    MidiInMessage msg_;
    uint8_t  status_ = 0;          // the status byte in force — running status, once established
    int      expect_ = 0;          // data bytes `status_` wants
    uint8_t  pending_[2] = {0, 0};
    int      pendingCount_ = 0;
    bool     inSysex_ = false;
    std::atomic<uint64_t> orphans_{0};   // read by the UI's counters, written by the drain
};

// ─── The route: the routing facts, flat, for a thread that cannot read the project ───────────────

struct MidiRouteInstrument {
    float   volume   = 1.0f;   // hex_to_float(ins.volume), baked into a note-on as the sequencer does
    float   pan      = 0.5f;
    uint8_t external = 0;      // routes to the cable, not to a voice
};

// ⚠️ The ONLY view of the project the drain sees, published whole from the UI thread. A new routing
// rule's facts go in here, never a pointer to the project.
struct MidiRoute {
    int16_t  instrument;                   // what a key plays: the instrument the UI is on; -1 = none
    int8_t   baseTrack;                    // the track the SONG cursor is on — the first voice
    int8_t   voices;                       // 1 = MONO, 2..8 = POLY: tracks baseTrack.. baseTrack+voices-1
    int16_t  instrumentCount;              // ids at or past this resolve to nothing
    int8_t   controlChannel;               // the CTL CH row: 0-15, MIDI_CTL_CH_ALL, or -1 for none
    uint8_t  learnArmed;                   // R is held: a knob on the control channel names, never drives
    uint8_t  velocity;                     // 0 = every key plays at full strength
    uint64_t claimed[2];                   // bit per controller number: a mapping would drive it
    MidiRouteInstrument ins[POOL_INSTRUMENTS];

    bool claims(uint8_t controller) const {
        return controller < 128 && ((claimed[controller >> 6] >> (controller & 63)) & 1u) != 0;
    }
};

// Build the route on the UI thread. A mapping whose destination is gone claims nothing, so its knob
// falls through to the tracks.
inline MidiRoute build_midi_route(const Project& p, int instrument, int baseTrack, int voices,
                                  int controlChannel, bool learnArmed, bool velocity = true) {
    MidiRoute r{};
    r.instrument      = static_cast<int16_t>(instrument);
    r.baseTrack       = static_cast<int8_t>(baseTrack < 0 ? 0 : (baseTrack >= POOL_TRACKS ? POOL_TRACKS - 1 : baseTrack));
    r.voices          = static_cast<int8_t>(voices < 1 ? 1 : (voices > POOL_TRACKS ? POOL_TRACKS : voices));
    r.instrumentCount = static_cast<int16_t>(p.instruments.size() < static_cast<size_t>(POOL_INSTRUMENTS)
                                                 ? p.instruments.size() : static_cast<size_t>(POOL_INSTRUMENTS));
    r.controlChannel  = static_cast<int8_t>(controlChannel);
    r.learnArmed      = learnArmed ? 1 : 0;
    r.velocity        = velocity ? 1 : 0;
    for (int i = 0; i < r.instrumentCount; ++i) {
        const Instrument& ins = p.instruments[static_cast<size_t>(i)];
        r.ins[i].volume   = hex_to_float(ins.volume);
        r.ins[i].pan      = hex_to_float(ins.pan);
        r.ins[i].external = instrument_routes_external(ins) ? 1 : 0;
    }
    for (const MidiMapping& m : p.midiMappings) {
        if (m.controller > 127) continue;
        if (!map_dest(m.dest) || !map_dest_present(p, m)) continue;
        r.claimed[m.controller >> 6] |= (1ull << (m.controller & 63));
    }
    return r;
}

using MidiRoutePublisher = SeqPublisher<MidiRoute>;

// A parsed message into bus records; every channel is heard. A key plays the UI's instrument on a
// window of `voices` tracks from the SONG cursor's, taking the LOWEST free one (all held: the oldest
// is stolen). ⚠️ A note-off follows the KEY across every track — the cursor may have moved.
// CC, program change and pitch bend reach the whole window.
class MidiInputRouter {
  public:
    static constexpr int MAX_EVENTS = POOL_TRACKS;   // at most one record per track

    MidiInputRouter() { release_all_keys(); }

    // The pointer must stay valid between calls; the drain owns the copy.
    void set_route(const MidiRoute* r) { route_ = r; }

    // Route one message at `frame`; returns records written to `out`.
    int route(const MidiInMessage& msg, int64_t frame, Event* out, int maxOut) {
        if (!route_ || !out || maxOut <= 0) return 0;

        if (!msg.is_channel()) { bump(nonChannel_); return 0; }

        int n = 0;

        // ⚠️ The note-off arm first: a velocity-0 note-on must find the track holding its key.
        if (msg.is_note_off()) {
            for (int t = 0; t < POOL_TRACKS && n < maxOut; ++t) {
                if (held_[t] != static_cast<int>(msg.data1) || heldCh_[t] != msg.channel) continue;
                held_[t] = -1;
                if (build(msg, frame, static_cast<uint8_t>(t), INSTRUMENT_NONE, out[n])) { ++n; bump(routed_); }
            }
            return n;
        }

        const int instrument = play_instrument();
        if (instrument < 0) { bump(noInstrument_); return 0; }

        if (msg.is_note_on()) {
            const int target = take_track();
            held_[target]    = static_cast<int>(msg.data1);
            heldCh_[target]  = msg.channel;
            heldIns_[target] = static_cast<int16_t>(instrument);
            taken_[target]   = ++clock_;
            if (build(msg, frame, static_cast<uint8_t>(target), instrument, out[n])) { ++n; bump(routed_); }
            else                                                                     bump(unsupported_);
            return n;
        }

        for (int t = window_begin(); t < window_end() && n < maxOut; ++t) {
            if (build(msg, frame, static_cast<uint8_t>(t), instrument, out[n])) { ++n; bump(routed_); }
            else                                                               bump(unsupported_);
        }
        return n;
    }

    // ⚠️ Call wherever the notes are silenced, or the allocator thinks tracks are busy and steals.
    void release_all_keys() {
        for (int t = 0; t < POOL_TRACKS; ++t) { held_[t] = -1; heldCh_[t] = 0; heldIns_[t] = INSTRUMENT_NONE; }
    }


    // Every path that produces no event counts why (unsupported = aftertouch).
    uint64_t routed() const { return routed_.load(std::memory_order_relaxed); }
    uint64_t nonChannel() const { return nonChannel_.load(std::memory_order_relaxed); }
    uint64_t noInstrument() const { return noInstrument_.load(std::memory_order_relaxed); }
    uint64_t unsupported() const { return unsupported_.load(std::memory_order_relaxed); }


  private:
    static void bump(std::atomic<uint64_t>& c) { c.fetch_add(1, std::memory_order_relaxed); }

    int play_instrument() const {
        const int id = route_->instrument;
        return (id < 0 || id >= route_->instrumentCount) ? -1 : id;
    }

    int window_begin() const { return route_->baseTrack; }
    int window_end() const {
        const int end = route_->baseTrack + route_->voices;
        return end > POOL_TRACKS ? POOL_TRACKS : end;
    }


    int take_track() const {
        int oldest = -1;
        for (int t = window_begin(); t < window_end(); ++t) {
            if (held_[t] < 0) return t;
            if (oldest < 0 || taken_[t] < taken_[oldest]) oldest = t;
        }
        return oldest;
    }

    // False = no bus form (aftertouch).
    bool build(const MidiInMessage& msg, int64_t frame, uint8_t track, int instrument, Event& ev) const {
        ev = Event{};
        ev.frame = frame;
        ev.track = track;

        // ⚠️ The note-off test first, for velocity-0 note-ons.
        if (msg.is_note_off()) {
            ev.instrument   = INSTRUMENT_NONE;      // track-scoped, like every note-off
            ev.type         = EV_NOTE_OFF;
            ev.noteOff.mode = NOTE_OFF_KEY;         // only MIDI in emits it
            return true;
        }

        if (msg.is_note_on()) {
            if (instrument < 0 || instrument >= POOL_INSTRUMENTS) return false;   // already routed
            const MidiRouteInstrument& ins = route_->ins[instrument];
            ev.instrument = static_cast<int16_t>(instrument);
            ev.type       = EV_NOTE_ON;

            NoteOnPayload& n = ev.noteOn;
            n.note = msg.data1;
            // ⚠️ The scheduler's wiring (emit_note): velGain = (v/127)², volGain = instrument volume.
            const uint8_t vel  = route_->velocity ? msg.data2 : 127;
            const float   unit = static_cast<float>(vel) / 127.0f;
            n.velocity    = static_cast<int8_t>(vel);
            n.velGainBits = f32_bits(unit * unit);
            n.volGainBits = f32_bits(ins.volume);
            n.panBits     = f32_bits(ins.pan);
            // No phrase behind it: no FX, no slice, its own table, no transpose.
            n.start = -1; n.slice = -1; n.tableId = -1; n.tableRow = -1;
            n.transpose = 0; n.pit = 0; n.arp = 0;
            n.pslOffBits = n.pslDurBits = n.pbnRateBits = n.vibSpdBits = n.vibDepBits = f32_bits(0.0f);
            return true;
        }

        ev.instrument = INSTRUMENT_NONE;   // everything below is track-scoped

        if (msg.status == EV_CC) {
            ev.type = EV_CC;
            // A literal controller, as `CCA` sends; 0-127 → 0-1, narrowed back by `to7bit`.
            ev.cc.param     = msg.data1;
            ev.cc.valueBits = f32_bits(static_cast<float>(msg.data2) / 127.0f);
            return true;
        }

        if (msg.status == EV_PROGRAM) {
            ev.type = EV_PROGRAM;
            ev.program.program = msg.data1;
            return true;
        }

        if (msg.status == EV_PITCH_BEND) {
            ev.type = EV_PITCH_BEND;
            ev.pitchBend.value14 =
                static_cast<uint16_t>((static_cast<int>(msg.data2) << 7) | static_cast<int>(msg.data1));
            return true;
        }

        return false;   // 0xA0 / 0xD0 pressure
    }

    const MidiRoute* route_ = nullptr;

    // Per track: the key held (−1 none), its channel, its instrument, and when it was taken.
    int      held_[POOL_TRACKS];
    uint8_t  heldCh_[POOL_TRACKS];
    int16_t  heldIns_[POOL_TRACKS];
    uint64_t taken_[POOL_TRACKS] = {};
    uint64_t clock_              = 0;

    std::atomic<uint64_t> routed_{0}, nonChannel_{0}, noInstrument_{0}, unsupported_{0};
};

// ─── What the drain saw, carried back to the UI thread ───────────────────────────────────────────

// Told about every handled message, on the UI thread (it may print or allocate). `count` may be 0:
// the message still arrived.
struct IMidiInObserver {
    virtual ~IMidiInObserver() = default;
    virtual void on_midi_in(const MidiInMessage& msg, const Event* events, int count) = 0;
};

struct MidiInSeen {
    enum Kind : uint8_t {
        ROUTED = 0,   // `events[0..count)` went to the engine; the UI owes thru and bookkeeping
        MAPPED = 1,   // a CC a mapping claims: the UI applies it to the song; nothing routed
        LEARN  = 2,   // a CC on the control channel while R is held: it names, does not drive
    };
    MidiInMessage msg;
    Kind          kind  = ROUTED;
    uint8_t       count = 0;
    Event         events[MidiInputRouter::MAX_EVENTS];
};

// Queue → parser → knob gate → router. `run` is called by the engine each block (or an engine-less
// host from its poll); what it handled goes back to the UI thread through `pop_seen`.
// ⚠️ Real-time safe: lock-free rings, the route copied under a sequence lock, and reset / release as
// REQUESTS honoured at the next run. A CC a mapping claims is withheld from the tracks here.
class MidiInPipeline {
  public:
    struct Apply {
        virtual ~Apply() = default;
        // One record from a live key. `external` = the note-on goes to the cable, not a voice.
        virtual void apply(const Event& ev, bool external) = 0;
    };

    static constexpr int SEEN_CAPACITY = 128;   // messages between two UI polls; a power of two

    // The backend's thread.
    MidiInQueue& sink() { return queue_; }

    // ── the UI thread ──
    void publish_route(const MidiRoute& r) { publisher_.publish(r); }

    // Forget parked bytes, the half message and held keys at the next run — up to a mark taken now,
    // so a newly opened port's first bytes are kept.
    void request_reset() {
        resetMark_.store(queue_.written(), std::memory_order_relaxed);
        requests_.fetch_or(REQ_RESET, std::memory_order_release);
    }
    void request_release_keys() { requests_.fetch_or(REQ_RELEASE, std::memory_order_release); }

    // The next handled message, oldest first.
    bool pop_seen(MidiInSeen& out) {
        const uint32_t tail = seenTail_.load(std::memory_order_acquire);
        const uint32_t head = seenHead_.load(std::memory_order_relaxed);
        if (head == tail) return false;
        out = seen_[head & (SEEN_CAPACITY - 1)];
        seenHead_.store(head + 1, std::memory_order_release);
        return true;
    }


    // ── the consumer ──

    // Drain → parse → gate → route → `apply`, every record at `frame`. `apply` may be null.
    int run(int64_t frame, Apply* apply) {
        service_requests();
        if (publisher_.read(route_, routeSeen_)) router_.set_route(&route_);
        if (!publisher_.published()) { queue_.clear(); return 0; }   // nothing to route against yet

        // The buffer is the ring's whole capacity, so one drain empties it.
        uint8_t buf[MidiInQueue::CAPACITY];
        const int n = queue_.drain(buf, static_cast<int>(sizeof buf));
        if (n <= 0) return 0;
        bytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

        int total = 0;
        for (int i = 0; i < n; ++i) {
            if (!parser_.feed(buf[i])) continue;
            messages_.fetch_add(1, std::memory_order_relaxed);
            const MidiInMessage& m = parser_.message();

            MidiInSeen seen{};
            seen.msg = m;

            // A CC goes to the mappings FIRST and is consumed if claimed; while learn is armed it only
            // names.
            if (m.status == EV_CC && ctl_ch_covers(route_.controlChannel, static_cast<int>(m.channel))) {
                if (route_.learnArmed)        { seen.kind = MidiInSeen::LEARN;  push_seen(seen); continue; }
                if (route_.claims(m.data1))   { seen.kind = MidiInSeen::MAPPED; push_seen(seen); continue; }
            }

            const int k = router_.route(m, frame, seen.events, MidiInputRouter::MAX_EVENTS);
            seen.count = static_cast<uint8_t>(k);
            total += k;
            injected_.fetch_add(static_cast<uint64_t>(k), std::memory_order_relaxed);
            for (int j = 0; j < k; ++j) {
                const Event& ev = seen.events[j];
                const bool external =
                    ev.type == EV_NOTE_ON && ev.instrument >= 0 && ev.instrument < route_.instrumentCount &&
                    route_.ins[ev.instrument].external != 0;
                if (apply) apply->apply(ev, external);
            }
            // ⚠️ Handed back even when `k == 0`: it still arrived.
            push_seen(seen);
        }
        return total;
    }

    // While the engine is not live (an export): bytes are dropped and counted, not saved for a burst.
    void discard() {
        service_requests();
        const int n = queue_.pending();
        if (n > 0) {
            queue_.clear();
            discarded_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        }
    }


    void service_requests() {
        const uint32_t r = requests_.exchange(0, std::memory_order_acquire);
        if (r & REQ_RESET) {
            queue_.discard_up_to(resetMark_.load(std::memory_order_relaxed));
            parser_.reset();
            router_.release_all_keys();
        }
        if (r & REQ_RELEASE) { router_.release_all_keys(); }
    }

    // ── counters, any thread ──
    uint64_t bytes() const { return bytes_.load(std::memory_order_relaxed); }
    uint64_t messages() const { return messages_.load(std::memory_order_relaxed); }
    uint64_t injected() const { return injected_.load(std::memory_order_relaxed); }
    uint64_t discarded() const { return discarded_.load(std::memory_order_relaxed); }
    // Handled messages the UI ring had no room for (thru missed them; the sound did not).
    uint64_t seen_dropped() const { return seenDropped_.load(std::memory_order_relaxed); }

    const MidiInputRouter& router() const { return router_; }
    const MidiParser&      parser() const { return parser_; }
    const MidiInQueue&     queue()  const { return queue_; }

  private:
    static constexpr uint32_t REQ_RESET = 1, REQ_RELEASE = 2;

    void push_seen(const MidiInSeen& s) {
        const uint32_t head = seenHead_.load(std::memory_order_acquire);
        const uint32_t tail = seenTail_.load(std::memory_order_relaxed);
        if (tail - head >= static_cast<uint32_t>(SEEN_CAPACITY)) {
            seenDropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        seen_[tail & (SEEN_CAPACITY - 1)] = s;
        seenTail_.store(tail + 1, std::memory_order_release);
    }

    MidiInQueue        queue_;
    MidiParser         parser_;
    MidiInputRouter    router_;
    MidiRoutePublisher publisher_;
    MidiRoute          route_{};        // the drain's copy
    uint32_t           routeSeen_ = 0;
    std::atomic<uint32_t> requests_{0};
    std::atomic<uint32_t> resetMark_{0};

    // ⚠️ On the heap: inline, the ring is ~83 KB and the host lives on the stack (Windows gives 1 MB).
    std::unique_ptr<MidiInSeen[]> seen_{new MidiInSeen[SEEN_CAPACITY]};
    std::atomic<uint32_t> seenHead_{0};   // the UI's
    std::atomic<uint32_t> seenTail_{0};   // the drain's

    std::atomic<uint64_t> bytes_{0}, messages_{0}, injected_{0}, discarded_{0}, seenDropped_{0};
};

}  // namespace songcore

#endif  // POCKETTRACKER_SONGCORE_MIDI_IN_H
