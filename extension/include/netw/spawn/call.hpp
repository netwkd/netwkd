#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/variant.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/promise.hpp"

namespace netw::spawn {

struct SpawnCall {
    godot::ObjectID host;
    godot::StringName method;
    godot::Array args;
    godot::Ref<NetwPlayer> owner;
    const char *verb = "";
    int64_t started_at = 0;
    bool waits = false;
};

struct SpawnRefusal {
    godot::Error code = godot::OK;
    godot::String detail;

    void refuse(godot::Error p_code, const godot::String &p_detail);
    godot::Ref<NetwPromise> promise() const;
    void settle(const godot::Ref<NetwPromise> &p_promise) const;
};

struct PendingSpawn {
    SpawnCall call;
    godot::Variant waiting;
    godot::Ref<NetwPromise> promise;
};

} // namespace netw::spawn
