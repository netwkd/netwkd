#pragma once

#include <cstdint>

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "godot/variant.hpp"
#include "netw/api/member_config.hpp"
#include "netw/repl/property_set.hpp"

namespace netw::property_set_builder {

void for_record(repl::PropertySet &r_set, int64_t p_record);

void from_property_config(
    repl::PropertySet &r_set,
    const godot::StringName &p_property,
    const godot::Ref<NetwMemberConfig> &p_config
);

bool from_property_configs(
    repl::PropertySet &r_set,
    const godot::Dictionary &p_configs,
    int64_t p_record
);

bool compile_node(
    repl::PropertySet &r_set,
    godot::Node *p_node,
    int64_t p_record
);

uint64_t from_node(godot::Node *p_node, int64_t p_record, godot::Object *p_api);

} // namespace netw::property_set_builder
