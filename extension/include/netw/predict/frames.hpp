#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"
#include "godot/variant.hpp"
#include "netw/wire/code_row.hpp"
#include "netw/wire/describe.hpp"
#include "netw/wire/plan.hpp"
#include "netw/wire/registry.hpp"

namespace netw::predict {

bool is_lane(uint8_t p_channel);

constexpr uint8_t ACK_SUBSTITUTED = 1U << 0;
constexpr uint8_t ACK_SUPERSEDED = 1U << 1;
constexpr int ACK_WITNESS_SHIFT = 2;
constexpr uint8_t ACK_WITNESS_MASK = 0x1c;
constexpr uint8_t ACK_COMPACT = 1U << 5;
constexpr uint8_t ACK_HELD = 1U << 6;
constexpr int ACK_RECORD_MAX = 21;
constexpr int ACK_FRAME_BYTES_MAX = 1150;
constexpr int COMMAND_TRANSITION_MAX = 255;
constexpr int64_t COMMAND_HOLD_MAX = 1023;

struct TransitionWire {
    int64_t index = 0;
    int64_t label = 0;
    bool fresh = false;
};

struct CommandEvidenceWire {
    uint8_t evidence_mask = 0;
    int32_t pre_fp = 0;
    int32_t post_fp = 0;
    int32_t e_digest = 0;
    int32_t topo_fp = 0;
    int32_t witness_fp = 0;
    int32_t pre_pose_fp = 0;
    int32_t pre_momentum_fp = 0;
    int32_t pre_controller_fp = 0;
    int32_t post_pose_fp = 0;
    int32_t post_momentum_fp = 0;
    int32_t post_controller_fp = 0;
    int32_t raw_fp = 0;
};

struct AckEvidenceWire {
    uint8_t evidence_mask = 0;
    int32_t pre_fp = 0;
    int32_t c_hash = 0;
    int32_t e_digest = 0;
    int32_t post_fp = 0;
    int32_t topo_fp = 0;
    int32_t witness_fp = 0;
    int32_t raw_fp = 0;
    uint8_t flags = 0;
};

struct CommandFrame {
    uint8_t epoch = 0;
    int64_t ack_of_acks = -1;
    bool asks_detail = false;
    int64_t hold = 0;
    godot::LocalVector<TransitionWire> transitions;
    godot::LocalVector<wire::CodeRow> payloads;
    godot::LocalVector<CommandEvidenceWire> evidence;
};

struct AckFrame {
    uint8_t epoch = 0;
    uint64_t base = 0;
    godot::LocalVector<AckEvidenceWire> records;
};

inline uint8_t ack_flags(int p_flags, int p_witness_class) {
    return uint8_t(
        (p_flags & ~int(ACK_WITNESS_MASK))
        | ((p_witness_class << ACK_WITNESS_SHIFT) & int(ACK_WITNESS_MASK))
    );
}

inline int ack_witness_class(uint8_t p_flags) {
    return int((p_flags & ACK_WITNESS_MASK) >> ACK_WITNESS_SHIFT);
}

godot::PackedByteArray encode_command(
    const CommandFrame &p_frame,
    const wire::WirePlan &p_input_plan
);

bool decode_command(
    const godot::PackedByteArray &p_bytes,
    const wire::WirePlan &p_input_plan,
    CommandFrame &r_frame,
    int *r_mistyped = nullptr
);

int ack_transitions_carried(const AckFrame &p_frame);
godot::PackedByteArray encode_ack(const AckFrame &p_frame);
bool decode_ack(const godot::PackedByteArray &p_bytes, AckFrame &r_frame);

godot::PackedByteArray encode_relay_request(bool p_subscribed);
bool decode_relay_request(
    const godot::PackedByteArray &p_bytes,
    bool &r_subscribed
);

struct FrameOrigin {
    int64_t sender = 0;
    int64_t controller = 0;
    int64_t authority = 0;
    bool receiver_is_authority = false;
};

godot::Error admit_frame(
    const wire::WireRegistry &p_registry,
    uint8_t p_channel,
    const FrameOrigin &p_origin,
    bool p_payload_empty,
    godot::Error p_route_verdict
);

godot::Dictionary spec_records();

} // namespace netw::predict
