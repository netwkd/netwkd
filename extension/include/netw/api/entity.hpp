#pragma once

#include <cstdint>

#include "godot/multiplayer.hpp"
#include "godot/multiplayer_synchronizer.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/rid.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/scene.hpp"
#include "netw/comp_table.hpp"
#include "netw/entity/stage.hpp"
#include "netw/repl/property_binding.hpp"
#include "netw/session/frames.hpp"

namespace netw_test {
struct Internals;
} // namespace netw_test

namespace netw {

class NetwDisplay;
class NetwInterest;
class NetwMultiplayer;
class NetwPersistence;
class NetwPrediction;
class NetwSimulation;
class ReplicationCore;
struct PeerRename;

class NetwEntity : public godot::RefCounted {
    GDCLASS(NetwEntity, godot::RefCounted)

public:
    enum Authority {
        AUTHORITY_SERVER = int(entity::Control::Authority::SERVER),
        AUTHORITY_CONTROLLER = int(entity::Control::Authority::CONTROLLER),
    };

    enum Transfer {
        TRANSFER_FIXED = int(entity::Control::Transfer::FIXED),
        TRANSFER_REQUESTABLE = int(entity::Control::Transfer::REQUESTABLE),
        TRANSFER_IMMEDIATE = int(entity::Control::Transfer::IMMEDIATE),
    };

    enum DisconnectRule {
        DISCONNECT_REVERT_TO_SERVER
        = int(entity::Control::DisconnectRule::REVERT_TO_SERVER),
        DISCONNECT_DESPAWN = int(entity::Control::DisconnectRule::DESPAWN),
    };

    enum ParentDespawnRule {
        PARENT_DESPAWN_CASCADE
        = int(entity::Control::ParentDespawnRule::CASCADE),
        PARENT_DESPAWN_DETACH = int(entity::Control::ParentDespawnRule::DETACH),
    };

    enum Lifecycle {
        LIFECYCLE_SERVER = int(entity::Control::Lifecycle::SESSION),
        LIFECYCLE_CONTROLLER = int(entity::Control::Lifecycle::CONTROLLER),
    };

    enum Stage {
        STAGE_UNBOUND = int(entity::Stage::UNBOUND),
        STAGE_ARMED = int(entity::Stage::ARMED),
        STAGE_LIVE = int(entity::Stage::LIVE),
        STAGE_DESPAWNING = int(entity::Stage::DESPAWNING),
        STAGE_LINGERING = int(entity::Stage::LINGERING),
        STAGE_FREED = int(entity::Stage::FREED),
    };

    using Hold = entity::Control::Hold;

private:
    friend class NetwMultiplayer;
#if defined(NETW_TESTS)
    friend struct netw_test::Internals;
#endif

    NetwEntityRecord *draft = nullptr;
    NetwMultiplayer *home = nullptr;
    godot::RID attached;
    godot::ObjectID owner_id;
    godot::ObjectID session_id;
    godot::Ref<NetwPlayer> written_view;
    int64_t written_player = 0;
    godot::TypedArray<godot::MultiplayerSynchronizer> synchronizers_cache;
    bool synchronizers_dirty = true;
    bool synchronizers_absent = false;
    bool ready_once_fired = false;
    bool owner_exiting_tree = false;
    bool despawning_watched = false;
    godot::RID scene_subject;

    NetwEntityRecord *get_record() const;
    void readmitted_ready();
    int64_t action_spawn_tick = -1;
    int64_t action_requester = 0;
    godot::Callable control_handler;
    godot::Callable lifecycle_handler;

    entity::Control *control() const;
    NetwMultiplayer *session_core() const;
    void unwrite_player();
    int64_t local_peer() const;
    void transition(int64_t p_stage);
    repl::PropertyBindingPtr derived_binding(int64_t p_record) const;

protected:
    static void _bind_methods();

public:
    NetwEntity();
    ~NetwEntity();

    static godot::Ref<NetwEntity> of(godot::Node *p_node);
    static godot::Ref<NetwEntity> ensure(godot::Node *p_root);
    static godot::Ref<NetwEntity> resolve(godot::Node *p_node);
    static godot::Ref<NetwEntity> declare(godot::Node *p_node);
    static void settle_parts(godot::Node *p_root, bool p_spawned);

    godot::Node *get_owner() const;
    void set_owner(godot::Node *p_owner);

    godot::StringName get_name() const;
    void set_name(const godot::StringName &p_name);
    int64_t get_peer_id() const;
    void set_peer_id(int64_t p_peer_id);
    int64_t get_player_id() const;
    void set_player_id(int64_t p_player_id);
    int64_t get_route() const;
    void set_route(int64_t p_route);
    godot::RID get_rid_handle() const;

    godot::Ref<godot::MultiplayerAPI> get_multiplayer() const;

    void stamp_multiplayer(const godot::Ref<NetwMultiplayer> &p_api);

    godot::Callable get_control_handler() const {
        return control_handler;
    }
    void set_control_handler(const godot::Callable &p_handler);
    void judge_control(const godot::Variant &p_request) const;
    godot::Callable get_lifecycle_handler() const {
        return lifecycle_handler;
    }
    void set_lifecycle_handler(const godot::Callable &p_handler);
    void judge_lifecycle(const godot::Variant &p_request) const;

    Transfer get_transfer() const;
    void set_transfer(Transfer p_value);
    Authority get_authority() const;
    void set_authority(Authority p_value);
    DisconnectRule get_on_controller_disconnect() const;
    void set_on_controller_disconnect(DisconnectRule p_value);
    ParentDespawnRule get_on_parent_despawn() const;
    void set_on_parent_despawn(ParentDespawnRule p_value);
    Lifecycle get_lifecycle() const;
    void set_lifecycle(Lifecycle p_value);
    bool has_structure_ops() const {
        return !get_record()->get_control_book().structure_ops.is_empty();
    }
    uint32_t structure_ops_outstanding() const {
        return get_record()->get_control_book().structure_ops.size();
    }
    bool get_declares_scene() const;
    void set_declares_scene(bool p_value);
    godot::StringName get_scene_label() const;
    void set_scene_label(const godot::StringName &p_value);
    int64_t get_scene_isolation() const;
    void set_scene_isolation(int64_t p_value);

    int64_t get_controller() const;
    int64_t steering() const;
    void set_controller(int64_t p_value);
    bool get_is_controlled_locally() const;
    godot::Ref<NetwPlayer> get_controller_player() const;
    void set_controller_player(const godot::Ref<NetwPlayer> &p_player);
    int64_t get_action_spawn_tick() const {
        return action_spawn_tick;
    }
    void set_action_spawn_tick(int64_t p_tick) {
        action_spawn_tick = p_tick;
    }
    int64_t get_action_requester() const {
        return action_requester;
    }
    void set_action_requester(int64_t p_peer) {
        action_requester = p_peer;
    }

    godot::Ref<NetwPromise> claim_control(
        Hold p_hold = entity::Control::HOLD_EXCLUSIVE
    );
    godot::Ref<NetwPromise> release_control(
        int64_t p_successor = entity::Control::SERVER
    );
    godot::Error transfer_control(int64_t p_peer);
    bool is_controller_here() const;
    bool is_claim_pending() const;
    bool is_claim_running_ahead() const;
    Hold get_hold() const;
    bool get_is_control_pending() const;
    uint64_t get_control_revision() const;
    uint64_t get_control_tenure() const;

    void set_replication_priority(double p_weight);
    double get_replication_priority() const;

    bool is_on_server() const;
    godot::Ref<NetwPlayer> get_player() const;
    void set_player(const godot::Ref<NetwPlayer> &p_player);
    godot::Ref<NetwPlayer> unbound_player() const;
    bool has_unbound_player() const {
        return written_player != 0;
    }
    bool get_is_player() const;

    Stage get_stage() const;
    godot::Ref<NetwDespawnOpts> get_active_despawn_opts() const;

    bool publish_stage(int64_t p_from, int64_t p_to);

    void arm(const godot::Ref<NetwMultiplayer> &p_api);

    void despawn(const godot::Ref<NetwDespawnOpts> &p_opts);

    void register_component(godot::Node *p_component);
    godot::NodePath relative_path(
        godot::Node *p_source,
        godot::Node *p_target
    ) const;
    int64_t comp_of(godot::Node *p_node) const;
    godot::String comp_path_of(godot::Node *p_node) const;
    godot::Node *comp_node_of(int64_t p_comp) const;
    bool has_comp_path(const godot::NodePath &p_path) const;
    bool get_comps_poisoned() const;

    NetwCompTable &comp_table();
    const NetwCompTable &comp_table() const;
    godot::NodePath property_path(
        godot::Node *p_source,
        const godot::StringName &p_property,
        godot::Node *p_base
    ) const;
    repl::PropertyBindingPtr get_state_binding() const;
    repl::PropertyBindingPtr get_input_binding() const;
    repl::PropertyBindingPtr get_broadcast_binding() const;
    godot::Ref<NetwInterest> get_interest() const;
    godot::Ref<NetwInterest> held_interest() const;
    godot::Ref<NetwScene> get_scene() const;
    godot::Ref<NetwPrediction> get_prediction() const;
    godot::Ref<NetwSimulation> get_simulation() const;
    godot::Ref<NetwPrediction> prediction_if_minted() const;
    godot::Ref<NetwSimulation> simulation_if_minted() const;
    godot::Ref<NetwPersistence> get_persistence() const;
    godot::Ref<NetwDisplay> get_display() const;

    godot::TypedArray<godot::MultiplayerSynchronizer> synchronizers();
    bool governs_property(
        const godot::NodePath &p_real_path,
        godot::Node *p_exclude
    ) const;
    void invalidate_synchronizers_cache();
    bool get_ready_fired() const {
        return ready_once_fired;
    }
    bool belongs_to(NetwMultiplayer *p_branch) const;
    void watch_scene_edges(const godot::RID &p_subject);
    void watch_despawning(bool p_watched);
    bool watches_despawning() const {
        return despawning_watched;
    }
    godot::Ref<NetwEntity> parent_entity() const;

    void hydrate_components();
    int64_t comp_structure_hash(const godot::PackedStringArray &p_paths) const;
    void _remote_hide();
    void _handle_tree_entered();
    void _handle_tree_exiting();
    void _on_owner_ready();
};

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwEntity::Hold);
VARIANT_ENUM_CAST(netw::NetwEntity::Transfer);
VARIANT_ENUM_CAST(netw::NetwEntity::Authority);
VARIANT_ENUM_CAST(netw::NetwEntity::DisconnectRule);
VARIANT_ENUM_CAST(netw::NetwEntity::ParentDespawnRule);
VARIANT_ENUM_CAST(netw::NetwEntity::Lifecycle);
VARIANT_ENUM_CAST(netw::NetwEntity::Stage);
