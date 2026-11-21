#include "netw/api/entity.hpp"

#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "godot/utility.hpp"
#include "netw/api/display.hpp"
#include "netw/game_call.hpp"
#include "netw/api/interest.hpp"
#include "netw/api/lifecycle_request.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/persistence.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/simulation.hpp"
#include "netw/colors.hpp"
#include "netw/entity/control.hpp"
#include "netw/entity/ids.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/interest/decl.hpp"
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

Callable *part_factories() {
    static Callable table[NetwEntityRecord::PART_MAX];
    return table;
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
    for (const NetwEntityRecord::Part part : parts) {
        Object *given = gd::live_object(p_stand_in->minted_part(part));
        if (given == nullptr) {
            continue;
        }
        Object *initial = gd::live_object(fresh->part(part));
        Object *into = gd::live_object(p_target->part(part));
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
            p_core->lifecycle_local_facts(
                p_entity->get_record(),
                p_entity->get_owner(),
                p_entity->synchronizers(),
                p_kind
            ),
            String(p_verb),
            p_entity->get_owner(),
            refusal
        )
        .admitted();
}

void collect_despawning_parts(
    Node *p_node,
    LocalVector<Ref<NetwEntity>> &r_parts
) {
    for (int at = 0; at < p_node->get_child_count(); ++at) {
        Node *child = p_node->get_child(at);
        const Ref<NetwEntity> part = tree::entity_object_at(child) != nullptr
            ? NetwEntity::of(child)
            : Ref<NetwEntity>();
        if (part.is_valid() && part->get_route() > 0
            && part->get_on_parent_despawn()
                == NetwEntity::PARENT_DESPAWN_DETACH) {
            continue;
        }
        collect_despawning_parts(child, r_parts);
        if (part.is_valid()) {
            r_parts.push_back(part);
        }
    }
}

} // namespace

NetwEntity::NetwEntity() : draft(memnew(NetwEntityRecord)) {
}

NetwEntity::~NetwEntity() {
    if (NetwMultiplayer::is_tearing_down(session_id)) {
        session_id = ObjectID();
    }
    abandon_control_claims();
    NetwMultiplayer::control_structure_refuse(
        *get_record(),
        0,
        ERR_UNAVAILABLE,
        String("the entity left before the session authority decided")
    );
    tree::unmark(uint64_t(owner_id), tree::MARK_ENTITY);
    NetwMultiplayer *const seated = home != nullptr ? home : session_core();
    if (seated != nullptr && get_record() != nullptr) {
        seated->predict_forget(*get_record());
    }
    if (home != nullptr) {
        if (NetwEntityRecord *const record = vacate()) {
            memdelete(record);
        }
    }
    if (draft != nullptr) {
        godot::memdelete(draft);
    }
}

NetwEntityRecord *NetwEntity::get_record() const {
    return home != nullptr ? home->entity_record(attached) : draft;
}

NetwEntityRecord *NetwEntity::record_in(
    const NetwMultiplayer *p_session
) const {
    if (p_session == nullptr) {
        return get_record();
    }
    if (home == p_session) {
        return p_session->entity_record(attached);
    }
    return home == nullptr ? draft : nullptr;
}

RID NetwEntity::attach(NetwMultiplayer *p_session) {
    if (home == p_session) {
        return attached;
    }
    NetwMultiplayer *const held = home != nullptr ? home : session_core();
    if (held != nullptr && held != p_session) {
        fault::raise(
            held,
            NETW_FAULT_ENTITY_MOVED,
            {String(get_name()),
             String("a second session"),
             String("a despawn there and a spawn in the new session")},
            uint64_t(gd::instance_id(get_owner()))
        );
        return RID();
    }
    const RID entity = p_session->slot_take_draft(draft, this);
    draft = nullptr;
    home = p_session;
    attached = entity;
    return entity;
}

bool NetwEntity::rekey(NetwMultiplayer *p_session, const RID &p_entity) {
    if (p_session == nullptr || home != p_session) {
        return false;
    }
    if (attached == p_entity) {
        return true;
    }
    if (!p_session->slot_rekey(attached, p_entity)) {
        return false;
    }
    if (scene_subject == attached) {
        scene_subject = p_entity;
    }
    attached = p_entity;
    return true;
}

void NetwEntity::detach(NetwMultiplayer *p_session) {
    if (p_session == nullptr || home != p_session) {
        return;
    }
    if (NetwEntityRecord *const record = vacate()) {
        draft = record;
    }
}

NetwEntityRecord *NetwEntity::vacate() {
    NETW_BUG_COND_V(home->slot_holder(attached) != this, nullptr);
    NetwEntityRecord *const record = home->slot_vacate(attached);
    home = nullptr;
    attached = RID();
    return record;
}

RID NetwEntity::adopt_in(NetwMultiplayer *p_session) {
    if (draft != nullptr && !draft->get_handle().is_valid()) {
        const RID minted = entity::mint();
        draft->adopt_handle(minted);
        entity::release(minted);
    }
    const RID handle = get_rid_handle();
    if (!handle.is_valid()) {
        return RID();
    }
    if (p_session->entity_get_state(handle)
        == NetwMultiplayer::ENTITY_STATE_DEAD) {
        return handle;
    }
    p_session->slot_adopt_view(
        handle,
        this,
        NetwMultiplayer::owner_of_wrapper(this)
    );
    return handle;
}

bool NetwEntity::bind_route(
    NetwMultiplayer *p_session,
    const RID &p_entity,
    int64_t p_route,
    Node *p_owner
) {
    if (!attach(p_session).is_valid()) {
        return false;
    }
#if defined(NETW_TESTS)
    if (attached != p_entity && p_session->slot_holds_unmoved(attached)) {
        NETW_BUG_REPORT();
    }
#endif
    if (!rekey(p_session, p_entity)) {
        return false;
    }
    if (!p_session->liveness_admit(p_entity)) {
        return false;
    }
    if (!p_session->slot_bind_route(p_entity, p_route)) {
        return false;
    }
    p_session->slot_adopt_view(p_entity, this, p_owner);
    return true;
}

void NetwEntity::announce_reparented(NetwMultiplayer *p_session) {
    Node *owner = get_owner();
    if (owner == nullptr || !owner->is_inside_tree()) {
        return;
    }
    NetwEntityRecord *const record = record_in(p_session);
    p_session->event_record(
        EventPlane::REPARENTED,
        get_rid_handle(),
        record != nullptr ? record->get_route() : 0,
        record != nullptr ? record->get_peer_id() : 0
    );
    session::Announcement row;
    row.kind = session::AnnouncementKind::REPARENTED;
    NetwMultiplayer::entity_announce(this, row);
}

void NetwEntity::adopt_root(Node *p_root) {
    if (p_root == nullptr) {
        return;
    }
    owner_id = gd::instance_id(p_root);
    p_root->set_meta(NetwMultiplayer::wrapper_meta(), Ref<NetwEntity>(this));
    tree::mark(p_root, tree::MARK_ENTITY);
    if (p_root->is_inside_tree()) {
        book_root(p_root, NetwMultiplayer::session_holding(p_root));
        callable_mp(this, &NetwEntity::_handle_tree_entered).call_deferred();
    }
}

void NetwEntity::book_root(Node *p_root, NetwMultiplayer *p_session) {
    if (p_session == nullptr) {
        return;
    }
    p_session->tree_book(p_root, this);
}

void NetwEntity::release_root(Node *p_root) {
    p_root->set_meta(NetwMultiplayer::wrapper_meta(), Ref<NetwEntity>(this));
    NetwMultiplayer::tree_release(p_root);
}

void NetwEntity::set_part_factory(int64_t p_part, const Callable &p_factory) {
    NETW_BUG_COND(p_part < 0 || p_part >= NetwEntityRecord::PART_MAX);
    part_factories()[p_part] = p_factory;
}

void NetwEntity::clear_part_factories() {
    for (int at = 0; at < NetwEntityRecord::PART_MAX; at++) {
        part_factories()[at] = Callable();
    }
}

Variant NetwEntity::minted_part(int64_t p_part) const {
    for (const MintedPart &held : parts) {
        if (held.part == p_part) {
            return held.value;
        }
    }
    return Variant();
}

Variant NetwEntity::part(int64_t p_part) {
    NETW_BUG_COND_V(
        p_part < 0 || p_part >= NetwEntityRecord::PART_MAX,
        Variant()
    );
    const Variant held = minted_part(p_part);
    if (Ref<RefCounted>(held).is_valid()) {
        return held;
    }
    const Callable &factory = part_factories()[p_part];
    if (!factory.is_valid()) {
        NETW_TRACE(
            sys::ENTITY,
            "No factory is registered for entity part %d.",
            int(p_part)
        );
        return Variant();
    }
    const Variant made = factory.call(this);
    const Ref<RefCounted> minted = made;
    NETW_BUG_COND_V(minted.is_null(), Variant());
    NETW_TRACE(sys::ENTITY, "Made entity part %d.", int(p_part));
    keep_part(p_part, made);
    return made;
}

void NetwEntity::keep_part(int64_t p_part, const Variant &p_value) {
    for (MintedPart &slot : parts) {
        if (slot.part == p_part) {
            slot.value = p_value;
            return;
        }
    }
    MintedPart minted_now;
    minted_now.part = p_part;
    minted_now.value = p_value;
    parts.push_back(minted_now);
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

Ref<NetwEntity> NetwEntity::stamp_header(
    Node *p_node,
    const Dictionary &p_header,
    NetwMultiplayer *p_session
) {
    const Ref<NetwEntity> entity = ensure(p_node);
    if (entity.is_null()) {
        return entity;
    }
    entity->set_name(p_header.get(StringName("entity_id"), StringName()));
    entity->set_peer_id(int64_t(p_header.get(StringName("peer_id"), 0)));
    entity->set_player_id(
        p_session != nullptr && p_session->is_host()
            ? p_session->player_incarnation(entity->get_peer_id())
            : int64_t(p_header.get(StringName("player_id"), 0))
    );
    entity->get_record()
        ->get_control()
        ->set_authority(
            bool(p_header.get(StringName("controller_authority"), false))
                ? int64_t(AUTHORITY_CONTROLLER)
                : int64_t(AUTHORITY_SERVER)
        );
    entity->write_controller(
        int64_t(p_header.get(StringName("controller"), 0))
    );
    entity->seed_control(
        uint64_t(int64_t(p_header.get(StringName("control_revision"), 0))),
        uint64_t(int64_t(p_header.get(StringName("control_tenure"), 0))),
        int64_t(p_header.get(StringName("control_hold"), 0))
    );
    entity->set_action_spawn_tick(
        int64_t(p_header.get(StringName("action_spawn_tick"), -1))
    );
    entity->set_action_requester(
        int64_t(p_header.get(StringName("action_requester"), 0))
    );
    entity->comp_table().set_wire_hash(
        int64_t(p_header.get(StringName("wire_hash"), 0))
    );
    if (entity->comp_table().get_table_hash() != 0) {
        entity->comp_table().reconcile(entity->is_on_server());
    }
    entity->set_declares_scene(
        bool(p_header.get(StringName("declares_scene"), false))
    );
    entity->set_scene_label(
        p_header.get(StringName("scene_label"), StringName())
    );
    entity->set_route(int64_t(p_header.get(StringName("route"), 0)));
    if (entity->get_stage() == int64_t(netw::entity::Stage::UNBOUND)) {
        entity->arm(p_session);
    }
    return entity;
}

bool NetwEntity::belongs_to(NetwMultiplayer *p_branch) const {
    NetwMultiplayer *core = session_core();
    return p_branch != nullptr && (core == nullptr || core == p_branch);
}

bool NetwEntity::readmit() {
    const Ref<NetwEntity> keep(this);
    abandon_control_claims();
    const int64_t from = get_stage();
    owner_exiting_tree = false;
    despawning_watched = false;
    scene_subject = RID();
    synchronizers_dirty = true;
    synchronizers_absent = false;
    ready_once_fired = true;
    detach(home);
    draft->readmit();
    despawning_opts = Ref<NetwDespawnOpts>();
    if (NetwMultiplayer *core = session_core()) {
        attach(core);
    }
    if (!publish_record_stage(*get_record(), from)) {
        return true;
    }
    callable_mp(this, &NetwEntity::readmitted_ready).call_deferred();
    NetwMultiplayer *core = session_core();
    Node *owner = get_owner();
    NetwEntityRecord *const record = get_record();
    if (spawn::Record *readmission = record->take_readmission()) {
        const spawn::Record recipe = *readmission;
        memdelete(readmission);
        if (core != nullptr && owner != nullptr && core->is_host()
            && core->spawn_readmit(owner, recipe)) {
            return true;
        }
    }
    if (core != nullptr) {
        arm(Ref<NetwMultiplayer>(core));
    }
    return false;
}

void NetwEntity::readmitted_ready() {
    const Ref<NetwEntity> keep(this);
    Node *owner = get_owner();
    if (!ready_once_fired || owner == nullptr || !owner->is_inside_tree()
        || get_stage() != int64_t(entity::Stage::LIVE)) {
        return;
    }
    ready_once_fired = false;
    _on_owner_ready();
    NetwEntityRecord *const held = get_record();
    if (!held->get_predicted_at_departure()) {
        return;
    }
    held->set_predicted_at_departure(false);
    if (NetwMultiplayer *core = session_core()) {
        core->predict_readmit(*held, owner);
    }
}

void NetwEntity::tree_entered() {
    const Ref<NetwEntity> keep(this);
    const bool respawned
        = get_stage() == int64_t(entity::Stage::FREED) && readmit();
    NetwMultiplayer *core = session_core();
    const int64_t route = get_route();
    const bool hidden = !respawned && core != nullptr && route > 0
        && get_stage() == int64_t(entity::Stage::LIVE)
        && core->liveness_route_state(route)
            == NetwMultiplayer::ENTITY_STATE_ABSENT;
    if (!respawned) {
        _handle_tree_entered();
    }
    Node *owner = get_owner();
    if (owner == nullptr || owner->is_queued_for_deletion()
        || entity::stage_is_leaving(get_stage())) {
        return;
    }
    if (hidden && ready_once_fired) {
        callable_mp(this, &NetwEntity::readmitted_ready).call_deferred();
    }
    const interest::Facet *facet = get_record()->find_interest_facet();
    if (facet != nullptr && facet->declaration()->has_labels()) {
        NetwMultiplayer *holding = NetwMultiplayer::session_holding(owner);
        NetwEntityRecord *held
            = holding != nullptr ? record_in(holding) : nullptr;
        if (held != nullptr) {
            holding->interest_activate(get_rid_handle(), *held);
        }
    }
    core = session_core();
    if (core != nullptr && scene_subject.is_valid() && get_owner() != nullptr) {
        core->scene_report_entity_edge(scene_subject, true, get_peer_id() != 0);
    }
}

void NetwEntity::tree_exiting() {
    const Ref<NetwEntity> keep(this);
    _handle_tree_exiting();
    const interest::Facet *facet = get_record()->find_interest_facet();
    if (facet != nullptr && facet->declaration()->has_labels()) {
        NetwMultiplayer *holding = NetwMultiplayer::session_holding(
            get_owner(),
            NetwMultiplayer::TreeState::EXITING
        );
        NetwEntityRecord *held
            = holding != nullptr ? record_in(holding) : nullptr;
        if (held != nullptr) {
            holding->interest_deactivate(get_rid_handle(), *held);
        }
    }
    NetwMultiplayer *core = session_core();
    if (core != nullptr && scene_subject.is_valid() && get_owner() != nullptr) {
        core->scene_report_entity_edge(
            scene_subject,
            false,
            get_peer_id() != 0
        );
    }
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
    if (p_api.is_null() || !attach(p_api.ptr()).is_valid()) {
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
    set_control_param(
        NetwMultiplayer::ENTITY_PARAM_TRANSFER,
        int64_t(p_value)
    );
}

NetwEntity::Authority NetwEntity::get_authority() const {
    return Authority(control()->get_authority());
}

void NetwEntity::set_authority(Authority p_value) {
    set_control_param(
        NetwMultiplayer::ENTITY_PARAM_AUTHORITY,
        int64_t(p_value)
    );
}

NetwEntity::DisconnectRule NetwEntity::get_on_controller_disconnect() const {
    return DisconnectRule(control()->get_on_disconnect());
}

void NetwEntity::set_on_controller_disconnect(DisconnectRule p_value) {
    set_control_param(
        NetwMultiplayer::ENTITY_PARAM_ON_CONTROLLER_DISCONNECT,
        int64_t(p_value)
    );
}

NetwEntity::ParentDespawnRule NetwEntity::get_on_parent_despawn() const {
    return ParentDespawnRule(control()->get_on_parent_despawn());
}

void NetwEntity::set_on_parent_despawn(ParentDespawnRule p_value) {
    set_control_param(
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
    return NetwMultiplayer::entity_scene_label(*get_record(), get_owner());
}

void NetwEntity::set_scene_label(const StringName &p_value) {
    get_record()->set_scene_label(p_value);
}

int64_t NetwEntity::get_scene_isolation() const {
    return NetwMultiplayer::entity_scene_isolation(*get_record(), get_owner());
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
    write_controller(
        NetwMultiplayer::control_steerer_of_peer(session_core(), p_value)
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
        write_controller(steered);
        return;
    }
    transfer_control_to(steered);
}

Error NetwEntity::transfer_control(int64_t p_peer) {
    return home != nullptr ? home->control_give(attached, p_peer)
                           : give_control_here(p_peer);
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
    set_control_param(
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
        write_controller(peer);
    } else if (peer == 0 && steers_previous) {
        control()->clear_controller();
        emit_control_change(previous, entity::Control::SERVER);
    }
}

bool NetwEntity::get_is_player() const {
    return get_peer_id() != 0;
}

NetwEntity::Stage NetwEntity::get_stage() const {
    return Stage(get_record()->get_stage());
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

bool NetwEntity::publish_record_stage(
    NetwEntityRecord &p_record,
    int64_t p_from
) {
    if (p_from == p_record.get_stage()) {
        return true;
    }
    if (get_record() == &p_record) {
        return publish_stage(p_from, p_record.get_stage());
    }
    return true;
}

bool NetwEntity::begin_despawn(
    NetwEntityRecord &p_record,
    const Ref<NetwDespawnOpts> &p_opts
) {
    const Ref<NetwEntity> keep(this);
    if (!entity::stage_can_begin_despawn(p_record.get_stage())) {
        return false;
    }
    Ref<NetwDespawnOpts> opts = p_opts;
    if (opts.is_null()) {
        opts.instantiate();
    }
    despawning_opts = opts;
    p_record.set_despawn_reason(opts->get_reason());
    const int64_t from = p_record.get_stage();
    p_record.transition(int64_t(entity::Stage::DESPAWNING));
    if (!publish_record_stage(p_record, from)) {
        despawning_opts = Ref<NetwDespawnOpts>();
        return false;
    }
    NETW_TRACE(sys::ENTITY, "Despawning because of '%s'.", opts->get_reason());
    const ObjectID owner = gd::instance_id(get_owner());
    const Ref<MultiplayerAPI> stamped = get_multiplayer();
    const RID captured_handle = p_record.get_handle();
    tree::despawning(this);
    if (get_record() == &p_record && p_record.get_handle() == captured_handle
        && get_multiplayer() == stamped
        && gd::instance_id(get_owner()) == owner
        && p_record.get_stage() == int64_t(entity::Stage::DESPAWNING)) {
        session::Announcement row;
        row.kind = session::AnnouncementKind::DESPAWNING;
        row.reason = opts->get_reason();
        NetwMultiplayer::entity_announce(this, row);
    }
    despawning_opts = Ref<NetwDespawnOpts>();
    return true;
}

bool NetwEntity::finish_teardown(NetwEntityRecord &p_record) {
    const Ref<NetwEntity> keep(this);
    const int64_t from = p_record.get_stage();
    if (!p_record.advance(int64_t(entity::Stage::FREED))) {
        return false;
    }
    if (!publish_record_stage(p_record, from)) {
        return true;
    }
    session::Announcement row;
    row.kind = session::AnnouncementKind::DESPAWNED;
    NetwMultiplayer::entity_announce(this, row);
    return true;
}

bool NetwEntity::complete_departure(NetwEntityRecord &p_record) {
    const Ref<NetwEntity> keep(this);
    if (p_record.get_stage() == int64_t(entity::Stage::FREED)) {
        return false;
    }
    if (entity::stage_can_begin_despawn(p_record.get_stage())) {
        const int64_t from = p_record.get_stage();
        p_record.advance(int64_t(entity::Stage::DESPAWNING));
        if (!publish_record_stage(p_record, from)) {
            return true;
        }
    }
    return finish_teardown(p_record);
}

void NetwEntity::emit_announcement(const session::Announcement &p_row) {
    switch (p_row.kind) {
        case session::AnnouncementKind::CONTROL_CHANGED:
            emit_signal(StringName("control_changed"), p_row.from, p_row.to);
            break;
        case session::AnnouncementKind::DESPAWNING:
            emit_signal(StringName("despawning"), StringName(p_row.reason));
            break;
        case session::AnnouncementKind::DESPAWNED:
            emit_signal(StringName("despawned"));
            break;
        case session::AnnouncementKind::LIFECYCLE_REFUSED: {
            const Ref<NetwLifecycleRequest> request
                = NetwLifecycleRequest::create(
                    lifecycle::Kind(p_row.code),
                    p_row.requester,
                    Ref<NetwEntity>(this),
                    Object::cast_to<Node>(gd::object_of(p_row.node))
                );
            request->deny(
                p_row.reason,
                p_row.refusal != OK ? p_row.refusal : ERR_UNAUTHORIZED
            );
            emit_signal(StringName("lifecycle_refused"), request);
        } break;
        case session::AnnouncementKind::REPARENTED:
            emit_signal(StringName("reparented"));
            break;
        case session::AnnouncementKind::HIDDEN:
            emit_signal(StringName("hidden"));
            break;
        case session::AnnouncementKind::MODE_CHANGED: {
            const Ref<NetwSimulation> handle = simulation_if_minted();
            if (handle.is_valid()) {
                handle->emit_signal(
                    StringName("mode_changed"),
                    NetwSimulation::Mode(int(p_row.from)),
                    NetwSimulation::Mode(int(p_row.to))
                );
            }
        } break;
        case session::AnnouncementKind::STATE_EVALUATED:
        case session::AnnouncementKind::EPISODE_CHANGED:
        case session::AnnouncementKind::RECOVERED:
        case session::AnnouncementKind::DIVERGENCE_DETECTED:
            NetwMultiplayer::prediction_emit(get_prediction().ptr(), p_row);
            break;
        case session::AnnouncementKind::PERSIST_LOADED:
        case session::AnnouncementKind::PERSIST_SAVED: {
            const NetwEntityRecord *record = get_record();
            const Ref<NetwPersistence> view = record != nullptr
                ? Ref<NetwPersistence>(
                      minted_part(NetwEntityRecord::PART_PERSISTENCE)
                  )
                : Ref<NetwPersistence>();
            if (view.is_valid()
                && p_row.kind == session::AnnouncementKind::PERSIST_LOADED) {
                view->announce_loaded(p_row.entity, p_row.flag);
            } else if (view.is_valid()) {
                view->announce_saved(p_row.entity);
            }
        } break;
        default:
            break;
    }
}

void NetwEntity::transition(int64_t p_stage) {
    const int64_t from = get_stage();
    if (get_record()->transition(p_stage)) {
        publish_record_stage(*get_record(), from);
    }
}

void NetwEntity::arm(const Ref<NetwMultiplayer> &p_api) {
    const Ref<NetwEntity> keep(this);
    NETW_BUG_COND(get_stage() != int64_t(entity::Stage::UNBOUND));
    settle_parts(get_owner(), false);
    unwrite_player();
    stamp_multiplayer(p_api);
    Node *claimed = get_owner();
    if (claimed != nullptr && !control()->get_configured()
        && claimed->get_multiplayer_authority() != SERVER_PEER) {
        control()->infer_authority(int64_t(AUTHORITY_CONTROLLER));
    }
    if (get_transfer() == TRANSFER_IMMEDIATE) {
        control()->infer_authority(int64_t(AUTHORITY_CONTROLLER));
    }
    if (get_transfer() == TRANSFER_IMMEDIATE
        && !control()->authority_follows_controller()) {
        fault::raise(
            session_core(),
            NETW_FAULT_CONFLICTING_SETTINGS,
            {String("{NetwEntity.TRANSFER_IMMEDIATE}"),
             String(get_name()),
             String("{NetwEntity.AUTHORITY_SERVER}")},
            uint64_t(gd::instance_id(get_owner()))
        );
        control()->set_transfer(int64_t(TRANSFER_REQUESTABLE));
    }
    if (!control()->get_configured()) {
        adopt_controller(
            NetwMultiplayer::control_default_controller(*get_record(), claimed)
        );
    } else {
        apply_control();
    }
    transition(int64_t(entity::Stage::ARMED));
}

void NetwEntity::_handle_tree_entered() {
    const Ref<NetwEntity> keep(this);
    NETW_ZONE_NC("NetwEntity tree entered", colors::LIVENESS);
    owner_exiting_tree = false;
    synchronizers_absent = false;
    refresh_control();
    Node *owner = get_owner();
    if (session_core() == nullptr) {
        stamp_multiplayer(NetwMultiplayer::session_holding(owner));
    }
    NetwMultiplayer::entity_enter_tree(
        this,
        owner,
        get_record(),
        NetwMultiplayer::session_holding(owner),
        is_on_server()
    );
}

void NetwEntity::hydrate_components() {
    Node *owner = get_owner();
    LocalVector<ObjectID> &registered = comp_registered;
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
             String("the replicated components of '") + String(get_name())
                 + String("'"),
             SERVER_PEER},
            uint64_t(gd::instance_id(owner))
        );
    }
    if (NetwMultiplayer *core = session_core()) {
        core->sync_pipeline_recapture_entity(get_rid_handle());
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
    const LocalVector<ObjectID> &registered = comp_registered;
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
    const Ref<NetwEntity> keep(this);
    if (ready_once_fired || get_stage() != int64_t(entity::Stage::LIVE)) {
        return;
    }
    ready_once_fired = true;
    NETW_ZONE_NC("NetwEntity owner ready", colors::LIVENESS);
    if (steering() != entity::Control::SERVER) {
        apply_control();
    }
    {
        NETW_ZONE_NC("NetwEntity hydrate components", colors::LIVENESS);
        hydrate_components();
    }
    NetwMultiplayer *core = session_core();
    if (core != nullptr
        && core->predict_lacks_state_rows(*get_record(), get_owner())) {
        Node *owner = get_owner();
        fault::raise(
            core,
            NETW_FAULT_PREDICTION_OFF,
            {owner != nullptr ? String(owner->get_path()) : String(get_name()),
             String(),
             String(),
             0,
             NETW_OFF_NO_STATE},
            uint64_t(gd::instance_id(owner))
        );
    }
    {
        NETW_ZONE_NC("NetwEntity persist and predict", colors::LIVENESS);
        if (core != nullptr && !core->persist_enroll(get_owner())) {
            core->predict_reconcile_declaration(*get_record(), get_owner());
        }
    }
    if (core != nullptr) {
        NETW_ZONE_NC("NetwEntity sim declare", colors::LIVENESS);
        core->sim_declare(get_rid_handle());
    }
    if (core != nullptr) {
        NETW_ZONE_NC("NetwEntity adopt effects", colors::LIVENESS);
        core->observe_entity_spawned(int64_t(get_instance_id()));
    }
}

void NetwEntity::_handle_tree_exiting() {
    const Ref<NetwEntity> keep(this);
    owner_exiting_tree = true;
    refresh_control();
    if (NetwMultiplayer *core = session_core()) {
        core->entity_capture_exit(this);
        return;
    }
    complete_departure(*get_record());
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
    despawn_admitted(p_opts);
}

void NetwEntity::despawn_admitted(const Ref<NetwDespawnOpts> &p_opts) {
    Ref<NetwDespawnOpts> opts = p_opts;
    if (opts.is_null()) {
        opts.instantiate();
    }
    NetwMultiplayer *api = session_core();
    if (api != nullptr && !api->is_host()) {
        api->entity_settle_captured(get_rid_handle());
        if (api->lifecycle_withhold_despawn(get_rid_handle(), opts)) {
            return;
        }
    }
    despawn_here(opts);
}

void NetwEntity::despawn_here(const Ref<NetwDespawnOpts> &p_opts) {
    const Ref<NetwDespawnOpts> opts = p_opts;
    NetwMultiplayer *api = session_core();
    Node *owner = get_owner();
    const Ref<NetwEntity> keep(this);
    const ObjectID captured_owner = owner_id;
    const ObjectID captured_session = session_id;
    const RID captured_handle = get_rid_handle();
    const auto unchanged = [&](int64_t p_stage) {
        return owner_id == captured_owner && session_id == captured_session
            && get_rid_handle() == captured_handle && get_stage() == p_stage;
    };
    if (api != nullptr && owner != nullptr
        && entity::stage_can_begin_despawn(get_stage())) {
        LocalVector<Ref<NetwEntity>> parts;
        collect_despawning_parts(owner, parts);
        for (const Ref<NetwEntity> &part : parts) {
            part->get_record()->set_despawn_reason(opts->get_reason());
            if (part->get_owner() != nullptr) {
                session::Announcement row;
                row.kind = session::AnnouncementKind::DESPAWNING;
                row.reason = opts->get_reason();
                NetwMultiplayer::entity_announce(part.ptr(), row);
            }
        }
    }
    if (!begin_despawn(*get_record(), opts)) {
        return;
    }
    if (!unchanged(int64_t(entity::Stage::DESPAWNING))) {
        return;
    }
    owner = get_owner();
    api = session_core();
    if (owner == nullptr || owner->is_queued_for_deletion()) {
        return;
    }
    if (api != nullptr) {
        api->spawn_detach_before_despawn(get_rid_handle());
        api->persist_depart(
            api->get_bindings()->find(owner),
            opts->get_flush_save()
        );
        api->scene_root_despawning(
            get_rid_handle(),
            api->is_host() && !opts->get_linger()
        );
    }
    if (owner->get_multiplayer_authority() != 1) {
        owner->set_multiplayer_authority(1);
    }
    if (opts->get_linger()) {
        transition(int64_t(entity::Stage::LINGERING));
        if (unchanged(int64_t(entity::Stage::LINGERING))) {
            linger_then_free(opts);
        }
        return;
    }
    if (opts->get_defer_free()) {
        Callable(owner, StringName("queue_free")).call_deferred();
    } else {
        owner->queue_free();
    }
}

void NetwEntity::linger_then_free(const Ref<NetwDespawnOpts> &p_opts) {
    Node *owner = get_owner();
    NetwMultiplayer *core = session_core();
    if (core == nullptr) {
        NetwMultiplayer::entity_deactivate(owner);
        if (owner != nullptr) {
            owner->queue_free();
        }
        return;
    }
    core->entity_linger(
        get_record(),
        owner,
        core->linger_pumps(p_opts->get_linger_seconds())
    );
}

void NetwEntity::_remote_hide() {
    session::Announcement row;
    row.kind = session::AnnouncementKind::HIDDEN;
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
    LocalVector<ObjectID> &registered = comp_registered;
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
    return comp_in(*get_record(), get_owner(), p_node);
}

String NetwEntity::comp_path_of(Node *p_node) const {
    return comp_path_in(*get_record(), get_owner(), p_node);
}

int64_t NetwEntity::comp_in(
    const NetwEntityRecord &p_record,
    Node *p_owner,
    Node *p_node
) {
    if (p_node == nullptr || p_owner == nullptr || p_node == p_owner) {
        return 0;
    }
    const NodePath rel = NetwMultiplayer::relative_path(p_owner, p_node);
    const NetwCompTable &table = p_record.get_comp_table();
    if (table.get_poisoned() || !table.has_path(String(rel))) {
        return NetwCompTable::FALLBACK_COMP;
    }
    return table.id_for_path(String(rel));
}

String NetwEntity::comp_path_in(
    const NetwEntityRecord &p_record,
    Node *p_owner,
    Node *p_node
) {
    if (comp_in(p_record, p_owner, p_node) != NetwCompTable::FALLBACK_COMP) {
        return String();
    }
    return String(NetwMultiplayer::relative_path(p_owner, p_node));
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
    return const_cast<NetwEntity *>(this)->part(
        NetwEntityRecord::PART_INTEREST
    );
}

Ref<NetwInterest> NetwEntity::held_interest() const {
    return minted_part(NetwEntityRecord::PART_INTEREST);
}

Ref<NetwScene> NetwEntity::get_scene() const {
    NetwMultiplayer *core = session_core();
    if (core == nullptr || get_owner() == nullptr) {
        return Ref<NetwScene>();
    }
    return core->entity_scene_facet(get_record(), get_owner());
}

Ref<NetwPrediction> NetwEntity::get_prediction() const {
    return const_cast<NetwEntity *>(this)->part(
        NetwEntityRecord::PART_PREDICTION
    );
}

Ref<NetwSimulation> NetwEntity::get_simulation() const {
    return const_cast<NetwEntity *>(this)->part(
        NetwEntityRecord::PART_SIMULATION
    );
}

Ref<NetwPrediction> NetwEntity::prediction_if_minted() const {
    return minted_part(NetwEntityRecord::PART_PREDICTION);
}

Ref<NetwSimulation> NetwEntity::simulation_if_minted() const {
    return minted_part(NetwEntityRecord::PART_SIMULATION);
}

Ref<NetwPersistence> NetwEntity::get_persistence() const {
    return const_cast<NetwEntity *>(this)->part(
        NetwEntityRecord::PART_PERSISTENCE
    );
}

Ref<NetwDisplay> NetwEntity::get_display() const {
    return const_cast<NetwEntity *>(this)->part(NetwEntityRecord::PART_DISPLAY);
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
