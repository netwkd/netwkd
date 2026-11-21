#pragma once

#include "godot/variant.hpp"
#include "netw/api/schema_core.hpp"
#include "netw/wire/code_row.hpp"
#include "netw/wire/plan.hpp"

namespace netw {
class NetwMultiplayer;
} // namespace netw

namespace netw::wire {

using netw::table::SchemaRecord;

bool cell_bytes(
    const ColumnPlan &p_slot,
    const godot::Variant &p_value,
    godot::PackedByteArray &r_bytes
);

bool cell_value(
    const ColumnPlan &p_slot,
    const godot::PackedByteArray &p_bytes,
    godot::Variant &r_value
);

bool cell_tail(bool p_text, const godot::Variant &p_value, TailRef &r_tail);

godot::Error value_tail(const godot::Variant &p_value, TailRef &r_tail);

godot::Variant tail_value(bool p_text, const TailRef &p_tail);

void tail_bytes(
    const ColumnPlan &p_slot,
    const TailRef &p_tail,
    godot::PackedByteArray &r_bytes
);

bool bytes_tail(
    const ColumnPlan &p_slot,
    const godot::PackedByteArray &p_bytes,
    TailRef &r_tail
);

template <class Stream>
bool carry_tail(Stream &p_stream, const ColumnPlan &p_slot, TailRef &r_tail) {
    godot::PackedByteArray encoded;
    if constexpr (!Stream::is_reading) {
        tail_bytes(p_slot, r_tail, encoded);
    }
    if (!p_stream.bytes_capped(encoded, VARIABLE_BYTES_CAP)) {
        return false;
    }
    if constexpr (Stream::is_reading) {
        return bytes_tail(p_slot, encoded, r_tail);
    }
    return true;
}

bool encode_scalar_column(
    const netw::table::SchemaColumn &p_column,
    const ColumnPlan &p_slot,
    uint32_t p_index,
    const godot::Variant &p_value,
    CodeRow &r_row,
    NetwMultiplayer *p_session = nullptr
);

bool encode_scalar_row(
    const SchemaRecord &p_schema,
    const godot::Array &p_values,
    CodeRow &r_row
);

bool decode_scalar_column(
    const netw::table::SchemaColumn &p_column,
    const ColumnPlan &p_slot,
    uint32_t p_index,
    const CodeRow &p_row,
    godot::PackedInt64Array &r_codes,
    godot::Variant &r_value
);

bool decode_scalar_row(
    const SchemaRecord &p_schema,
    const CodeRow &p_row,
    godot::Array &r_values
);

} // namespace netw::wire
