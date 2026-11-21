#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/variant.hpp"

namespace netw::spawn {

struct Anchor {
    bool entity_relative = false;
    int64_t route = 0;
    godot::String path;
};

struct NodeOp {
    enum Kind : uint8_t { BUILD, PLACE, MOVE, FREE, HIDE, DISCARD, ORPHAN };

    Kind kind = FREE;
    bool strict = false;
    bool requested = false;
    bool lingers = false;
    bool keeps_node = false;
    int64_t route = 0;
    int64_t sender = 0;
    godot::ObjectID node;
    Anchor parent;
    uint64_t revision = 0;
    uint64_t author = 0;
    godot::StringName reason;
};

} // namespace netw::spawn
