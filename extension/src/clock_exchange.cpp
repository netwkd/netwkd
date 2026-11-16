#include "netw/clock_exchange.hpp"

#include "netw/wire/registry.hpp"

using namespace godot;

namespace netw {

void ClockExchange::owe_ping(bool p_forces) {
    ping_forces = p_forces || (ping_owed && ping_forces);
    ping_owed = true;
}

bool ClockExchange::ping_is_owed() const {
    return ping_owed;
}

bool ClockExchange::ping_waits() const {
    return ping_owed && !ping_forces;
}

bool ClockExchange::ping_boards(int64_t p_now_ms, bool p_datagram_leaves) {
    if (!ping_owed) {
        return false;
    }
    if (ping_forces || p_datagram_leaves) {
        return true;
    }
    if (ping_since_ms < 0) {
        ping_since_ms = p_now_ms;
        return false;
    }
    return p_now_ms < ping_since_ms
        || p_now_ms - ping_since_ms >= wire::RIDE_DEADLINE_MS;
}

session::ClockPing ClockExchange::take_ping(uint64_t p_now_usec) {
    ping_owed = false;
    ping_forces = false;
    ping_since_ms = -1;
    session::ClockPing ping;
    ping.origin = p_now_usec & 0xFFFFFFFF;
    return ping;
}

void ClockExchange::hear_ping(
    int64_t p_peer,
    const session::ClockPing &p_ping,
    uint64_t p_now_usec
) {
    Asked &row = asked[p_peer];
    row.origin = p_ping.origin;
    row.heard_usec = p_now_usec;
}

bool ClockExchange::pong_boards(
    int64_t p_peer,
    int64_t p_now_ms,
    bool p_datagram_leaves
) {
    Asked *row = asked.getptr(p_peer);
    if (row == nullptr) {
        return false;
    }
    if (p_datagram_leaves) {
        return true;
    }
    if (row->since_ms < 0) {
        row->since_ms = p_now_ms;
        return false;
    }
    return p_now_ms < row->since_ms
        || p_now_ms - row->since_ms >= wire::RIDE_DEADLINE_MS;
}

PackedInt64Array ClockExchange::peers_asking() const {
    PackedInt64Array out;
    for (const KeyValue<int64_t, Asked> &row : asked) {
        out.push_back(row.key);
    }
    out.sort();
    return out;
}

session::ClockPong ClockExchange::take_pong(
    int64_t p_peer,
    uint64_t p_now_usec
) {
    session::ClockPong pong;
    const Asked *found = asked.getptr(p_peer);
    if (found == nullptr) {
        return pong;
    }
    const uint64_t waited = p_now_usec > found->heard_usec
        ? (p_now_usec - found->heard_usec + WAIT_UNIT_USEC / 2)
            / WAIT_UNIT_USEC
        : 0;
    pong.origin = found->origin;
    pong.wait = waited < WAIT_LIMIT ? waited : WAIT_LIMIT;
    asked.erase(p_peer);
    return pong;
}

double ClockExchange::round_trip_seconds(
    const session::ClockPong &p_pong,
    uint64_t p_now_usec
) {
    const uint32_t elapsed
        = uint32_t(p_now_usec & 0xFFFFFFFF) - uint32_t(p_pong.origin);
    const uint64_t waited = p_pong.wait * WAIT_UNIT_USEC;
    const uint64_t travelled = elapsed > waited ? elapsed - waited : 0;
    return double(travelled) / 1'000'000.0;
}

void ClockExchange::forget_peer(int64_t p_peer) {
    asked.erase(p_peer);
}

void ClockExchange::clear() {
    asked.clear();
    ping_owed = false;
    ping_forces = false;
    ping_since_ms = -1;
}

} // namespace netw
