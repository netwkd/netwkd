#include "netw/sealed.hpp"

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

double TrafficMeter::read(int64_t p_peer, int p_index) const {
    constexpr int CAUSES = CAUSE_COUNT + 1;
    const HashMap<int64_t, Row>::ConstIterator found = rows.find(p_peer);
    if (found == rows.end() || p_index < 0) {
        return 0.0;
    }
    const Rate &rate = found->value.rate;
    if (p_index < CAUSES) {
        return rate.sent[p_index];
    }
    if (p_index < CAUSES * 2) {
        return rate.received[p_index - CAUSES];
    }
    if (p_index == CAUSES * 2) {
        return rate.sent_packets;
    }
    if (p_index == CAUSES * 2 + 1) {
        return rate.received_packets;
    }
    return 0.0;
}

TrafficMeter::Rate TrafficMeter::rate_of(int64_t p_peer) const {
    const HashMap<int64_t, Row>::ConstIterator found = rows.find(p_peer);
    return found != rows.end() ? found->value.rate : Rate();
}

} // namespace netw::wire
