#include "netw/value_variant.hpp"

#include <cstring>
#include <initializer_list>

#include "godot/object.hpp"
#include "netw/schema_core.hpp"

using namespace godot;

namespace netw::value {

static_assert(MAX_DEPTH == SchemaCore::MAX_VALUE_DEPTH);
static_assert(MAX_ELEMENTS == SchemaCore::MAX_VALUE_ELEMENTS);

namespace {

const Variant::Type TYPES[TAG_COUNT] = {
    Variant::NIL,
    Variant::BOOL,
    Variant::BOOL,
    Variant::INT,
    Variant::FLOAT,
    Variant::FLOAT,
    Variant::STRING,
    Variant::STRING_NAME,
    Variant::NODE_PATH,
    Variant::VECTOR2,
    Variant::VECTOR2I,
    Variant::VECTOR3,
    Variant::VECTOR3I,
    Variant::VECTOR4,
    Variant::VECTOR4I,
    Variant::QUATERNION,
    Variant::COLOR,
    Variant::RECT2,
    Variant::RECT2I,
    Variant::PLANE,
    Variant::AABB,
    Variant::BASIS,
    Variant::TRANSFORM2D,
    Variant::TRANSFORM3D,
    Variant::PROJECTION,
    Variant::ARRAY,
    Variant::ARRAY,
    Variant::DICTIONARY,
    Variant::DICTIONARY,
    Variant::PACKED_BYTE_ARRAY,
    Variant::PACKED_INT32_ARRAY,
    Variant::PACKED_INT64_ARRAY,
    Variant::PACKED_FLOAT32_ARRAY,
    Variant::PACKED_FLOAT64_ARRAY,
    Variant::PACKED_STRING_ARRAY,
    Variant::PACKED_VECTOR2_ARRAY,
    Variant::PACKED_VECTOR3_ARRAY,
    Variant::PACKED_COLOR_ARRAY,
    Variant::PACKED_VECTOR4_ARRAY,
};

Tag tag_of(Variant::Type p_type) {
    for (int tag = 0; tag < TAG_COUNT; ++tag) {
        if (TYPES[tag] == p_type) {
            return Tag(tag);
        }
    }
    return TAG_COUNT;
}

bool exact(const CharString &p_utf8) {
    return is_exact_text(
        reinterpret_cast<const uint8_t *>(p_utf8.get_data()),
        p_utf8.length()
    );
}

bool append_text(Bytes &r_out, Tag p_tag, const String &p_text) {
    const CharString utf8 = p_text.utf8();
    if (!exact(utf8)) {
        return false;
    }
    write_text(r_out, p_tag, utf8.get_data(), utf8.length());
    return true;
}

void append_reals(
    Bytes &r_out,
    Tag p_tag,
    std::initializer_list<double> p_parts
) {
    float narrowed[16];
    int count = 0;
    for (const double part : p_parts) {
        narrowed[count++] = float(part);
    }
    write_reals(r_out, p_tag, narrowed, count);
}

void append_ints(
    Bytes &r_out,
    Tag p_tag,
    std::initializer_list<int32_t> p_parts
) {
    write_ints(r_out, p_tag, p_parts.begin(), int(p_parts.size()));
}

template <class Packed, class Each>
void append_packed(Bytes &r_out, Tag p_tag, const Packed &p_held, Each p_each) {
    write_tag(r_out, p_tag);
    write_varuint(r_out, uint64_t(p_held.size()));
    const auto *elements = p_held.ptr();
    for (int64_t at = 0; at < p_held.size(); ++at) {
        p_each(elements[at]);
    }
}

Error append(const Variant &p_value, int p_depth, int &r_budget, Bytes &r_out) {
    if (p_depth > MAX_DEPTH) {
        return ERR_CYCLIC_LINK;
    }
    if (--r_budget < 0) {
        return ERR_OUT_OF_MEMORY;
    }
    switch (p_value.get_type()) {
        case Variant::NIL:
            write_nil(r_out);
            return OK;
        case Variant::OBJECT:
            if (gd::live_object(p_value) != nullptr) {
                return ERR_INVALID_DATA;
            }
            write_nil(r_out);
            return OK;
        case Variant::BOOL:
            write_bool(r_out, bool(p_value));
            return OK;
        case Variant::INT:
            write_int(r_out, int64_t(p_value));
            return OK;
        case Variant::FLOAT:
            write_float(r_out, double(p_value));
            return OK;
        case Variant::STRING:
        case Variant::STRING_NAME:
        case Variant::NODE_PATH: {
            const String held = p_value;
            if (p_value.get_type() == Variant::NODE_PATH) {
                const CharString utf8 = held.utf8();
                const uint8_t *path
                    = reinterpret_cast<const uint8_t *>(utf8.get_data());
                if (!is_node_path(path, utf8.length())) {
                    return ERR_PARSE_ERROR;
                }
            }
            return append_text(r_out, tag_of(p_value.get_type()), held)
                ? OK
                : ERR_PARSE_ERROR;
        }
        case Variant::VECTOR2: {
            const Vector2 held = p_value;
            append_reals(r_out, TAG_VECTOR2, {held.x, held.y});
            return OK;
        }
        case Variant::VECTOR2I: {
            const Vector2i held = p_value;
            append_ints(r_out, TAG_VECTOR2I, {held.x, held.y});
            return OK;
        }
        case Variant::VECTOR3: {
            const Vector3 held = p_value;
            append_reals(r_out, TAG_VECTOR3, {held.x, held.y, held.z});
            return OK;
        }
        case Variant::VECTOR3I: {
            const Vector3i held = p_value;
            append_ints(r_out, TAG_VECTOR3I, {held.x, held.y, held.z});
            return OK;
        }
        case Variant::VECTOR4: {
            const Vector4 held = p_value;
            append_reals(r_out, TAG_VECTOR4, {held.x, held.y, held.z, held.w});
            return OK;
        }
        case Variant::VECTOR4I: {
            const Vector4i held = p_value;
            append_ints(r_out, TAG_VECTOR4I, {held.x, held.y, held.z, held.w});
            return OK;
        }
        case Variant::QUATERNION: {
            const Quaternion held = p_value;
            append_reals(
                r_out,
                TAG_QUATERNION,
                {held.x, held.y, held.z, held.w}
            );
            return OK;
        }
        case Variant::COLOR: {
            const Color held = p_value;
            append_reals(r_out, TAG_COLOR, {held.r, held.g, held.b, held.a});
            return OK;
        }
        case Variant::RECT2: {
            const Rect2 held = p_value;
            const Vector2 &corner = held.position;
            const Vector2 &extent = held.size;
            append_reals(
                r_out,
                TAG_RECT2,
                {corner.x, corner.y, extent.x, extent.y}
            );
            return OK;
        }
        case Variant::RECT2I: {
            const Rect2i held = p_value;
            const Vector2i &corner = held.position;
            const Vector2i &extent = held.size;
            append_ints(
                r_out,
                TAG_RECT2I,
                {corner.x, corner.y, extent.x, extent.y}
            );
            return OK;
        }
        case Variant::PLANE: {
            const Plane held = p_value;
            const Vector3 &normal = held.normal;
            append_reals(
                r_out,
                TAG_PLANE,
                {normal.x, normal.y, normal.z, held.d}
            );
            return OK;
        }
        case Variant::AABB: {
            const AABB held = p_value;
            const Vector3 &corner = held.position;
            const Vector3 &extent = held.size;
            append_reals(
                r_out,
                TAG_AABB,
                {corner.x, corner.y, corner.z, extent.x, extent.y, extent.z}
            );
            return OK;
        }
        case Variant::BASIS: {
            const Basis held = p_value;
            const Vector3 *rows = held.rows;
            append_reals(
                r_out,
                TAG_BASIS,
                {rows[0].x,
                 rows[0].y,
                 rows[0].z,
                 rows[1].x,
                 rows[1].y,
                 rows[1].z,
                 rows[2].x,
                 rows[2].y,
                 rows[2].z}
            );
            return OK;
        }
        case Variant::TRANSFORM2D: {
            const Transform2D held = p_value;
            const Vector2 *columns = held.columns;
            append_reals(
                r_out,
                TAG_TRANSFORM2D,
                {columns[0].x,
                 columns[0].y,
                 columns[1].x,
                 columns[1].y,
                 columns[2].x,
                 columns[2].y}
            );
            return OK;
        }
        case Variant::TRANSFORM3D: {
            const Transform3D held = p_value;
            const Vector3 *rows = held.basis.rows;
            const Vector3 &origin = held.origin;
            append_reals(
                r_out,
                TAG_TRANSFORM3D,
                {rows[0].x,
                 rows[0].y,
                 rows[0].z,
                 rows[1].x,
                 rows[1].y,
                 rows[1].z,
                 rows[2].x,
                 rows[2].y,
                 rows[2].z,
                 origin.x,
                 origin.y,
                 origin.z}
            );
            return OK;
        }
        case Variant::PROJECTION: {
            const Projection held = p_value;
            const Vector4 *columns = held.columns;
            append_reals(
                r_out,
                TAG_PROJECTION,
                {columns[0].x,
                 columns[0].y,
                 columns[0].z,
                 columns[0].w,
                 columns[1].x,
                 columns[1].y,
                 columns[1].z,
                 columns[1].w,
                 columns[2].x,
                 columns[2].y,
                 columns[2].z,
                 columns[2].w,
                 columns[3].x,
                 columns[3].y,
                 columns[3].z,
                 columns[3].w}
            );
            return OK;
        }
        case Variant::ARRAY: {
            const Array list = p_value;
            if (list.is_typed()) {
                const Tag element
                    = tag_of(Variant::Type(list.get_typed_builtin()));
                if (element == TAG_COUNT) {
                    return ERR_INVALID_DATA;
                }
                write_tag(r_out, TAG_TYPED_ARRAY);
                write_tag(r_out, element);
            } else {
                write_tag(r_out, TAG_ARRAY);
            }
            write_varuint(r_out, uint64_t(list.size()));
            for (int at = 0; at < list.size(); ++at) {
                const Error nested
                    = append(list[at], p_depth + 1, r_budget, r_out);
                if (nested != OK) {
                    return nested;
                }
            }
            return OK;
        }
        case Variant::DICTIONARY: {
            const Dictionary map = p_value;
            if (map.is_typed()) {
                const Tag key
                    = tag_of(Variant::Type(map.get_typed_key_builtin()));
                const Tag held
                    = tag_of(Variant::Type(map.get_typed_value_builtin()));
                if (key == TAG_COUNT || held == TAG_COUNT) {
                    return ERR_INVALID_DATA;
                }
                write_tag(r_out, TAG_TYPED_DICTIONARY);
                write_tag(r_out, key);
                write_tag(r_out, held);
            } else {
                write_tag(r_out, TAG_DICTIONARY);
            }
            const Array keys = map.keys();
            write_varuint(r_out, uint64_t(keys.size()));
            for (int at = 0; at < keys.size(); ++at) {
                Error nested = append(keys[at], p_depth + 1, r_budget, r_out);
                if (nested != OK) {
                    return nested;
                }
                nested = append(map[keys[at]], p_depth + 1, r_budget, r_out);
                if (nested != OK) {
                    return nested;
                }
            }
            return OK;
        }
        case Variant::PACKED_BYTE_ARRAY: {
            const PackedByteArray held = p_value;
            write_tag(r_out, TAG_BYTES);
            write_varuint(r_out, uint64_t(held.size()));
            write_span(r_out, held.ptr(), held.size());
            return OK;
        }
        case Variant::PACKED_INT32_ARRAY: {
            const PackedInt32Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_INT32,
                held,
                [&](int32_t p_element) {
                    write_u32(r_out, uint32_t(p_element));
                }
            );
            return OK;
        }
        case Variant::PACKED_INT64_ARRAY: {
            const PackedInt64Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_INT64,
                held,
                [&](int64_t p_element) {
                    write_u64(r_out, uint64_t(p_element));
                }
            );
            return OK;
        }
        case Variant::PACKED_FLOAT32_ARRAY: {
            const PackedFloat32Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_FLOAT32,
                held,
                [&](float p_element) { write_f32(r_out, p_element); }
            );
            return OK;
        }
        case Variant::PACKED_FLOAT64_ARRAY: {
            const PackedFloat64Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_FLOAT64,
                held,
                [&](double p_element) { write_f64(r_out, p_element); }
            );
            return OK;
        }
        case Variant::PACKED_STRING_ARRAY: {
            const PackedStringArray held = p_value;
            bool texts = true;
            append_packed(
                r_out,
                TAG_PACKED_STRING,
                held,
                [&](const String &p_element) {
                    const CharString utf8 = p_element.utf8();
                    texts = texts && exact(utf8);
                    write_varuint(r_out, uint64_t(utf8.length()));
                    write_span(
                        r_out,
                        reinterpret_cast<const uint8_t *>(utf8.get_data()),
                        utf8.length()
                    );
                }
            );
            return texts ? OK : ERR_PARSE_ERROR;
        }
        case Variant::PACKED_VECTOR2_ARRAY: {
            const PackedVector2Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_VECTOR2,
                held,
                [&](const Vector2 &p_element) {
                    write_f32(r_out, float(p_element.x));
                    write_f32(r_out, float(p_element.y));
                }
            );
            return OK;
        }
        case Variant::PACKED_VECTOR3_ARRAY: {
            const PackedVector3Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_VECTOR3,
                held,
                [&](const Vector3 &p_element) {
                    write_f32(r_out, float(p_element.x));
                    write_f32(r_out, float(p_element.y));
                    write_f32(r_out, float(p_element.z));
                }
            );
            return OK;
        }
        case Variant::PACKED_COLOR_ARRAY: {
            const PackedColorArray held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_COLOR,
                held,
                [&](const Color &p_element) {
                    write_f32(r_out, p_element.r);
                    write_f32(r_out, p_element.g);
                    write_f32(r_out, p_element.b);
                    write_f32(r_out, p_element.a);
                }
            );
            return OK;
        }
        case Variant::PACKED_VECTOR4_ARRAY: {
            const PackedVector4Array held = p_value;
            append_packed(
                r_out,
                TAG_PACKED_VECTOR4,
                held,
                [&](const Vector4 &p_element) {
                    write_f32(r_out, float(p_element.x));
                    write_f32(r_out, float(p_element.y));
                    write_f32(r_out, float(p_element.z));
                    write_f32(r_out, float(p_element.w));
                }
            );
            return OK;
        }
        default:
            return ERR_INVALID_DATA;
    }
}

String text_of(Reader &p_reader) {
    const uint64_t size = p_reader.varuint();
    const uint8_t *utf8 = p_reader.span(int64_t(size));
    if (!p_reader.ok || size == 0) {
        return String();
    }
    return String::utf8(reinterpret_cast<const char *>(utf8), int(size));
}

Basis basis_of(const float *p_rows) {
    return Basis(
        p_rows[0],
        p_rows[1],
        p_rows[2],
        p_rows[3],
        p_rows[4],
        p_rows[5],
        p_rows[6],
        p_rows[7],
        p_rows[8]
    );
}

template <class Packed, class Each>
bool build_packed(
    Reader &p_reader,
    int p_width,
    Variant &r_value,
    Each p_each
) {
    const uint64_t count = p_reader.varuint();
    if (!p_reader.ok || count > uint64_t(p_reader.left()) / p_width) {
        return false;
    }
    Packed held;
    held.resize(int64_t(count));
    auto *elements = held.ptrw();
    for (uint64_t at = 0; at < count; ++at) {
        elements[at] = p_each(p_reader);
    }
    r_value = held;
    return p_reader.ok;
}

bool build(Reader &p_reader, Variant &r_value) {
    const Tag tag = p_reader.tag();
    if (!p_reader.ok) {
        return false;
    }
    float reals[16] = {};
    int32_t ints[4] = {};
    switch (tag) {
        case TAG_NIL:
            r_value = Variant();
            return true;
        case TAG_FALSE:
            r_value = false;
            return true;
        case TAG_TRUE:
            r_value = true;
            return true;
        case TAG_INT:
            r_value = p_reader.svarint();
            return p_reader.ok;
        case TAG_FLOAT:
            r_value = p_reader.f64();
            return p_reader.ok;
        case TAG_FLOAT32:
            r_value = double(p_reader.f32());
            return p_reader.ok;
        case TAG_STRING:
            r_value = text_of(p_reader);
            return p_reader.ok;
        case TAG_STRING_NAME:
            r_value = StringName(text_of(p_reader));
            return p_reader.ok;
        case TAG_NODE_PATH:
            r_value = NodePath(text_of(p_reader));
            return p_reader.ok;
        case TAG_VECTOR2:
            p_reader.reals(reals, 2);
            r_value = Vector2(reals[0], reals[1]);
            return p_reader.ok;
        case TAG_VECTOR2I:
            p_reader.ints(ints, 2);
            r_value = Vector2i(ints[0], ints[1]);
            return p_reader.ok;
        case TAG_VECTOR3:
            p_reader.reals(reals, 3);
            r_value = Vector3(reals[0], reals[1], reals[2]);
            return p_reader.ok;
        case TAG_VECTOR3I:
            p_reader.ints(ints, 3);
            r_value = Vector3i(ints[0], ints[1], ints[2]);
            return p_reader.ok;
        case TAG_VECTOR4:
            p_reader.reals(reals, 4);
            r_value = Vector4(reals[0], reals[1], reals[2], reals[3]);
            return p_reader.ok;
        case TAG_VECTOR4I:
            p_reader.ints(ints, 4);
            r_value = Vector4i(ints[0], ints[1], ints[2], ints[3]);
            return p_reader.ok;
        case TAG_QUATERNION:
            p_reader.reals(reals, 4);
            r_value = Quaternion(reals[0], reals[1], reals[2], reals[3]);
            return p_reader.ok;
        case TAG_COLOR:
            p_reader.reals(reals, 4);
            r_value = Color(reals[0], reals[1], reals[2], reals[3]);
            return p_reader.ok;
        case TAG_RECT2:
            p_reader.reals(reals, 4);
            r_value = Rect2(reals[0], reals[1], reals[2], reals[3]);
            return p_reader.ok;
        case TAG_RECT2I:
            p_reader.ints(ints, 4);
            r_value = Rect2i(ints[0], ints[1], ints[2], ints[3]);
            return p_reader.ok;
        case TAG_PLANE:
            p_reader.reals(reals, 4);
            r_value = Plane(reals[0], reals[1], reals[2], reals[3]);
            return p_reader.ok;
        case TAG_AABB:
            p_reader.reals(reals, 6);
            r_value = AABB(
                Vector3(reals[0], reals[1], reals[2]),
                Vector3(reals[3], reals[4], reals[5])
            );
            return p_reader.ok;
        case TAG_BASIS:
            p_reader.reals(reals, 9);
            r_value = basis_of(reals);
            return p_reader.ok;
        case TAG_TRANSFORM2D:
            p_reader.reals(reals, 6);
            r_value = Transform2D(
                Vector2(reals[0], reals[1]),
                Vector2(reals[2], reals[3]),
                Vector2(reals[4], reals[5])
            );
            return p_reader.ok;
        case TAG_TRANSFORM3D:
            p_reader.reals(reals, 12);
            r_value = Transform3D(
                basis_of(reals),
                Vector3(reals[9], reals[10], reals[11])
            );
            return p_reader.ok;
        case TAG_PROJECTION:
            p_reader.reals(reals, 16);
            r_value = Projection(
                Vector4(reals[0], reals[1], reals[2], reals[3]),
                Vector4(reals[4], reals[5], reals[6], reals[7]),
                Vector4(reals[8], reals[9], reals[10], reals[11]),
                Vector4(reals[12], reals[13], reals[14], reals[15])
            );
            return p_reader.ok;
        case TAG_ARRAY:
        case TAG_TYPED_ARRAY: {
            Array list;
            if (tag == TAG_TYPED_ARRAY) {
                const Tag element = p_reader.tag();
                list.set_typed(TYPES[element], StringName(), Variant());
            }
            const uint64_t count = p_reader.varuint();
            for (uint64_t at = 0; at < count && p_reader.ok; ++at) {
                Variant element;
                if (!build(p_reader, element)) {
                    return false;
                }
                list.push_back(element);
            }
            r_value = list;
            return p_reader.ok;
        }
        case TAG_DICTIONARY:
        case TAG_TYPED_DICTIONARY: {
            Dictionary map;
            if (tag == TAG_TYPED_DICTIONARY) {
                const Tag key = p_reader.tag();
                const Tag held = p_reader.tag();
                map.set_typed(
                    TYPES[key],
                    StringName(),
                    Variant(),
                    TYPES[held],
                    StringName(),
                    Variant()
                );
            }
            const uint64_t count = p_reader.varuint();
            for (uint64_t at = 0; at < count && p_reader.ok; ++at) {
                Variant key;
                Variant held;
                if (!build(p_reader, key) || !build(p_reader, held)) {
                    return false;
                }
                map[key] = held;
            }
            r_value = map;
            return p_reader.ok && uint64_t(map.size()) == count;
        }
        case TAG_BYTES: {
            const uint64_t size = p_reader.varuint();
            const uint8_t *bytes = p_reader.span(int64_t(size));
            if (!p_reader.ok) {
                return false;
            }
            PackedByteArray held;
            held.resize(int64_t(size));
            if (size > 0) {
                std::memcpy(held.ptrw(), bytes, size_t(size));
            }
            r_value = held;
            return true;
        }
        case TAG_PACKED_INT32:
            return build_packed<PackedInt32Array>(
                p_reader,
                4,
                r_value,
                [](Reader &p_from) { return int32_t(p_from.u32()); }
            );
        case TAG_PACKED_INT64:
            return build_packed<PackedInt64Array>(
                p_reader,
                8,
                r_value,
                [](Reader &p_from) { return int64_t(p_from.u64()); }
            );
        case TAG_PACKED_FLOAT32:
            return build_packed<PackedFloat32Array>(
                p_reader,
                4,
                r_value,
                [](Reader &p_from) { return p_from.f32(); }
            );
        case TAG_PACKED_FLOAT64:
            return build_packed<PackedFloat64Array>(
                p_reader,
                8,
                r_value,
                [](Reader &p_from) { return p_from.f64(); }
            );
        case TAG_PACKED_STRING:
            return build_packed<PackedStringArray>(
                p_reader,
                1,
                r_value,
                [](Reader &p_from) { return text_of(p_from); }
            );
        case TAG_PACKED_VECTOR2:
            return build_packed<PackedVector2Array>(
                p_reader,
                8,
                r_value,
                [](Reader &p_from) {
                    float parts[2];
                    p_from.reals(parts, 2);
                    return Vector2(parts[0], parts[1]);
                }
            );
        case TAG_PACKED_VECTOR3:
            return build_packed<PackedVector3Array>(
                p_reader,
                12,
                r_value,
                [](Reader &p_from) {
                    float parts[3];
                    p_from.reals(parts, 3);
                    return Vector3(parts[0], parts[1], parts[2]);
                }
            );
        case TAG_PACKED_COLOR:
            return build_packed<PackedColorArray>(
                p_reader,
                16,
                r_value,
                [](Reader &p_from) {
                    float parts[4];
                    p_from.reals(parts, 4);
                    return Color(parts[0], parts[1], parts[2], parts[3]);
                }
            );
        case TAG_PACKED_VECTOR4:
            return build_packed<PackedVector4Array>(
                p_reader,
                16,
                r_value,
                [](Reader &p_from) {
                    float parts[4];
                    p_from.reals(parts, 4);
                    return Vector4(parts[0], parts[1], parts[2], parts[3]);
                }
            );
        case TAG_COUNT:
            return false;
    }
    return false;
}

constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t mix(uint32_t p_hash, uint8_t p_byte) {
    return (p_hash ^ uint32_t(p_byte)) * FNV_PRIME;
}

bool plain_text(const String &p_text, int64_t &r_size) {
    const char32_t *chars = p_text.ptr();
    const int64_t length = p_text.length();
    r_size = 0;
    for (int64_t at = 0; at < length; ++at) {
        const char32_t point = chars[at];
        const bool surrogate = point >= 0xd800 && point <= 0xdfff;
        if (point == 0 || point > 0x10ffff || surrogate
            || (at == 0 && point == 0xfeff)) {
            return false;
        }
        r_size += point < 0x80 ? 1
            : point < 0x800    ? 2
            : point < 0x10000  ? 3
                               : 4;
    }
    return true;
}

uint32_t hash_plain_text(
    uint32_t p_hash,
    Tag p_tag,
    const String &p_text,
    int64_t p_size
) {
    uint32_t hashed = mix(p_hash, uint8_t(p_tag));
    uint64_t remaining = uint64_t(p_size);
    do {
        uint8_t group = uint8_t(remaining & 0x7f);
        remaining >>= 7;
        hashed = mix(hashed, remaining != 0 ? group | 0x80 : group);
    } while (remaining != 0);
    const char32_t *chars = p_text.ptr();
    for (int64_t at = 0; at < p_text.length(); ++at) {
        const uint32_t point = uint32_t(chars[at]);
        if (point < 0x80) {
            hashed = mix(hashed, uint8_t(point));
        } else if (point < 0x800) {
            hashed = mix(hashed, uint8_t(0xc0 | (point >> 6)));
            hashed = mix(hashed, uint8_t(0x80 | (point & 0x3f)));
        } else if (point < 0x10000) {
            hashed = mix(hashed, uint8_t(0xe0 | (point >> 12)));
            hashed = mix(hashed, uint8_t(0x80 | ((point >> 6) & 0x3f)));
            hashed = mix(hashed, uint8_t(0x80 | (point & 0x3f)));
        } else {
            hashed = mix(hashed, uint8_t(0xf0 | (point >> 18)));
            hashed = mix(hashed, uint8_t(0x80 | ((point >> 12) & 0x3f)));
            hashed = mix(hashed, uint8_t(0x80 | ((point >> 6) & 0x3f)));
            hashed = mix(hashed, uint8_t(0x80 | (point & 0x3f)));
        }
    }
    return hashed;
}

bool build_valid(const uint8_t *p_bytes, int64_t p_size, Variant &r_value) {
    Reader reader(p_bytes, p_size);
    Variant built;
    if (!build(reader, built)) {
        return false;
    }
    r_value = built;
    return true;
}

} // namespace

Error encode(const Variant &p_value, Bytes &r_bytes) {
    const uint32_t start = r_bytes.size();
    int budget = MAX_ELEMENTS;
    const Error verdict = append(p_value, 0, budget, r_bytes);
    if (verdict != OK) {
        r_bytes.resize(start);
    }
    return verdict;
}

int64_t decode(const uint8_t *p_bytes, int64_t p_size, Variant &r_value) {
    const int64_t size = validate(p_bytes, p_size);
    if (size < 0 || !build_valid(p_bytes, size, r_value)) {
        return -1;
    }
    return size;
}

Error encode_packed(const Variant &p_value, PackedByteArray &r_bytes) {
    Bytes bytes;
    const Error verdict = encode(p_value, bytes);
    if (verdict != OK) {
        write_nil(bytes);
    }
    r_bytes.resize(int64_t(bytes.size()));
    std::memcpy(r_bytes.ptrw(), bytes.ptr(), bytes.size());
    return verdict;
}

bool decode_packed(const PackedByteArray &p_bytes, Variant &r_value) {
    return validate(p_bytes.ptr(), p_bytes.size()) == p_bytes.size()
        && build_valid(p_bytes.ptr(), p_bytes.size(), r_value);
}

uint32_t hash_variant(uint32_t p_hash, const Variant &p_value) {
    const Variant::Type type = p_value.get_type();
    if (type == Variant::STRING || type == Variant::STRING_NAME) {
        const String held = p_value;
        int64_t size = 0;
        if (plain_text(held, size)) {
            return hash_plain_text(p_hash, tag_of(type), held, size);
        }
    }
    static thread_local Bytes bytes;
    bytes.clear();
    if (encode(p_value, bytes) != OK) {
        bytes.clear();
        write_nil(bytes);
    }
    return hash(p_hash, bytes.ptr(), int64_t(bytes.size()));
}

Tag kind_of_type(int p_type) {
    if (p_type <= Variant::NIL || p_type >= Variant::VARIANT_MAX) {
        return TAG_COUNT;
    }
    const Tag tag = tag_of(Variant::Type(p_type));
    return tag == TAG_COUNT ? TAG_COUNT : kind_of(tag);
}

} // namespace netw::value
