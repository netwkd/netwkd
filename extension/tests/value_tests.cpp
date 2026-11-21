#include "support/fault_log.h"
#include "support/internals.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include <cstdint>
#include <vector>

#include "godot/node.hpp"
#include "godot/variant.hpp"
#include "netw/value.hpp"

namespace TestNetwValue {

using namespace godot;
using netw::value::Bytes;
using netw::value::Tag;
using netw_test::Internals;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
    Variant value;
    Tag tag = netw::value::TAG_NIL;
    std::vector<uint8_t> bytes;
    const char *says = nullptr;
};

typedef netw_test::LawRowFor<Stand> Law;

Bytes bytes_of(const std::vector<uint8_t> &p_bytes) {
    Bytes out;
    for (const uint8_t byte : p_bytes) {
        out.push_back(byte);
    }
    return out;
}

Bytes encoded(const Variant &p_value) {
    Bytes out;
    Internals::value_encode(p_value, out);
    return out;
}

bool same_typing(const Variant &p_left, const Variant &p_right) {
    if (p_left.get_type() == Variant::ARRAY) {
        const Array left = p_left;
        const Array right = p_right;
        return left.get_typed_builtin() == right.get_typed_builtin();
    }
    if (p_left.get_type() == Variant::DICTIONARY) {
        const Dictionary left = p_left;
        const Dictionary right = p_right;
        return left.get_typed_key_builtin() == right.get_typed_key_builtin()
            && left.get_typed_value_builtin()
            == right.get_typed_value_builtin();
    }
    return true;
}

LawVerdict round_trip(const Stand &p_stand) {
    Bytes bytes;
    if (Internals::value_encode(p_stand.value, bytes) != OK) {
        return law_broken("the writer refused the value");
    }
    if (bytes.size() == 0) {
        return law_broken("the writer wrote nothing");
    }
    if (bytes[0] != uint8_t(p_stand.tag)) {
        return law_broken("the value was written with tag %d", int(bytes[0]));
    }
    if (Internals::value_validate(bytes) != int64_t(bytes.size())) {
        return law_broken("validate did not take every byte written");
    }
    Variant read;
    if (Internals::value_decode(bytes, read) != int64_t(bytes.size())) {
        return law_broken("the reader did not take every byte written");
    }
    if (read.get_type() != p_stand.value.get_type() || read != p_stand.value
        || !same_typing(read, p_stand.value)) {
        return law_broken("the value read back differs from the one written");
    }
    if (Internals::value_compare(encoded(read), bytes) != 0) {
        return law_broken("the value read back writes different bytes");
    }
    return law_held();
}

LawVerdict refused_bytes(const Stand &p_stand) {
    const Bytes bytes = bytes_of(p_stand.bytes);
    if (Internals::value_validate(bytes) != -1) {
        return law_broken("validate accepted the bytes");
    }
    Variant read = String("untouched");
    if (Internals::value_decode(bytes, read) != -1
        || read != Variant(String("untouched"))) {
        return law_broken("the reader built a value from the bytes");
    }
    return law_held();
}

LawVerdict refused_build(const Stand &p_stand) {
    const Bytes bytes = bytes_of(p_stand.bytes);
    if (Internals::value_validate(bytes) != int64_t(bytes.size())) {
        return law_broken("validate refused a well formed value");
    }
    Variant read = String("untouched");
    if (Internals::value_decode(bytes, read) != -1
        || read != Variant(String("untouched"))) {
        return law_broken("the reader built a dictionary that lost a pair");
    }
    return law_held();
}

LawVerdict refused_whole(const Stand &p_stand) {
    PackedByteArray bytes;
    for (const uint8_t byte : p_stand.bytes) {
        bytes.push_back(byte);
    }
    Variant read = String("untouched");
    if (Internals::value_unpacked(bytes, read)
        || read != Variant(String("untouched"))) {
        return law_broken("the reader built a value from the bytes");
    }
    return law_held();
}

LawVerdict refused_value(const Stand &p_stand) {
    Bytes bytes;
    bytes.push_back(0xaa);
    if (Internals::value_encode(p_stand.value, bytes) == OK) {
        return law_broken("the writer accepted the value");
    }
    if (bytes.size() != 1 || bytes[0] != 0xaa) {
        return law_broken("the writer left bytes behind");
    }
    return law_held();
}

LawVerdict sent_as_nil(const Stand &p_stand) {
    netw_test::FaultWatch watch;
    const PackedByteArray sent = Internals::value_sent(p_stand.value);
    if (sent.size() != 1 || sent[0] != netw::value::TAG_NIL) {
        return law_broken("the value was not sent as NIL");
    }
    const int64_t said = p_stand.says == nullptr
        ? watch.errors()
        : watch.errors_containing(String(p_stand.says) + String(", which"));
    if (said != (p_stand.says == nullptr ? 0 : 1)) {
        return law_broken("the error named it %d times", int(said));
    }
    if (p_stand.says != nullptr && watch.errors_containing("[NW1019]") != 1) {
        return law_broken("the error was not NW1019");
    }
    return law_held();
}

Dictionary pairs_in_order(bool p_reversed, int p_score) {
    Dictionary inner;
    Dictionary outer;
    if (p_reversed) {
        inner.set(String("y"), 2);
        inner.set(String("x"), 1);
        Array list;
        list.push_back(inner);
        outer.set(String("b"), list);
        outer.set(String("a"), p_score);
        return outer;
    }
    inner.set(String("x"), 1);
    inner.set(String("y"), 2);
    Array list;
    list.push_back(inner);
    outer.set(String("a"), p_score);
    outer.set(String("b"), list);
    return outer;
}

LawVerdict hash_order(const Stand &) {
    const Bytes forward = encoded(pairs_in_order(false, 1));
    const Bytes reversed = encoded(pairs_in_order(true, 1));
    const Bytes other = encoded(pairs_in_order(false, 2));
    if (Internals::value_compare(forward, reversed) == 0) {
        return law_broken("insertion order did not reach the bytes");
    }
    if (Internals::value_hash(forward) != Internals::value_hash(reversed)) {
        return law_broken("insertion order changed the hash");
    }
    if (Internals::value_hash(forward) == Internals::value_hash(other)) {
        return law_broken("a changed value kept its hash");
    }
    return law_held();
}

LawVerdict hash_pinned(const Stand &) {
    Dictionary nested;
    nested.set(String("b"), 1);
    Array listed;
    listed.push_back(2);
    nested.set(String("a"), listed);
    const Variant values[] = {
        Variant(),
        true,
        int64_t(7),
        int64_t(-300000000000),
        0.1,
        0.5,
        String::utf8("caf\xc3\xa9"),
        StringName("speed"),
        NodePath(String("Body:position")),
        Vector3(1, 2, -3),
        nested,
        String::chr(0xFEFF) + String("hi"),
        String::utf8("\xe6\x97\xa5\xf0\x9d\x84\x9e"),
    };
    const uint32_t pinned[] = {
        84696351u,
        118251589u,
        1098457588u,
        3182874011u,
        2177851834u,
        4023176301u,
        91036762u,
        2252502820u,
        1179619067u,
        832390115u,
        1455492051u,
        84696351u,
        1008486337u,
    };
    char got[192] = {0};
    int written = 0;
    bool held = true;
    for (int at = 0; at < 13; ++at) {
        const uint32_t hashed = Internals::value_hash_of(values[at]);
        held = held && hashed == pinned[at];
        written += std::snprintf(
            got + written,
            sizeof(got) - size_t(written),
            "%u,",
            hashed
        );
    }
    return held ? law_held() : law_broken("hashed %s", got);
}

const Law ROUND_TRIP
    = {"a value written and read back",
       "it opens with its tag, comes back equal and writes the same bytes",
       &round_trip};
const Law HASH_PINNED
    = {"a fingerprint of known values",
       "each hashes to the number it hashed to when it was pinned",
       &hash_pinned};
const Law REFUSED_BYTES
    = {"bytes no writer produces",
       "validate and the reader refuse them and nothing is built",
       &refused_bytes};
const Law REFUSED_BUILD
    = {"a dictionary that holds one key twice",
       "its shape validates and the reader refuses to build it",
       &refused_build};
const Law REFUSED_WHOLE
    = {"a value with bytes after it",
       "a field that holds one value refuses it and nothing is built",
       &refused_whole};
const Law REFUSED_VALUE
    = {"a value the format does not hold",
       "the writer refuses it and leaves the bytes as they were",
       &refused_value};
const Law SENT_AS_NIL
    = {"a value sent in place of one the format does not hold",
       "it arrives as NIL, and the error names what it held unless it was null",
       &sent_as_nil};
const Law HASH_ORDER
    = {"two dictionaries with the same pairs",
       "their bytes keep insertion order and their hashes ignore it",
       &hash_order};

Variant nested(int p_arrays) {
    Array inner;
    for (int at = 1; at < p_arrays; ++at) {
        Array outer;
        outer.push_back(inner);
        inner = outer;
    }
    return inner;
}

Variant wide(int p_count) {
    Array list;
    list.resize(p_count);
    return list;
}

std::vector<uint8_t> nested_bytes(int p_arrays) {
    std::vector<uint8_t> out;
    for (int at = 1; at < p_arrays; ++at) {
        out.push_back(netw::value::TAG_ARRAY);
        out.push_back(1);
    }
    out.push_back(netw::value::TAG_ARRAY);
    out.push_back(0);
    return out;
}

std::vector<uint8_t> nested_dictionary_bytes(int p_dictionaries) {
    std::vector<uint8_t> out;
    for (int at = 1; at < p_dictionaries; ++at) {
        out.insert(out.end(), {netw::value::TAG_DICTIONARY, 1, 0});
    }
    out.push_back(netw::value::TAG_DICTIONARY);
    out.push_back(0);
    return out;
}

std::vector<uint8_t> wide_bytes(int p_count) {
    std::vector<uint8_t> out = {netw::value::TAG_ARRAY};
    for (uint32_t left = uint32_t(p_count); true; left >>= 7) {
        if (left < 0x80) {
            out.push_back(uint8_t(left));
            break;
        }
        out.push_back(uint8_t(left & 0x7f) | 0x80);
    }
    out.resize(out.size() + size_t(p_count), netw::value::TAG_NIL);
    return out;
}

std::vector<Stand> written() {
    using namespace netw::value;
    Array mixed;
    mixed.push_back(1);
    mixed.push_back(String("two"));
    mixed.push_back(nested(2));
    Array typed;
    typed.set_typed(Variant::INT, StringName(), Variant());
    typed.push_back(4);
    typed.push_back(-5);
    Dictionary map;
    map.set(String("b"), 1);
    map.set(2, Vector2(0.5, 1.5));
    Dictionary typed_map;
    typed_map.set_typed(
        Variant::STRING,
        StringName(),
        Variant(),
        Variant::INT,
        StringName(),
        Variant()
    );
    typed_map.set(String("score"), 7);
    PackedByteArray raw;
    raw.push_back(0);
    raw.push_back(255);
    PackedInt32Array int32s;
    int32s.push_back(-1);
    int32s.push_back(1 << 30);
    PackedInt64Array int64s;
    int64s.push_back(int64_t(1) << 40);
    PackedFloat32Array float32s;
    float32s.push_back(0.25f);
    PackedFloat64Array float64s;
    float64s.push_back(0.1);
    PackedStringArray strings;
    strings.push_back(String("ada"));
    strings.push_back(String());
    PackedVector2Array vector2s;
    vector2s.push_back(Vector2(1, 2));
    PackedVector3Array vector3s;
    vector3s.push_back(Vector3(1, 2, 3));
    PackedColorArray colors;
    colors.push_back(Color(0.5, 0.25, 0, 1));
    PackedVector4Array vector4s;
    vector4s.push_back(Vector4(1, 2, 3, 4));
    const Basis basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9));
    return {
        {"NIL", Variant(), TAG_NIL},
        {"FALSE", false, TAG_FALSE},
        {"TRUE", true, TAG_TRUE},
        {"INT", int64_t(-300000000000), TAG_INT},
        {"FLOAT", 0.1, TAG_FLOAT},
        {"FLOAT32", 0.5, TAG_FLOAT32},
        {"STRING", String::utf8("h\xc3\xa9llo"), TAG_STRING},
        {"STRING_NAME", StringName("hero"), TAG_STRING_NAME},
        {"NODE_PATH", NodePath(String("Level/Ada")), TAG_NODE_PATH},
        {"an absolute NODE_PATH with subnames",
         NodePath(String("/root/Level:position:x")),
         TAG_NODE_PATH},
        {"VECTOR2", Vector2(1.5, -2), TAG_VECTOR2},
        {"VECTOR2I", Vector2i(-7, 1 << 20), TAG_VECTOR2I},
        {"VECTOR3", Vector3(1, 2, -3), TAG_VECTOR3},
        {"VECTOR3I", Vector3i(1, -2, 3), TAG_VECTOR3I},
        {"VECTOR4", Vector4(1, 2, 3, 4), TAG_VECTOR4},
        {"VECTOR4I", Vector4i(1, 2, 3, INT32_MIN), TAG_VECTOR4I},
        {"QUATERNION", Quaternion(0, 0, 0.5, 0.5), TAG_QUATERNION},
        {"COLOR", Color(0.25, 0.5, 0.75, 1), TAG_COLOR},
        {"RECT2", Rect2(1, 2, 3, 4), TAG_RECT2},
        {"RECT2I", Rect2i(1, 2, 3, INT32_MAX), TAG_RECT2I},
        {"PLANE", Plane(0, 1, 0, 2), TAG_PLANE},
        {"AABB", AABB(Vector3(1, 2, 3), Vector3(4, 5, 6)), TAG_AABB},
        {"BASIS", basis, TAG_BASIS},
        {"TRANSFORM2D",
         Transform2D(Vector2(1, 2), Vector2(3, 4), Vector2(5, 6)),
         TAG_TRANSFORM2D},
        {"TRANSFORM3D", Transform3D(basis, Vector3(1, 2, 3)), TAG_TRANSFORM3D},
        {"PROJECTION",
         Projection(
             Vector4(1, 2, 3, 4),
             Vector4(5, 6, 7, 8),
             Vector4(9, 10, 11, 12),
             Vector4(13, 14, 15, 16)
         ),
         TAG_PROJECTION},
        {"ARRAY", mixed, TAG_ARRAY},
        {"TYPED_ARRAY", typed, TAG_TYPED_ARRAY},
        {"DICTIONARY", map, TAG_DICTIONARY},
        {"TYPED_DICTIONARY", typed_map, TAG_TYPED_DICTIONARY},
        {"BYTES", raw, TAG_BYTES},
        {"PACKED_INT32", int32s, TAG_PACKED_INT32},
        {"PACKED_INT64", int64s, TAG_PACKED_INT64},
        {"PACKED_FLOAT32", float32s, TAG_PACKED_FLOAT32},
        {"PACKED_FLOAT64", float64s, TAG_PACKED_FLOAT64},
        {"PACKED_STRING", strings, TAG_PACKED_STRING},
        {"PACKED_VECTOR2", vector2s, TAG_PACKED_VECTOR2},
        {"PACKED_VECTOR3", vector3s, TAG_PACKED_VECTOR3},
        {"PACKED_COLOR", colors, TAG_PACKED_COLOR},
        {"PACKED_VECTOR4", vector4s, TAG_PACKED_VECTOR4},
        {"17 arrays deep, the deepest held", nested(17), TAG_ARRAY},
        {"65,535 elements in one array, the most held", wide(65535), TAG_ARRAY},
    };
}

std::vector<Stand> malformed() {
    return {
        {"a tag past the last one", Variant(), {}, {39}},
        {"a VECTOR3 cut short", Variant(), {}, {11, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"an INT in more bytes than it needs", Variant(), {}, {3, 0x81, 0}},
        {"an INT past 64 bits",
         Variant(),
         {},
         {3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02}},
        {"a FLOAT that a FLOAT32 holds",
         Variant(),
         {},
         {4, 0, 0, 0, 0, 0, 0, 0xe0, 0x3f}},
        {"a STRING that is not UTF-8", Variant(), {}, {6, 2, 0xc3, 0x28}},
        {"a STRING holding a NUL", Variant(), {}, {6, 1, 0}},
        {"a NODE_PATH with an empty subname",
         Variant(),
         {},
         {8, 4, 'a', ':', ':', 'b'}},
        {"a NODE_PATH with a trailing slash", Variant(), {}, {8, 2, 'a', '/'}},
        {"a STRING opening with a byte order mark",
         Variant(),
         {},
         {6, 3, 0xef, 0xbb, 0xbf}},
        {"a STRING in overlong UTF-8", Variant(), {}, {6, 2, 0xc0, 0x80}},
        {"a STRING holding a surrogate",
         Variant(),
         {},
         {6, 3, 0xed, 0xa0, 0x80}},
        {"a STRING holding a code point past 0x10FFFF",
         Variant(),
         {},
         {6, 4, 0xf4, 0x90, 0x80, 0x80}},
        {"a STRING longer than its bytes", Variant(), {}, {6, 5, 'a'}},
        {"a BYTES longer than its bytes", Variant(), {}, {29, 5, 1}},
        {"a STRING length at 2^63",
         Variant(),
         {},
         {6, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}},
        {"a PACKED_INT32 counting more than its bytes",
         Variant(),
         {},
         {30, 3, 0, 0, 0, 0}},
        {"a DICTIONARY counting more than half its bytes",
         Variant(),
         {},
         {27, 3, 0, 0, 0, 0}},
        {"18 dictionaries deep", Variant(), {}, nested_dictionary_bytes(18)},
        {"a TYPED_DICTIONARY of NIL and NIL", Variant(), {}, {28, 0, 0, 0}},
        {"a TYPED_ARRAY whose element tag is TRUE", Variant(), {}, {26, 2, 0}},
        {"a TYPED_DICTIONARY whose key tag is FLOAT32",
         Variant(),
         {},
         {28, 5, 0, 0}},
        {"a TYPED_DICTIONARY of INT keys holding a STRING key",
         Variant(),
         {},
         {28, 3, 0, 1, 6, 0, 0}},
        {"a RECT2I component past 32 bits",
         Variant(),
         {},
         {18, 0x80, 0x80, 0x80, 0x80, 0x10, 0, 0, 0}},
        {"a VECTOR2I component past 32 bits",
         Variant(),
         {},
         {10, 0x80, 0x80, 0x80, 0x80, 0x10, 0}},
        {"a TYPED_ARRAY of INT holding a STRING",
         Variant(),
         {},
         {26, 3, 1, 6, 0}},
        {"a TYPED_ARRAY of no type", Variant(), {}, {26, 0, 0}},
        {"an ARRAY counting more than its bytes", Variant(), {}, {25, 5, 0}},
        {"18 arrays deep", Variant(), {}, nested_bytes(18)},
        {"65,536 elements in one array", Variant(), {}, wide_bytes(65536)},
    };
}

TEST_CASE("[Netw][Value][Hosted] a value reads back as the value written") {
    for (const Stand &stand : written()) {
        NETW_CELL(ROUND_TRIP, stand);
        NETW_LAW_HOLDS(ROUND_TRIP, stand);
    }
    for (const Stand &stand : malformed()) {
        NETW_CELL(REFUSED_BYTES, stand);
        NETW_LAW_HOLDS(REFUSED_BYTES, stand);
    }
    const Stand built[] = {
        {"a key written twice", Variant(), {}, {27, 2, 3, 2, 0, 3, 2, 1}},
        {"0.0 and -0.0 as two keys",
         Variant(),
         {},
         {27, 2, 5, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0x80, 0}},
        {"a StringName and a String of one text as two keys",
         Variant(),
         {},
         {27, 2, 6, 1, 'a', 0, 7, 1, 'a', 0}},
        {"two NaN keys",
         Variant(),
         {},
         {27, 2, 5, 0, 0, 0xc0, 0x7f, 0, 5, 0, 0, 0xc0, 0x7f, 1}},
    };
    for (const Stand &stand : built) {
        NETW_CELL(REFUSED_BUILD, stand);
        NETW_LAW_HOLDS(REFUSED_BUILD, stand);
    }
    {
        const Stand trailing
            = {"a NIL with a byte after it", Variant(), {}, {0, 0}};
        NETW_CELL(REFUSED_WHOLE, trailing);
        NETW_LAW_HOLDS(REFUSED_WHOLE, trailing);
    }
    const String marked = String::chr(0xFEFF) + String("hi");
    PackedStringArray marked_list;
    marked_list.push_back(String("ok"));
    marked_list.push_back(marked);
    Node *held = memnew(Node);
    Node *freed = memnew(Node);
    const Variant gone = freed;
    memdelete(freed);
    Array classed;
    classed.set_typed(Variant::OBJECT, StringName("Node"), Variant());
    const Stand refused[] = {
        {"an RID", RID(), {}, {}},
        {"a node", held, {}, {}},
        {"a Callable", Callable(), {}, {}},
        {"a Signal", Signal(), {}, {}},
        {"an empty Array[Node]", classed, {}, {}},
        {"18 arrays deep", nested(18), {}, {}},
        {"65,536 elements in one array", wide(65536), {}, {}},
        {"a String opening with a byte order mark", marked, {}, {}},
        {"a PackedStringArray opening with a byte order mark",
         marked_list,
         {},
         {}},
    };
    for (const Stand &stand : refused) {
        NETW_CELL(REFUSED_VALUE, stand);
        NETW_LAW_HOLDS(REFUSED_VALUE, stand);
    }
    Array holding;
    holding.push_back(1);
    holding.push_back(held);
    const Stand sent[] = {
        {"a null Object", Variant(static_cast<Object *>(nullptr)), {}, {}},
        {"a freed node", gone, {}, {}},
        {"a node", held, {}, {}, "an Object"},
        {"an empty Array[Node]", classed, {}, {}, "an Array typed to a class"},
        {"an Array holding a node",
         holding,
         {},
         {},
         "an Array holding an Object"},
        {"a Callable", Callable(), {}, {}, "a Callable"},
        {"a Signal", Signal(), {}, {}, "a Signal"},
        {"a String opening with a byte order mark",
         marked,
         {},
         {},
         "a character UTF-8 cannot hold"},
    };
    for (const Stand &stand : sent) {
        NETW_CELL(SENT_AS_NIL, stand);
        NETW_LAW_HOLDS(SENT_AS_NIL, stand);
    }
    memdelete(held);
    {
        const Stand orders
            = {"pairs inserted in two orders", Variant(), {}, {}};
        NETW_CELL(HASH_ORDER, orders);
        NETW_LAW_HOLDS(HASH_ORDER, orders);
    }
    {
        const Stand known
            = {"thirteen values of every common kind", Variant(), {}, {}};
        NETW_CELL(HASH_PINNED, known);
        NETW_LAW_HOLDS(HASH_PINNED, known);
    }
}

} // namespace TestNetwValue
