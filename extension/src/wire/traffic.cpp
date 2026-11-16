#include "netw/wire/traffic.hpp"

#include <algorithm>

namespace netw::wire {

using namespace godot;

void TrafficMeter::sample(
    int64_t p_peer,
    const AttributionBook &p_book,
    const WireRegistry &p_channels,
    int64_t p_packet_overhead
) {
    const TrafficTotals now = p_book.traffic_totals(p_peer, p_channels);
    Row &row = rows[p_peer];
    const TrafficTotals &was = row.seen;
    Rate &rate = row.rate;

    int64_t framed_out = 0;
    int64_t framed_in = 0;
    for (int cause = 0; cause < CAUSE_COUNT; ++cause) {
        const int64_t sent = now.sent[cause] - was.sent[cause];
        const int64_t received = now.received[cause] - was.received[cause];
        framed_out += sent;
        framed_in += received;
        rate.sent[cause] = double(sent) / elapsed;
        rate.received[cause] = double(received) / elapsed;
    }

    const int64_t packets_out = now.packets_out - was.packets_out;
    const int64_t packets_in = now.packets_in - was.packets_in;
    const int64_t unframed_out = now.wire_out - was.wire_out - framed_out;
    const int64_t unframed_in = now.wire_in - was.wire_in - framed_in;
    const int64_t headers_out = std::max<int64_t>(0, unframed_out)
        + packets_out * p_packet_overhead;
    const int64_t headers_in = std::max<int64_t>(0, unframed_in)
        + packets_in * p_packet_overhead;
    rate.sent[HEADERS] = double(headers_out) / elapsed;
    rate.received[HEADERS] = double(headers_in) / elapsed;
    rate.sent_packets = double(packets_out) / elapsed;
    rate.received_packets = double(packets_in) / elapsed;
    row.seen = now;
}

void TrafficMeter::advance(
    double p_seconds,
    const AttributionBook &p_book,
    const WireRegistry &p_channels,
    int64_t p_packet_overhead
) {
    elapsed += p_seconds;
    if (elapsed < window) {
        return;
    }
    peers.clear();
    p_book.traffic_peers(peers);
    sample(0, p_book, p_channels, p_packet_overhead);
    for (const int64_t peer : peers) {
        sample(peer, p_book, p_channels, p_packet_overhead);
    }
    elapsed = 0.0;
}

void TrafficMeter::forget_peer(int64_t p_peer) {
    rows.erase(p_peer);
}

Dictionary TrafficMeter::read(int64_t p_peer) const {
    static const char *const CAUSES[CAUSE_COUNT + 1]
        = {"state", "input", "calls", "spawns", "upkeep", "headers"};
    const HashMap<int64_t, Row>::ConstIterator found = rows.find(p_peer);
    const Rate rate = found != rows.end() ? found->value.rate : Rate();

    Dictionary sent;
    Dictionary received;
    for (int cause = 0; cause <= CAUSE_COUNT; ++cause) {
        const StringName key(CAUSES[cause]);
        sent[key] = rate.sent[cause];
        received[key] = rate.received[cause];
    }
    Dictionary out;
    out[StringName("sent")] = sent;
    out[StringName("received")] = received;
    out[StringName("sent_packets")] = rate.sent_packets;
    out[StringName("received_packets")] = rate.received_packets;
    return out;
}

#if defined(DEBUG_ENABLED)
void TrafficMeter::read_flat(int64_t p_peer, Array &r_out) const {
    const HashMap<int64_t, Row>::ConstIterator found = rows.find(p_peer);
    const Rate rate = found != rows.end() ? found->value.rate : Rate();

    r_out.resize(FLAT_SIZE);
    for (int cause = 0; cause <= CAUSE_COUNT; ++cause) {
        r_out[cause] = rate.sent[cause];
        r_out[CAUSE_COUNT + 1 + cause] = rate.received[cause];
    }
    r_out[FLAT_SIZE - 2] = rate.sent_packets;
    r_out[FLAT_SIZE - 1] = rate.received_packets;
}
#endif

} // namespace netw::wire
