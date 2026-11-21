#pragma once

#include "godot/local_vector.hpp"
#include "godot/variant.hpp"
#include "netw/table/delta_mode.hpp"
#include "netw/table/quant_spec.hpp"
#include "netw/value.hpp"

namespace netw {
class SchemaCore;
} // namespace netw

namespace netw::table {

struct SchemaColumn {
    godot::StringName key;
    int type = 15;
    int stride = 1;
    DeltaMode delta = DeltaMode::FULL;
    QuantSpec quant;
    uint8_t value_tag = value::TAG_COUNT;
    uint8_t element_tags[2] = {value::TAG_COUNT, value::TAG_COUNT};
};

class SchemaRecord {
    friend class netw::SchemaCore;

    int redeclare_at = 0;
    bool redeclare_open = false;
    bool redeclare_failed = false;

public:
    godot::StringName name;
    godot::LocalVector<SchemaColumn> columns;
    int storage_version = 1;
    bool sealed = false;
    int shape_hash = 0;

    const SchemaColumn *at(int column) const;
    SchemaColumn *at(int column);
    int column_count() const;
};

} // namespace netw::table
