#include "netw/schema_book.hpp"

#include <cstdint>

#include "godot/object.hpp"

using namespace godot;

namespace netw {

namespace {

const Variant::Type STORAGE_TYPES[SchemaCore::COLUMN_TYPE_COUNT] = {
    Variant::PACKED_FLOAT32_ARRAY,
    Variant::PACKED_FLOAT64_ARRAY,
    Variant::PACKED_INT32_ARRAY,
    Variant::PACKED_INT32_ARRAY,
    Variant::PACKED_INT32_ARRAY,
    Variant::PACKED_INT32_ARRAY,
    Variant::PACKED_INT32_ARRAY,
    Variant::PACKED_INT64_ARRAY,
    Variant::PACKED_BYTE_ARRAY,
    Variant::PACKED_VECTOR2_ARRAY,
    Variant::PACKED_VECTOR3_ARRAY,
    Variant::PACKED_VECTOR4_ARRAY,
    Variant::PACKED_COLOR_ARRAY,
    Variant::PACKED_VECTOR4_ARRAY,
    Variant::PACKED_INT64_ARRAY,
    Variant::ARRAY,
    Variant::PACKED_STRING_ARRAY,
};

const Variant::Type ELEMENT_TYPES[SchemaCore::COLUMN_TYPE_COUNT] = {
    Variant::FLOAT,
    Variant::FLOAT,
    Variant::INT,
    Variant::INT,
    Variant::INT,
    Variant::INT,
    Variant::INT,
    Variant::INT,
    Variant::BOOL,
    Variant::VECTOR2,
    Variant::VECTOR3,
    Variant::VECTOR4,
    Variant::COLOR,
    Variant::QUATERNION,
    Variant::INT,
    Variant::NIL,
    Variant::STRING,
};

bool integer_fits(int type, int64_t value) {
    switch (type) {
        case SchemaCore::I8:
            return value >= -128 && value <= 127;
        case SchemaCore::U8:
            return value >= 0 && value <= 255;
        case SchemaCore::I16:
            return value >= -32768 && value <= 32767;
        case SchemaCore::U16:
            return value >= 0 && value <= 65535;
        case SchemaCore::I32:
            return value >= INT32_MIN && value <= INT32_MAX;
        default:
            return true;
    }
}

Error storable(const Variant &value, int depth, int &r_budget) {
    if (depth > SchemaCore::MAX_VALUE_DEPTH) {
        return ERR_CYCLIC_LINK;
    }
    if (--r_budget < 0) {
        return ERR_OUT_OF_MEMORY;
    }
    switch (value.get_type()) {
        case Variant::OBJECT:
        case Variant::RID:
        case Variant::CALLABLE:
        case Variant::SIGNAL:
            return ERR_INVALID_DATA;
        case Variant::ARRAY: {
            const Array list = value;
            for (int at = 0; at < list.size(); ++at) {
                const Error nested = storable(list[at], depth + 1, r_budget);
                if (nested != OK) {
                    return nested;
                }
            }
            return OK;
        }
        case Variant::DICTIONARY: {
            const Dictionary map = value;
            const Array keys = map.keys();
            for (int at = 0; at < keys.size(); ++at) {
                Error nested = storable(keys[at], depth + 1, r_budget);
                if (nested != OK) {
                    return nested;
                }
                nested = storable(map[keys[at]], depth + 1, r_budget);
                if (nested != OK) {
                    return nested;
                }
            }
            return OK;
        }
        default:
            return OK;
    }
}

Error validate_element(int type, const Variant &value) {
    if (type == SchemaCore::VARIANT) {
        return SchemaBook::validate_storable(value);
    }
    if (type == SchemaCore::STRING) {
        const Variant::Type given = value.get_type();
        const bool text
            = given == Variant::STRING || given == Variant::STRING_NAME;
        return text ? OK : ERR_INVALID_DATA;
    }
    const Variant::Type want
        = static_cast<Variant::Type>(SchemaBook::element_type(type));
    const Variant::Type given = value.get_type();
    if (want == Variant::FLOAT && given == Variant::INT) {
        return OK;
    }
    if (given != want) {
        return ERR_INVALID_DATA;
    }
    if (want == Variant::INT && !integer_fits(type, int64_t(value))) {
        return ERR_PARAMETER_RANGE_ERROR;
    }
    return OK;
}

} // namespace

void SchemaBook::set_column_quantizer(
    const RID &schema,
    int column,
    const Ref<NetwQuantize> &quantizer
) {
    if (assign_quantizer(record_of(schema), column, quantizer)) {
        LocalVector<Ref<NetwQuantize>> *held = quantizers.getptr(schema);
        if (held == nullptr) {
            held = &quantizers.insert(schema, LocalVector<Ref<NetwQuantize>>())
                        ->value;
        }
        if (int(held->size()) <= column) {
            held->resize(uint32_t(column + 1));
        }
        (*held)[uint32_t(column)] = quantizer;
    }
}

void SchemaBook::add_migration(
    const RID &schema,
    int from_version,
    const Callable &step
) {
    SchemaRecord *record = record_of(schema);
    if (record == nullptr || from_version < 1 || step.is_null()) {
        return;
    }
    LocalVector<SchemaMigration> *held = migrations.getptr(schema);
    if (held != nullptr) {
        for (const SchemaMigration &existing : *held) {
            if (existing.from_version != from_version) {
                continue;
            }
            if (record->sealed && existing.step != step) {
                refuse_redeclare(record);
            }
            return;
        }
    }
    if (record->sealed) {
        refuse_redeclare(record);
        return;
    }
    if (held == nullptr) {
        held
            = &migrations.insert(schema, LocalVector<SchemaMigration>())->value;
    }
    SchemaMigration made;
    made.from_version = from_version;
    made.step = step;
    held->push_back(made);
}

Callable SchemaBook::migration_from(const RID &schema, int from_version) const {
    const LocalVector<SchemaMigration> *held = migrations_of(schema);
    if (held == nullptr) {
        return Callable();
    }
    for (const SchemaMigration &step : *held) {
        if (step.from_version == from_version) {
            return step.step;
        }
    }
    return Callable();
}

const LocalVector<SchemaMigration> *SchemaBook::migrations_of(
    const RID &schema
) const {
    return migrations.getptr(schema);
}

Ref<NetwQuantize> SchemaBook::column_quantizer(
    const RID &schema,
    int column
) const {
    const SchemaColumn *found = column_at(record_of(schema), column);
    return found != nullptr ? Ref<NetwQuantize>(quantizer_of(found->quant))
                            : Ref<NetwQuantize>();
}

bool SchemaBook::assign_quantizer(
    SchemaRecord *record,
    int column,
    const Ref<NetwQuantize> &quantizer
) {
    if (record == nullptr || record->sealed) {
        return false;
    }
    SchemaColumn *found = record->at(column);
    if (found == nullptr) {
        return false;
    }
    found->quant = spec_of(quantizer, found->type);
    return true;
}

table::QuantSpec SchemaBook::spec_of(
    const Ref<NetwQuantize> &quantizer,
    int type
) {
    table::QuantSpec spec;
    if (quantizer.is_null()) {
        return spec;
    }
    const Variant::Type element
        = static_cast<Variant::Type>(element_type(type));
    spec.kind = table::Quant::CUSTOM;
    spec.width = quantizer->bit_width(element);
    spec.parts = quantizer->stride(element);
    spec.label = StringName(quantizer->get_class());
    spec.object = uint64_t(quantizer->get_instance_id());
    quantizer->hold();
    const NetwQuantizeScalar *scalar
        = Object::cast_to<NetwQuantizeScalar>(quantizer.ptr());
    if (scalar != nullptr) {
        spec.kind = table::Quant::SCALAR;
        spec.bits = scalar->get_bit_count();
        spec.low = scalar->get_min_limit();
        spec.high = scalar->get_max_limit();
        return spec;
    }
    const NetwQuantizeAngle *angle
        = Object::cast_to<NetwQuantizeAngle>(quantizer.ptr());
    if (angle != nullptr) {
        spec.kind = table::Quant::ANGLE;
        spec.bits = angle->get_bit_count();
        spec.low = angle->get_centered_on_zero() ? -table::ANGLE_PI : 0.0;
        spec.high = spec.low + table::ANGLE_TAU;
        return spec;
    }
    const NetwQuantizeQuaternion *turn
        = Object::cast_to<NetwQuantizeQuaternion>(quantizer.ptr());
    if (turn != nullptr) {
        spec.kind = table::Quant::TURN;
        spec.bits = turn->get_bit_count();
    }
    return spec;
}

NetwQuantize *SchemaBook::quantizer_of(const table::QuantSpec &spec) {
    if (!spec.quantized()) {
        return nullptr;
    }
    return Object::cast_to<NetwQuantize>(gd::object_of(ObjectID(spec.object)));
}

int SchemaBook::type_from_variant(int variant_type) {
    switch (variant_type) {
        case Variant::BOOL:
            return BOOL;
        case Variant::INT:
            return I64;
        case Variant::FLOAT:
            return F64;
        case Variant::VECTOR2:
            return VECTOR2;
        case Variant::VECTOR3:
            return VECTOR3;
        case Variant::VECTOR4:
            return VECTOR4;
        case Variant::COLOR:
            return COLOR;
        case Variant::QUATERNION:
            return QUATERNION;
        case Variant::STRING:
        case Variant::STRING_NAME:
            return STRING;
        default:
            return VARIANT;
    }
}

Variant SchemaBook::make_storage(int type) {
    if (!is_column_type(type)) {
        return Variant();
    }
    switch (STORAGE_TYPES[type]) {
        case Variant::PACKED_FLOAT32_ARRAY:
            return PackedFloat32Array();
        case Variant::PACKED_FLOAT64_ARRAY:
            return PackedFloat64Array();
        case Variant::PACKED_INT32_ARRAY:
            return PackedInt32Array();
        case Variant::PACKED_INT64_ARRAY:
            return PackedInt64Array();
        case Variant::PACKED_BYTE_ARRAY:
            return PackedByteArray();
        case Variant::PACKED_VECTOR2_ARRAY:
            return PackedVector2Array();
        case Variant::PACKED_VECTOR3_ARRAY:
            return PackedVector3Array();
        case Variant::PACKED_VECTOR4_ARRAY:
            return PackedVector4Array();
        case Variant::PACKED_COLOR_ARRAY:
            return PackedColorArray();
        case Variant::PACKED_STRING_ARRAY:
            return PackedStringArray();
        case Variant::ARRAY:
            return Array();
        default:
            return Variant();
    }
}

int SchemaBook::storage_type(int type) {
    return is_column_type(type) ? static_cast<int>(STORAGE_TYPES[type]) : -1;
}

int SchemaBook::element_type(int type) {
    return is_column_type(type) ? static_cast<int>(ELEMENT_TYPES[type]) : -1;
}

Error SchemaBook::validate_storable(const Variant &value) {
    int budget = MAX_VALUE_ELEMENTS;
    return storable(value, 0, budget);
}

Error SchemaBook::validate_value(int type, int stride, const Variant &value) {
    if (!is_column_type(type) || stride < 1) {
        return ERR_INVALID_PARAMETER;
    }
    if (stride == 1) {
        return validate_element(type, value);
    }
    if (value.get_type() != Variant::ARRAY) {
        return ERR_INVALID_DATA;
    }
    const Array elements = value;
    if (elements.size() != stride) {
        return ERR_INVALID_DATA;
    }
    for (int at = 0; at < elements.size(); ++at) {
        const Error nested = validate_element(type, elements[at]);
        if (nested != OK) {
            return nested;
        }
    }
    return OK;
}

} // namespace netw
