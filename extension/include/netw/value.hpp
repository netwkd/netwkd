#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"

namespace netw::value {

enum Tag : uint8_t {
    TAG_NIL = 0,
    TAG_FALSE = 1,
    TAG_TRUE = 2,
    TAG_INT = 3,
    TAG_FLOAT = 4,
    TAG_FLOAT32 = 5,
    TAG_STRING = 6,
    TAG_STRING_NAME = 7,
    TAG_NODE_PATH = 8,
    TAG_VECTOR2 = 9,
    TAG_VECTOR2I = 10,
    TAG_VECTOR3 = 11,
    TAG_VECTOR3I = 12,
    TAG_VECTOR4 = 13,
    TAG_VECTOR4I = 14,
    TAG_QUATERNION = 15,
    TAG_COLOR = 16,
    TAG_RECT2 = 17,
    TAG_RECT2I = 18,
    TAG_PLANE = 19,
    TAG_AABB = 20,
    TAG_BASIS = 21,
    TAG_TRANSFORM2D = 22,
    TAG_TRANSFORM3D = 23,
    TAG_PROJECTION = 24,
    TAG_ARRAY = 25,
    TAG_TYPED_ARRAY = 26,
    TAG_DICTIONARY = 27,
    TAG_TYPED_DICTIONARY = 28,
    TAG_BYTES = 29,
    TAG_PACKED_INT32 = 30,
    TAG_PACKED_INT64 = 31,
    TAG_PACKED_FLOAT32 = 32,
    TAG_PACKED_FLOAT64 = 33,
    TAG_PACKED_STRING = 34,
    TAG_PACKED_VECTOR2 = 35,
    TAG_PACKED_VECTOR3 = 36,
    TAG_PACKED_COLOR = 37,
    TAG_PACKED_VECTOR4 = 38,
    TAG_COUNT = 39,
};

constexpr int MAX_DEPTH = 16;
constexpr int MAX_ELEMENTS = 65536;

using Bytes = godot::LocalVector<uint8_t>;

void write_tag(Bytes &r_out, Tag p_tag);
void write_varuint(Bytes &r_out, uint64_t p_value);
void write_svarint(Bytes &r_out, int64_t p_value);
void write_u32(Bytes &r_out, uint32_t p_value);
void write_u64(Bytes &r_out, uint64_t p_value);
void write_f32(Bytes &r_out, float p_value);
void write_f64(Bytes &r_out, double p_value);
void write_span(Bytes &r_out, const uint8_t *p_bytes, int64_t p_size);

void write_nil(Bytes &r_out);
void write_bool(Bytes &r_out, bool p_value);
void write_int(Bytes &r_out, int64_t p_value);
void write_float(Bytes &r_out, double p_value);
void write_text(Bytes &r_out, Tag p_tag, const char *p_utf8, int64_t p_size);
void write_reals(
    Bytes &r_out,
    Tag p_tag,
    const float *p_components,
    int p_count
);
void write_ints(
    Bytes &r_out,
    Tag p_tag,
    const int32_t *p_components,
    int p_count
);

struct Reader {
    const uint8_t *at = nullptr;
    const uint8_t *end = nullptr;
    bool ok = true;

    Reader(const uint8_t *p_bytes, int64_t p_size);

    int64_t left() const {
        return end - at;
    }

    Tag tag();
    uint64_t varuint();
    int64_t svarint();
    uint32_t u32();
    uint64_t u64();
    float f32();
    double f64();
    const uint8_t *span(int64_t p_size);
    void reals(float *r_components, int p_count);
    void ints(int32_t *r_components, int p_count);
};

Tag kind_of(Tag p_tag);

bool is_exact_text(const uint8_t *p_utf8, int64_t p_size);

bool is_node_path(const uint8_t *p_utf8, int64_t p_size);

bool skip(Reader &r_reader);

int64_t validate(const uint8_t *p_bytes, int64_t p_size);

int compare(
    const uint8_t *p_left,
    int64_t p_left_size,
    const uint8_t *p_right,
    int64_t p_right_size
);

uint32_t hash(uint32_t p_hash, const uint8_t *p_bytes, int64_t p_size);

} // namespace netw::value
