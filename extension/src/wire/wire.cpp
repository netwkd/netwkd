#include "netw/sealed.hpp"

#include "netw/wire/wire.hpp"

namespace netw::wire {

void Wire::count_sent(int64_t p_bytes) {
    sent_packets += 1;
    sent_bytes += p_bytes;
}

void Wire::count_received(int64_t p_bytes) {
    received_packets += 1;
    received_bytes += p_bytes;
}

void Wire::count_state_ack_out() {
    state_acks_out += 1;
}

void Wire::count_state_ack_in() {
    state_acks_in += 1;
}

void Wire::count_standalone_ack_out() {
    state_acks_out += 1;
    standalone_acks_out += 1;
}

void Wire::note_subject(int64_t p_route) {
    attribution_subject_route = p_route;
}

int64_t Wire::take_subject(int64_t p_route) {
    const int64_t subject = p_route != 0 ? p_route : attribution_subject_route;
    attribution_subject_route = 0;
    return subject;
}

void Wire::observe_in(
    int64_t p_peer,
    int64_t p_channel,
    int64_t p_route,
    int64_t p_comp,
    int64_t p_bytes
) {
    Attribution incoming;
    incoming.peer = p_peer;
    incoming.channel = p_channel;
    incoming.route = p_route;
    incoming.comp = p_comp;
    incoming.bits = p_bytes * 8;
    attribution.observe_in(incoming);
}

void Wire::advance_traffic(double p_seconds, int64_t p_packet_overhead) {
    traffic.advance(p_seconds, attribution, channels, p_packet_overhead);
}

NetwCarrierDatagram Wire::frame_datagram(
    int64_t p_peer,
    const godot::PackedByteArray &p_payload,
    bool p_reliable,
    bool p_carrier,
    const Stamp &p_stamp
#if defined(DEBUG_ENABLED)
    ,
    bool p_observe
#endif
) {
    NetwCarrierDatagram out;
    count_sent(p_payload.size());

    int64_t seq = -1;
    int64_t ack = -1;
    uint32_t history = 0;
    bool echo_waited = false;
    if (!p_reliable) {
        seq = seq_book.next_send_seq(p_peer, p_stamp.now_usec);
        if (seq_book.has_inbound(p_peer)) {
            ack = seq_book.inbound_seq(p_peer);
            history = seq_book.inbound_delivery_history(p_peer);
            echo_waited = seq_book.echo_waited(p_peer);
            count_state_ack_out();
            seq_book.note_echoed(p_peer, uint16_t(ack), p_stamp.elapsed_ms);
        }
    }
    attribution.commit(
        p_peer,
        p_reliable,
        p_carrier,
        seq
#if defined(DEBUG_ENABLED)
        ,
        p_observe ? &out.observations : nullptr
#endif
    );
    out.bytes = NetwCarrierFrame::build(
        frame_stream,
        p_payload,
        p_reliable,
        seq,
        ack,
        history,
        p_stamp.base_tick,
        echo_waited
    );
    out.seq = seq;
    return out;
}

godot::PackedByteArray Wire::standalone_ack(
    int64_t p_peer,
    int64_t p_ack,
    const Stamp &p_stamp
) {
    const int64_t seq = seq_book.next_send_seq(p_peer, p_stamp.now_usec, false);
    const godot::PackedByteArray framed = NetwCarrierFrame::build(
        godot::PackedByteArray(),
        false,
        seq,
        p_ack,
        seq_book.inbound_delivery_history(p_peer),
        p_stamp.base_tick,
        seq_book.echo_waited(p_peer)
    );
    seq_book.note_echoed(p_peer, uint16_t(p_ack), p_stamp.elapsed_ms);
    count_standalone_ack_out();
    count_sent(framed.size());
    attribution.note_framing_out(framed.size());
    attribution.note_datagram(false, p_peer, int64_t(framed.size()));
    return framed;
}

Delivery Wire::channel_delivery(
    int64_t p_channel,
    const Delivery *p_requested
) const {
    if (p_channel < 0 || p_channel > 255) {
        return Delivery::IMMEDIATE;
    }
    if (p_requested == nullptr) {
        return channels.delivery_of(uint8_t(p_channel));
    }
    return channels.delivery_of(uint8_t(p_channel), *p_requested);
}

} // namespace netw::wire
