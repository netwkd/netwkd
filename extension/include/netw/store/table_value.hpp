#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/variant.hpp"
#include "netw/store/coder.hpp"
#include "netw/store/resident.hpp"
#include "netw/store/table.hpp"
#include "netw/wire/code_row.hpp"

namespace netw {
class NetwMultiplayer;
} // namespace netw

namespace netw::store {

void write_bytes(
    const Table &p_table,
    uint8_t *p_at,
    uint32_t p_column,
    const godot::Variant &p_value
);

godot::Variant read_bytes(
    const Table &p_table,
    const uint8_t *p_at,
    uint32_t p_column
);

void write_value(
    Table &r_table,
    uint32_t p_row,
    uint32_t p_column,
    const godot::Variant &p_value
);

godot::Variant read_value(
    const Table &p_table,
    uint32_t p_row,
    uint32_t p_column
);

godot::Variant history_value(
    const Table &p_table,
    uint32_t p_row,
    uint32_t p_age,
    uint32_t p_column
);

bool encode(
    const Table &p_table,
    uint32_t p_column,
    const godot::Variant &p_value,
    wire::CodeRow &r_row,
    NetwMultiplayer *p_session
);

bool typed_from_variant(
    const ColumnCoder &p_coder,
    const godot::Variant &p_value,
    uint8_t *r_value
);

void resolve_ports(Table &r_table, ResidentRows &r_rows, godot::Object *p_node);

} // namespace netw::store
