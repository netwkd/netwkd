#include "netw/api/netw_multiplayer.hpp"

#include "godot/multiplayer.hpp"
#include "godot/object.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/auth_protocol.hpp"
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

void write_authority(Node *p_node, int64_t p_to, int64_t p_prior) {
    const int64_t now = p_node->get_multiplayer_authority();
    if (now == p_to) {
        return;
    }
    if (now != p_prior) {
        fault::raise(
            NetwMultiplayer::session_holding(p_node),
            NETW_FAULT_AUTHORITY_OVERRIDDEN,
            {String(p_node->get_name()), String(), String(), 0, now, p_to},
            gd::instance_id(p_node)
        );
    }
    p_node->set_multiplayer_authority(int(p_to), false);
}

void project_node(
    Node *p_owner,
    Node *p_node,
    int64_t p_follower,
    int64_t p_prior_follower,
    uint64_t p_watch
) {
    if (p_node != p_owner && tree::is_entity_root(p_node)) {
        return;
    }
    write_authority(p_node, p_follower, p_prior_follower);
    const bool watched = p_follower != SERVER_PEER;
    if (watched && p_watch != 0) {
        tree::watch(p_node, p_watch);
    }
    const int children = p_node->get_child_count(true);
    for (int at = 0; at < children; ++at) {
        project_node(
            p_owner,
            p_node->get_child(at, true),
            p_follower,
            p_prior_follower,
            p_watch
        );
    }
}

LocalVector<NetwEntityRecord::ControlClaim> take_rejected_control_claims(
    NetwEntityRecord &p_record
) {
    LocalVector<NetwEntityRecord::ControlClaim> held
        = std::move(p_record.get_control_book().control_claims);
    p_record.get_control()->pending.clear();
    return held;
}

void reject_control_claims(
    const LocalVector<NetwEntityRecord::ControlClaim> &p_claims,
    const NetwEntityRecord &p_record,
    const char *p_detail
) {
    for (const NetwEntityRecord::ControlClaim &claim : p_claims) {
        claim.promise->reject(
            ERR_UNAVAILABLE,
            vformat(p_detail, String(p_record.get_entity_id()))
        );
    }
}

} // namespace

void NetwMultiplayer::control_structure_issue(
    NetwEntityRecord &p_record,
    uint64_t p_base,
    const Ref<NetwPromise> &p_promise
) {
    NetwEntityRecord::StructureOp issued;
    issued.base = p_base;
    issued.promise = p_promise;
    p_record.get_control_book().structure_ops.push_back(issued);
}

void NetwMultiplayer::control_structure_accept(
    NetwEntityRecord &p_record,
    uint64_t p_revision
) {
    LocalVector<NetwEntityRecord::StructureOp> &structure_ops
        = p_record.get_control_book().structure_ops;
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
    NetwEntityRecord &p_record,
    uint64_t p_base,
    Error p_code,
    const String &p_detail
) {
    bool refused = false;
    LocalVector<NetwEntityRecord::StructureOp> &structure_ops
        = p_record.get_control_book().structure_ops;
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

Error NetwEntity::transfer_control_to(int64_t p_steering) {
    if (get_stage() == int64_t(entity::Stage::UNBOUND)) {
        fault::raise(
            session_core(),
            NETW_FAULT_NOT_RUNNING,
            {String("Giving '") + String(get_name())
                 + String("' to a peer"),
             String(),
             String("the entity")},
            uint64_t(gd::instance_id(get_owner()))
        );
        return ERR_UNCONFIGURED;
    }
    if (!is_on_server()) {
        fault::raise(
            session_core(),
            NETW_FAULT_SERVER_ONLY,
            {String("{NetwEntity.controller}")},
            uint64_t(gd::instance_id(get_owner()))
        );
        return ERR_UNAUTHORIZED;
    }
    if (home != nullptr) {
        home->control_change(attached, p_steering);
    } else {
        decide_control_here(p_steering, entity::Control::HOLD_NONE, 0, 0);
    }
    return OK;
}

Error NetwMultiplayer::control_give(const RID &p_entity, int64_t p_peer) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    return view->give_control_here(p_peer);
}

Error NetwEntity::give_control_here(int64_t p_peer) {
    if (p_peer <= 0) {
        fault::raise(
            session_core(),
            NETW_FAULT_BAD_VALUE,
            {String("{NetwEntity.controller}"),
             String::num_int64(p_peer),
             String("a peer id, or 1 for the server")},
            uint64_t(gd::instance_id(get_owner()))
        );
        return ERR_INVALID_PARAMETER;
    }
    const int64_t steering
        = NetwMultiplayer::control_steerer_of_peer(session_core(), p_peer);
    const Error given = transfer_control_to(steering);
    if (given == OK) {
        note_control_host_gift(p_peer, steering);
    }
    return given;
}

void NetwMultiplayer::control_note_host_gift(
    const RID &p_entity,
    int64_t p_peer,
    int64_t p_steering,
    uint64_t p_revision
) {
    if (p_peer != SERVER_PEER || p_steering != entity::Control::SERVER) {
        return;
    }
    HostGift gift;
    gift.entity = p_entity;
    gift.revision = p_revision;
    control_host_gifts.insert(p_entity.get_id(), gift);
}

void NetwEntity::note_control_host_gift(int64_t p_peer, int64_t p_steering) {
    if (NetwMultiplayer *core = session_core()) {
        core->control_note_host_gift(
            get_rid_handle(),
            p_peer,
            p_steering,
            control()->revision
        );
    }
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
        entity->transfer_control_to(SERVER_PEER);
    }
}

int64_t NetwMultiplayer::control_steerer_of_peer(
    const NetwMultiplayer *p_session,
    int64_t p_peer
) {
    if (p_peer == SERVER_PEER && p_session != nullptr
        && p_session->player_has(SERVER_PEER)) {
        return SERVER_PEER;
    }
    return entity::Control::from_public(p_peer);
}

Error NetwMultiplayer::control_set_param(
    const RID &p_entity,
    EntityParam p_param,
    const Variant &p_value
) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    return view->set_control_param(p_param, p_value);
}

Error NetwEntity::set_control_param(int64_t p_param, const Variant &p_value) {
    entity::Control *settings = control();
    const uint64_t subject = uint64_t(gd::instance_id(get_owner()));
    switch (p_param) {
        case NetwMultiplayer::ENTITY_PARAM_AUTHORITY: {
            const int64_t value = int64_t(p_value);
            if (value != NetwEntity::AUTHORITY_SERVER
                && value != NetwEntity::AUTHORITY_CONTROLLER) {
                fault::raise(
                    session_core(),
                    NETW_FAULT_BAD_VALUE,
                    {String("{NetwEntity.authority}"),
                     String::num_int64(value),
                     String("{NetwEntity.AUTHORITY_SERVER} or "
                            "{NetwEntity.AUTHORITY_CONTROLLER}")},
                    subject
                );
                return ERR_INVALID_PARAMETER;
            }
            if (get_stage() != int64_t(entity::Stage::UNBOUND)) {
                fault::raise(
                    session_core(),
                    NETW_FAULT_TOO_LATE,
                    {String("{NetwEntity.authority}"),
                     String("'") + String(get_name()) + String("'"),
                     String("the node enters the tree")},
                    subject
                );
                return ERR_LOCKED;
            }
            settings->set_authority(value);
            return OK;
        }
        case NetwMultiplayer::ENTITY_PARAM_TRANSFER: {
            const int64_t value = int64_t(p_value);
            if (value == NetwEntity::TRANSFER_IMMEDIATE
                && !settings->authority_follows_controller()
                && get_stage() != int64_t(entity::Stage::UNBOUND)) {
                fault::raise(
                    session_core(),
                    NETW_FAULT_SETTING_REFUSED,
                    {String("{NetwEntity.TRANSFER_IMMEDIATE} on '")
                         + String(get_name()) + String("'"),
                     String("{NetwEntity.AUTHORITY_SERVER}"),
                     String("{NetwEntity.AUTHORITY_CONTROLLER} before the "
                            "node enters the tree")},
                    subject
                );
                return ERR_UNAVAILABLE;
            }
            settings->set_transfer(value);
            NetwMultiplayer *core = session_core();
            if (core != nullptr) {
                core->sim_mark(get_rid_handle());
                core->sim_follow_steering(get_rid_handle());
            }
            return OK;
        }
        case NetwMultiplayer::ENTITY_PARAM_ON_CONTROLLER_DISCONNECT:
            settings->set_on_disconnect(int64_t(p_value));
            return OK;
        case NetwMultiplayer::ENTITY_PARAM_ON_PARENT_DESPAWN:
            settings->set_on_parent_despawn(int64_t(p_value));
            return OK;
        case NetwMultiplayer::ENTITY_PARAM_REPLICATION_PRIORITY: {
            const double weight = double(p_value);
            if (!(weight > 0.0)) {
                fault::raise(
                    session_core(),
                    NETW_FAULT_BAD_VALUE,
                    {String("{NetwEntity.replication_priority}"),
                     String::num(weight),
                     String("a weight above 0, or {NetwEntity.interest} to "
                            "hide the entity from peers")},
                    subject
                );
                return ERR_INVALID_PARAMETER;
            }
            get_record()->set_replication_priority(weight);
            refresh_control();
            return OK;
        }
        case NetwMultiplayer::ENTITY_PARAM_HOLD:
            fault::raise(
                session_core(),
                NETW_FAULT_UNSUPPORTED,
                {String("{NetwMultiplayer.entity_set_param}"),
                 String("{NetwMultiplayer.ENTITY_PARAM_HOLD}"),
                 String("{NetwMultiplayer.entity_request_control}")}
            );
            return ERR_UNAVAILABLE;
        default:
            fault::raise(
                session_core(),
                NETW_FAULT_BAD_VALUE,
                {"An entity setting",
                 vformat("setting %d", int(p_param)),
                 "an entity setting that exists"}
            );
            return ERR_INVALID_PARAMETER;
    }
}

void NetwMultiplayer::control_record(const RID &p_entity, int64_t p_value) {
    NetwEntityRecord *const record = entity_record(p_entity);
    const int64_t was = record->get_control()->get_controller();
    if (record->set_controller(p_value)) {
        control_announce(p_entity, was, p_value);
    }
}

void NetwEntity::write_controller(int64_t p_peer) {
    if (home != nullptr) {
        home->control_record(attached, p_peer);
        return;
    }
    const int64_t was = draft->get_control()->get_controller();
    if (draft->set_controller(p_peer)) {
        emit_control_change(was, p_peer);
    }
}

void NetwMultiplayer::control_follow(const RID &p_entity) {
    predict_follow_control(p_entity);
    sim_follow_control(p_entity);
    display_follow_control(p_entity);
}

void NetwMultiplayer::control_announce(
    const RID &p_entity,
    int64_t p_was,
    int64_t p_peer
) {
    views.holder_of(p_entity.get_id())->emit_control_change(p_was, p_peer);
}

void NetwEntity::emit_control_change(int64_t p_was, int64_t p_peer) {
    refresh_control();
    session::Announcement row;
    row.kind = session::AnnouncementKind::CONTROL_CHANGED;
    row.from = entity::Control::public_peer(p_was);
    row.to = entity::Control::public_peer(p_peer);
    get_record()->set_announced_steerer(row.to);
    NetwMultiplayer::entity_announce(this, row);
    NetwMultiplayer *core = session_core();
    if (core == nullptr) {
        return;
    }
    core->event_record(
        EventPlane::CONTROL_CHANGED,
        get_rid_handle(),
        get_route(),
        row.to,
        OK,
        row.from,
        row.to
    );
}

void NetwEntity::apply_control(bool p_announce) {
    if (home != nullptr) {
        home->control_project(attached, p_announce);
    } else {
        project_control(p_announce);
    }
}

void NetwMultiplayer::control_project(const RID &p_entity, bool p_announce) {
    views.holder_of(p_entity.get_id())->project_control(p_announce);
}

void NetwEntity::project_control(bool p_announce) {
    NetwMultiplayer *api = session_core();
    NetwEntityRecord &record = *get_record();
    if (Node *owner = get_owner()) {
        const entity::Control &control = *record.get_control();
        const int64_t steerer
            = entity::Control::public_peer(control.get_controller());
        const int64_t follower
            = control.authority_follows_controller() ? steerer : SERVER_PEER;
        const int64_t prior = record.get_projected_follower() != 0
            ? record.get_projected_follower()
            : SERVER_PEER;
        project_node(
            owner,
            owner,
            follower,
            prior,
            uint64_t(get_instance_id())
        );
        record.set_projected_follower(follower);
        const int64_t was = record.get_announced_steerer() != 0
            ? record.get_announced_steerer()
            : SERVER_PEER;
        record.set_announced_steerer(steerer);
        if (was != steerer && p_announce) {
            NETW_TRACE(
                sys::ENTITY,
                "Control moved from peer %d to peer %d.",
                int(was),
                int(steerer)
            );
            session::Announcement row;
            row.kind = session::AnnouncementKind::CONTROL_CHANGED;
            row.from = was;
            row.to = steerer;
            NetwMultiplayer::entity_announce(this, row);
        }
    }
    if (api != nullptr && get_record()->projects_to_server()) {
        api->entity_watch_authority_joins();
    }
    if (!control()->authority_follows_controller()
        && get_controller() != SERVER_PEER
        && get_owner() != nullptr
        && get_owner()->is_inside_tree()
        && streams_after_spawn(synchronizers()) && api != nullptr) {
        fault::raise(
            api,
            NETW_FAULT_CONFLICTING_SETTINGS,
            {String("NetwEntity.controller"),
             String(get_name()),
             String("NetwEntity.authority at AUTHORITY_SERVER")},
            uint64_t(gd::instance_id(get_owner()))
        );
    }
    refresh_control();
}

void NetwEntity::project_control_descendant(Node *p_node) {
    Node *owner = get_owner();
    if (p_node == nullptr || owner == nullptr) {
        return;
    }
    Node *parent = p_node->get_parent();
    if (parent != owner && !owner->is_ancestor_of(parent)) {
        tree::unwatch(parent);
        return;
    }
    const int64_t follower = get_record()->get_projected_follower();
    if (follower != 0 && owner->is_ancestor_of(p_node)) {
        project_node(
            owner,
            p_node,
            follower,
            SERVER_PEER,
            uint64_t(get_instance_id())
        );
    }
    refresh_control();
}

void NetwMultiplayer::control_adopt(const RID &p_entity, int64_t p_peer) {
    NetwEntityRecord *const record = entity_record(p_entity);
    const int64_t was = record->get_control()->get_controller();
    if (!record->set_controller(p_peer)) {
        control_project(p_entity, true);
        return;
    }
    control_project(p_entity, false);
    control_announce(p_entity, was, p_peer);
}

void NetwEntity::adopt_controller(int64_t p_peer) {
    if (home != nullptr) {
        home->control_adopt(attached, p_peer);
        return;
    }
    const int64_t was = draft->get_control()->get_controller();
    if (!draft->set_controller(p_peer)) {
        project_control(true);
        return;
    }
    project_control(false);
    emit_control_change(was, p_peer);
}

int64_t NetwMultiplayer::control_default_controller(
    const NetwEntityRecord &p_record,
    const Node *p_owner
) {
    const int64_t resolved = p_record.get_control()->resolve();
    if (resolved != entity::Control::SERVER
        || !p_record.get_control()->authority_follows_controller()) {
        return resolved;
    }
    const int64_t claimed = p_owner != nullptr
        ? int64_t(p_owner->get_multiplayer_authority())
        : SERVER_PEER;
    return entity::Control::from_public(claimed);
}

void NetwMultiplayer::control_peer_departed(
    const RID &p_entity,
    int64_t p_peer
) {
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity);
    Node *owner = entity.is_valid() ? entity->get_owner() : nullptr;
    if (owner == nullptr || !owner->is_inside_tree()
        || !entity->is_on_server()) {
        return;
    }
    const entity::Control::Verdict verdict
        = entity->control()->disconnect_verdict(
            p_peer,
            entity->get_peer_id()
        );
    NetwMultiplayer *core = entity->session_core();
    if (verdict != entity::Control::NOTHING && core != nullptr
        && core->player_keep_entity(entity->get_rid_handle(), p_peer)) {
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
            entity->despawn(
                NetwDespawnOpts::create(StringName("peer_disconnected"))
            );
        } break;
        case entity::Control::REVERT_TO_SERVER:
            control_change(p_entity, entity::Control::SERVER);
            break;
        case entity::Control::DESPAWN_CONTROLLER:
            entity->despawn(
                NetwDespawnOpts::create(StringName("controller_disconnected"))
            );
            break;
        default:
            break;
    }
}

bool NetwMultiplayer::control_rename_peers(
    NetwEntityRecord &p_record,
    const PeerRename &p_rename
) {
    entity::Control *held = p_record.get_control();
    const int64_t peer = p_record.get_peer_id();
    const int64_t controller = held->controller;
    p_record.set_peer_id(p_rename.of(peer));
    held->controller = p_rename.of(controller);
    return p_record.get_peer_id() != peer
        || held->controller != controller;
}

void NetwMultiplayer::control_follow_rename(const RID &p_entity) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (view.is_null()) {
        return;
    }
    const int64_t stage = view->get_stage();
    if (stage == int64_t(entity::Stage::ARMED)
        || stage == int64_t(entity::Stage::LIVE)) {
        view->apply_control(false);
    }
}

void NetwMultiplayer::control_forget_server_claims(const RID &p_entity) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (view.is_null()) {
        return;
    }
    NetwEntityRecord *const record = view->get_record();
    if (!record->get_control_book().control_claims.is_empty()
        || record->get_control()->has_pending()) {
        control_reject_claims(
            p_entity,
            "the server moved while '%s' waited for control"
        );
    }
    record->get_control_book().held_claim_image
        = NetwEntityRecord::HeldClaimImage();
}

bool NetwEntity::control_deciding_locally() const {
    Node *owner = get_owner();
    if (owner == nullptr || !owner->is_inside_tree()) {
        return false;
    }
    const Ref<MultiplayerAPI> api = owner->get_multiplayer();
    if (api.is_valid() && api->get_multiplayer_peer().is_null()) {
        return true;
    }
    return is_on_server();
}

bool NetwEntity::control_immediate_unavailable() {
    if (control()->get_transfer()
        != int64_t(entity::Control::Transfer::IMMEDIATE)) {
        return false;
    }
    Node *owner = get_owner();
    if (owner != nullptr && authoring::declares_prediction(owner)) {
        return true;
    }
    if (get_state_binding() != nullptr) {
        return true;
    }
    return !synchronizers().is_empty();
}

String NetwMultiplayer::control_refusal_detail(
    const NetwEntityRecord &p_record,
    int64_t p_hold
) {
    return vformat(
        "control of '%s' with hold %d was refused, peer %d holds it with "
        "hold %d",
        String(p_record.get_entity_id()),
        p_hold,
        entity::Control::public_peer(p_record.get_control()->resolve()),
        p_record.get_control()->hold
    );
}

Error NetwEntity::control_local_refusal(int64_t p_local, int64_t p_hold) {
    if (control()->pending.size() >= entity::Control::MAX_PENDING) {
        return ERR_UNAVAILABLE;
    }
    if (control_immediate_unavailable()) {
        return ERR_UNAVAILABLE;
    }
    if (control_deciding_locally()
        && !control()->admits_request()) {
        return ERR_UNAVAILABLE;
    }
    if (entity::Control::excludes(
            p_local,
            p_hold,
            steering(),
            control()->hold
        )) {
        return ERR_UNAUTHORIZED;
    }
    return OK;
}

Ref<NetwPromise> NetwMultiplayer::control_claim_by_contact(
    id::Entity p_entity,
    id::Entity p_source
) {
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity.rid);
    const Ref<NetwEntity> source = slot_pinned_entity(p_source.rid);
    NetwMultiplayer *core
        = entity.is_valid() ? entity->session_core() : nullptr;
    if (source.is_null() || core == nullptr
        || !source->get_is_controlled_locally()) {
        return NetwPromise::rejected(
            ERR_UNAUTHORIZED,
            vformat(
                "a contact claim on '%s' names a source this peer does not "
                "control",
                entity.is_valid() ? String(entity->get_name()) : String()
            )
        );
    }
    const int64_t live = core->liveness_route_of(source.ptr());
    return core->control_claim(
        entity->get_rid_handle(),
        entity::Control::HOLD_SOFT,
        live > 0 ? live : source->get_route()
    );
}

bool NetwMultiplayer::control_controls_source(
    const NetwMultiplayer *p_core,
    int64_t p_requester,
    int64_t p_source_route
) {
    if (p_source_route <= 0) {
        return true;
    }
    const Ref<NetwEntity> source = p_core != nullptr
        ? p_core->wrapper_for_route(p_source_route)
        : Ref<NetwEntity>();
    return source.is_valid() && source->steering() == p_requester;
}

Ref<NetwPromise> NetwEntity::claim_control(Hold p_hold) {
    if (home != nullptr) {
        return home->control_claim(attached, int64_t(p_hold), 0);
    }
    Node *owner = get_owner();
    const Ref<MultiplayerAPI> api = owner != nullptr && owner->is_inside_tree()
        ? owner->get_multiplayer()
        : Ref<MultiplayerAPI>();
    if (api.is_null()) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' is in no session, so no peer can decide its control",
                String(get_name())
            )
        );
    }
    const int64_t local = api->get_unique_id();
    const Error refused = control_local_refusal(local, int64_t(p_hold));
    if (refused != OK) {
        return NetwPromise::rejected(
            refused,
            NetwMultiplayer::control_refusal_detail(*draft, int64_t(p_hold))
        );
    }
    if (!control()->has_pending() && steering() == local
        && control()->hold == int64_t(p_hold)) {
        return NetwPromise::resolved(Ref<NetwEntity>(this));
    }
    if (!control_deciding_locally()) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' has no session plane to carry a control request",
                String(get_name())
            )
        );
    }
    using Ruling = entity::Control::Ruling;
    const Ruling ruling = control()->rule(local, int64_t(p_hold), steering());
    if (ruling == Ruling::UNAVAILABLE || control_immediate_unavailable()) {
        fault::raise(
            session_core(),
            NETW_FAULT_CONTROL_REFUSED,
            {String(get_name()), String(), String(), local},
            uint64_t(gd::instance_id(owner))
        );
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            NetwMultiplayer::control_refusal_detail(*draft, int64_t(p_hold))
        );
    }
    Error denied = OK;
    String reason;
    if (ruling == Ruling::EXCLUDED
        || (ruling == Ruling::ASK_FILTER
            && control_admit_request(local, int64_t(p_hold), denied, reason)
                == 0)) {
        return NetwPromise::rejected(
            denied != OK ? denied : ERR_UNAUTHORIZED,
            !reason.is_empty() ? reason
                : NetwMultiplayer::control_refusal_detail(
                      *get_record(), int64_t(p_hold)
                  )
        );
    }
    decide_control_here(local, int64_t(p_hold), local, 0);
    return NetwPromise::resolved(Ref<NetwEntity>(this));
}

Ref<NetwPromise> NetwEntity::check_control_release(
    int64_t p_successor,
    int64_t p_local
) const {
    if (p_successor < 0) {
        return NetwPromise::rejected(
            ERR_INVALID_PARAMETER,
            vformat(
                "'%s' cannot be released to peer %d. Pass a peer id, or 0 "
                "for the server",
                String(get_name()),
                p_successor
            )
        );
    }
    if (!control()->claims_pending() && steering() != p_local) {
        return NetwPromise::rejected(
            ERR_UNAUTHORIZED,
            vformat(
                "'%s' is not controlled by peer %d, so it has nothing to "
                "release",
                String(get_name()),
                p_local
            )
        );
    }
    return Ref<NetwPromise>();
}

Ref<NetwPromise> NetwEntity::control_release_missing_copy(
    int64_t p_successor
) const {
    return NetwPromise::rejected(
        ERR_UNAVAILABLE,
        vformat(
            "peer %d holds no copy of '%s', so it cannot take it on",
            p_successor,
            String(get_name())
        )
    );
}

Ref<NetwPromise> NetwEntity::release_control(int64_t p_successor) {
    if (home != nullptr) {
        return home->control_release(attached, p_successor);
    }
    const int64_t local = p_successor < 0 ? 0 : local_peer();
    const Ref<NetwPromise> refused = check_control_release(p_successor, local);
    if (refused.is_valid()) {
        return refused;
    }
    if (p_successor != entity::Control::SERVER && p_successor != SERVER_PEER) {
        return control_release_missing_copy(p_successor);
    }
    if (!control_deciding_locally()) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' cannot carry another control request now",
                String(get_name())
            )
        );
    }
    if (steering() != local) {
        return NetwPromise::rejected(
            ERR_UNAUTHORIZED,
            NetwMultiplayer::control_refusal_detail(
                *draft, entity::Control::HOLD_NONE
            )
        );
    }
    const int64_t successor
        = NetwMultiplayer::control_steerer_of_peer(session_core(), p_successor);
    decide_control_here(successor, entity::Control::HOLD_NONE, local, 0);
    note_control_host_gift(p_successor, successor);
    return NetwPromise::resolved(Ref<NetwEntity>(this));
}

void NetwEntity::abandon_control_claims() {
    if (home != nullptr && get_reference_count() > 0) {
        home->control_abandon_claims(attached);
        return;
    }
    NetwEntityRecord &record = *get_record();
    const LocalVector<NetwEntityRecord::ControlClaim> held
        = take_rejected_control_claims(record);
    refresh_control();
    reject_control_claims(
        held,
        record,
        "'%s' was freed with a control request outstanding"
    );
}

Ref<NetwPromise> NetwMultiplayer::control_claim(
    const RID &p_entity,
    int64_t p_hold,
    int64_t p_source_route
) {
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity);
    if (entity.is_null()) {
        return NetwPromise::rejected(
            ERR_DOES_NOT_EXIST,
            "the entity is not in this session"
        );
    }
    NetwEntityRecord &record = *entity->get_record();
    entity::Control &control = *record.get_control();
    Node *owner = entity->get_owner();
    const Ref<MultiplayerAPI> api = owner != nullptr && owner->is_inside_tree()
        ? owner->get_multiplayer()
        : Ref<MultiplayerAPI>();
    if (api.is_null()) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' is in no session, so no peer can decide its control",
                String(record.get_entity_id())
            )
        );
    }
    const int64_t local = api->get_unique_id();
    const int64_t hold = p_hold;
    const Error refused = entity->control_local_refusal(local, hold);
    if (refused != OK) {
        return NetwPromise::rejected(
            refused,
            control_refusal_detail(record, hold)
        );
    }
    if (!control.has_pending() && control.resolve() == local
        && control.hold == hold) {
        return NetwPromise::resolved(entity);
    }
    if (entity->control_deciding_locally()) {
        Error denied = OK;
        String reason;
        const entity::Control::Outcome outcome = control_decide_request(
            p_entity,
            local,
            0,
            hold,
            p_source_route,
            &denied,
            &reason
        );
        if (outcome == entity::Control::Outcome::GRANTED) {
            return NetwPromise::resolved(entity);
        }
        const Error ruled = outcome == entity::Control::Outcome::UNAVAILABLE
            ? ERR_UNAVAILABLE
            : ERR_UNAUTHORIZED;
        return NetwPromise::rejected(
            denied != OK ? denied : ruled,
            !reason.is_empty() ? reason : control_refusal_detail(record, hold)
        );
    }
    if (get_replication_plane() == nullptr) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' has no session plane to carry a control request",
                String(record.get_entity_id())
            )
        );
    }
    const bool was_ahead = entity->is_claim_running_ahead();
    const entity::Control::Pending *issued = control.issue(
        entity::Control::OpKind::REQUEST,
        hold,
        clock_get_tick()
    );
    if (issued == nullptr) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            control_refusal_detail(record, hold)
        );
    }
    NetwEntityRecord::ControlClaim claim;
    claim.op = issued->op;
    claim.promise.instantiate();
    record.get_control_book().control_claims.push_back(claim);
    NETW_TRACE(sys::ENTITY, "Asking the session authority for control.");
    control_send_op(p_entity, *issued, 0, PackedByteArray(), p_source_route);
    control_follow_claim(entity->get_rid_handle(), was_ahead);
    return claim.promise;
}

Ref<NetwPromise> NetwMultiplayer::control_release(
    const RID &p_entity,
    int64_t p_successor
) {
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity);
    if (entity.is_null()) {
        return NetwPromise::rejected(
            ERR_DOES_NOT_EXIST,
            "the entity is not in this session"
        );
    }
    NetwEntityRecord &record = *entity->get_record();
    entity::Control &control = *record.get_control();
    const int64_t local = p_successor < 0 ? 0 : entity->local_peer();
    const Ref<NetwPromise> refused = entity->check_control_release(
        p_successor, local
    );
    if (refused.is_valid()) {
        return refused;
    }
    if (!control_holds_copy(p_entity, p_successor)) {
        return entity->control_release_missing_copy(p_successor);
    }
    const PackedByteArray image = control_final_image(
        p_entity, clock_get_tick()
    );
    if (entity->control_deciding_locally()) {
        const entity::Control::Outcome outcome
            = control_decide_release(p_entity, local, 0, p_successor, image);
        if (outcome == entity::Control::Outcome::GRANTED) {
            return NetwPromise::resolved(entity);
        }
        return NetwPromise::rejected(
            outcome == entity::Control::Outcome::UNAVAILABLE ? ERR_UNAVAILABLE
                                                             : ERR_UNAUTHORIZED,
            control_refusal_detail(record, entity::Control::HOLD_NONE)
        );
    }
    const entity::Control::Pending *issued = control.issue(
        entity::Control::OpKind::RELEASE,
        entity::Control::HOLD_NONE,
        clock_get_tick()
    );
    if (issued == nullptr) {
        return NetwPromise::rejected(
            ERR_UNAVAILABLE,
            vformat(
                "'%s' cannot carry another control request now",
                String(record.get_entity_id())
            )
        );
    }
    NetwEntityRecord::ControlClaim claim;
    claim.op = issued->op;
    claim.promise.instantiate();
    record.get_control_book().control_claims.push_back(claim);
    entity->refresh_control();
    control_send_op(p_entity, *issued, p_successor, image);
    return claim.promise;
}

bool NetwMultiplayer::control_holds_copy(const RID &p_entity, int64_t p_peer) {
    if (p_peer == entity::Control::SERVER || p_peer == SERVER_PEER) {
        return true;
    }
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity);
    return entity.is_valid() && (p_peer == entity->local_peer()
        || rpc_recipients(entity->get_rid_handle()).has(int32_t(p_peer)));
}

LocalVector<PropertyBindingPtr> NetwMultiplayer::control_image_bindings(
    const RID &p_entity
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
    const RID &p_entity,
    int64_t p_tick
) {
    const NetwEntityRecord *const record = entity_record(p_entity);
    int64_t measured = 0;
    const PackedByteArray bytes
        = control_state_image(p_entity, p_tick, measured);
    if (!bytes.is_empty() || measured <= session::CONTROL_FINAL_STATE_CAP) {
        return bytes;
    }
    fault::raise(
        this,
        NETW_FAULT_LIMIT_EXCEEDED,
        {String("'") + String(record->get_entity_id())
             + String("' released authority"),
         String("the state handed to the next authority"),
         String("fewer or smaller properties on this entity"),
         0,
         measured,
         session::CONTROL_FINAL_STATE_CAP},
        uint64_t(gd::instance_id(wrapper_owner(p_entity)))
    );
    return PackedByteArray();
}

PackedByteArray NetwMultiplayer::control_state_image(
    const RID &p_entity,
    int64_t p_tick,
    int64_t &r_size
) {
    r_size = 0;
    const LocalVector<PropertyBindingPtr> bindings
        = control_image_bindings(p_entity);
    if (bindings.is_empty()) {
        return PackedByteArray();
    }
    LocalVector<PackedByteArray> runs;
    int64_t size = 0;
    for (const PropertyBindingPtr &binding : bindings) {
        const PackedByteArray run = store_take_image(binding);
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
    const RID &p_entity,
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
    for (uint32_t at = 0; at < bindings.size(); ++at) {
        const PropertyBindingPtr &binding = bindings[at];
        store_install_image(
            binding,
            keys[at],
            values[at],
            int64_t(stamped) - 1
        );
    }
    return int64_t(stamped) - 1;
}

void NetwMultiplayer::control_start_claim_images(const RID &p_entity) {
    NetwEntityRecord *const record = entity_record(p_entity);
    ReplicationCore *plane = get_replication_plane();
    if (record == nullptr || plane == nullptr
        || record->get_control()->get_transfer()
            != NetwEntity::TRANSFER_IMMEDIATE) {
        return;
    }
    const int64_t now = clock_get_tick();
    const int64_t tickrate
        = int64_t(clock_get_param(CLOCK_PARAM_TICKRATE));
    record->get_control_book().claim_image_until
        = now + 2 * tickrate;
    plane->watch_claim_image(slot_pinned_entity(p_entity));
}

bool NetwMultiplayer::control_sends_claim_image(
    const RID &p_entity,
    int64_t p_tick
) {
    const NetwEntity *const entity = views.holder_of(p_entity.get_id());
    if (entity == nullptr || entity->get_owner() == nullptr
        || p_tick
            > entity->get_record()->get_control_book().claim_image_until) {
        return false;
    }
    return entity->is_claim_running_ahead() || entity->is_controller_here();
}

PackedByteArray NetwMultiplayer::control_claim_image(
    const RID &p_entity,
    int64_t p_tick
) {
    const NetwEntityRecord *const record = entity_record(p_entity);
    if (record == nullptr) {
        return PackedByteArray();
    }
    int64_t measured = 0;
    session::ClaimImage frame;
    frame.observed_revision = record->get_control()->revision;
    frame.state = control_state_image(p_entity, p_tick, measured);
    return frame.state.is_empty() ? PackedByteArray()
                                  : session::frame_write(frame);
}

bool NetwMultiplayer::control_claim_image_admits(
    const RID &p_entity,
    int64_t p_sender,
    uint64_t p_observed
) {
    const NetwEntity *const entity = views.holder_of(p_entity.get_id());
    return entity != nullptr && p_sender != 0 && p_sender == entity->steering()
        && p_sender != entity->local_peer()
        && p_observed + 1 >= entity->control()->tenure;
}

void NetwMultiplayer::control_install_claim_image(
    const RID &p_entity,
    const PackedByteArray &p_state
) {
    NetwEntityRecord *const record = entity_record(p_entity);
    if (record == nullptr) {
        return;
    }
    const int64_t installed = control_install_final_image(
        p_entity,
        p_state,
        record->get_control_book().claim_image_tick
    );
    NetwEntityRecord::ControlBook &book
        = record->get_control_book();
    if (installed > book.claim_image_tick) {
        book.claim_image_tick = installed;
    }
}

void NetwMultiplayer::control_install_held_claim_image(
    const RID &p_entity,
    bool p_tenure_changed
) {
    NetwEntity *const entity = views.holder_of(p_entity.get_id());
    if (entity == nullptr) {
        return;
    }
    NetwEntityRecord::ControlBook &book
        = entity->get_record()->get_control_book();
    if (p_tenure_changed) {
        book.claim_image_tick = -1;
    }
    if (entity->steering() == entity->local_peer()) {
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
    const int64_t now = clock_get_tick();
    const int64_t window = int64_t(clock_get_param(CLOCK_PARAM_TICKRATE));
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
    const RID &p_entity,
    int64_t p_sender,
    const session::ClaimImage &p_image
) {
    NetwEntity *const entity = views.holder_of(p_entity.get_id());
    if (entity == nullptr || p_image.state.is_empty()
        || p_sender == entity->local_peer()) {
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
    if (entity->steering() == p_sender) {
        return;
    }
    NetwEntityRecord::HeldClaimImage &held_claim_image
        = entity->get_record()->get_control_book().held_claim_image;
    held_claim_image.sender = p_sender;
    held_claim_image.observed_revision = p_image.observed_revision;
    held_claim_image.received_tick = clock_get_tick();
    held_claim_image.state = p_image.state;
}

LocalVector<PropertyBindingPtr> NetwMultiplayer::control_authored_bindings(
    const RID &p_entity
) {
    LocalVector<PropertyBindingPtr> out;
    const NetwEntityRecord *const record = entity_record(p_entity);
    ReplicationCore *plane = get_replication_plane();
    if (record == nullptr || plane == nullptr) {
        return out;
    }
    const int64_t live = liveness_core.route_of(p_entity);
    const LocalVector<PropertyBindingPtr> derived
        = plane->derived_group(live > 0 ? live : record->get_route());
    for (const PropertyBindingPtr &binding : derived) {
        if (binding != nullptr && binding->get_set() != nullptr
            && repl::record_follows_tenure(binding->get_set()->record)) {
            out.push_back(binding);
        }
    }
    return out;
}

void NetwEntity::refresh_control() {
    NetwMultiplayer *core = session_core();
    if (core != nullptr) {
        core->store_resource_entity(*get_record());
        core->sim_follow_steering(get_rid_handle());
    }
}

void NetwMultiplayer::control_follow_claim(
    const RID &p_entity,
    bool p_was_ahead
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    wrapper->refresh_control();
    const bool ahead = wrapper->is_claim_running_ahead();
    if (ahead == p_was_ahead) {
        return;
    }
    const LocalVector<PropertyBindingPtr> bindings
        = control_authored_bindings(p_entity);
    for (uint32_t at = 0; at < bindings.size(); ++at) {
        const PropertyBindingPtr &binding = bindings[at];
        if (ahead) {
            store_retain(binding);
        } else if (!wrapper->is_controller_here()) {
            store_reinstall(binding);
        }
    }
    sim_follow_claim(p_entity);
    if (ahead) {
        control_start_claim_images(p_entity);
    }
}

void NetwMultiplayer::control_send_op(
    const RID &p_entity,
    const entity::Control::Pending &p_op,
    int64_t p_successor,
    const PackedByteArray &p_final_state,
    int64_t p_source_route
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    ReplicationCore *plane = get_replication_plane();
    if (plane == nullptr) {
        return;
    }
    const entity::Control::Pending issued = p_op;
    entity_settle_captured(p_entity);
    session::ControlRequest request;
    request.source_route = uint64_t(MAX(p_source_route, int64_t(0)));
    request.op = issued.op;
    request.kind = uint8_t(issued.kind);
    request.hold = uint8_t(issued.hold);
    request.observed_revision = wrapper->control()->revision;
    request.issued_tick = uint64_t(MAX(issued.issued_tick, int64_t(0)));
    request.successor = uint64_t(MAX(p_successor, int64_t(0)));
    request.final_state = p_final_state;
    plane->request_control(wrapper, request);
    plane->watch_control(wrapper);
}

Ref<NetwPromise> NetwMultiplayer::control_take_claim(
    NetwEntityRecord &p_record,
    uint64_t p_op
) {
    LocalVector<NetwEntityRecord::ControlClaim> &control_claims
        = p_record.get_control_book().control_claims;
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
    const RID &p_entity,
    uint64_t p_op,
    entity::Control::Outcome p_outcome,
    Error p_code,
    const String &p_reason
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    entity::Control &control = *record.get_control();
    const bool was_ahead = wrapper->is_claim_running_ahead();
    entity::Control::Pending taken;
    if (control.take_pending(p_op, taken)) {
        const Ref<NetwPromise> promise = control_take_claim(record, p_op);
        control_follow_claim(p_entity, was_ahead);
        control_settle_claim(
            p_entity,
            promise,
            taken,
            p_outcome,
            p_code,
            p_reason
        );
        return;
    }
    if (control.take_abandoned(p_op)
        && p_outcome == entity::Control::Outcome::GRANTED
        && wrapper->steering() == wrapper->local_peer()) {
        control_release(p_entity, entity::Control::SERVER);
    }
}

void NetwMultiplayer::control_settle_claim(
    const RID &p_entity,
    const Ref<NetwPromise> &p_promise,
    const entity::Control::Pending &p_taken,
    entity::Control::Outcome p_outcome,
    Error p_code,
    const String &p_reason
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    if (p_promise.is_null()) {
        return;
    }
    const bool nothing_to_release
        = p_taken.kind == entity::Control::OpKind::RELEASE
        && p_outcome == entity::Control::Outcome::UNAUTHORIZED
        && wrapper->steering() != wrapper->local_peer();
    if (p_outcome == entity::Control::Outcome::GRANTED || nothing_to_release) {
        p_promise->resolve(wrapper);
        return;
    }
    const Error ruled = p_outcome == entity::Control::Outcome::UNAVAILABLE
        ? ERR_UNAVAILABLE
        : ERR_UNAUTHORIZED;
    p_promise->reject(
        p_code != OK ? p_code : ruled,
        !p_reason.is_empty() ? p_reason
                             : control_refusal_detail(record, p_taken.hold)
    );
}

bool NetwMultiplayer::control_expire(
    const RID &p_entity,
    int64_t p_tick,
    int64_t p_deadline
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return false;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    entity::Control &control = *record.get_control();
    if (wrapper->get_owner() == nullptr) {
        control_abandon_claims(p_entity);
        return false;
    }
    LocalVector<uint64_t> expired;
    for (uint32_t at = 0; at < control.pending.size(); ++at) {
        const entity::Control::Pending &held = control.pending[at];
        if (p_tick - held.issued_tick >= p_deadline) {
            expired.push_back(held.op);
        }
    }
    if (expired.is_empty()) {
        return control.has_pending();
    }
    const bool was_ahead = wrapper->is_claim_running_ahead();
    LocalVector<Ref<NetwPromise>> timed_out;
    for (uint32_t at = 0; at < expired.size(); ++at) {
        entity::Control::Pending taken;
        if (!control.take_pending(expired[at], taken)) {
            continue;
        }
        if (taken.kind == entity::Control::OpKind::REQUEST) {
            control.abandon(taken.op);
        }
        const Ref<NetwPromise> promise = control_take_claim(record, taken.op);
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
                String(wrapper->get_name()),
                p_deadline
            )
        );
    }
    return control.has_pending();
}

void NetwMultiplayer::control_seed(
    const RID &p_entity,
    uint64_t p_revision,
    uint64_t p_tenure,
    int64_t p_hold
) {
    NetwEntityRecord *const record = entity_record(p_entity);
    record->get_control()->seed(p_revision, p_tenure, p_hold);
    views.holder_of(p_entity.get_id())->refresh_control();
}

void NetwEntity::seed_control(
    uint64_t p_revision,
    uint64_t p_tenure,
    int64_t p_hold
) {
    if (home != nullptr) {
        home->control_seed(attached, p_revision, p_tenure, p_hold);
        return;
    }
    draft->get_control()->seed(p_revision, p_tenure, p_hold);
    refresh_control();
}

void NetwMultiplayer::control_handle_request(
    const RID &p_entity,
    int64_t p_sender,
    const session::ControlRequest &p_request
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }

    Node *owner = wrapper->get_owner();
    if (owner == nullptr || !owner->is_inside_tree()
        || !wrapper->is_on_server()) {
        fault::raise(
            this,
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

int64_t NetwEntity::control_admit_request(
    int64_t p_requester,
    int64_t p_hold,
    Error &r_code,
    String &r_reason
) {
    if (!get_record()->get_control()->admits_request()) {
        fault::raise(
            nullptr,
            NETW_FAULT_CONTROL_REFUSED,
            {String(get_name()), String(), String(), p_requester},
            uint64_t(gd::instance_id(this))
        );
        return 0;
    }
    const Ref<NetwControlRequest> request
        = NetwControlRequest::create(p_requester, p_hold);
    judge_control(request);
    if (request->is_denied()) {
        r_code = request->get_code();
        r_reason = request->get_reason();
        NETW_DEBUG(
            sys::ENTITY,
            "The control handler denied peer %d.",
            int(p_requester)
        );
        return 0;
    }
    return p_requester;
}

entity::Control::Outcome NetwMultiplayer::control_decide_request(
    const RID &p_entity,
    int64_t p_requester,
    uint64_t p_op,
    int64_t p_hold,
    int64_t p_source_route,
    Error *r_code,
    String *r_reason
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return entity::Control::Outcome::UNAVAILABLE;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    entity::Control &control = *record.get_control();
    using Outcome = entity::Control::Outcome;
    using Ruling = entity::Control::Ruling;
    Error denied = OK;
    String reason;
    const Ruling ruling
        = control.rule(p_requester, p_hold, control.resolve());
    Outcome outcome = Outcome::GRANTED;
    if (ruling == Ruling::UNAVAILABLE
        || wrapper->control_immediate_unavailable()) {
        fault::raise(
            this,
            NETW_FAULT_CONTROL_REFUSED,
            {String(record.get_entity_id()), String(), String(), p_requester},
            uint64_t(gd::instance_id(wrapper->get_owner()))
        );
        outcome = Outcome::UNAVAILABLE;
    } else if (ruling == Ruling::EXCLUDED) {
        outcome = Outcome::UNAUTHORIZED;
    } else if (!control_controls_source(
                   wrapper->session_core(),
                   p_requester,
                   p_source_route
               )) {
        outcome = Outcome::UNAUTHORIZED;
    } else if (
        ruling == Ruling::ASK_FILTER
        && wrapper->control_admit_request(
               p_requester,
               p_hold,
               denied,
               reason
           ) == 0
    ) {
        outcome = Outcome::UNAUTHORIZED;
    }
    const Error ruled = outcome == Outcome::GRANTED ? OK
        : outcome == Outcome::UNAVAILABLE           ? ERR_UNAVAILABLE
                                                    : ERR_UNAUTHORIZED;
    event_record(
        EventPlane::CONTROL_REQUESTED,
        wrapper->get_rid_handle(),
        record.get_route(),
        p_requester,
        ruled
    );
    if (outcome == Outcome::GRANTED) {
        wrapper->decide_control_here(p_requester, p_hold, p_requester, p_op);
    } else {
        control_refuse(
            wrapper->get_rid_handle(),
            p_requester,
            p_op,
            outcome,
            denied,
            reason
        );
    }
    if (r_code != nullptr && r_reason != nullptr) {
        *r_code = denied;
        *r_reason = reason;
    }
    return outcome;
}

entity::Control::Outcome NetwMultiplayer::control_decide_release(
    const RID &p_entity,
    int64_t p_requester,
    uint64_t p_op,
    int64_t p_successor,
    const PackedByteArray &p_final_state
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return entity::Control::Outcome::UNAVAILABLE;
    }

    using Outcome = entity::Control::Outcome;
    if (wrapper->steering() != p_requester) {
        control_refuse(p_entity, p_requester, p_op, Outcome::UNAUTHORIZED);
        return Outcome::UNAUTHORIZED;
    }
    if (!control_holds_copy(p_entity, p_successor)) {
        control_refuse(p_entity, p_requester, p_op, Outcome::UNAVAILABLE);
        return Outcome::UNAVAILABLE;
    }
    const int64_t steering
        = control_steerer_of_peer(wrapper->session_core(), p_successor);
    wrapper->decide_control_here(
        steering,
        entity::Control::HOLD_NONE,
        p_requester,
        p_op,
        p_final_state
    );
    wrapper->note_control_host_gift(p_successor, steering);
    return Outcome::GRANTED;
}

void NetwMultiplayer::control_refuse(
    const RID &p_entity,
    int64_t p_requester,
    uint64_t p_op,
    entity::Control::Outcome p_outcome,
    Error p_code,
    const String &p_reason
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    ReplicationCore *plane = get_replication_plane();
    if (p_op == 0 || plane == nullptr) {
        return;
    }
    session::ControlApply reply;
    reply.controller
        = uint64_t(MAX(record.get_control()->resolve(), int64_t(0)));
    reply.revision = record.get_control()->revision;
    reply.hold = uint8_t(record.get_control()->hold);
    reply.op = p_op;
    reply.outcome = uint8_t(p_outcome);
    reply.code = uint64_t(p_code);
    reply.reason = auth::capped_reason(p_reason);
    reply.kept = player_kept_despawns(p_entity.get_id());
    plane->reply_control(wrapper, reply, p_requester);
}

void NetwMultiplayer::control_restate(const RID &p_entity, int64_t p_peer) {
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity);
    ReplicationCore *plane = get_replication_plane();
    if (entity.is_null() || plane == nullptr) {
        return;
    }
    session::ControlApply restated;
    restated.controller = uint64_t(MAX(entity->steering(), int64_t(0)));
    restated.revision = entity->control()->revision;
    restated.hold = uint8_t(entity->control()->hold);
    restated.kept = player_kept_despawns(p_entity.get_id());
    plane->reply_control(entity.ptr(), restated, p_peer);
}

void NetwMultiplayer::control_decide(
    const RID &p_entity,
    int64_t p_peer,
    int64_t p_hold,
    int64_t p_issuer,
    uint64_t p_op,
    const PackedByteArray &p_final_state,
    bool p_sends_owner
) {
    const Ref<NetwEntity> entity = slot_pinned_entity(p_entity);
    entity->decide_control_here(
        p_peer,
        p_hold,
        p_issuer,
        p_op,
        p_final_state,
        p_sends_owner
    );
}

void NetwEntity::decide_control_here(
    int64_t p_peer,
    int64_t p_hold,
    int64_t p_issuer,
    uint64_t p_op,
    const PackedByteArray &p_final_state,
    bool p_sends_owner
) {
    const bool tenure_changed = control()->decide(p_peer, p_hold);
    refresh_control();
    adopt_controller(p_peer);
    NetwMultiplayer *core = session_core();
    if (tenure_changed && core != nullptr) {
        core->row_streams_follow_tenure(
            get_route(),
            control()->tenure
        );
    }
    if (core != nullptr) {
        const RID entity = get_rid_handle();
        core->control_install_final_image(entity, p_final_state);
        core->control_install_held_claim_image(entity, tenure_changed);
    } else {
        NetwEntityRecord::ControlBook &book
            = get_record()->get_control_book();
        if (tenure_changed) {
            book.claim_image_tick = -1;
        }
        book.held_claim_image = NetwEntityRecord::HeldClaimImage();
    }
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr) {
        return;
    }
    session::ControlApply decision;
    decision.controller = uint64_t(MAX(p_peer, int64_t(0)));
    decision.revision = control()->revision;
    decision.tenure_changed = tenure_changed;
    decision.hold = uint8_t(control()->hold);
    decision.op = p_op;
    decision.outcome = uint8_t(entity::Control::Outcome::GRANTED);
    decision.final_state = p_final_state;
    decision.has_owner = p_sends_owner;
    decision.kept = core != nullptr
        && core->player_kept_despawns(get_rid_handle().get_id());
    decision.peer = uint64_t(MAX(get_peer_id(), int64_t(0)));
    decision.player_id = uint64_t(MAX(get_player_id(), int64_t(0)));
    plane->broadcast_control(this, decision, p_issuer);
}

void NetwMultiplayer::control_change(const RID &p_entity, int64_t p_peer) {
    control_decide(p_entity, p_peer, entity::Control::HOLD_NONE, 0, 0);
}

void NetwMultiplayer::control_reassign(
    const RID &p_entity,
    int64_t p_peer,
    int64_t p_player_id,
    int64_t p_controller
) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (view.is_null()) {
        return;
    }
    NetwEntityRecord *const record = view->get_record();
    record->set_peer_id(p_peer);
    record->set_player_id(p_player_id);
    view->unwrite_player();
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
    const RID &p_entity,
    int64_t p_departed
) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (view.is_null()) {
        return;
    }
    const NetwEntityRecord *const record = view->get_record();
    if (record->get_peer_id() != p_departed) {
        control_change(p_entity, entity::Control::SERVER);
        return;
    }
    const int64_t controller = record->get_control()->resolve();
    control_reassign(
        p_entity,
        0,
        0,
        controller == p_departed ? int64_t(entity::Control::SERVER) : controller
    );
}

void NetwMultiplayer::control_raise(const RID &p_entity, uint64_t p_floor) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (view.is_null()) {
        return;
    }
    entity::Control *held = view->control();
    held->revision = MAX(held->revision, p_floor) + 1;
    held->tenure = held->revision;
    view->refresh_control();
    NetwMultiplayer *core = view->session_core();
    if (core != nullptr) {
        core->row_streams_follow_tenure(view->get_route(), held->tenure);
    }
    ReplicationCore *plane
        = core != nullptr ? core->get_replication_plane() : nullptr;
    if (plane == nullptr) {
        return;
    }
    session::ControlApply decision;
    decision.controller = uint64_t(MAX(view->steering(), int64_t(0)));
    decision.revision = held->revision;
    decision.tenure_changed = true;
    decision.hold = uint8_t(held->hold);
    decision.kept = core != nullptr
        && core->player_kept_despawns(view->get_rid_handle().get_id());
    plane->broadcast_control(view.ptr(), decision, 0);
}

void NetwMultiplayer::control_handle_apply(
    const RID &p_entity,
    const session::ControlApply &p_applied
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    player_hear_kept(p_entity.get_id(), p_applied.kept);
    if (record.get_control()->install(
            p_applied.revision,
            p_applied.tenure_changed,
            int64_t(p_applied.hold)
        )) {
        if (p_applied.has_owner) {
            record.set_peer_id(int64_t(p_applied.peer));
            record.set_player_id(int64_t(p_applied.player_id));
            wrapper->unwrite_player();
        }
        wrapper->refresh_control();
        wrapper->adopt_controller(int64_t(p_applied.controller));
        if (p_applied.tenure_changed) {
            row_streams_follow_tenure(
                record.get_route(),
                record.get_control()->tenure
            );
        }
        const RID entity = wrapper->get_rid_handle();
        control_install_final_image(entity, p_applied.final_state);
        control_install_held_claim_image(entity, p_applied.tenure_changed);
    }
    if (p_applied.op != 0) {
        control_settle(
            wrapper->get_rid_handle(),
            p_applied.op,
            entity::Control::Outcome(p_applied.outcome),
            Error(int(p_applied.code)),
            p_applied.reason
        );
    }
}

void NetwMultiplayer::control_abandon_claims(const RID &p_entity) {
    control_reject_claims(
        p_entity,
        "'%s' was freed with a control request outstanding"
    );
}

void NetwMultiplayer::control_reject_claims(
    const RID &p_entity,
    const char *p_detail
) {
    const Ref<NetwEntity> wrapper = slot_pinned_entity(p_entity);
    if (wrapper.is_null()) {
        return;
    }
    NetwEntityRecord &record = *wrapper->get_record();
    const bool was_ahead = wrapper->is_claim_running_ahead();
    LocalVector<NetwEntityRecord::ControlClaim> held
        = take_rejected_control_claims(record);
    control_follow_claim(p_entity, was_ahead);
    reject_control_claims(held, record, p_detail);
}

void NetwMultiplayer::control_receive_request(
    const RID &p_entity,
    int64_t p_sender,
    const session::ControlRequest &p_request
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_valid()) {
        control_handle_request(p_entity, p_sender, p_request);
    }
}

void NetwMultiplayer::control_receive_apply(
    const RID &p_entity,
    const session::ControlApply &p_applied
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_valid()) {
        control_handle_apply(p_entity, p_applied);
    }
}

void NetwMultiplayer::control_receive_claim_image(
    const RID &p_entity,
    int64_t p_sender,
    const session::ClaimImage &p_image
) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_valid()) {
        control_handle_claim_image(p_entity, p_sender, p_image);
    }
}

} // namespace netw
