#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/variant.hpp"
#include "netw/subsystems.hpp"

namespace netw {

struct ObjectPort {
    godot::ObjectID id;
    int64_t lost = 0;

    bool bound() const;
    bool bind(godot::Object *p_owner);
    void unbind();
    godot::Object *resolve(SubsystemName p_module);
};

bool named_held(godot::Object *p_owner, const godot::StringName &p_key);

godot::Variant named_get(
    godot::Object *p_owner,
    const godot::StringName &p_key
);

void named_set(
    godot::Object *p_owner,
    const godot::StringName &p_key,
    const godot::Variant &p_value
);

godot::Variant port_get(godot::Object *p_owner, const godot::StringName &p_key);

bool port_rests(godot::Object *p_owner, const godot::StringName &p_key);

void port_set(
    godot::Object *p_owner,
    const godot::StringName &p_key,
    const godot::Variant &p_value
);

godot::Dictionary port_capture(
    ObjectPort &r_port,
    SubsystemName p_module,
    const godot::Array &p_keys
);

bool port_apply(
    ObjectPort &r_port,
    SubsystemName p_module,
    const godot::Dictionary &p_payload
);

#if defined(DEBUG_ENABLED)
void port_count_write(godot::Object *p_owner);
uint32_t port_writes_counted(godot::Object *p_owner);
void port_stop_counting(uint64_t p_owner);
#else
inline void port_count_write(godot::Object *) {
}
#endif

} // namespace netw
