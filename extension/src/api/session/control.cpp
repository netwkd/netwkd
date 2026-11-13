#include "netw/api/netw_multiplayer.hpp"

#include "godot/multiplayer.hpp"
#include "godot/object.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/entity/control.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/peer_rename.hpp"
#include "netw/repl/set_model.hpp"
#include "netw/sync_authoring.hpp"
#include "netw/tree_hook.hpp"
#include "netw/wire/stream.hpp"

using namespace godot;
using namespace netw;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertySet;

namespace {

bool streams_after_spawn(const TypedArray<MultiplayerSynchronizer> &p_held) {
    for (int at = 0; at < p_held.size(); ++at) {
        MultiplayerSynchronizer *sync
            = Object::cast_to<MultiplayerSynchronizer>(p_held[at]);
        const Ref<SceneReplicationConfig> config = sync != nullptr
            ? sync->get_replication_config()
            : Ref<SceneReplicationConfig>();
        if (config.is_null()) {
            continue;
        }
        const TypedArray<NodePath> paths = config->get_properties();
        for (int row = 0; row < paths.size(); ++row) {
            const NodePath path = paths[row];
            if (int(config->property_get_replication_mode(path))
                != SceneReplicationConfig::REPLICATION_MODE_NEVER) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void NetwMultiplayer::control_structure_issue(
    NetwEntity *p_entity,
    uint64_t p_base,
    const Ref<NetwPromise> &p_promise
) {
    NetwEntityRecord::StructureOp issued;
    issued.base = p_base;
    issued.promise = p_promise;
    p_entity->get_record()->get_control_book().structure_ops.push_back(issued);
}

void NetwMultiplayer::control_structure_accept(
    NetwEntity *p_entity,
    uint64_t p_revision
) {
    LocalVector<NetwEntityRecord::StructureOp> &structure_ops
        = p_entity->get_record()->get_control_book().structure_ops;
    uint32_t at = 0;
    while (at < structure_ops.size()) {
        if (structure_ops[at].base >= p_revision) {
            ++at;
            continue;
        }
        const Ref<NetwPromise> promise = structure_ops[at].promise;
        structure_ops.remove_at(at);
        if (promise.is_valid() && !promise->get_is_settled()) {
            promise->resolve(OK);
        }
    }
}

bool NetwMultiplayer::control_structure_refuse(
    NetwEntity *p_entity,
    uint64_t p_base,
    Error p_code,
    const String &p_detail
) {
    bool refused = false;
    LocalVector<NetwEntityRecord::StructureOp> &structure_ops
        = p_entity->get_record()->get_control_book().structure_ops;
    uint32_t at = 0;
    while (at < structure_ops.size()) {
        if (structure_ops[at].base < p_base) {
            ++at;
            continue;
        }
        const Ref<NetwPromise> promise = structure_ops[at].promise;
        structure_ops.remove_at(at);
        refused = true;
        if (promise.is_valid() && !promise->get_is_settled()) {
            promise->reject(p_code, p_detail);
        }
    }
    return refused;
}

Error NetwMultiplayer::control_transfer(
    NetwEntity *p_entity,
    int64_t p_steering
) {
    if (p_entity->get_stage() == int64_t(entity::Stage::UNBOUND)) {
        fault::raise(
            p_entity->session_core(),
            NETW_FAULT_NOT_RUNNING,
            {String("Giving '") + String(p_entity->get_name())
                 + String("' to a peer"),
             String(),
             String("the entity")},
            uint64_t(gd::instance_id(p_entity->get_owner()))
        );
        return ERR_UNCONFIGURED;
    }
    if (!p_entity->is_on_server()) {
        fault::raise(
            p_entity->session_core(),
            NETW_FAULT_SERVER_ONLY,
            {String("{NetwEntity.controller}")},
            uint64_t(gd::instance_id(p_entity->get_owner()))
        );
        return ERR_UNAUTHORIZED;
    }
    control_change(p_entity, p_steering);
    return OK;
}

Error NetwMultiplayer::control_give(NetwEntity *p_entity, int64_t p_peer) {
    if (p_peer <= 0) {
        fault::raise(
            p_entity->session_core(),
            NETW_FAULT_BAD_VALUE,
            {String("{NetwEntity.controller}"),
             String::num_int64(p_peer),
             String("a peer id, or 1 for the server")},
            uint64_t(gd::instance_id(p_entity->get_owner()))
        );
        return ERR_INVALID_PARAMETER;
    }
    const int64_t steering = control_steerer_of_peer(p_entity, p_peer);
    const Error given = control_transfer(p_entity, steering);
    if (given == OK) {
        control_note_host_gift(p_entity, p_peer, steering);
    }
    return given;
}

void NetwMultiplayer::control_note_host_gift(
    NetwEntity *p_entity,
    int64_t p_peer,
    int64_t p_steering
) {
    NetwMultiplayer *core = p_entity->session_core();
    if (p_peer != SERVER_PEER || p_steering != entity::Control::SERVER
        || core == nullptr) {
        return;
    }
    const RID rid = p_entity->get_rid_handle();
    HostGift gift;
    gift.entity = rid;
    gift.revision = p_entity->control()->revision;
    core->control_host_gifts.insert(rid.get_id(), gift);
}

void NetwMultiplayer::control_seat_host() {
    LocalVector<HostGift> gifts;
    gifts.reserve(control_host_gifts.size());
    for (const KeyValue<uint64_t, HostGift> &gift : control_host_gifts) {
        gifts.push_back(gift.value);
    }
    control_host_gifts.clear();
    for (const HostGift &gift : gifts) {
        const Ref<NetwEntity> entity = entity_get_view(gift.entity);
        if (entity.is_null() || entity->control()->revision != gift.revision
            || entity->steering() != entity::Control::SERVER) {
            continue;
        }
        control_transfer(entity.ptr(), SERVER_PEER);
    }
}

int64_t NetwMultiplayer::control_steerer_of_peer(
    const NetwEntity *p_entity,
    int64_t p_peer
) {
    const NetwMultiplayer *core = p_entity->session_core();
    if (p_peer == SERVER_PEER && core != nullptr
        && core->player_has(SERVER_PEER)) {
        return SERVER_PEER;
    }
    return entity::Control::from_public(p_peer);
}

Error NetwMultiplayer::control_set_param(
    NetwEntity *p_entity,
    EntityParam p_param,
    const Variant &p_value
) {
    entity::Control *control = p_entity->control();
    const uint64_t subject = uint64_t(gd::instance_id(p_entity->get_owner()));
    switch (p_param) {
        case ENTITY_PARAM_AUTHORITY: {
            const int64_t value = int64_t(p_value);
            if (value != NetwEntity::AUTHORITY_SERVER
                && value != NetwEntity::AUTHORITY_CONTROLLER) {
                fault::raise(
                    p_entity->session_core(),
                    NETW_FAULT_BAD_VALUE,
                    {String("{NetwEntity.authority}"),
                     String::num_int64(value),
                     String("{NetwEntity.AUTHORITY_SERVER} or "
                            "{NetwEntity.AUTHORITY_CONTROLLER}")},
                    subject
                );
                return ERR_INVALID_PARAMETER;
            }
            if (p_entity->get_stage() != int64_t(entity::Stage::UNBOUND)) {
                fault::raise(
                    p_entity->session_core(),
                    NETW_FAULT_TOO_LATE,
                    {String("{NetwEntity.authority}"),
                     String("'") + String(p_entity->get_name()) + String("'"),
                     String("the node enters the tree")},
                    subject
                );
                return ERR_LOCKED;
            }
            control->set_authority(value);
            return OK;
        }
        case ENTITY_PARAM_TRANSFER: {
            const int64_t value = int64_t(p_value);
            if (value == NetwEntity::TRANSFER_IMMEDIATE
                && !control->authority_follows_controller()
                && p_entity->get_stage() != int64_t(entity::Stage::UNBOUND)) {
                fault::raise(
                    p_entity->session_core(),
                    NETW_FAULT_SETTING_REFUSED,
                    {String("{NetwEntity.TRANSFER_IMMEDIATE} on '")
                         + String(p_entity->get_name()) + String("'"),
                     String("{NetwEntity.AUTHORITY_SERVER}"),
                     String("{NetwEntity.AUTHORITY_CONTROLLER} before the "
                            "node enters the tree")},
                    subject
                );
                return ERR_UNAVAILABLE;
            }
            control->set_transfer(value);
            NetwMultiplayer *core = p_entity->session_core();
            if (core != nullptr) {
                core->sim_mark(p_entity->get_rid_handle());
                core->sim_follow_steering(p_entity);
            }
            return OK;
        }
        case ENTITY_PARAM_ON_CONTROLLER_DISCONNECT:
            control->set_on_disconnect(int64_t(p_value));
            return OK;
        case ENTITY_PARAM_ON_PARENT_DESPAWN:
            control->set_on_parent_despawn(int64_t(p_value));
            return OK;
        case ENTITY_PARAM_REPLICATION_PRIORITY: {
            const double weight = double(p_value);
            if (!(weight > 0.0)) {
                fault::raise(
                    p_entity->session_core(),
                    NETW_FAULT_BAD_VALUE,
                    {String("{NetwEntity.replication_priority}"),
                     String::num(weight),
                     String("a weight above 0, or {NetwEntity.interest} to "
                            "hide the entity from peers")},
                    subject
                );
                return ERR_INVALID_PARAMETER;
            }
            p_entity->get_record()->set_replication_priority(weight);
            control_moved(p_entity);
            return OK;
        }
        case ENTITY_PARAM_HOLD:
            fault::raise(
                p_entity->session_core(),
                NETW_FAULT_UNSUPPORTED,
                {String("{NetwMultiplayer.entity_set_param}"),
                 String("{NetwMultiplayer.ENTITY_PARAM_HOLD}"),
                 String("{NetwMultiplayer.entity_request_control}")}
            );
            return ERR_UNAVAILABLE;
        default:
            fault::raise(
                p_entity->session_core(),
                NETW_FAULT_BAD_VALUE,
                {"An entity setting",
                 vformat("setting %d", int(p_param)),
                 "an entity setting that exists"}
            );
            return ERR_INVALID_PARAMETER;
    }
}

void NetwMultiplayer::control_record(NetwEntity *p_entity, int64_t p_value) {
    const int64_t was = p_entity->control()->get_controller();
    if (!p_entity->get_record()->set_controller(nullptr, p_value)) {
        return;
    }
    control_announce(p_entity, was, p_value);
}

void NetwMultiplayer::control_follow(const RID &p_entity) {
    predict_follow_control(p_entity);
    sim_follow_control(p_entity);
    display_follow_control(p_entity);
}

void NetwMultiplayer::control_announce(
    NetwEntity *p_entity,
    int64_t p_was,
    int64_t p_peer
) {
    control_moved(p_entity);
    Announcement row;
    row.kind = AnnouncementKind::CONTROL_CHANGED;
    row.from = entity::Control::public_peer(p_was);
    row.to = entity::Control::public_peer(p_peer);
    entity_announce(p_entity, row);
    NetwMultiplayer *core = p_entity->session_core();
    if (core == nullptr) {
        return;
    }
    core->event_record(
        EventPlane::CONTROL_CHANGED,
        p_entity->get_rid_handle(),
        p_entity->get_route(),
        row.to,
        OK,
        row.from,
        row.to
    );
}

void NetwMultiplayer::control_apply(NetwEntity *p_entity) {
    control_project(p_entity, p_entity);
}

void NetwMultiplayer::control_project(
    NetwEntity *p_entity,
    Object *p_announcer
) {
    NetwMultiplayer *api = p_entity->session_core();
    p_entity->get_record()->apply_control(
        p_announcer,
        p_entity->get_owner(),
        uint64_t(p_entity->get_instance_id())
    );
    if (api != nullptr && p_entity->get_record()->projects_to_server()) {
        api->entity_watch_authority_joins();
    }
    if (!p_entity->control()->authority_follows_controller()
        && p_entity->get_controller() != SERVER_PEER
        && p_entity->get_owner() != nullptr
        && p_entity->get_owner()->is_inside_tree()
        && streams_after_spawn(p_entity->synchronizers()) && api != nullptr) {
        fault::raise(
            api,
            NETW_FAULT_CONFLICTING_SETTINGS,
            {String("NetwEntity.controller"),
             String(p_entity->get_name()),
             String("NetwEntity.authority at AUTHORITY_SERVER")},
            uint64_t(gd::instance_id(p_entity->get_owner()))
        );
    }
    control_moved(p_entity);
}

void NetwMultiplayer::control_descendant_entered(
    NetwEntity *p_entity,
    Node *p_node
) {
    Node *owner = p_entity->get_owner();
    if (p_node == nullptr || owner == nullptr) {
        return;
    }
    Node *parent = p_node->get_parent();
    if (parent != owner && !owner->is_ancestor_of(parent)) {
        tree::unwatch(parent);
        return;
    }
    p_entity->get_record()
        ->project_entered(owner, p_node, uint64_t(p_entity->get_instance_id()));
    control_moved(p_entity);
}

void NetwMultiplayer::control_adopt(NetwEntity *p_entity, int64_t p_peer) {
    const int64_t was = p_entity->control()->get_controller();
    if (!p_entity->get_record()->set_controller(nullptr, p_peer)) {
        control_apply(p_entity);
        return;
    }
    control_project(p_entity, nullptr);
    control_announce(p_entity, was, p_peer);
}

int64_t NetwMultiplayer::control_default_controller(
    const NetwEntity *p_entity
) {
    const int64_t resolved = p_entity->control()->resolve();
    if (resolved != entity::Control::SERVER
        || !p_entity->control()->authority_follows_controller()) {
        return resolved;
    }
    Node *owner = p_entity->get_owner();
    const int64_t claimed = owner != nullptr
        ? int64_t(owner->get_multiplayer_authority())
        : SERVER_PEER;
    return entity::Control::from_public(claimed);
}

void NetwMultiplayer::control_peer_departed(
    NetwEntity *p_entity,
    int64_t p_peer
) {
    Node *owner = p_entity->get_owner();
    if (owner == nullptr || !owner->is_inside_tree()
        || !p_entity->is_on_server()) {
        return;
    }
    const entity::Control::Verdict verdict
        = p_entity->control()->disconnect_verdict(
            p_peer,
            p_entity->get_peer_id()
        );
    NetwMultiplayer *core = p_entity->session_core();
    if (verdict != entity::Control::NOTHING && core != nullptr
        && core->player_keep_entity(p_entity, p_peer)) {
        return;
    }
    switch (verdict) {
        case entity::Control::DESPAWN_REPRESENTED: {
            NETW_INFO(
                sys::ENTITY,
                "Peer %d disconnected, so its entity '%s' is despawned.",
                p_peer,
                owner->get_name()
            );
            p_entity->despawn(
                NetwDespawnOpts::create(StringName("peer_disconnected"))
            );
        } break;
        case entity::Control::REVERT_TO_SERVER:
            control_change(p_entity, entity::Control::SERVER);
            break;
        case entity::Control::DESPAWN_CONTROLLER:
            p_entity->despawn(
                NetwDespawnOpts::create(StringName("controller_disconnected"))
            );
            break;
        default:
            break;
    }
}

bool NetwMultiplayer::control_rename_peers(
    NetwEntity *p_entity,
    const PeerRename &p_rename
) {
    entity::Control *held = p_entity->control();
    const int64_t peer = p_entity->get_record()->get_peer_id();
    const int64_t controller = held->controller;
    p_entity->get_record()->set_peer_id(p_rename.of(peer));
    held->controller = p_rename.of(controller);
    return p_entity->get_record()->get_peer_id() != peer
        || held->controller != controller;
}

void NetwMultiplayer::control_follow_rename(NetwEntity *p_entity) {
    const int64_t stage = p_entity->get_stage();
    if (stage == int64_t(entity::Stage::ARMED)
        || stage == int64_t(entity::Stage::LIVE)) {
        control_project(p_entity, nullptr);
    }
}

void NetwMultiplayer::control_forget_server_claims(NetwEntity *p_entity) {
    if (!p_entity->get_record()->get_control_book().control_claims.is_empty()
        || p_entity->control()->has_pending()) {
        control_reject_claims(
            p_entity,
            "the server moved while '%s' waited for control"
        );
    }
    p_entity->get_record()->get_control_book().held_claim_image
        = NetwEntityRecord::HeldClaimImage();
}

bool NetwMultiplayer::control_deciding_locally(const NetwEntity *p_entity) {
    Node *owner = p_entity->get_owner();
    if (owner == nullptr || !owner->is_inside_tree()) {
        return false;
    }
    const Ref<MultiplayerAPI> api = owner->get_multiplayer();
    if (api.is_valid() && api->get_multiplayer_peer().is_null()) {
        return true;
    }
    return p_entity->is_on_server();
}

bool NetwMultiplayer::control_immediate_unavailable(NetwEntity *p_entity) {
    if (p_entity->control()->get_transfer()
        != int64_t(entity::Control::Transfer::IMMEDIATE)) {
        return false;
    }
    Node *owner = p_entity->get_owner();
    if (owner != nullptr && authoring::declares_prediction(owner)) {
        return true;
    }
    if (p_entity->get_state_binding() != nullptr) {
        return true;
    }
    return !p_entity->synchronizers().is_empty();
}

String NetwMultiplayer::control_refusal_detail(
    const NetwEntity *p_entity,
    int64_t p_hold
) {
    return vformat(
        "control of '%s' with hold %d was refused, peer %d holds it with "
        "hold %d",
        String(p_entity->get_name()),
        p_hold,
        p_entity->get_controller(),
        p_entity->control()->hold
    );
}

Error NetwMultiplayer::control_local_refusal(
    NetwEntity *p_entity,
    int64_t p_local,
    int64_t p_hold
) {
    if (p_entity->control()->pending.size() >= entity::Control::MAX_PENDING) {
        return ERR_UNAVAILABLE;
    }
    if (control_immediate_unavailable(p_entity)) {
        return ERR_UNAVAILABLE;
    }
    if (control_deciding_locally(p_entity)
        && !p_entity->control()->admits_request()) {
        return ERR_UNAVAILABLE;
    }
    if (entity::Control::excludes(
            p_local,
            p_hold,
            p_entity->steering(),
            p_entity->control()->hold
        )) {
        return ERR_UNAUTHORIZED;
    }
    return OK;
}

Ref<NetwPromise> NetwMultiplayer::control_claim_by_contact(
    NetwEntity *p_entity,
    const Ref<NetwEntity> &p_source
) {
    NetwMultiplayer *core = p_entity->session_core();
    if (p_source.is_null() || core == nullptr
        || !p_source->get_is_controlled_locally()) {
        return NetwPromise::rejected(
            ERR_UNAUTHORIZED,
            vformat(
                "a contact claim on '%s' names a source this peer does not "
                "control",
                String(p_entity->get_name())
            )
        );
    }
    const int64_t live = core->liveness_route_of(p_source.ptr());
    return control_claim(
        p_entity,
        entity::Control::HOLD_SOFT,
        live > 0 ? live : p_source->get_route()
    );
}

bool NetwMultiplayer::control_controls_source(
    const NetwEntity *p_entity,
    int64_t p_requester,
    int64_t p_source_route
) {
    if (p_source_route <= 0) {
        return true;
    }
    NetwMultiplayer *core = p_entity->session_core();
    const Ref<NetwEntity> source = core != nullptr
        ? core->wrapper_for_route(p_source_route)
        : Ref<NetwEntity>();
    return source.is_valid() && source->steering() == p_requester;
}

Ref<NetwPromise> NetwMultiplayer::control_claim(
    NetwEntity *p_entity,
    int64_t p_hold,
    int64_t p_source_route
) {
    Node *owner = p_entity->get_owner();
    const Ref<MultiplayerAPI> api = owner != nullptr && owner->is_inside_tree()
        ? owner->get_multiplayer()
        : Ref<MultiplayerAPI>();
    if (api.is_null()) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' is in no session, so no peer can decide its control",
                String(p_entity->get_name())
            )
        );
    }
    const int64_t local = api->get_unique_id();
    const int64_t hold = p_hold;
    const Error refused = control_local_refusal(p_entity, local, hold);
    if (refused != OK) {
        return NetwPromise::rejected(
            refused,
            control_refusal_detail(p_entity, hold)
        );
    }
    if (!p_entity->control()->has_pending() && p_entity->steering() == local
        && p_entity->control()->hold == hold) {
        return NetwPromise::resolved(Ref<NetwEntity>(p_entity));
    }
    if (control_deciding_locally(p_entity)) {
        const entity::Control::Outcome outcome
            = control_decide_request(p_entity, local, 0, hold, p_source_route);
        if (outcome == entity::Control::Outcome::GRANTED) {
            return NetwPromise::resolved(Ref<NetwEntity>(p_entity));
        }
        return NetwPromise::rejected(
            outcome == entity::Control::Outcome::UNAVAILABLE ? ERR_UNAVAILABLE
                                                             : ERR_UNAUTHORIZED,
            control_refusal_detail(p_entity, hold)
        );
    }
    NetwMultiplayer *core = p_entity->session_core();
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr || core == nullptr) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' has no session plane to carry a control request",
                String(p_entity->get_name())
            )
        );
    }
    const bool was_ahead = p_entity->is_claim_running_ahead();
    const entity::Control::Pending *issued = p_entity->control()->issue(
        entity::Control::OpKind::REQUEST,
        hold,
        core->clock_get_tick()
    );
    if (issued == nullptr) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            control_refusal_detail(p_entity, hold)
        );
    }
    NetwEntityRecord::ControlClaim claim;
    claim.op = issued->op;
    claim.promise.instantiate();
    p_entity->get_record()->get_control_book().control_claims.push_back(claim);
    NETW_TRACE(sys::ENTITY, "Asking the session authority for control.");
    control_send_op(p_entity, *issued, 0, PackedByteArray(), p_source_route);
    control_follow_claim(p_entity, was_ahead);
    return claim.promise;
}

Ref<NetwPromise> NetwMultiplayer::control_release(
    NetwEntity *p_entity,
    int64_t p_successor
) {
    if (p_successor < 0) {
        return NetwPromise::rejected(
            ERR_INVALID_PARAMETER,
            vformat(
                "'%s' cannot be released to peer %d. Pass a peer id, or 0 "
                "for the server",
                String(p_entity->get_name()),
                p_successor
            )
        );
    }
    const int64_t local = p_entity->local_peer();
    if (!p_entity->control()->claims_pending()
        && p_entity->steering() != local) {
        return NetwPromise::rejected(
            ERR_UNAUTHORIZED,
            vformat(
                "'%s' is not controlled by peer %d, so it has nothing to "
                "release",
                String(p_entity->get_name()),
                local
            )
        );
    }
    if (!control_holds_copy(p_entity, p_successor)) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "peer %d holds no copy of '%s', so it cannot take it on",
                p_successor,
                String(p_entity->get_name())
            )
        );
    }
    NetwMultiplayer *core = p_entity->session_core();
    const PackedByteArray image = control_final_image(
        p_entity,
        core != nullptr ? core->clock_get_tick() : 0
    );
    if (control_deciding_locally(p_entity)) {
        const entity::Control::Outcome outcome
            = control_decide_release(p_entity, local, 0, p_successor, image);
        if (outcome == entity::Control::Outcome::GRANTED) {
            return NetwPromise::resolved(Ref<NetwEntity>(p_entity));
        }
        return NetwPromise::rejected(
            outcome == entity::Control::Outcome::UNAVAILABLE ? ERR_UNAVAILABLE
                                                             : ERR_UNAUTHORIZED,
            control_refusal_detail(p_entity, entity::Control::HOLD_NONE)
        );
    }
    const entity::Control::Pending *issued = core == nullptr
        ? nullptr
        : p_entity->control()->issue(
              entity::Control::OpKind::RELEASE,
              entity::Control::HOLD_NONE,
              core->clock_get_tick()
          );
    if (issued == nullptr) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' cannot carry another control request now",
                String(p_entity->get_name())
            )
        );
    }
    NetwEntityRecord::ControlClaim claim;
    claim.op = issued->op;
    claim.promise.instantiate();
    p_entity->get_record()->get_control_book().control_claims.push_back(claim);
    control_moved(p_entity);
    control_send_op(p_entity, *issued, p_successor, image);
    return claim.promise;
}

bool NetwMultiplayer::control_holds_copy(NetwEntity *p_entity, int64_t p_peer) {
    if (p_peer == entity::Control::SERVER || p_peer == SERVER_PEER) {
        return true;
    }
    NetwMultiplayer *core = p_entity->session_core();
    if (core == nullptr) {
        return false;
    }
    if (p_peer == p_entity->local_peer()) {
        return true;
    }
    return core->rpc_recipients(Ref<NetwEntity>(p_entity))
        .has(int32_t(p_peer));
}

LocalVector<PropertyBindingPtr> NetwMultiplayer::control_image_bindings(
    NetwEntity *p_entity
) {
    LocalVector<PropertyBindingPtr> out;
    const LocalVector<PropertyBindingPtr> authored
        = control_authored_bindings(p_entity);
    for (const PropertyBindingPtr &binding : authored) {
        if (binding->get_set()->get_record() == PropertySet::RECORD_BROADCAST) {
            out.push_back(binding);
        }
    }
    return out;
}

PackedByteArray NetwMultiplayer::control_final_image(
    NetwEntity *p_entity,
    int64_t p_tick
) {
    int64_t measured = 0;
    const PackedByteArray bytes
        = control_state_image(p_entity, p_tick, measured);
    if (!bytes.is_empty() || measured <= session::CONTROL_FINAL_STATE_CAP) {
        return bytes;
    }
    fault::raise(
        p_entity->session_core(),
        NETW_FAULT_LIMIT_EXCEEDED,
        {String("'") + String(p_entity->get_name())
             + String("' released authority"),
         String("the state handed to the next authority"),
         String("fewer or smaller properties on this entity"),
         0,
         measured,
         session::CONTROL_FINAL_STATE_CAP},
        uint64_t(gd::instance_id(p_entity->get_owner()))
    );
    return PackedByteArray();
}

PackedByteArray NetwMultiplayer::control_state_image(
    NetwEntity *p_entity,
    int64_t p_tick,
    int64_t &r_size
) {
    r_size = 0;
    const LocalVector<PropertyBindingPtr> bindings
        = control_image_bindings(p_entity);
    NetwMultiplayer *core = p_entity->session_core();
    if (bindings.is_empty() || core == nullptr) {
        return PackedByteArray();
    }
    LocalVector<PackedByteArray> runs;
    int64_t size = 0;
    for (const PropertyBindingPtr &binding : bindings) {
        const PackedByteArray run = core->store_take_image(binding);
        if (run.is_empty()) {
            return PackedByteArray();
        }
        size += run.size();
        runs.push_back(run);
    }
    wire::WriteStream stream;
    uint64_t stamped = uint64_t(MAX(p_tick, int64_t(-1)) + 1);
    uint64_t count = uint64_t(runs.size());
    stream.varuint(stamped);
    stream.varuint(count);
    for (PackedByteArray &run : runs) {
        stream.bytes_capped(run, session::CONTROL_FINAL_STATE_CAP);
    }
    stream.align_verify();
    const PackedByteArray bytes = stream.to_bytes();
    r_size = MAX(size, int64_t(bytes.size()));
    if (stream.ok() && r_size <= session::CONTROL_FINAL_STATE_CAP) {
        return bytes;
    }
    return PackedByteArray();
}

int64_t NetwMultiplayer::control_install_final_image(
    NetwEntity *p_entity,
    const PackedByteArray &p_final_state,
    int64_t p_after_tick
) {
    if (p_final_state.is_empty()) {
        return -2;
    }
    const LocalVector<PropertyBindingPtr> bindings
        = control_image_bindings(p_entity);
    wire::ReadStream reader(p_final_state);
    uint64_t stamped = 0;
    uint64_t count = 0;
    if (!reader.varuint(stamped) || !reader.varuint(count)
        || count != uint64_t(bindings.size())
        || int64_t(stamped) - 1 <= p_after_tick) {
        return -2;
    }
    LocalVector<Array> keys;
    LocalVector<Array> values;
    for (const PropertyBindingPtr &binding : bindings) {
        PackedByteArray run;
        Array run_keys;
        Array run_values;
        if (!reader.bytes_capped(run, session::CONTROL_FINAL_STATE_CAP)
            || !binding->read_image(run, run_keys, run_values)) {
            return -2;
        }
        keys.push_back(run_keys);
        values.push_back(run_values);
    }
    NetwMultiplayer *core = p_entity->session_core();
    for (uint32_t at = 0; at < bindings.size() && core != nullptr; ++at) {
        const PropertyBindingPtr &binding = bindings[at];
        core->store_install_image(
            binding,
            keys[at],
            values[at],
            int64_t(stamped) - 1
        );
    }
    return int64_t(stamped) - 1;
}

void NetwMultiplayer::control_start_claim_images(NetwEntity *p_entity) {
    NetwMultiplayer *core = p_entity->session_core();
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (core == nullptr || plane == nullptr
        || p_entity->get_transfer() != NetwEntity::TRANSFER_IMMEDIATE) {
        return;
    }
    const int64_t now = core->clock_get_tick();
    const int64_t tickrate
        = int64_t(core->clock_get_param(NetwMultiplayer::CLOCK_PARAM_TICKRATE));
    p_entity->get_record()->get_control_book().claim_image_until
        = now + 2 * tickrate;
    plane->watch_claim_image(Ref<NetwEntity>(p_entity));
}

bool NetwMultiplayer::control_sends_claim_image(
    const NetwEntity *p_entity,
    int64_t p_tick
) {
    if (p_entity->get_owner() == nullptr
        || p_tick
            > p_entity->get_record()->get_control_book().claim_image_until) {
        return false;
    }
    return p_entity->is_claim_running_ahead() || p_entity->is_controller_here();
}

PackedByteArray NetwMultiplayer::control_claim_image(
    NetwEntity *p_entity,
    int64_t p_tick
) {
    int64_t measured = 0;
    session::ClaimImage frame;
    frame.observed_revision = p_entity->control()->revision;
    frame.state = control_state_image(p_entity, p_tick, measured);
    return frame.state.is_empty() ? PackedByteArray()
                                  : session::frame_write(frame);
}

bool NetwMultiplayer::control_claim_image_admits(
    const NetwEntity *p_entity,
    int64_t p_sender,
    uint64_t p_observed
) {
    return p_sender != 0 && p_sender == p_entity->steering()
        && p_sender != p_entity->local_peer()
        && p_observed + 1 >= p_entity->control()->tenure;
}

void NetwMultiplayer::control_install_claim_image(
    NetwEntity *p_entity,
    const PackedByteArray &p_state
) {
    const int64_t installed = control_install_final_image(
        p_entity,
        p_state,
        p_entity->get_record()->get_control_book().claim_image_tick
    );
    NetwEntityRecord::ControlBook &book
        = p_entity->get_record()->get_control_book();
    if (installed > book.claim_image_tick) {
        book.claim_image_tick = installed;
    }
}

void NetwMultiplayer::control_install_held_claim_image(
    NetwEntity *p_entity,
    bool p_tenure_changed
) {
    NetwEntityRecord::ControlBook &book
        = p_entity->get_record()->get_control_book();
    if (p_tenure_changed) {
        book.claim_image_tick = -1;
    }
    if (p_entity->steering() == p_entity->local_peer()) {
        book.held_claim_image = NetwEntityRecord::HeldClaimImage();
        if (p_tenure_changed) {
            control_start_claim_images(p_entity);
        }
        return;
    }
    if (book.held_claim_image.state.is_empty()) {
        return;
    }
    const NetwEntityRecord::HeldClaimImage held = book.held_claim_image;
    book.held_claim_image = NetwEntityRecord::HeldClaimImage();
    NetwMultiplayer *core = p_entity->session_core();
    const int64_t now = core != nullptr ? core->clock_get_tick() : -1;
    const int64_t window = core != nullptr
        ? int64_t(core->clock_get_param(NetwMultiplayer::CLOCK_PARAM_TICKRATE))
        : 0;
    if (now - held.received_tick > window
        || !control_claim_image_admits(
            p_entity,
            held.sender,
            held.observed_revision
        )) {
        return;
    }
    control_install_claim_image(p_entity, held.state);
}

void NetwMultiplayer::control_handle_claim_image(
    NetwEntity *p_entity,
    int64_t p_sender,
    const session::ClaimImage &p_image
) {
    if (p_image.state.is_empty() || p_sender == p_entity->local_peer()) {
        return;
    }
    if (control_claim_image_admits(
            p_entity,
            p_sender,
            p_image.observed_revision
        )) {
        control_install_claim_image(p_entity, p_image.state);
        return;
    }
    if (p_entity->steering() == p_sender) {
        return;
    }
    NetwMultiplayer *core = p_entity->session_core();
    NetwEntityRecord::HeldClaimImage &held_claim_image
        = p_entity->get_record()->get_control_book().held_claim_image;
    held_claim_image.sender = p_sender;
    held_claim_image.observed_revision = p_image.observed_revision;
    held_claim_image.received_tick
        = core != nullptr ? core->clock_get_tick() : -1;
    held_claim_image.state = p_image.state;
}

LocalVector<PropertyBindingPtr> NetwMultiplayer::control_authored_bindings(
    NetwEntity *p_entity
) {
    LocalVector<PropertyBindingPtr> out;
    NetwMultiplayer *core = p_entity->session_core();
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr || core == nullptr) {
        return out;
    }
    const int64_t live = core->liveness_route_of(p_entity);
    const LocalVector<PropertyBindingPtr> derived
        = plane->derived_group(live > 0 ? live : p_entity->get_route());
    for (const PropertyBindingPtr &binding : derived) {
        if (binding != nullptr && binding->get_set() != nullptr
            && repl::record_follows_tenure(binding->get_set()->record)) {
            out.push_back(binding);
        }
    }
    return out;
}

void NetwMultiplayer::control_moved(NetwEntity *p_entity) {
    NetwMultiplayer *core = p_entity->session_core();
    if (core != nullptr) {
        core->store_resource_entity(p_entity);
        core->sim_follow_steering(p_entity);
    }
}

void NetwMultiplayer::control_follow_claim(
    NetwEntity *p_entity,
    bool p_was_ahead
) {
    control_moved(p_entity);
    const bool ahead = p_entity->is_claim_running_ahead();
    if (ahead == p_was_ahead) {
        return;
    }
    NetwMultiplayer *core = p_entity->session_core();
    const LocalVector<PropertyBindingPtr> bindings
        = control_authored_bindings(p_entity);
    for (uint32_t at = 0; at < bindings.size() && core != nullptr; ++at) {
        const PropertyBindingPtr &binding = bindings[at];
        if (ahead) {
            core->store_retain(binding);
        } else if (!p_entity->is_controller_here()) {
            core->store_reinstall(binding);
        }
    }
    if (core != nullptr) {
        core->sim_follow_claim(Ref<NetwEntity>(p_entity));
    }
    if (ahead) {
        control_start_claim_images(p_entity);
    }
}

void NetwMultiplayer::control_send_op(
    NetwEntity *p_entity,
    const entity::Control::Pending &p_op,
    int64_t p_successor,
    const PackedByteArray &p_final_state,
    int64_t p_source_route
) {
    NetwMultiplayer *core = p_entity->session_core();
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr || core == nullptr) {
        return;
    }
    core->entity_settle_captured(p_entity);
    session::ControlRequest request;
    request.source_route = uint64_t(MAX(p_source_route, int64_t(0)));
    request.op = p_op.op;
    request.kind = uint8_t(p_op.kind);
    request.hold = uint8_t(p_op.hold);
    request.observed_revision = p_entity->control()->revision;
    request.issued_tick = uint64_t(MAX(p_op.issued_tick, int64_t(0)));
    request.successor = uint64_t(MAX(p_successor, int64_t(0)));
    request.final_state = p_final_state;
    plane->request_control(p_entity, request);
    plane->watch_control(p_entity);
}

Ref<NetwPromise> NetwMultiplayer::control_take_claim(
    NetwEntity *p_entity,
    uint64_t p_op
) {
    LocalVector<NetwEntityRecord::ControlClaim> &control_claims
        = p_entity->get_record()->get_control_book().control_claims;
    for (uint32_t at = 0; at < control_claims.size(); ++at) {
        if (control_claims[at].op == p_op) {
            const Ref<NetwPromise> promise = control_claims[at].promise;
            control_claims.remove_at(at);
            return promise;
        }
    }
    return Ref<NetwPromise>();
}

void NetwMultiplayer::control_settle(
    NetwEntity *p_entity,
    uint64_t p_op,
    entity::Control::Outcome p_outcome
) {
    const bool was_ahead = p_entity->is_claim_running_ahead();
    entity::Control::Pending taken;
    if (p_entity->control()->take_pending(p_op, taken)) {
        const Ref<NetwPromise> promise = control_take_claim(p_entity, p_op);
        control_follow_claim(p_entity, was_ahead);
        control_settle_claim(p_entity, promise, taken, p_outcome);
        return;
    }
    if (p_entity->control()->take_abandoned(p_op)
        && p_outcome == entity::Control::Outcome::GRANTED
        && p_entity->steering() == p_entity->local_peer()) {
        control_release(p_entity, entity::Control::SERVER);
    }
}

void NetwMultiplayer::control_settle_claim(
    NetwEntity *p_entity,
    const Ref<NetwPromise> &p_promise,
    const entity::Control::Pending &p_taken,
    entity::Control::Outcome p_outcome
) {
    if (p_promise.is_null()) {
        return;
    }
    const bool nothing_to_release
        = p_taken.kind == entity::Control::OpKind::RELEASE
        && p_outcome == entity::Control::Outcome::UNAUTHORIZED
        && p_entity->steering() != p_entity->local_peer();
    if (p_outcome == entity::Control::Outcome::GRANTED || nothing_to_release) {
        p_promise->resolve(Ref<NetwEntity>(p_entity));
        return;
    }
    p_promise->reject(
        p_outcome == entity::Control::Outcome::UNAVAILABLE ? ERR_UNAVAILABLE
                                                           : ERR_UNAUTHORIZED,
        control_refusal_detail(p_entity, p_taken.hold)
    );
}

bool NetwMultiplayer::control_expire(
    NetwEntity *p_entity,
    int64_t p_tick,
    int64_t p_deadline
) {
    if (p_entity->get_owner() == nullptr) {
        control_abandon_claims(p_entity);
        return false;
    }
    LocalVector<uint64_t> expired;
    for (uint32_t at = 0; at < p_entity->control()->pending.size(); ++at) {
        const entity::Control::Pending &held = p_entity->control()->pending[at];
        if (p_tick - held.issued_tick >= p_deadline) {
            expired.push_back(held.op);
        }
    }
    if (expired.is_empty()) {
        return p_entity->control()->has_pending();
    }
    const bool was_ahead = p_entity->is_claim_running_ahead();
    LocalVector<Ref<NetwPromise>> timed_out;
    for (uint32_t at = 0; at < expired.size(); ++at) {
        entity::Control::Pending taken;
        if (!p_entity->control()->take_pending(expired[at], taken)) {
            continue;
        }
        if (taken.kind == entity::Control::OpKind::REQUEST) {
            p_entity->control()->abandon(taken.op);
        }
        const Ref<NetwPromise> promise = control_take_claim(p_entity, taken.op);
        if (promise.is_valid()) {
            timed_out.push_back(promise);
        }
    }
    control_follow_claim(p_entity, was_ahead);
    for (const Ref<NetwPromise> &promise : timed_out) {
        promise->reject(
            ERR_TIMEOUT,
            vformat(
                "no control decision on '%s' arrived within %d ticks",
                String(p_entity->get_name()),
                p_deadline
            )
        );
    }
    return p_entity->control()->has_pending();
}

void NetwMultiplayer::control_seed(
    NetwEntity *p_entity,
    uint64_t p_revision,
    uint64_t p_tenure,
    int64_t p_hold
) {
    p_entity->control()->seed(p_revision, p_tenure, p_hold);
    control_moved(p_entity);
}

void NetwMultiplayer::control_handle_request(
    NetwEntity *p_entity,
    int64_t p_sender,
    const session::ControlRequest &p_request
) {
    Node *owner = p_entity->get_owner();
    if (owner == nullptr || !owner->is_inside_tree()
        || !p_entity->is_on_server()) {
        fault::raise(
            p_entity->session_core(),
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_sender},
            owner != nullptr ? uint64_t(gd::instance_id(owner)) : 0
        );
        return;
    }
    int64_t requester = p_sender;
    const Ref<MultiplayerAPI> api = owner->get_multiplayer();
    if (requester == 0 && api.is_valid()
        && api->get_multiplayer_peer().is_valid()) {
        requester = api->get_unique_id();
    }
    if (p_request.kind == uint8_t(entity::Control::OpKind::RELEASE)) {
        control_decide_release(
            p_entity,
            requester,
            p_request.op,
            int64_t(p_request.successor),
            p_request.final_state
        );
        return;
    }
    control_decide_request(
        p_entity,
        requester,
        p_request.op,
        int64_t(p_request.hold),
        int64_t(p_request.source_route)
    );
}

entity::Control::Outcome NetwMultiplayer::control_decide_request(
    NetwEntity *p_entity,
    int64_t p_requester,
    uint64_t p_op,
    int64_t p_hold,
    int64_t p_source_route
) {
    using Outcome = entity::Control::Outcome;
    using Ruling = entity::Control::Ruling;
    const Ruling ruling
        = p_entity->control()->rule(p_requester, p_hold, p_entity->steering());
    Outcome outcome = Outcome::GRANTED;
    if (ruling == Ruling::UNAVAILABLE
        || control_immediate_unavailable(p_entity)) {
        fault::raise(
            p_entity->session_core(),
            NETW_FAULT_CONTROL_REFUSED,
            {String(p_entity->get_name()), String(), String(), p_requester},
            uint64_t(gd::instance_id(p_entity->get_owner()))
        );
        outcome = Outcome::UNAVAILABLE;
    } else if (ruling == Ruling::EXCLUDED) {
        outcome = Outcome::UNAUTHORIZED;
    } else if (!control_controls_source(
                   p_entity,
                   p_requester,
                   p_source_route
               )) {
        outcome = Outcome::UNAUTHORIZED;
    } else if (
        ruling == Ruling::ASK_FILTER
        && p_entity->get_record()
                ->admit_control_request(p_entity, p_requester, p_hold)
            == 0
    ) {
        outcome = Outcome::UNAUTHORIZED;
    }
    NetwMultiplayer *core = p_entity->session_core();
    if (core != nullptr) {
        const Error ruled = outcome == Outcome::GRANTED ? OK
            : outcome == Outcome::UNAVAILABLE           ? ERR_UNAVAILABLE
                                                        : ERR_UNAUTHORIZED;
        core->event_record(
            EventPlane::CONTROL_REQUESTED,
            p_entity->get_rid_handle(),
            p_entity->get_route(),
            p_requester,
            ruled
        );
    }
    if (outcome == Outcome::GRANTED) {
        control_decide(p_entity, p_requester, p_hold, p_requester, p_op);
    } else {
        control_refuse(p_entity, p_requester, p_op, outcome);
    }
    return outcome;
}

entity::Control::Outcome NetwMultiplayer::control_decide_release(
    NetwEntity *p_entity,
    int64_t p_requester,
    uint64_t p_op,
    int64_t p_successor,
    const PackedByteArray &p_final_state
) {
    using Outcome = entity::Control::Outcome;
    if (p_entity->steering() != p_requester) {
        control_refuse(p_entity, p_requester, p_op, Outcome::UNAUTHORIZED);
        return Outcome::UNAUTHORIZED;
    }
    if (!control_holds_copy(p_entity, p_successor)) {
        control_refuse(p_entity, p_requester, p_op, Outcome::UNAVAILABLE);
        return Outcome::UNAVAILABLE;
    }
    const int64_t steering = control_steerer_of_peer(p_entity, p_successor);
    control_decide(
        p_entity,
        steering,
        entity::Control::HOLD_NONE,
        p_requester,
        p_op,
        p_final_state
    );
    control_note_host_gift(p_entity, p_successor, steering);
    return Outcome::GRANTED;
}

void NetwMultiplayer::control_refuse(
    NetwEntity *p_entity,
    int64_t p_requester,
    uint64_t p_op,
    entity::Control::Outcome p_outcome
) {
    NetwMultiplayer *core = p_entity->session_core();
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (p_op == 0 || plane == nullptr) {
        return;
    }
    session::ControlApply reply;
    reply.controller = uint64_t(MAX(p_entity->steering(), int64_t(0)));
    reply.revision = p_entity->control()->revision;
    reply.hold = uint8_t(p_entity->control()->hold);
    reply.op = p_op;
    reply.outcome = uint8_t(p_outcome);
    reply.kept = core != nullptr
        && core->player_kept_despawns(p_entity->get_rid_handle().get_id());
    plane->reply_control(p_entity, reply, p_requester);
}

void NetwMultiplayer::control_restate(NetwEntity *p_entity, int64_t p_peer) {
    NetwMultiplayer *core = p_entity->session_core();
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr || core == nullptr) {
        return;
    }
    session::ControlApply restated;
    restated.controller = uint64_t(MAX(p_entity->steering(), int64_t(0)));
    restated.revision = p_entity->control()->revision;
    restated.hold = uint8_t(p_entity->control()->hold);
    restated.kept
        = core->player_kept_despawns(p_entity->get_rid_handle().get_id());
    plane->reply_control(p_entity, restated, p_peer);
}

void NetwMultiplayer::control_decide(
    NetwEntity *p_entity,
    int64_t p_peer,
    int64_t p_hold,
    int64_t p_issuer,
    uint64_t p_op,
    const PackedByteArray &p_final_state,
    bool p_sends_owner
) {
    const bool tenure_changed = p_entity->control()->decide(p_peer, p_hold);
    control_moved(p_entity);
    control_adopt(p_entity, p_peer);
    NetwMultiplayer *core = p_entity->session_core();
    if (tenure_changed && core != nullptr) {
        core->row_streams_follow_tenure(
            p_entity->get_route(),
            p_entity->control()->tenure
        );
    }
    control_install_final_image(p_entity, p_final_state);
    control_install_held_claim_image(p_entity, tenure_changed);
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr) {
        return;
    }
    session::ControlApply decision;
    decision.controller = uint64_t(MAX(p_peer, int64_t(0)));
    decision.revision = p_entity->control()->revision;
    decision.tenure_changed = tenure_changed;
    decision.hold = uint8_t(p_entity->control()->hold);
    decision.op = p_op;
    decision.outcome = uint8_t(entity::Control::Outcome::GRANTED);
    decision.final_state = p_final_state;
    decision.has_owner = p_sends_owner;
    decision.kept = core != nullptr
        && core->player_kept_despawns(p_entity->get_rid_handle().get_id());
    decision.peer = uint64_t(MAX(p_entity->get_peer_id(), int64_t(0)));
    decision.player_id = uint64_t(MAX(p_entity->get_player_id(), int64_t(0)));
    plane->broadcast_control(p_entity, decision, p_issuer);
}

void NetwMultiplayer::control_change(NetwEntity *p_entity, int64_t p_peer) {
    control_decide(p_entity, p_peer, entity::Control::HOLD_NONE, 0, 0);
}

void NetwMultiplayer::control_reassign(
    NetwEntity *p_entity,
    int64_t p_peer,
    int64_t p_player_id,
    int64_t p_controller
) {
    p_entity->set_peer_id(p_peer);
    p_entity->set_player_id(p_player_id);
    p_entity->unwrite_player();
    control_decide(
        p_entity,
        p_controller,
        entity::Control::HOLD_NONE,
        0,
        0,
        PackedByteArray(),
        true
    );
}

void NetwMultiplayer::control_revert_to_server(
    NetwEntity *p_entity,
    int64_t p_departed
) {
    if (p_entity->get_peer_id() != p_departed) {
        control_change(p_entity, entity::Control::SERVER);
        return;
    }
    const int64_t controller = p_entity->control()->resolve();
    control_reassign(
        p_entity,
        0,
        0,
        controller == p_departed ? int64_t(entity::Control::SERVER) : controller
    );
}

void NetwMultiplayer::control_raise(NetwEntity *p_entity, uint64_t p_floor) {
    entity::Control *held = p_entity->control();
    held->revision = MAX(held->revision, p_floor) + 1;
    held->tenure = held->revision;
    control_moved(p_entity);
    NetwMultiplayer *core = p_entity->session_core();
    if (core != nullptr) {
        core->row_streams_follow_tenure(p_entity->get_route(), held->tenure);
    }
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr) {
        return;
    }
    session::ControlApply decision;
    decision.controller = uint64_t(MAX(p_entity->steering(), int64_t(0)));
    decision.revision = held->revision;
    decision.tenure_changed = true;
    decision.hold = uint8_t(held->hold);
    decision.kept = core != nullptr
        && core->player_kept_despawns(p_entity->get_rid_handle().get_id());
    plane->broadcast_control(p_entity, decision, 0);
}

void NetwMultiplayer::control_handle_apply(
    NetwEntity *p_entity,
    const session::ControlApply &p_applied
) {
    NetwMultiplayer *core = p_entity->session_core();
    if (core != nullptr) {
        core->player_hear_kept(
            p_entity->get_rid_handle().get_id(),
            p_applied.kept
        );
    }
    if (p_entity->control()->install(
            p_applied.revision,
            p_applied.tenure_changed,
            int64_t(p_applied.hold)
        )) {
        if (p_applied.has_owner) {
            p_entity->set_peer_id(int64_t(p_applied.peer));
            p_entity->set_player_id(int64_t(p_applied.player_id));
            p_entity->unwrite_player();
        }
        control_moved(p_entity);
        control_adopt(p_entity, int64_t(p_applied.controller));
        if (p_applied.tenure_changed && core != nullptr) {
            core->row_streams_follow_tenure(
                p_entity->get_route(),
                p_entity->control()->tenure
            );
        }
        control_install_final_image(p_entity, p_applied.final_state);
        control_install_held_claim_image(p_entity, p_applied.tenure_changed);
    }
    if (p_applied.op != 0) {
        control_settle(
            p_entity,
            p_applied.op,
            entity::Control::Outcome(p_applied.outcome)
        );
    }
}

void NetwMultiplayer::control_abandon_claims(NetwEntity *p_entity) {
    control_reject_claims(
        p_entity,
        "'%s' was freed with a control request outstanding"
    );
}

void NetwMultiplayer::control_reject_claims(
    NetwEntity *p_entity,
    const char *p_detail
) {
    LocalVector<NetwEntityRecord::ControlClaim> &control_claims
        = p_entity->get_record()->get_control_book().control_claims;
    LocalVector<NetwEntityRecord::ControlClaim> held;
    held.reserve(control_claims.size());
    for (uint32_t at = 0; at < control_claims.size(); ++at) {
        held.push_back(control_claims[at]);
    }
    const bool was_ahead = p_entity->is_claim_running_ahead();
    control_claims.clear();
    p_entity->control()->pending.clear();
    control_follow_claim(p_entity, was_ahead);
    for (uint32_t at = 0; at < held.size(); ++at) {
        held[at].promise->reject(
            ERR_UNAVAILABLE,
            vformat(p_detail, String(p_entity->get_name()))
        );
    }
}

void NetwMultiplayer::entity_arm(
    NetwEntity *p_entity,
    const Ref<NetwMultiplayer> &p_api
) {
    const Ref<NetwEntity> keep(p_entity);
    NETW_BUG_COND(p_entity->get_stage() != int64_t(entity::Stage::UNBOUND));
    NetwEntity::settle_parts(p_entity->get_owner(), false);
    p_entity->unwrite_player();
    p_entity->stamp_multiplayer(p_api);
    Node *claimed = p_entity->get_owner();
    if (claimed != nullptr && !p_entity->control()->get_configured()
        && claimed->get_multiplayer_authority() != SERVER_PEER) {
        p_entity->control()->infer_authority(
            int64_t(NetwEntity::AUTHORITY_CONTROLLER)
        );
    }
    if (p_entity->get_transfer() == NetwEntity::TRANSFER_IMMEDIATE) {
        p_entity->control()->infer_authority(
            int64_t(NetwEntity::AUTHORITY_CONTROLLER)
        );
    }
    if (p_entity->get_transfer() == NetwEntity::TRANSFER_IMMEDIATE
        && !p_entity->control()->authority_follows_controller()) {
        fault::raise(
            p_entity->session_core(),
            NETW_FAULT_CONFLICTING_SETTINGS,
            {String("{NetwEntity.TRANSFER_IMMEDIATE}"),
             String(p_entity->get_name()),
             String("{NetwEntity.AUTHORITY_SERVER}")},
            uint64_t(gd::instance_id(p_entity->get_owner()))
        );
        p_entity->control()->set_transfer(
            int64_t(NetwEntity::TRANSFER_REQUESTABLE)
        );
    }
    if (!p_entity->control()->get_configured()) {
        control_adopt(p_entity, control_default_controller(p_entity));
    } else {
        control_apply(p_entity);
    }
    p_entity->transition(int64_t(entity::Stage::ARMED));
}

void NetwMultiplayer::control_receive_request(
    const RID &p_entity,
    int64_t p_sender,
    const session::ControlRequest &p_request
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_valid()) {
        control_handle_request(entity.ptr(), p_sender, p_request);
    }
}

void NetwMultiplayer::control_receive_apply(
    const RID &p_entity,
    const session::ControlApply &p_applied
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_valid()) {
        control_handle_apply(entity.ptr(), p_applied);
    }
}

void NetwMultiplayer::control_receive_claim_image(
    const RID &p_entity,
    int64_t p_sender,
    const session::ClaimImage &p_image
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_valid()) {
        control_handle_claim_image(entity.ptr(), p_sender, p_image);
    }
}

} // namespace netw
