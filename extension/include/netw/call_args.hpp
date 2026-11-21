#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"
#include "godot/variant.hpp"
#include "netw/api/quantize.hpp"
#include "netw/wire/stream.hpp"

namespace netw {

class NetwMultiplayer;

}

namespace netw::call_args {

enum SlotKind {
    KIND_RAW = 0,
    KIND_QUANTIZED = 1,
    KIND_NODE_REF = 2,
};

constexpr int VALUE_CAP = 65535;
constexpr int COUNT_BYTES = 2;

struct NodeRef {
    int64_t route = 0;
    int64_t comp = 0;
    godot::String path;
};

struct Slot {
    godot::Variant value;
    bool addresses_node = false;
    NodeRef node;
};

Slot of_value(const godot::Variant &value);

Slot of_node(int64_t route, int64_t comp, const godot::String &path);

godot::PackedByteArray value_bytes(
    const godot::Variant &value,
    const godot::String &name,
    NetwMultiplayer *session
);

void report_unsendable(
    NetwMultiplayer *session,
    const godot::String &name,
    const godot::Variant &value,
    godot::Error refusal
);

bool write(
    wire::WriteStream &stream,
    const godot::LocalVector<Slot> &slots,
    const godot::Array &quantizers,
    const godot::Array &types,
    NetwMultiplayer *session
);

bool read(
    wire::ReadStream &stream,
    const godot::Array &quantizers,
    const godot::Array &types,
    godot::LocalVector<Slot> &r_slots
);

bool values_write(
    wire::WriteStream &stream,
    const godot::Array &values,
    const godot::Array &quantizers,
    const godot::Array &types,
    NetwMultiplayer *session
);

bool values_read(
    wire::ReadStream &stream,
    const godot::Array &quantizers,
    const godot::Array &types,
    godot::Array &r_values
);

godot::Ref<NetwQuantize> quantizer_at(
    const godot::Array &quantizers,
    int index
);

int type_at(const godot::Array &types, int index);

} // namespace netw::call_args
