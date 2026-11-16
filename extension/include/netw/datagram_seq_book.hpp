#pragma once

#include <cstdint>

#include "godot/rid.hpp"
#include "godot/variant.hpp"

namespace netw {

class DatagramSeqBook {
public:
    static constexpr int SENT_WINDOW = 256;
    static constexpr int64_t UNECHOED_LIMIT = 8;
    static constexpr int64_t SAMPLED_ECHO_PERIOD_MS = 1000;

private:
    struct EchoDebt {
        int64_t since_ms = 0;
        int64_t unechoed = 0;
        bool waited = false;
        bool answer_follows = false;
    };

    struct RoundTrip {
        uint16_t seqs[SENT_WINDOW] = {};
        uint64_t sent_usec[SENT_WINDOW] = {};
        bool stamped[SENT_WINDOW] = {};
        double smoothed_ms = 0.0;
        double jitter_ms = 0.0;
        bool measured = false;
    };

    godot::HashMap<int64_t, uint16_t> send_seqs;
    godot::HashMap<int64_t, RoundTrip> round_trips;
    godot::HashMap<int64_t, uint16_t> inbound_freshest;
    godot::HashMap<int64_t, uint32_t> inbound_history;
    godot::HashMap<int64_t, uint16_t> peer_acks;
    godot::HashMap<int64_t, uint32_t> peer_ack_histories;
    godot::HashMap<int64_t, uint16_t> last_echoed;
    godot::HashMap<int64_t, EchoDebt> echo_debts;
    godot::HashMap<int64_t, int64_t> sampled_echo_ms;
    godot::HashMap<int64_t, int64_t> reorders;
    godot::HashMap<int64_t, int64_t> duplicates;

    void owe_echo(
        int64_t peer,
        uint16_t seq,
        int64_t step,
        int64_t now_ms,
        bool carries_frames
    );

public:
    static bool is_fresher(uint16_t a, uint16_t b);
    static int64_t distance(uint16_t newer, uint16_t older);

    uint16_t next_send_seq(
        int64_t peer,
        uint64_t now_usec,
        bool carries_frames = true
    );

    bool has_inbound(int64_t peer) const;
    uint16_t inbound_seq(int64_t peer) const;
    uint32_t inbound_delivery_history(int64_t peer) const;
    bool note_inbound(
        int64_t peer,
        uint16_t seq,
        int64_t now_ms,
        bool carries_frames = true
    );
    int64_t reorder_count(int64_t peer) const;
    int64_t duplicate_count(int64_t peer) const;

    bool note_peer_ack(
        int64_t peer,
        uint16_t ack,
        uint32_t history,
        uint64_t now_usec,
        bool echo_waited = false
    );
    int64_t peer_ack(int64_t peer) const;
    uint32_t peer_ack_history(int64_t peer) const;
    double round_trip_ms(int64_t peer) const;
    double round_trip_jitter_ms(int64_t peer) const;

    void note_echoed(int64_t peer, uint16_t seq, int64_t now_ms);
    godot::PackedInt64Array peers_owed_echo() const;
    bool echo_is_due(int64_t peer, int64_t now_ms) const;
    bool echo_is_over_limit(int64_t peer) const;
    void await_answer(int64_t peer);
    void note_echo_held(int64_t peer);
    bool echo_waited(int64_t peer) const;

    void forget_peer(int64_t peer);
    void clear();
};

} // namespace netw
