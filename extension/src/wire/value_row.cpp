#include "netw/wire/value_row.hpp"

#include <cstring>

#include "netw/api/quantize.hpp"
#include "netw/call_args.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/schema_book.hpp"
#include "netw/value_variant.hpp"
#include "netw/wire/plan.hpp"

namespace netw::wire {

namespace {

using namespace godot;

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

double component_of(const Variant &p_value, int p_type, int p_element) {
    switch (p_type) {
        case SchemaCore::VECTOR2:
            return double(Vector2(p_value)[p_element]);
        case SchemaCore::VECTOR3:
            return double(Vector3(p_value)[p_element]);
        case SchemaCore::VECTOR4:
            return double(Vector4(p_value)[p_element]);
        case SchemaCore::COLOR:
            return double(Color(p_value)[p_element]);
        case SchemaCore::QUATERNION:
            return double(Quaternion(p_value)[p_element]);
        default:
            return 0.0;
    }
}

uint64_t width_mask(int p_width) {
    return p_width >= 64 ? ~uint64_t(0) : ((uint64_t(1) << p_width) - 1);
}

bool raw_code(
    int p_type,
    int p_width,
    const Variant &p_value,
    int p_element,
    uint64_t &r_code
) {
    if (p_type == SchemaCore::ENTITY) {
        const int64_t route = int64_t(p_value);
        r_code = route > 0 ? (uint64_t(route + 1) & width_mask(p_width)) : 0;
        return true;
    }
    if (integer_column(p_type)) {
        r_code = uint64_t(int64_t(p_value)) & width_mask(p_width);
        return true;
    }
    switch (p_type) {
        case SchemaCore::F32:
            r_code = f32_code(double(p_value));
            return true;
        case SchemaCore::F64:
            r_code = f64_code(double(p_value));
            return true;
        case SchemaCore::BOOL:
            r_code = bool(p_value) ? 1 : 0;
            return true;
        case SchemaCore::VECTOR2:
        case SchemaCore::VECTOR3:
        case SchemaCore::VECTOR4:
        case SchemaCore::COLOR:
        case SchemaCore::QUATERNION:
            r_code = f32_code(component_of(p_value, p_type, p_element));
            return true;
        default:
            return false;
    }
}

bool raw_value(
    int p_type,
    int p_width,
    const uint64_t *p_codes,
    Variant &r_value
) {
    if (p_type == SchemaCore::ENTITY) {
        r_value = p_codes[0] == 0 ? int64_t(0) : int64_t(p_codes[0] - 1);
        return true;
    }
    if (integer_column(p_type)) {
        r_value = signed_integer(p_type) ? sign_extend(p_codes[0], p_width)
                                         : int64_t(p_codes[0]);
        return true;
    }
    switch (p_type) {
        case SchemaCore::F32:
            r_value = f32_value(p_codes[0]);
            return true;
        case SchemaCore::F64:
            r_value = f64_value(p_codes[0]);
            return true;
        case SchemaCore::BOOL:
            r_value = p_codes[0] != 0;
            return true;
        case SchemaCore::VECTOR2:
            r_value = Vector2(f32_value(p_codes[0]), f32_value(p_codes[1]));
            return true;
        case SchemaCore::VECTOR3:
            r_value = Vector3(
                f32_value(p_codes[0]),
                f32_value(p_codes[1]),
                f32_value(p_codes[2])
            );
            return true;
        case SchemaCore::VECTOR4:
            r_value = Vector4(
                f32_value(p_codes[0]),
                f32_value(p_codes[1]),
                f32_value(p_codes[2]),
                f32_value(p_codes[3])
            );
            return true;
        case SchemaCore::COLOR:
            r_value = Color(
                float(f32_value(p_codes[0])),
                float(f32_value(p_codes[1])),
                float(f32_value(p_codes[2])),
                float(f32_value(p_codes[3]))
            );
            return true;
        case SchemaCore::QUATERNION:
            r_value = Quaternion(
                f32_value(p_codes[0]),
                f32_value(p_codes[1]),
                f32_value(p_codes[2]),
                f32_value(p_codes[3])
            );
            return true;
        default:
            return false;
    }
}

} // namespace

bool cell_bytes(
    const ColumnPlan &p_slot,
    const Variant &p_value,
    PackedByteArray &r_bytes
) {
    if (p_slot.text) {
        r_bytes = String(p_value).to_utf8_buffer();
        return true;
    }
    value::Bytes bytes;
    if (value::encode(p_value, bytes) != OK) {
        return false;
    }
    r_bytes.resize(int64_t(bytes.size()));
    memcpy(r_bytes.ptrw(), bytes.ptr(), bytes.size());
    return true;
}

bool cell_value(
    const ColumnPlan &p_slot,
    const PackedByteArray &p_bytes,
    Variant &r_value
) {
    if (p_slot.text) {
        if (!value::is_exact_text(p_bytes.ptr(), p_bytes.size())) {
            return false;
        }
        r_value = gd::utf8_string(p_bytes);
        return true;
    }
    return value::decode(p_bytes.ptr(), p_bytes.size(), r_value)
        == p_bytes.size();
}

bool cell_tail(bool p_text, const Variant &p_value, TailRef &r_tail) {
    if (p_text) {
        const CharString utf8 = String(p_value).utf8();
        if (!value::is_exact_text(
                reinterpret_cast<const uint8_t *>(utf8.get_data()),
                utf8.length()
            )) {
            return false;
        }
        r_tail = TailRef::of(
            reinterpret_cast<const uint8_t *>(utf8.get_data()),
            uint32_t(utf8.length())
        );
        return true;
    }
    return value_tail(p_value, r_tail) == OK;
}

Error value_tail(const Variant &p_value, TailRef &r_tail) {
    r_tail = TailRef();
    if (p_value.get_type() == Variant::NIL) {
        return OK;
    }
    value::Bytes bytes;
    const Error refusal = value::encode(p_value, bytes);
    if (refusal != OK) {
        return refusal;
    }
    if (bytes.size() == 1 && bytes[0] == uint8_t(value::TAG_NIL)) {
        return OK;
    }
    r_tail = TailRef::of(bytes.ptr(), bytes.size());
    return OK;
}

Variant tail_value(bool p_text, const TailRef &p_tail) {
    if (p_text) {
        return gd::utf8_span(p_tail.ptr(), p_tail.size());
    }
    if (p_tail.is_empty()) {
        return Variant();
    }
    Variant value;
    if (value::decode(p_tail.ptr(), p_tail.size(), value)
        != int64_t(p_tail.size())) {
        return Variant();
    }
    return value;
}

void tail_bytes(
    const ColumnPlan &p_slot,
    const TailRef &p_tail,
    PackedByteArray &r_bytes
) {
    if (!p_slot.text && p_tail.is_empty()) {
        r_bytes.resize(1);
        r_bytes.set(0, uint8_t(value::TAG_NIL));
        return;
    }
    r_bytes.resize(int64_t(p_tail.size()));
    if (p_tail.size() > 0) {
        memcpy(r_bytes.ptrw(), p_tail.ptr(), p_tail.size());
    }
}

bool bytes_tail(
    const ColumnPlan &p_slot,
    const PackedByteArray &p_bytes,
    TailRef &r_tail
) {
    if (p_slot.text) {
        if (!value::is_exact_text(p_bytes.ptr(), p_bytes.size())) {
            return false;
        }
    } else {
        Variant checked;
        if (value::decode(p_bytes.ptr(), p_bytes.size(), checked)
            != p_bytes.size()) {
            return false;
        }
        if (p_bytes.size() == 1 && p_bytes[0] == uint8_t(value::TAG_NIL)) {
            r_tail = TailRef();
            return true;
        }
    }
    r_tail = TailRef::of(p_bytes.ptr(), uint32_t(p_bytes.size()));
    return true;
}

bool encode_scalar_column(
    const SchemaColumn &p_column,
    const ColumnPlan &p_slot,
    uint32_t p_index,
    const Variant &p_value,
    CodeRow &r_row,
    NetwMultiplayer *p_session
) {
    const int at = int(p_index);
    const Variant::Type expected
        = Variant::Type(SchemaBook::element_type(p_column.type));
    if (p_slot.variable) {
        const bool textual = p_column.type != SchemaCore::STRING
            || Variant::can_convert_strict(p_value.get_type(), Variant::STRING);
        const String name = vformat("Property '%s'", String(p_column.key));
        TailRef tail;
        if (!p_slot.text) {
            const Error refusal = value_tail(p_value, tail);
            if (refusal != OK) {
                call_args::report_unsendable(p_session, name, p_value, refusal);
            }
        } else if (!textual) {
            call_args::report_unsendable(
                p_session,
                name,
                p_value,
                ERR_INVALID_DATA
            );
        } else if (!cell_tail(true, p_value, tail)) {
            fault::raise(
                p_session,
                NETW_FAULT_UNSENDABLE_VALUE,
                {name,
                 String(
                     "text that opens with a byte order mark or holds a "
                     "NUL"
                 ),
                 String("text without them")}
            );
        }
        r_row.write_tail(p_index, tail);
        return true;
    }
    if (!Variant::can_convert_strict(p_value.get_type(), expected)) {
        fault::raise(
            p_session,
            NETW_FAULT_UNSENDABLE_VALUE,
            {vformat("Column %d", at),
             String("a value of the wrong type"),
             String("the type its schema declares")}
        );
        return false;
    }
    if (p_column.quant.quantized()) {
        const NetwQuantize *quantizer
            = SchemaBook::quantizer_of(p_column.quant);
        if (quantizer == nullptr || !quantizer->supports_type(expected)) {
            fault::raise(
                p_session,
                NETW_FAULT_UNSENDABLE_VALUE,
                {vformat("Column %d", at),
                 String("a quantizer that does not support its type"),
                 String("a quantizer made for that type")}
            );
            return false;
        }
        for (int element = 0; element < p_slot.stride; ++element) {
            const uint64_t code = uint64_t(quantizer->encode(p_value, element));
            if (!r_row.write(p_slot, element, code)) {
                fault::raise(
                    p_session,
                    NETW_FAULT_UNSENDABLE_VALUE,
                    {vformat("Column %d", at),
                     String("a quantizer code with more bits than it declares"),
                     String("a quantizer whose bit count matches its codes")}
                );
                return false;
            }
        }
        return true;
    }
    for (int element = 0; element < p_slot.stride; ++element) {
        uint64_t code = 0;
        NETW_BUG_COND_V(
            !raw_code(p_column.type, p_slot.width, p_value, element, code)
                || !r_row.write(p_slot, element, code),
            false
        );
    }
    return true;
}

bool encode_scalar_row(
    const SchemaRecord &p_schema,
    const Array &p_values,
    CodeRow &r_row
) {
    NETW_ZONE_NC("Wire value row encode", colors::WIRE);
    const WirePlan plan = WirePlan::compile(p_schema);
    NETW_BUG_COND_V(
        !is_scalar_schema(p_schema, plan)
            || p_values.size() != p_schema.column_count(),
        false
    );
    CodeRow staged = CodeRow::for_plan(plan);
    for (int at = 0; at < p_schema.column_count(); ++at) {
        if (!encode_scalar_column(
                *p_schema.at(at),
                plan.column(uint32_t(at)),
                uint32_t(at),
                p_values[at],
                staged
            )) {
            return false;
        }
    }
    r_row = staged;
    NETW_TRACE(
        sys::WIRE,
        "Packed %d values into %d bits.",
        p_values.size(),
        int(plan.row_bits())
    );
    return true;
}

bool decode_scalar_column(
    const SchemaColumn &p_column,
    const ColumnPlan &p_slot,
    uint32_t p_index,
    const CodeRow &p_row,
    PackedInt64Array &r_codes,
    Variant &r_value
) {
    if (p_slot.variable) {
        r_value = tail_value(p_row.is_text(p_index), p_row.tail(p_index));
        return true;
    }
    constexpr int WIDEST = 16;
    if (p_slot.stride < 1 || p_slot.stride > WIDEST) {
        return false;
    }
    uint64_t codes[WIDEST] = {};
    for (int element = 0; element < p_slot.stride; ++element) {
        codes[element] = p_row.read(p_slot, element);
    }
    if (!p_column.quant.quantized()) {
        return raw_value(p_column.type, p_slot.width, codes, r_value);
    }
    const Variant::Type expected
        = Variant::Type(SchemaBook::element_type(p_column.type));
    const NetwQuantize *quantizer = SchemaBook::quantizer_of(p_column.quant);
    if (quantizer == nullptr || !quantizer->supports_type(expected)) {
        return false;
    }
    if (r_codes.size() != int64_t(p_slot.stride)) {
        r_codes.resize(int64_t(p_slot.stride));
    }
    for (int element = 0; element < p_slot.stride; ++element) {
        r_codes.set(int64_t(element), int64_t(codes[element]));
    }
    r_value = quantizer->decode(r_codes, expected);
    return true;
}

bool decode_scalar_row(
    const SchemaRecord &p_schema,
    const CodeRow &p_row,
    Array &r_values
) {
    NETW_ZONE_NC("Wire value row decode", colors::WIRE);
    const WirePlan plan = WirePlan::compile(p_schema);
    if (!is_scalar_schema(p_schema, plan) || !p_row.valid_for(plan)) {
        return false;
    }
    Array staged;
    PackedInt64Array held;
    for (int at = 0; at < p_schema.column_count(); ++at) {
        Variant value;
        if (!decode_scalar_column(
                *p_schema.at(at),
                plan.column(uint32_t(at)),
                uint32_t(at),
                p_row,
                held,
                value
            )) {
            return false;
        }
        staged.push_back(value);
    }
    r_values = staged;
    return true;
}

} // namespace netw::wire
