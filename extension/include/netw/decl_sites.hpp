#pragma once

#include "godot/callable.hpp"
#include "godot/object.hpp"
#include "godot/script_backtrace.hpp"
#include "godot/variant.hpp"
#include "netw_faults.h"

namespace netw::decl_sites {

void record(
    netw_fault_site p_kind,
    godot::Object *p_declarer,
    const godot::StringName &p_member = godot::StringName()
);
void record_handler(const godot::Callable &p_handler);
gd::ScriptSite find(
    netw_fault_site p_kind,
    godot::Object *p_declarer,
    const godot::StringName &p_member = godot::StringName()
);
void clear();

} // namespace netw::decl_sites
