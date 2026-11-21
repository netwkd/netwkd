#include "netw/sealed.hpp"

#include "netw/store/coder.hpp"

#include <cstring>

#include "netw/schema_core.hpp"

using namespace godot;

namespace netw::store {

namespace {

template <typename T> void put(uint8_t *p_at, const T &p_value) {
    memcpy(p_at, &p_value, sizeof(T));
}

template <typename T> T take(const uint8_t *p_at) {
    T value;
    memcpy(&value, p_at, sizeof(T));
    return value;
}

uint64_t low_mask(int p_width) {
    return p_width >= 64 ? ~uint64_t(0) : ((uint64_t(1) << p_width) - 1);
}

uint64_t f32_code(double p_value) {
    const float narrowed = float(p_value);
    uint32_t bits = 0;
    memcpy(&bits, &narrowed, sizeof(bits));
    return uint64_t(bits);
}

double f32_value(uint64_t p_code) {
    const uint32_t bits = uint32_t(p_code);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t f64_code(double p_value) {
    uint64_t bits = 0;
    memcpy(&bits, &p_value, sizeof(bits));
    return bits;
}

double f64_value(uint64_t p_code) {
    double value = 0.0;
    memcpy(&value, &p_code, sizeof(value));
    return value;
}

bool signed_integer(int p_type) {
    return p_type == SchemaCore::I8 || p_type == SchemaCore::I16
        || p_type == SchemaCore::I32 || p_type == SchemaCore::I64;
}

bool integer_column(int p_type) {
    return signed_integer(p_type) || p_type == SchemaCore::U8
        || p_type == SchemaCore::U16;
}

int64_t sign_extend(uint64_t p_value, int p_width) {
    if (p_width >= 64) {
        return int64_t(p_value);
    }
    const uint64_t sign = uint64_t(1) << (p_width - 1);
    return int64_t((p_value ^ sign) - sign);
}

int components_of(int p_type) {
    switch (p_type) {
        case SchemaCore::VECTOR2:
            return 2;
        case SchemaCore::VECTOR3:
            return 3;
        case SchemaCore::VECTOR4:
        case SchemaCore::COLOR:
        case SchemaCore::QUATERNION:
            return 4;
        case SchemaCore::VARIANT:
        case SchemaCore::STRING:
            return 0;
        default:
            return 1;
    }
}

bool floating(int p_type) {
    return p_type == SchemaCore::F32 || p_type == SchemaCore::F64;
}

double component(int p_type, const uint8_t *p_value, int p_element) {
    switch (p_type) {
        case SchemaCore::VECTOR2:
            return double(take<Vector2>(p_value)[p_element]);
        case SchemaCore::VECTOR3:
            return double(take<Vector3>(p_value)[p_element]);
        case SchemaCore::VECTOR4:
            return double(take<Vector4>(p_value)[p_element]);
        case SchemaCore::COLOR:
            return double(take<Color>(p_value)[p_element]);
        case SchemaCore::QUATERNION:
            return double(take<Quaternion>(p_value)[p_element]);
        default:
            return take<double>(p_value);
    }
}

uint64_t raw_code(
    const ColumnCoder &p_coder,
    const uint8_t *p_value,
    int p_at
) {
    const int type = p_coder.type;
    const int width = p_coder.slot.width;
    if (type == SchemaCore::ENTITY) {
        const int64_t route = take<int64_t>(p_value);
        return route > 0 ? (uint64_t(route + 1) & low_mask(width)) : 0;
    }
    if (integer_column(type)) {
        return uint64_t(take<int64_t>(p_value)) & low_mask(width);
    }
    switch (type) {
        case SchemaCore::F64:
            return f64_code(take<double>(p_value));
        case SchemaCore::BOOL:
            return *p_value != 0 ? 1 : 0;
        default:
            return f32_code(component(type, p_value, p_at));
    }
}

void raw_canonical(
    const ColumnCoder &p_coder,
    const uint64_t *p_codes,
    uint8_t *r_value
) {
    const int type = p_coder.type;
    if (type == SchemaCore::ENTITY) {
        put(r_value, p_codes[0] == 0 ? int64_t(0) : int64_t(p_codes[0] - 1));
        return;
    }
    if (integer_column(type)) {
        put(r_value,
            signed_integer(type) ? sign_extend(p_codes[0], p_coder.slot.width)
                                 : int64_t(p_codes[0]));
        return;
    }
    switch (type) {
        case SchemaCore::F32:
            put(r_value, f32_value(p_codes[0]));
            return;
        case SchemaCore::F64:
            put(r_value, f64_value(p_codes[0]));
            return;
        case SchemaCore::BOOL:
            *r_value = p_codes[0] != 0 ? 1 : 0;
            return;
        case SchemaCore::VECTOR2:
            put(r_value, Vector2(f32_value(p_codes[0]), f32_value(p_codes[1])));
            return;
        case SchemaCore::VECTOR3:
            put(r_value,
                Vector3(
                    f32_value(p_codes[0]),
                    f32_value(p_codes[1]),
                    f32_value(p_codes[2])
                ));
            return;
        case SchemaCore::VECTOR4:
            put(r_value,
                Vector4(
                    f32_value(p_codes[0]),
                    f32_value(p_codes[1]),
                    f32_value(p_codes[2]),
                    f32_value(p_codes[3])
                ));
            return;
        case SchemaCore::COLOR:
            put(r_value,
                Color(
                    float(f32_value(p_codes[0])),
                    float(f32_value(p_codes[1])),
                    float(f32_value(p_codes[2])),
                    float(f32_value(p_codes[3]))
                ));
            return;
        default:
            put(r_value,
                Quaternion(
                    f32_value(p_codes[0]),
                    f32_value(p_codes[1]),
                    f32_value(p_codes[2]),
                    f32_value(p_codes[3])
                ));
            return;
    }
}

double axis_value(const ColumnCoder &p_coder, uint64_t p_code) {
    return table::scalar_value(
        p_coder.bits,
        p_coder.low,
        p_coder.high,
        int64_t(p_code)
    );
}

} // namespace

ColumnCoder coder_of(
    const netw::table::SchemaColumn &p_column,
    const wire::ColumnPlan &p_slot
) {
    ColumnCoder out;
    out.type = p_column.type;
    out.slot = p_slot;
    const int components = components_of(p_column.type);
    if (p_slot.variable || p_column.stride != 1 || components == 0
        || p_slot.width <= 0 || p_slot.width > 64) {
        return out;
    }
    const table::QuantSpec &spec = p_column.quant;
    if (!spec.quantized()) {
        if (p_slot.stride == components) {
            out.kind = Coder::RAW;
        }
        return out;
    }
    out.bits = spec.bits;
    out.low = spec.low;
    out.high = spec.high;
    const bool axes = floating(p_column.type)
        || p_column.type == SchemaCore::VECTOR2
        || p_column.type == SchemaCore::VECTOR3;
    if (spec.kind == table::Quant::SCALAR && axes
        && p_slot.stride == components) {
        out.kind = Coder::SCALAR;
        return out;
    }
    if (spec.kind == table::Quant::ANGLE && floating(p_column.type)
        && p_slot.stride == 1) {
        out.kind = Coder::ANGLE;
        return out;
    }
    if (spec.kind == table::Quant::TURN
        && p_column.type == SchemaCore::QUATERNION && p_slot.stride == 1) {
        out.kind = Coder::TURN;
    }
    return out;
}

bool code_value(
    const ColumnCoder &p_coder,
    const uint8_t *p_value,
    uint64_t *r_codes
) {
    const uint64_t mask = low_mask(p_coder.slot.width);
    for (int at = 0; at < p_coder.slot.stride; ++at) {
        uint64_t code = 0;
        switch (p_coder.kind) {
            case Coder::RAW:
                code = raw_code(p_coder, p_value, at);
                break;
            case Coder::SCALAR:
                code = uint64_t(
                    table::scalar_code(
                        p_coder.bits,
                        p_coder.low,
                        p_coder.high,
                        component(p_coder.type, p_value, at)
                    )
                );
                break;
            case Coder::ANGLE:
                code = uint64_t(
                    table::angle_code(p_coder.bits, take<double>(p_value))
                );
                break;
            case Coder::TURN:
                code = uint64_t(
                    table::turn_code(p_coder.bits, take<Quaternion>(p_value))
                );
                break;
            case Coder::VARIANT:
                return false;
        }
        if ((code & ~mask) != 0) {
            return false;
        }
        r_codes[at] = code;
    }
    return true;
}

void canonical_value(
    const ColumnCoder &p_coder,
    const uint64_t *p_codes,
    uint8_t *r_value
) {
    switch (p_coder.kind) {
        case Coder::RAW:
            raw_canonical(p_coder, p_codes, r_value);
            return;
        case Coder::SCALAR: {
            if (p_coder.type == SchemaCore::VECTOR3) {
                put(r_value,
                    Vector3(
                        axis_value(p_coder, p_codes[0]),
                        axis_value(p_coder, p_codes[1]),
                        axis_value(p_coder, p_codes[2])
                    ));
                return;
            }
            if (p_coder.type == SchemaCore::VECTOR2) {
                put(r_value,
                    Vector2(
                        axis_value(p_coder, p_codes[0]),
                        axis_value(p_coder, p_codes[1])
                    ));
                return;
            }
            put(r_value, axis_value(p_coder, p_codes[0]));
            return;
        }
        case Coder::ANGLE:
            put(r_value,
                table::angle_value(
                    p_coder.bits,
                    p_coder.low < 0.0,
                    int64_t(p_codes[0])
                ));
            return;
        case Coder::TURN:
            put(r_value, table::turn_value(p_coder.bits, int64_t(p_codes[0])));
            return;
        case Coder::VARIANT:
            return;
    }
}

uint64_t read_code(
    const uint64_t *p_words,
    const wire::ColumnPlan &p_slot,
    int p_element
) {
    const int width = p_slot.width;
    const int64_t offset = p_slot.offset + int64_t(p_element) * width;
    const int64_t word = offset >> 6;
    const int within = int(offset & 63);
    uint64_t value = p_words[word] >> within;
    const int taken = 64 - within;
    if (taken < width) {
        value |= p_words[word + 1] << taken;
    }
    return value & low_mask(width);
}

void write_code(
    uint64_t *r_words,
    const wire::ColumnPlan &p_slot,
    int p_element,
    uint64_t p_code
) {
    const int width = p_slot.width;
    const int64_t offset = p_slot.offset + int64_t(p_element) * width;
    const int64_t word = offset >> 6;
    const int within = int(offset & 63);
    const uint64_t mask = low_mask(width);
    r_words[word] &= ~(mask << within);
    r_words[word] |= (p_code & mask) << within;
    const int taken = 64 - within;
    if (taken < width) {
        r_words[word + 1] &= ~(mask >> taken);
        r_words[word + 1] |= (p_code & mask) >> taken;
    }
}

} // namespace netw::store
