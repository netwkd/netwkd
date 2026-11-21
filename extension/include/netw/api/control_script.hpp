#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "netw/entity/control.hpp"

namespace netw::entity {

void declare_policy(
    godot::Object *p_script,
    const godot::StringName &p_name,
    int64_t p_policy,
    bool p_is_signal
);

bool script_admits(
    godot::Object *p_node,
    const godot::StringName &p_name,
    bool p_is_signal,
    int64_t p_sender,
    int64_t p_controller
);

} // namespace netw::entity
