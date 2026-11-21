#pragma once

#include "godot/variant.hpp"
#include "netw/display/decl.hpp"

namespace netw::display {

godot::Error set_param(Decl &decl, int param, const godot::Variant &value);
godot::Variant get_param(const Decl &decl, int param);

} // namespace netw::display
