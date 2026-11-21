#include "netw/call_args.hpp"

#include <utility>

#include "godot/object.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/profile.hpp"
#include "netw/value_variant.hpp"
#include "netw/wire/frame.hpp"

using namespace godot;

namespace netw::call_args {

namespace {

struct Offense {
    String held;
    String fix;
};

bool exact_text(const String &p_text) {
    const CharString utf8 = p_text.utf8();
    return value::is_exact_text(
        reinterpret_cast<const uint8_t *>(utf8.get_data()),
        utf8.length()
    );
}

bool path_text(const String &p_text) {
    const CharString utf8 = p_text.utf8();
    return value::is_node_path(
        reinterpret_cast<const uint8_t *>(utf8.get_data()),
        utf8.length()
    );
}

bool offense_in(const Variant &p_value, int p_depth, Offense &r_offense);

bool offense_inside(
    const char *p_container,
    const Variant &p_element,
    int p_depth,
    Offense &r_offense
) {
    Offense inner;
    if (!offense_in(p_element, p_depth + 1, inner)) {
        return false;
    }
    r_offense.held = vformat("%s holding %s", p_container, inner.held);
    r_offense.fix = inner.fix;
    return true;
}

bool offense_in(const Variant &p_value, int p_depth, Offense &r_offense) {
    if (p_depth > value::MAX_DEPTH) {
        return false;
    }
    const String text_fix = String("text without them");
    switch (p_value.get_type()) {
        case Variant::OBJECT:
            if (gd::live_object(p_value) == nullptr) {
                return false;
            }
            r_offense = {"an Object", "the object's data or its node path"};
            return true;
        case Variant::RID:
            r_offense = {"an RID", "the data the RID names"};
            return true;
        case Variant::CALLABLE:
            r_offense = {"a Callable", "the method's name"};
            return true;
        case Variant::SIGNAL:
            r_offense = {"a Signal", "the signal's name"};
            return true;
        case Variant::NODE_PATH:
            if (exact_text(String(p_value)) && !path_text(String(p_value))) {
                r_offense
                    = {"a NodePath whose text does not read back as itself",
                       "a NodePath built from text with no empty name"};
                return true;
            }
            [[fallthrough]];
        case Variant::STRING:
        case Variant::STRING_NAME:
            if (exact_text(String(p_value))) {
                return false;
            }
            r_offense
                = {"text with a NUL, a byte order mark at its start or a "
                   "character UTF-8 cannot hold",
                   text_fix};
            return true;
        case Variant::PACKED_STRING_ARRAY: {
            const PackedStringArray held = p_value;
            for (int64_t at = 0; at < held.size(); ++at) {
                if (!exact_text(held[at])) {
                    r_offense
                        = {"a PackedStringArray holding text with a NUL, "
                           "a byte order mark or a character UTF-8 "
                           "cannot hold",
                           text_fix};
                    return true;
                }
            }
            return false;
        }
        case Variant::ARRAY: {
            const Array list = p_value;
            if (list.is_typed()
                && Variant::Type(list.get_typed_builtin()) == Variant::OBJECT) {
                r_offense
                    = {"an Array typed to a class",
                       "an untyped Array, or an Array of node paths"};
                return true;
            }
            for (int at = 0; at < list.size(); ++at) {
                if (offense_inside("an Array", list[at], p_depth, r_offense)) {
                    return true;
                }
            }
            return false;
        }
        case Variant::DICTIONARY: {
            const Dictionary map = p_value;
            const bool classed
                = Variant::Type(map.get_typed_key_builtin()) == Variant::OBJECT
                || Variant::Type(map.get_typed_value_builtin())
                    == Variant::OBJECT;
            if (classed) {
                r_offense
                    = {"a Dictionary typed to a class",
                       "an untyped Dictionary, or one of node paths"};
                return true;
            }
            const Array keys = map.keys();
            for (int at = 0; at < keys.size(); ++at) {
                if (offense_inside("a Dictionary", keys[at], p_depth, r_offense)
                    || offense_inside(
                        "a Dictionary",
                        map[keys[at]],
                        p_depth,
                        r_offense
                    )) {
                    return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

Offense unsendable(const Variant &p_value, Error p_refusal) {
    if (p_refusal == ERR_CYCLIC_LINK) {
        return {"a value nested more than 16 deep", "a value nested less"};
    }
    if (p_refusal == ERR_OUT_OF_MEMORY) {
        return {
            "more than 65,536 elements",
            "a value of at most 65,536 elements"
        };
    }
    Offense found;
    if (!offense_in(p_value, 0, found)) {
        found = {"a value netwkd cannot send", "a plain value"};
    }
    return found;
}

void raise_unsendable(
    NetwMultiplayer *p_session,
    const String &p_name,
    const Variant &p_value,
    Error p_refusal
) {
    const Offense offense = unsendable(p_value, p_refusal);
    fault::raise(
        p_session,
        NETW_FAULT_UNSENDABLE_VALUE,
        {p_name, offense.held, offense.fix}
    );
}

bool raw_write(
    wire::WriteStream &p_stream,
    const Variant &p_value,
    int p_index,
    NetwMultiplayer *p_session
) {
    static thread_local value::Bytes bytes;
    bytes.clear();
    const Error refusal = value::encode(p_value, bytes);
    if (refusal != OK) {
        value::write_nil(bytes);
    }
    bool written = false;
    if (bytes.size() > uint32_t(VALUE_CAP)) {
        PackedByteArray oversized;
        oversized.resize(int64_t(bytes.size()));
        written = p_stream.bytes_capped(oversized, VALUE_CAP);
    } else {
        written = p_stream.align_verify()
            && p_stream.raw_span(bytes.ptr(), int64_t(bytes.size()));
    }
    if (refusal != OK) {
        raise_unsendable(
            p_session,
            vformat("Value %d", p_index),
            p_value,
            refusal
        );
    }
    return written;
}

bool raw_read(wire::ReadStream &p_stream, Variant &r_value) {
    const uint8_t *bytes = nullptr;
    int64_t size = 0;
    if (!p_stream.aligned_rest(bytes, size)) {
        return false;
    }
    const int64_t taken
        = value::decode(bytes, MIN(size, int64_t(VALUE_CAP)), r_value);
    return taken >= 0 && p_stream.skip_bytes(taken);
}

bool node_write(wire::WriteStream &p_stream, const NodeRef &p_node) {
    uint64_t route = uint64_t(MAX(p_node.route, int64_t(0)));
    uint64_t comp = uint64_t(p_node.comp) & 0xFF;
    if (!p_stream.varuint(route, 5) || !p_stream.bits(comp, 8)) {
        return false;
    }
    if (comp != wire::FRAME_COMP_PATH) {
        return true;
    }
    String path = p_node.path;
    return wire::string_field(p_stream, path);
}

bool node_read(wire::ReadStream &p_stream, NodeRef &r_node) {
    uint64_t route = 0;
    uint64_t comp = 0;
    if (!p_stream.varuint(route, 5) || !p_stream.bits(comp, 8)) {
        return false;
    }
    r_node.route = int64_t(route);
    r_node.comp = int64_t(comp);
    if (comp != wire::FRAME_COMP_PATH) {
        return true;
    }
    return wire::string_field(p_stream, r_node.path);
}

bool quantized_write(
    wire::WriteStream &p_stream,
    const Variant &p_value,
    const Ref<NetwQuantize> &p_quantizer
) {
    return p_quantizer->write(p_stream, p_value);
}

bool quantized_read(
    wire::ReadStream &p_stream,
    Variant::Type p_type,
    const Ref<NetwQuantize> &p_quantizer,
    Variant &r_value
) {
    return p_quantizer->read(p_stream, p_type, r_value);
}

bool slot_is_quantized(
    const Slot &p_slot,
    const Ref<NetwQuantize> &p_quantizer,
    Variant::Type p_declared
) {
    return p_quantizer.is_valid() && p_quantizer->supports_type(p_declared)
        && p_quantizer->supports_type(p_slot.value.get_type());
}

} // namespace

void report_unsendable(
    NetwMultiplayer *p_session,
    const String &p_name,
    const Variant &p_value,
    Error p_refusal
) {
    raise_unsendable(p_session, p_name, p_value, p_refusal);
}

Slot of_value(const Variant &p_value) {
    Slot out;
    out.value = p_value;
    return out;
}

PackedByteArray value_bytes(
    const Variant &p_value,
    const String &p_name,
    NetwMultiplayer *p_session
) {
    PackedByteArray bytes;
    const Error refusal = value::encode_packed(p_value, bytes);
    if (refusal != OK) {
        raise_unsendable(p_session, p_name, p_value, refusal);
    }
    return bytes;
}

Slot of_node(int64_t p_route, int64_t p_comp, const String &p_path) {
    Slot out;
    out.addresses_node = true;
    out.node.route = p_route;
    out.node.comp = p_comp;
    out.node.path = p_path;
    return out;
}

Ref<NetwQuantize> quantizer_at(const Array &p_quantizers, int p_index) {
    if (p_index >= p_quantizers.size()
        || p_quantizers[p_index].get_type() == Variant::NIL) {
        return Ref<NetwQuantize>();
    }
    return p_quantizers[p_index];
}

int type_at(const Array &p_types, int p_index) {
    if (p_index >= p_types.size()
        || p_types[p_index].get_type() != Variant::INT) {
        return int(Variant::NIL);
    }
    return int(p_types[p_index]);
}

bool write(
    wire::WriteStream &p_stream,
    const LocalVector<Slot> &p_slots,
    const Array &p_quantizers,
    const Array &p_types,
    NetwMultiplayer *p_session
) {
    NETW_ZONE_LEAF("Call args write", colors::CODEC, profile::SUBSYSTEM_WIRE);
    NETW_ZONE_VALUE(int64_t(p_slots.size()));
    uint64_t count = uint64_t(p_slots.size());
    if (!p_stream.varuint(count, COUNT_BYTES)) {
        return false;
    }
    for (uint32_t at = 0; at < p_slots.size(); ++at) {
        const Slot &slot = p_slots[at];
        const Ref<NetwQuantize> quantizer = quantizer_at(p_quantizers, int(at));
        const Variant::Type declared
            = static_cast<Variant::Type>(type_at(p_types, int(at)));
        const bool quantized = !slot.addresses_node
            && slot_is_quantized(slot, quantizer, declared);
        int64_t kind = slot.addresses_node
            ? KIND_NODE_REF
            : (quantized ? KIND_QUANTIZED : KIND_RAW);
        if (!p_stream.int_range(kind, KIND_RAW, KIND_NODE_REF)) {
            return false;
        }
        if (slot.addresses_node) {
            if (!node_write(p_stream, slot.node)) {
                return false;
            }
            continue;
        }
        const bool placed = quantized
            ? quantized_write(p_stream, slot.value, quantizer)
            : raw_write(p_stream, slot.value, int(at), p_session);
        if (!placed) {
            return false;
        }
    }
    return true;
}

bool read(
    wire::ReadStream &p_stream,
    const Array &p_quantizers,
    const Array &p_types,
    LocalVector<Slot> &r_slots
) {
    NETW_ZONE_LEAF("Call args read", colors::CODEC, profile::SUBSYSTEM_WIRE);
    uint64_t count = 0;
    if (!p_stream.varuint(count, COUNT_BYTES)) {
        return false;
    }
    NETW_ZONE_VALUE(int64_t(count));
    LocalVector<Slot> staged;
    staged.reserve(uint32_t(count));
    for (uint64_t at = 0; at < count; ++at) {
        int64_t kind = 0;
        if (!p_stream.int_range(kind, KIND_RAW, KIND_NODE_REF)) {
            return false;
        }
        if (kind == KIND_NODE_REF) {
            Slot slot;
            slot.addresses_node = true;
            if (!node_read(p_stream, slot.node)) {
                return false;
            }
            staged.push_back(slot);
            continue;
        }
        Variant value;
        const Ref<NetwQuantize> quantizer = quantizer_at(p_quantizers, int(at));
        const bool taken = kind == KIND_QUANTIZED
            ? quantizer.is_valid()
                && quantized_read(
                    p_stream,
                    static_cast<Variant::Type>(type_at(p_types, int(at))),
                    quantizer,
                    value
                )
            : raw_read(p_stream, value);
        if (!taken) {
            return false;
        }
        staged.push_back(of_value(value));
    }
    r_slots = staged;
    return true;
}

bool values_write(
    wire::WriteStream &p_stream,
    const Array &p_values,
    const Array &p_quantizers,
    const Array &p_types,
    NetwMultiplayer *p_session
) {
    LocalVector<Slot> slots;
    slots.reserve(uint32_t(p_values.size()));
    for (int at = 0; at < p_values.size(); ++at) {
        slots.push_back(of_value(p_values[at]));
    }
    return write(p_stream, slots, p_quantizers, p_types, p_session);
}

bool values_read(
    wire::ReadStream &p_stream,
    const Array &p_quantizers,
    const Array &p_types,
    Array &r_values
) {
    LocalVector<Slot> slots;
    if (!read(p_stream, p_quantizers, p_types, slots)) {
        return false;
    }
    Array out;
    for (uint32_t at = 0; at < slots.size(); ++at) {
        out.push_back(slots[at].value);
    }
    r_values = out;
    return true;
}

} // namespace netw::call_args
