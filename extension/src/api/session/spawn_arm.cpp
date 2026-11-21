#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/scene_tree.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/colors.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/profile.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"

using namespace godot;

namespace netw {

namespace {

const char *SIG_TREE_ENTERED = "tree_entered";

void gather_nested_boundaries(Node *p_root, LocalVector<Node *> &r_out) {
    if (p_root == nullptr) {
        return;
    }
    for (int at = 0; at < p_root->get_child_count(); ++at) {
        Node *child = p_root->get_child(at);
        if (spawn::Pipeline::roots_published_entity(child)) {
            r_out.push_back(child);
        } else {
            gather_nested_boundaries(child, r_out);
        }
    }
}

void connect_entry_once(
    Object *p_source,
    const StringName &p_signal,
    const Callable &p_callable,
    uint32_t p_flags
) {
    if (p_source == nullptr || p_source->is_connected(p_signal, p_callable)) {
        return;
    }
    p_source->connect(p_signal, p_callable, p_flags);
}

} // namespace

Ref<NetwEntity> NetwMultiplayer::spawn_arm(
    spawn::Record *p_record,
    Node *p_node
) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return Ref<NetwEntity>();
    }
    const Ref<NetwEntity> entity = spawn_arm_identity(
        p_record,
        p_node,
        callable_mp(this, &NetwMultiplayer::spawn_declare_stage)
    );
    if (entity.is_null()) {
        return entity;
    }
    const int64_t route = entity->get_route();
    pipeline->arm(*p_record);
    if (entity->get_stage() == int64_t(netw::entity::Stage::UNBOUND)) {
        entity->arm(this);
    }
    persist_bind_spawned(p_node);
    if (p_node->is_inside_tree()) {
        entity_go_live(entity->get_rid_handle());
        spawn_schedule_armed_flush(route);
    } else {
        pipeline->await_entry(route, p_node);
        connect_entry_once(
            p_node,
            SIG_TREE_ENTERED,
            callable_mp(this, &NetwMultiplayer::spawn_on_armed_tree_entered)
                .bind(route),
            Object::CONNECT_ONE_SHOT
        );
    }
    return entity;
}

void NetwMultiplayer::spawn_schedule_armed_flush(int64_t p_route) {
    session_defer(
        callable_mp(this, &NetwMultiplayer::spawn_flush_armed_spawn)
            .bind(p_route),
        StringName(vformat("spawn-armed-flush?%d", p_route))
    );
}

void NetwMultiplayer::spawn_on_armed_tree_entered(int64_t p_route) {
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        pipeline->entered(p_route);
        spawn_schedule_armed_flush(p_route);
    }
}

void NetwMultiplayer::spawn_flush_armed_spawn(int64_t p_route) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    spawn::Ops ops;
    const bool issued = pipeline->flush_armed_spawn(p_route, ops);
    spawn_act(ops);
    if (issued) {
        spawn_reconcile_nested(p_route);
    }
}

Ref<NetwEntity> NetwMultiplayer::spawn_adopt_in_place(Node *p_root) {
    if (!is_host() || p_root == nullptr || !p_root->is_inside_tree()) {
        return Ref<NetwEntity>();
    }
    spawn::Record record;
    record.set_recipe(spawn::Book::RECIPE_ADOPT);
    record.bind_origin(scene_outer_of(p_root)->get_parent());
    const Ref<NetwEntity> entity = spawn_arm(&record, p_root);
    if (entity.is_valid()) {
        liveness_bind_route(record.get_route(), entity.ptr());
        spawn_flush_armed_spawn(record.get_route());
    }
    return entity;
}

bool NetwMultiplayer::spawn_readmit(
    Node *p_node,
    const spawn::Record &p_recipe
) {
    if (spawn_plane() == nullptr || !is_host() || p_node == nullptr
        || !p_node->is_inside_tree()) {
        return false;
    }
    spawn::Record record = p_recipe;
    record.set_route(0);
    record.set_parent_route(0);
    record.set_recipients(PackedInt32Array());
    record.bind_node(p_node);
    record.bind_origin(p_node->get_parent());
    return spawn_arm(&record, p_node).is_valid();
}

Ref<NetwEntity> NetwMultiplayer::spawn_arm_consumed(
    Node *p_node,
    Object *p_spawner,
    int p_scene_index,
    const Variant &p_data
) {
    if (spawn_plane() == nullptr) {
        return Ref<NetwEntity>();
    }
    NetwEntity::settle_parts(p_node, true);
    spawn::Record record;
    record.set_recipe(spawn::Book::RECIPE_SPAWNER);
    record.bind_spawner(Object::cast_to<MultiplayerSpawner>(p_spawner));
    record.set_scene_index(p_scene_index);
    record.set_custom_data(p_data);
    return spawn_arm(&record, p_node);
}

void NetwMultiplayer::spawn_publish_nested(Node *p_node) {
    spawn::Pipeline *pipeline = spawn_plane();
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    if (entity.is_null() || p_node == nullptr || !p_node->is_inside_tree()) {
        return;
    }
    const int64_t route = entity->get_route();
    if (route <= 0 || pipeline->is_booked(route)) {
        return;
    }
    spawn::Record record;
    record.set_recipe(spawn::Book::RECIPE_ADOPT);
    record.bind_origin(scene_outer_of(p_node)->get_parent());
    if (spawn_arm(&record, p_node).is_null()) {
        return;
    }
    liveness_bind_route(record.get_route(), entity.ptr());
    pipeline->count_nested_published();
    spawn_flush_armed_spawn(record.get_route());
}

void NetwMultiplayer::spawn_reconcile_nested(int64_t p_owner_route) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr || !is_host()) {
        return;
    }
    const spawn::Record *owner = pipeline->spawned_record(p_owner_route);
    Node *root = owner != nullptr ? owner->node() : nullptr;
    if (root == nullptr || !root->is_inside_tree()) {
        return;
    }
    LocalVector<Node *> boundaries;
    gather_nested_boundaries(root, boundaries);
    for (uint32_t at = 0; at < boundaries.size(); ++at) {
        spawn_publish_nested(boundaries[at]);
    }
}

void NetwMultiplayer::spawn_reconcile_nested_child(int64_t p_route) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr || !is_host()) {
        return;
    }
    Node *node = wrapper_owner(liveness_core.rid_from_route(int(p_route)));
    if (node == nullptr || pipeline->is_booked(p_route)) {
        return;
    }
    for (Node *above = node->get_parent(); above != nullptr;
         above = above->get_parent()) {
        if (!spawn::Pipeline::roots_published_entity(above)) {
            continue;
        }
        const Ref<NetwEntity> owner = NetwEntity::of(above);
        if (pipeline->holds_spawned_route(owner->get_route())) {
            spawn_publish_nested(node);
        }
        return;
    }
    spawn_publish_placed(node, p_route);
}

void NetwMultiplayer::spawn_publish_placed(Node *p_node, int64_t p_route) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (!session_is_online()) {
        return;
    }
    const bool placed = pipeline->take_placed_before_host(p_route);
    const SceneTree *tree = gd::scene_tree();
    const bool owned = placed || p_node->get_owner() != nullptr
        || (tree != nullptr && tree->get_current_scene() == p_node);
    const bool isolated
        = scene_root_is_isolated(p_node) && scene_world_of(p_node) == nullptr;
    if (owned && !isolated) {
        spawn_publish_nested(p_node);
        return;
    }
    if (!pipeline->refuse_placed_once(uint64_t(p_node->get_instance_id()))) {
        return;
    }
    if (owned) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_REFUSED,
            {String("'") + String(p_node->get_name()) + String("'"),
             String("an isolated scene placed in the editor"),
             String("{Netw.spawn} for it, or isolation turned off")},
            uint64_t(gd::instance_id(p_node))
        );
        return;
    }
    fault::raise(
        this,
        NETW_FAULT_SPAWN_REFUSED,
        {String("'") + String(p_node->get_name()) + String("'"),
         String("a node added to the tree without {Netw.spawn}"),
         String("{Netw.spawn} for it")},
        uint64_t(gd::instance_id(p_node))
    );
}

void NetwMultiplayer::spawn_note_nested_candidate(int64_t p_route) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr || !is_host() || pipeline->is_booked(p_route)) {
        return;
    }
    session_defer(
        callable_mp(this, &NetwMultiplayer::spawn_reconcile_nested_child)
            .bind(p_route),
        StringName(vformat("spawn-nested?%d", p_route))
    );
}

} // namespace netw
