#include "netw/sealed.hpp"

#include "netw/value.hpp"

#include <algorithm>
#include <cstring>

namespace netw::value {

namespace {

constexpr int VARINT_GROUPS = 10;

uint64_t zigzag_encode(int64_t p_value) {
    return (uint64_t(p_value) << 1) ^ uint64_t(p_value >> 63);
}

int64_t zigzag_decode(uint64_t p_value) {
    return int64_t((p_value >> 1) ^ uint64_t(-int64_t(p_value & 1)));
}

constexpr uint32_t FNV_PRIME = 16777619u;

bool survives_float(double p_value) {
    const double widened = double(float(p_value));
    return std::memcmp(&widened, &p_value, sizeof(double)) == 0;
}

int real_count(Tag p_tag) {
    switch (p_tag) {
        case TAG_VECTOR2:
            return 2;
        case TAG_VECTOR3:
            return 3;
        case TAG_VECTOR4:
        case TAG_QUATERNION:
        case TAG_COLOR:
        case TAG_RECT2:
        case TAG_PLANE:
            return 4;
        case TAG_AABB:
        case TAG_TRANSFORM2D:
            return 6;
        case TAG_BASIS:
            return 9;
        case TAG_TRANSFORM3D:
            return 12;
        case TAG_PROJECTION:
            return 16;
        default:
            return 0;
    }
}

int int_count(Tag p_tag) {
    switch (p_tag) {
        case TAG_VECTOR2I:
            return 2;
        case TAG_VECTOR3I:
            return 3;
        case TAG_VECTOR4I:
        case TAG_RECT2I:
            return 4;
        default:
            return 0;
    }
}

int packed_width(Tag p_tag) {
    switch (p_tag) {
        case TAG_PACKED_INT32:
        case TAG_PACKED_FLOAT32:
            return 4;
        case TAG_PACKED_INT64:
        case TAG_PACKED_FLOAT64:
        case TAG_PACKED_VECTOR2:
            return 8;
        case TAG_PACKED_VECTOR3:
            return 12;
        case TAG_PACKED_COLOR:
        case TAG_PACKED_VECTOR4:
            return 16;
        default:
            return 0;
    }
}

bool is_kind(Tag p_tag) {
    return kind_of(p_tag) == p_tag;
}

int64_t sequence_length(const uint8_t *p_bytes, int64_t p_size) {
    const uint8_t lead = p_bytes[0];
    if (lead == 0) {
        return -1;
    }
    if (lead < 0x80) {
        return 1;
    }
    int64_t length = 0;
    uint32_t point = 0;
    uint32_t least = 0;
    if ((lead & 0xe0) == 0xc0) {
        length = 2;
        point = lead & 0x1f;
        least = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
        point = lead & 0x0f;
        least = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
        point = lead & 0x07;
        least = 0x10000;
    } else {
        return -1;
    }
    if (length > p_size) {
        return -1;
    }
    for (int64_t at = 1; at < length; ++at) {
        if ((p_bytes[at] & 0xc0) != 0x80) {
            return -1;
        }
        point = (point << 6) | (p_bytes[at] & 0x3f);
    }
    const bool surrogate = point >= 0xd800 && point <= 0xdfff;
    if (point < least || point > 0x10ffff || surrogate) {
        return -1;
    }
    return length;
}

bool is_text(const uint8_t *p_bytes, int64_t p_size) {
    const bool marked = p_size >= 3 && p_bytes[0] == 0xef && p_bytes[1] == 0xbb
        && p_bytes[2] == 0xbf;
    if (marked) {
        return false;
    }
    int64_t at = 0;
    while (at < p_size) {
        const int64_t length = sequence_length(p_bytes + at, p_size - at);
        if (length < 0) {
            return false;
        }
        at += length;
    }
    return true;
}

bool text(Reader &r_reader) {
    const uint64_t size = r_reader.varuint();
    const uint8_t *utf8 = r_reader.span(int64_t(size));
    return r_reader.ok && is_text(utf8, int64_t(size));
}

bool walk(Reader &r_reader, int p_depth, int &r_budget, Tag &r_kind);

bool elements(Reader &r_reader, int p_depth, int &r_budget, Tag p_want) {
    const uint64_t count = r_reader.varuint();
    if (!r_reader.ok || count > uint64_t(r_reader.left())) {
        return false;
    }
    for (uint64_t at = 0; at < count; ++at) {
        Tag kind = TAG_NIL;
        if (!walk(r_reader, p_depth + 1, r_budget, kind)) {
            return false;
        }
        if (p_want != TAG_NIL && kind != p_want) {
            return false;
        }
    }
    return true;
}

bool pairs(
    Reader &r_reader,
    int p_depth,
    int &r_budget,
    Tag p_want_key,
    Tag p_want_value
) {
    const uint64_t count = r_reader.varuint();
    if (!r_reader.ok || count > uint64_t(r_reader.left()) / 2) {
        return false;
    }
    for (uint64_t at = 0; at < count; ++at) {
        Tag key = TAG_NIL;
        Tag held = TAG_NIL;
        if (!walk(r_reader, p_depth + 1, r_budget, key)
            || !walk(r_reader, p_depth + 1, r_budget, held)) {
            return false;
        }
        if ((p_want_key != TAG_NIL && key != p_want_key)
            || (p_want_value != TAG_NIL && held != p_want_value)) {
            return false;
        }
    }
    return true;
}

bool walk(Reader &r_reader, int p_depth, int &r_budget, Tag &r_kind) {
    if (p_depth > MAX_DEPTH) {
        return false;
    }
    if (--r_budget < 0) {
        return false;
    }
    const Tag tag = r_reader.tag();
    if (!r_reader.ok) {
        return false;
    }
    r_kind = kind_of(tag);
    if (real_count(tag) > 0) {
        r_reader.span(int64_t(real_count(tag)) * 4);
        return r_reader.ok;
    }
    if (int_count(tag) > 0) {
        int32_t parts[4];
        r_reader.ints(parts, int_count(tag));
        return r_reader.ok;
    }
    if (packed_width(tag) > 0) {
        const uint64_t count = r_reader.varuint();
        const uint64_t room = uint64_t(r_reader.left()) / packed_width(tag);
        if (!r_reader.ok || count > room) {
            return false;
        }
        r_reader.span(int64_t(count) * packed_width(tag));
        return r_reader.ok;
    }
    switch (tag) {
        case TAG_NIL:
        case TAG_FALSE:
        case TAG_TRUE:
            return true;
        case TAG_INT:
            r_reader.svarint();
            return r_reader.ok;
        case TAG_FLOAT: {
            const double held = r_reader.f64();
            return r_reader.ok && !survives_float(held);
        }
        case TAG_FLOAT32:
            r_reader.f32();
            return r_reader.ok;
        case TAG_STRING:
        case TAG_STRING_NAME:
            return text(r_reader);
        case TAG_NODE_PATH: {
            const uint64_t size = r_reader.varuint();
            const uint8_t *path = r_reader.span(int64_t(size));
            return r_reader.ok && is_text(path, int64_t(size))
                && is_node_path(path, int64_t(size));
        }
        case TAG_ARRAY:
            return elements(r_reader, p_depth, r_budget, TAG_NIL);
        case TAG_TYPED_ARRAY: {
            const Tag element = r_reader.tag();
            if (!r_reader.ok || !is_kind(element) || element == TAG_NIL) {
                return false;
            }
            return elements(r_reader, p_depth, r_budget, element);
        }
        case TAG_DICTIONARY:
            return pairs(r_reader, p_depth, r_budget, TAG_NIL, TAG_NIL);
        case TAG_TYPED_DICTIONARY: {
            const Tag key = r_reader.tag();
            const Tag held = r_reader.tag();
            const bool typed = key != TAG_NIL || held != TAG_NIL;
            if (!r_reader.ok || !is_kind(key) || !is_kind(held) || !typed) {
                return false;
            }
            return pairs(r_reader, p_depth, r_budget, key, held);
        }
        case TAG_BYTES: {
            const uint64_t size = r_reader.varuint();
            r_reader.span(int64_t(size));
            return r_reader.ok;
        }
        case TAG_PACKED_STRING: {
            const uint64_t count = r_reader.varuint();
            if (!r_reader.ok || count > uint64_t(r_reader.left())) {
                return false;
            }
            for (uint64_t at = 0; at < count; ++at) {
                if (!text(r_reader)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

void mix(uint32_t &r_hash, const uint8_t *p_bytes, int64_t p_size) {
    for (int64_t at = 0; at < p_size; ++at) {
        r_hash = (r_hash ^ uint32_t(p_bytes[at])) * FNV_PRIME;
    }
}

struct Pair {
    const uint8_t *key = nullptr;
    int64_t key_size = 0;
    const uint8_t *held = nullptr;
    int64_t held_size = 0;
};

const uint8_t *skip_from(Reader &r_reader) {
    const uint8_t *start = r_reader.at;
    skip(r_reader);
    return start;
}

void mix_value(uint32_t &r_hash, const uint8_t *p_bytes, int64_t p_size) {
    Reader reader(p_bytes, p_size);
    const Tag tag = reader.tag();
    const bool list = tag == TAG_ARRAY || tag == TAG_TYPED_ARRAY;
    const bool map = tag == TAG_DICTIONARY || tag == TAG_TYPED_DICTIONARY;
    if (!list && !map) {
        mix(r_hash, p_bytes, p_size);
        return;
    }
    if (tag == TAG_TYPED_ARRAY) {
        reader.tag();
    }
    if (tag == TAG_TYPED_DICTIONARY) {
        reader.tag();
        reader.tag();
    }
    const uint64_t count = reader.varuint();
    mix(r_hash, p_bytes, reader.at - p_bytes);
    if (list) {
        for (uint64_t at = 0; at < count; ++at) {
            const uint8_t *element = skip_from(reader);
            mix_value(r_hash, element, reader.at - element);
        }
        return;
    }
    godot::LocalVector<Pair> sorted;
    for (uint64_t at = 0; at < count; ++at) {
        Pair pair;
        pair.key = skip_from(reader);
        pair.key_size = reader.at - pair.key;
        pair.held = skip_from(reader);
        pair.held_size = reader.at - pair.held;
        sorted.push_back(pair);
    }
    std::sort(
        sorted.ptr(),
        sorted.ptr() + sorted.size(),
        [](const Pair &p_left, const Pair &p_right) {
            const int keys = compare(
                p_left.key,
                p_left.key_size,
                p_right.key,
                p_right.key_size
            );
            if (keys != 0) {
                return keys < 0;
            }
            return compare(
                       p_left.held,
                       p_left.held_size,
                       p_right.held,
                       p_right.held_size
                   )
                < 0;
        }
    );
    for (const Pair &pair : sorted) {
        mix_value(r_hash, pair.key, pair.key_size);
        mix_value(r_hash, pair.held, pair.held_size);
    }
}

} // namespace

void write_tag(Bytes &r_out, Tag p_tag) {
    r_out.push_back(uint8_t(p_tag));
}

void write_varuint(Bytes &r_out, uint64_t p_value) {
    uint64_t remaining = p_value;
    do {
        uint8_t group = uint8_t(remaining & 0x7f);
        remaining >>= 7;
        if (remaining != 0) {
            group |= 0x80;
        }
        r_out.push_back(group);
    } while (remaining != 0);
}

void write_svarint(Bytes &r_out, int64_t p_value) {
    write_varuint(r_out, zigzag_encode(p_value));
}

void write_u32(Bytes &r_out, uint32_t p_value) {
    for (int shift = 0; shift < 32; shift += 8) {
        r_out.push_back(uint8_t(p_value >> shift));
    }
}

void write_u64(Bytes &r_out, uint64_t p_value) {
    for (int shift = 0; shift < 64; shift += 8) {
        r_out.push_back(uint8_t(p_value >> shift));
    }
}

void write_f32(Bytes &r_out, float p_value) {
    uint32_t image = 0;
    std::memcpy(&image, &p_value, sizeof(float));
    write_u32(r_out, image);
}

void write_f64(Bytes &r_out, double p_value) {
    uint64_t image = 0;
    std::memcpy(&image, &p_value, sizeof(double));
    write_u64(r_out, image);
}

void write_span(Bytes &r_out, const uint8_t *p_bytes, int64_t p_size) {
    if (p_size <= 0) {
        return;
    }
    const uint32_t start = r_out.size();
    r_out.resize(start + uint32_t(p_size));
    std::memcpy(r_out.ptr() + start, p_bytes, size_t(p_size));
}

void write_nil(Bytes &r_out) {
    write_tag(r_out, TAG_NIL);
}

void write_bool(Bytes &r_out, bool p_value) {
    write_tag(r_out, p_value ? TAG_TRUE : TAG_FALSE);
}

void write_int(Bytes &r_out, int64_t p_value) {
    write_tag(r_out, TAG_INT);
    write_svarint(r_out, p_value);
}

void write_float(Bytes &r_out, double p_value) {
    if (survives_float(p_value)) {
        write_tag(r_out, TAG_FLOAT32);
        write_f32(r_out, float(p_value));
        return;
    }
    write_tag(r_out, TAG_FLOAT);
    write_f64(r_out, p_value);
}

void write_text(Bytes &r_out, Tag p_tag, const char *p_utf8, int64_t p_size) {
    write_tag(r_out, p_tag);
    write_varuint(r_out, uint64_t(p_size));
    write_span(r_out, reinterpret_cast<const uint8_t *>(p_utf8), p_size);
}

void write_reals(
    Bytes &r_out,
    Tag p_tag,
    const float *p_components,
    int p_count
) {
    write_tag(r_out, p_tag);
    for (int at = 0; at < p_count; ++at) {
        write_f32(r_out, p_components[at]);
    }
}

void write_ints(
    Bytes &r_out,
    Tag p_tag,
    const int32_t *p_components,
    int p_count
) {
    write_tag(r_out, p_tag);
    for (int at = 0; at < p_count; ++at) {
        write_svarint(r_out, p_components[at]);
    }
}

bool is_exact_text(const uint8_t *p_utf8, int64_t p_size) {
    return is_text(p_utf8, p_size);
}

bool is_node_path(const uint8_t *p_utf8, int64_t p_size) {
    int64_t at = p_size > 0 && p_utf8[0] == '/' ? 1 : 0;
    const int64_t names = at;
    int64_t run = 0;
    for (; at < p_size && p_utf8[at] != ':'; ++at) {
        if (p_utf8[at] != '/') {
            run += 1;
            continue;
        }
        if (run == 0) {
            return false;
        }
        run = 0;
    }
    if (at > names && run == 0) {
        return false;
    }
    while (at < p_size) {
        run = 0;
        for (at += 1; at < p_size && p_utf8[at] != ':'; ++at) {
            run += 1;
        }
        if (run == 0) {
            return false;
        }
    }
    return true;
}

Reader::Reader(const uint8_t *p_bytes, int64_t p_size)
    : at(p_bytes), end(p_bytes + (p_size > 0 ? p_size : 0)) {
}

Tag Reader::tag() {
    const uint8_t *byte = span(1);
    if (!ok) {
        return TAG_NIL;
    }
    if (*byte >= TAG_COUNT) {
        ok = false;
        return TAG_NIL;
    }
    return Tag(*byte);
}

uint64_t Reader::varuint() {
    uint64_t value = 0;
    for (int group = 0; group < VARINT_GROUPS; ++group) {
        const uint8_t *byte = span(1);
        if (!ok) {
            return 0;
        }
        const uint64_t low = *byte & 0x7f;
        if (group == VARINT_GROUPS - 1 && low > 1) {
            break;
        }
        value |= low << (group * 7);
        if ((*byte & 0x80) != 0) {
            continue;
        }
        if (group > 0 && low == 0) {
            break;
        }
        return value;
    }
    ok = false;
    return 0;
}

int64_t Reader::svarint() {
    return zigzag_decode(varuint());
}

uint32_t Reader::u32() {
    const uint8_t *bytes = span(4);
    if (!ok) {
        return 0;
    }
    uint32_t value = 0;
    for (int at = 0; at < 4; ++at) {
        value |= uint32_t(bytes[at]) << (at * 8);
    }
    return value;
}

uint64_t Reader::u64() {
    const uint8_t *bytes = span(8);
    if (!ok) {
        return 0;
    }
    uint64_t value = 0;
    for (int at = 0; at < 8; ++at) {
        value |= uint64_t(bytes[at]) << (at * 8);
    }
    return value;
}

float Reader::f32() {
    const uint32_t image = u32();
    float value = 0.0f;
    std::memcpy(&value, &image, sizeof(float));
    return value;
}

double Reader::f64() {
    const uint64_t image = u64();
    double value = 0.0;
    std::memcpy(&value, &image, sizeof(double));
    return value;
}

const uint8_t *Reader::span(int64_t p_size) {
    if (!ok || p_size < 0 || p_size > left()) {
        ok = false;
        return nullptr;
    }
    const uint8_t *start = at;
    at += p_size;
    return start;
}

void Reader::reals(float *r_components, int p_count) {
    for (int part = 0; part < p_count; ++part) {
        r_components[part] = f32();
    }
}

void Reader::ints(int32_t *r_components, int p_count) {
    for (int part = 0; part < p_count; ++part) {
        const int64_t value = svarint();
        if (value < INT32_MIN || value > INT32_MAX) {
            ok = false;
        }
        r_components[part] = int32_t(value);
    }
}

Tag kind_of(Tag p_tag) {
    switch (p_tag) {
        case TAG_TRUE:
            return TAG_FALSE;
        case TAG_FLOAT32:
            return TAG_FLOAT;
        case TAG_TYPED_ARRAY:
            return TAG_ARRAY;
        case TAG_TYPED_DICTIONARY:
            return TAG_DICTIONARY;
        default:
            return p_tag;
    }
}

bool skip(Reader &r_reader) {
    int budget = MAX_ELEMENTS;
    Tag kind = TAG_NIL;
    if (!walk(r_reader, 0, budget, kind)) {
        r_reader.ok = false;
    }
    return r_reader.ok;
}

int64_t validate(const uint8_t *p_bytes, int64_t p_size) {
    Reader reader(p_bytes, p_size);
    if (!skip(reader)) {
        return -1;
    }
    return reader.at - p_bytes;
}

int compare(
    const uint8_t *p_left,
    int64_t p_left_size,
    const uint8_t *p_right,
    int64_t p_right_size
) {
    const int64_t shared = std::min(p_left_size, p_right_size);
    const int order = shared > 0 ? std::memcmp(p_left, p_right, shared) : 0;
    if (order != 0) {
        return order;
    }
    if (p_left_size == p_right_size) {
        return 0;
    }
    return p_left_size < p_right_size ? -1 : 1;
}

uint32_t hash(uint32_t p_hash, const uint8_t *p_bytes, int64_t p_size) {
    uint32_t mixed = p_hash;
    if (validate(p_bytes, p_size) != p_size) {
        mix(mixed, p_bytes, p_size);
        return mixed;
    }
    mix_value(mixed, p_bytes, p_size);
    return mixed;
}

} // namespace netw::value
