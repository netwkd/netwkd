#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "godot/local_vector.hpp"
#include "netw/wire/attribution.hpp"
#include "netw/wire/registry.hpp"

namespace netw::wire {

class TrafficMeter {
public:
    static constexpr double WINDOW_SECONDS = 1.0;
    static constexpr int HEADERS = CAUSE_COUNT;

    struct Rate {
        double sent[CAUSE_COUNT + 1] = {};
        double received[CAUSE_COUNT + 1] = {};
        double sent_packets = 0.0;
        double received_packets = 0.0;
    };

private:
    struct Row {
        TrafficTotals seen;
        Rate rate;
    };

    godot::HashMap<int64_t, Row> rows;
    godot::LocalVector<int64_t> peers;
    double elapsed = 0.0;
    double window = WINDOW_SECONDS;

    void sample(
        int64_t p_peer,
        const AttributionBook &p_book,
        const WireRegistry &p_channels,
        int64_t p_packet_overhead
    );

public:
    void advance(
        double p_seconds,
        const AttributionBook &p_book,
        const WireRegistry &p_channels,
        int64_t p_packet_overhead
    );
    void set_window(double p_seconds) {
        window = p_seconds;
    }
    void forget_peer(int64_t p_peer);
    double read(int64_t p_peer, int p_index) const;
    Rate rate_of(int64_t p_peer) const;
#if defined(DEBUG_ENABLED)
    static constexpr int FLAT_SIZE = (CAUSE_COUNT + 1) * 2 + 2;
#endif
};

} // namespace netw::wire
