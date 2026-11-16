#pragma once

#include <cstdint>

#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/session/frames.hpp"

namespace netw {

class ClockExchange {
public:
    static constexpr uint64_t WAIT_UNIT_USEC = 100;
    static constexpr uint64_t WAIT_LIMIT = 0xFFFF;

private:
    struct Asked {
        uint64_t origin = 0;
        uint64_t heard_usec = 0;
        int64_t since_ms = -1;
    };

    godot::HashMap<int64_t, Asked> asked;
    int64_t ping_since_ms = -1;
    bool ping_owed = false;
    bool ping_forces = false;

public:
    void owe_ping(bool p_forces);
    bool ping_is_owed() const;
    bool ping_waits() const;
    bool ping_boards(int64_t p_now_ms, bool p_datagram_leaves);
    session::ClockPing take_ping(uint64_t p_now_usec);

    void hear_ping(
        int64_t p_peer,
        const session::ClockPing &p_ping,
        uint64_t p_now_usec
    );
    godot::PackedInt64Array peers_asking() const;
    bool pong_boards(int64_t p_peer, int64_t p_now_ms, bool p_datagram_leaves);
    session::ClockPong take_pong(int64_t p_peer, uint64_t p_now_usec);

    static double round_trip_seconds(
        const session::ClockPong &p_pong,
        uint64_t p_now_usec
    );

    void forget_peer(int64_t p_peer);
    void clear();
};

} // namespace netw
