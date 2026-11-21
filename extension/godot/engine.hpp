#pragma once

#include "godot/object.hpp"
#include "godot/variant.hpp"

#if defined(NETW_MODULE)
#include "core/config/engine.h"
#include "core/core_bind.h"

namespace godot {
using ::Engine;
} // namespace godot
#elif defined(NETW_GDEXTENSION)
#include <godot_cpp/classes/engine.hpp>
#else
#error "Define NETW_MODULE or NETW_GDEXTENSION."
#endif

namespace netw::gd {

inline godot::Object *named_singleton(const godot::StringName &p_name) {
    godot::Engine *engine = godot::Engine::get_singleton();
    if (engine == nullptr || !engine->has_singleton(p_name)) {
        return nullptr;
    }
#if defined(NETW_MODULE)
    return engine->get_singleton_object(p_name);
#else
    return engine->get_singleton(p_name);
#endif
}

} // namespace netw::gd
