#pragma once

#include "godot/variant.hpp"
#include "netw/wire/describe.hpp"

namespace netw::wire {

namespace detail {

godot::Dictionary dump_field(const char *name, const Spec &spec);

} // namespace detail

godot::Array dump_fields(const std::vector<FieldSpec> &fields);

template <class... Fields>
godot::Array spec_dump(const Description<Fields...> &description) {
    return dump_fields(field_specs(description));
}

godot::Dictionary frame_spec_records();
godot::Dictionary control_spec_records();
godot::Dictionary carrier_spec_records();
godot::Dictionary awareness_spec_records();

} // namespace netw::wire

namespace netw::session {

godot::Dictionary frame_spec_records();

} // namespace netw::session
