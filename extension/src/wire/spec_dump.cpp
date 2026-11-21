#include "netw/wire/spec_dump.hpp"

#include "netw/carrier_frame.hpp"
#include "netw/interest/relay.hpp"
#include "netw/wire/control_record.hpp"
#include "netw/wire/frame.hpp"

using namespace godot;

namespace netw::wire {

namespace detail {

Dictionary dump_field(const char *name, const Spec &spec) {
    Dictionary out;
    out["name"] = String(name);
    out["kind"] = String(spec_kind_name(spec.kind));
    switch (spec.kind) {
        case SpecKind::BITS:
            out["width"] = spec.low;
            break;
        case SpecKind::INT_RANGE:
            out["low"] = spec.low;
            out["high"] = spec.high;
            out["width"]
                = int64_t(bits_required(uint64_t(spec.high - spec.low)));
            break;
        case SpecKind::VARUINT:
        case SpecKind::SVARINT:
            out["max_bytes"] = spec.low;
            break;
        case SpecKind::BOOL1:
            out["width"] = int64_t(1);
            break;
        case SpecKind::BYTES_CAPPED:
        case SpecKind::STRING:
            out["cap"] = spec.low;
            break;
    }
    return out;
}

} // namespace detail

Array dump_fields(const std::vector<FieldSpec> &fields) {
    Array out;
    for (const FieldSpec &field : fields) {
        out.push_back(detail::dump_field(field.name, field.spec));
    }
    return out;
}

namespace {

Dictionary dump_records(const std::vector<SpecRecord> &records) {
    Dictionary out;
    for (const SpecRecord &record : records) {
        out[String(record.name)] = dump_fields(record.fields);
    }
    return out;
}

} // namespace

Dictionary frame_spec_records() {
    return dump_records(frame_spec_rows());
}

Dictionary control_spec_records() {
    return dump_records(control_spec_rows());
}

Dictionary carrier_spec_records() {
    Dictionary out;
    out["DatagramReliable"] = spec_dump(DatagramHead::reliable_wire);
    out["DatagramUnreliable"] = spec_dump(DatagramHead::unreliable_wire);
    out["DatagramAcked"] = spec_dump(DatagramHead::acked_wire);
    out["DatagramAckedHeld"] = spec_dump(DatagramHead::acked_wire);
    return out;
}

Dictionary awareness_spec_records() {
    Dictionary out;
    out["AwarenessEdge"] = spec_dump(interest::AwarenessEdge::wire);
    return out;
}

} // namespace netw::wire
