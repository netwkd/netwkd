#include "netw/api/netw_multiplayer.hpp"
#include "netw/link/shaped_peer.hpp"

#include "godot/net_peers.hpp"
#include "godot/transport_state.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/join_config.hpp"
#include "netw/api/lag_compensation_config.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/scene.hpp"
#include "netw/diag/hints.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/spawn/book.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"

#include "netw/api/participant.hpp"
#include "netw/api/server_info.hpp"

#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/multiplayer_spawner.hpp"
#include "godot/multiplayer_synchronizer.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/packed_scene.hpp"
#include "godot/physics_server.hpp"
#include "godot/project_settings.hpp"
#include "godot/rendering_server.hpp"
#include "godot/resource.hpp"
#include "godot/resource_loader.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/context.hpp"
#include "netw/api/database.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/refusal.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/auth_protocol.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/connect/browse_list.hpp"
#include "netw/connect/creation.hpp"
#include "netw/connect/directory_transport.hpp"
#include "netw/decl_sites.hpp"
#include "netw/game_call.hpp"
#include "netw/held_bytes.hpp"
#include "netw/log.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/repl/sync_compat.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/scene_loads.hpp"
#include "netw/script/model.hpp"
#include "netw/session/frames.hpp"
#include "netw/stage.hpp"
#include "netw/synchronizers.hpp"
#include "netw/wire/frame.hpp"

using namespace godot;
using namespace netw;

namespace netw {

namespace {

StringName &displaced_default_interface() {
    static StringName held;
    return held;
}

const StringName &installed_default_interface() {
    static StringName *installed
        = memnew(StringName(NetwMultiplayer::get_class_static()));
    return *installed;
}

godot::LocalVector<godot::ObjectID> live_registry;

struct TreePauseHold {
    int sessions = 0;
    bool paused_by_game = false;
};

TreePauseHold &tree_pause_hold() {
    static TreePauseHold hold;
    return hold;
}

constexpr double POLL_PUMP_RATE = 60.0;

const char *SIG_CONNECTED_TO_SERVER = "connected_to_server";
const char *SIG_CONNECTION_FAILED = "connection_failed";
const char *SIG_PEER_AUTHENTICATING = "peer_authenticating";
const char *SIG_PEER_AUTHENTICATION_FAILED = "peer_authentication_failed";
const char *SIG_PEER_PACKET = "peer_packet";
const char *SIG_SERVER_DISCONNECTED = "server_disconnected";
constexpr int POLLS_BEFORE_TURNED_AWAY_PEER_DROPS = 2;
constexpr int POLLS_BEFORE_SHUTDOWN_CLOSES = 30;
constexpr double LEAVE_GIVE_UP_SECONDS = 2.0;

bool names_the_root_window(const Variant &p_configuration) {
    Node *root = gd::scene_root();
    return root != nullptr && p_configuration.get_type() == Variant::NODE_PATH
        && NodePath(p_configuration) == root->get_path();
}

} // namespace

Object *script_of(Object *p_service) {
    if (p_service == nullptr) {
        return nullptr;
    }
    const Variant script = p_service->get_script();
    return script.get_type() == Variant::OBJECT ? static_cast<Object *>(script)
                                                : nullptr;
}

NetwMultiplayer::NetwMultiplayer() {
    Ref<SceneMultiplayer> transport;
    transport.instantiate();
    transport->set_root_path(NodePath("/root"));
    session_set_inner(transport);
    prediction_engine.bind_session(
        this,
        &world.clock,
        &store(),
        &liveness_core,
        &binding_book,
        &sim_rows
    );
    lagcomp_core.seat_world(&store());
    predict_runner.seat_core(this, &prediction_engine);
    sim_rows.seed_with(
        [](void *p_context, sim::Row &r_row) {
            static_cast<NetwMultiplayer *>(p_context)->sim_seed(r_row);
        },
        this
    );
    store().stamp_with(&NetwMultiplayer::store_stamp_thunk, this);
    store().session = this;
    store().arrays = &array_tables;
    store().pull = &store_pull;
    array_tables.seat(&store());
    store_pull.seat(&store());

    display_hooks.session = this;

    session_core.announce_to(this);

    interest_flush = callable_mp(this, &NetwMultiplayer::interest_flush_sink);

    connect_core.bind_session(this);

    ReplicationCore *plane = memnew(ReplicationCore);
    replication_owner = plane;
    plane->install(
        this,
        &world.clock,
        &store(),
        &liveness_core,
        &channel_book,
        &binding_book,
        &set_book,
        &prediction_engine
    );
#if defined(DEBUG_ENABLED)
    plane->seat_profiler(&network_profiler, &profiler_outgoing);
#endif
#if defined(NETW_TESTS)
    plane->seat_idle(&idle_work);
#endif
    plane->get_sync_model()->attach_book(&binding_book);

    scene_install();
    display_bind_session();

    const Signal joined(this, StringName("peer_connected"));
    connect_once(
        joined,
        callable_mp(this, &NetwMultiplayer::interest_peer_connected)
    );
    connect_once(
        joined,
        callable_mp(this, &NetwMultiplayer::replication_replay_tables)
    );
    adopt_schema_declarations();
    adopt_table_declarations();

    live_registry.push_back(gd::instance_id(this));
}

void NetwMultiplayer::embed_adopt_inner(const Ref<SceneMultiplayer> &p_inner) {
    if (p_inner.is_null() || p_inner == inner) {
        return;
    }
    const Callable adopted = p_inner->get_auth_callback();
    auth_timeout_seconds = p_inner->get_auth_timeout();
    session_set_inner(p_inner);
    if (adopted.is_valid()) {
        set_auth_callback(adopted);
    }
    session_apply_auth_config();
}

void NetwMultiplayer::embed_dispose() {
    if (!embed_claim_dispose()) {
        return;
    }
    session_flush_deferred();
    persist_dispose();
    set_auth_callback(Callable());
    scene_dispose();
    session_dispose();
    display_clear_runtimes();
    ReplicationCore *plane = (get_replication_plane());
    if (plane != nullptr) {
        plane->dispose();
    }
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->clear_bindings();
    }
    rpc_dispose();
    property_set_clear();
    binding_book.clear();
    clear_flat_family_state();
    service_clear();
    session_clear_roster();
    player_clear();
    announce_drop();
}

NetwMultiplayer::PlayerRow *NetwMultiplayer::player_row(int64_t p_peer) {
    const int64_t *player_id = player_id_of_peer.getptr(p_peer);
    return player_id != nullptr ? players.getptr(*player_id) : nullptr;
}

const NetwMultiplayer::PlayerRow *NetwMultiplayer::player_row(
    int64_t p_peer
) const {
    const int64_t *player_id = player_id_of_peer.getptr(p_peer);
    return player_id != nullptr ? players.getptr(*player_id) : nullptr;
}

int64_t NetwMultiplayer::player_mint_id() {
    const int64_t minted = player_incarnation_next;
    player_incarnation_next += 1;
    return minted;
}

void NetwMultiplayer::participant_index_peer(
    int64_t p_peer,
    int64_t p_player_id
) {
    player_id_of_peer[p_peer] = p_player_id;
}

void NetwMultiplayer::participant_unindex(int64_t p_peer, int64_t p_player_id) {
    const int64_t *current = player_id_of_peer.getptr(p_peer);
    if (current != nullptr && *current == p_player_id) {
        player_id_of_peer.erase(p_peer);
    }
}

bool NetwMultiplayer::player_ensure(const session::AcceptFrame &p_accepted) {
    if (p_accepted.player_id == 0) {
        return false;
    }
    const int64_t player_id = int64_t(p_accepted.player_id);
    player_incarnation_next = MAX(player_incarnation_next, player_id + 1);
    PlayerRow *found = players.getptr(player_id);
    if (found != nullptr) {
        if (found->peer != p_accepted.peer_id) {
            participant_unindex(found->peer, player_id);
            found->peer = p_accepted.peer_id;
        }
        found->departed = false;
        found->drop_owed = false;
        participant_index_peer(p_accepted.peer_id, player_id);
        return true;
    }
    if (const int64_t *stale = player_id_of_peer.getptr(p_accepted.peer_id)) {
        const int64_t stale_id = *stale;
        if (PlayerRow *departing = players.getptr(stale_id)) {
            departing->admitted = false;
            departing->departed = true;
        }
        player_drop_row(stale_id);
    }
    NETW_TRACE(
        sys::SESSION,
        "Gave player id %d to peer %d.",
        int(player_id),
        int(p_accepted.peer_id)
    );
    PlayerRow seating;
    seating.peer = p_accepted.peer_id;
    seating.incarnation = player_id;
    seating.username = p_accepted.username;
    seating.account = p_accepted.account;
    seating.account_name = p_accepted.account_name;
    players[player_id] = seating;
    participant_index_peer(p_accepted.peer_id, player_id);
    return true;
}

Ref<NetwPlayer> NetwMultiplayer::player_view(int64_t p_player) const {
    if (!players.has(p_player)) {
        return Ref<NetwPlayer>();
    }
    if (const Ref<NetwPlayer> *held = views.player_views.getptr(p_player)) {
        return *held;
    }
    Ref<NetwPlayer> minted;
    minted.instantiate();
    minted->session_id = gd::instance_id(this);
    minted->incarnation = p_player;
    views.player_views[p_player] = minted;
    return minted;
}

Ref<NetwPlayer> NetwMultiplayer::player_of(int64_t p_peer) const {
    const int64_t *player_id = player_id_of_peer.getptr(p_peer);
    return player_id != nullptr ? player_view(*player_id) : Ref<NetwPlayer>();
}

bool NetwMultiplayer::player_has(int64_t p_peer) const {
    return player_row(p_peer) != nullptr;
}

TypedArray<NetwPlayer> NetwMultiplayer::player_all() const {
    LocalVector<int64_t> peers;
    for (const KeyValue<int64_t, PlayerRow> &row : players) {
        if (!row.value.departed && !row.value.drop_owed) {
            peers.push_back(row.value.peer);
        }
    }
    peers.sort();
    TypedArray<NetwPlayer> out;
    for (uint32_t at = 0; at < peers.size(); at++) {
        out.push_back(player_of(peers[at]));
    }
    return out;
}

Ref<NetwPlayer> NetwMultiplayer::player_joined_of(int64_t p_peer) const {
    if (!session_has_accepted(p_peer)) {
        return Ref<NetwPlayer>();
    }
    return player_of(p_peer);
}

TypedArray<NetwPlayer> NetwMultiplayer::player_joined_all() {
    const LocalVector<session::AcceptFrame> joins = session_accepted_joins();
    TypedArray<NetwPlayer> out;
    for (uint32_t at = 0; at < joins.size(); ++at) {
        const Ref<NetwPlayer> seated = player_joined_of(joins[at].peer_id);
        if (seated.is_valid()) {
            out.push_back(seated);
        }
    }
    return out;
}

PackedInt32Array NetwMultiplayer::player_list() const {
    const LocalVector<session::AcceptFrame> joins = session_accepted_joins();
    PackedInt32Array out;
    for (uint32_t at = 0; at < joins.size(); ++at) {
        const int64_t player = peer_get_player(joins[at].peer_id);
        if (player != 0) {
            out.push_back(player);
        }
    }
    return out;
}

Ref<NetwPlayer> NetwMultiplayer::player_local() {
    return player_joined_of(get_unique_id());
}

int64_t NetwMultiplayer::player_get_local() {
    return peer_get_player(get_unique_id());
}

bool NetwMultiplayer::player_admit(int64_t p_peer) {
    PlayerRow *found = player_row(p_peer);
    if (found == nullptr || found->admitted) {
        return false;
    }
    found->admitted = true;
    return true;
}

Ref<NetwPlayer> NetwMultiplayer::player_admitted_of(int64_t p_peer) const {
    const PlayerRow *found = player_row(p_peer);
    if (found == nullptr || !found->admitted) {
        return Ref<NetwPlayer>();
    }
    return player_of(p_peer);
}

TypedArray<NetwPlayer> NetwMultiplayer::player_admitted_all() const {
    LocalVector<int64_t> peers;
    for (const KeyValue<int64_t, PlayerRow> &row : players) {
        if (row.value.admitted) {
            peers.push_back(row.value.peer);
        }
    }
    peers.sort();
    TypedArray<NetwPlayer> out;
    for (uint32_t at = 0; at < peers.size(); at++) {
        out.push_back(player_admitted_of(peers[at]));
    }
    return out;
}

Ref<NetwPlayer> NetwMultiplayer::player_admitted_local() {
    return player_admitted_of(get_unique_id());
}

int64_t NetwMultiplayer::player_find_account(const String &p_account) const {
    if (p_account.is_empty()) {
        return 0;
    }
    for (const KeyValue<int64_t, PlayerRow> &row : players) {
        if (row.value.admitted && !row.value.departed && !row.value.drop_owed
            && row.value.account == p_account) {
            return row.key;
        }
    }
    return 0;
}

TypedArray<NetwEntity> NetwMultiplayer::player_entities_held(
    int64_t p_peer,
    int64_t p_player_id
) {
    TypedArray<NetwEntity> held;
    if (!is_host() && !player_is_active(p_player_id)) {
        return held;
    }
    views.entities.each([&](uint64_t, const session::EntityView &p_view) {
        const Ref<NetwEntity> &entity = p_view.live;
        if (entity.is_valid() && entity->get_owner() != nullptr
            && entity->get_peer_id() == p_peer
            && entity->get_player_id() == p_player_id
            && !entity::stage_is_leaving(entity->get_stage())) {
            held.push_back(entity);
        }
    });
    if (p_player_id == 0) {
        return held;
    }
    for (const KeyValue<uint64_t, KeptEntity> &kept : kept_entities) {
        if (kept.value.player_id != p_player_id) {
            continue;
        }
        const Ref<NetwEntity> entity = wrapper_for_id(int64_t(kept.key));
        if (entity.is_valid() && !held.has(entity)) {
            held.push_back(entity);
        }
    }
    return held;
}

bool NetwMultiplayer::player_keep_entity(const RID &p_entity, int64_t p_peer) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (session_settings.return_window <= 0.0 || view.is_null() || !is_host()
        || p_peer == get_unique_id()) {
        return false;
    }
    const PlayerRow *leaving = player_row(p_peer);
    if (leaving == nullptr || !leaving->admitted
        || leaving->account.is_empty()) {
        return false;
    }
    if (leaving->leave_reason != NetwPlayer::LEAVE_DROPPED
        && leaving->leave_reason != NetwPlayer::LEAVE_STEPPED_AWAY) {
        return false;
    }
    const NetwEntityRecord *const record = view->get_record();
    const int64_t route = record->get_route();
    spawn::Pipeline *spawns = spawn_plane();
    if (spawns != nullptr && spawns->get_spawn_book()->has_armed(route)) {
        return false;
    }
    const bool owned = record->get_peer_id() == p_peer;
    KeptEntity kept;
    kept.player_id = leaving->incarnation;
    kept.account = leaving->account;
    kept.deadline_msec = int64_t(Time::get_singleton()->get_ticks_msec())
        + int64_t(session_settings.return_window * 1000.0);
    kept.owned = owned;
    kept.controlled
        = entity::Control::public_peer(record->get_control()->resolve())
        == p_peer;
    kept.despawns = owned
        || record->get_control()->get_on_disconnect()
            == NetwEntity::DISCONNECT_DESPAWN;
    kept_entities[p_entity.get_id()] = kept;
    if (!owned) {
        control_revert_to_server(p_entity, p_peer);
        return true;
    }
    const RID standing = scene_of(p_entity);
    spawn::Record *issued = spawns != nullptr
        ? spawns->get_spawn_book()->spawned_of(route)
        : nullptr;
    if (issued != nullptr) {
        issued->set_peer_id(0);
    }
    control_revert_to_server(p_entity, p_peer);
    membership_drop_entity_in(p_entity, standing, p_peer);
    return true;
}

void NetwMultiplayer::player_hand_back(int64_t p_peer) {
    if (!is_host() || kept_entities.is_empty()) {
        return;
    }
    const PlayerRow *returning = player_row(p_peer);
    if (returning == nullptr || returning->account.is_empty()) {
        return;
    }
    const String account = returning->account;
    LocalVector<uint64_t> held;
    for (const KeyValue<uint64_t, KeptEntity> &kept : kept_entities) {
        if (kept.value.account == account) {
            held.push_back(kept.key);
        }
    }
    spawn::Pipeline *spawns = spawn_plane();
    for (const uint64_t id : held) {
        const KeptEntity kept = kept_entities[id];
        kept_entities.erase(id);
        const Ref<NetwEntity> entity = wrapper_for_id(int64_t(id));
        if (entity.is_null()) {
            continue;
        }
        const int64_t controller
            = entity::Control::from_public(entity->get_controller());
        const int64_t driver
            = kept.controlled && controller == entity::Control::SERVER
            ? p_peer
            : controller;
        if (!kept.owned) {
            if (driver != controller || kept.despawns) {
                control_reassign(
                    entity->get_rid_handle(),
                    entity->get_peer_id(),
                    entity->get_player_id(),
                    driver
                );
            }
            continue;
        }
        spawn::Record *issued = spawns != nullptr
            ? spawns->get_spawn_book()->spawned_of(entity->get_route())
            : nullptr;
        if (issued != nullptr) {
            issued->set_peer_id(p_peer);
        }
        control_reassign(
            entity->get_rid_handle(),
            p_peer,
            returning->incarnation,
            driver
        );
        membership_place_entity(entity->get_rid_handle());
        scene_adopt_entity(entity->get_rid_handle());
    }
}

void NetwMultiplayer::player_expire_kept(int64_t p_now_msec) {
    LocalVector<uint64_t> due;
    for (const KeyValue<uint64_t, KeptEntity> &kept : kept_entities) {
        if (kept.value.deadline_msec <= p_now_msec) {
            due.push_back(kept.key);
        }
    }
    for (const uint64_t id : due) {
        const bool despawns = kept_entities[id].despawns;
        kept_entities.erase(id);
        const Ref<NetwEntity> entity = wrapper_for_id(int64_t(id));
        if (despawns && entity.is_valid()) {
            entity->despawn(
                NetwDespawnOpts::create(StringName("return_window"))
            );
        }
    }
}

void NetwMultiplayer::player_despawn_kept() {
    player_expire_kept(INT64_MAX);
}

bool NetwMultiplayer::player_kept_despawns(uint64_t p_id) const {
    const KeptEntity *kept = kept_entities.getptr(p_id);
    return kept != nullptr && kept->despawns;
}

bool NetwMultiplayer::player_kept_controlled(uint64_t p_id) const {
    const KeptEntity *kept = kept_entities.getptr(p_id);
    return kept != nullptr && kept->controlled;
}

void NetwMultiplayer::player_tell_kept(int64_t p_peer) {
    if (!is_host()) {
        return;
    }
    for (const KeyValue<uint64_t, KeptEntity> &kept : kept_entities) {
        const Ref<NetwEntity> entity = wrapper_for_id(int64_t(kept.key));
        if (kept.value.despawns && entity.is_valid()) {
            control_restate(entity->get_rid_handle(), p_peer);
        }
    }
}

void NetwMultiplayer::player_hear_kept(uint64_t p_id, bool p_kept) {
    if (is_host()) {
        return;
    }
    if (p_kept) {
        kept_by_server.insert(p_id);
    } else {
        kept_by_server.erase(p_id);
    }
}

void NetwMultiplayer::player_despawn_heard_kept() {
    LocalVector<uint64_t> heard;
    for (const uint64_t id : kept_by_server) {
        heard.push_back(id);
    }
    kept_by_server.clear();
    const bool admitted = halt_admits_despawn;
    halt_admits_despawn = true;
    for (const uint64_t id : heard) {
        const Ref<NetwEntity> entity = wrapper_for_id(int64_t(id));
        if (entity.is_valid()
            && entity->get_stage() < int64_t(entity::Stage::DESPAWNING)) {
            entity->despawn(
                NetwDespawnOpts::create(StringName("return_window"))
            );
        }
    }
    halt_admits_despawn = admitted;
}

SceneMembership &NetwMultiplayer::membership_book() {
    return scene_membership;
}

int64_t NetwMultiplayer::membership_entity_id(const RID &p_entity) {
    return int64_t(p_entity.get_id());
}

bool NetwMultiplayer::membership_member_active(int64_t p_peer) const {
    const PlayerRow *found = player_row(p_peer);
    return found != nullptr && !found->departed;
}

bool NetwMultiplayer::membership_destination_foreign(Node *p_owner) const {
    if (p_owner == nullptr) {
        return false;
    }
    for (Node *above = p_owner->get_parent(); above != nullptr;
         above = above->get_parent()) {
        const RID held = entity_at_or_above(above);
        if (!held.is_valid()) {
            continue;
        }
        return slot_record(held.get_id()) == nullptr;
    }
    return false;
}

void NetwMultiplayer::membership_place_entity(const RID &p_entity) {
    const NetwEntityRecord *record = entity_record(p_entity);
    if (!is_host() || record == nullptr) {
        return;
    }
    const int64_t peer = record->get_peer_id();
    const RID handle = p_entity;
    if (peer == 0 || !handle.is_valid()) {
        return;
    }
    Node *owner = wrapper_owner(handle);
    if (owner == nullptr) {
        return;
    }
    const int64_t entity = membership_entity_id(handle);
    RID scene = scene_of(handle);
    if (scene == handle || scene_despawned_roots.find(scene) >= 0) {
        scene = RID();
    }
    if (membership_destination_foreign(owner)) {
        fault::raise(
            this,
            NETW_FAULT_UNSUPPORTED,
            {vformat("Entity %d", int(record->get_route())),
             "a move under another {MultiplayerViewport}",
             "a despawn here and a spawn in the other viewport"}
        );
        scene = RID();
    }
    if (scene.is_valid() && !membership_member_active(peer)) {
        NETW_TRACE(
            sys::SCENE,
            "Route %d is in a scene that peer %d is not in, so nothing "
            "watches it.",
            int(record->get_route()),
            int(peer)
        );
        scene = RID();
    }
    LocalVector<RID> standing;
    scene_membership.scenes_of_entity(peer, entity, standing);
    for (uint32_t at = 0; at < standing.size(); ++at) {
        if (standing[at] != scene) {
            scene_membership.entity_exit(peer, standing[at], entity);
        }
    }
    if (scene.is_valid()) {
        scene_membership.entity_enter(peer, scene, entity);
    }
    membership_settle();
}

void NetwMultiplayer::membership_drop_entity(
    const RID &p_entity,
    int64_t p_peer
) {
    if (!p_entity.is_valid() || p_peer == 0) {
        return;
    }
    const int64_t entity = membership_entity_id(p_entity);
    LocalVector<RID> standing;
    scene_membership.scenes_of_entity(p_peer, entity, standing);
    for (uint32_t at = 0; at < standing.size(); ++at) {
        scene_membership.entity_exit(p_peer, standing[at], entity);
    }
    membership_settle();
}

void NetwMultiplayer::membership_drop_entity_in(
    const RID &p_entity,
    const RID &p_scene,
    int64_t p_peer
) {
    if (!p_entity.is_valid() || !p_scene.is_valid() || p_peer == 0) {
        return;
    }
    scene_membership
        .entity_exit(p_peer, p_scene, membership_entity_id(p_entity));
    membership_settle();
}

void NetwMultiplayer::membership_mirror_release(int64_t p_peer) {
    scene_membership.forget_member(p_peer);
    membership_settle();
}

void NetwMultiplayer::membership_settle() {
    const LocalVector<SceneMembership::Edge> &edges
        = scene_membership.pending_edges();
    LocalVector<SceneMembership::Edge> settling;
    settling.reserve(edges.size());
    for (uint32_t at = 0; at < edges.size(); ++at) {
        settling.push_back(edges[at]);
    }
#if defined(NETW_TESTS)
    residency_audit::Ledger &book = residency_audit::ledger();
    for (uint32_t at = 0; at < settling.size(); ++at) {
        if (settling[at].subscribed) {
            book.entered++;
        } else {
            book.left++;
        }
    }
#endif
    scene_membership.clear_edges();
    for (uint32_t at = 0; at < settling.size(); ++at) {
        scene_sync_viewer(settling[at].scene, settling[at].member);
    }
    if (!settling.is_empty()) {
        scene_player_display_invalidate();
    }
#if defined(NETW_TESTS)
    membership_audit();
#endif
}

#if defined(NETW_TESTS)

RID NetwMultiplayer::membership_residency_of(int64_t p_entity) const {
    NetwEntityRecord *const record = slot_record(p_entity);
    if (record == nullptr) {
        return RID();
    }
    const RID handle = record->get_handle();
    if (wrapper_owner(handle) == nullptr) {
        return RID();
    }
    const RID scene = scene_of(handle);
    if (scene == handle || scene_despawned_roots.find(scene) >= 0) {
        return RID();
    }
    return scene;
}

void NetwMultiplayer::membership_note_divergence(
    int64_t p_member,
    const RID &p_scene,
    int64_t p_entity
) {
    residency_audit::Ledger &book = residency_audit::ledger();
    book.divergences++;
    if (book.divergences > 1) {
        return;
    }
    book.member_of_first = p_member;
    book.scene_of_first = int64_t(p_scene.get_id());
    book.entity_of_first = p_entity;
}

void NetwMultiplayer::membership_audit() {
    if (!is_host() || membership_audit_held > 0
        || !entity_departures.is_empty()) {
        return;
    }
    residency_audit::Ledger &book = residency_audit::ledger();
    for (const KeyValue<int64_t, PlayerRow> &row : players) {
        LocalVector<RID> scenes;
        scene_membership.entity_scenes_of(row.value.peer, scenes);
        for (uint32_t at = 0; at < scenes.size(); ++at) {
            const PackedInt64Array entities
                = scene_membership.entities_of(row.value.peer, scenes[at]);
            for (int which = 0; which < entities.size(); ++which) {
                book.observations++;
                if (membership_residency_of(entities[which]) == scenes[at]) {
                    continue;
                }
                membership_note_divergence(
                    row.value.peer,
                    scenes[at],
                    entities[which]
                );
            }
        }
    }
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    LocalVector<NetwEntityRecord *> held;
    world.entities.each([&held](uint64_t, const EntityRow &p_slot) {
        if (NetwEntityRecord *record = p_slot.routed_record()) {
            held.push_back(record);
        }
    });
    for (NetwEntityRecord *record : held) {
        const int64_t peer = record->get_peer_id();
        if (peer == 0 || !membership_member_active(peer)
            || !pipeline->is_booked(record->get_route())) {
            continue;
        }
        const int64_t entity = membership_entity_id(record->get_handle());
        const RID resides = membership_residency_of(entity);
        if (!resides.is_valid()) {
            continue;
        }
        book.observations++;
        if (scene_membership.entities_of(peer, resides).has(entity)) {
            continue;
        }
        membership_note_divergence(peer, resides, entity);
    }
}

#endif

void NetwMultiplayer::player_forget(int64_t p_player) {
    const PlayerRow *found = players.getptr(p_player);
    if (found == nullptr) {
        return;
    }
    const int64_t peer = found->peer;
    if (p_player == local_player_id) {
        local_player_id = 0;
    }
    participant_unindex(peer, p_player);
    player_drop_row(p_player);
    membership_mirror_release(peer);
}

void NetwMultiplayer::player_drop_row(int64_t p_player) {
    PlayerRow *found = players.getptr(p_player);
    if (found != nullptr && found->pins > 0) {
        found->drop_owed = true;
        return;
    }
    if (const Ref<NetwPlayer> *held = views.player_views.getptr(p_player)) {
        (*held)->row_dropped = true;
    }
    players.erase(p_player);
    views.player_views.erase(p_player);
}

bool NetwMultiplayer::player_pin(int64_t p_player) {
    PlayerRow *found = players.getptr(p_player);
    NETW_BUG_COND_V(found == nullptr, false);
    found->pins += 1;
    return true;
}

void NetwMultiplayer::player_unpin(int64_t p_player) {
    PlayerRow *found = players.getptr(p_player);
    if (found == nullptr || found->pins == 0) {
        return;
    }
    found->pins -= 1;
    if (found->pins == 0 && found->drop_owed) {
        player_drop_row(p_player);
    }
}

void NetwMultiplayer::player_clear() {
    local_player_id = 0;
    for (const KeyValue<int64_t, Ref<NetwPlayer>> &held : views.player_views) {
        held.value->session_id = ObjectID();
    }
    players.clear();
    views.player_views.clear();
    player_id_of_peer.clear();
    kept_entities.clear();
    kept_by_server.clear();
    control_host_gifts.clear();
    scene_membership.clear();
}

int64_t NetwMultiplayer::player_incarnation(int64_t p_peer) const {
    const PlayerRow *found = player_row(p_peer);
    return found != nullptr && !found->departed ? found->incarnation : 0;
}

bool NetwMultiplayer::player_is_active(int64_t p_player) const {
    const PlayerRow *found = players.getptr(p_player);
    return found != nullptr && found->admitted && !found->departed;
}

int64_t NetwMultiplayer::player_get_peer(int64_t p_player) const {
    const PlayerRow *found = players.getptr(p_player);
    return found != nullptr ? found->peer : 0;
}

StringName NetwMultiplayer::player_get_username(int64_t p_player) const {
    const PlayerRow *found = players.getptr(p_player);
    return found != nullptr ? found->username : StringName();
}

String NetwMultiplayer::player_get_account(int64_t p_player) const {
    const PlayerRow *found = players.getptr(p_player);
    return found != nullptr ? found->account : String();
}

String NetwMultiplayer::player_get_account_name(int64_t p_player) const {
    const PlayerRow *found = players.getptr(p_player);
    return found != nullptr ? found->account_name : String();
}

TypedArray<RID> NetwMultiplayer::player_get_entities(int64_t p_player) {
    TypedArray<RID> out;
    const PlayerRow *found = players.getptr(p_player);
    if (found == nullptr) {
        return out;
    }
    const TypedArray<NetwEntity> held
        = player_entities_held(found->peer, p_player);
    for (int at = 0; at < held.size(); ++at) {
        const Ref<NetwEntity> entity = held[at];
        out.push_back(entity->get_rid_handle());
    }
    return out;
}

RID NetwMultiplayer::player_get_scene(int64_t p_player) {
    return player_is_active(p_player) ? scene_of_peer(player_get_peer(p_player))
                                      : RID();
}

bool NetwMultiplayer::player_holds(const Ref<NetwPlayer> &p_who) const {
    return p_who.is_valid() && p_who->core() == this
        && player_is_active(p_who->incarnation);
}

Error NetwMultiplayer::player_kick(int64_t p_player, const String &p_reason) {
    return player_kick_from(p_player, p_reason, false);
}

Error NetwMultiplayer::player_kick_from(
    int64_t p_player,
    const String &p_reason,
    bool p_door
) {
    const String verb = p_door ? String("{NetwSession.kick}")
                               : String("{NetwMultiplayer.player_kick}");
    if (!is_host()) {
        fault::raise(this, NETW_FAULT_SERVER_ONLY, {verb});
        return ERR_UNAUTHORIZED;
    }
    if (!player_is_active(p_player)) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {verb,
             p_door ? String("a player that is not in this session")
                    : vformat("player %d", p_player),
             p_door ? String("a player from {NetwSession.players}")
                    : String(
                          "the id of a client's player, such as one from "
                          "{NetwMultiplayer.player_list}"
                      )}
        );
        return ERR_INVALID_PARAMETER;
    }
    const int64_t peer = player_get_peer(p_player);
    if (peer == get_unique_id()) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {verb,
             "the server's own player",
             vformat(
                 "a client's player, or call %s to end the session",
                 p_door ? "{NetwSession.leave}"
                        : "{NetwMultiplayer.session_leave}"
             )}
        );
        return ERR_INVALID_PARAMETER;
    }
    player_note_leave(peer, NetwPlayer::LEAVE_KICKED);
    player_release_id(p_player);
    session_turn_peer_away(peer, p_reason);
    return OK;
}

void NetwMultiplayer::player_request_kick(
    int64_t p_player,
    const String &p_reason
) {
    const int64_t wire_player_limit = int64_t(1) << 35;
    const uint64_t player
        = p_player > 0 && p_player < wire_player_limit ? uint64_t(p_player) : 0;
    const PackedByteArray payload
        = session::frame_write(session::KickRequest{player, p_reason});
    if (is_server()) {
        session_publish_control(wire::CHANNEL_SESSION_KICK_REQUEST, 1, payload);
        return;
    }
    send_to(
        MultiplayerPeer::TARGET_PEER_SERVER,
        0,
        wire::CHANNEL_SESSION_KICK_REQUEST,
        payload,
        true,
        0,
        String()
    );
}

void NetwMultiplayer::player_note_leave(
    int64_t p_peer,
    enums::NetwPlayer::LeaveReason p_reason
) {
    PlayerRow *found = player_row(p_peer);
    if (found == nullptr) {
        return;
    }
    found->leave_reason = p_reason;
    if (!is_host() || inner.is_null()) {
        return;
    }
    session::PlayerLeft left;
    left.player_id = uint64_t(*player_id_of_peer.getptr(p_peer));
    left.reason = int64_t(p_reason);
    const PackedByteArray payload = session::frame_write(left);
    const PackedInt32Array peers = gd::api_peer_ids(inner);
    for (int at = 0; at < peers.size(); ++at) {
        if (peers[at] == p_peer || peers[at] == get_unique_id()) {
            continue;
        }
        send_to(
            peers[at],
            0,
            wire::CHANNEL_SESSION_PLAYER_LEFT,
            payload,
            true,
            0,
            String()
        );
    }
}

void NetwMultiplayer::player_release_id(int64_t p_player) {
    PlayerRow *found = players.getptr(p_player);
    const int64_t peer = found != nullptr ? found->peer : 0;
    NetwPlayer::LeaveReason reason = NetwPlayer::LEAVE_DROPPED;
    if (found != nullptr) {
        reason = found->leave_reason;
        found->admitted = false;
        found->departed = true;
        membership_mirror_release(peer);
    }
    join_roster.forget(peer);
    if (found != nullptr) {
        player_publish_left(p_player, reason);
    }
    player_forget(p_player);
}

void NetwMultiplayer::player_publish_left(
    int64_t p_player,
    enums::NetwPlayer::LeaveReason p_reason
) {
    if (views.session_handle.is_null()) {
        return;
    }
    Announcement row;
    row.kind = AnnouncementKind::PLAYER_LEFT;
    row.player = p_player;
    row.code = int64_t(p_reason);
    announce(row);
}

void NetwMultiplayer::player_publish_joined(int64_t p_peer) {
    const int64_t *seated = player_id_of_peer.getptr(p_peer);
    const Ref<NetwPlayer> player = player_of(p_peer);
    if (seated == nullptr || player.is_null()) {
        return;
    }
    Announcement row;
    row.player = *seated;
    if (p_peer == get_unique_id()) {
        local_player_id = row.player;
        if (views.session_handle.is_valid()) {
            row.kind = AnnouncementKind::LOCAL_JOINED;
            announce(row);
        }
    }
    if (scene_admits_on_join) {
        scene_on_player_joined(player);
    }
    if (views.session_handle.is_valid()) {
        row.kind = AnnouncementKind::PLAYER_JOINED;
        announce(row);
    }
}

bool NetwMultiplayer::session_is_active() const {
    SceneTree *loop = gd::scene_tree();
    if (loop == nullptr) {
        NETW_TRACE(sys::SESSION, "A live session has no SceneTree.");
        return false;
    }
    const Ref<MultiplayerAPI> live = loop->get_multiplayer(get_root_path());
    return live.ptr() == this;
}

NetwMultiplayer *NetwMultiplayer::session_holding(
    Node *p_node,
    TreeState p_state
) {
    if (p_node == nullptr || !p_node->is_inside_tree()) {
        NETW_TRACE(
            sys::SESSION,
            "A node outside the scene tree has no session."
        );
        return nullptr;
    }
    Ref<MultiplayerAPI> installed;
    if (p_state == TreeState::EXITING) {
        String path;
        for (Node *at = p_node; at != nullptr; at = at->get_parent()) {
            path = "/" + String(at->get_name()) + path;
        }
        installed = p_node->get_tree()->get_multiplayer(NodePath(path));
    } else {
        installed = p_node->get_multiplayer();
    }
    NetwMultiplayer *session
        = Object::cast_to<NetwMultiplayer>(installed.ptr());
    if (session == nullptr) {
        NETW_TRACE(sys::SESSION, "A node's branch has no NetwMultiplayer.");
    }
    return session;
}

Ref<NetwMultiplayer> NetwMultiplayer::session_of(Node *p_node) {
    if (p_node != nullptr) {
        return Ref<NetwMultiplayer>(session_holding(p_node));
    }
    NETW_ZONE_NC("netw::session_of", colors::SESSION);
    NetwMultiplayer *only = nullptr;
    int64_t running = 0;
    LocalVector<ObjectID> kept;
    for (const ObjectID &id : live_registry) {
        NetwMultiplayer *session
            = Object::cast_to<NetwMultiplayer>(gd::instance_from_id(id));
        if (session == nullptr) {
            continue;
        }
        kept.push_back(id);
        if (session->session_is_active()) {
            only = session;
            running += 1;
        }
    }
    live_registry = kept;
    if (running != 1) {
        fault::raise(
            nullptr,
            NETW_FAULT_SESSION_NOT_FOUND,
            {String("{NetwMultiplayer.session_of}"),
             String(),
             String(),
             0,
             running}
        );
        return Ref<NetwMultiplayer>();
    }
    return Ref<NetwMultiplayer>(only);
}

NodePath NetwMultiplayer::relative_path(Object *p_source, Object *p_target) {
    Node *source = Object::cast_to<Node>(p_source);
    Node *target = Object::cast_to<Node>(p_target);
    if (source == nullptr || target == nullptr) {
        return NodePath();
    }
    return source->get_path_to(target);
}

int NetwMultiplayer::service_row(Object *p_type) const {
    const ObjectID wanted = gd::instance_id(p_type);
    for (uint32_t at = 0; at < views.services.size(); at++) {
        if (views.services[at].type == wanted) {
            return int(at);
        }
    }
    return -1;
}

int NetwMultiplayer::service_row_named(const StringName &p_class) const {
    if (p_class == StringName()) {
        return -1;
    }
    for (uint32_t at = 0; at < views.services.size(); at++) {
        if (views.services[at].class_key == p_class) {
            return int(at);
        }
    }
    return -1;
}

Object *NetwMultiplayer::get_service_named(const StringName &p_class) const {
    const int at = service_row_named(p_class);
    return at < 0 ? nullptr : gd::instance_from_id(views.services[at].service);
}

void NetwMultiplayer::service_register(Node *p_service, Object *p_type) {
    if (p_service == nullptr) {
        return;
    }
    Object *type = p_type ? p_type : script_of(p_service);
    if (type == nullptr) {
        const StringName owned = p_service->get_class();
        const int held = service_row_named(owned);
        if (held >= 0) {
            views.services[held].service = gd::instance_id(p_service);
        } else {
            session::ServiceRow row;
            row.service = gd::instance_id(p_service);
            row.class_key = owned;
            views.services.push_back(row);
        }
        discovery_watch_directory(p_service);
        return;
    }
    const int at = service_row(type);
    if (at >= 0) {
        Object *held = gd::instance_from_id(views.services[at].service);
        if (held == p_service) {
            return;
        }
        if (held != nullptr) {
            fault::raise(
                this,
                NETW_FAULT_DECLARED_AGAIN,
                {String("The service"), type->to_string()}
            );
        }
        views.services[at].service = gd::instance_id(p_service);
        discovery_watch_directory(p_service);
        return;
    }
    session::ServiceRow row;
    row.type = gd::instance_id(type);
    row.service = gd::instance_id(p_service);
    views.services.push_back(row);
    discovery_watch_directory(p_service);
}

void NetwMultiplayer::service_unregister(Node *p_service, Object *p_type) {
    Object *type = p_type ? p_type : script_of(p_service);
    int at = type ? service_row(type) : -1;
    if (at < 0 && type == nullptr && p_service != nullptr) {
        at = service_row_named(p_service->get_class());
    }
    if (at < 0
        || gd::instance_from_id(views.services[at].service) != p_service) {
        return;
    }
    views.services.remove_at(at);
    discovery_unwatch_directory(p_service);
}

StringName NetwMultiplayer::class_named_by(Object *p_type) const {
    if (p_type == nullptr
        || p_type->get_class() != StringName("GDScriptNativeClass")) {
        return StringName();
    }
    const uint64_t asked = uint64_t(gd::instance_id(p_type));
    const StringName *cached = views.native_class_names.getptr(asked);
    if (cached != nullptr) {
        return *cached;
    }
    StringName named;
    const Variant made = p_type->call("new");
    Object *probe = gd::live_object(made);
    if (probe != nullptr) {
        named = probe->get_class();
        if (Object::cast_to<RefCounted>(probe) == nullptr) {
            memdelete(probe);
        }
    }
    views.native_class_names.insert(asked, named);
    return named;
}

Node *NetwMultiplayer::service_get(Object *p_type) const {
    const int at = service_row(p_type);
    if (at >= 0) {
        return Object::cast_to<Node>(
            gd::instance_from_id(views.services[at].service)
        );
    }
    return Object::cast_to<Node>(get_service_named(class_named_by(p_type)));
}

void NetwMultiplayer::service_clear() {
    views.services.clear();
    for (const KeyValue<uint64_t, RID> &held : directory_slots) {
        connect::free_transport_slot(held.value);
    }
    directory_slots.clear();
}

bool NetwMultiplayer::session_publish_control(
    int64_t p_channel,
    int64_t p_sender,
    const PackedByteArray &p_payload
) {
    const bool from_server = p_sender == SERVER_PEER;
    session::SessionReason stated;
    const bool states_reason = session::frame_read(p_payload, stated);
    if (p_channel == wire::CHANNEL_SESSION_PAUSE) {
        if (from_server && states_reason) {
            session_apply_pause(true, stated.reason);
        }
        return true;
    }
    if (p_channel == wire::CHANNEL_SESSION_UNPAUSE) {
        if (from_server && p_payload.is_empty()) {
            session_apply_pause(false, String());
        }
        return true;
    }
    if (p_channel == wire::CHANNEL_SESSION_KICKED) {
        if (from_server && states_reason) {
            session_note_exit(enums::NetwSession::KICKED, stated.reason);
        }
        return true;
    }
    if (p_channel == wire::CHANNEL_SESSION_SHUTDOWN) {
        if (from_server && states_reason) {
            session_note_exit(
                enums::NetwSession::SERVER_SHUTDOWN,
                stated.reason
            );
            session_server_shut_down = !is_host();
        }
        return true;
    }
    if (p_channel == wire::CHANNEL_SESSION_KICK_REQUEST) {
        session::KickRequest asked;
        if (is_host() && session::frame_read(p_payload, asked)) {
            Announcement row;
            row.kind = AnnouncementKind::KICK_REQUESTED;
            row.requester = player_incarnation(p_sender);
            row.target = int64_t(asked.player);
            row.reason = asked.reason;
            announce(row);
        }
        return true;
    }
    if (p_channel == wire::CHANNEL_SESSION_LEAVE) {
        session::SessionLeave leaving;
        if (is_host() && p_sender != get_unique_id()
            && session::frame_read(p_payload, leaving)) {
            session_receive_leave(p_sender, leaving.returning);
        }
        return true;
    }
    if (p_channel == wire::CHANNEL_SESSION_PLAYER_LEFT) {
        session::PlayerLeft left;
        if (from_server && !is_host() && session::frame_read(p_payload, left)) {
            session_receive_player_left(left);
        }
        return true;
    }
    return false;
}

void NetwMultiplayer::announce(const Announcement &p_row) {
    announce_push(p_row);
    announce_flush();
}

bool NetwMultiplayer::announce_pin(const Announcement &p_row) {
    const bool entity = !p_row.entity.is_valid() || slot_pin(p_row.entity);
    const bool scene = !p_row.scene.is_valid() || slot_pin(p_row.scene);
    const bool scene_to
        = !p_row.scene_to.is_valid() || slot_pin(p_row.scene_to);
    const bool player = p_row.player == 0 || player_pin(p_row.player);
    const bool endpoint
        = !p_row.endpoint.is_valid() || endpoint_pin(p_row.endpoint);
    if (entity && scene && scene_to && player && endpoint) {
        return true;
    }
    if (p_row.endpoint.is_valid() && endpoint) {
        endpoint_unpin(p_row.endpoint);
    }
    if (p_row.entity.is_valid() && entity) {
        slot_unpin(p_row.entity);
    }
    if (p_row.scene.is_valid() && scene) {
        slot_unpin(p_row.scene);
    }
    if (p_row.scene_to.is_valid() && scene_to) {
        slot_unpin(p_row.scene_to);
    }
    if (p_row.player != 0 && player) {
        player_unpin(p_row.player);
    }
    return false;
}

void NetwMultiplayer::announce_push(const Announcement &p_row) {
    if (announce_pin(p_row)) {
        announcements.push_back(p_row);
    }
}

void NetwMultiplayer::announce_unpin(const Announcement &p_row) {
    if (p_row.entity.is_valid()) {
        slot_unpin(p_row.entity);
    }
    if (p_row.scene.is_valid()) {
        slot_unpin(p_row.scene);
    }
    if (p_row.scene_to.is_valid()) {
        slot_unpin(p_row.scene_to);
    }
    if (p_row.player != 0) {
        player_unpin(p_row.player);
    }
    if (p_row.endpoint.is_valid()) {
        endpoint_unpin(p_row.endpoint);
    }
}

void NetwMultiplayer::announce_drop() {
    for (uint32_t at = announce_next; at < announcements.size(); ++at) {
        announce_unpin(announcements[at]);
    }
    announcements.clear();
    announce_next = 0;
}

void NetwMultiplayer::announce_dispatch(const Announcement &p_row) {
    const Ref<NetwSession> session = views.session_handle;
    const Ref<NetwConnection> connection = views.connection_handle;
    switch (p_row.kind) {
        case AnnouncementKind::KICK_REQUESTED: {
            if (session.is_valid() && player_is_active(p_row.requester)
                && player_is_active(p_row.target)) {
                session->announce_kick_requested(
                    player_view(p_row.requester),
                    player_view(p_row.target),
                    p_row.reason
                );
            }
        } break;
        case AnnouncementKind::TICKRATE_MISMATCH: {
            const Ref<NetwClock> clock_view = views.clock_handle;
            if (clock_view.is_valid()) {
                clock_view->announce_tickrate_mismatch(p_row.server_tickrate);
            }
        } break;
        case AnnouncementKind::ENTITY_ENTERED:
        case AnnouncementKind::ENTITY_EXITED: {
            const Ref<NetwEntity> entity = slot_pinned_entity(p_row.entity);
            const Ref<NetwScene> scene
                = views.pinned_scene_of(p_row.scene.get_id());
            const bool present = p_row.kind == AnnouncementKind::ENTITY_ENTERED;
            if (entity.is_valid() && scene.is_valid()) {
                scene->announce_entity(entity, present);
                if (p_row.entity_is_player) {
                    scene->announce_player_entity(entity, present);
                }
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::SCENE_REMOVED: {
            const Ref<NetwScene> scene
                = views.pinned_scene_of(p_row.scene.get_id());
            if (session.is_valid() && scene.is_valid()) {
                session->announce_scene_removed(scene);
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::SCENE_READY: {
            const Ref<NetwScene> scene
                = views.pinned_scene_of(p_row.scene.get_id());
            if (session.is_valid()) {
                session->announce_scene_ready(scene);
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::SCENE_CHANGED: {
            const Ref<NetwPlayer> player = player_view(p_row.player);
            if (player.is_valid()) {
                player->announce_scene_changed(
                    views.pinned_scene_of(p_row.scene.get_id()),
                    views.pinned_scene_of(p_row.scene_to.get_id())
                );
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::STATE_CHANGED: {
            if (session.is_valid()) {
                const NetwSession::State state = NetwSession::State(p_row.code);
                session->announce_state_changed(state);
            }
        } break;
        case AnnouncementKind::ENTERED: {
            if (session.is_valid()) {
                session->announce_entered();
            }
        } break;
        case AnnouncementKind::LOCAL_JOINED:
        case AnnouncementKind::PLAYER_JOINED:
        case AnnouncementKind::PLAYER_LEFT: {
            const Ref<NetwPlayer> player = player_view(p_row.player);
            if (session.is_valid() && player.is_valid()) {
                if (p_row.kind == AnnouncementKind::LOCAL_JOINED) {
                    session->announce_local_joined(player);
                } else if (p_row.kind == AnnouncementKind::PLAYER_JOINED) {
                    session->announce_player_joined(player);
                } else {
                    session->announce_player_left(
                        player,
                        NetwPlayer::LeaveReason(p_row.code)
                    );
                }
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::JOIN_FAILED: {
            if (session.is_valid()) {
                session->announce_join_failed(p_row.code, p_row.reason);
            }
        } break;
        case AnnouncementKind::PAUSED_CHANGED: {
            if (session.is_valid()) {
                session->announce_paused_changed(p_row.paused, p_row.reason);
            }
        } break;
        case AnnouncementKind::SERVER_TRANSFERRED: {
            if (session.is_valid()) {
                session->announce_server_transferred(
                    p_row.successor_was,
                    p_row.former_now
                );
            }
        } break;
        case AnnouncementKind::VIEWER_ENTERED:
        case AnnouncementKind::VIEWER_EXITED: {
            const Ref<NetwScene> scene
                = views.pinned_scene_of(p_row.scene.get_id());
            const Ref<NetwPlayer> player = player_view(p_row.player);
            if (scene.is_valid() && player.is_valid()) {
                scene->announce_viewer(
                    player,
                    p_row.kind == AnnouncementKind::VIEWER_ENTERED
                );
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::CONTROL_CHANGED:
        case AnnouncementKind::DESPAWNING:
        case AnnouncementKind::DESPAWNED:
        case AnnouncementKind::LIFECYCLE_REFUSED:
        case AnnouncementKind::REPARENTED:
        case AnnouncementKind::HIDDEN:
        case AnnouncementKind::MODE_CHANGED:
        case AnnouncementKind::STATE_EVALUATED:
        case AnnouncementKind::EPISODE_CHANGED:
        case AnnouncementKind::RECOVERED:
        case AnnouncementKind::DIVERGENCE_DETECTED:
        case AnnouncementKind::PERSIST_LOADED:
        case AnnouncementKind::PERSIST_SAVED: {
            const Ref<NetwEntity> entity = slot_pinned_entity(p_row.entity);
            if (entity.is_valid()) {
                entity->emit_announcement(p_row);
            }
            announce_unpin(p_row);
        } break;
        case AnnouncementKind::DATABASE_FAILED: {
            const Ref<NetwDatabase> *held
                = views.database_handles.getptr(p_row.database);
            const Ref<NetwDatabase> database
                = held != nullptr ? *held : Ref<NetwDatabase>();
            if (database.is_valid()) {
                database
                    ->announce_failed(p_row.database, p_row.code, p_row.reason);
            }
        } break;
        case AnnouncementKind::SYNCHRONIZED: {
            const Ref<NetwClock> clock_view = views.clock_handle;
            if (clock_view.is_valid()) {
                clock_view->announce_synchronized();
            }
        } break;
        case AnnouncementKind::TABLE_RECEIVED: {
            const Ref<NetwTable> *handle
                = views.table_handles.getptr(p_row.table);
            if (handle != nullptr) {
                const Ref<NetwTable> table = *handle;
                table->announce(p_row.to);
            }
        } break;
        case AnnouncementKind::CONNECTION_PROGRESSED: {
            if (connection.is_valid()) {
                connection->announce_progressed(p_row.reason, p_row.amount);
            }
        } break;
        case AnnouncementKind::CONNECTION_FAILED: {
            if (connection.is_valid()) {
                connection->announce_failed(p_row.code, p_row.reason);
            }
        } break;
        case AnnouncementKind::ATTEMPT_SETTLED: {
            if (p_row.code == OK) {
                p_row.promise->resolve(int64_t(OK));
            } else {
                p_row.promise->reject(Error(p_row.code), p_row.reason);
            }
        } break;
        case AnnouncementKind::JOIN_ADDRESS_CHANGED: {
            if (connection.is_valid()) {
                connection->announce_join_address_changed(p_row.reason);
            }
        } break;
        case AnnouncementKind::ENDPOINT_ADDED:
        case AnnouncementKind::ENDPOINT_REMOVED:
        case AnnouncementKind::INVITED:
        case AnnouncementKind::ENDPOINT_CHANGED: {
            const session::EndpointView *held
                = views.endpoint_views.getptr(p_row.endpoint);
            const Ref<NetwEndpoint> view
                = held != nullptr ? held->view : Ref<NetwEndpoint>();
            const bool heard = connection.is_valid() && view.is_valid();
            if (heard && p_row.kind == AnnouncementKind::ENDPOINT_ADDED) {
                connection->announce_endpoint_added(view);
            } else if (
                heard && p_row.kind == AnnouncementKind::ENDPOINT_REMOVED
            ) {
                connection->announce_endpoint_removed(view);
            } else if (heard && p_row.kind == AnnouncementKind::INVITED) {
                connection->announce_invited(view);
            } else if (heard) {
                connection->announce_endpoint_updated(view);
            }
            if (p_row.kind == AnnouncementKind::ENDPOINT_REMOVED) {
                endpoint_view_drop(p_row.endpoint);
            }
            announce_unpin(p_row);
        } break;
    }
}

void NetwMultiplayer::announce_flush() {
    if (announce_draining || is_inside_step()) {
        return;
    }
    announce_drain();
}

void NetwMultiplayer::announce_drain() {
    if (announce_next >= announcements.size()) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    const bool outermost = !announce_draining;
    announce_draining = true;
    while (announce_next < announcements.size()) {
        const Announcement row = announcements[announce_next];
        announce_next += 1;
        announce_dispatch(row);
    }
    if (outermost) {
        announcements.clear();
        announce_next = 0;
        announce_draining = false;
    }
}

void NetwMultiplayer::session_admit(const session::AcceptFrame &p_accepted) {
    if (p_accepted.peer_id == 0 || p_accepted.player_id == 0) {
        return;
    }
    if (!join_roster.remember(p_accepted)) {
        return;
    }
    const int64_t joined = p_accepted.peer_id;
    if (joined == get_unique_id()) {
        join_awaiting_admission = false;
        submitted_join.reset();
    }
    if (!player_ensure(p_accepted)) {
        return;
    }
    player_admit(joined);
    player_hand_back(joined);
    if (joined == SERVER_PEER) {
        control_seat_host();
    }
    player_tell_kept(joined);
    player_publish_joined(joined);
    if (!has_server_role()) {
        auth_advance(joined);
    }
}

bool NetwMultiplayer::session_preflight_join(
    const session::AcceptFrame &p_accepted,
    const JoinRequest &p_request
) {
    const JoinPlan plan = session_resolve_join();
    if (!plan.available) {
        session_fail_join(
            p_accepted.peer_id,
            ERR_UNCONFIGURED,
            "This server declares a join handler whose object is unavailable"
        );
        return false;
    }
    if (!plan.handler.is_valid()) {
        return true;
    }
    if (plan.declared && p_request.arg_values.is_empty()
        && !plan.arg_types.is_empty()) {
        fault::raise(
            this,
            NETW_FAULT_JOIN_ARGS_MISMATCH,
            {String(), String(), String(), p_accepted.peer_id}
        );
        session_fail_join(
            p_accepted.peer_id,
            ERR_UNCONFIGURED,
            session_join_args_refusal(p_request)
        );
        return false;
    }
    return true;
}

void NetwMultiplayer::session_fail_join(
    int64_t p_peer,
    Error p_error,
    const String &p_reason
) {
    const int64_t peer = p_peer;
    join_roster.forget(peer);
    session_refuse(peer, p_reason);
    if (peer == get_unique_id()) {
        session_report_join_failure(p_error, p_reason);
        return;
    }
    if (auth_refuse(peer, p_error, p_reason) || !is_host()) {
        return;
    }
    send_to(
        peer,
        0,
        wire::CHANNEL_SESSION_JOIN,
        auth::encode_refused(int64_t(p_error), p_reason),
        true,
        0,
        String()
    );
    session_turn_peer_away(peer, String());
}

void NetwMultiplayer::session_turn_peer_away(
    int64_t p_peer,
    const String &p_reason
) {
    if (!p_reason.is_empty()) {
        send_to(
            p_peer,
            0,
            wire::CHANNEL_SESSION_KICKED,
            session::frame_write(session::SessionReason{p_reason}),
            true,
            0,
            String()
        );
    }
    for (const TurnedAwayPeer &row : turned_away_peers) {
        if (row.peer == p_peer) {
            return;
        }
    }
    turned_away_peers.push_back(TurnedAwayPeer{p_peer, 0});
}

void NetwMultiplayer::session_drop_turned_away_peer(int64_t p_peer) {
    if (effective_peer.is_valid()) {
        effective_peer->disconnect_peer(int32_t(p_peer));
    }
    session_settle_peer_loss(p_peer);
}

void NetwMultiplayer::session_poll_turned_away() {
    LocalVector<int64_t> due;
    uint32_t kept = 0;
    for (uint32_t at = 0; at < turned_away_peers.size(); ++at) {
        TurnedAwayPeer row = turned_away_peers[at];
        row.polls += 1;
        if (row.polls < POLLS_BEFORE_TURNED_AWAY_PEER_DROPS) {
            turned_away_peers[kept++] = row;
            continue;
        }
        due.push_back(row.peer);
    }
    turned_away_peers.resize(kept);
    for (const int64_t peer : due) {
        session_drop_turned_away_peer(peer);
    }
}

void NetwMultiplayer::session_report_join_failure(
    Error p_error,
    const String &p_reason
) {
    join_awaiting_admission = false;
    prepared_join.reset();
    submitted_join.reset();
    Announcement row;
    row.kind = AnnouncementKind::JOIN_FAILED;
    row.code = int64_t(p_error);
    row.reason = p_reason;
    announce(row);
}

void NetwMultiplayer::session_run_join_handler(
    const session::AcceptFrame &p_accepted,
    const Array &p_args
) {
    if (!is_host() || p_accepted.peer_id == 0) {
        return;
    }
    const int64_t peer = p_accepted.peer_id;
    PlayerRow *seated = player_row(peer);
    if (seated == nullptr || !seated->admitted || seated->handled) {
        return;
    }
    const JoinPlan plan = session_resolve_join();
    if (!plan.available || !plan.handler.is_valid()) {
        seated->handled = true;
        return;
    }
    const Ref<NetwPlayer> joining = player_of(peer);
    if (joining.is_null()) {
        return;
    }
    seated->handled = true;
    Array args;
    args.push_back(joining);
    args.append_array(p_args);
    bool called = false;
    {
        NETW_ZONE_NC("session run join handler", colors::SESSION);
        gd::call_checked(plan.handler, args, called);
    }
    if (!called) {
        fault::raise(
            this,
            NETW_FAULT_JOIN_ARGS_MISMATCH,
            {String(), String(), String(), peer}
        );
    }
}

void NetwMultiplayer::session_after_entered() {
    if (auth_admitted_roster.has_value()) {
        session_apply_admitted_roster();
        return;
    }
    const int64_t role = session_get_role();
    if (!prepared_join.has_value() && role != ROLE_DEDICATED_SERVER) {
        prepared_join = JoinRequest();
    }
    if (role == ROLE_CLIENT) {
        session_submit_prepared_join();
        return;
    }
    session_submit_host_join();
}

void NetwMultiplayer::session_apply_admitted_roster() {
    const PackedByteArray roster = *auth_admitted_roster;
    auth_admitted_roster.reset();
    prepared_join.reset();
    LocalVector<session::AcceptFrame> rows;
    if (!session::roster_read(roster, rows)) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), SERVER_PEER}
        );
        return;
    }
    for (const session::AcceptFrame &row : rows) {
        if (row.peer_id == get_unique_id()) {
            connect_core.advertise(row.join_address, int64_t(row.max_players));
        }
        session_admit(row);
    }
}

void NetwMultiplayer::session_submit_host_join() {
    if (!prepared_join.has_value()) {
        return;
    }
    if (scene_list().size() > 0) {
        session_submit_prepared_join();
        return;
    }
    host_join_waits_startup = true;
}

void NetwMultiplayer::session_submit_host_join_on_startup() {
    session_submit_prepared_join();
}

void NetwMultiplayer::session_after_edge(int64_t, int64_t p_new) {
    session_watch_window_close(p_new != SESSION_STATE_OFFLINE);
    if (p_new == SESSION_STATE_OFFLINE && session_exited_awaits_offline >= 0) {
        callable_mp(this, &NetwMultiplayer::session_announce_exited)
            .call_deferred(
                session_exited_awaits_offline,
                session_exited_detail
            );
        session_exited_awaits_offline = -1;
        session_exited_detail = String();
    }
    if (p_new == SESSION_STATE_OFFLINE && session_departing.is_valid()) {
        const Ref<NetwPromise> left = session_departing;
        session_departing.unref();
        left->resolve(OK);
    }
    if (p_new == SESSION_STATE_OFFLINE) {
        unreachable_peers.clear();
        settled_peer_departures.clear();
        session_clear_prepared_join();
        auth_admitted_roster.reset();
        serverless_mesh_polls = 0;
        transfer_awaits_roster = false;
        transfer_authored = PackedInt64Array();
        transfer_called = false;
        transfer_gone = PackedInt64Array();
        transfer_former = 0;
        clock_engine().set_held(false);
        const PackedStringArray repeated = faults.end_session();
        for (int at = 0; at < repeated.size(); ++at) {
            gd::print(repeated[at]);
        }
    }
}

void NetwMultiplayer::session_relay_peer_connected(int64_t p_peer) {
    if (installing_transfer) {
        return;
    }
    if (!link_handshakes.has(p_peer)) {
        connect_core.peer_connected(p_peer);
    }
    peer_mark_reachable(p_peer);
    settled_peer_departures.erase(p_peer);
    event_record(EventPlane::PEER_JOINED, RID(), 0, p_peer);
    emit_signal(StringName("peer_connected"), p_peer);
    LinkHandshake *link = link_handshakes.getptr(p_peer);
    if (link == nullptr || !link->admitted.has_value()) {
        return;
    }
    const session::AcceptFrame admitted = *link->admitted;
    const Array args = link->admitted_args;
    link->admitted.reset();
    link->admitted_args = Array();
    session_publish_admitted(admitted, args);
}

void NetwMultiplayer::session_relay_peer_disconnected(int64_t p_peer) {
    if (installing_transfer) {
        return;
    }
    session_settle_peer_loss(p_peer);
}

void NetwMultiplayer::session_clear_disconnected_peer(int64_t p_peer) {
    auth_forget_link(p_peer);
    session_clear_link_delivery(p_peer);
}

void NetwMultiplayer::session_clear_link_delivery(int64_t p_peer) {
    world.wire.seq_book.forget_peer(p_peer);
    world.clock.exchange.forget_peer(p_peer);
    liveness_core.abandon_lease(int(p_peer));
    ReplicationCore *plane = (get_replication_plane());
    if (plane != nullptr) {
        plane->clear_peer(p_peer);
    }
    rpc_handle_disconnect(p_peer);
}

void NetwMultiplayer::session_teardown_online() {
    hosted_role.reset();
    session_transition(SESSION_STATE_DISCONNECTING);
    session_transition(SESSION_STATE_OFFLINE);
}

void NetwMultiplayer::session_settle_peer_loss(int64_t p_peer) {
    if (p_peer == 0 || settled_peer_departures.has(p_peer)) {
        return;
    }
    settled_peer_departures.insert(p_peer);

    const bool terminates_session = p_peer == SERVER_PEER
        && !embed_is_disposing()
        && session_get_stored_state() == SESSION_STATE_ONLINE;

    unreachable_peers.insert(p_peer);
    session_clear_disconnected_peer(p_peer);

    interest_peer_disconnected(p_peer);
    entity_settle_peer_departure(p_peer);
    if (spawn::Pipeline *spawns = spawn_plane()) {
        spawns->get_spawn_book()->forget_peer(p_peer);
    }
    halt_forget_peer(p_peer);

    event_record(EventPlane::PEER_LEFT, RID(), 0, p_peer);
    emit_signal(StringName("peer_disconnected"), p_peer);
    peer_forget(p_peer);

    if (terminates_session) {
        session_teardown_online();
    }
}

void NetwMultiplayer::session_transport_connected() {
    if (installing_transfer) {
        return;
    }
    if (session_get_stored_state() != SESSION_STATE_CONNECTING) {
        NETW_TRACE(
            sys::SESSION,
            "The transport connected while the session is in state %d.",
            session_get_stored_state()
        );
        return;
    }
    session_push_desired_role();
    session_resolve_online(get_unique_id());
}

void NetwMultiplayer::session_transport_failed() {
    if (session_get_stored_state() != SESSION_STATE_CONNECTING) {
        NETW_TRACE(
            sys::SESSION,
            "The transport failed while the session is in state %d.",
            session_get_stored_state()
        );
        return;
    }
    connect_core.on_transport_failed();
    session_transition(SESSION_STATE_OFFLINE);
}

void NetwMultiplayer::session_transport_dropped() {
    if (embed_is_disposing()) {
        NETW_TRACE(
            sys::SESSION,
            "The transport dropped while the session is already closing."
        );
        return;
    }
    if (session_get_stored_state() == SESSION_STATE_CONNECTING) {
        session_transport_failed();
        return;
    }
    if (session_get_stored_state() != SESSION_STATE_ONLINE) {
        NETW_TRACE(
            sys::SESSION,
            "The transport dropped while the session is in state %d.",
            session_get_stored_state()
        );
        return;
    }
    session_teardown_online();
}

void NetwMultiplayer::session_settle_clear_state() {
    callable_mp(this, &NetwMultiplayer::clear_session_state).call_deferred();
}

void NetwMultiplayer::session_set_tree_paused(bool p_paused) {
    TreePauseHold &hold = tree_pause_hold();
    SceneTree *loop = gd::scene_tree();
    if (p_paused) {
        if (hold.sessions++ == 0) {
            hold.paused_by_game = loop != nullptr && loop->is_paused();
        }
        if (loop != nullptr) {
            loop->set_pause(true);
        }
        return;
    }
    if (hold.sessions == 0) {
        return;
    }
    hold.sessions -= 1;
    if (hold.sessions == 0 && !hold.paused_by_game && loop != nullptr) {
        loop->set_pause(false);
    }
}

void NetwMultiplayer::session_apply_pause(
    bool p_paused,
    const String &p_reason
) {
    if (session_paused == p_paused) {
        return;
    }
    session_paused = p_paused;
    session_pause_reason = p_paused ? p_reason : String();
    session_set_tree_paused(p_paused);
    Announcement row;
    row.kind = AnnouncementKind::PAUSED_CHANGED;
    row.paused = p_paused;
    row.reason = session_pause_reason;
    announce(row);
}

void NetwMultiplayer::session_dispose() {
    session_clear_prepared_join();
    auth_clear();
    probe_clear();
}

const char *NetwMultiplayer::session_identity_differs(
    const JoinRequest &p_request
) const {
    if (p_request.app_tag != auth_app_tag) {
        return "app";
    }
    if (p_request.wire_identity != SessionCore::compute_wire_identity()) {
        return "wire";
    }
    return nullptr;
}

void NetwMultiplayer::session_receive_join(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (!is_host()) {
        session_receive_join_answer(p_payload, p_sender);
        return;
    }
    const Time *reading = Time::get_singleton();
    if (session_core.join_flooded(
            int(p_sender),
            reading ? int64_t(reading->get_ticks_msec()) : 0
        )) {
        if (session_core.claim_flood_report(int(p_sender))) {
            fault::raise(
                this,
                NETW_FAULT_JOIN_FLOOD,
                {String(), String(), String(), p_sender}
            );
        }
        return;
    }
    session_take_join(p_payload, p_sender);
}

void NetwMultiplayer::session_receive_join_answer(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (p_sender != SERVER_PEER) {
        return;
    }
    const auth::Answer answer = auth::decode_answer(p_payload);
    if (!answer.ok || answer.kind != auth::AnswerKind::REFUSED) {
        return;
    }
    session_report_join_failure(Error(answer.code), answer.reason);
}

void NetwMultiplayer::session_take_join(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (structure_halted) {
        halt_park_join(p_payload, p_sender);
        return;
    }
    session_accept_join(p_payload, p_sender);
}

void NetwMultiplayer::session_accept_join(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    JoinRequest requested;
    if (!requested.deserialize(p_payload)) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_sender}
        );
        auth_refuse(
            p_sender,
            ERR_UNCONFIGURED,
            "The join could not be read. Run the same build of the game on "
            "both computers."
        );
        return;
    }
    requested.peer_id = p_sender;
    const char *differs = session_identity_differs(requested);
    if (differs != nullptr) {
        fault::raise(
            this,
            NETW_FAULT_PEER_MISMATCH,
            {String("its join"),
             String(),
             String(differs) + String(" identity"),
             p_sender}
        );
        session_fail_join(
            p_sender,
            ERR_UNCONFIGURED,
            vformat("Incompatible build: %s", String(differs))
        );
        return;
    }
    if (p_sender != get_unique_id() && join_roster.has_accepted(p_sender)) {
        NETW_TRACE(
            sys::SESSION,
            "Peer %d already holds a player, so its join was dropped.",
            int(p_sender)
        );
        return;
    }
    if (!session_decode_join_args(requested)) {
        session_fail_join(
            p_sender,
            ERR_UNCONFIGURED,
            session_join_args_refusal(requested)
        );
        return;
    }
    session_start_admission(requested);
}

void NetwMultiplayer::session_finish_join(
    const JoinRequest &p_request,
    const Ref<NetwJoinRequest> &p_admission
) {
    JoinRequest requested = p_request;
    const int64_t p_sender = requested.peer_id;
    const String account = p_admission->get_account();
    const int64_t playing = player_find_account(account);
    const int64_t playing_peer = player_get_peer(playing);
    if (playing != 0 && playing_peer == get_unique_id()
        && p_sender != get_unique_id()) {
        session_fail_join(
            p_sender,
            ERR_ALREADY_IN_USE,
            "This account is already playing as the server's player."
        );
        return;
    }
    if (playing != 0 && playing_peer != p_sender) {
        const int64_t replaced = playing_peer;
        player_note_leave(replaced, NetwPlayer::LEAVE_DROPPED);
        session_turn_peer_away(replaced, String("Signed in again."));
        session_settle_peer_loss(replaced);
    }
    session::AcceptFrame accepting;
    if (!session_resolve_inbound_join(requested, p_sender, accepting)) {
        auth_refuse(
            p_sender,
            ERR_UNCONFIGURED,
            session_join_args_refusal(requested)
        );
        return;
    }
    accepting.account = account;
    accepting.account_name
        = account.is_empty() ? String() : p_admission->get_account_name();
    const Array decoded = requested.arg_values;
    if (!session_preflight_join(accepting, requested)) {
        return;
    }
    LinkHandshake *link = auth_joining_link(p_sender);
    if (link != nullptr) {
        link->admitted = accepting;
        link->admitted_args = decoded;
        link->join = PackedByteArray();
        if (auth_send_raw(
                p_sender,
                auth::encode_admitted(auth_admitted_bytes(accepting))
            )
            != OK) {
            disconnect_peer(p_sender);
        }
        return;
    }
    session_publish_admitted(accepting, decoded);
}

PackedByteArray NetwMultiplayer::auth_admitted_bytes(
    const session::AcceptFrame &p_joiner
) const {
    const int64_t joiner = p_joiner.peer_id;
    LocalVector<session::AcceptFrame> rows;
    for (const session::AcceptFrame &row : join_roster.accepted_joins()) {
        if (row.peer_id != joiner) {
            rows.push_back(session::accept_seen_by(row, joiner));
        }
    }
    session::AcceptFrame own = p_joiner;
    const String address = connect_core.join_address();
    if (wire::string_bytes(address) <= wire::STRING_CAP) {
        own.join_address = address;
    }
    own.max_players = uint64_t(transport_get_max_players());
    rows.push_back(own);
    return session::roster_write(rows);
}

void NetwMultiplayer::session_publish_admitted(
    const session::AcceptFrame &p_accepted,
    const Array &p_args
) {
    const session::AcceptFrame accepting = p_accepted;
    const Array decoded = p_args;
    const int64_t p_sender = accepting.peer_id;
    session_admit(accepting);

    if (inner.is_valid()) {
        set_peer_ids(gd::api_peer_ids(inner));
    }
    const PackedInt32Array peers = NETW_API_VIRTUAL(get_peer_ids)();
    for (int at = 0; at < int(peers.size()); ++at) {
        session_send_accept(accepting, peers[at]);
    }
    if (p_sender != get_unique_id()) {
        send_to(
            p_sender,
            0,
            wire::CHANNEL_SESSION_ROSTER,
            join_roster.roster_frame(p_sender),
            true,
            0,
            String()
        );
        liveness_grant_lease(p_sender);
        if (session_paused) {
            send_to(
                p_sender,
                0,
                wire::CHANNEL_SESSION_PAUSE,
                session::frame_write(
                    session::SessionReason{session_pause_reason}
                ),
                true,
                0,
                String()
            );
        }
    }
    session_run_join_handler(accepting, decoded);
}

PackedByteArray NetwMultiplayer::session_accept_payload(
    session::AcceptFrame p_row,
    int64_t p_destination
) const {
    if (p_destination == p_row.peer_id) {
        const String address = connect_core.join_address();
        const int64_t length = wire::string_bytes(address);
        if (length <= wire::STRING_CAP) {
            p_row.join_address = address;
        } else {
            fault::raise(
                nullptr,
                NETW_FAULT_LIMIT_EXCEEDED,
                {String("{NetwConnection.join_address}"),
                 String("the address in the join packet"),
                 String(
                     "a shorter address in the transport or lobby "
                     "directory"
                 ),
                 p_destination,
                 length,
                 int64_t(wire::STRING_CAP)}
            );
        }
        p_row.max_players = uint64_t(transport_get_max_players());
    }
    return session::frame_write(session::accept_seen_by(p_row, p_destination));
}

void NetwMultiplayer::session_send_accept(
    const session::AcceptFrame &p_row,
    int64_t p_destination
) {
    send_to(
        p_destination,
        0,
        wire::CHANNEL_SESSION_ACCEPT,
        session_accept_payload(p_row, p_destination),
        true,
        0,
        String()
    );
}

void NetwMultiplayer::session_receive_accept(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (p_sender != SERVER_PEER) {
        return;
    }
    session::AcceptFrame accepted;
    if (!session::accept_read(p_payload, accepted)) {
        NETW_TRACE(
            sys::SESSION,
            "A join acceptance that did not decode fully was ignored."
        );
        return;
    }
    if (accepted.peer_id == get_unique_id()) {
        connect_core.advertise(
            accepted.join_address,
            int64_t(accepted.max_players)
        );
    }
    session_admit(accepted);
}

int64_t NetwMultiplayer::transport_get_max_players() const {
    if (!has_server_role()) {
        return connect_core.advertised_max_players();
    }
    return session_settings.server_info.is_valid()
        ? session_settings.server_info->get_max_players()
        : 0;
}

Error NetwMultiplayer::transport_set_join_address(const String &p_address) {
    if (!session_is_online()) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {"{NetwConnection.join_address}", String(), "the session"}
        );
        return ERR_UNAVAILABLE;
    }
    if (!has_server_role()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwConnection.join_address}"}
        );
        return ERR_UNAVAILABLE;
    }
    if (wire::string_bytes(p_address) > wire::STRING_CAP) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwConnection.join_address}",
             vformat(
                 "an address %d bytes long",
                 int(wire::string_bytes(p_address))
             ),
             vformat("an address of at most %d bytes", wire::STRING_CAP)}
        );
        return ERR_INVALID_PARAMETER;
    }
    connect_core.advertise(p_address, 0);
    const int64_t mine = get_unique_id();
    for (const session::AcceptFrame &row : join_roster.accepted_joins()) {
        if (row.peer_id != mine) {
            session_send_accept(row, row.peer_id);
        }
    }
    return OK;
}

void NetwMultiplayer::session_receive_roster(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (p_sender != SERVER_PEER) {
        return;
    }
    LocalVector<session::AcceptFrame> rows;
    if (!session::roster_read(p_payload, rows)) {
        NETW_TRACE(
            sys::SESSION,
            "A roster that did not decode fully was ignored."
        );
        return;
    }
    for (uint32_t at = 0; at < rows.size(); ++at) {
        session_admit(rows[at]);
    }
    if (transfer_awaits_roster) {
        transfer_readmitted();
    }
}

void NetwMultiplayer::session_receive_control(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    int64_t p_channel
) {
    session_publish_control(p_channel, p_sender, p_payload);
}

SessionCore &NetwMultiplayer::session_plane() {
    return session_core;
}

void NetwMultiplayer::session_announce_entered() {
    session_exit_reason = -1;
    session_exit_detail = String();
    transfer_label = get_unique_id();
    transfer_installed = TransferInstall();
    spawner_session_entered();
    scene_on_session_entered();
    if (world.clock.get_handshakes_on_enter()) {
        clock_start_handshake();
    }
    Announcement row;
    row.kind = AnnouncementKind::ENTERED;
    announce(row);
    scene_player_display_invalidate();
    entity_publish_placed();
    entity_demote_placed();
    session_after_entered();
    connect_core.on_session_online();
}

void NetwMultiplayer::session_announce_ended() {
    const int reason = session_exit_reason >= 0
        ? session_exit_reason
        : int(enums::NetwSession::CONNECTION_LOST);
    const String detail = session_exit_detail;
    session_exit_reason = -1;
    session_exit_detail = String();
    announce_drain();
    if (views.session_handle.is_valid()) {
        views.session_handle->announce_exiting(
            enums::NetwSession::ExitReason(reason),
            detail
        );
    }
    session_apply_pause(false, String());
    spawner_session_ended();
    rpc_drop_deferred_calls();
    liveness_settle_clear_session();
    interest_session_ended();
    session_settle_clear_state();
    clear_roster();
    session_set_role(ROLE_NONE);
    scene_on_session_reclaimed();
    if (spawn::Pipeline *pipeline = spawn_plane()) {
        const LocalVector<ObjectID> nodes = pipeline->end_session_nodes();
        for (const ObjectID &id : nodes) {
            free_returned(Variant(gd::instance_from_id(id)), Variant());
        }
    }
    scene_player_display_invalidate();
    callable_mp(this, &NetwMultiplayer::session_announce_exited)
        .call_deferred(reason, detail);
}

void NetwMultiplayer::session_announce_exited(
    int p_reason,
    const String &p_detail
) {
    if (session_get_stored_state() == SESSION_STATE_DISCONNECTING) {
        session_exited_awaits_offline = p_reason;
        session_exited_detail = p_detail;
        return;
    }
    if (views.session_handle.is_valid()) {
        views.session_handle->announce_exited(
            enums::NetwSession::ExitReason(p_reason),
            p_detail
        );
    }
}

void NetwMultiplayer::session_note_exit(
    enums::NetwSession::ExitReason p_reason,
    const String &p_detail
) {
    if (session_exit_reason >= 0
        || session_get_stored_state() != SESSION_STATE_ONLINE) {
        return;
    }
    session_exit_reason = int(p_reason);
    session_exit_detail = p_detail;
}

void NetwMultiplayer::session_leave_for_schema(const String &p_node) {
    if (is_host() || session_exit_reason >= 0
        || session_get_stored_state() != SESSION_STATE_ONLINE) {
        return;
    }
    const String reason = vformat(
        "'%s' synchronizes different properties here than on the server. "
        "Run the same build on both peers.",
        p_node
    );
    fault::raise(
        this,
        NETW_FAULT_PEER_MISMATCH,
        {String("its session"),
         String(),
         String("the synchronized properties of '") + p_node + String("'"),
         SERVER_PEER}
    );
    session_note_exit(enums::NetwSession::KICKED, reason);
    schema_leave_pending = true;
}

void NetwMultiplayer::session_announce_edge(int p_old, int p_new) {
    sim_follow_server();
    session_after_edge(p_old, p_new);
    connect_core.on_session_edge(p_old, p_new);
    session_publish_state();
}

void NetwMultiplayer::session_publish_state() {
    const SessionState now = session_get_state();
    if (now == session_published_state) {
        return;
    }
    session_published_state = now;
    Announcement row;
    row.kind = AnnouncementKind::STATE_CHANGED;
    row.code = int64_t(now);
    announce(row);
}

session::AcceptFrame NetwMultiplayer::session_accepted_join(
    int64_t p_peer
) const {
    return join_roster.accepted_join(p_peer);
}

bool NetwMultiplayer::session_has_accepted(int64_t p_peer) const {
    return join_roster.has_accepted(p_peer);
}

LocalVector<session::AcceptFrame> NetwMultiplayer::
    session_accepted_joins() const {
    return join_roster.accepted_joins();
}

bool NetwMultiplayer::session_remember_join(
    const session::AcceptFrame &p_accepted
) {
    return join_roster.remember(p_accepted);
}

#if defined(NETW_TESTS)
PackedByteArray NetwMultiplayer::session_accept_bytes(
    int64_t p_peer,
    int64_t p_destination
) const {
    if (!join_roster.has_accepted(p_peer)) {
        return PackedByteArray();
    }
    return session_accept_payload(
        join_roster.accepted_join(p_peer),
        p_destination
    );
}

PackedByteArray NetwMultiplayer::session_roster_bytes(
    int64_t p_destination
) const {
    return join_roster.roster_frame(p_destination);
}
#endif

void NetwMultiplayer::session_forget_peer(int64_t p_peer) {
    peer_buckets.erase(p_peer);
    join_roster.forget(p_peer);
}

void NetwMultiplayer::session_clear_roster() {
    peer_buckets.clear();
    link_handshakes.clear();
    pending_admissions.clear();
    join_roster.clear();
    unreachable_peers.clear();
    settled_peer_departures.clear();
}

PackedInt32Array NetwMultiplayer::reachable_peer_ids() const {
    const PackedInt32Array known = gd::api_peer_ids(inner);
    if (unreachable_peers.is_empty()) {
        return known;
    }
    PackedInt32Array out;
    for (int at = 0; at < known.size(); at++) {
        if (!unreachable_peers.has(int64_t(known[at]))) {
            out.push_back(known[at]);
        }
    }
    return out;
}

PackedInt32Array NetwMultiplayer::spawn_peer_ids() const {
    const PackedInt32Array reachable = reachable_peer_ids();
    if (resync_awaited.is_empty()) {
        return reachable;
    }
    PackedInt32Array out;
    for (int at = 0; at < reachable.size(); at++) {
        if (!resync_awaited.has(int64_t(reachable[at]))) {
            out.push_back(reachable[at]);
        }
    }
    return out;
}

void NetwMultiplayer::peer_sweep_transport_state() {
    if (!session_link_is_connected()) {
        return;
    }
    const Ref<MultiplayerPeer> link = inner->get_multiplayer_peer();
    const PackedInt32Array roster = gd::api_peer_ids(inner);
    for (int at = 0; at < roster.size(); at++) {
        const int64_t peer = int64_t(roster[at]);
        if (unreachable_peers.has(peer) || gd::peer_is_relayed(inner, peer)) {
            continue;
        }
        if (!gd::peer_can_receive(link, peer)) {
            peer_mark_unreachable(peer);
        }
    }
}

bool NetwMultiplayer::session_links_peers_but_not_server() const {
    if (!session_link_is_connected()
        || inner->get_multiplayer_peer()->get_unique_id() == SERVER_PEER) {
        return false;
    }
    const PackedInt32Array admitted = gd::api_peer_ids(inner);
    const PackedInt32Array authenticating = get_authenticating_peers();
    if (admitted.has(int32_t(SERVER_PEER))
        || authenticating.has(int32_t(SERVER_PEER))) {
        return false;
    }
    return !admitted.is_empty() || !authenticating.is_empty();
}

void NetwMultiplayer::session_watch_serverless_mesh() {
    if (serverless_mesh_polls >= SERVERLESS_MESH_WARN_POLLS
        || session_get_stored_state() != SESSION_STATE_CONNECTING
        || !session_links_peers_but_not_server()) {
        return;
    }
    serverless_mesh_polls += 1;
    if (serverless_mesh_polls == SERVERLESS_MESH_WARN_POLLS) {
        fault::raise(
            this,
            NETW_FAULT_PEER_REFUSED,
            {String(),
             "no link to peer 1",
             "the peer after it links to the endpoint created with id 1"}
        );
    }
}

void NetwMultiplayer::session_watch_late_server() {
    if (session_get_stored_state() != SESSION_STATE_CONNECTING
        || !session_link_is_connected()
        || NETW_API_VIRTUAL(get_unique_id)() != SERVER_PEER) {
        return;
    }
    session_transport_connected();
}

bool NetwMultiplayer::session_link_is_connected() const {
    const Ref<MultiplayerPeer> link = inner.is_valid()
        ? inner->get_multiplayer_peer()
        : Ref<MultiplayerPeer>();
    return link.is_valid()
        && link->get_connection_status()
        == MultiplayerPeer::CONNECTION_CONNECTED;
}

void NetwMultiplayer::peer_mark_unreachable(int64_t p_peer) {
    if (p_peer == 0 || unreachable_peers.has(p_peer)
        || gd::peer_is_relayed(inner, p_peer)) {
        return;
    }
    if (p_peer == SERVER_PEER && relabel_source.is_valid()) {
        return;
    }
    if (peer_released_by_transport(p_peer)) {
        NETW_TRACE(
            sys::TRANSPORT,
            "Peer %d is leaving, so a packet for it was dropped.",
            int(p_peer)
        );
    } else {
        fault::raise(
            this,
            NETW_FAULT_LINK_FAILED,
            {String("sending to it"),
             String(),
             vformat("peer %d (it refused a packet)", int(p_peer)),
             p_peer}
        );
    }
    session_settle_peer_loss(p_peer);
}

bool NetwMultiplayer::peer_released_by_transport(int64_t p_peer) const {
    if (!session_link_is_connected()) {
        return false;
    }
    const Ref<MultiplayerPeer> link = inner->get_multiplayer_peer();
    const LocalMultiplayerPeer *local
        = Object::cast_to<LocalMultiplayerPeer>(link.ptr());
    if (local != nullptr) {
        return !local->is_linked_to(int(p_peer));
    }
    return !gd::peer_can_receive(link, p_peer);
}

void NetwMultiplayer::peer_mark_reachable(int64_t p_peer) {
    unreachable_peers.erase(p_peer);
}

NodePath NetwMultiplayer::get_root_path() const {
    return inner.is_valid() ? inner->get_root_path() : NodePath();
}

void NetwMultiplayer::set_root_path(const NodePath &p_path) {
    if (inner.is_valid()) {
        inner->set_root_path(p_path);
    }
}

bool NetwMultiplayer::is_object_decoding_allowed() const {
    return inner.is_valid() && inner->is_object_decoding_allowed();
}

void NetwMultiplayer::set_allow_object_decoding(bool p_value) {
    if (inner.is_valid()) {
        inner->set_allow_object_decoding(p_value);
    }
}

double NetwMultiplayer::get_auth_timeout() const {
    return auth_timeout_seconds;
}

void NetwMultiplayer::set_auth_timeout(double p_seconds) {
    if (p_seconds < 0.0) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwMultiplayer.auth_timeout}",
             vformat("%s seconds", String::num(p_seconds)),
             "0 for no limit or a number of seconds above zero"}
        );
        return;
    }
    auth_timeout_seconds = p_seconds;
    if (inner.is_valid() && !auth_is_armed()) {
        inner->set_auth_timeout(p_seconds);
    }
}

bool NetwMultiplayer::is_refusing_new_connections() const {
    return inner.is_valid() && inner->is_refusing_new_connections();
}

void NetwMultiplayer::set_refuse_new_connections(bool p_value) {
    if (inner.is_valid()) {
        inner->set_refuse_new_connections(p_value);
    }
}

bool NetwMultiplayer::is_server_relay_enabled() const {
    return inner.is_valid() && inner->is_server_relay_enabled();
}

void NetwMultiplayer::set_server_relay_enabled(bool p_value) {
    if (inner.is_valid()) {
        inner->set_server_relay_enabled(p_value);
    }
}

int NetwMultiplayer::get_max_sync_packet_size() const {
    return inner.is_valid() ? inner->get_max_sync_packet_size() : 0;
}

void NetwMultiplayer::set_max_sync_packet_size(int p_value) {
    if (inner.is_valid()) {
        inner->set_max_sync_packet_size(p_value);
    }
}

int NetwMultiplayer::get_max_delta_packet_size() const {
    return inner.is_valid() ? inner->get_max_delta_packet_size() : 0;
}

void NetwMultiplayer::set_max_delta_packet_size(int p_value) {
    if (inner.is_valid()) {
        inner->set_max_delta_packet_size(p_value);
    }
}

int NetwMultiplayer::get_max_future_action_ticks() const {
    return max_future_action_ticks;
}

void NetwMultiplayer::set_max_future_action_ticks(int p_value) {
    max_future_action_ticks = p_value;
}

int NetwMultiplayer::get_input_gate_deadline_ticks() const {
    return input_gate_deadline_ticks;
}

void NetwMultiplayer::set_input_gate_deadline_ticks(int p_value) {
    input_gate_deadline_ticks = p_value;
}

TypedArray<NetwPlayer> NetwMultiplayer::get_connected_players() const {
    return player_all();
}

Error NetwMultiplayer::configuration_door_refuses_config(
    NetwMultiplayer *p_session,
    Object *p_config
) {
    const char *verb = nullptr;
    if (Object::cast_to<NetwSessionConfig>(p_config) != nullptr) {
        verb = "Netw.configure_session";
    } else if (Object::cast_to<NetwClockConfig>(p_config) != nullptr) {
        verb = "Netw.configure_clock";
    } else if (
        Object::cast_to<NetwLagCompensationConfig>(p_config) != nullptr
    ) {
        verb = "Netw.configure_lagcomp";
    }
    if (verb == nullptr) {
        return OK;
    }
    fault::raise(
        p_session,
        NETW_FAULT_UNSUPPORTED,
        {vformat(
             "A %s passed to {MultiplayerAPI.object_configuration_add}",
             p_config->get_class()
         ),
         "the engine's configuration door",
         vformat("{%s} before the session starts", verb)}
    );
    return ERR_UNAVAILABLE;
}

Error NetwMultiplayer::send_auth(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    if (inner.is_null()) {
        return ERR_UNCONFIGURED;
    }
    if (!auth_is_armed()) {
        return inner->send_auth(int32_t(p_peer), p_data);
    }
    if (p_data.is_empty()) {
        return ERR_INVALID_PARAMETER;
    }
    return inner->send_auth(int32_t(p_peer), auth::encode_game(p_data));
}

Error NetwMultiplayer::complete_auth(int64_t p_peer) {
    if (inner.is_null()) {
        return ERR_UNCONFIGURED;
    }
    if (!auth_is_armed()) {
        return inner->complete_auth(int32_t(p_peer));
    }
    if (!get_authenticating_peers().has(int32_t(p_peer))) {
        return ERR_INVALID_PARAMETER;
    }
    LinkHandshake &link = link_handshakes[p_peer];
    if (link.game_completed) {
        return ERR_FILE_CANT_WRITE;
    }
    link.game_completed = true;
    auth_advance(p_peer);
    return OK;
}

Error NetwMultiplayer::auth_send_raw(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    if (inner.is_null()) {
        return ERR_UNCONFIGURED;
    }
    return inner->send_auth(int32_t(p_peer), p_data);
}

bool NetwMultiplayer::auth_is_armed() const {
    return inner.is_valid()
        && inner->get_auth_callback()
        == callable_mp(
               const_cast<NetwMultiplayer *>(this),
               &NetwMultiplayer::auth_receive
        );
}

PackedInt32Array NetwMultiplayer::get_authenticating_peers() const {
    if (inner.is_null()) {
        return PackedInt32Array();
    }
#if defined(NETW_MODULE)
    return inner->get_authenticating_peer_ids();
#else
    return inner->get_authenticating_peers();
#endif
}

void NetwMultiplayer::auth_set_app_tag(int64_t p_tag) {
    auth_app_tag = p_tag;
}

int64_t NetwMultiplayer::auth_app_tag_of() const {
    return auth_app_tag;
}

void NetwMultiplayer::set_auth_callback(const Callable &p_callback) {
    auth_app_callback = p_callback;
}

Callable NetwMultiplayer::get_auth_callback() const {
    return auth_app_callback;
}

void NetwMultiplayer::auth_clear() {
    auth_disarm();
    auth_app_callback = Callable();
}

void NetwMultiplayer::auth_arm() {
    if (inner.is_null()) {
        return;
    }
    inner->set_auth_callback(callable_mp(this, &NetwMultiplayer::auth_receive));
    inner->set_auth_timeout(0.0);
}

void NetwMultiplayer::auth_disarm() {
    if (inner.is_null()) {
        return;
    }
    const Callable ours = callable_mp(this, &NetwMultiplayer::auth_receive);
    if (inner->get_auth_callback() == ours) {
        inner->set_auth_callback(Callable());
        inner->set_auth_timeout(auth_timeout_seconds);
    }
}

void NetwMultiplayer::auth_receive(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    const auth::Kind kind = auth::classify(p_data);
    if (kind == auth::Kind::PROBE) {
        session_answer_probe(p_peer);
        return;
    }
    const LinkHandshake *link = link_handshakes.getptr(p_peer);
    if (link != nullptr && link->refused) {
        return;
    }
    switch (kind) {
        case auth::Kind::HELLO:
            auth_receive_hello(p_peer, p_data);
            return;
        case auth::Kind::GAME:
            auth_receive_game(p_peer, p_data);
            return;
        case auth::Kind::JOIN:
            auth_receive_join(p_peer, p_data);
            return;
        case auth::Kind::ANSWER:
            auth_receive_answer(p_peer, p_data);
            return;
        default:
            break;
    }
    fault::raise(
        this,
        NETW_FAULT_MALFORMED_DATAGRAM,
        {String(), String(), String(), p_peer}
    );
    disconnect_peer(p_peer);
}

void NetwMultiplayer::auth_send_hello(int64_t p_peer) {
    NETW_ZONE_NC("netw::auth_send_hello", colors::SESSION);
    if (p_peer == 0 || inner.is_null()
        || !get_authenticating_peers().has(int32_t(p_peer))) {
        return;
    }
    connect_core.peer_connected(p_peer);
    link_handshakes[p_peer] = LinkHandshake();
    LinkHandshake &opened = link_handshakes[p_peer];
    const double seconds = auth_link_seconds();
    opened.deadline = seconds > 0.0
        ? int64_t(Time::get_singleton()->get_ticks_msec())
            + int64_t(seconds * 1000.0)
        : 0;
    if (has_server_role()) {
        return;
    }
    const bool to_server = p_peer == SERVER_PEER;
    const bool proves = to_server && session_requires_proof();
    int flags = proves ? auth::HELLO_PROOF_CAPABLE : 0;
    if (auth_app_callback.is_valid()) {
        flags |= auth::HELLO_GAME_AUTH;
    }
    NETW_DEBUG(
        sys::SESSION,
        "Sending a hello to peer %d with app tag %s.",
        int(p_peer),
        String::num_uint64(auth_app_tag, 16)
    );
    const Error sent = auth_send_raw(
        p_peer,
        auth::encode_client_hello(uint64_t(auth_app_tag), flags)
    );
    if (sent != OK) {
        fault::raise(
            this,
            NETW_FAULT_LINK_FAILED,
            {String("authentication"),
             String(),
             vformat("peer %d (error %d)", int(p_peer), int(sent)),
             p_peer}
        );
        return;
    }
    auth_advance(p_peer);
}

void NetwMultiplayer::auth_advance(int64_t p_peer) {
    LinkHandshake *link = link_handshakes.getptr(p_peer);
    if (link == nullptr || link->completed || link->refused) {
        return;
    }
    const bool game_done
        = !auth_app_callback.is_valid() || link->game_completed;
    if (!game_done) {
        return;
    }
    if (has_server_role()) {
        if (!link->hello_heard) {
            return;
        }
        if (get_unique_id() != SERVER_PEER) {
            auth_complete_link(p_peer);
            return;
        }
        if (!link->join_received || link->joining) {
            return;
        }
        link->joining = true;
        const PackedByteArray join = link->join;
        session_take_join(join, p_peer);
        return;
    }
    if (p_peer != SERVER_PEER && !join_roster.has_accepted(p_peer)) {
        return;
    }
    if (p_peer != SERVER_PEER) {
        auth_complete_link(p_peer);
        return;
    }
    if (!link->hello_heard || (link->challenged && !link->proven)
        || link->join_sent) {
        return;
    }
    link->join_sent = true;
    JoinRequest request;
    if (prepared_join.has_value()) {
        request = *prepared_join;
        prepared_join.reset();
    }
    session_submit_request(request);
    connection_announce_progressed(
        String("Waiting for the server to admit the player."),
        -1.0
    );
}

Error NetwMultiplayer::auth_complete_link(int64_t p_peer) {
    LinkHandshake &link = link_handshakes[p_peer];
    if (link.completed) {
        return OK;
    }
    link.completed = true;
    if (inner.is_null()) {
        return ERR_UNCONFIGURED;
    }
    const Error completed = inner->complete_auth(int32_t(p_peer));
    if (completed != OK) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_peer}
        );
        disconnect_peer(p_peer);
    }
    return completed;
}

void NetwMultiplayer::auth_forget_link(int64_t p_peer) {
    link_handshakes.erase(p_peer);
    pending_admissions.erase(p_peer);
}

NetwMultiplayer::LinkHandshake *NetwMultiplayer::auth_joining_link(
    int64_t p_peer
) {
    LinkHandshake *link = link_handshakes.getptr(p_peer);
    return link != nullptr && link->joining && !link->completed ? link
                                                                : nullptr;
}

void NetwMultiplayer::auth_receive_game(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    if (!auth_app_callback.is_valid()) {
        fault::raise(
            this,
            NETW_FAULT_PEER_MISMATCH,
            {String("its authentication"),
             String(),
             String("NetwMultiplayer.auth_callback"),
             p_peer}
        );
        disconnect_peer(p_peer);
        return;
    }
    Array args;
    args.push_back(p_peer);
    args.push_back(auth::payload_of(p_data));
    game_call(auth_app_callback, args, game_calls::AUTH_CALLBACK);
}

void NetwMultiplayer::auth_receive_join(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    LinkHandshake *link = link_handshakes.getptr(p_peer);
    if (!has_server_role() || get_unique_id() != SERVER_PEER || link == nullptr
        || !link->hello_heard) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_peer}
        );
        disconnect_peer(p_peer);
        return;
    }
    if (link->join_received) {
        return;
    }
    link->join_received = true;
    link->join = auth::payload_of(p_data);
    auth_advance(p_peer);
}

void NetwMultiplayer::auth_receive_answer(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    LinkHandshake *link = link_handshakes.getptr(p_peer);
    if (link == nullptr || link->completed) {
        return;
    }
    const auth::Answer answer = auth::decode_answer(p_data);
    if (has_server_role()) {
        if (answer.ok && answer.kind == auth::AnswerKind::CONFIRMED
            && link->admitted.has_value()) {
            auth_complete_link(p_peer);
        }
        return;
    }
    if (p_peer != SERVER_PEER
        || (answer.ok && answer.kind == auth::AnswerKind::CONFIRMED)
        || (answer.ok && answer.kind == auth::AnswerKind::ADMITTED
            && !link->join_sent)) {
        return;
    }
    if (!answer.ok) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), SERVER_PEER}
        );
        auth_refused_locally(
            ERR_UNCONFIGURED,
            "The server's answer could not be read. Run the same build of "
            "the game on both computers."
        );
        return;
    }
    if (answer.kind == auth::AnswerKind::ADMITTED) {
        auth_admitted_roster = answer.payload;
        auth_send_raw(p_peer, auth::encode_confirmed());
        auth_complete_link(p_peer);
        return;
    }
    link->refused = true;
    auth_refused_locally(Error(answer.code), answer.reason);
}

bool NetwMultiplayer::auth_refuse(
    int64_t p_peer,
    Error p_code,
    const String &p_reason
) {
    LinkHandshake *link = link_handshakes.getptr(p_peer);
    if (link == nullptr || link->completed || !link->hello_heard
        || p_peer == get_unique_id()) {
        return false;
    }
    if (link->refused) {
        return true;
    }
    link->refused = true;
    link->refused_polls = 0;
    link->join = PackedByteArray();
    pending_admissions.erase(p_peer);
    auth_send_raw(p_peer, auth::encode_refused(int64_t(p_code), p_reason));
    return true;
}

void NetwMultiplayer::auth_poll_refused() {
    if (!has_server_role()) {
        return;
    }
    const int64_t now = int64_t(Time::get_singleton()->get_ticks_msec());
    LocalVector<int64_t> due;
    LocalVector<int64_t> remembered;
    LocalVector<int64_t> expired;
    for (KeyValue<int64_t, LinkHandshake> &entry : link_handshakes) {
        LinkHandshake &link = entry.value;
        if (!link.refused) {
            continue;
        }
        if (link.refused_polls < POLLS_BEFORE_TURNED_AWAY_PEER_DROPS) {
            link.refused_polls += 1;
            if (link.refused_polls == POLLS_BEFORE_TURNED_AWAY_PEER_DROPS) {
                (link.deadline > now ? remembered : due).push_back(entry.key);
            }
            continue;
        }
        if (link.deadline <= now) {
            expired.push_back(entry.key);
        }
    }
    for (const int64_t peer : due) {
        link_handshakes.erase(peer);
        disconnect_peer(peer);
    }
    for (const int64_t peer : remembered) {
        if (effective_peer.is_valid()) {
            effective_peer->disconnect_peer(int32_t(peer));
        }
    }
    for (const int64_t peer : expired) {
        const bool pending = get_authenticating_peers().has(int32_t(peer));
        relay_auth_failed(peer);
        if (pending) {
            disconnect_peer(peer);
        }
    }
}

void NetwMultiplayer::auth_refused_locally(
    Error p_code,
    const String &p_reason
) {
    connect_core.on_join_refused(p_code, p_reason);
    session_report_join_failure(p_code, p_reason);
    callable_mp(this, &NetwMultiplayer::auth_close_refused_peer)
        .call_deferred();
}

void NetwMultiplayer::auth_close_refused_peer() {
    if (session_get_stored_state() != SESSION_STATE_CONNECTING) {
        return;
    }
    const Ref<MultiplayerPeer> refused = assigned_peer;
    peer_assign(Ref<MultiplayerPeer>());
    if (refused.is_valid()) {
        refused->close();
    }
}

#if defined(NETW_TESTS)
bool NetwMultiplayer::auth_link_is_completed(int64_t p_peer) const {
    const HashMap<int64_t, LinkHandshake>::ConstIterator found
        = link_handshakes.find(p_peer);
    return found != link_handshakes.end() && found->value.completed;
}
#endif

void NetwMultiplayer::auth_receive_hello(
    int64_t p_peer,
    const PackedByteArray &p_data
) {
    NETW_ZONE_NC("netw::auth_receive_hello", colors::SESSION);
    const auth::Hello decoded
        = auth::decode_client_hello(p_data, uint64_t(auth_app_tag));
    NETW_DEBUG(
        sys::SESSION,
        "Read a hello from peer %d with app tag %s, expecting %s.",
        int(p_peer),
        String::num_uint64(int64_t(decoded.app_tag), 16),
        String::num_uint64(auth_app_tag, 16)
    );
    if (!decoded.ok()) {
        if (decoded.refusal == auth::Refusal::APP) {
            fault::raise(
                this,
                NETW_FAULT_PEER_MISMATCH,
                {String("its authentication"),
                 String(),
                 String("the app tag"),
                 p_peer}
            );
            session_refuse(p_peer, "Incompatible game build");
        } else {
            fault::raise(
                this,
                NETW_FAULT_MALFORMED_DATAGRAM,
                {String(), String(), String(), p_peer}
            );
        }
        if (decoded.refusal == auth::Refusal::FRAMING) {
            disconnect_peer(p_peer);
            return;
        }
        const String outdated = decoded.refusal == auth::Refusal::APP
            ? "The server runs another build of this game. Update the game "
              "on both computers."
            : "The server runs another version of netwkd. Update the "
              "game on both computers.";
        if (!has_server_role() && p_peer == SERVER_PEER) {
            session_fail_own_join(p_peer, ERR_UNCONFIGURED, outdated);
            return;
        }
        if (has_server_role()) {
            link_handshakes[p_peer].hello_heard = true;
            if (auth_refuse(p_peer, ERR_UNCONFIGURED, outdated)) {
                return;
            }
        }
        disconnect_peer(p_peer);
        return;
    }
    LinkHandshake &heard = link_handshakes[p_peer];
    if (heard.completed || (heard.hello_heard && !has_server_role())) {
        return;
    }
    heard.hello_heard = true;
    const int64_t now = int64_t(Time::get_singleton()->get_ticks_msec());
    if (!has_server_role()) {
        if (p_peer == SERVER_PEER) {
            heard.deadline
                = decoded.deadline_msec > 0 ? now + decoded.deadline_msec : 0;
        }
        if (p_peer != SERVER_PEER
            || (decoded.flags & auth::HELLO_PROOF_CHALLENGE) == 0) {
            auth_advance(p_peer);
            return;
        }
        if (p_data.size() != auth::HELLO_HEADER_LEN + auth::CHALLENGE_LENGTH) {
            disconnect_peer(p_peer);
            return;
        }
        heard.challenged = true;
        session_request_proof(
            p_peer,
            String::utf8(
                reinterpret_cast<const char *>(p_data.ptr())
                    + auth::HELLO_HEADER_LEN,
                auth::CHALLENGE_LENGTH
            )
        );
        return;
    }
    const bool game_auth = (decoded.flags & auth::HELLO_GAME_AUTH) != 0;
    if (game_auth != auth_app_callback.is_valid()) {
        const String unmatched = game_auth
            ? "The joining peer sets NetwMultiplayer.auth_callback and this "
              "server does not. Set the same auth_callback on both peers."
            : "This server sets NetwMultiplayer.auth_callback and the joining "
              "peer does not. Set the same auth_callback on both peers.";
        fault::raise(
            this,
            NETW_FAULT_PEER_MISMATCH,
            {String("its authentication"),
             String(),
             String("NetwMultiplayer.auth_callback"),
             p_peer}
        );
        session_refuse(p_peer, unmatched);
        if (!auth_refuse(p_peer, ERR_UNCONFIGURED, unmatched)) {
            disconnect_peer(p_peer);
        }
        return;
    }
    const bool required = session_requires_proof();
    int flags = required ? auth::HELLO_PROOF_CHALLENGE : 0;
    if (auth_app_callback.is_valid()) {
        flags |= auth::HELLO_GAME_AUTH;
    }
    const int64_t left
        = heard.deadline > 0 ? MAX(heard.deadline - now, int64_t(1)) : 0;
    PackedByteArray reply
        = auth::encode_client_hello(uint64_t(auth_app_tag), flags, left);
    if (required) {
        if (heard.challenge.is_empty()) {
            heard.challenge = session_mint_challenge();
        }
        if (heard.challenge.is_empty()) {
            disconnect_peer(p_peer);
            return;
        }
        const CharString text = heard.challenge.utf8();
        for (int at = 0; at < text.length(); ++at) {
            reply.push_back(uint8_t(text[at]));
        }
    }
    if (auth_send_raw(p_peer, reply) != OK) {
        disconnect_peer(p_peer);
        return;
    }
    auth_advance(p_peer);
}

session_decl::Book &NetwMultiplayer::declaration_book() {
    return declaration_slots;
}

void NetwMultiplayer::declarations_install_from(Node *p_scope) {
    declaration_slots.install_from(this, p_scope);
}

void NetwMultiplayer::declarations_withdraw(
    ObjectID p_scope,
    session_decl::Kind p_kind,
    uint64_t p_generation
) {
    declaration_slots.withdraw(p_scope, p_kind, p_generation);
}

FaultBook::Admit NetwMultiplayer::fault_admit(const FaultKey &p_key) {
    return faults.admit(p_key, clock_get_tick());
}

PackedByteArray NetwMultiplayer::probe_reply_payload(bool &r_answered) {
    r_answered = true;
    const Ref<NetwServerInfo> base = session_get_server_info();
    const session_decl::Resolved declared
        = declaration_slots.resolve(this, session_decl::KIND_SERVER_INFO);
    if (declared.state == session_decl::ABSENT) {
        return NetwServerInfo::payload_for(base, this);
    }
    if (declared.state != session_decl::READY) {
        declaration_slots.report_unresolved(
            this,
            session_decl::KIND_SERVER_INFO,
            declared.state
        );
        r_answered = false;
        return PackedByteArray();
    }
    Array args;
    args.push_back(base);
    game_call(declared.callable, args, game_calls::SERVER_INFO_HANDLER);
    return NetwServerInfo::payload_for(base, this);
}

void NetwMultiplayer::session_answer_probe(int64_t p_peer) {
    NETW_ZONE_NC("netw::session_answer_probe", colors::TRANSPORT);
    if (inner.is_null()) {
        return;
    }
    const int64_t now = int64_t(Time::get_singleton()->get_ticks_msec());
    if (!probe_guard.admit(p_peer, now)) {
        NETW_TRACE(
            sys::TRANSPORT,
            "A probe from peer %d got a busy answer (%d this second, %d "
            "tracked).",
            int(p_peer),
            probe_guard.window_count(),
            probe_guard.active_count()
        );
        auth_send_raw(
            p_peer,
            auth::encode_probe_reply(
                int(auth::ProbeStatus::BUSY),
                PackedByteArray()
            )
        );
        return;
    }
    bool answered = false;
    const PackedByteArray payload = probe_reply_payload(answered);
    auth_send_raw(
        p_peer,
        auth::encode_probe_reply(
            int(answered ? auth::ProbeStatus::OK : auth::ProbeStatus::ERROR),
            payload
        )
    );
}

bool NetwMultiplayer::probe_forget(int64_t p_peer) {
    return probe_guard.forget(p_peer);
}

void NetwMultiplayer::probe_clear() {
    probe_guard.clear();
}

void NetwMultiplayer::disconnect_peer(int64_t p_peer) {
    if (inner.is_null()) {
        return;
    }
    inner->disconnect_peer(int32_t(p_peer));
}

void NetwMultiplayer::clear() {
    if (inner.is_null()) {
        return;
    }
    inner->clear();
}

bool NetwMultiplayer::peer_has_bucket(
    int64_t p_peer,
    const Variant &p_type
) const {
    const Dictionary *held = peer_buckets.getptr(p_peer);
    return held != nullptr && held->has(p_type);
}

Variant NetwMultiplayer::peer_get_bucket(
    int64_t p_peer,
    const Variant &p_type
) {
    if (!session_has_accepted(p_peer)) {
        fault::raise(
            this,
            NETW_FAULT_UNKNOWN_PEER,
            {"{NetwSession.bucket_of}", String(), String(), p_peer}
        );
        return Variant();
    }
    Object *type = Object::cast_to<Object>(p_type);
    if (type == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_BAD_SCRIPT,
            {"{NetwSession.bucket_of}",
             "no script",
             "a script class that extends RefCounted"}
        );
        return Variant();
    }
    if (!peer_buckets.has(p_peer)) {
        peer_buckets.insert(p_peer, Dictionary());
    }
    Dictionary &held = peer_buckets[p_peer];
    if (held.has(p_type)) {
        return held[p_type];
    }
    const Ref<RefCounted> minted
        = Ref<RefCounted>(type->call(StringName("new")));
    if (minted.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_BAD_SCRIPT,
            {"{NetwSession.bucket_of}",
             "a script that does not extend RefCounted",
             "a script class that extends RefCounted"}
        );
        return Variant();
    }
    held[p_type] = minted;
    return minted;
}

void NetwMultiplayer::session_refuse(int64_t p_peer, const String &p_reason) {
    join_roster.refuse(p_peer, p_reason);
}

String NetwMultiplayer::session_refusal(int64_t p_peer) const {
    return join_roster.refusal(p_peer);
}

namespace {

bool is_session_core_state(int64_t p_state) {
    return p_state == SessionCore::STATE_OFFLINE
        || p_state == SessionCore::STATE_CONNECTING
        || p_state == SessionCore::STATE_ONLINE
        || p_state == SessionCore::STATE_DISCONNECTING;
}

} // namespace

void NetwMultiplayer::session_set_state(SessionState p_state) {
    if (!is_session_core_state(p_state)) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwMultiplayer.session_set_state}",
             vformat("state %d", int(p_state)),
             "{NetwSession.STATE_OFFLINE}, {NetwSession.STATE_CONNECTING}, "
             "{NetwSession.STATE_ONLINE} or "
             "{NetwSession.STATE_DISCONNECTING}"}
        );
        return;
    }
    session_core.set_state(SessionCore::State(p_state));
}

void NetwMultiplayer::session_set_role(Role p_role) {
    session_core.set_role(SessionCore::Role(p_role));
    sim_follow_server();
}

void NetwMultiplayer::session_set_desired_role(Role p_role) {
    session_core.set_desired_role(SessionCore::Role(p_role));
}

void NetwMultiplayer::session_transition(SessionState p_state) {
    if (!is_session_core_state(p_state)) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwMultiplayer.session_transition}",
             vformat("state %d", int(p_state)),
             "{NetwSession.STATE_OFFLINE}, {NetwSession.STATE_CONNECTING}, "
             "{NetwSession.STATE_ONLINE} or "
             "{NetwSession.STATE_DISCONNECTING}"}
        );
        return;
    }
    session_core.transition(SessionCore::State(p_state));
}

void NetwMultiplayer::session_peer_assigned(bool p_live, int p_unique_id) {
    session_core.on_peer_assigned(p_live, p_unique_id);
}

void NetwMultiplayer::session_resolve_online(int p_unique_id) {
    session_core.resolve_online(p_unique_id);
}

void NetwMultiplayer::session_set_join_override(
    const Callable &p_handler,
    const Array &p_quantizers
) {
    join_handler_override = p_handler;
    join_handler_override_quantizers = p_quantizers;
}

Callable NetwMultiplayer::session_join_override() const {
    return join_handler_override.is_valid() ? join_handler_override
                                            : Callable();
}

Array NetwMultiplayer::session_join_override_quantizers() const {
    return join_handler_override.is_valid() ? join_handler_override_quantizers
                                            : Array();
}

int64_t NetwMultiplayer::session_app_tag(const StringName &p_app_id) {
    return SessionCore::compute_app_tag(p_app_id);
}

void NetwMultiplayer::session_broadcast_control(
    uint8_t p_channel,
    const PackedByteArray &p_payload
) {
    if (inner.is_valid()) {
        set_peer_ids(gd::api_peer_ids(inner));
    }
    const PackedInt32Array peers = NETW_API_VIRTUAL(get_peer_ids)();
    for (int at = 0; at < int(peers.size()); ++at) {
        send_to(peers[at], 0, p_channel, p_payload, true, 0, String());
    }
    session_publish_control(
        p_channel,
        MultiplayerPeer::TARGET_PEER_SERVER,
        p_payload
    );
}

void NetwMultiplayer::session_pause(const String &p_reason) {
    if (!session_core.is_server_role()) {
        fault::raise(this, NETW_FAULT_SERVER_ONLY, {"{NetwSession.pause}"});
        return;
    }
    session_broadcast_control(
        wire::CHANNEL_SESSION_PAUSE,
        session::frame_write(session::SessionReason{p_reason})
    );
}

void NetwMultiplayer::session_unpause() {
    if (!session_core.is_server_role()) {
        fault::raise(this, NETW_FAULT_SERVER_ONLY, {"{NetwSession.unpause}"});
        return;
    }
    session_broadcast_control(wire::CHANNEL_SESSION_UNPAUSE, PackedByteArray());
}

bool NetwMultiplayer::session_is_paused() const {
    return session_paused;
}

void NetwMultiplayer::session_notify_shutdown(const String &p_reason) {
    if (!session_core.is_server_role()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwMultiplayer.session_notify_shutdown}"}
        );
        return;
    }
    session_broadcast_control(
        wire::CHANNEL_SESSION_SHUTDOWN,
        session::frame_write(session::SessionReason{p_reason})
    );
}

namespace {

LocalVector<uint64_t> &sessions_tearing_down() {
    static LocalVector<uint64_t> sessions;
    return sessions;
}

void forget_torn_down_sessions() {
    LocalVector<uint64_t> &sessions = sessions_tearing_down();
    uint32_t at = 0;
    while (at < sessions.size()) {
        if (gd::object_registered(ObjectID(sessions[at]))) {
            ++at;
        } else {
            sessions.remove_at_unordered(at);
        }
    }
}

} // namespace

bool NetwMultiplayer::is_tearing_down(ObjectID p_session) {
    forget_torn_down_sessions();
    return sessions_tearing_down().has(uint64_t(p_session));
}

NetwMultiplayer::~NetwMultiplayer() {
    prediction_engine.bind_session(
        nullptr,
        &world.clock,
        &store(),
        &liveness_core,
        &binding_book,
        &sim_rows
    );
    predict_runner.seat_core(nullptr, &prediction_engine);
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        row.value->session = nullptr;
    }
    for (NetwPredictSlotEngine *engine : predict_retired) {
        engine->session = nullptr;
    }
    if (replication_owner != nullptr) {
        replication_owner->unseat();
    }
    forget_torn_down_sessions();
    sessions_tearing_down().push_back(uint64_t(gd::instance_id(this)));
    tree_forget_session(this);
    if (session_paused) {
        session_set_tree_paused(false);
    }
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        row.value->release();
        predict_retired.push_back(row.value);
    }
    predict_engines.clear();
    for (NetwPredictSlotEngine *engine : predict_retired) {
        godot::memdelete(engine);
    }
    predict_retired.clear();
    capture_close();
    clock_release();
    lagcomp_release();
    declaration_slots.release();
    if (replication_owner != nullptr) {
        replication_owner->release_spawn_builds();
        memdelete(replication_owner);
        replication_owner = nullptr;
    }
    for (const KeyValue<uint64_t, RID> &held : directory_slots) {
        connect::free_transport_slot(held.value);
    }
    directory_slots.clear();
    announce_drop();
    wrapper_detach_all();
    layer_forget_all();
    views.endpoint_views.clear();
}

Error NetwMultiplayer::session_flush_tick(int64_t p_tick) {
    NETW_STAGE(OTHER, "Session tick", colors::SESSION);
    if (layer_driver_count() > 0) {
        const Error admitted = interest_flush_now();
        if (admitted != OK) {
            return admitted;
        }
    }
    ReplicationCore *plane = (get_replication_plane());
    if (plane != nullptr) {
        plane->on_clock_tick(p_tick);
    }
    const Error err = embed_poll_transport();
    settle_advance();
    session_flush_deferred();
    scene_presentation_warn_split();
    return err;
}

void NetwMultiplayer::session_on_clock_tick(double, int64_t p_tick) {
    sink_verdict(session_flush_tick(p_tick), 0);
#if defined(NETW_PROFILING)
    if (NETW_PROFILE_CONNECTED()) {
        profile_book_bytes();
    }
#endif
}

#if defined(NETW_PROFILING)
void NetwMultiplayer::profile_book_bytes() const {
    NETW_ZONE_NC("Memory sample", colors::SESSION);
    NETW_PLOT(
        profile::names::MEMORY_STORE_ROWS,
        store().row_bytes() + store_pull.held_bytes()
    );
    NETW_PLOT(profile::names::MEMORY_PEER_ROWS, store().peer_bytes());
    NETW_PLOT(profile::names::MEMORY_PEER_FLIGHTS, store().flight_bytes());
    int64_t transitions = 0;
    int64_t inputs = 0;
    int64_t journal = 0;
    int64_t slots = 0;
    prediction_engine.held_bytes(transitions, inputs, journal, slots);
    NETW_PLOT(profile::names::MEMORY_PREDICT_TRANSITIONS, transitions);
    NETW_PLOT(profile::names::MEMORY_PREDICT_INPUTS, inputs);
    NETW_PLOT(profile::names::MEMORY_PREDICT_JOURNAL, journal);
    NETW_PLOT(profile::names::MEMORY_PREDICT_SLOTS, slots);
    NETW_PLOT(profile::names::MEMORY_DISPLAY, display_book.held_bytes());
    NETW_PLOT(profile::names::MEMORY_BINDINGS, binding_book.held_bytes());
    NETW_PLOT(profile::names::MEMORY_SETS, set_book_bytes());
    NETW_PLOT(profile::names::MEMORY_ENTITIES, entity_bytes());
    const SyncCompat *compat = sync_adapter();
    NETW_PLOT(
        profile::names::MEMORY_COMPAT,
        compat != nullptr ? compat->held_bytes() : int64_t(0)
    );
}
#endif

#if defined(NETW_PROFILING) || defined(DEBUG_ENABLED)
int64_t NetwMultiplayer::set_book_bytes() const {
    return set_book.held_bytes([](const repl::PropertySet &p_set) {
        return held::vector(p_set.columns);
    });
}

int64_t NetwMultiplayer::entity_bytes() const {
    int64_t bytes = held::map(views.node_entities)
        + world.entities.held_bytes([](const EntityRow &p_slot) {
              return p_slot.record != nullptr ? p_slot.record->held_bytes()
                                              : int64_t(0);
          })
        + views.entities.held_bytes([](const session::EntityView &p_view) {
              const int64_t object = int64_t(sizeof(NetwEntity));
              int64_t owned = p_view.live.is_valid() ? object : 0;
              owned += p_view.retired.is_valid() ? object : 0;
              if (p_view.holder != nullptr) {
                  owned += p_view.holder->held_part_bytes();
              }
              return owned;
          });
    ReplicationCore *replication = get_replication_plane();
    if (replication != nullptr) {
        bytes += replication->get_spawn_pipeline()
                     ->get_spawn_book()
                     ->held_bytes();
    }
    return bytes;
}
#endif

void NetwMultiplayer::step_leave() {
    --step_depth;
    announce_flush();
    effect_drain();
    predict_free_retired();
}

Error NetwMultiplayer::NETW_API_VIRTUAL(poll)() {
    NETW_FRAME(profile::names::POLL_FRAME);
    NETW_STAGE(OTHER, "Network poll", colors::SESSION);
    Engine *engine = Engine::get_singleton();
    if (engine != nullptr && engine->is_editor_hint()) {
        return OK;
    }
    NETW_NETWORK_PROFILE(profiler_poll());
    const Ref<NetwMultiplayer> keep(this);
    ++poll_depth;
    step_enter();
    embed_settle_on_first_poll();
    if (!transfer_called) {
        transfer_gone.clear();
    }
    {
        NETW_STAGE(TRANSPORT, "Transport poll", colors::TRANSPORT);
        if (inner.is_null() && effective_peer.is_valid()) {
            effective_peer->poll();
        }
        peer_sweep_transport_state();
        session_watch_serverless_mesh();
        session_watch_late_server();
    }
    const double frame_delta
        = poll_delta(Time::get_singleton()->get_ticks_usec());
    session_poll_admissions(int64_t(Time::get_singleton()->get_ticks_msec()));
    connect_core.on_poll(frame_delta);
    world.wire.advance_traffic(frame_delta, packet_overhead());
    const Error err = embed_poll_transport();
    session_poll_shutdown();
    session_poll_server_shutdown();
    session_poll_turned_away();
    auth_poll_refused();
    if (schema_leave_pending) {
        schema_leave_pending = false;
        session_close_peer();
    }
    SceneLoads::shared().poll(engine->get_process_frames());
    if (spawn_plane() != nullptr) {
        spawn_retry_builds();
        spawn_sweep_pending();
    }
    const bool clocked = clock_engine().get_configured();
    if (!clocked) {
        settle_advance();
    }
    session_flush_deferred();
    if (!clocked) {
        scene_presentation_warn_split();
    }
    clock_engine().count_poll(clock_reading());
    if (config_is_consumed(session_decl::KIND_CLOCK_CONFIG)) {
        clock_attach_pump();
    }
    advance_frame();
    persist_pump(frame_delta);
    ReplicationCore *plane = (get_replication_plane());
    if (plane != nullptr) {
        plane->on_poll();
    }
    sink_verdict(display_pump(display_delta()), 0);
    step_leave();
    NETW_HINTS(diag_poll());
    NETW_STAGE_FLUSH();
    --poll_depth;
    return err;
}

bool NetwMultiplayer::peer_assignment_repeats(
    const Ref<MultiplayerPeer> &p_peer
) const {
    if (p_peer == assigned_peer) {
        return true;
    }
    return p_peer.is_valid() && p_peer == effective_peer;
}

Ref<MultiplayerPeer> NetwMultiplayer::peer_shaped(
    const Ref<MultiplayerPeer> &p_peer
) const {
#if defined(DEBUG_ENABLED)
    if (p_peer.is_null()
        || Object::cast_to<link::ShapedPeer>(*p_peer) != nullptr
        || p_peer->is_class("OfflineMultiplayerPeer")
        || (session_settings.link_conditions.is_null()
            && session_settings.host_link_conditions.is_null())) {
        return p_peer;
    }
    const Ref<NetwLinkConditions> &shaping = gd::peer_is_server(p_peer)
        ? session_settings.host_link_conditions
        : session_settings.link_conditions;
    if (shaping.is_null()) {
        return p_peer;
    }
    if (peer_relabels(p_peer)) {
        fault::raise(
            const_cast<NetwMultiplayer *>(this),
            NETW_FAULT_UNSUPPORTED,
            {String("The session link conditions"),
             vformat(
                 "%s, which can move the server",
                 String(p_peer->get_class())
             ),
             String("lag simulated inside the transport")}
        );
        return p_peer;
    }
    Ref<link::ShapedPeer> shaped;
    shaped.instantiate();
    shaped->initialize(p_peer, shaping);
    return shaped;
#else
    return p_peer;
#endif
}

void NetwMultiplayer::NETW_API_VIRTUAL(set_multiplayer_peer)(
    const Ref<MultiplayerPeer> &p_peer
) {
    peer_assign(p_peer);
    effect_drain();
}

void NetwMultiplayer::peer_assign(const Ref<MultiplayerPeer> &p_peer) {
    if (peer_assignment_repeats(p_peer)) {
        return;
    }
    const connect::PeerOffer *offered = connect::offer_of_peer(p_peer);
    if (offered != nullptr && !offered->claimed
        && offered->session != gd::instance_id(this)) {
        fault::raise(
            this,
            NETW_FAULT_PEER_REFUSED,
            {String(),
             "its link to another session",
             "the peer to the session that created it"}
        );
        return;
    }
    if (p_peer.is_valid()
        && p_peer->get_connection_status()
            == MultiplayerPeer::CONNECTION_DISCONNECTED) {
        fault::raise(
            this,
            NETW_FAULT_PEER_REFUSED,
            {String(),
             "its disconnected state",
             "the peer after create_server or create_client succeeds"}
        );
        return;
    }
    const bool live
        = p_peer.is_valid() && !p_peer->is_class("OfflineMultiplayerPeer");
    if (config_applying) {
        fault::raise(
            this,
            NETW_FAULT_BUSY_APPLYING,
            {"{MultiplayerAPI.multiplayer_peer}"}
        );
        return;
    }
    if (live) {
        if (apply_pending_config(false, "the peer assignment") != OK) {
            return;
        }
        session_apply_auth_config();
    }
    if (session_get_stored_state() == SESSION_STATE_ONLINE) {
        session_note_exit(
            live ? enums::NetwSession::REPLACED
                 : enums::NetwSession::LOCAL_LEAVE,
            String()
        );
        session_teardown_online();
    }
    const Ref<MultiplayerPeer> standing_assigned = assigned_peer;
    const Ref<MultiplayerPeer> standing_effective = effective_peer;
    assigned_peer = p_peer;
    effective_peer = peer_shaped(p_peer);
    if (inner.is_valid()) {
        inner->set_multiplayer_peer(effective_peer);
        if (inner->get_multiplayer_peer() != effective_peer) {
            assigned_peer = standing_assigned;
            effective_peer = standing_effective;
            return;
        }
    }
    relabel_follow(live ? p_peer : Ref<MultiplayerPeer>());
    session_push_desired_role();
    connect_core.on_peer_assigned(p_peer);
    session_peer_assigned(live, live ? p_peer->get_unique_id() : 0);
}

RID NetwMultiplayer::endpoint_add(
    const RID &p_transport,
    const String &p_address,
    const String &p_display_name
) {
    if (transport_held(p_transport) == nullptr) {
        return RID();
    }
    return connect_core.target_add(p_transport, p_address, p_display_name);
}

RID NetwMultiplayer::endpoint_invite(
    const RID &p_transport,
    const String &p_address,
    const String &p_from_name
) {
    return connect_core.invite(p_transport, p_address, p_from_name);
}

void NetwMultiplayer::endpoint_remove(const RID &p_endpoint) {
    if (connect_core.list().holds(p_endpoint)) {
        connect_core.target_remove(p_endpoint);
    }
}

RID NetwMultiplayer::endpoint_find(
    const RID &p_transport,
    const String &p_address
) {
    return connect_core.target_find(p_transport, p_address);
}

TypedArray<RID> NetwMultiplayer::endpoint_list() {
    return connect_core.list().list();
}

RID NetwMultiplayer::endpoint_get_current() {
    return connect_core.current_target();
}

Ref<NetwEndpoint> NetwMultiplayer::endpoint_view(const RID &p_endpoint) {
    const session::EndpointView *held = views.endpoint_views.getptr(p_endpoint);
    if (held != nullptr) {
        return held->view;
    }
    const connect::TargetRow *row = connect_core.list().row(p_endpoint);
    if (row == nullptr || !connect_core.list().holds(p_endpoint)) {
        return Ref<NetwEndpoint>();
    }
    const StringName peer_class = transport_class_of(row->transport);
    if (peer_class.is_empty()) {
        return Ref<NetwEndpoint>();
    }
    Ref<NetwEndpoint> minted;
    minted.instantiate();
    minted->session_id = gd::instance_id(this);
    minted->endpoint = p_endpoint;
    minted->peer_class = peer_class;
    minted->address = row->address;
    views.endpoint_views[p_endpoint].view = minted;
    return minted;
}

void NetwMultiplayer::endpoint_view_drop(const RID &p_endpoint) {
    session::EndpointView *held = views.endpoint_views.getptr(p_endpoint);
    if (held == nullptr) {
        return;
    }
    if (held->pins > 0) {
        held->dropped = true;
        return;
    }
    views.endpoint_views.erase(p_endpoint);
}

Variant NetwMultiplayer::endpoint_get_param(
    const RID &p_endpoint,
    EndpointParam p_param
) {
    const connect::TargetRow *held = connect_core.list().row(p_endpoint);
    if (held == nullptr || !connect_core.list().holds(p_endpoint)) {
        return Variant();
    }
    switch (p_param) {
        case ENDPOINT_PARAM_TRANSPORT:
            return held->transport;
        case ENDPOINT_PARAM_ADDRESS:
            return held->address;
        case ENDPOINT_PARAM_DISPLAY_NAME:
            return held->display_name;
        case ENDPOINT_PARAM_SOURCE: {
            LobbyDirectory *found
                = Object::cast_to<LobbyDirectory>(gd::object_of(held->source));
            return found == nullptr ? String() : found->get_display_name();
        }
        case ENDPOINT_PARAM_INVITED_BY:
            return held->invited_by;
        default:
            return Variant();
    }
}

Variant NetwMultiplayer::endpoint_get_state(
    const RID &p_endpoint,
    EndpointState p_state
) {
    const connect::TargetRow *held = connect_core.list().row(p_endpoint);
    if (held == nullptr || !connect_core.list().holds(p_endpoint)) {
        return Variant();
    }
    switch (p_state) {
        case ENDPOINT_STATE_FLAGS: {
            int64_t flags = 0;
            if (connect_core.list().is_removable(p_endpoint)) {
                flags |= ENDPOINT_FLAG_CALLER;
            }
            if (connect_core.target_is_available(p_endpoint)) {
                flags |= ENDPOINT_FLAG_AVAILABLE;
            }
            if (held->observed) {
                flags |= ENDPOINT_FLAG_OBSERVED;
            }
            if (held->invite_pending) {
                flags |= ENDPOINT_FLAG_INVITED;
            }
            return flags;
        }
        case ENDPOINT_STATE_STATUS:
            return held->observed ? int64_t(held->status) : int64_t(FAILED);
        case ENDPOINT_STATE_INFO:
            return held->info;
        default:
            return Variant();
    }
}

void NetwMultiplayer::endpoint_probe(const RID &p_endpoint) {
    if (connect_core.list().holds(p_endpoint)) {
        connect_core.probe_target(p_endpoint);
    }
}

void NetwMultiplayer::endpoint_set_browsing(bool p_enabled) {
    connect_core.set_browsing(p_enabled);
}

bool NetwMultiplayer::endpoint_is_browsing() const {
    return connect_core.is_browsing();
}

void NetwMultiplayer::endpoint_set_probe_interval(double p_seconds) {
    connect_core.set_probe_interval(p_seconds);
}

double NetwMultiplayer::endpoint_get_probe_interval() const {
    return connect_core.get_probe_interval();
}

const connect::TransportSlot *NetwMultiplayer::transport_held(
    const RID &p_transport
) const {
    const connect::TransportSlot *slot = connect::transport_slot(p_transport);
    if (slot == nullptr) {
        return nullptr;
    }
    if (slot->session != ObjectID() && slot->session != gd::instance_id(this)) {
        return nullptr;
    }
    return slot;
}

StringName NetwMultiplayer::transport_class_of(const RID &p_transport) const {
    const connect::TransportSlot *slot = transport_held(p_transport);
    return slot != nullptr ? slot->peer_class : StringName();
}

RID NetwMultiplayer::transport_create_peer(
    const RID &p_transport,
    TransportMode p_mode,
    const String &p_address,
    const Dictionary &p_settings,
    const Callable &p_completed
) {
    return connect_core.create_peer(
        p_transport,
        int(p_mode),
        p_address,
        p_settings,
        p_completed
    );
}

void NetwMultiplayer::transport_cancel_peer_creation(const RID &p_ticket) {
    connect_core.cancel_peer_creation(p_ticket);
}

Ref<NetwPromise> NetwMultiplayer::endpoint_join(
    const RID &p_endpoint,
    const Dictionary &p_settings
) {
    return connect_core.join_target(p_endpoint, p_settings);
}

Ref<NetwPromise> NetwMultiplayer::transport_host(
    const RID &p_transport,
    const Dictionary &p_settings
) {
    const Variant named = p_settings.get("role", Variant());
    const bool serves = named.get_type() == Variant::INT
        && (int64_t(named) == ROLE_DEDICATED_SERVER
            || int64_t(named) == ROLE_LISTEN_SERVER);
    if (named.get_type() != Variant::NIL && !serves) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwConnection.host}",
             vformat("role %s", String(named)),
             "{NetwSession.ROLE_DEDICATED_SERVER} or "
             "{NetwSession.ROLE_LISTEN_SERVER}"}
        );
    }
    Dictionary passed = p_settings.duplicate();
    passed.erase("role");
    const Ref<NetwPromise> outcome = connect_core.host(p_transport, passed);
    if (outcome->get_is_failed()) {
        return outcome;
    }
    hosted_role.reset();
    if (serves) {
        hosted_role = Role(int64_t(named));
    }
    return outcome;
}

void NetwMultiplayer::transport_cancel_attempt() {
    connect_core.cancel_attempt();
}

Error NetwMultiplayer::transport_set_defaults(
    const RID &p_transport,
    const Dictionary &p_settings
) {
    if (transport_held(p_transport) == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    return connect_core.set_defaults(p_transport, p_settings);
}

Dictionary NetwMultiplayer::transport_get_defaults(const RID &p_transport) {
    if (transport_held(p_transport) == nullptr) {
        return Dictionary();
    }
    return connect_core.defaults_of(p_transport);
}

void NetwMultiplayer::connection_attempt_moved() {
    session_publish_state();
}

void NetwMultiplayer::connection_announce_progressed(
    const String &p_message,
    double p_ratio
) {
    Announcement row;
    row.kind = AnnouncementKind::CONNECTION_PROGRESSED;
    row.reason = p_message;
    row.amount = p_ratio;
    announce(row);
}

bool NetwMultiplayer::connection_announce_failed(
    Error p_error,
    const String &p_reason
) {
    const Ref<NetwConnection> connection = views.connection_handle;
    const bool heard = connection.is_valid() && connection->hears_failed();
    Announcement row;
    row.kind = AnnouncementKind::CONNECTION_FAILED;
    row.code = int64_t(p_error);
    row.reason = p_reason;
    announce(row);
    return heard;
}

void NetwMultiplayer::connection_settle_attempt(
    const Ref<NetwPromise> &p_promise,
    Error p_error,
    const String &p_reason
) {
    if (p_error != OK) {
        hosted_role.reset();
    }
    Announcement row;
    row.kind = AnnouncementKind::ATTEMPT_SETTLED;
    row.promise = p_promise;
    row.code = int64_t(p_error);
    row.reason = p_reason;
    announce(row);
}

void NetwMultiplayer::connection_announce_join_address(
    const String &p_address
) {
    Announcement row;
    row.kind = AnnouncementKind::JOIN_ADDRESS_CHANGED;
    row.reason = p_address;
    announce(row);
}

void NetwMultiplayer::endpoint_announce(
    AnnouncementKind p_kind,
    const RID &p_endpoint
) {
    Announcement row;
    row.kind = p_kind;
    row.endpoint = p_endpoint;
    announce(row);
}

bool NetwMultiplayer::endpoint_pin(const RID &p_endpoint) {
    if (endpoint_view(p_endpoint).is_null()) {
        return false;
    }
    views.endpoint_views[p_endpoint].pins += 1;
    return true;
}

void NetwMultiplayer::endpoint_unpin(const RID &p_endpoint) {
    session::EndpointView *held = views.endpoint_views.getptr(p_endpoint);
    if (held == nullptr || held->pins == 0) {
        return;
    }
    held->pins -= 1;
    if (held->pins == 0 && held->dropped) {
        views.endpoint_views.erase(p_endpoint);
    }
}

RID NetwMultiplayer::transport_register(const Ref<Script> &p_type) {
    const Ref<Script> script
        = netw::script::model::declaring_script(p_type.ptr());
    if (script.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_BAD_SCRIPT,
            {"{NetwMultiplayer.transport_register}",
             "no script",
             "a script that extends {NetwTransport}"}
        );
        return RID();
    }
    return connect_core.register_transport(script);
}

Error NetwMultiplayer::transport_unregister(const RID &p_transport) {
    return connect_core.unregister_transport(p_transport);
}

RID NetwMultiplayer::transport_find_script(const Ref<Script> &p_type) const {
    return connect_core.registry().slot_of_script(
        netw::script::model::declaring_script(p_type.ptr())
    );
}

TypedArray<RID> NetwMultiplayer::transport_list() {
    TypedArray<RID> held = connect_core.registry().slots();
    Array named = connect_core.registry().peer_classes();
    const TypedArray<RID> stock = connect::TransportBook::shared().slots();
    const Array stock_named = connect::TransportBook::shared().peer_classes();
    for (int64_t at = 0; at < stock.size(); at++) {
        if (named.has(stock_named[at])) {
            continue;
        }
        named.push_back(stock_named[at]);
        held.push_back(stock[at]);
    }
    for (const KeyValue<uint64_t, RID> &row : directory_slots) {
        const connect::TransportSlot *slot = connect::transport_slot(row.value);
        if (slot == nullptr || named.has(slot->peer_class)) {
            continue;
        }
        named.push_back(slot->peer_class);
        held.push_back(row.value);
    }
    return held;
}

RID NetwMultiplayer::transport_find(const StringName &p_peer_class) {
    const RID registered = connect_core.registry().slot_of(p_peer_class);
    if (registered.is_valid()) {
        return registered;
    }
    const RID booked = connect::TransportBook::shared().slot_of(p_peer_class);
    if (booked.is_valid()) {
        return booked;
    }
    for (const KeyValue<uint64_t, RID> &row : directory_slots) {
        const connect::TransportSlot *slot = connect::transport_slot(row.value);
        if (slot != nullptr && slot->peer_class == p_peer_class) {
            return row.value;
        }
    }
    return RID();
}

Variant NetwMultiplayer::transport_get_param(
    const RID &p_transport,
    TransportParam p_param
) {
    const connect::TransportSlot *slot = transport_held(p_transport);
    if (slot == nullptr) {
        return Variant();
    }
    if (p_param == TRANSPORT_PARAM_PEER_CLASS) {
        return slot->peer_class;
    }
    connect::TransportFacts facts;
    if (!connect_core.transport_facts(*slot, facts)) {
        return Variant();
    }
    switch (p_param) {
        case TRANSPORT_PARAM_DISPLAY_NAME:
            return facts.display_name;
        case TRANSPORT_PARAM_ADDRESS_LABEL:
            return facts.address_label;
        case TRANSPORT_PARAM_ADDRESS_PLACEHOLDER:
            return facts.address_placeholder;
        case TRANSPORT_PARAM_ADDRESS_HELP:
            return facts.address_help;
        case TRANSPORT_PARAM_HOST_SETTINGS:
            return facts.host_settings;
        case TRANSPORT_PARAM_CLIENT_SETTINGS:
            return facts.client_settings;
        case TRANSPORT_PARAM_TIMEOUT_HINT:
            return facts.timeout_hint;
        case TRANSPORT_PARAM_CAPABILITIES: {
            int64_t flags = 0;
            flags |= facts.is_available ? TRANSPORT_AVAILABLE : 0;
            flags |= facts.can_host_here ? TRANSPORT_CAN_HOST : 0;
            flags |= facts.can_probe ? TRANSPORT_CAN_PROBE : 0;
            flags |= facts.can_browse ? TRANSPORT_CAN_BROWSE : 0;
            flags |= facts.accepts_empty_address
                ? TRANSPORT_ACCEPTS_EMPTY_ADDRESS
                : 0;
            return flags;
        }
        default:
            return Variant();
    }
}

Array NetwMultiplayer::session_get_join_schema() {
    Array result;
    const JoinPlan plan = session_resolve_join();
    if (!plan.available || !plan.handler.is_valid()) {
        return result;
    }
    const Callable handler = plan.handler;
    const Array types = plan.arg_types;

    PackedStringArray names;
    Object *target = handler.get_object();
    const Variant script_var
        = target != nullptr ? target->get_script() : Variant();
    const Ref<Script> script = script_var;
    if (script.is_valid() && !handler.is_custom()) {
        const Array methods = gd::script_method_list(script);
        for (int at = 0; at < methods.size(); ++at) {
            const Dictionary method = methods[at];
            if (StringName(method.get("name", StringName()))
                != handler.get_method()) {
                continue;
            }
            const Array args = method.get("args", Array());
            for (int arg_at = 1; arg_at < args.size(); ++arg_at) {
                const Dictionary arg = args[arg_at];
                names.push_back(String(arg.get("name", String())));
            }
            break;
        }
    }

    for (int at = 0; at < types.size(); ++at) {
        Dictionary entry;
        entry["name"] = at < names.size() && !names[at].is_empty()
            ? names[at]
            : String("arg") + String::num_int64(at);
        entry["type"] = types[at];
        entry["class_name"] = StringName();
        entry["default"] = Variant();
        result.push_back(entry);
    }
    return result;
}

connect::ProbeHooks NetwMultiplayer::discovery_probe_hooks() {
    connect::ProbeHooks made;
    made.authenticating
        = callable_mp(this, &NetwMultiplayer::discovery_probe_authenticating);
    made.auth_received
        = callable_mp(this, &NetwMultiplayer::discovery_probe_auth_received);
    made.connection_failed = callable_mp(
        this,
        &NetwMultiplayer::discovery_probe_connection_failed
    );
    made.authentication_failed = callable_mp(
        this,
        &NetwMultiplayer::discovery_probe_authentication_failed
    );
    return made;
}

void NetwMultiplayer::discovery_probe_authenticating(int64_t p_peer_id) {
    connect::ProbeClient *reached = connect_core.probe_client();
    if (reached != nullptr) {
        reached->on_authenticating(p_peer_id);
    }
}

void NetwMultiplayer::discovery_probe_auth_received(
    int64_t p_peer_id,
    const PackedByteArray &p_data
) {
    connect::ProbeClient *reached = connect_core.probe_client();
    if (reached != nullptr) {
        reached->on_auth_received(p_peer_id, p_data);
    }
}

void NetwMultiplayer::discovery_probe_connection_failed() {
    connect::ProbeClient *reached = connect_core.probe_client();
    if (reached != nullptr) {
        reached->on_connection_failed();
    }
}

void NetwMultiplayer::discovery_probe_authentication_failed(int64_t p_peer_id) {
    connect::ProbeClient *reached = connect_core.probe_client();
    if (reached != nullptr) {
        reached->on_authentication_failed(p_peer_id);
    }
}

void NetwMultiplayer::transport_report_progress(
    const RID &p_ticket,
    const String &p_message,
    double p_ratio
) {
    connect_core.on_progress(p_ticket, p_message, p_ratio);
}

void NetwMultiplayer::transport_complete(
    const RID &p_ticket,
    Error p_error,
    const Variant &p_result,
    const String &p_message
) {
    connect_core.complete(p_ticket, p_error, p_result, p_message);
}

void NetwMultiplayer::transport_publish_targets(
    const RID &p_transport,
    const TypedArray<Dictionary> &p_targets
) {
    const connect::TransportSlot *slot = transport_held(p_transport);
    if (slot != nullptr) {
        connect_core.publish_targets(p_transport, slot->directory, p_targets);
    }
}

String NetwMultiplayer::transport_get_join_address() {
    return connect_core.join_address();
}

Dictionary NetwMultiplayer::transport_get_peer_diagnostics(int64_t p_peer_id) {
    return connect_core.diagnostics(p_peer_id);
}

Object *NetwMultiplayer::discovery_find_directory(
    const StringName &p_peer_class
) const {
    for (const session::ServiceRow &row : views.services) {
        Object *service = gd::instance_from_id(row.service);
        if (connect::DirectoryTransport::peer_class_of_directory(service)
            == p_peer_class) {
            return service;
        }
    }
    return nullptr;
}

LocalVector<ObjectID> NetwMultiplayer::discovery_directories() const {
    LocalVector<ObjectID> found;
    for (const session::ServiceRow &row : views.services) {
        Object *service = gd::instance_from_id(row.service);
        if (connect::DirectoryTransport::is_directory(service)) {
            found.push_back(row.service);
        }
    }
    return found;
}

void NetwMultiplayer::discovery_watch_directory(Object *p_directory) {
    if (!connect::DirectoryTransport::is_directory(p_directory)) {
        return;
    }
    const int64_t held = int64_t(uint64_t(gd::instance_id(p_directory)));
    const StringName ready(connect::DirectoryTransport::SIG_PEER_READY);
    const StringName failed(connect::DirectoryTransport::SIG_FAILED);
    const StringName listed(connect::DirectoryTransport::SIG_LIST_PUBLISHED);
    const Callable on_ready
        = callable_mp(this, &NetwMultiplayer::discovery_directory_delivered)
              .bind(held);
    const Callable on_failed
        = callable_mp(this, &NetwMultiplayer::discovery_directory_failed)
              .bind(held);
    const Callable on_listed
        = callable_mp(this, &NetwMultiplayer::discovery_directory_listed)
              .bind(held);
    if (p_directory->has_signal(ready)
        && !p_directory->is_connected(ready, on_ready)) {
        p_directory->connect(ready, on_ready);
    }
    if (p_directory->has_signal(failed)
        && !p_directory->is_connected(failed, on_failed)) {
        p_directory->connect(failed, on_failed);
    }
    if (p_directory->has_signal(listed)
        && !p_directory->is_connected(listed, on_listed)) {
        p_directory->connect(listed, on_listed);
    }
    const StringName named
        = connect::DirectoryTransport::peer_class_of_directory(p_directory);
    if (named != StringName() && !directory_slots.has(uint64_t(held))) {
        directory_slots.insert(
            uint64_t(held),
            connect::mint_directory_slot(
                named,
                gd::instance_id(this),
                gd::instance_id(p_directory)
            )
        );
    }
    Object::cast_to<LobbyDirectory>(p_directory)->watched_by(this);
    connect_core.watch_directory(p_directory);
}

void NetwMultiplayer::discovery_unwatch_directory(Object *p_directory) {
    if (p_directory == nullptr) {
        return;
    }
    LobbyDirectory *directory = Object::cast_to<LobbyDirectory>(p_directory);
    if (directory != nullptr) {
        directory->unwatched_by(this);
    }
    connect_core.forget_directory(gd::instance_id(p_directory));
    const HashMap<uint64_t, RID>::Iterator held
        = directory_slots.find(uint64_t(gd::instance_id(p_directory)));
    if (held == directory_slots.end()) {
        return;
    }
    connect::free_transport_slot(held->value);
    directory_slots.remove(held);
}

void NetwMultiplayer::discovery_directory_delivered(
    const Ref<MultiplayerPeer> &p_peer,
    int64_t p_directory
) {
    transport_complete(
        connect_core.directory_ticket(p_directory),
        OK,
        p_peer,
        String()
    );
}

void NetwMultiplayer::discovery_directory_failed(
    int64_t p_error,
    const String &p_message,
    int64_t p_directory
) {
    transport_complete(
        connect_core.directory_ticket(p_directory),
        Error(p_error),
        Variant(),
        p_message
    );
}

void NetwMultiplayer::discovery_directory_listed(
    const PackedStringArray &p_addresses,
    const PackedStringArray &p_names,
    const Array &p_infos,
    int64_t p_directory
) {
    const StringName named = connect_core.directory_peer_class(p_directory);
    if (named.is_empty()) {
        return;
    }
    connect_core.publish_targets(
        transport_find(named),
        ObjectID(uint64_t(p_directory)),
        connect::targets_of(p_addresses, p_names, p_infos)
    );
}

Ref<NetwConnection> NetwMultiplayer::get_connection() {
    if (views.connection_handle.is_null()) {
        views.connection_handle.instantiate();
        views.connection_handle->bind_session(this);
    }
    return views.connection_handle;
}

Ref<NetwSession> NetwMultiplayer::get_session() {
    if (views.session_handle.is_null()) {
        views.session_handle.instantiate();
        views.session_handle->bind_session(this);
    }
    return views.session_handle;
}

Ref<NetwClock> NetwMultiplayer::get_clock() {
    if (views.clock_handle.is_null()) {
        views.clock_handle.instantiate();
        views.clock_handle->bind_session(this);
    }
    return views.clock_handle;
}

Ref<MultiplayerPeer> NetwMultiplayer::NETW_API_VIRTUAL(get_multiplayer_peer)() {
    return assigned_peer;
}

int32_t NetwMultiplayer::NETW_API_VIRTUAL(get_unique_id)() NETW_API_CONST {
    if (!assigned_peer.is_valid()
        || assigned_peer->get_connection_status()
            == MultiplayerPeer::CONNECTION_DISCONNECTED) {
        return 1;
    }
    return assigned_peer->get_unique_id();
}

PackedInt32Array NetwMultiplayer::
    NETW_API_VIRTUAL(get_peer_ids)() NETW_API_CONST {
    if (assigned_peer.is_valid() && inner.is_valid()) {
        return gd::api_peer_ids(inner);
    }
    return peer_ids;
}

int32_t NetwMultiplayer::
    NETW_API_VIRTUAL(get_remote_sender_id)() NETW_API_CONST {
    if (dispatching_sender != 0) {
        return int32_t(dispatching_sender);
    }
    if (inner.is_null()) {
        return 0;
    }
    return int32_t(inner->get_remote_sender_id());
}

Error NetwMultiplayer::NETW_API_VIRTUAL(object_configuration_add)(
    Object *p_object,
    NETW_API_CONFIG_ARG p_configuration
) {
    Object *declared = p_configuration.get_validated_object();
    const Error refused = configuration_door_refuses_config(this, declared);
    if (refused != OK) {
        return refused;
    }
    ReplicationCore *plane = (get_replication_plane());
    Node *node = Object::cast_to<Node>(p_object);
    MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(declared);
    if (plane != nullptr && spawner != nullptr) {
        return plane->get_spawner_compat()->consume(node, spawner);
    }
    MultiplayerSynchronizer *sync
        = Object::cast_to<MultiplayerSynchronizer>(declared);
    if (plane != nullptr && node != nullptr && sync != nullptr) {
        return plane->get_sync_compat()->consume(node, sync);
    }
    return inner->object_configuration_add(p_object, p_configuration);
}

Error NetwMultiplayer::NETW_API_VIRTUAL(object_configuration_remove)(
    Object *p_object,
    NETW_API_CONFIG_ARG p_configuration
) {
    Object *declared = p_configuration.get_validated_object();
    const Error refused = configuration_door_refuses_config(this, declared);
    if (refused != OK) {
        return refused;
    }
    ReplicationCore *plane = (get_replication_plane());
    Node *node = Object::cast_to<Node>(p_object);
    MultiplayerSpawner *spawner = Object::cast_to<MultiplayerSpawner>(declared);
    if (plane != nullptr && spawner != nullptr) {
        return plane->get_spawner_compat()->consume_remove(node, spawner);
    }
    MultiplayerSynchronizer *sync
        = Object::cast_to<MultiplayerSynchronizer>(declared);
    if (plane != nullptr && node != nullptr && sync != nullptr) {
        return plane->get_sync_compat()->consume_remove(node, sync);
    }
    const Error removed
        = inner->object_configuration_remove(p_object, p_configuration);
    if (p_object == nullptr && names_the_root_window(p_configuration)) {
        embed_dispose();
    }
    return removed;
}

void NetwMultiplayer::set_peer_ids(const PackedInt32Array &p_peer_ids) {
    peer_ids = p_peer_ids;
}

NetwLivenessCore *NetwMultiplayer::get_liveness_core() {
    return &liveness_core;
}

NetwSceneCore *NetwMultiplayer::get_scene_core() {
    return &scene_core;
}

bool NetwMultiplayer::scene_is_live(const RID &p_scene) const {
    return scene_core.is_live(p_scene);
}

JoinRoster &NetwMultiplayer::join_book() {
    return join_roster;
}

display::Book *NetwMultiplayer::get_display_book() {
    return &display_book;
}

NetwChannelBook *NetwMultiplayer::get_channel_book() {
    return &channel_book;
}

NetwLagCompCore *NetwMultiplayer::get_lagcomp_core() {
    return &lagcomp_core;
}

NetwPredictionEngine *NetwMultiplayer::get_prediction_engine() {
    return &prediction_engine;
}

void NetwMultiplayer::interest_flush_sink() {
    sink_verdict(interest_flush_now(), 0);
}

void NetwMultiplayer::set_interest_flush(const Callable &p_flush) {
    interest_flush = p_flush;
}

namespace {

uint64_t clock_wall_usec() {
    const godot::Time *reading = godot::Time::get_singleton();
    return reading ? reading->get_ticks_usec() : 0;
}

PackedByteArray clock_rate_frame(int p_tickrate) {
    session::ClockRate rate;
    rate.tickrate = uint64_t(MAX(p_tickrate, 0));
    return session::frame_write(rate);
}

} // namespace

void NetwMultiplayer::clock_set_mismatch_action(MismatchAction p_action) {
    world.clock.set_mismatch_action(Clock::Mismatch(p_action));
}

void NetwMultiplayer::clock_request_handshake() {
    NETW_ZONE_NC("NetwMultiplayer clock handshake", colors::CLOCK);
    NETW_TRACE(sys::CLOCK, "Clock handshake asks the server for its tickrate.");
    send_to(
        SERVER_PEER,
        0,
        wire::CHANNEL_CLOCK_HANDSHAKE,
        clock_rate_frame(clock_engine().get_tickrate()),
        true,
        0,
        String()
    );
}

void NetwMultiplayer::clock_owe_ping(bool p_forces) {
    world.clock.exchange.owe_ping(p_forces);
}

bool NetwMultiplayer::clock_ping_waits() const {
    return world.clock.exchange.ping_waits();
}

void NetwMultiplayer::clock_board(int64_t p_now_ms, bool p_upkeep_rides) {
    NetwCarrierBuffers &carrier = world.wire.carrier;
    const auto batch_leaves = [&](int64_t p_peer) {
        return !p_upkeep_rides
            || (carrier.pending(p_peer, false) > 0
                && !carrier.rides(p_peer, p_now_ms));
    };
    const PackedInt64Array asking = world.clock.exchange.peers_asking();
    for (int at = 0; at < asking.size(); ++at) {
        const bool leaves = batch_leaves(asking[at]);
        if (!world.clock.exchange.pong_boards(asking[at], p_now_ms, leaves)) {
            continue;
        }
        session::ClockPong pong
            = world.clock.exchange.take_pong(asking[at], clock_wall_usec());
        pong.tick = uint64_t(clock_engine().get_tick()) & 0xFFFFFFFF;
        pong.phase = uint64_t(clock_engine().tick_phase() * 255.0) & 0xFF;
        send_to(
            asking[at],
            0,
            wire::CHANNEL_CLOCK_PONG,
            session::frame_write(pong),
            false,
            0,
            String(),
            wire::Delivery::TICK
        );
    }
    const bool batch_waits = carrier.pending(SERVER_PEER, false) > 0;
    const bool ping_leaves = !p_upkeep_rides || batch_waits;
    if (world.clock.exchange.ping_boards(p_now_ms, ping_leaves)) {
        clock_board_ping();
    }
}

void NetwMultiplayer::clock_board_ping() {
    NETW_ZONE_NC("Clock ping", colors::CLOCK);
    send_to(
        SERVER_PEER,
        0,
        wire::CHANNEL_CLOCK_PING,
        session::frame_write(world.clock.exchange.take_ping(clock_wall_usec())),
        false,
        0,
        String(),
        wire::Delivery::TICK
    );
}

void NetwMultiplayer::clock_receive_handshake(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (!is_host()) {
        return;
    }
    session::ClockRate asked;
    if (!session::frame_read(p_payload, asked)) {
        NETW_TRACE(
            sys::CLOCK,
            "A clock handshake that did not decode fully was refused."
        );
        return;
    }
    send_to(
        p_sender,
        0,
        wire::CHANNEL_CLOCK_HANDSHAKE_REPLY,
        clock_rate_frame(clock_engine().get_tickrate()),
        true,
        0,
        String()
    );
}

void NetwMultiplayer::clock_receive_handshake_reply(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (p_sender != SERVER_PEER) {
        return;
    }
    session::ClockRate answered;
    if (!session::frame_read(p_payload, answered)) {
        fault::raise(
            this,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), SERVER_PEER}
        );
        return;
    }
    const int server_tickrate = int(answered.tickrate);
    switch (clock_engine().tickrate_verdict(server_tickrate)) {
        case Clock::RATE_AGREES:
            break;
        case Clock::RATE_DISCONNECTS:
            if (effective_peer.is_valid()) {
                effective_peer->close();
            }
            break;
        case Clock::RATE_SIGNALS: {
            Announcement row;
            row.kind = AnnouncementKind::TICKRATE_MISMATCH;
            row.server_tickrate = server_tickrate;
            announce(row);
        } break;
        case Clock::RATE_WARNS:
            fault::raise(
                this,
                NETW_FAULT_PEER_MISMATCH,
                {String("the clock"),
                 String(),
                 String("NetwClockConfig.tickrate"),
                 SERVER_PEER}
            );
            break;
    }
    clock_owe_ping(true);
}

void NetwMultiplayer::clock_receive_ping(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    session::ClockPing ping;
    if (!is_host() || !session::frame_read(p_payload, ping)) {
        return;
    }
    world.clock.exchange.hear_ping(p_sender, ping, clock_wall_usec());
}

void NetwMultiplayer::clock_receive_pong(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    session::ClockPong pong;
    if (p_sender != SERVER_PEER || !session::frame_read(p_payload, pong)) {
        return;
    }
    clock_ingest_pong(pong, clock_wall_usec(), !is_host());
}

void NetwMultiplayer::set_display_spec_override(
    const LocalVector<display::SpecRow> &p_rows
) {
    display_hooks.spec_override.clear();
    for (uint32_t at = 0; at < p_rows.size(); ++at) {
        display_hooks.spec_override.push_back(p_rows[at]);
    }
    display_hooks.spec_override_armed = true;
    display_hooks.spec_asks = 0;
}

int NetwMultiplayer::display_spec_asks() const {
    return display_hooks.spec_asks;
}

int NetwMultiplayer::declared_quantum() const {
    return MAX(
        1,
        int(Math::round(clock_engine().physics_factor(clock_reading())))
    );
}

NetwMultiplayer::SessionState NetwMultiplayer::
    session_get_stored_state() const {
    return SessionState(session_core.get_state());
}

NetwMultiplayer::SessionState NetwMultiplayer::session_get_state() const {
    const SessionState own = session_get_stored_state();
    if (own == SESSION_STATE_OFFLINE
        && connect_core.attempt_state() == connect::ATTEMPT_CREATING) {
        return SESSION_STATE_CREATING;
    }
    return own;
}

NetwMultiplayer::Role NetwMultiplayer::session_get_role() const {
    return Role(session_core.get_role());
}

bool NetwMultiplayer::session_is_online() const {
    return session_get_stored_state() == SESSION_STATE_ONLINE;
}

bool NetwMultiplayer::is_host() const {
    return session_core.holds_server_authority();
}

void NetwMultiplayer::structure_halt_enter() {
    structure_halted = true;
}

void NetwMultiplayer::structure_halt_leave() {
    structure_halted = false;
    halt_release();
}

bool NetwMultiplayer::is_structure_halted() const {
    return structure_halted;
}

void NetwMultiplayer::halt_park_join(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    for (HaltJoin &held : halt_joins) {
        if (held.sender == p_sender) {
            held.payload = p_payload;
            return;
        }
    }
    HaltJoin parked;
    parked.payload = p_payload;
    parked.sender = p_sender;
    halt_joins.push_back(parked);
}

void NetwMultiplayer::halt_forget_peer(int64_t p_peer) {
    for (int64_t at = int64_t(halt_joins.size()) - 1; at >= 0; --at) {
        if (halt_joins[uint32_t(at)].sender == p_peer) {
            halt_joins.remove_at(uint32_t(at));
        }
    }
    halt_leases.erase(p_peer);
    if (resync_awaited.erase(p_peer)) {
        transfer_settle_resync();
    }
}

void NetwMultiplayer::halt_release() {
    if (!is_host()) {
        halt_joins.clear();
        halt_leases.clear();
        return;
    }
    while (!halt_leases.is_empty() && !structure_halted) {
        const int64_t peer = halt_leases[0];
        halt_leases.remove_at(0);
        liveness_grant_lease(peer);
    }
    while (!halt_joins.is_empty() && !structure_halted) {
        const HaltJoin parked = halt_joins[0];
        halt_joins.remove_at(0);
        session_accept_join(parked.payload, parked.sender);
    }
}

int64_t NetwMultiplayer::structure_ops_outstanding() const {
    int64_t outstanding = 0;
    world.entities.each([&outstanding](uint64_t, const EntityRow &p_slot) {
        if (p_slot.node == 0) {
            return;
        }
        Node *owner
            = Object::cast_to<Node>(gd::object_of(ObjectID(p_slot.node)));
        const Ref<NetwEntity> entity = NetwEntity::of(owner);
        if (entity.is_valid()) {
            outstanding += int64_t(entity->structure_ops_outstanding());
        }
    });
    return outstanding;
}

int64_t NetwMultiplayer::route_floor() const {
    return int64_t(liveness_core.route_floor());
}

bool NetwMultiplayer::has_server_role() const {
    return session_core.is_server_role();
}

bool NetwMultiplayer::session_is_local_client() const {
    const Role current = session_get_role();
    return current == ROLE_CLIENT || current == ROLE_LISTEN_SERVER;
}

void NetwMultiplayer::auth_relay_authenticating(int64_t p_peer) {
    if (p_peer == SERVER_PEER) {
        connect_core.on_server_answered();
    }
    emit_signal(SIG_PEER_AUTHENTICATING, p_peer);
}

double NetwMultiplayer::poll_delta(int64_t p_now_usec) {
    const int64_t previous = last_poll_usec;
    if (previous == p_now_usec) {
        return 0.0;
    }
    last_poll_usec = p_now_usec;
    return previous <= 0 ? 0.0 : double(p_now_usec - previous) / 1000000.0;
}

double NetwMultiplayer::display_delta() {
    const int64_t frame
        = int64_t(Engine::get_singleton()->get_process_frames());
    const int64_t previous = last_display_frame;
    if (previous == frame) {
        return 0.0;
    }
    last_display_frame = frame;
    const Node *root = gd::scene_root();
    return previous < 0 || root == nullptr ? 0.0
                                           : root->get_process_delta_time();
}

int64_t NetwMultiplayer::get_frame_counter() const {
    return frame_counter;
}

void NetwMultiplayer::clock_apply_config(const Ref<NetwClockConfig> &p_config) {
    if (p_config.is_null()) {
        return;
    }
    Clock &clock = clock_engine();
    clock.set_tickrate(int(p_config->get_tickrate()));
    const int64_t ticks = p_config->get_tickrate();
    const int64_t sends = p_config->get_send_rate();
    const int64_t widest = predict::SEND_INTERVAL_MAX;
    const int64_t least = (ticks + widest - 1) / widest;
    const bool accepted
        = sends >= least && sends <= ticks && ticks % sends == 0;
    if (sends > 0 && !accepted) {
        int64_t below = MIN(sends, ticks);
        while (below >= least && ticks % below != 0) {
            --below;
        }
        int64_t above = CLAMP(sends, least, ticks);
        while (ticks % above != 0) {
            ++above;
        }
        String asked = vformat(
            "a rate that divides %d ticks per second, such as %d",
            ticks,
            below
        );
        if (below < least) {
            asked = vformat(
                "a rate that divides %d ticks per second and sends at least "
                "every %d ticks, such as %d",
                ticks,
                widest,
                above
            );
        } else if (above > below) {
            asked += vformat(" or %d", above);
        }
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {"{NetwClockConfig.sends_per_second}",
             String::num_int64(sends),
             asked}
        );
    }
    clock.set_send_interval(accepted ? int(ticks / sends) : 1);
    clock.set_max_ticks_per_frame(int(p_config->get_max_ticks_per_frame()));
    clock.set_stall_threshold(p_config->get_stall_threshold());
    clock.set_use_physics_interpolation(
        p_config->get_use_physics_interpolation()
    );
    clock.set_sync_mode(Clock::SyncMode(p_config->get_sync_mode()));
    clock.set_panic_snap_threshold(int(p_config->get_panic_snap_threshold()));
    clock.set_stretch_nudge_factor(p_config->get_stretch_nudge_factor());
    clock.set_display_offset(int(p_config->get_display_offset()));
    clock.set_jitter_multiplier(p_config->get_jitter_multiplier());
    clock.set_jitter_window(int(p_config->get_jitter_window()));
    clock.set_jitter_stability_threshold(
        p_config->get_jitter_stability_threshold()
    );
    clock.set_enable_drift_logging(p_config->get_enable_drift_logging());
}

void NetwMultiplayer::clock_sweep_effects(double, int64_t p_tick) {
    lagcomp_effect_sweep(p_tick);
}

bool NetwMultiplayer::_get(const StringName &p_name, Variant &r_ret) const {
    if (p_name != StringName("_native_core")) {
        return false;
    }
    r_ret = Variant(const_cast<NetwMultiplayer *>(this));
    return true;
}

void NetwMultiplayer::session_set_inner(const Ref<SceneMultiplayer> &p_inner) {
    if (inner == p_inner) {
        return;
    }
    if (inner.is_valid()) {
        disconnect_once(
            Signal(inner.ptr(), SIG_CONNECTED_TO_SERVER),
            callable_mp(this, &NetwMultiplayer::session_transport_connected)
        );
        disconnect_once(
            Signal(inner.ptr(), SIG_CONNECTION_FAILED),
            callable_mp(this, &NetwMultiplayer::session_transport_failed)
        );
        disconnect_once(
            Signal(inner.ptr(), SIG_SERVER_DISCONNECTED),
            callable_mp(this, &NetwMultiplayer::session_transport_dropped)
        );
        disconnect_once(
            Signal(inner.ptr(), SIG_PEER_PACKET),
            callable_mp(this, &NetwMultiplayer::session_on_inner_packet)
        );
        disconnect_once(
            Signal(inner.ptr(), StringName("peer_connected")),
            callable_mp(this, &NetwMultiplayer::session_relay_peer_connected)
        );
        disconnect_once(
            Signal(inner.ptr(), StringName("peer_disconnected")),
            callable_mp(this, &NetwMultiplayer::session_relay_peer_disconnected)
        );
        stop_relay_from(inner.ptr(), SIG_CONNECTED_TO_SERVER, 0);
        stop_relay_from(inner.ptr(), SIG_CONNECTION_FAILED, 0);
        stop_relay_from(inner.ptr(), SIG_SERVER_DISCONNECTED, 0);
        auth_disarm();
        inner->disconnect(
            SIG_PEER_AUTHENTICATING,
            callable_mp(this, &NetwMultiplayer::auth_relay_authenticating)
        );
        inner->disconnect(
            SIG_PEER_AUTHENTICATING,
            callable_mp(this, &NetwMultiplayer::auth_send_hello)
        );
        inner->disconnect(
            SIG_PEER_AUTHENTICATION_FAILED,
            callable_mp(this, &NetwMultiplayer::relay_auth_failed)
        );
    }
    inner = p_inner;
    if (inner.is_null()) {
        return;
    }
    connect_once(
        Signal(inner.ptr(), SIG_CONNECTED_TO_SERVER),
        callable_mp(this, &NetwMultiplayer::session_transport_connected)
    );
    connect_once(
        Signal(inner.ptr(), SIG_CONNECTION_FAILED),
        callable_mp(this, &NetwMultiplayer::session_transport_failed)
    );
    connect_once(
        Signal(inner.ptr(), SIG_SERVER_DISCONNECTED),
        callable_mp(this, &NetwMultiplayer::session_transport_dropped)
    );
    connect_once(
        Signal(inner.ptr(), SIG_PEER_PACKET),
        callable_mp(this, &NetwMultiplayer::session_on_inner_packet)
    );
    connect_once(
        Signal(inner.ptr(), StringName("peer_connected")),
        callable_mp(this, &NetwMultiplayer::session_relay_peer_connected)
    );
    connect_once(
        Signal(inner.ptr(), StringName("peer_disconnected")),
        callable_mp(this, &NetwMultiplayer::session_relay_peer_disconnected)
    );
    relay_from(inner.ptr(), SIG_CONNECTED_TO_SERVER, 0);
    relay_from(inner.ptr(), SIG_CONNECTION_FAILED, 0);
    relay_from(inner.ptr(), SIG_SERVER_DISCONNECTED, 0);
    inner->connect(
        SIG_PEER_AUTHENTICATING,
        callable_mp(this, &NetwMultiplayer::auth_send_hello)
    );
    inner->connect(
        SIG_PEER_AUTHENTICATING,
        callable_mp(this, &NetwMultiplayer::auth_relay_authenticating)
    );
    inner->connect(
        SIG_PEER_AUTHENTICATION_FAILED,
        callable_mp(this, &NetwMultiplayer::relay_auth_failed)
    );
}

void NetwMultiplayer::session_set_root(const Callable &p_reader) {
    session_root_reader = p_reader;
}

int64_t NetwMultiplayer::linger_pumps(double p_seconds) const {
    const double rate = clock_engine().get_configured()
        ? double(clock_engine().get_tickrate())
        : POLL_PUMP_RATE;
    return Clock::pumps_for(p_seconds, rate);
}

bool NetwMultiplayer::action_gate_arm(
    int64_t p_route,
    const RID &p_entity,
    int64_t p_display_tick
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_null()) {
        return false;
    }
    const int64_t requester = entity->get_action_requester();
    if (requester != 0 && has_multiplayer_peer()
        && requester == get_unique_id()) {
        return false;
    }
    return action_gates.arm(
        p_route,
        entity->get_owner(),
        entity->get_action_spawn_tick(),
        p_display_tick
    );
}

PackedInt64Array NetwMultiplayer::action_gate_sweep(int64_t p_display_tick) {
    return action_gates.reveal_reached(p_display_tick);
}

void NetwMultiplayer::action_gate_drop(int64_t p_route) {
    action_gates.reveal(p_route);
}

void NetwMultiplayer::action_gate_clear() {
    action_gates.clear();
}

int64_t NetwMultiplayer::action_gate_count() const {
    return int64_t(action_gates.size());
}

Node *NetwMultiplayer::session_get_root() const {
    if (session_root_reader.is_valid()) {
        const Variant answered
            = game_call(session_root_reader, Array(), game_calls::ROOT_READER);
        Node *declared = Object::cast_to<Node>(gd::live_object(answered));
        if (declared != nullptr) {
            session_root_last_live = gd::instance_id(declared);
        }
        return declared;
    }
    if (inner.is_null()) {
        return nullptr;
    }
    Node *root = gd::scene_root();
    if (root == nullptr) {
        return nullptr;
    }
    return root->get_node_or_null(inner->get_root_path());
}

Ref<NetwSessionConfig> NetwMultiplayer::session_get_config() const {
    Ref<NetwSessionConfig> snapshot;
    snapshot.instantiate();
    snapshot->set_app_id(session_settings.app_id);
    snapshot->set_desired_role(Role(session_settings.desired_role));
    if (session_settings.link_conditions.is_valid()) {
        const Ref<NetwLinkConditions> link
            = session_settings.link_conditions->duplicate();
        snapshot->set_link_conditions(link);
    }
    if (session_settings.host_link_conditions.is_valid()) {
        const Ref<NetwLinkConditions> link
            = session_settings.host_link_conditions->duplicate();
        snapshot->set_host_link_conditions(link);
    }
    if (session_settings.server_info.is_valid()) {
        Ref<NetwServerInfo> info;
        info.instantiate();
        info->copy_values_from(**session_settings.server_info);
        snapshot->set_server_info(info);
    }
    snapshot->set_bandwidth_kilobytes_per_second(
        session_settings.bandwidth_kilobytes_per_second
    );
    snapshot->set_transfer_mode(session_settings.transfer_mode);
    snapshot->set_transfer_deadline_ticks(
        session_settings.transfer_deadline_ticks
    );
    snapshot->set_return_window_seconds(session_settings.return_window);
    return snapshot;
}

void NetwMultiplayer::session_set_server_info(
    const Ref<NetwServerInfo> &p_info
) {
    if (p_info.is_null()) {
        session_settings.server_info = Ref<NetwServerInfo>();
        return;
    }
    session_settings.server_info.instantiate();
    session_settings.server_info->copy_values_from(**p_info);
}

Ref<NetwServerInfo> NetwMultiplayer::session_get_server_info() const {
    Ref<NetwServerInfo> info;
    const Ref<NetwSessionConfig> config = session_get_config();
    const Ref<NetwServerInfo> declared
        = config.is_valid() ? config->get_server_info() : Ref<NetwServerInfo>();
    if (declared.is_valid()) {
        info = declared->duplicate();
    }
    if (info.is_null()) {
        info.instantiate();
    }
    info->set_is_local_listener(true);
    info->set_players(get_connected_players().size());
    info->set_app_id(session_get_app_id());
    return info;
}

StringName NetwMultiplayer::session_get_app_id() const {
    return session_settings.app_id;
}

Error NetwMultiplayer::session_initialize(
    const Ref<NetwSessionConfig> &p_from
) {
    const int slot = int(session_decl::KIND_SESSION_CONFIG)
        - int(session_decl::KIND_SESSION_CONFIG);
    if (config_consumption[slot].consumed) {
        fault::raise(
            this,
            NETW_FAULT_DECLARED_ONLINE,
            {"{Netw.configure_session}", String(), "the session settings"}
        );
        return ERR_ALREADY_IN_USE;
    }
    config_consumption[slot].consumed = true;
    if (p_from.is_valid()) {
        session_settings.app_id = p_from->get_app_id();
        session_settings.desired_role = p_from->get_desired_role();
        const Ref<NetwLinkConditions> link = p_from->get_link_conditions();
        if (link.is_valid()) {
            session_settings.link_conditions = link->duplicate();
        }
        const Ref<NetwLinkConditions> host_link
            = p_from->get_host_link_conditions();
        if (host_link.is_valid()) {
            session_settings.host_link_conditions = host_link->duplicate();
        }
        const Ref<NetwServerInfo> info = p_from->get_server_info();
        if (info.is_valid()) {
            session_settings.server_info.instantiate();
            session_settings.server_info->copy_values_from(**info);
        }
        session_settings.bandwidth_kilobytes_per_second
            = p_from->get_bandwidth_kilobytes_per_second();
        session_settings.transfer_mode = p_from->get_transfer_mode();
        session_settings.transfer_deadline_ticks
            = p_from->get_transfer_deadline_ticks();
        session_settings.return_window = p_from->get_return_window_seconds();
    }
    connect_core.bind_session(this);
    session_push_desired_role();
    session_apply_auth_config();
    return OK;
}

NetwMultiplayer::JoinPlan NetwMultiplayer::session_resolve_join() {
    JoinPlan plan;
    plan.available = true;
    const Callable override = session_join_override();
    if (override.is_valid()) {
        plan.handler = override;
        plan.quantizers = session_join_override_quantizers();
        plan.arg_types = join_arg_types(override);
        plan.declared = true;
        return plan;
    }
    const session_decl::Resolved declared
        = declaration_slots.resolve(this, session_decl::KIND_JOIN);
    if (declared.state == session_decl::READY) {
        const Ref<NetwJoinConfig> config = declared.payload;
        if (config.is_valid()) {
            plan.quantizers = config->get_quantizers();
            plan.arg_types = config->get_arg_types();
        }
        plan.handler = declared.callable;
        plan.declared = true;
        return plan;
    }
    if (declared.state != session_decl::ABSENT) {
        declaration_slots
            .report_unresolved(this, session_decl::KIND_JOIN, declared.state);
        plan.available = false;
        return plan;
    }
    return plan;
}

Array NetwMultiplayer::join_declared_arg_types(const Callable &p_handler) {
    Object *target = p_handler.get_object();
    const Ref<Script> script = netw::script::model::declaring_script(target);
    return script.is_valid()
        ? netw::script::model::get_method_arg_types(
              script,
              p_handler.get_method()
          )
        : gd::bound_method_arg_types(target, p_handler.get_method());
}

Array NetwMultiplayer::join_arg_types(const Callable &p_handler) {
    const Array types = join_declared_arg_types(p_handler);
    return types.is_empty() ? Array() : types.slice(1);
}

Ref<NetwJoinConfig> NetwMultiplayer::session_join_declare(
    const Callable &p_handler,
    const PackedInt32Array &p_arg_types
) {
    Node *scope = Object::cast_to<Node>(p_handler.get_object());
    if (!p_handler.is_valid() || scope == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{Netw.configure_join}"),
             String(),
             String("a method of a Node inside the session's branch")}
        );
        return refused<NetwJoinConfig>();
    }
    game_call_declared(p_handler, game_calls::JOIN_HANDLER);
    decl_sites::record_handler(p_handler);
    Array arg_types;
    for (int at = 0; at < p_arg_types.size(); ++at) {
        arg_types.push_back(p_arg_types[at]);
    }
    Ref<NetwJoinConfig> config;
    config.instantiate();
    config->set_context_script(netw::script::model::declaring_script(scope));
    config->set_context_name(
        p_handler.is_custom() ? StringName() : p_handler.get_method()
    );
    config->set_arg_types(arg_types);
    if (session_decl::declare(
            scope,
            session_decl::KIND_JOIN,
            p_handler,
            config,
            "configure_join"
        )
        != OK) {
        return refused<NetwJoinConfig>();
    }
    return config;
}

int64_t NetwMultiplayer::join_schema_hash(
    const Array &p_arg_types,
    const Array &p_quantizers
) {
    String signature;
    for (int at = 0; at < p_arg_types.size(); ++at) {
        signature += String::num_int64(int64_t(p_arg_types[at])) + ",";
    }
    signature += "|";
    for (int at = 0; at < p_quantizers.size(); ++at) {
        const Ref<NetwQuantize> quantizer = p_quantizers[at];
        signature
            += (quantizer.is_valid() ? quantizer->get_class() : String("_"))
            + ",";
    }
    return int64_t(int32_t(signature.hash()));
}

void NetwMultiplayer::session_encode_join_args(JoinRequest &r_request) {
    const JoinPlan plan = session_resolve_join();
    const Callable handler = plan.handler;
    const Array quantizers = plan.quantizers;
    r_request.arg_types = PackedByteArray();
    const int typed = MIN(r_request.arg_values.size(), 255);
    for (int at = 0; at < typed; ++at) {
        r_request.arg_types.push_back(
            uint8_t(r_request.arg_values[at].get_type())
        );
    }
    if (r_request.arg_values.is_empty() || !plan.available
        || !handler.is_valid()) {
        r_request.arg_bytes = PackedByteArray();
        r_request.schema_hash = 0;
        return;
    }
    wire::WriteStream stream;
    if (!call_args::values_write(
            stream,
            r_request.arg_values,
            quantizers,
            plan.arg_types,
            this
        )
        || !stream.align_verify()) {
        r_request.arg_bytes = PackedByteArray();
        r_request.schema_hash = 0;
        return;
    }
    r_request.arg_bytes = stream.to_bytes();
    r_request.schema_hash = join_schema_hash(plan.arg_types, quantizers);
}

bool NetwMultiplayer::session_decode_join_args(JoinRequest &r_request) {
    if (r_request.arg_bytes.is_empty()) {
        r_request.arg_values = Array();
        return true;
    }
    const JoinPlan plan = session_resolve_join();
    const Callable handler = plan.handler;
    const Array quantizers = plan.quantizers;
    if (!plan.available || !handler.is_valid()) {
        NETW_TRACE(
            sys::SESSION,
            "A join carried arguments, but no handler reads them."
        );
        return false;
    }
    if (r_request.schema_hash != join_schema_hash(plan.arg_types, quantizers)) {
        NETW_TRACE(
            sys::SESSION,
            "A join's arguments do not match this server's handler."
        );
        return false;
    }
    wire::ReadStream stream(r_request.arg_bytes);
    Array values;
    if (!call_args::values_read(stream, quantizers, plan.arg_types, values)
        || !stream.align_verify() || stream.bits_remaining() != 0) {
        NETW_TRACE(sys::SESSION, "A join's arguments did not decode fully.");
        return false;
    }
    r_request.arg_values = values;
    return true;
}

String NetwMultiplayer::session_join_args_refusal(
    const JoinRequest &p_request
) {
    const PackedByteArray &sent = p_request.arg_types;
    const JoinPlan plan = session_resolve_join();
    if (!plan.available || !plan.handler.is_valid()) {
        return vformat(
            "This server declares no join handler, and the join carried %d "
            "arguments. Declare the same Netw.configure_join on both peers.",
            sent.size()
        );
    }
    const Array schema = session_get_join_schema();
    if (sent.size() != schema.size()) {
        return vformat(
            "The join handler takes %d arguments, and the join carried %d. "
            "Pass %d arguments to Netw.prepare_join.",
            schema.size(),
            sent.size(),
            schema.size()
        );
    }
    for (int at = 0; at < schema.size(); ++at) {
        const Dictionary entry = schema[at];
        const Variant::Type expected = Variant::Type(int(entry["type"]));
        const Variant::Type carried = Variant::Type(int(sent[at]));
        if (expected == Variant::NIL || expected == carried) {
            continue;
        }
        const String wanted = Variant::get_type_name(expected);
        return vformat(
            "Join argument %d '%s' must be %s, and the join carried %s. Pass "
            "%s to Netw.prepare_join.",
            at,
            String(entry["name"]),
            wanted,
            Variant::get_type_name(carried),
            wanted
        );
    }
    return String(
        "The join arguments are encoded differently on this server. Declare "
        "the same Netw.configure_join on both peers."
    );
}

bool NetwMultiplayer::session_resolve_inbound_join(
    JoinRequest &r_request,
    int64_t p_sender,
    session::AcceptFrame &r_accepted
) {
    if (!session_decode_join_args(r_request)) {
        fault::raise(
            this,
            NETW_FAULT_JOIN_ARGS_MISMATCH,
            {String(), String(), String(), p_sender}
        );
        return false;
    }
    r_accepted.peer_id = p_sender;
    r_accepted.username = r_request.username;
    r_accepted.player_id = uint64_t(player_mint_id());
    return true;
}

void NetwMultiplayer::session_set_prepared_join(
    const StringName &p_username,
    const Array &p_args
) {
    prepared_join = connect::request_of(p_username, p_args);
}

const std::optional<JoinRequest> &NetwMultiplayer::
    session_prepared_join() const {
    return prepared_join;
}

void NetwMultiplayer::session_submit_join(
    const StringName &p_username,
    const Array &p_args
) {
    const std::optional<JoinRequest> request
        = connect::request_of(p_username, p_args);
    if (!request.has_value()) {
        NETW_TRACE(sys::SESSION, "A join with no username sends nothing.");
        return;
    }
    session_submit_request(*request);
}

void NetwMultiplayer::session_submit_request(const JoinRequest &p_request) {
    NETW_ZONE_NC("netw::session_submit_join", colors::SESSION);
    JoinRequest sending = p_request;
    const int64_t mine = get_unique_id();
    if (is_host() && session_requires_proof() && !session_has_proof(mine)) {
        LinkHandshake &local = link_handshakes[mine];
        local.local_join = p_request;
        if (local.proof.result.is_null()) {
            session_request_proof(
                mine,
                local.challenge.is_empty() ? session_mint_challenge()
                                           : local.challenge
            );
        }
        return;
    }
    const int64_t proven = is_host() ? mine : int64_t(SERVER_PEER);
    if (session_has_proof(proven)) {
        sending.proof_bytes = call_args::value_bytes(
            link_handshakes[proven].proofs,
            String("The admission proof"),
            this
        );
        if (sending.proof_bytes.size() > 16383) {
            session_fail_own_join(
                proven,
                ERR_UNCONFIGURED,
                "The admission proof exceeds 16383 bytes."
            );
            return;
        }
    }
    session_encode_join_args(sending);
    sending.app_tag = auth_app_tag;
    sending.wire_identity = SessionCore::compute_wire_identity();
    submitted_join = p_request;
    join_awaiting_admission = true;
    if (is_host()) {
        session_receive_join(sending.serialize(), get_unique_id());
        return;
    }
    const LinkHandshake *link = link_handshakes.getptr(SERVER_PEER);
    if (link != nullptr && link->join_sent && !link->completed) {
        auth_send_raw(SERVER_PEER, auth::encode_join(sending.serialize()));
        return;
    }
    send_to(
        SERVER_PEER,
        0,
        wire::CHANNEL_SESSION_JOIN,
        sending.serialize(),
        true,
        0,
        String()
    );
}

Ref<NetwPromise> NetwMultiplayer::session_leave() {
    return session_depart(false);
}

Ref<NetwPromise> NetwMultiplayer::session_step_away() {
    return session_depart(true);
}

Ref<NetwPromise> NetwMultiplayer::session_depart(bool p_returning) {
    NETW_ZONE_NC("netw::session_leave", colors::SESSION);
    if (session_get_stored_state() == SESSION_STATE_OFFLINE) {
        return NetwPromise::resolved(OK);
    }
    if (session_leaving.is_valid()) {
        return session_leaving;
    }
    session_leave_returning = p_returning;
    if (!is_host() || get_bindings()->enrolled().is_empty()) {
        return session_close_peer();
    }
    Ref<NetwPromise> left;
    left.instantiate();
    session_leaving = left;
    const Ref<NetwPromise> drained = persist_flush_all();
    drained->when_settled(
        callable_mp(this, &NetwMultiplayer::session_leave_drained)
            .bind(drained, left)
    );
    return left;
}

void NetwMultiplayer::session_leave_drained(
    const Ref<NetwPromise> &p_drained,
    const Ref<NetwPromise> &p_left
) {
    session_leaving.unref();
    const Error drained = Error(int(p_drained->get_result()));
    if (drained != OK) {
        fault::raise(
            this,
            NETW_FAULT_STORAGE_FAILED,
            {String("Saving to the database"),
             String("the leave"),
             String(
                 "its file or service before calling "
                 "{NetwSession.leave} again"
             )}
        );
        p_left->resolve(int(drained));
        return;
    }
    const Ref<NetwPromise> closed = session_close_peer();
    closed->when_settled(
        callable_mp(this, &NetwMultiplayer::session_leave_closed)
            .bind(closed, p_left)
    );
}

void NetwMultiplayer::session_leave_closed(
    const Ref<NetwPromise> &p_closed,
    const Ref<NetwPromise> &p_left
) {
    p_left->resolve(p_closed->get_result());
}

Ref<NetwPromise> NetwMultiplayer::session_close_peer() {
    if (session_get_stored_state() == SESSION_STATE_OFFLINE) {
        return NetwPromise::resolved(OK);
    }
    if (session_shutting_down.is_valid()) {
        return session_shutting_down;
    }
    if (session_departing.is_valid()) {
        return session_departing;
    }
    NETW_TRACE(sys::SESSION, "Leaving the session.");
    NETW_INFO(sys::SESSION, "Disconnecting player.");
    session_note_exit(enums::NetwSession::LOCAL_LEAVE, String());
    if (!is_host()) {
        const bool waits = session_sends_leave();
        if (waits) {
            session_send_leave(session_leave_returning);
        }
        if (session_get_stored_state() == SESSION_STATE_ONLINE) {
            session_transition(SESSION_STATE_DISCONNECTING);
        }
        return session_close_client(waits);
    }
    const bool shuts_down = inner.is_valid() && effective_peer.is_valid()
        && !gd::api_peer_ids(inner).is_empty();
    if (shuts_down) {
        player_note_leave(get_unique_id(), NetwPlayer::LEAVE_QUIT);
        session_notify_shutdown(String());
    }
    session_transition(SESSION_STATE_DISCONNECTING);
    if (!shuts_down) {
        return session_close_now();
    }
    effective_peer->set_refuse_new_connections(true);
    session_shutting_down.instantiate();
    session_shutdown_peer = effective_peer;
    session_shutdown_polls = 0;
    return session_shutting_down;
}

void NetwMultiplayer::session_poll_shutdown() {
    if (session_shutting_down.is_null()) {
        return;
    }
    session_shutdown_polls += 1;
    const bool same_peer = effective_peer.is_valid()
        && effective_peer == session_shutdown_peer && inner.is_valid();
    const PackedInt32Array clients
        = same_peer ? gd::api_peer_ids(inner) : PackedInt32Array();
    if (session_shutdown_polls == POLLS_BEFORE_TURNED_AWAY_PEER_DROPS) {
        for (int at = 0; at < clients.size(); ++at) {
            effective_peer->disconnect_peer(clients[at], false);
        }
    }
    const bool waiting = !clients.is_empty()
        || session_shutdown_polls <= POLLS_BEFORE_TURNED_AWAY_PEER_DROPS;
    if (same_peer && waiting
        && session_shutdown_polls < POLLS_BEFORE_SHUTDOWN_CLOSES) {
        return;
    }
    const Ref<NetwPromise> left = session_shutting_down;
    session_shutting_down.unref();
    session_shutdown_peer.unref();
    if (!same_peer) {
        session_settle_leave(left);
        return;
    }
    const Ref<NetwPromise> closed = session_close_now();
    closed->when_settled(
        callable_mp(this, &NetwMultiplayer::session_leave_closed)
            .bind(closed, left)
    );
}

void NetwMultiplayer::session_poll_server_shutdown() {
    if (!session_server_shut_down) {
        return;
    }
    session_server_shut_down = false;
    if (is_host() || session_get_stored_state() != SESSION_STATE_ONLINE) {
        return;
    }
    session_transition(SESSION_STATE_DISCONNECTING);
    session_close_client(false);
}

Ref<NetwPromise> NetwMultiplayer::session_close_now() {
    if (effective_peer.is_valid()) {
        effective_peer->close();
    }
    connect_core.on_session_left();

    Ref<NetwPromise> left;
    left.instantiate();
    const Callable settle
        = callable_mp(this, &NetwMultiplayer::session_settle_leave).bind(left);
    SceneTree *tree = gd::scene_tree();
    if (tree == nullptr) {
        session_settle_leave(left);
        return left;
    }
    const Ref<SceneTreeTimer> window
        = tree->create_timer(LEAVE_GIVE_UP_SECONDS);
    if (window.is_null()) {
        session_settle_leave(left);
        return left;
    }
    window->connect(StringName("timeout"), settle, Object::CONNECT_ONE_SHOT);
    connect(
        StringName("server_disconnected"),
        settle,
        Object::CONNECT_ONE_SHOT
    );
    return left;
}

void NetwMultiplayer::session_settle_leave(const Ref<NetwPromise> &p_left) {
    if (p_left->get_is_settled()) {
        return;
    }
    const Callable settle
        = callable_mp(this, &NetwMultiplayer::session_settle_leave)
              .bind(p_left);
    if (is_connected(StringName("server_disconnected"), settle)) {
        disconnect(StringName("server_disconnected"), settle);
    }
    session_transition(SESSION_STATE_OFFLINE);
    p_left->resolve(OK);
}

bool NetwMultiplayer::session_sends_leave() const {
    return !is_host() && inner.is_valid() && effective_peer.is_valid()
        && gd::api_peer_ids(inner).has(SERVER_PEER);
}

void NetwMultiplayer::session_send_leave(bool p_returning) {
    session::SessionLeave leaving;
    leaving.returning = p_returning;
    send_to(
        SERVER_PEER,
        0,
        wire::CHANNEL_SESSION_LEAVE,
        session::frame_write(leaving),
        true,
        0,
        String()
    );
}

Ref<NetwPromise> NetwMultiplayer::session_close_client(bool p_waits) {
    const Ref<MultiplayerPeer> link = effective_peer;
    Ref<NetwPromise> left;
    left.instantiate();
    SceneTree *tree = gd::scene_tree();
    Ref<SceneTreeTimer> give_up;
    if (p_waits && tree != nullptr) {
        give_up = tree->create_timer(LEAVE_GIVE_UP_SECONDS);
    }
    if (give_up.is_null()) {
        session_settle_client_leave(link, left);
        return left;
    }
    session_departing = left;
    const Callable settle
        = callable_mp(this, &NetwMultiplayer::session_settle_client_leave)
              .bind(link, left);
    give_up->connect(StringName("timeout"), settle, Object::CONNECT_ONE_SHOT);
    connect(
        StringName("server_disconnected"),
        settle,
        Object::CONNECT_ONE_SHOT
    );
    return left;
}

void NetwMultiplayer::session_settle_client_leave(
    const Ref<MultiplayerPeer> &p_link,
    const Ref<NetwPromise> &p_left
) {
    if (p_left->get_is_settled()) {
        return;
    }
    const Callable settle
        = callable_mp(this, &NetwMultiplayer::session_settle_client_leave)
              .bind(p_link, p_left);
    if (is_connected(StringName("server_disconnected"), settle)) {
        disconnect(StringName("server_disconnected"), settle);
    }
    if (session_departing == p_left) {
        session_departing.unref();
    }
    if (p_link.is_valid()) {
        gd::disconnect_server_later(p_link);
        p_link->close();
    }
    if (effective_peer == p_link) {
        connect_core.on_session_left();
    }
    session_settle_leave(p_left);
}

void NetwMultiplayer::session_receive_leave(
    int64_t p_sender,
    bool p_returning
) {
    player_note_leave(
        p_sender,
        p_returning ? NetwPlayer::LEAVE_STEPPED_AWAY : NetwPlayer::LEAVE_QUIT
    );
    session_turn_peer_away(p_sender, String());
}

void NetwMultiplayer::session_receive_player_left(
    const session::PlayerLeft &p_left
) {
    const int64_t player_id = int64_t(p_left.player_id);
    PlayerRow *found = players.getptr(player_id);
    if (found == nullptr || player_id == local_player_id || !found->admitted) {
        return;
    }
    found->leave_reason = NetwPlayer::LeaveReason(p_left.reason);
    player_release_id(player_id);
}

void NetwMultiplayer::session_leave_on_quit() {
    if (is_host()) {
        session_end_on_quit();
        return;
    }
    if (!session_sends_leave()) {
        return;
    }
    session_send_leave(false);
    gd::disconnect_server_later(effective_peer);
}

void NetwMultiplayer::session_end_on_quit() {
    if (inner.is_null() || effective_peer.is_null()
        || peer_relabels(effective_peer)) {
        return;
    }
    const PackedInt32Array clients = gd::api_peer_ids(inner);
    if (clients.is_empty()) {
        return;
    }
    if (session_shutting_down.is_null()
        && session_get_stored_state() == SESSION_STATE_ONLINE) {
        player_note_leave(get_unique_id(), NetwPlayer::LEAVE_QUIT);
        session_notify_shutdown(String());
    }
    gd::disconnect_clients_later(effective_peer, clients);
}

void NetwMultiplayer::session_on_window_close() {
    SceneTree *tree = gd::scene_tree();
    if (tree != nullptr && tree->is_auto_accept_quit()) {
        session_leave_on_quit();
    }
}

void NetwMultiplayer::session_watch_window_close(bool p_watch) {
    SceneTree *tree = gd::scene_tree();
    Window *root = tree != nullptr ? tree->get_root() : nullptr;
    if (root == nullptr) {
        return;
    }
    const Callable on_close
        = callable_mp(this, &NetwMultiplayer::session_on_window_close);
    const bool watching
        = root->is_connected(StringName("close_requested"), on_close);
    if (p_watch && !watching) {
        root->connect(StringName("close_requested"), on_close);
    } else if (!p_watch && watching) {
        root->disconnect(StringName("close_requested"), on_close);
    }
}

void NetwMultiplayer::session_clear_prepared_join() {
    prepared_join.reset();
    resubmit_join.reset();
    submitted_join.reset();
    join_awaiting_admission = false;
    const Callable on_authority = callable_mp(
        this,
        &NetwMultiplayer::session_resubmit_join_on_authority_peer
    );
    if (is_connected(StringName("peer_connected"), on_authority)) {
        disconnect(StringName("peer_connected"), on_authority);
    }
}

void NetwMultiplayer::session_submit_prepared_join() {
    if (!prepared_join.has_value()) {
        return;
    }
    const JoinRequest request = *prepared_join;
    prepared_join.reset();
    session_submit_request(request);
    if (is_host() || NETW_API_VIRTUAL(get_peer_ids)().has(SERVER_PEER)) {
        return;
    }
    NETW_TRACE(
        sys::SESSION,
        "The join waits for one resend, because the server is not seen yet."
    );
    resubmit_join = request;
    const Callable on_authority = callable_mp(
        this,
        &NetwMultiplayer::session_resubmit_join_on_authority_peer
    );
    if (!is_connected(StringName("peer_connected"), on_authority)) {
        connect(StringName("peer_connected"), on_authority);
    }
}

void NetwMultiplayer::session_resubmit_join_on_authority_peer(int64_t p_peer) {
    if (!resubmit_join.has_value() || p_peer != SERVER_PEER) {
        return;
    }
    const Callable on_authority = callable_mp(
        this,
        &NetwMultiplayer::session_resubmit_join_on_authority_peer
    );
    if (is_connected(StringName("peer_connected"), on_authority)) {
        disconnect(StringName("peer_connected"), on_authority);
    }
    const JoinRequest request = *resubmit_join;
    resubmit_join.reset();
    session_submit_request(request);
}

Ref<NetwPromise> NetwMultiplayer::session_prepare_join(
    const StringName &p_username,
    const Array &p_args
) {
    NETW_ZONE_NC("netw::session_prepare_join", colors::SESSION);
    session_clear_prepared_join();
    if (String(p_username).is_empty()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{Netw.prepare_join}", String(), "username"}
        );
        return NetwPromise::resolved(ERR_INVALID_PARAMETER);
    }

    auth_arm();
    prepared_join = connect::request_of(p_username, p_args);
    return NetwPromise::resolved(OK);
}

void NetwMultiplayer::session_push_desired_role() {
    session_set_desired_role(session_get_authored_role());
}

void NetwMultiplayer::session_apply_auth_config() {
    auth_set_app_tag(session_app_tag(session_settings.app_id));
    auth_arm();
}

NetwMultiplayer::Role NetwMultiplayer::session_get_authored_role() const {
    if (hosted_role.has_value()) {
        return *hosted_role;
    }
    const int64_t named = session_settings.desired_role;
    if (named < ROLE_NONE || named > ROLE_LISTEN_SERVER) {
        return ROLE_LISTEN_SERVER;
    }
    return Role(named);
}

void NetwMultiplayer::set_relay_sender(int64_t p_sender) {
    dispatching_sender = p_sender;
}

int64_t NetwMultiplayer::rpc_get_relay_sender() const {
    return dispatching_sender;
}

bool NetwMultiplayer::connect_once(
    Signal p_signal,
    const Callable &p_callback,
    int64_t p_flags
) {
    if (p_signal.is_connected(p_callback)) {
        return true;
    }
    const int64_t verdict = int64_t(p_signal.connect(p_callback, p_flags));
    if (verdict == int64_t(OK)) {
        return true;
    }
    count_verdict(ERR_UNAVAILABLE, 0);
    NETW_BUG_V(false);
}

bool NetwMultiplayer::disconnect_once(
    Signal p_signal,
    const Callable &p_callback
) {
    if (!p_signal.is_connected(p_callback)) {
        return false;
    }
    p_signal.disconnect(p_callback);
    return true;
}

void NetwMultiplayer::clear_flat_family_state() {
    layer_forget_all();
    scene_clear_pending_facets();
    array_tables.clear_session();
    display_clear_declarations();
}

void NetwMultiplayer::clear_session_state() {
    turned_away_peers.clear();
    display_last_timing.reset();
    world.wire.seq_book.clear();
    world.clock.exchange.clear();
    ReplicationCore *plane = (get_replication_plane());
    if (plane != nullptr) {
        plane->clear_session();
    }
    rpc_clear_session();
    clear_verdicts();
    clear_flat_family_state();
}

SchemaBook *NetwMultiplayer::get_schema_core() {
    return &schema_core;
}

void NetwMultiplayer::clear_roster() {
    session_clear_roster();
    player_clear();
}

int64_t NetwMultiplayer::register_rewind(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return -1;
    }
    const int64_t slot
        = lagcomp_core.timeline_register(wrapper, LagBook::DEFAULT_WINDOW);
    store_bind_entity(p_entity);
    return slot;
}

Ref<NetwMultiplayer> NetwMultiplayer::create() {
    Ref<NetwMultiplayer> made;
    made.instantiate();
    return made;
}

void NetwMultiplayer::install_default_interface() {
    ProjectSettings *settings = ProjectSettings::get_singleton();
    const bool wanted = settings == nullptr
        || bool(settings->get_setting(INSTALL_AS_DEFAULT_SETTING, true));
    if (!wanted) {
        return;
    }
    displaced_default_interface() = MultiplayerAPI::get_default_interface();
    MultiplayerAPI::set_default_interface(installed_default_interface());
}

void NetwMultiplayer::restore_default_interface() {
    const StringName restoring = displaced_default_interface();
    displaced_default_interface() = StringName();
    if (restoring == StringName()
        || MultiplayerAPI::get_default_interface() != get_class_static()) {
        return;
    }
    MultiplayerAPI::set_default_interface(restoring);
}

void NetwMultiplayer::embed_settle_on_first_poll() {
    if (embed_phase_value != EMBED_PHASE_DECLARING
        || session_get_root() == nullptr) {
        return;
    }
    embed_settle();
}

void NetwMultiplayer::peer_forget(int64_t p_peer) {
    const bool leaving = player_admitted_of(p_peer).is_valid();
    const int64_t *seated = player_id_of_peer.getptr(p_peer);
    const int64_t player_id = seated != nullptr ? *seated : 0;
    NetwPlayer::LeaveReason reason = NetwPlayer::LEAVE_DROPPED;
    if (PlayerRow *found = players.getptr(player_id)) {
        reason = found->leave_reason;
        found->admitted = false;
        found->departed = true;
    }
    session_forget_peer(p_peer);
    if (leaving) {
        player_publish_left(player_id, reason);
    }
    player_forget(player_id);
    world.wire.attribution.forget_peer(p_peer);
    world.wire.traffic.forget_peer(p_peer);
}

int64_t NetwMultiplayer::peer_get_player(int64_t p_peer) const {
    if (!session_has_accepted(p_peer)) {
        return 0;
    }
    const int64_t *player = player_id_of_peer.getptr(p_peer);
    return player != nullptr && players.has(*player) ? *player : 0;
}

void NetwMultiplayer::handle_deny(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    session::DenyKey denied;
    if (p_sender != SERVER_PEER || !session::frame_read(p_payload, denied)) {
        return;
    }
    effect_discard_denied(
        denied.key,
        denied.code == OK ? ERR_UNAUTHORIZED : Error(denied.code),
        denied.reason
    );
}

void NetwMultiplayer::handle_confirm(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    session::ConfirmKey confirmed;
    if (p_sender != SERVER_PEER || !session::frame_read(p_payload, confirmed)) {
        return;
    }
    effect_adopt(confirmed.key);
}

bool NetwMultiplayer::is_coroutine(const Variant &p_value) {
    if (p_value.get_type() != Variant::OBJECT) {
        return false;
    }
    Object *object = p_value;
    return object != nullptr
        && object->get_class() == String("GDScriptFunctionState");
}

bool NetwMultiplayer::is_waiting(const Variant &p_value) {
    return is_coroutine(p_value)
        || Object::cast_to<NetwPromise>(gd::live_object(p_value)) != nullptr;
}

void NetwMultiplayer::free_when_returned(const Variant &p_waiting) {
    Object *waiting = gd::live_object(p_waiting);
    if (NetwPromise *promise = Object::cast_to<NetwPromise>(waiting)) {
        promise->then(
            callable_mp_static(&NetwMultiplayer::free_returned).bind(p_waiting)
        );
        return;
    }
    if (is_coroutine(p_waiting)) {
        waiting->connect(
            StringName("completed"),
            callable_mp_static(&NetwMultiplayer::free_returned).bind(p_waiting),
            Object::CONNECT_ONE_SHOT
        );
    }
}

void NetwMultiplayer::free_returned(const Variant &p_value, const Variant &) {
    Node *node = Object::cast_to<Node>(gd::live_object(p_value));
    if (node == nullptr || node->is_queued_for_deletion()) {
        return;
    }
    if (node->is_inside_tree()) {
        node->queue_free();
        return;
    }
    if (Node *parent = node->get_parent()) {
        parent->remove_child(node);
    }
    memdelete(node);
}

void NetwMultiplayer::promise_refuse(
    const Ref<NetwPromise> &p_promise,
    Error p_code,
    const String &p_detail
) {
    p_promise->reject(p_code, p_detail);
    p_promise->mark_failure_observed();
}

void NetwMultiplayer::spawn_async_returned(
    const Variant &p_value,
    const Variant &,
    uint64_t p_plane,
    int64_t p_serial
) {
    NetwMultiplayer *session = Object::cast_to<NetwMultiplayer>(
        gd::instance_from_id(ObjectID(p_plane))
    );
    spawn::Pipeline *pipeline
        = session != nullptr ? session->spawn_plane() : nullptr;
    if (pipeline == nullptr
        || !session->spawn_pending_returned(p_value, p_serial)) {
        free_returned(p_value, Variant());
    }
}

void NetwMultiplayer::spawn_async_failed(
    int64_t p_code,
    const String &p_detail,
    const Variant &,
    uint64_t p_plane,
    int64_t p_serial
) {
    NetwMultiplayer *session = Object::cast_to<NetwMultiplayer>(
        gd::instance_from_id(ObjectID(p_plane))
    );
    if (session != nullptr && session->spawn_plane() != nullptr) {
        session->spawn_pending_failed(p_code, p_detail, p_serial);
    }
}

void NetwMultiplayer::spawn_build_returned(
    const Variant &p_value,
    const Variant &,
    uint64_t p_plane,
    int64_t p_route,
    int64_t p_serial
) {
    NetwMultiplayer *session = Object::cast_to<NetwMultiplayer>(
        gd::instance_from_id(ObjectID(p_plane))
    );
    spawn::Pipeline *pipeline
        = session != nullptr ? session->spawn_plane() : nullptr;
    if (pipeline == nullptr) {
        free_returned(p_value, Variant());
        return;
    }
    spawn::Ops ops;
    const bool held = pipeline->build_returned(p_value, p_route, p_serial, ops);
    session->spawn_act(ops);
    if (!held) {
        free_returned(p_value, Variant());
    }
}

void NetwMultiplayer::spawn_build_failed(
    int64_t,
    const String &p_detail,
    const Variant &,
    uint64_t p_plane,
    int64_t p_route,
    int64_t p_serial
) {
    NetwMultiplayer *session = Object::cast_to<NetwMultiplayer>(
        gd::instance_from_id(ObjectID(p_plane))
    );
    if (spawn::Pipeline *pipeline
        = session != nullptr ? session->spawn_plane() : nullptr) {
        spawn::Ops ops;
        pipeline->build_failed(p_detail, p_route, p_serial, ops);
        session->spawn_act(ops);
    }
}

} // namespace netw
