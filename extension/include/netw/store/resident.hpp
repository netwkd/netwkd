#pragma once

#include <cstdint>

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/physics_body.hpp"
#include "godot/physics_server.hpp"
#include "godot/rid.hpp"
#include "godot/spatial_node.hpp"
#include "godot/templates.hpp"
#include "netw/held_bytes.hpp"

namespace netw::store {

struct NodeCell {
    godot::Object *object = nullptr;
    godot::RigidBody3D *solid = nullptr;
    godot::RigidBody2D *flat = nullptr;
    godot::Node3D *frame_3d = nullptr;
    godot::CanvasItem *frame_2d = nullptr;
    godot::PhysicsDirectBodyState3D *state_3d = nullptr;
    godot::RID body;
    uint64_t epoch = 0;
    bool in_tree = false;
    bool root = false;

    bool same_as(const NodeCell &p_other) const;
};

struct ResidentRows {
    godot::LocalVector<uint64_t> nodes;
    godot::LocalVector<NodeCell> cells;
    godot::LocalVector<uint64_t> scripts;
    bool ports_resolved = false;
};

class Residents {
    godot::HashSet<uint64_t> held;
    godot::LocalVector<ResidentRows> tables;
    uint64_t epoch = 1;
    bool watching = false;

public:
    static NodeCell derive(uint64_t p_node);
    static NodeCell derive_object(godot::Object *p_object);

    void watch(bool p_watching);

    bool is_watching() const {
        return watching;
    }

    int64_t held_bytes() const;

    ResidentRows &rows(uint32_t p_table);

    uint64_t node_of(uint32_t p_table, uint32_t p_row) const {
        return p_table < tables.size() && p_row < tables[p_table].nodes.size()
            ? tables[p_table].nodes[p_row]
            : 0;
    }

    void add_row(uint32_t p_table, uint64_t p_node);

    void swap_remove(uint32_t p_table, uint32_t p_row);

    void drop_rows() {
        tables.clear();
    }

    uint64_t current() const {
        return epoch;
    }

    bool holds(uint64_t p_node) const {
        return held.has(p_node);
    }

    void note_removed(uint64_t p_node);

    void clear();

    const NodeCell &resolve(
        uint64_t p_node,
        NodeCell &r_cell,
        bool p_validating
    );
};

} // namespace netw::store
