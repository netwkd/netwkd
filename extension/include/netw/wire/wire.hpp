#pragma once

#include <cstdint>

#include "netw/carrier_buffers.hpp"
#include "netw/carrier_frame.hpp"
#include "netw/datagram_seq_book.hpp"
#include "netw/wire/attribution.hpp"
#include "netw/wire/registry.hpp"
#include "netw/wire/stream.hpp"
#include "netw/wire/traffic.hpp"

namespace netw::wire {

struct Wire {
    struct Stamp {
        uint64_t now_usec = 0;
        int64_t elapsed_ms = 0;
        int64_t base_tick = 0;
    };

    WireRegistry channels = WireRegistry::create_default();
    DatagramSeqBook seq_book;
    NetwCarrierBuffers carrier;
    AttributionBook attribution;
    TrafficMeter traffic;
    WriteStream frame_stream;
    int64_t sent_packets = 0;
    int64_t sent_bytes = 0;
    int64_t received_packets = 0;
    int64_t received_bytes = 0;
    int64_t state_acks_out = 0;
    int64_t state_acks_in = 0;
    int64_t standalone_acks_out = 0;
    int64_t attribution_subject_route = 0;

    void count_sent(int64_t p_bytes);
    void count_received(int64_t p_bytes);
    void count_state_ack_out();
    void count_state_ack_in();
    void count_standalone_ack_out();

    void note_subject(int64_t p_route);
    int64_t take_subject(int64_t p_route);
    void observe_in(
        int64_t p_peer,
        int64_t p_channel,
        int64_t p_route,
        int64_t p_comp,
        int64_t p_bytes
    );

    void advance_traffic(double p_seconds, int64_t p_packet_overhead);

    NetwCarrierDatagram frame_datagram(
        int64_t p_peer,
        const godot::PackedByteArray &p_payload,
        bool p_reliable,
        bool p_carrier,
        const Stamp &p_stamp
#if defined(DEBUG_ENABLED)
        ,
        bool p_observe = false
#endif
    );
    godot::PackedByteArray standalone_ack(
        int64_t p_peer,
        int64_t p_ack,
        const Stamp &p_stamp
    );

    Delivery channel_delivery(
        int64_t p_channel,
        const Delivery *p_requested
    ) const;
};

} // namespace netw::wire
