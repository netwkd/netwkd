#pragma once

#include <cstdint>

#include "godot/variant.hpp"
#include "netw/value.hpp"

namespace netw::value {

godot::Error encode(const godot::Variant &p_value, Bytes &r_bytes);

int64_t decode(const uint8_t *p_bytes, int64_t p_size, godot::Variant &r_value);

godot::Error encode_packed(
    const godot::Variant &p_value,
    godot::PackedByteArray &r_bytes
);

bool decode_packed(
    const godot::PackedByteArray &p_bytes,
    godot::Variant &r_value
);

uint32_t hash_variant(uint32_t p_hash, const godot::Variant &p_value);

Tag kind_of_type(int p_type);

} // namespace netw::value
