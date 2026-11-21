#include "netw/store/table_value.hpp"

#include <cstring>

#include "godot/physics_body.hpp"
#include "godot/script.hpp"
#include "netw/schema_book.hpp"
#include "netw/store/port.hpp"
#include "netw/wire/value_row.hpp"

using namespace godot;

namespace netw::store {

namespace {

BodyKey body_key_3d(const StringName &p_key) {
    static const StringName position("position");
    static const StringName quaternion("quaternion");
    static const StringName linear("linear_velocity");
    static const StringName angular("angular_velocity");
    static const StringName sleeping("sleeping");
    if (p_key == position) {
        return BodyKey::POSITION;
    }
    if (p_key == quaternion) {
        return BodyKey::TURN;
    }
    if (p_key == linear) {
        return BodyKey::LINEAR;
    }
    if (p_key == angular) {
        return BodyKey::ANGULAR;
    }
    if (p_key == sleeping) {
        return BodyKey::SLEEPING;
    }
    return BodyKey::NONE;
}

BodyKey body_key_2d(const StringName &p_key) {
    static const StringName rotation("rotation");
    static const StringName quaternion("quaternion");
    if (p_key == rotation) {
        return BodyKey::TURN;
    }
    if (p_key == quaternion) {
        return BodyKey::NONE;
    }
    return body_key_3d(p_key);
}

bool script_declares(const Ref<Script> &p_script, const StringName &p_key) {
    const Array declared = gd::script_property_list(p_script);
    for (int at = 0; at < declared.size(); ++at) {
        const Dictionary info = declared[at];
        if (StringName(info.get("name", StringName())) == p_key) {
            return true;
        }
    }
    return false;
}

template <typename T> void put(uint8_t *p_at, const T &p_value) {
    memcpy(p_at, &p_value, sizeof(T));
}

template <typename T> T take(const uint8_t *p_at) {
    T value;
    memcpy(&value, p_at, sizeof(T));
    return value;
}

} // namespace

void resolve_columns(
    Object *p_node,
    const LocalVector<StringName> &p_keys,
    LocalVector<Column> &r_columns
) {
    r_columns.resize(p_keys.size());
    const bool solid = Object::cast_to<RigidBody3D>(p_node) != nullptr;
    const bool flat = Object::cast_to<RigidBody2D>(p_node) != nullptr;
    const Ref<Script> script
        = p_node != nullptr ? Ref<Script>(p_node->get_script()) : Ref<Script>();
    for (uint32_t at = 0; at < p_keys.size(); ++at) {
        const StringName &key = p_keys[at];
        const BodyKey body = solid ? body_key_3d(key)
            : flat                 ? body_key_2d(key)
                                   : BodyKey::NONE;
        r_columns[at].body = body;
        if (body != BodyKey::NONE) {
            r_columns[at].port = Port::BODY;
        } else if (gd::indexed_path(key) != nullptr) {
            r_columns[at].port = Port::INDEXED;
        } else if (script.is_valid() && script_declares(script, key)) {
            r_columns[at].port = Port::SCRIPT;
        } else {
            r_columns[at].port = Port::ENGINE;
        }
    }
}

void resolve_ports(Table &r_table, ResidentRows &r_rows, Object *p_node) {
    if (r_rows.ports_resolved || p_node == nullptr) {
        return;
    }
    r_rows.ports_resolved = true;
    LocalVector<StringName> keys;
    for (uint32_t at = 0; at < r_table.columns.size(); ++at) {
        keys.push_back(r_table.schema.at(int(at))->key);
    }
    LocalVector<Column> resolved;
    resolve_columns(p_node, keys, resolved);
    for (uint32_t at = 0; at < r_table.columns.size(); ++at) {
        r_table.columns[at].port = resolved[at].port;
        r_table.columns[at].body = resolved[at].body;
        r_table.body_columns += resolved[at].port == Port::BODY ? 1 : 0;
        r_table.indexed_columns += resolved[at].port == Port::INDEXED ? 1 : 0;
    }
}

void write_value(
    Table &r_table,
    uint32_t p_row,
    uint32_t p_column,
    const Variant &p_value
) {
    const uint32_t size = r_table.columns[p_column].value_bytes;
    if (size == 0) {
        return;
    }
    write_bytes(
        r_table,
        r_table.values[p_column].ptr() + uint64_t(p_row) * size,
        p_column,
        p_value
    );
}

void write_bytes(
    const Table &p_table,
    uint8_t *p_at,
    uint32_t p_column,
    const Variant &p_value
) {
    uint8_t *at = p_at;
    switch (p_table.schema.at(int(p_column))->type) {
        case SchemaCore::BOOL:
            *at = bool(p_value) ? 1 : 0;
            return;
        case SchemaCore::F32:
        case SchemaCore::F64:
            put(at, double(p_value));
            return;
        case SchemaCore::VECTOR2:
            put(at, Vector2(p_value));
            return;
        case SchemaCore::VECTOR3:
            put(at, Vector3(p_value));
            return;
        case SchemaCore::VECTOR4:
            put(at, Vector4(p_value));
            return;
        case SchemaCore::COLOR:
            put(at, Color(p_value));
            return;
        case SchemaCore::QUATERNION:
            put(at, Quaternion(p_value));
            return;
        default:
            put(at, int64_t(p_value));
            return;
    }
}

Variant read_value(const Table &p_table, uint32_t p_row, uint32_t p_column) {
    const uint32_t size = p_table.columns[p_column].value_bytes;
    if (size == 0) {
        return p_table.tail_columns == 0
            ? Variant()
            : wire::tail_value(
                  p_table.plan.column(p_column).text,
                  p_table.cell_tails[p_row * p_table.tail_columns + p_column]
              );
    }
    return read_bytes(
        p_table,
        p_table.values[p_column].ptr() + uint64_t(p_row) * size,
        p_column
    );
}

Variant read_bytes(
    const Table &p_table,
    const uint8_t *p_at,
    uint32_t p_column
) {
    const uint8_t *at = p_at;
    switch (p_table.schema.at(int(p_column))->type) {
        case SchemaCore::BOOL:
            return *at != 0;
        case SchemaCore::F32:
        case SchemaCore::F64:
            return take<double>(at);
        case SchemaCore::VECTOR2:
            return take<Vector2>(at);
        case SchemaCore::VECTOR3:
            return take<Vector3>(at);
        case SchemaCore::VECTOR4:
            return take<Vector4>(at);
        case SchemaCore::COLOR:
            return take<Color>(at);
        case SchemaCore::QUATERNION:
            return take<Quaternion>(at);
        default:
            return take<int64_t>(at);
    }
}

bool encode(
    const Table &p_table,
    uint32_t p_column,
    const Variant &p_value,
    wire::CodeRow &r_row,
    NetwMultiplayer *p_session
) {
    return wire::encode_scalar_column(
        *p_table.schema.at(int(p_column)),
        p_table.plan.column(p_column),
        p_column,
        p_value,
        r_row,
        p_session
    );
}

Variant history_value(
    const Table &p_table,
    uint32_t p_row,
    uint32_t p_age,
    uint32_t p_column
) {
    if (p_age >= p_table.history_count(p_row)
        || p_column >= p_table.columns.size()) {
        return Variant();
    }
    const uint32_t sample = p_table.history_slot(p_row, p_age);
    if (p_table.columns[p_column].value_bytes == 0) {
        return p_table.tail_columns == 0
            ? Variant()
            : wire::tail_value(
                  p_table.plan.column(p_column).text,
                  p_table
                      .history_tails[sample * p_table.tail_columns + p_column]
              );
    }
    return read_bytes(
        p_table,
        p_table.history_values.ptr() + uint64_t(sample) * p_table.row_bytes
            + p_table.value_offsets[p_column],
        p_column
    );
}

bool decode_coded(
    Table &r_table,
    uint32_t p_column,
    const wire::CodeRow &p_codes,
    uint8_t *r_value
) {
    Variant value;
    if (!wire::decode_scalar_column(
            *r_table.schema.at(int(p_column)),
            r_table.plan.column(p_column),
            p_column,
            p_codes,
            r_table.decode_codes[p_column],
            value
        )) {
        return false;
    }
    write_bytes(r_table, r_value, p_column, value);
    return true;
}

bool typed_from_variant(
    const ColumnCoder &p_coder,
    const Variant &p_value,
    uint8_t *r_value
) {
    const Variant::Type expected
        = Variant::Type(SchemaBook::element_type(p_coder.type));
    if (!Variant::can_convert_strict(p_value.get_type(), expected)) {
        return false;
    }
    switch (p_coder.type) {
        case SchemaCore::BOOL:
            *r_value = bool(p_value) ? 1 : 0;
            return true;
        case SchemaCore::F32:
        case SchemaCore::F64:
            put(r_value, double(p_value));
            return true;
        case SchemaCore::VECTOR2:
            put(r_value, Vector2(p_value));
            return true;
        case SchemaCore::VECTOR3:
            put(r_value, Vector3(p_value));
            return true;
        case SchemaCore::VECTOR4:
            put(r_value, Vector4(p_value));
            return true;
        case SchemaCore::COLOR:
            put(r_value, Color(p_value));
            return true;
        case SchemaCore::QUATERNION:
            put(r_value, Quaternion(p_value));
            return true;
        default:
            put(r_value, int64_t(p_value));
            return true;
    }
}

} // namespace netw::store
