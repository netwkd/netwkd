#include "netw/api/entity.hpp"

#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "godot/utility.hpp"
#include "netw/api/display.hpp"
#include "netw/game_call.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/persistence.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/simulation.hpp"
#include "netw/colors.hpp"
#include "netw/entity/control.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/lifecycle/rule.hpp"
#include "netw/log.hpp"
#include "netw/peer_rename.hpp"
#include "netw/profile.hpp"
#include "netw/repl/set_model.hpp"
#include "netw/spawn/record.hpp"
#include "netw/store/validate.hpp"
#include "netw/sync_authoring.hpp"
#include "netw/synchronizers.hpp"
#include "netw/tree_hook.hpp"
#include "netw/wire/stream.hpp"

using namespace godot;
using namespace netw;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertySet;

namespace {

constexpr const char *SIG_DESPAWNED = "despawned";
constexpr const char *SIG_DESPAWNING = "despawning";
constexpr const char *SIG_CONTROL_CHANGED = "control_changed";
constexpr const char *SIG_HIDDEN = "hidden";
constexpr const char *SIG_LIFECYCLE_REFUSED = "lifecycle_refused";
constexpr const char *SIG_REPARENTED = "reparented";

LocalVector<ObjectID> &pending_parts() {
    static LocalVector<ObjectID> held;
    return held;
}

Ref<NetwEntity> entity_above(Node *p_node) {
    for (Node *above = p_node->get_parent(); above != nullptr;
         above = above->get_parent()) {
        if (tree::entity_object_at(above) != nullptr) {
            return NetwEntity::of(above);
        }
    }
    return Ref<NetwEntity>();
}

Ref<NetwEntity> part_target(Node *p_node) {
    if (p_node->get_owner() == nullptr
        || !p_node->get_scene_file_path().is_empty()) {
        return Ref<NetwEntity>();
    }
    const Ref<NetwEntity> target = entity_above(p_node);
    if (target.is_null() || target->get_declares_scene()) {
        return Ref<NetwEntity>();
    }
    return target;
}

String owner_name(const Ref<NetwEntity> &p_entity) {
    Node *node = p_entity->get_owner();
    return node != nullptr ? String(node->get_name()) : String();
}

void raise_part_fault(
    netw_fault_code p_code,
    Node *p_part,
    const Ref<NetwEntity> &p_target,
    const String &p_option
) {
    const Ref<Script> script = p_part->get_script();
    fault::raise(
        NetwMultiplayer::session_holding(p_target->get_owner()),
        p_code,
        {p_option, String(p_part->get_name()), owner_name(p_target)},
        uint64_t(gd::instance_id(
            script.is_valid() ? static_cast<Object *>(script.ptr()) : p_part
        ))
    );
}

bool part_only(const StringName &p_option) {
    return p_option == StringName("name")
        || p_option == StringName("on_parent_despawn");
}

void copy_changed(
    Object *p_from,
    Object *p_fresh,
    Object *p_into,
    Node *p_part,
    const Ref<NetwEntity> &p_target
) {
    const StringName klass = p_from->get_class();
    const Array infos = gd::property_list(p_from);
    for (int at = 0; at < infos.size(); ++at) {
        const Dictionary info = infos[at];
        const StringName option = info["name"];
        if (gd::property_setter(klass, option) == StringName()) {
            continue;
        }
        const Variant given = p_from->get(option);
        const Variant initial = p_fresh->get(option);
        if (given == initial) {
            continue;
        }
        const String spelled = String("{") + String(klass) + String(".")
            + String(option) + String("}");
        if (part_only(option)) {
            raise_part_fault(NETW_FAULT_PART_ONLY, p_part, p_target, spelled);
            continue;
        }
        const Variant held = p_into->get(option);
        if (held != initial && held != given) {
            raise_part_fault(
                NETW_FAULT_PART_CONFLICT,
                p_part,
                p_target,
                spelled
            );
            continue;
        }
        p_into->set(option, given);
    }
}

void fold_part(
    Node *p_part,
    const Ref<NetwEntity> &p_stand_in,
    const Ref<NetwEntity> &p_target
) {
    Ref<NetwEntity> fresh;
    fresh.instantiate();
    copy_changed(
        p_stand_in.ptr(),
        fresh.ptr(),
        p_target.ptr(),
        p_part,
        p_target
    );
    if (p_stand_in->held_interest().is_valid()) {
        raise_part_fault(
            NETW_FAULT_PART_ONLY,
            p_part,
            p_target,
            String("{NetwEntity.interest}")
        );
    }
    const NetwEntityRecord::Part parts[] = {
        NetwEntityRecord::PART_PREDICTION,
        NetwEntityRecord::PART_SIMULATION,
        NetwEntityRecord::PART_DISPLAY,
    };
    NetwEntityRecord *const stand_in
        = NetwMultiplayer::record_of_wrapper(p_stand_in.ptr());
    NetwEntityRecord *const blank
        = NetwMultiplayer::record_of_wrapper(fresh.ptr());
    NetwEntityRecord *const target
        = NetwMultiplayer::record_of_wrapper(p_target.ptr());
    for (const NetwEntityRecord::Part part : parts) {
        Object *given = gd::live_object(stand_in->minted_part(part));
        if (given == nullptr) {
            continue;
        }
        Object *initial = gd::live_object(blank->part(part, fresh.ptr()));
        Object *into = gd::live_object(target->part(part, p_target.ptr()));
        if (initial != nullptr && into != nullptr) {
            copy_changed(given, initial, into, p_part, p_target);
        }
    }
    p_part->remove_meta(NetwMultiplayer::wrapper_meta());
    tree::unmark(uint64_t(gd::instance_id(p_part)), tree::MARK_ENTITY);
    p_stand_in->set_owner(nullptr);
}

int depth_of(Node *p_node) {
    int depth = 0;
    for (Node *above = p_node->get_parent(); above != nullptr;
         above = above->get_parent()) {
        depth += 1;
    }
    return depth;
}

Ref<NetwEntity> as_entity(const Ref<RefCounted> &p_wrapper) {
    return Ref<NetwEntity>(Object::cast_to<NetwEntity>(p_wrapper.ptr()));
}

bool lifecycle_admits(
    NetwEntity *p_entity,
    NetwMultiplayer *p_core,
    lifecycle::Kind p_kind,
    const char *p_verb
) {
    if (p_core == nullptr) {
        return true;
    }
    String refusal;
    return p_core
        ->lifecycle_judge_verb(
            p_core->lifecycle_local_facts(p_entity, p_kind),
            String(p_verb),
            p_entity->get_owner(),
            refusal
        )
        .admitted();
}

} // namespace

NetwEntity::NetwEntity() : draft(memnew(NetwEntityRecord)) {
}

NetwEntity::~NetwEntity() {
    if (NetwMultiplayer::is_tearing_down(session_id)) {
        session_id = ObjectID();
    }
    NetwMultiplayer::control_abandon_claims(this);
    NetwMultiplayer::control_structure_refuse(
        this,
        0,
        ERR_UNAVAILABLE,
        String("the entity left before the session authority decided")
    );
    tree::unmark(uint64_t(owner_id), tree::MARK_ENTITY);
    if (home != nullptr) {
        home->wrapper_release(this);
    }
    if (draft != nullptr) {
        godot::memdelete(draft);
    }
}

NetwEntityRecord *NetwEntity::get_record() const {
    return NetwMultiplayer::record_of_wrapper(this);
}

Ref<NetwEntity> NetwEntity::of(Node *p_node) {
    return as_entity(NetwMultiplayer::wrapper_at(p_node));
}

Ref<NetwEntity> NetwEntity::ensure(Node *p_root) {
    return as_entity(NetwMultiplayer::wrapper_ensure(p_root));
}

Ref<NetwEntity> NetwEntity::resolve(Node *p_node) {
    return as_entity(NetwMultiplayer::wrapper_resolve(p_node));
}

Ref<NetwEntity> NetwEntity::declare(Node *p_node) {
    if (p_node == nullptr || p_node->is_inside_tree()) {
        return resolve(p_node);
    }
    if (Object *own = tree::entity_object_at(p_node)) {
        return Ref<NetwEntity>(Object::cast_to<NetwEntity>(own));
    }
    if (p_node->get_parent() == nullptr) {
        const Ref<NetwEntity> stand_in = ensure(p_node);
        pending_parts().push_back(gd::instance_id(p_node));
        return stand_in;
    }
    const Ref<NetwEntity> above = entity_above(p_node);
    if (above.is_null()) {
        return resolve(p_node);
    }
    if (above->get_declares_scene()
        || !p_node->get_scene_file_path().is_empty()) {
        return ensure(p_node);
    }
    return above;
}

void NetwEntity::settle_parts(Node *p_root, bool p_spawned) {
    LocalVector<ObjectID> &pending = pending_parts();
    if (p_root == nullptr || pending.is_empty()) {
        return;
    }
    LocalVector<Node *> under;
    for (uint32_t at = 0; at < pending.size();) {
        Node *node = Object::cast_to<Node>(gd::object_of(pending[at]));
        if (node != nullptr && node != p_root
            && !p_root->is_ancestor_of(node)) {
            at += 1;
            continue;
        }
        pending.remove_at_unordered(at);
        if (node == nullptr) {
            continue;
        }
        if (node != p_root) {
            under.push_back(node);
            continue;
        }
        const Ref<NetwEntity> target = p_spawned ? Ref<NetwEntity>()
                                                 : part_target(node);
        if (target.is_valid()) {
            raise_part_fault(
                NETW_FAULT_PART_TOO_LATE,
                node,
                target,
                String()
            );
        }
    }
    for (uint32_t at = 1; at < under.size(); ++at) {
        Node *held = under[at];
        const int depth = depth_of(held);
        uint32_t into = at;
        while (into > 0 && depth_of(under[into - 1]) > depth) {
            under[into] = under[into - 1];
            into -= 1;
        }
        under[into] = held;
    }
    for (Node *node : under) {
        const Ref<NetwEntity> stand_in = of(node);
        const Ref<NetwEntity> target = part_target(node);
        if (stand_in.is_null() || stand_in->get_owner() != node
            || target.is_null()) {
            continue;
        }
        if (target->get_stage() != STAGE_UNBOUND) {
            raise_part_fault(NETW_FAULT_PART_TOO_LATE, node, target, String());
            continue;
        }
        fold_part(node, stand_in, target);
    }
}

bool NetwEntity::belongs_to(NetwMultiplayer *p_branch) const {
    NetwMultiplayer *core = session_core();
    return p_branch != nullptr && (core == nullptr || core == p_branch);
}

void NetwEntity::readmitted_ready() {
    NetwMultiplayer::entity_readmitted_ready(this);
}

void NetwEntity::watch_scene_edges(const RID &p_subject) {
    scene_subject = p_subject;
}

void NetwEntity::watch_despawning(bool p_watched) {
    despawning_watched = p_watched;
}

Node *NetwEntity::get_owner() const {
    return Object::cast_to<Node>(gd::object_of(owner_id));
}

void NetwEntity::set_owner(Node *p_owner) {
    owner_id = gd::instance_id(p_owner);
}

entity::Control *NetwEntity::control() const {
    return get_record()->get_control();
}

NetwMultiplayer *NetwEntity::session_core() const {
    return Object::cast_to<NetwMultiplayer>(gd::object_of(session_id));
}

Ref<MultiplayerAPI> NetwEntity::get_multiplayer() const {
    return Ref<MultiplayerAPI>(session_core());
}

void NetwEntity::stamp_multiplayer(const Ref<NetwMultiplayer> &p_api) {
    if (p_api.is_null() || !p_api->slot_attach(this).is_valid()) {
        return;
    }
    session_id = gd::instance_id(p_api.ptr());
}

StringName NetwEntity::get_name() const {
    return get_record()->get_entity_id();
}

void NetwEntity::set_name(const StringName &p_name) {
    get_record()->set_entity_id(p_name);
}

int64_t NetwEntity::get_peer_id() const {
    return get_record()->get_peer_id();
}

void NetwEntity::set_peer_id(int64_t p_peer_id) {
    get_record()->set_peer_id(p_peer_id);
}

int64_t NetwEntity::get_player_id() const {
    return get_record()->get_player_id();
}

void NetwEntity::set_player_id(int64_t p_player_id) {
    get_record()->set_player_id(p_player_id);
}

int64_t NetwEntity::get_route() const {
    return get_record()->get_route();
}

void NetwEntity::set_route(int64_t p_route) {
    get_record()->set_route(p_route);
}

RID NetwEntity::get_rid_handle() const {
    return draft != nullptr ? draft->get_handle() : attached;
}

void NetwEntity::set_control_handler(const Callable &p_handler) {
    if (!game_call_declared(p_handler, game_calls::CONTROL_HANDLER)
        && !p_handler.is_valid()) {
        return;
    }
    control_handler = p_handler;
}

static void judge_now(
    NetwMultiplayer *p_session,
    const Callable &p_handler,
    const Variant &p_request,
    const GameCall &p_site
) {
    if (!p_handler.is_valid()) {
        return;
    }
    Array args;
    args.push_back(p_request);
    const Variant returned = game_call(p_handler, args, p_site);
    const Object *resumes = gd::live_object(returned);
    if (resumes != nullptr && resumes->is_class("GDScriptFunctionState")) {
        fault::raise(
            p_session,
            NETW_FAULT_HANDLER_AWAITED,
            {String("{") + p_site.member + String("}")}
        );
    }
}

void NetwEntity::judge_control(const Variant &p_request) const {
    judge_now(
        session_core(),
        control_handler,
        p_request,
        game_calls::CONTROL_HANDLER
    );
}

void NetwEntity::set_lifecycle_handler(const Callable &p_handler) {
    if (!game_call_declared(p_handler, game_calls::LIFECYCLE_HANDLER)
        && !p_handler.is_valid()) {
        return;
    }
    lifecycle_handler = p_handler;
}

void NetwEntity::judge_lifecycle(const Variant &p_request) const {
    judge_now(
        session_core(),
        lifecycle_handler,
        p_request,
        game_calls::LIFECYCLE_HANDLER
    );
}

NetwEntity::Transfer NetwEntity::get_transfer() const {
    return Transfer(control()->get_transfer());
}

void NetwEntity::set_transfer(Transfer p_value) {
    NetwMultiplayer::control_set_param(
        this,
        NetwMultiplayer::ENTITY_PARAM_TRANSFER,
        int64_t(p_value)
    );
}

NetwEntity::Authority NetwEntity::get_authority() const {
    return Authority(control()->get_authority());
}

void NetwEntity::set_authority(Authority p_value) {
    NetwMultiplayer::control_set_param(
        this,
        NetwMultiplayer::ENTITY_PARAM_AUTHORITY,
        int64_t(p_value)
    );
}

NetwEntity::DisconnectRule NetwEntity::get_on_controller_disconnect() const {
    return DisconnectRule(control()->get_on_disconnect());
}

void NetwEntity::set_on_controller_disconnect(DisconnectRule p_value) {
    NetwMultiplayer::control_set_param(
        this,
        NetwMultiplayer::ENTITY_PARAM_ON_CONTROLLER_DISCONNECT,
        int64_t(p_value)
    );
}

NetwEntity::ParentDespawnRule NetwEntity::get_on_parent_despawn() const {
    return ParentDespawnRule(control()->get_on_parent_despawn());
}

void NetwEntity::set_on_parent_despawn(ParentDespawnRule p_value) {
    NetwMultiplayer::control_set_param(
        this,
        NetwMultiplayer::ENTITY_PARAM_ON_PARENT_DESPAWN,
        int64_t(p_value)
    );
}

NetwEntity::Lifecycle NetwEntity::get_lifecycle() const {
    return Lifecycle(control()->get_lifecycle());
}

void NetwEntity::set_lifecycle(Lifecycle p_value) {
    control()->set_lifecycle(int64_t(p_value));
}

bool NetwEntity::get_declares_scene() const {
    return get_record()->get_declares_scene();
}

void NetwEntity::set_declares_scene(bool p_value) {
    get_record()->set_declares_scene(p_value);
}

StringName NetwEntity::get_scene_label() const {
    return get_record()->scene_label_of(get_owner());
}

void NetwEntity::set_scene_label(const StringName &p_value) {
    get_record()->set_scene_label(p_value);
}

int64_t NetwEntity::get_scene_isolation() const {
    return get_record()->scene_isolation_of(get_owner());
}

void NetwEntity::set_scene_isolation(int64_t p_value) {
    get_record()->set_scene_isolation(p_value);
}

int64_t NetwEntity::get_controller() const {
    return entity::Control::public_peer(steering());
}

int64_t NetwEntity::steering() const {
    return control()->resolve();
}

void NetwEntity::set_controller(int64_t p_value) {
    if (get_stage() != int64_t(entity::Stage::UNBOUND)) {
        transfer_control(p_value);
        return;
    }
    if (p_value <= 0) {
        fault::raise(
            session_core(),
            NETW_FAULT_BAD_VALUE,
            {String("{NetwEntity.controller}"),
             String::num_int64(p_value),
             String("a peer id, or 1 for the server")},
            uint64_t(gd::instance_id(get_owner()))
        );
        return;
    }
    NetwMultiplayer::control_record(
        this,
        NetwMultiplayer::control_steerer_of_peer(this, p_value)
    );
}

bool NetwEntity::get_is_controlled_locally() const {
    return is_controller_here() || is_claim_running_ahead();
}

bool NetwEntity::is_controller_here() const {
    return control()->controlled_by(local_peer());
}

bool NetwEntity::is_claim_running_ahead() const {
    return control()->runs_ahead() && !is_controller_here();
}

bool NetwEntity::is_claim_pending() const {
    return control()->claims_pending();
}

Ref<NetwPlayer> NetwEntity::get_controller_player() const {
    NetwMultiplayer *core = session_core();
    const int64_t steered = steering();
    if (core == nullptr || steered == entity::Control::SERVER) {
        return Ref<NetwPlayer>();
    }
    return core->player_of(steered);
}

int64_t NetwEntity::local_peer() const {
#if defined(NETW_TESTS)
    store::peer_reads_seen() += 1;
#endif
    Node *owner = get_owner();
    if (owner == nullptr) {
        return 0;
    }
    NetwMultiplayer *api = session_core();
    Ref<MultiplayerAPI> resolved;
    if (api != nullptr) {
        resolved = Ref<MultiplayerAPI>(api);
    } else if (owner->is_inside_tree()) {
        resolved = owner->get_multiplayer();
    }
    if (resolved.is_null() || resolved->get_multiplayer_peer().is_null()) {
        return 0;
    }
    return resolved->get_unique_id();
}

void NetwEntity::set_controller_player(const Ref<NetwPlayer> &p_player) {
    const int64_t peer = p_player.is_valid() ? p_player->get_peer_id() : 0;
    if (p_player.is_valid() && peer <= 0) {
        fault::raise(
            session_core(),
            NETW_FAULT_UNKNOWN_PEER,
            {String("{NetwEntity.controller}"), String(), String(), peer},
            uint64_t(gd::instance_id(get_owner()))
        );
        return;
    }
    const int64_t steered
        = p_player.is_valid() ? peer : entity::Control::SERVER;
    if (get_stage() == int64_t(entity::Stage::UNBOUND)) {
        NetwMultiplayer::control_record(this, steered);
        return;
    }
    NetwMultiplayer::control_transfer(this, steered);
}

Error NetwEntity::transfer_control(int64_t p_peer) {
    return NetwMultiplayer::control_give(this, p_peer);
}

Ref<NetwPromise> NetwEntity::claim_control(Hold p_hold) {
    return NetwMultiplayer::control_claim(this, int64_t(p_hold), 0);
}

Ref<NetwPromise> NetwEntity::release_control(int64_t p_successor) {
    return NetwMultiplayer::control_release(this, p_successor);
}

NetwEntity::Hold NetwEntity::get_hold() const {
    return Hold(control()->hold);
}

bool NetwEntity::get_is_control_pending() const {
    return control()->has_pending();
}

uint64_t NetwEntity::get_control_revision() const {
    return control()->revision;
}

uint64_t NetwEntity::get_control_tenure() const {
    return control()->tenure;
}

void NetwEntity::set_replication_priority(double p_weight) {
    NetwMultiplayer::control_set_param(
        this,
        NetwMultiplayer::ENTITY_PARAM_REPLICATION_PRIORITY,
        p_weight
    );
}

double NetwEntity::get_replication_priority() const {
    return get_record()->get_replication_priority();
}

bool NetwEntity::is_on_server() const {
    NetwMultiplayer *api = session_core();
    if (api == nullptr) {
        return true;
    }
    return api->is_host();
}

Ref<NetwPlayer> NetwEntity::unbound_player() const {
    return written_view.is_valid() && written_view->core() != nullptr
        ? written_view
        : Ref<NetwPlayer>();
}

void NetwEntity::unwrite_player() {
    written_view.unref();
    written_player = 0;
}

Ref<NetwPlayer> NetwEntity::get_player() const {
    if (has_unbound_player()) {
        return unbound_player();
    }
    NetwMultiplayer *core = session_core();
    const int64_t represented = get_peer_id();
    if (core == nullptr || represented == 0) {
        return Ref<NetwPlayer>();
    }
    return core->player_of(represented);
}

void NetwEntity::set_player(const Ref<NetwPlayer> &p_player) {
    if (get_stage() != int64_t(entity::Stage::UNBOUND)) {
        fault::raise(
            session_core(),
            NETW_FAULT_TOO_LATE,
            {String("{NetwEntity.player}"),
             String("'") + String(get_name()) + String("'"),
             String("the entity is spawned")},
            uint64_t(gd::instance_id(get_owner()))
        );
        return;
    }
    const int64_t previous = get_peer_id();
    const bool steers_previous = previous != 0 && steering() == previous;
    const int64_t peer = p_player.is_valid() ? p_player->get_peer_id() : 0;
    written_view = p_player;
    written_player = p_player.is_valid() ? p_player->incarnation : 0;
    set_peer_id(peer);
    set_player_id(written_player);
    if (peer > 0 && (!control()->get_configured() || steers_previous)) {
        NetwMultiplayer::control_record(this, peer);
    } else if (peer == 0 && steers_previous) {
        control()->clear_controller();
        NetwMultiplayer::control_announce(
            this,
            previous,
            entity::Control::SERVER
        );
    }
}

bool NetwEntity::get_is_player() const {
    return get_peer_id() != 0;
}

NetwEntity::Stage NetwEntity::get_stage() const {
    return Stage(get_record()->get_stage());
}

Ref<NetwDespawnOpts> NetwEntity::get_active_despawn_opts() const {
    return get_record()->get_active_despawn_opts();
}

bool NetwEntity::publish_stage(int64_t p_from, int64_t p_to) {
    if (p_from == p_to) {
        return true;
    }
    const Ref<NetwEntity> keep(this);
    NetwEntityRecord *const captured = get_record();
    const ObjectID owner = owner_id;
    Node *const node = get_owner();
    const ObjectID parent
        = gd::instance_id(node != nullptr ? node->get_parent() : nullptr);
    const ObjectID session = session_id;
    const RID handle = get_rid_handle();
    const Ref<NetwMultiplayer> core(session_core());
    if (core.is_valid()) {
        core->entity_note_stage(captured, p_from, p_to);
    }
#if defined(NETW_TESTS)
    if (has_signal("stage_changed")) {
        emit_signal("stage_changed", p_from, p_to);
    }
#endif
    Node *const remaining = get_owner();
    return get_record() == captured && owner_id == owner
        && session_id == session && remaining == node
        && session_core() == core.ptr() && get_rid_handle() == handle
        && get_record()->get_stage() == p_to
        && (remaining == nullptr
            || (gd::instance_id(remaining->get_parent()) == parent
                && (p_to == STAGE_FREED
                    || !remaining->is_queued_for_deletion())));
}

void NetwEntity::transition(int64_t p_stage) {
    const int64_t from = get_stage();
    if (get_record()->transition(p_stage)) {
        get_record()->publish_stage(this, from);
    }
}

void NetwEntity::arm(const Ref<NetwMultiplayer> &p_api) {
    NetwMultiplayer::entity_arm(this, p_api);
}

void NetwEntity::_handle_tree_entered() {
    NetwMultiplayer::entity_handle_entered(this);
}

void NetwEntity::hydrate_components() {
    Node *owner = get_owner();
    LocalVector<ObjectID> &registered = get_record()->get_comp_registered();
    PackedStringArray paths;
    for (uint32_t at = 0; at < registered.size(); at++) {
        Object *component = gd::object_of(registered[at]);
        if (component == nullptr) {
            continue;
        }
        const NodePath relative
            = NetwMultiplayer::relative_path(owner, component);
        if (!relative.is_empty()) {
            paths.push_back(String(relative));
        }
    }
    NetwCompTable &table = get_record()->get_comp_table();
    table.assign(paths);
    int64_t structure = 0;
    {
        NETW_ZONE_NC("NetwEntity comp structure hash", colors::LIVENESS);
        structure = comp_structure_hash(table.sorted_paths());
    }
    table.set_table_hash(structure);
    if (table.reconcile(is_on_server())) {
        fault::raise(
            session_core(),
            NETW_FAULT_PEER_MISMATCH,
            {String("the component list of '") + String(get_name())
                 + String("'"),
             String(),
             String("the networked components of '") + String(get_name())
                 + String("'"),
             SERVER_PEER},
            uint64_t(gd::instance_id(owner))
        );
    }
    if (NetwMultiplayer *core = session_core()) {
        core->sync_pipeline_recapture_entity(this);
    }
}

int64_t NetwEntity::comp_structure_hash(
    const PackedStringArray &p_paths
) const {
    String seed;
    for (int at = 0; at < p_paths.size(); at++) {
        seed += p_paths[at] + String(",");
    }
    seed += "|methods:";

    LocalVector<Ref<Script>> scripts;
    Node *owner = get_owner();
    if (owner != nullptr) {
        const Ref<Script> declared = owner->get_script();
        if (declared.is_valid()) {
            scripts.push_back(declared);
        }
    }
    const LocalVector<ObjectID> &registered
        = get_record()->get_comp_registered();
    for (uint32_t at = 0; at < registered.size(); at++) {
        Object *component = gd::object_of(registered[at]);
        if (component == nullptr) {
            continue;
        }
        const Ref<Script> declared = component->get_script();
        if (declared.is_valid()) {
            scripts.push_back(declared);
        }
    }

    for (uint32_t at = 0; at < scripts.size(); at++) {
        PackedStringArray methods;
        gd::script_rpc_method_names(scripts[at], methods);
        methods.sort();
        seed += "|m:" + String("/").join(methods);

        PackedStringArray properties;
        PackedStringArray signals;
        gd::script_property_and_signal_names(scripts[at], properties, signals);
        if (scripts[at]->get_class() == StringName("CSharpScript")) {
            properties.clear();
        }
        properties.sort();
        signals.sort();
        seed += "|p:" + String("/").join(properties);
        seed += "|s:" + String("/").join(signals);
    }
    return seed.hash() & 0xFFFF;
}

void NetwEntity::_on_owner_ready() {
    NetwMultiplayer::entity_owner_ready(this);
}

void NetwEntity::_handle_tree_exiting() {
    NetwMultiplayer::entity_handle_exiting(this);
}

void NetwEntity::despawn(const Ref<NetwDespawnOpts> &p_opts) {
    if (!lifecycle_admits(
            this,
            session_core(),
            lifecycle::Kind::DESPAWN,
            "NetwEntity.despawn"
        )) {
        return;
    }
    NetwMultiplayer::entity_despawn_admitted(this, p_opts);
}

void NetwEntity::_remote_hide() {
    NetwMultiplayer::Announcement row;
    row.kind = NetwMultiplayer::AnnouncementKind::HIDDEN;
    NetwMultiplayer::entity_announce(this, row);
}

NetwCompTable &NetwEntity::comp_table() {
    return get_record()->get_comp_table();
}

const NetwCompTable &NetwEntity::comp_table() const {
    return get_record()->get_comp_table();
}

bool NetwEntity::get_comps_poisoned() const {
    return get_record()->get_comp_table().get_poisoned();
}

bool NetwEntity::has_comp_path(const NodePath &p_path) const {
    return get_record()->get_comp_table().has_path(String(p_path));
}

Node *NetwEntity::comp_node_of(int64_t p_comp) const {
    return get_record()->get_comp_table().resolve_node(
        get_owner(),
        p_comp,
        String()
    );
}

void NetwEntity::register_component(Node *p_component) {
    Node *owner = get_owner();
    if (p_component == nullptr || p_component == owner) {
        return;
    }
    LocalVector<ObjectID> &registered = get_record()->get_comp_registered();
    const ObjectID id = gd::instance_id(p_component);
    for (uint32_t at = 0; at < registered.size(); at++) {
        if (registered[at] == id) {
            return;
        }
    }
    registered.push_back(id);
    if (get_record()->get_comp_table().get_table_hash() != 0) {
        fault::raise(
            session_core(),
            NETW_FAULT_TOO_LATE,
            {String("{Netw.configure_property}"),
             String("'") + String(p_component->get_name()) + String("'"),
             String("the node enters the tree")},
            uint64_t(gd::instance_id(p_component))
        );
    }
}

NodePath NetwEntity::relative_path(Node *p_source, Node *p_target) const {
    return NetwMultiplayer::relative_path(p_source, p_target);
}

int64_t NetwEntity::comp_of(Node *p_node) const {
    Node *owner = get_owner();
    if (p_node == nullptr || owner == nullptr || p_node == owner) {
        return 0;
    }
    const NodePath rel = relative_path(owner, p_node);
    const NetwCompTable &table = get_record()->get_comp_table();
    if (table.get_poisoned() || !table.has_path(String(rel))) {
        return NetwCompTable::FALLBACK_COMP;
    }
    return table.id_for_path(String(rel));
}

String NetwEntity::comp_path_of(Node *p_node) const {
    if (comp_of(p_node) != NetwCompTable::FALLBACK_COMP) {
        return String();
    }
    return String(relative_path(get_owner(), p_node));
}

NodePath NetwEntity::property_path(
    Node *p_source,
    const StringName &p_property,
    Node *p_base
) const {
    return NetwMultiplayer::property_path(
        p_source,
        p_property,
        p_base != nullptr ? p_base : get_owner()
    );
}

PropertyBindingPtr NetwEntity::derived_binding(int64_t p_record) const {
    NetwMultiplayer *core = session_core();
    if (core == nullptr) {
        return PropertyBindingPtr();
    }
    return core->entity_record_binding(
        get_record(),
        get_owner(),
        p_record,
        get_rid_handle()
    );
}

PropertyBindingPtr NetwEntity::get_state_binding() const {
    return derived_binding(repl::SET_RECORD_STATE);
}

PropertyBindingPtr NetwEntity::get_input_binding() const {
    return derived_binding(repl::SET_RECORD_INPUT);
}

PropertyBindingPtr NetwEntity::get_broadcast_binding() const {
    return derived_binding(repl::SET_RECORD_BROADCAST);
}

Ref<NetwInterest> NetwEntity::get_interest() const {
    return const_cast<NetwEntity *>(this)->get_record()->part(
        NetwEntityRecord::PART_INTEREST,
        const_cast<NetwEntity *>(this)
    );
}

Ref<NetwInterest> NetwEntity::held_interest() const {
    return get_record()->minted_part(NetwEntityRecord::PART_INTEREST);
}

Ref<NetwScene> NetwEntity::get_scene() const {
    NetwMultiplayer *core = session_core();
    if (core == nullptr || get_owner() == nullptr) {
        return Ref<NetwScene>();
    }
    return core->entity_scene_facet(
        get_record(),
        const_cast<NetwEntity *>(this)
    );
}

Ref<NetwPrediction> NetwEntity::get_prediction() const {
    return const_cast<NetwEntity *>(this)->get_record()->part(
        NetwEntityRecord::PART_PREDICTION,
        const_cast<NetwEntity *>(this)
    );
}

Ref<NetwSimulation> NetwEntity::get_simulation() const {
    return const_cast<NetwEntity *>(this)->get_record()->part(
        NetwEntityRecord::PART_SIMULATION,
        const_cast<NetwEntity *>(this)
    );
}

Ref<NetwPrediction> NetwEntity::prediction_if_minted() const {
    return get_record()->minted_part(NetwEntityRecord::PART_PREDICTION);
}

Ref<NetwSimulation> NetwEntity::simulation_if_minted() const {
    return get_record()->minted_part(NetwEntityRecord::PART_SIMULATION);
}

Ref<NetwPersistence> NetwEntity::get_persistence() const {
    return const_cast<NetwEntity *>(this)->get_record()->part(
        NetwEntityRecord::PART_PERSISTENCE,
        const_cast<NetwEntity *>(this)
    );
}

Ref<NetwDisplay> NetwEntity::get_display() const {
    return const_cast<NetwEntity *>(this)->get_record()->part(
        NetwEntityRecord::PART_DISPLAY,
        const_cast<NetwEntity *>(this)
    );
}

TypedArray<MultiplayerSynchronizer> NetwEntity::synchronizers() {
    if (!synchronizers_dirty
        && (synchronizers_absent || !synchronizers_cache.is_empty())) {
        return synchronizers_cache;
    }
    Node *owner = get_owner();
    if (owner == nullptr) {
        return synchronizers_cache;
    }
    const TypedArray<MultiplayerSynchronizer> found
        = synchronizers::of_node(owner);
    if (!found.is_empty() || !owner_exiting_tree) {
        synchronizers_cache = found;
    }
    synchronizers_dirty = owner_exiting_tree && synchronizers_cache.is_empty();
    synchronizers_absent = synchronizers_cache.is_empty() && !owner_exiting_tree
        && owner->is_inside_tree();
    return synchronizers_cache;
}

bool NetwEntity::governs_property(
    const NodePath &p_real_path,
    Node *p_exclude
) const {
    NetwMultiplayer *core = session_core();
    if (core == nullptr) {
        return false;
    }
    return core->entity_governs_property(
        get_owner(),
        p_real_path,
        p_exclude,
        get_rid_handle()
    );
}

void NetwEntity::invalidate_synchronizers_cache() {
    synchronizers_dirty = true;
    synchronizers_absent = false;
    Node *owner = get_owner();
    if (owner != nullptr) {
        synchronizers::clear_cache(owner);
    }
}

Ref<NetwEntity> NetwEntity::parent_entity() const {
    NetwMultiplayer *core = session_core();
    if (core == nullptr || get_owner() == nullptr) {
        return Ref<NetwEntity>();
    }
    return as_entity(core->entity_get_view(
        core->entity_parent_of(get_record()->get_handle())
    ));
}

void NetwEntity::_bind_methods() {
    ClassDB::bind_static_method(
        "NetwEntity",
        D_METHOD("of", "node"),
        &NetwEntity::of
    );

    ClassDB::bind_method(D_METHOD("get_root"), &NetwEntity::get_owner);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "root",
            PROPERTY_HINT_NODE_TYPE,
            "Node",
            PROPERTY_USAGE_DEFAULT,
            "Node"
        ),
        "",
        "get_root"
    );

    ClassDB::bind_method(D_METHOD("get_name"), &NetwEntity::get_name);
    ClassDB::bind_method(D_METHOD("set_name", "name"), &NetwEntity::set_name);
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING_NAME, "name"),
        "set_name",
        "get_name"
    );

    ClassDB::bind_method(D_METHOD("get_peer_id"), &NetwEntity::get_peer_id);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "peer_id"), "", "get_peer_id");

    ClassDB::bind_method(D_METHOD("get_rid"), &NetwEntity::get_rid_handle);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::RID,
            "rid",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_rid"
    );

    ClassDB::bind_method(
        D_METHOD("get_control_handler"),
        &NetwEntity::get_control_handler
    );
    ClassDB::bind_method(
        D_METHOD("set_control_handler", "handler"),
        &NetwEntity::set_control_handler
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "control_handler"),
        "set_control_handler",
        "get_control_handler"
    );
    ClassDB::bind_method(
        D_METHOD("get_lifecycle_handler"),
        &NetwEntity::get_lifecycle_handler
    );
    ClassDB::bind_method(
        D_METHOD("set_lifecycle_handler", "handler"),
        &NetwEntity::set_lifecycle_handler
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::CALLABLE, "lifecycle_handler"),
        "set_lifecycle_handler",
        "get_lifecycle_handler"
    );

    ClassDB::bind_method(D_METHOD("get_transfer"), &NetwEntity::get_transfer);
    ClassDB::bind_method(
        D_METHOD("set_transfer", "value"),
        &NetwEntity::set_transfer
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "transfer",
            PROPERTY_HINT_ENUM,
            "Fixed,Requestable,Immediate",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEntity.Transfer"
        ),
        "set_transfer",
        "get_transfer"
    );

    ClassDB::bind_method(D_METHOD("get_authority"), &NetwEntity::get_authority);
    ClassDB::bind_method(
        D_METHOD("set_authority", "value"),
        &NetwEntity::set_authority
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "authority",
            PROPERTY_HINT_ENUM,
            "Server,Controller",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEntity.Authority"
        ),
        "set_authority",
        "get_authority"
    );

    ClassDB::bind_method(
        D_METHOD("get_on_controller_disconnect"),
        &NetwEntity::get_on_controller_disconnect
    );
    ClassDB::bind_method(
        D_METHOD("set_on_controller_disconnect", "value"),
        &NetwEntity::set_on_controller_disconnect
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "on_controller_disconnect",
            PROPERTY_HINT_ENUM,
            "Revert To Server,Despawn",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEntity.DisconnectRule"
        ),
        "set_on_controller_disconnect",
        "get_on_controller_disconnect"
    );

    ClassDB::bind_method(
        D_METHOD("get_on_parent_despawn"),
        &NetwEntity::get_on_parent_despawn
    );
    ClassDB::bind_method(
        D_METHOD("set_on_parent_despawn", "value"),
        &NetwEntity::set_on_parent_despawn
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "on_parent_despawn",
            PROPERTY_HINT_ENUM,
            "Cascade,Detach",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEntity.ParentDespawnRule"
        ),
        "set_on_parent_despawn",
        "get_on_parent_despawn"
    );

    ClassDB::bind_method(D_METHOD("get_lifecycle"), &NetwEntity::get_lifecycle);
    ClassDB::bind_method(
        D_METHOD("set_lifecycle", "value"),
        &NetwEntity::set_lifecycle
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "lifecycle",
            PROPERTY_HINT_ENUM,
            "Session,Controller",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEntity.Lifecycle"
        ),
        "set_lifecycle",
        "get_lifecycle"
    );

    ClassDB::bind_method(
        D_METHOD("get_controller"),
        &NetwEntity::get_controller_player
    );
    ClassDB::bind_method(
        D_METHOD("set_controller", "player"),
        &NetwEntity::set_controller_player
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "controller",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPlayer",
            PROPERTY_USAGE_NONE
        ),
        "set_controller",
        "get_controller"
    );

    ClassDB::bind_method(
        D_METHOD("get_is_controlled_locally"),
        &NetwEntity::get_is_controlled_locally
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::BOOL,
            "is_controlled_locally",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_is_controlled_locally"
    );

    ClassDB::bind_method(
        D_METHOD("claim_control", "hold"),
        &NetwEntity::claim_control,
        DEFVAL(entity::Control::HOLD_EXCLUSIVE)
    );
    ClassDB::bind_method(
        D_METHOD("release_control", "successor"),
        &NetwEntity::release_control,
        DEFVAL(entity::Control::SERVER)
    );
    ClassDB::bind_method(D_METHOD("get_hold"), &NetwEntity::get_hold);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "hold",
            PROPERTY_HINT_ENUM,
            "None,Soft,Exclusive",
            PROPERTY_USAGE_NONE | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwEntity.Hold"
        ),
        String(),
        "get_hold"
    );
    ClassDB::bind_method(
        D_METHOD("get_is_control_pending"),
        &NetwEntity::get_is_control_pending
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::BOOL,
            "is_control_pending",
            PROPERTY_HINT_NONE,
            "",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_is_control_pending"
    );

    ClassDB::bind_method(
        D_METHOD("set_replication_priority", "weight"),
        &NetwEntity::set_replication_priority
    );
    ClassDB::bind_method(
        D_METHOD("get_replication_priority"),
        &NetwEntity::get_replication_priority
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::FLOAT,
            "replication_priority",
            PROPERTY_HINT_RANGE,
            "0.001,1000,0.001,or_greater"
        ),
        "set_replication_priority",
        "get_replication_priority"
    );

    ClassDB::bind_method(D_METHOD("get_player"), &NetwEntity::get_player);
    ClassDB::bind_method(
        D_METHOD("set_player", "player"),
        &NetwEntity::set_player
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "player",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPlayer"
        ),
        "set_player",
        "get_player"
    );

    ClassDB::bind_method(
        D_METHOD("despawn", "opts"),
        &NetwEntity::despawn,
        DEFVAL(Ref<NetwDespawnOpts>())
    );
    ClassDB::bind_method(
        D_METHOD("_handle_tree_entered"),
        &NetwEntity::_handle_tree_entered
    );
    ClassDB::bind_method(
        D_METHOD("_handle_tree_exiting"),
        &NetwEntity::_handle_tree_exiting
    );
    ClassDB::bind_method(
        D_METHOD("_on_owner_ready"),
        &NetwEntity::_on_owner_ready
    );
    ClassDB::bind_method(D_METHOD("get_interest"), &NetwEntity::get_interest);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "interest",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwInterest",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_interest"
    );
    ClassDB::bind_method(D_METHOD("get_scene"), &NetwEntity::get_scene);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "scene",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwScene",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_scene"
    );
    ClassDB::bind_method(
        D_METHOD("get_prediction"),
        &NetwEntity::get_prediction
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "prediction",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPrediction",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_prediction"
    );
    ClassDB::bind_method(
        D_METHOD("get_simulation"),
        &NetwEntity::get_simulation
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "simulation",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwSimulation",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_simulation"
    );
    ClassDB::bind_method(
        D_METHOD("get_persistence"),
        &NetwEntity::get_persistence
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "persistence",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwPersistence",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_persistence"
    );
    ClassDB::bind_method(D_METHOD("get_display"), &NetwEntity::get_display);
    ADD_PROPERTY(
        PropertyInfo(
            Variant::OBJECT,
            "display",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwDisplay",
            PROPERTY_USAGE_NONE
        ),
        "",
        "get_display"
    );

    ADD_SIGNAL(
        MethodInfo(SIG_DESPAWNING, PropertyInfo(Variant::STRING_NAME, "reason"))
    );
    ADD_SIGNAL(MethodInfo(SIG_DESPAWNED));
    ADD_SIGNAL(MethodInfo(SIG_HIDDEN));
    ADD_SIGNAL(MethodInfo(
        SIG_CONTROL_CHANGED,
        PropertyInfo(Variant::INT, "previous_peer"),
        PropertyInfo(Variant::INT, "peer")
    ));
    ADD_SIGNAL(MethodInfo(
        SIG_LIFECYCLE_REFUSED,
        PropertyInfo(
            Variant::OBJECT,
            "request",
            PROPERTY_HINT_RESOURCE_TYPE,
            "NetwLifecycleRequest"
        )
    ));
    ADD_SIGNAL(MethodInfo(SIG_REPARENTED));

    ClassDB::bind_integer_constant(
        get_class_static(),
        "Authority",
        "AUTHORITY_SERVER",
        int(entity::Control::Authority::SERVER)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Authority",
        "AUTHORITY_CONTROLLER",
        int(entity::Control::Authority::CONTROLLER)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Transfer",
        "TRANSFER_FIXED",
        int(entity::Control::Transfer::FIXED)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Transfer",
        "TRANSFER_REQUESTABLE",
        int(entity::Control::Transfer::REQUESTABLE)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Transfer",
        "TRANSFER_IMMEDIATE",
        int(entity::Control::Transfer::IMMEDIATE)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Hold",
        "HOLD_NONE",
        entity::Control::HOLD_NONE
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Hold",
        "HOLD_SOFT",
        entity::Control::HOLD_SOFT
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Hold",
        "HOLD_EXCLUSIVE",
        entity::Control::HOLD_EXCLUSIVE
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "DisconnectRule",
        "DISCONNECT_REVERT_TO_SERVER",
        int(entity::Control::DisconnectRule::REVERT_TO_SERVER)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "DisconnectRule",
        "DISCONNECT_DESPAWN",
        int(entity::Control::DisconnectRule::DESPAWN)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "ParentDespawnRule",
        "PARENT_DESPAWN_CASCADE",
        int(entity::Control::ParentDespawnRule::CASCADE)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "ParentDespawnRule",
        "PARENT_DESPAWN_DETACH",
        int(entity::Control::ParentDespawnRule::DETACH)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Lifecycle",
        "LIFECYCLE_SERVER",
        int(entity::Control::Lifecycle::SESSION)
    );
    ClassDB::bind_integer_constant(
        get_class_static(),
        "Lifecycle",
        "LIFECYCLE_CONTROLLER",
        int(entity::Control::Lifecycle::CONTROLLER)
    );
}

} // namespace netw
