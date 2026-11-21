#include "netw/api/interest.hpp"

#include <cstdlib>

#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/scene.hpp"

#include "netw/api/participant.hpp"

#include "godot/class_db.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/packed_scene.hpp"
#include "godot/physics_server.hpp"
#include "godot/rendering_server.hpp"
#include "godot/resource.hpp"
#include "godot/scene_tree.hpp"
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/entity/ids.hpp"
#include "netw/entity/stage.hpp"
#include "netw/lifecycle/rule.hpp"
#include "netw/log.hpp"
#include "netw/object_port.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/repl/sync_compat.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/script/model.hpp"
#include "netw/session/frames.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/synchronizers.hpp"
#include "netw/tree_hook.hpp"
#include "netw/wire/frame.hpp"

using namespace godot;
using namespace netw;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertyColumn;
using repl::PropertySet;

namespace {

void mint_handle(NetwEntityRecord *p_record) {
    const RID minted = entity::mint();
    p_record->adopt_handle(minted);
    entity::release(minted);
}

StringName departure_key(uint64_t p_entity) {
    return StringName(vformat("entity-departure?%d", int64_t(p_entity)));
}

template <typename K, typename V>
void move_key(HashMap<K, V> &r_map, uint64_t p_from, uint64_t p_to) {
    if (const typename HashMap<K, V>::Iterator held = r_map.find(K(p_from))) {
        const V value = held->value;
        r_map.remove(held);
        r_map.insert(K(p_to), value);
    }
}

Variant world_pose_of(Node *p_node) {
    if (p_node == nullptr || !p_node->is_inside_tree()) {
        return Variant();
    }
    if (Node3D *spatial = Object::cast_to<Node3D>(p_node)) {
        return spatial->get_global_transform();
    }
    if (Node2D *flat = Object::cast_to<Node2D>(p_node)) {
        return flat->get_global_transform();
    }
    return Variant();
}

void world_pose_restore(Node *p_node, const Variant &p_pose) {
    if (p_node == nullptr || !p_node->is_inside_tree()) {
        return;
    }
    if (p_pose.get_type() == Variant::TRANSFORM3D) {
        if (Node3D *spatial = Object::cast_to<Node3D>(p_node)) {
            spatial->set_global_transform(Transform3D(p_pose));
        }
    } else if (p_pose.get_type() == Variant::TRANSFORM2D) {
        if (Node2D *flat = Object::cast_to<Node2D>(p_node)) {
            flat->set_global_transform(Transform2D(p_pose));
        }
    }
}

} // namespace

session::EntityView &NetwMultiplayer::entity_view_row(uint64_t p_id) {
    world.entities.ensure(p_id).viewed = true;
    return views.entities.ensure(p_id);
}

RID NetwMultiplayer::slot_take_draft(
    NetwEntityRecord *p_record,
    NetwEntity *p_holder
) {
    if (!p_record->get_handle().is_valid()
        || entity_record(p_record->get_handle()) != nullptr) {
        mint_handle(p_record);
    }
    const RID entity = p_record->get_handle();
    world.entities.ensure(entity.get_id()).record = p_record;
    entity_view_row(entity.get_id()).holder = p_holder;
    return entity;
}

NetwEntity *NetwMultiplayer::slot_holder(const RID &p_entity) const {
    const session::EntityView *view = views.entities.find(p_entity.get_id());
    return world.entities.has(p_entity.get_id()) && view != nullptr
        ? view->holder
        : nullptr;
}

bool NetwMultiplayer::liveness_admit(const RID &p_entity) {
    return liveness_core.adopt(p_entity);
}

bool NetwMultiplayer::slot_bind_route(const RID &p_entity, int64_t p_route) {
    if (!liveness_core.bind_route(p_entity, int(p_route))) {
        return false;
    }
    world.entities.ensure(p_entity.get_id()).routed = true;
    return true;
}

void NetwMultiplayer::slot_adopt_view(
    const RID &p_entity,
    NetwEntity *p_wrapper,
    Object *p_owner
) {
    if (!p_entity.is_valid()) {
        return;
    }
    liveness_core.adopt(p_entity);
    EntityRow &slot = world.entities.ensure(p_entity.get_id());
    session::EntityView &view = entity_view_row(p_entity.get_id());
    view.live = Ref<NetwEntity>(p_wrapper);
    sim_follow_view(p_entity, true);
    view.retired = Ref<NetwEntity>();
    slot.swept = false;
    view.indexed = gd::instance_id(p_wrapper);
    if (p_owner != nullptr) {
        slot.node = uint64_t(gd::instance_id(p_owner));
    }
}

void NetwMultiplayer::tree_book(Node *p_root, NetwEntity *p_wrapper) {
    views.node_entities.insert(
        uint64_t(p_root->get_instance_id()),
        Ref<NetwEntity>(p_wrapper)
    );
    tree_adopt(p_root, this);
}

bool NetwMultiplayer::slot_rekey(const RID &p_from, const RID &p_to) {
    NETW_BUG_COND_V(world.entities.record(p_to.get_id()) != nullptr, false);
    if (!liveness_core.rekey(p_from, p_to)) {
        return false;
    }
    NetwEntityRecord *const record = entity_record(p_from);
    if (!record->adopt_handle(p_to)) {
        return false;
    }
    const uint64_t from = p_from.get_id();
    const uint64_t to = p_to.get_id();
    const session::EntityView held = *views.entities.find(from);
    EntityRow &moved = world.entities.ensure(to);
    moved.record = record;
    session::EntityView &arrived = entity_view_row(to);
    arrived.holder = held.holder;
    arrived.live = held.live;
    arrived.retired = held.retired;
    arrived.indexed = held.indexed;
    EntityRow *left = world.entities.find(from);
    moved.routed = left->routed;
    moved.node = left->node;
    moved.swept = left->swept;
    left->record = nullptr;
    left->routed = false;
    left->node = 0;
    left->swept = false;
    session::EntityView *vacated = views.entities.find(from);
    vacated->holder = nullptr;
    vacated->live = Ref<NetwEntity>();
    vacated->retired = Ref<NetwEntity>();
    vacated->indexed = ObjectID();
    move_key(announced_scene_of_entity, from, to);
    for (uint32_t at = announce_next; at < announcements.size(); at++) {
        Announcement &pending = announcements[at];
        if (pending.entity == p_from && slot_pin(p_to)) {
            pending.entity = p_to;
            slot_unpin(p_from);
        }
    }
    slot_settle(int64_t(from));
    move_key(lifecycle_withheld, from, to);
    move_key(views.departing, from, to);
    move_key(predict_engines, from, to);
    move_key(kept_entities, from, to);
    if (kept_by_server.erase(from)) {
        kept_by_server.insert(to);
    }
    move_key(control_host_gifts, from, to);
    if (HostGift *gift = control_host_gifts.getptr(to)) {
        gift->entity = p_to;
    }
    prediction_engine.rekey_member(p_from, p_to);
    display_book.rekey(p_from, p_to);
    const bool departs = entity_departures.has(from);
    move_key(entity_departures, from, to);
    if (departs) {
        entity_departures[to].entity = p_to;
        session_cancel_deferred(departure_key(from));
        session_defer(
            callable_mp(this, &NetwMultiplayer::entity_settle_departure)
                .bind(int64_t(to)),
            departure_key(to)
        );
    }
    return true;
}

#if defined(NETW_TESTS)
bool NetwMultiplayer::slot_holds_unmoved(const RID &p_entity) const {
    const int64_t key = int64_t(p_entity.get_id());
    return sim_rows.row_of(p_entity) != nullptr
        || interest_engine.has_entity(key)
        || interest_engine.has_memberships(key);
}
#endif

NetwEntityRecord *NetwMultiplayer::entity_record(const RID &p_entity) const {
    return world.entities.record(p_entity.get_id());
}

NetwEntityRecord *NetwMultiplayer::record_of(Object *p_wrapper) const {
    const NetwEntity *entity = Object::cast_to<NetwEntity>(p_wrapper);
    return entity != nullptr ? entity->record_in(this) : nullptr;
}

NetwEntityRecord *NetwMultiplayer::slot_vacate(const RID &p_entity) {
    EntityRow *slot = world.entities.find(p_entity.get_id());
    session::EntityView *view = views.entities.find(p_entity.get_id());
    NetwEntityRecord *const record = slot->record;
    slot->record = nullptr;
    view->holder = nullptr;
    slot->routed = false;
    slot_settle(p_entity.get_id());
    return record;
}

void NetwMultiplayer::wrapper_detach_all() {
    LocalVector<NetwEntity *> holders;
    holders.reserve(views.entities.size());
    views.entities.each(
        [&holders](uint64_t, const session::EntityView &p_view) {
            if (p_view.holder != nullptr) {
                holders.push_back(p_view.holder);
            }
        }
    );
    for (NetwEntity *holder : holders) {
        holder->detach(this);
    }
}

NetwEntityRecord *NetwMultiplayer::slot_record(int64_t p_id) const {
    return world.entities.routed_record(uint64_t(p_id));
}

void NetwMultiplayer::slot_settle(int64_t p_id) {
    const session::EntityView *view = views.entities.find(uint64_t(p_id));
    const bool unviewed = view == nullptr || view->empty();
    if (unviewed && world.entities.has(uint64_t(p_id))) {
        views.entities.erase(uint64_t(p_id));
    }
    if (world.entities.settle(uint64_t(p_id), unviewed)) {
        announced_scene_of_entity.erase(uint64_t(p_id));
    }
}

bool NetwMultiplayer::slot_pin(const RID &p_handle) {
    NETW_BUG_COND_V(!world.entities.has(p_handle.get_id()), false);
    entity::retain(p_handle);
    StringName label;
    session::EntityView *view = views.entities.find(p_handle.get_id());
    if (view != nullptr && view->holder != nullptr) {
        const Ref<NetwScene> minted
            = view->holder->minted_part(NetwEntityRecord::PART_SCENE);
        if (minted.is_valid()) {
            view->pinned_scene = minted;
            label = scene_stem(p_handle);
        }
    }
    world.entities.pin(p_handle.get_id(), label);
    return true;
}

void NetwMultiplayer::slot_unpin(const RID &p_handle) {
    const int64_t id = int64_t(p_handle.get_id());
    const entity::Rows::Unpin step = world.entities.unpin(uint64_t(id));
    if (!step.unpinned) {
        return;
    }
    if (step.last) {
        session::EntityView *view = views.entities.find(uint64_t(id));
        if (view != nullptr) {
            view->pinned_scene = Ref<NetwScene>();
        }
        if (step.swept && view != nullptr) {
            view->retired = Ref<NetwEntity>();
        }
        slot_settle(id);
    }
    entity::release(p_handle);
}

Ref<NetwEntity> NetwMultiplayer::slot_pinned_entity(const RID &p_handle) const {
    const session::EntityView *view = views.entities.find(p_handle.get_id());
    if (view == nullptr) {
        return Ref<NetwEntity>();
    }
    if (view->live.is_valid()) {
        return view->live;
    }
    if (view->retired.is_valid()) {
        return view->retired;
    }
    if (view->holder != nullptr && view->holder->get_reference_count() > 0) {
        return Ref<NetwEntity>(view->holder);
    }
    return Ref<NetwEntity>();
}

Ref<NetwScene> session::Views::pinned_scene_of(uint64_t p_id) const {
    const EntityView *view = entities.find(p_id);
    return view != nullptr ? view->pinned_scene : Ref<NetwScene>();
}

NetwEntity *session::Views::holder_of(uint64_t p_id) const {
    const EntityView *view = entities.find(p_id);
    if (view == nullptr) {
        return nullptr;
    }
    return view->holder != nullptr ? view->holder : view->live.ptr();
}

Ref<NetwEntity> session::Views::departing_of(uint64_t p_id) const {
    const Ref<NetwEntity> *held = departing.getptr(p_id);
    return held != nullptr ? *held : Ref<NetwEntity>();
}

Node *NetwMultiplayer::wrapper_owner(const RID &p_entity) const {
    const EntityRow *slot = world.entities.find(p_entity.get_id());
    return slot != nullptr && slot->node != 0
        ? Object::cast_to<Node>(gd::instance_from_id(ObjectID(slot->node)))
        : nullptr;
}

namespace {

Callable &wrapper_mint() {
    static Callable factory;
    return factory;
}

} // namespace

StringName NetwMultiplayer::wrapper_meta() {
    return NetwEntityRecord::entity_meta();
}

void NetwMultiplayer::set_wrapper_factory(const Callable &p_factory) {
    wrapper_mint() = p_factory;
}

bool NetwMultiplayer::has_wrapper_factory() {
    return wrapper_mint().is_valid();
}

void NetwMultiplayer::clear_wrapper_factory() {
    wrapper_mint() = Callable();
}

Callable NetwMultiplayer::wrapper_factory() {
    return wrapper_mint();
}

Ref<NetwEntity> NetwMultiplayer::wrapper_at(Object *p_node) {
    Node *walker = Object::cast_to<Node>(p_node);
    while (walker != nullptr) {
        Object *found = tree::entity_object_at(walker);
        if (found != nullptr) {
            return Ref<NetwEntity>(Object::cast_to<NetwEntity>(found));
        }
        walker = walker->get_parent();
    }
    return Ref<NetwEntity>();
}

Ref<NetwEntity> NetwMultiplayer::wrapper_ensure(Node *p_root) {
    Node *root = p_root;
    if (root == nullptr) {
        return Ref<NetwEntity>();
    }
    if (Object *found = tree::entity_object_at(root)) {
        return Ref<NetwEntity>(Object::cast_to<NetwEntity>(found));
    }
    const Callable &factory = wrapper_mint();
    if (!factory.is_valid()) {
        Ref<NetwEntity> entity;
        entity.instantiate();
        entity->adopt_root(root);
        return entity;
    }
    const Variant made = factory.call(root);
    Ref<NetwEntity> minted = made;
    NETW_BUG_COND_V(minted.is_null(), minted);
    return minted;
}

Ref<NetwEntity> NetwMultiplayer::wrapper_resolve(Node *p_node) {
    Node *node = p_node;
    if (node == nullptr) {
        return Ref<NetwEntity>();
    }
    Ref<NetwEntity> existing = wrapper_at(node);
    if (existing.is_valid()) {
        return existing;
    }
    if (node->is_inside_tree()) {
        return Ref<NetwEntity>();
    }
    Node *root = node;
    while (root->get_parent() != nullptr) {
        if (root->get_parent()->is_inside_tree()) {
            return Ref<NetwEntity>();
        }
        root = root->get_parent();
    }
    return wrapper_ensure(root);
}

Object *NetwMultiplayer::wrapper_stamp_identity(
    Node *p_node,
    const StringName &p_entity_id,
    int64_t p_peer_id
) {
    Node *node = p_node;
    if (node == nullptr) {
        return nullptr;
    }
    if (String(p_entity_id).is_empty()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{NetwEntity.id}", String(), "entity id"},
            gd::instance_id(node)
        );
        return p_node;
    }
    node->set_name(String(p_entity_id));
    const Ref<NetwEntity> wrapper = wrapper_ensure(node);
    if (wrapper.is_valid()) {
        wrapper->set_name(p_entity_id);
        wrapper->set_peer_id(p_peer_id);
    }
    return node;
}

RID NetwMultiplayer::entity_at_or_above(Node *p_node) const {
    const Ref<NetwEntity> wrapper = wrapper_at(p_node);
    if (wrapper.is_null()) {
        return RID();
    }
    const RID entity = wrapper->get_rid_handle();
    const EntityRow *slot = world.entities.find(entity.get_id());
    const session::EntityView *view = views.entities.find(entity.get_id());
    if (slot == nullptr || slot->routed_record() == nullptr || view == nullptr
        || view->indexed != gd::instance_id(wrapper.ptr())) {
        return RID();
    }
    return entity;
}

RID NetwMultiplayer::entity_parent_of(const RID &p_entity) const {
    Node *owner = wrapper_owner(p_entity);
    if (owner == nullptr) {
        return RID();
    }
    return entity_at_or_above(owner->get_parent());
}

Ref<NetwScene> NetwMultiplayer::entity_scene_facet(
    NetwEntityRecord *p_record,
    Node *p_owner
) {
    if (p_record == nullptr) {
        return Ref<NetwScene>();
    }
    Node *owner = p_owner;
    const RID own
        = owner != nullptr ? entity_of(owner) : p_record->get_handle();
    const RID resolved = scene_of(own);
    if (!resolved.is_valid()) {
        return Ref<NetwScene>();
    }
    return scene_handle_of(resolved);
}

void NetwMultiplayer::entity_move(Node *p_owner, Node *p_new_parent) {
    Node *owner = p_owner;
    Node *parent = p_new_parent;
    if (owner == nullptr || parent == nullptr) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{Netw.reparent}", String(), "node or new parent"}
        );
        return;
    }
    owner = scene_outer_of(owner);
    NETW_TRACE(sys::ENTITY, "Moving an entity under %s.", parent->get_name());
    if (owner->is_inside_tree()) {
        owner->reparent(parent);
    } else {
        if (owner->get_parent() != nullptr) {
            owner->get_parent()->remove_child(owner);
        }
        parent->add_child(owner);
    }
}

void NetwMultiplayer::entity_free_owner(uint64_t p_owner_id) {
    Node *owner
        = Object::cast_to<Node>(gd::instance_from_id(ObjectID(p_owner_id)));
    if (owner != nullptr) {
        owner->queue_free();
    }
}

namespace {

void rename_reserved_to_identity(Node *p_owner, const StringName &p_entity_id) {
    const String seated = String(p_owner->get_name());
    if (seated == seated.validate_node_name()) {
        return;
    }
    const String identity = String(p_entity_id).validate_node_name();
    if (identity.is_empty()) {
        return;
    }
    p_owner->set_name(identity);
    NETW_TRACE(
        sys::ENTITY,
        "'%s' is a name the engine reserves, so '%s' is taken from the "
        "entity id instead",
        seated,
        p_owner->get_name()
    );
}

} // namespace

void NetwMultiplayer::entity_enter_tree(
    Object *p_wrapper,
    Node *p_owner,
    NetwEntityRecord *p_record,
    NetwMultiplayer *p_session,
    bool p_is_authority
) {
    Node *owner = p_owner;
    if (owner == nullptr || p_wrapper == nullptr || p_record == nullptr) {
        return;
    }
    const Ref<RefCounted> keep(Object::cast_to<RefCounted>(p_wrapper));
    const Ref<NetwMultiplayer> session(p_session);
    NetwEntity *wrapper = Object::cast_to<NetwEntity>(p_wrapper);
    const ObjectID owner_id = gd::instance_id(owner);
    const ObjectID parent_id = gd::instance_id(owner->get_parent());
    RID handle = p_record->get_handle();
    Ref<MultiplayerAPI> stamped = wrapper != nullptr
        ? wrapper->get_multiplayer()
        : Ref<MultiplayerAPI>();
    const auto intact = [&]() {
        if (gd::object_of(owner_id) != owner
            || gd::instance_id(owner->get_parent()) != parent_id
            || owner->is_queued_for_deletion()
            || p_record->get_handle() != handle) {
            return false;
        }
        return wrapper == nullptr
            || (wrapper->get_record() == p_record
                && wrapper->get_owner() == owner
                && wrapper->get_multiplayer() == stamped);
    };
    const auto publish = [&](int64_t p_from) {
        const int64_t to = p_record->get_stage();
        if (session.is_valid() && session.ptr() != stamped.ptr()) {
            session->entity_note_stage(p_record, p_from, to);
        }
        if (wrapper != nullptr && wrapper->get_record() == p_record) {
            wrapper->publish_stage(p_from, to);
        }
        return intact() && p_record->get_stage() == to;
    };
    {
        NETW_ZONE_NC("entity enter identity", colors::LIVENESS);
        rename_reserved_to_identity(owner, p_record->get_entity_id());
    }
    const bool is_reparent
        = p_record->get_stage() == int64_t(entity::Stage::LIVE);
    {
        NETW_ZONE_NC("entity enter hydrate identity", colors::LIVENESS);
        entity_hydrate_identity(*p_record, owner);
    }

    if (!is_reparent
        && p_record->get_stage() == int64_t(entity::Stage::UNBOUND)) {
        NETW_ZONE_NC("entity enter arm", colors::LIVENESS);
        NetwEntity *armed = Object::cast_to<NetwEntity>(p_wrapper);
        if (armed != nullptr) {
            armed->arm(Ref<NetwMultiplayer>());
            if (!intact()
                || p_record->get_stage() != int64_t(entity::Stage::ARMED)) {
                return;
            }
        }
    }

    if (!is_reparent && p_record->get_route() == 0 && p_session != nullptr
        && p_is_authority) {
        p_record->set_route(p_session->liveness_allocate_route(p_wrapper));
    }
    if (p_record->get_entity_id() == StringName()
        && p_record->get_route() > 0) {
        p_record->set_entity_id(
            spawn::Book::default_entity_id(
                nullptr,
                owner,
                p_record->get_route()
            )
        );
    }
    if (p_record->get_route() > 0 && p_session != nullptr) {
        {
            NETW_ZONE_NC("entity enter bind route", colors::LIVENESS);
            p_session->liveness_bind_route(p_record->get_route(), p_wrapper);
        }
        if (p_is_authority) {
            NETW_ZONE_NC("entity enter nested candidate", colors::LIVENESS);
            p_session->spawn_note_nested_candidate(p_record->get_route());
        }
    }

    if (is_reparent) {
        NetwEntity *moved = Object::cast_to<NetwEntity>(p_wrapper);
        if (moved != nullptr) {
            moved->apply_control();
        }
    }

    {
        NETW_ZONE_NC("entity enter connect ready", colors::LIVENESS);
        const NetwEntity *readied = Object::cast_to<NetwEntity>(p_wrapper);
        const Callable ready(p_wrapper, StringName("_on_owner_ready"));
        if ((readied == nullptr || !readied->get_ready_fired())
            && !owner->is_connected(StringName("ready"), ready)) {
            owner
                ->connect(StringName("ready"), ready, Object::CONNECT_ONE_SHOT);
        }
    }

    if (!is_reparent && !entity::stage_is_leaving(p_record->get_stage())) {
        handle = p_record->get_handle();
        if (wrapper != nullptr) {
            stamped = wrapper->get_multiplayer();
        }
        const int64_t from = p_record->get_stage();
        {
            NETW_ZONE_NC("entity enter transition live", colors::LIVENESS);
            if (!p_record->transition(int64_t(entity::Stage::LIVE))) {
                return;
            }
        }
        if (!publish(from)) {
            return;
        }
        NETW_TRACE(sys::ENTITY, "'%s' is live", owner->get_name());
        if (p_session != nullptr) {
            NETW_ZONE_NC("entity enter spawning event", colors::LIVENESS);
            p_session->event_record(
                EventPlane::SPAWNING,
                p_record->get_handle(),
                p_record->get_route(),
                p_record->get_peer_id()
            );
        }
        if (!intact()
            || p_record->get_stage() != int64_t(entity::Stage::LIVE)) {
            return;
        }
        if (p_record->get_declares_scene() && p_session != nullptr) {
            p_session->scene_root_online(owner);
        }
        spawn::Pipeline *pipeline
            = p_session != nullptr ? p_session->spawn_plane() : nullptr;
        if (wrapper != nullptr && pipeline != nullptr
            && pipeline->is_booked(p_record->get_route())) {
            p_session->membership_place_entity(wrapper->get_rid_handle());
        }
    }
}

void NetwMultiplayer::entity_demote_placed() {
    spawn::Pipeline *pipeline = spawn_plane();
    if (is_host() || pipeline == nullptr) {
        return;
    }
    const auto booked_at = [pipeline](Node *p_node) {
        const NetwEntity *held
            = Object::cast_to<NetwEntity>(tree::entity_object_at(p_node));
        return held != nullptr && held->get_route() > 0
            && pipeline->is_booked(held->get_route());
    };
    LocalVector<Ref<NetwEntity>> placed;
    for (const KeyValue<uint64_t, Ref<NetwEntity>> &held :
         views.node_entities) {
        const Ref<NetwEntity> &entity = held.value;
        Node *owner = entity.is_valid() ? entity->get_owner() : nullptr;
        if (owner == nullptr || entity->get_route() <= 0
            || entity->get_stage() != int64_t(entity::Stage::LIVE)) {
            continue;
        }
        bool booked = false;
        for (Node *at = owner; at != nullptr && !booked;
             at = at->get_parent()) {
            booked = booked_at(at);
        }
        if (!booked) {
            placed.push_back(entity);
        }
    }
    for (const Ref<NetwEntity> &entity : placed) {
        Node *owner = entity->get_owner();
        const int64_t route = entity->get_route();
        liveness_core.unbind_route(entity->get_rid_handle());
        if (entity->get_name()
            == spawn::Book::default_entity_id(nullptr, owner, route)) {
            entity->set_name(StringName());
        }
        entity->set_route(0);
    }
}

void NetwMultiplayer::entity_publish_placed() {
    if (!is_host()) {
        return;
    }
    LocalVector<Ref<NetwEntity>> live;
    for (const KeyValue<uint64_t, Ref<NetwEntity>> &held :
         views.node_entities) {
        if (held.value.is_valid()
            && held.value->get_stage() == int64_t(entity::Stage::LIVE)) {
            live.push_back(held.value);
        }
    }
    for (const Ref<NetwEntity> &entity : live) {
        Node *owner = entity->get_owner();
        if (owner == nullptr || !owner->is_inside_tree()) {
            continue;
        }
        int64_t route = entity->get_route();
        if (route == 0) {
            route = liveness_allocate_route(entity.ptr());
            entity->set_route(route);
        }
        if (route <= 0) {
            continue;
        }
        if (entity->get_name() == StringName()) {
            entity->set_name(
                spawn::Book::default_entity_id(nullptr, owner, route)
            );
        }
        if (spawn::Pipeline *pipeline = spawn_plane()) {
            pipeline->note_placed_before_host(route);
        }
        spawn_note_nested_candidate(route);
    }
}

Node *NetwMultiplayer::entity_component_node(
    const RID &p_entity,
    int64_t p_comp
) const {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return nullptr;
    }
    return wrapper->comp_node_of(p_comp);
}

RID NetwMultiplayer::spawn_call(
    const Callable &p_function,
    const Array &p_args,
    int64_t p_player
) {
    NETW_ZONE_NC("session spawn function", colors::LIVENESS);
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {"{Netw.spawn}", String(), "replication"}
        );
        return RID();
    }
    Ref<NetwPlayer> owner;
    if (!spawn_owner(p_player, "{NetwMultiplayer.spawn_call}", owner)) {
        return RID();
    }
    Object *node = spawn_verb(p_function, p_args, owner);
    if (node == nullptr) {
        spawn_tell_player_failed(p_player, p_function.get_method());
    }
    return node != nullptr ? entity_of(node) : RID();
}

bool NetwMultiplayer::spawn_owner(
    int64_t p_player,
    const char *p_verb,
    Ref<NetwPlayer> &r_owner
) {
    const PlayerRow *row = players.getptr(p_player);
    const bool left = row != nullptr
        ? row->departed
        : p_player > 0 && p_player < player_incarnation_next;
    if (p_player != 0 && !player_is_active(p_player) && !left) {
        fault::raise(
            this,
            NETW_FAULT_BAD_VALUE,
            {p_verb,
             vformat("player %d", p_player),
             "the id of a player in this session, such as one from "
             "{NetwMultiplayer.player_list}"}
        );
        return false;
    }
    r_owner = p_player != 0 ? player_view(p_player) : Ref<NetwPlayer>();
    if (r_owner.is_null() && left) {
        r_owner.instantiate();
        r_owner->incarnation = p_player;
    }
    return true;
}

Ref<NetwPromise> NetwMultiplayer::spawn_call_async(
    const Callable &p_function,
    const Array &p_args
) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {"{Netw.spawn_async}", String(), "replication"}
        );
        return NetwPromise::refused(
            ERR_UNCONFIGURED,
            "Netw.spawn_async needs replication running in the session."
        );
    }
    return spawn_verb_async(p_function, p_args, Ref<NetwPlayer>());
}

void NetwMultiplayer::spawn_register_constructor(
    const StringName &p_id,
    const Callable &p_function,
    const Array &p_arg_types,
    const Array &p_quantizers
) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {vformat("Registering spawn constructor '%s'", String(p_id)),
             String(),
             "replication"}
        );
        return;
    }
    spawn_constructor_register(p_id, p_function, p_arg_types, p_quantizers);
}

RID NetwMultiplayer::spawn_registered(
    const StringName &p_id,
    const Array &p_args,
    int64_t p_player
) {
    NETW_ZONE_NC("session spawn registered", colors::LIVENESS);
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {vformat("Spawn constructor '%s'", String(p_id)),
             String(),
             "replication"}
        );
        return RID();
    }
    Ref<NetwPlayer> owner;
    if (!spawn_owner(p_player, "{NetwMultiplayer.spawn_registered}", owner)) {
        return RID();
    }
    Object *node = spawn_verb_registered(p_id, p_args, owner);
    return node != nullptr ? entity_of(node) : RID();
}

RID NetwMultiplayer::entity_attach(Node *p_node) {
    const Ref<NetwEntity> wrapper = wrapper_ensure(p_node);
    if (wrapper.is_null()) {
        return RID();
    }
    if (wrapper->get_multiplayer().is_null()) {
        wrapper->arm(this);
    }
    if (!wrapper->attach(this).is_valid()) {
        return RID();
    }
    return wrapper->adopt_in(this);
}

TypedArray<Dictionary> NetwMultiplayer::spawn_get_state(const RID &p_entity) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {"Reading spawn state", String(), "replication"}
        );
        return TypedArray<Dictionary>();
    }
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    Node *owner = wrapper.is_valid() ? wrapper->get_owner() : nullptr;
    if (owner == nullptr) {
        return TypedArray<Dictionary>();
    }
    return pipeline->collect_spawn_state(owner);
}

PropertyBindingPtr NetwMultiplayer::entity_derived_binding(
    Node *p_owner,
    int64_t p_record,
    int64_t p_route
) {
    ReplicationCore *plane = get_replication_plane();
    if (p_owner == nullptr || plane == nullptr) {
        return PropertyBindingPtr();
    }
    const PropertyBindingPtr declared
        = plane->derived_binding(p_owner, p_record);
    if (declared != nullptr) {
        return declared;
    }
    return entity_group_binding(p_record, p_route);
}

PropertyBindingPtr NetwMultiplayer::entity_record_binding(
    NetwEntityRecord *p_held,
    Node *p_owner,
    int64_t p_record,
    const RID &p_entity
) {
    if (p_owner == nullptr || get_replication_plane() == nullptr) {
        return nullptr;
    }
    const int64_t route = liveness_core.route_of(p_entity);
    PropertyBindingPtr seated = p_held != nullptr
        ? binding_book.live(p_held->get_record_binding(p_record))
        : nullptr;
    if (seated == nullptr) {
        seated = entity_group_binding(p_record, route);
    }
    static const bool verify
        = std::getenv("NETW_VERIFY_BINDING_INDEX") != nullptr;
    if (verify) {
        const PropertyBindingPtr scanned
            = entity_derived_binding(p_owner, p_record, route);
        if (scanned != seated) {
            NETW_BUG_V(scanned);
        }
    }
    return seated;
}

PropertyBindingPtr NetwMultiplayer::entity_group_binding(
    int64_t p_record,
    int64_t p_route
) {
    if (p_route <= 0) {
        return PropertyBindingPtr();
    }
    NETW_ZONE_NC("entity derived group fallback", colors::WIRE);
    const LocalVector<PropertyBindingPtr> group = entity_derived_group(p_route);
    for (const PropertyBindingPtr &candidate : group) {
        if (candidate == nullptr || candidate->get_set() == nullptr) {
            continue;
        }
        if (candidate->get_set()->get_record() == p_record) {
            return candidate;
        }
    }
    return PropertyBindingPtr();
}

LocalVector<PropertyBindingPtr> NetwMultiplayer::entity_derived_group(
    int64_t p_route
) {
    ReplicationCore *plane = get_replication_plane();
    if (plane == nullptr || p_route <= 0) {
        return LocalVector<PropertyBindingPtr>();
    }
    return plane->derived_group(p_route);
}

bool NetwMultiplayer::entity_governs_property(
    Node *p_owner,
    const NodePath &p_path,
    Node *p_exclude,
    const RID &p_entity
) {
    Node *owner = p_owner;
    if (owner == nullptr || p_path.is_empty()) {
        return false;
    }
    const gd::NodeProperty target = gd::node_property(owner, p_path);
    if (!target.is_property()) {
        return false;
    }
    const TypedArray<MultiplayerSynchronizer> streams
        = synchronizers::of_node(owner);
    for (int at = 0; at < streams.size(); at++) {
        Object *stream = streams[at];
        if (stream == nullptr || stream == p_exclude) {
            continue;
        }
        const Array governed = synchronizers::governed_targets(stream, owner);
        for (int row = 0; row < governed.size(); row++) {
            const Array pair = governed[row];
            Object *held = pair[0];
            if (held == target.object && NodePath(pair[1]) == target.sub) {
                return true;
            }
        }
    }
    const LocalVector<PropertyBindingPtr> group
        = entity_derived_group(liveness_core.route_of(p_entity));
    for (const PropertyBindingPtr &binding : group) {
        if (binding == nullptr || binding->node() != target.object) {
            continue;
        }
        const PropertySet *declaration = binding->get_set();
        if (declaration == nullptr) {
            continue;
        }
        for (const PropertyColumn &column : declaration->columns) {
            if (NodePath(String(":") + String(column.get_key()))
                == target.sub) {
                return true;
            }
        }
    }
    return false;
}

void NetwMultiplayer::entity_note_stage(
    NetwEntityRecord *p_record,
    int64_t p_from,
    int64_t p_to
) {
    if (p_record == nullptr || p_to == p_from) {
        return;
    }
    if (entity::stage_is_leaving(p_to)) {
        kept_entities.erase(p_record->get_handle().get_id());
        kept_by_server.erase(p_record->get_handle().get_id());
    }
    event_record(
        EventPlane::STAGE_TRANSITION,
        p_record->get_handle(),
        p_record->get_route(),
        p_record->get_peer_id(),
        OK,
        p_from,
        p_to
    );
}

void NetwMultiplayer::entity_linger(
    NetwEntityRecord *p_record,
    Node *p_owner,
    int64_t p_pumps
) {
    Node *owner = p_owner;
    if (p_record == nullptr || owner == nullptr) {
        return;
    }
    entity_deactivate(owner);
    NETW_TRACE(sys::ENTITY, "Staying for %d more updates.", int(p_pumps));
    session_defer_after(
        callable_mp(this, &NetwMultiplayer::entity_free_owner)
            .bind(uint64_t(gd::instance_id(owner))),
        StringName(),
        int(p_pumps)
    );
}

Ref<NetwEntity> NetwMultiplayer::entity_get_view(const RID &p_entity) const {
    const session::EntityView *view = views.entities.find(p_entity.get_id());
    return view != nullptr ? view->live : Ref<NetwEntity>();
}

Ref<NetwEntity> NetwMultiplayer::wrapper_for_route(int64_t p_route) const {
    return entity_get_view(liveness_core.rid_from_route(int(p_route)));
}

Ref<NetwEntity> NetwMultiplayer::wrapper_for_id(int64_t p_id) const {
    const EntityRow *slot = world.entities.find(uint64_t(p_id));
    const session::EntityView *view = views.entities.find(uint64_t(p_id));
    if (slot == nullptr || slot->swept || view == nullptr) {
        return Ref<NetwEntity>();
    }
    return view->live.is_valid() ? view->live : view->retired;
}

void NetwMultiplayer::entity_settle_peer_departure(int64_t p_peer) {
    LocalVector<Ref<NetwEntity>> settling;
    settling.reserve(views.entities.size());
    views.entities.each(
        [&settling](uint64_t, const session::EntityView &p_view) {
            if (p_view.live.is_valid()) {
                settling.push_back(p_view.live);
            }
        }
    );
    const bool admitted = halt_admits_despawn;
    halt_admits_despawn = true;
    for (uint32_t at = 0; at < settling.size(); at++) {
        if (settling[at].is_valid()) {
            control_peer_departed(settling[at]->get_rid_handle(), p_peer);
        }
    }
    halt_admits_despawn = admitted;
}

TypedArray<NetwEntity> NetwMultiplayer::wrapper_live() const {
    TypedArray<NetwEntity> out;
    const PackedInt32Array routes = liveness_core.live_routes();
    for (int at = 0; at < routes.size(); at++) {
        const Ref<NetwEntity> wrapper = wrapper_for_route(routes[at]);
        if (wrapper.is_valid()) {
            out.push_back(wrapper);
        }
    }
    return out;
}

void NetwMultiplayer::wrapper_sweep_retired() {
    LocalVector<int64_t> retired;
    views.entities.each(
        [&retired](uint64_t p_id, const session::EntityView &p_view) {
            if (p_view.retired.is_valid()) {
                retired.push_back(int64_t(p_id));
            }
        }
    );
    for (const int64_t id : retired) {
        EntityRow *slot = world.entities.find(uint64_t(id));
        const Ref<NetwEntity> wrapper = views.entities.find(id)->retired;
        if (slot->routed_record() == nullptr) {
            wrapper_unindex_owner(id, gd::instance_id(wrapper.ptr()));
            slot = world.entities.find(uint64_t(id));
        }
        if (slot != nullptr && slot->pins > 0) {
            slot->swept = true;
        } else if (slot != nullptr) {
            views.entities.find(id)->retired = Ref<NetwEntity>();
            slot_settle(id);
        }
    }
}

void NetwMultiplayer::wrapper_unindex_owner(
    int64_t p_id,
    ObjectID p_wrapper
) {
    EntityRow *slot = world.entities.find(uint64_t(p_id));
    if (slot == nullptr) {
        return;
    }
    if (slot->node != 0) {
        const ObjectID owner = ObjectID(slot->node);
        slot->node = 0;
        script::model::clear_node_overlay(
            Object::cast_to<Node>(gd::instance_from_id(owner))
        );
        slot = world.entities.find(uint64_t(p_id));
    }
    session::EntityView *view = slot != nullptr
        ? views.entities.find(uint64_t(p_id))
        : nullptr;
    if (view != nullptr && p_wrapper.is_valid()
        && view->indexed == p_wrapper) {
        view->indexed = ObjectID();
    }
    slot_settle(p_id);
}

void NetwMultiplayer::wrapper_clear() {
    sim_rows.each(sim_scan);
    for (sim::Row *row : sim_scan) {
        row->viewed = false;
    }
    wrapper_detach_all();
    LocalVector<uint64_t> unpinned;
    session::Views &held = views;
    world.entities.each([&unpinned, &held](uint64_t p_id, EntityRow &p_slot) {
        if (p_slot.pins == 0) {
            unpinned.push_back(p_id);
            return;
        }
        session::EntityView *view = held.entities.find(p_id);
        if (view != nullptr && view->live.is_valid()) {
            view->retired = view->live;
            view->live = Ref<NetwEntity>();
        }
        p_slot.node = 0;
        if (view != nullptr) {
            view->indexed = ObjectID();
        }
        p_slot.swept = true;
    });
    if (unpinned.size() == world.entities.size()) {
        world.entities.clear();
        views.entities.clear();
    } else {
        for (const uint64_t id : unpinned) {
            world.entities.erase(id);
            views.entities.erase(id);
        }
    }
    announced_scene_of_entity.clear();
}

#if defined(NETW_TESTS)
NetwMultiplayer::BookSizes NetwMultiplayer::book_sizes() const {
    BookSizes sizes;
    world.entities.each([&sizes](uint64_t, const EntityRow &p_slot) {
        sizes.wrapper_owners += p_slot.node != 0 ? 1 : 0;
    });
    views.entities.each([&sizes](uint64_t, const session::EntityView &p_view) {
        sizes.handle_by_wrapper += p_view.indexed.is_valid() ? 1 : 0;
    });
    sizes.attributed_routes = world.wire.attribution.attributed_route_count();
    sizes.node_overlays = script::model::node_overlay_count();
    sizes.wrapper_slots = int64_t(world.entities.size());
    sizes.liveness_routes = liveness_core.route_count();
    sizes.display_rows = display_book.size();
    sizes.layer_views = int64_t(layer_rows.get_rid_count());
    sizes.engine_layers = interest_engine.layer_count();
    sizes.attribution_peer_keys = world.wire.attribution.peer_keyed_count();
    sizes.lagcomp_timelines = lagcomp_core.timeline_registered();
    return sizes;
}
#endif

Error NetwMultiplayer::liveness_adopt_route(
    int64_t p_route,
    const Ref<RefCounted> &p_wrapper,
    Node *p_owner
) {
    NetwEntity *const wrapper = Object::cast_to<NetwEntity>(p_wrapper.ptr());
    if (p_route <= 0 || wrapper == nullptr) {
        return ERR_INVALID_PARAMETER;
    }
    const RID held = liveness_core.rid_from_route(int(p_route));
    const RID own = wrapper->attach(this);
    if (!own.is_valid()) {
        return ERR_UNAVAILABLE;
    }
    if (held.is_valid()) {
        const NetwLivenessCore::State held_state
            = liveness_core.state_of(held);
        const bool absent = held_state == NetwLivenessCore::STATE_ABSENT;
        const Ref<NetwEntity> bound = entity_get_view(held);
        if (bound == p_wrapper && !absent
            && held_state != NetwLivenessCore::STATE_DEAD) {
            return ERR_ALREADY_EXISTS;
        }
        if (held != own && !absent
            && (bound.is_valid() || liveness_core.entity_is_valid(own))) {
            NETW_TRACE(
                sys::LIVENESS,
                "Route %d already belongs to another entity.",
                int(p_route)
            );
            return ERR_UNAVAILABLE;
        }
    }
    const RID entity = held.is_valid() ? held : own;
    if (!wrapper->bind_route(this, entity, p_route, p_owner)) {
        return ERR_UNAVAILABLE;
    }
    NetwEntityRecord *const record = wrapper->record_in(this);
    NETW_BUG_COND_V(record == nullptr, ERR_BUG);
    record->set_route(p_route);
    return OK;
}

void NetwMultiplayer::liveness_publish_live(int64_t p_route) {
    NETW_ZONE_NC("liveness publish live", colors::LIVENESS);
    const RID entity = liveness_core.rid_from_route(int(p_route));
    NetwEntityRecord *record = nullptr;
    RID viewed;
    {
        NETW_ZONE_NC("liveness resolve wrapper", colors::LIVENESS);
        if (entity_get_view(entity).is_valid()) {
            record = entity_record(entity);
            viewed = entity;
        }
    }
    if (record != nullptr) {
        NETW_ZONE_NC("liveness emit spawned event", colors::LIVENESS);
        event_record(
            EventPlane::SPAWNED,
            record->get_handle(),
            p_route,
            record->get_peer_id()
        );
    }
    {
        NETW_ZONE_NC("liveness emit entity live", colors::LIVENESS);
        if (SyncPipeline *pipeline = sync_pipeline()) {
            if (pipeline->hears_entity_live) {
                sync_pipeline_on_entity_live(p_route, viewed);
            }
        }
        if (SyncCompat *adapter = sync_adapter()) {
            if (adapter->hears_entity_live) {
                sync_compat_entity_live(p_route, viewed);
            }
        }
        scene_on_entity_live(entity);
        if (display_session_bound) {
            display_on_entity_live(p_route, entity);
        }
        if (spawn::Pipeline *spawning = spawn_plane()) {
            if (spawning->retries_scene_parks()) {
                spawn_retry_scene_parked_spawns();
            }
            if (spawning->applies_action_gate()) {
                spawn_apply_action_gate(p_route, entity);
            }
        }
    }
}

bool NetwMultiplayer::liveness_linger(const RID &p_entity) {
    if (!liveness_core.set_state(p_entity, NetwLivenessCore::STATE_LINGERING)) {
        return false;
    }
    return true;
}

void NetwMultiplayer::liveness_unindex_wrapper(const RID &p_entity) {
    const int64_t id = p_entity.get_id();
    session::EntityView *view = views.entities.find(uint64_t(id));
    if (view != nullptr && view->live.is_valid()) {
        view->retired = view->live;
        view->live = Ref<NetwEntity>();
        sim_follow_view(p_entity, false);
    }
}

void NetwMultiplayer::liveness_forget_wrapper(const RID &p_entity) {
    const int64_t id = p_entity.get_id();
    EntityRow *slot = world.entities.find(uint64_t(id));
    if (slot == nullptr) {
        return;
    }
    slot->routed = false;
    const session::EntityView *view = views.entities.find(uint64_t(id));
    if (view != nullptr && view->holder != nullptr) {
        view->holder->detach(this);
    }
    slot = world.entities.find(uint64_t(id));
    view = views.entities.find(uint64_t(id));
    if (slot == nullptr || (view != nullptr && view->live.is_valid())) {
        return;
    }
    const Ref<NetwEntity> retired
        = view != nullptr ? view->retired : Ref<NetwEntity>();
    wrapper_unindex_owner(id, gd::instance_id(retired.ptr()));
}

void NetwMultiplayer::liveness_release(int64_t p_route) {
    const RID entity = liveness_core.rid_from_route(int(p_route));
    liveness_core.set_state(entity, NetwLivenessCore::STATE_DEAD);
    liveness_core.fail_live(int(p_route));
    liveness_unindex_wrapper(entity);
}

void NetwMultiplayer::liveness_announce_dead(int64_t p_route) {
    const RID entity = liveness_core.rid_from_route(int(p_route));
    replication_clear_route(p_route);
    display_retire_route(p_route);
    world.wire.attribution.retire_route(p_route);
    event_record(EventPlane::DESPAWNED, entity, p_route, 0);
    liveness_forget_wrapper(entity);
}

bool NetwMultiplayer::liveness_retire(int64_t p_route) {
    liveness_release(p_route);
    liveness_announce_dead(p_route);
    return true;
}

bool NetwMultiplayer::liveness_hide(int64_t p_route) {
    const RID entity = liveness_core.rid_from_route(int(p_route));
    if (!liveness_core.hide_route(int(p_route))) {
        return false;
    }
    const Ref<NetwEntity> wrapper = entity_get_view(entity);
    if (wrapper.is_valid()) {
        wrapper->watch_despawning(false);
    }
    entity_release_body(
        entity,
        wrapper.is_valid() ? wrapper->record_in(this) : nullptr,
        p_route,
        true
    );
    liveness_unindex_wrapper(entity);
    event_record(EventPlane::HIDDEN, entity, p_route, 0);
    liveness_forget_wrapper(entity);
    return true;
}

Node *NetwMultiplayer::owner_of_wrapper(Object *p_wrapper) {
    NetwEntity *entity = Object::cast_to<NetwEntity>(p_wrapper);
    if (entity == nullptr) {
        return nullptr;
    }
    return entity->get_owner();
}

int64_t NetwMultiplayer::liveness_reserve_route() {
    return liveness_core.reserve_route();
}

int64_t NetwMultiplayer::liveness_lease_remaining() const {
    return int64_t(liveness_core.lease_remaining());
}

void NetwMultiplayer::liveness_grant_lease(int64_t p_peer) {
    if (structure_halted) {
        if (!halt_leases.has(p_peer)) {
            halt_leases.push_back(p_peer);
        }
        return;
    }
    const int base = liveness_core.grant_lease(int(p_peer), ROUTE_LEASE_BLOCK);
    if (base == 0) {
        return;
    }
    session::RouteLease granted;
    granted.base = uint64_t(base);
    granted.count = uint64_t(ROUTE_LEASE_BLOCK);
    send_to(
        p_peer,
        0,
        wire::CHANNEL_ROUTE_LEASE,
        session::frame_write(granted),
        true,
        0,
        String()
    );
}

Error NetwMultiplayer::liveness_spend_lease(int64_t p_peer, int64_t p_route) {
    if (!is_host()) {
        return ERR_UNAUTHORIZED;
    }
    if (!liveness_core.spend_lease(int(p_peer), int(p_route))) {
        return ERR_UNAUTHORIZED;
    }
    if (liveness_core.granted_remaining(int(p_peer))
        <= ROUTE_LEASE_REFILL_AT) {
        liveness_grant_lease(p_peer);
    }
    return OK;
}

void NetwMultiplayer::liveness_receive_lease(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (p_sender != SERVER_PEER) {
        return;
    }
    session::RouteLease granted;
    if (!session::frame_read(p_payload, granted) || granted.base == 0
        || granted.count == 0) {
        return;
    }
    liveness_core.install_lease(int(granted.base), int(granted.count));
}

int64_t NetwMultiplayer::liveness_allocate_route(Object *p_wrapper) {
    const int64_t standing = liveness_route_of(p_wrapper);
    if (standing > 0) {
        return standing;
    }
    const int64_t route = liveness_core.reserve_route();
    return liveness_bind_route(route, p_wrapper) ? route : 0;
}

bool NetwMultiplayer::liveness_bind_route(int64_t p_route, Object *p_wrapper) {
    if (Object::cast_to<NetwEntity>(p_wrapper) == nullptr) {
        return false;
    }
    const Ref<RefCounted> wrapper(p_wrapper);
    Node *owner = owner_of_wrapper(p_wrapper);
    const Error verdict = liveness_adopt_route(p_route, wrapper, owner);
    if (verdict == ERR_ALREADY_EXISTS) {
        return true;
    }
    if (verdict != OK) {
        return false;
    }

    NetwEntity *entity = Object::cast_to<NetwEntity>(p_wrapper);
    if (owner != nullptr && entity != nullptr) {
        entity->watch_despawning(true);
    }

    liveness_publish_live(p_route);
    liveness_core.flush_live(int(p_route));
    return true;
}

void NetwMultiplayer::liveness_bind_routes_data(
    const PackedInt64Array &p_routes
) {
    liveness_core.bind_routes_data(p_routes);
}

void NetwMultiplayer::liveness_tombstone_routes_data(
    const PackedInt64Array &p_routes
) {
    liveness_core.tombstone_routes_data(p_routes);
}

int64_t NetwMultiplayer::liveness_route_of(Object *p_wrapper) const {
    NetwEntityRecord *const record = record_of(p_wrapper);
    if (record == nullptr) {
        return 0;
    }
    return liveness_core.route_of(record->get_handle());
}

NetwMultiplayer::EntityState NetwMultiplayer::liveness_route_state(
    int64_t p_route
) const {
    return EntityState(liveness_core.route_state(int(p_route)));
}

int64_t NetwMultiplayer::liveness_route_epoch(int64_t p_route) const {
    return int64_t(liveness_core.route_epoch(int(p_route)));
}

uint64_t NetwMultiplayer::liveness_route_anchor(int64_t p_route) const {
    return liveness_core.route_anchor(int(p_route));
}

NetwMultiplayer::AnchorRevision NetwMultiplayer::anchor_installed(
    int64_t p_route
) const {
    AnchorRevision installed;
    installed.revision = liveness_core.route_anchor(int(p_route));
    installed.author = liveness_core.route_anchor_author(int(p_route));
    return installed;
}

uint64_t NetwMultiplayer::entity_advance_anchor(int64_t p_route) {
    const uint64_t held = liveness_route_anchor(p_route);
    if (held == 0) {
        return 0;
    }
    entity_install_anchor(p_route, held + 1, uint64_t(get_unique_id()));
    return held + 1;
}

void NetwMultiplayer::entity_install_anchor(
    int64_t p_route,
    uint64_t p_anchor,
    uint64_t p_author
) {
    if (!liveness_core.set_route_anchor(int(p_route), p_anchor, p_author)) {
        return;
    }
    row_streams_follow_anchor(p_route, p_anchor, p_author);
}

int64_t NetwMultiplayer::liveness_route_wire_life(int64_t p_route) const {
    return int64_t(liveness_core.route_wire_epoch(int(p_route)));
}

bool NetwMultiplayer::liveness_epoch_admits(
    int64_t p_route,
    int64_t p_epoch
) const {
    return liveness_core.epoch_admits(int(p_route), int(p_epoch));
}

bool NetwMultiplayer::liveness_adopt_epoch(int64_t p_route, int64_t p_epoch) {
    return liveness_core.adopt_epoch(int(p_route), int(p_epoch));
}

void NetwMultiplayer::liveness_raise_epoch_to_wire(int64_t p_route) {
    liveness_core.raise_epoch_to_wire(int(p_route));
}

Error NetwMultiplayer::entity_frame_verdict(int64_t p_route) const {
    const EntityState state = liveness_route_state(p_route);
    if (state == ENTITY_STATE_UNKNOWN) {
        return ERR_DOES_NOT_EXIST;
    }
    if (state == ENTITY_STATE_LINGERING || state == ENTITY_STATE_DEAD) {
        return ERR_SKIP;
    }
    const Ref<NetwEntity> entity
        = Object::cast_to<NetwEntity>(wrapper_for_route(p_route).ptr());
    if (entity.is_null() || entity->get_owner() == nullptr) {
        return ERR_UNAVAILABLE;
    }
    return OK;
}

RID NetwMultiplayer::liveness_adopt(const RID &p_entity) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    return view.is_valid() ? view->adopt_in(this) : RID();
}

RID NetwMultiplayer::entity_of(Object *p_node) {
    const Ref<NetwEntity> wrapper = wrapper_at(p_node);
    return wrapper.is_valid() ? wrapper->adopt_in(this) : RID();
}

TypedArray<NetwEntity> NetwMultiplayer::liveness_get_entities() const {
    return wrapper_live();
}

void NetwMultiplayer::liveness_schedule_when_live(
    int64_t p_route,
    const Callable &p_callback,
    int64_t p_origin,
    int64_t p_timeout,
    bool p_on_clock,
    const Callable &p_on_timeout
) {
    liveness_core.when_live(
        int(p_route),
        p_callback,
        int(p_origin),
        int(p_timeout),
        p_on_clock,
        p_on_timeout
    );
}

int64_t NetwMultiplayer::liveness_pending_live_count() const {
    return liveness_core.pending_live_count();
}

void NetwMultiplayer::liveness_poll(int64_t p_clock_tick) {
    const PackedInt32Array expired = liveness_core.poll(int(p_clock_tick));
    for (int at = 0; at < expired.size(); at++) {
        fault::raise(
            this,
            NETW_FAULT_LIVENESS_TIMEOUT,
            {String::num_int64(expired[at]),
             String(),
             String(),
             0,
             expired[at],
             0}
        );
    }
}

void NetwMultiplayer::liveness_poll_now() {
    NETW_ZONE_NC("Liveness poll", colors::LIVENESS);
    liveness_poll(
        clock_engine().get_configured() ? clock_engine().get_ticks_run() : 0
    );
}

void NetwMultiplayer::liveness_clear_session() {
    display_clear_runtimes();
    LocalVector<RID> predicted;
    predicted.reserve(predict_engines.size());
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        predicted.push_back(row.value->seated_rid());
    }
    for (const RID &entity : predicted) {
        unregister_prediction(entity);
    }
    for (const KeyValue<uint64_t, WithheldDespawn> &row : lifecycle_withheld) {
        const NetwEntity *held = views.holder_of(row.key);
        Node *owner = held != nullptr ? held->get_owner() : nullptr;
        if (owner != nullptr && !owner->is_queued_for_deletion()) {
            owner->queue_free();
        }
    }
    lifecycle_withheld.clear();
    entity_departures.clear();
    views.departing.clear();
    entity_generation += 1;
    liveness_core.clear();
    wrapper_clear();
}

void NetwMultiplayer::liveness_settle_clear_session() {
    session_defer(
        callable_mp(this, &NetwMultiplayer::liveness_clear_session),
        StringName("liveness-clear-session")
    );
}

void NetwMultiplayer::entity_capture_exit(Object *p_wrapper) {
    NetwEntity *entity = Object::cast_to<NetwEntity>(p_wrapper);
    if (entity == nullptr) {
        return;
    }
    persist_capture_exit(entity->get_owner());
    const bool scene_pending = entity->get_declares_scene()
        && entity::stage_can_begin_despawn(entity->get_stage());
    if (entity->get_declares_scene() && !scene_pending) {
        scene_root_offline(entity->get_owner());
    }
    const uint64_t departing = entity->get_rid_handle().get_id();
    EntityDeparture *standing = entity_departures.getptr(departing);
    EntityDeparture fresh;
    EntityDeparture &row = standing != nullptr ? *standing : fresh;
    if (scene_pending && !row.scene_pending) {
        row.scene_pending = true;
        row.scene_outer = gd::instance_id(scene_outer_of(entity->get_owner()));
    }
    if (standing == nullptr) {
        row.applied = applying_remote_frame;
        Node *owner = entity->get_owner();
        row.parent_id
            = gd::instance_id(owner != nullptr ? owner->get_parent() : nullptr);
    } else {
        row.applied = row.applied && applying_remote_frame;
    }
    row.entity = entity->get_rid_handle();
    views.departing[row.entity.get_id()] = Ref<NetwEntity>(entity);
    row.owner_id = gd::instance_id(entity->get_owner());
    row.generation = entity_generation;
    row.route = liveness_route_of(p_wrapper);
    row.withheld = row.withheld || lifecycle_withheld.has(row.entity.get_id());
    row.terminal
        = row.terminal || !entity::stage_can_begin_despawn(entity->get_stage());
    row.hidden = row.hidden
        || (row.route > 0
            && liveness_route_state(row.route) == ENTITY_STATE_ABSENT);
    const bool admitted_here
        = applying_remote_frame && admitting_route == row.route;
    row.author = admitted_here ? admitting_author : int64_t(0);
    if (row.route > 0) {
        if (ReplicationCore *plane = get_replication_plane()) {
            spawn::Pipeline *spawns = plane->get_spawn_pipeline();
            row.received = spawns->holds_received_route(row.route);
            if (!row.terminal && !admitted_here
                && spawns->holds_spawned_route(row.route)) {
                entity_advance_anchor(row.route);
            } else if (
                !row.terminal && !row.minted && row.received
                && !applying_remote_frame
                && lifecycle_authors_move(entity->get_rid_handle())
            ) {
                const AnchorRevision base = anchor_installed(row.route);
                row.base = base.revision;
                row.base_author = base.author;
                row.minted = entity_advance_anchor(row.route) > 0;
                if (const HashMap<int64_t, Ref<NetwPromise>>::Iterator waiting
                    = lifecycle_promises.find(row.route)) {
                    row.promise = waiting->value;
                    lifecycle_promises.remove(waiting);
                }
            }
        }
    }
    entity_capture_residency(row, entity->get_peer_id());
    if (standing == nullptr) {
        entity_departures.insert(departing, row);
    }
    session_defer(
        callable_mp(this, &NetwMultiplayer::entity_settle_departure)
            .bind(int64_t(departing)),
        departure_key(departing)
    );
}

bool NetwMultiplayer::entity_left_tree(
    const RID &p_entity,
    const Node *p_owner
) const {
    if (p_owner != nullptr && p_owner->is_inside_tree()) {
        return false;
    }
    const EntityState state = entity_get_state(p_entity);
    return entity_departures.has(p_entity.get_id())
        || state == ENTITY_STATE_DEAD || state == ENTITY_STATE_ABSENT;
}

void NetwMultiplayer::entity_capture_residency(
    EntityDeparture &r_row,
    int64_t p_peer
) {
    if (p_peer == 0) {
        return;
    }
    const RID leaving = scene_of(r_row.entity);
    if (!leaving.is_valid() || leaving == r_row.entity) {
        return;
    }
    for (uint32_t at = 0; at < r_row.residencies.size(); at++) {
        if (r_row.residencies[at].scene == leaving
            && r_row.residencies[at].peer == p_peer) {
            return;
        }
    }
    DepartedResidency resided;
    resided.scene = leaving;
    resided.peer = p_peer;
    r_row.residencies.push_back(resided);
}

entity::Outcome NetwMultiplayer::entity_departure_outcome(
    const EntityDeparture &p_row
) const {
    entity::Settlement settlement;
    const Ref<NetwEntity> wrapper = views.departing_of(p_row.entity.get_id());
    settlement.terminal = p_row.terminal
        || (wrapper.is_valid()
            && !entity::stage_can_begin_despawn(wrapper->get_stage()));
    settlement.hidden = p_row.hidden;
    settlement.received = p_row.received;
    Node *owner = Object::cast_to<Node>(gd::object_of(p_row.owner_id));
    settlement.owner_live = owner != nullptr;
    settlement.owner_in_tree = owner != nullptr && owner->is_inside_tree();
    settlement.session_current
        = p_row.generation == entity_generation && wrapper.is_valid();
    return entity::classify(settlement);
}

void NetwMultiplayer::entity_settle_departure(int64_t p_entity) {
    const Ref<NetwMultiplayer> keep(this);
    const uint64_t departing = uint64_t(p_entity);
    EntityDeparture *standing = entity_departures.getptr(departing);
    if (standing == nullptr
        || lifecycle_withheld.has(standing->entity.get_id())) {
        return;
    }
    const EntityDeparture row = *standing;
    entity_departures.erase(departing);
    const Ref<NetwEntity> held = views.departing_of(row.entity.get_id());
#if defined(NETW_TESTS)
    membership_audit_held += 1;
#endif
    const entity::Outcome outcome = entity_departure_outcome(row);
    if (row.scene_pending) {
        scene_root_settle_exit(
            row.entity,
            row.owner_id,
            row.scene_outer,
            outcome == entity::Outcome::MOVE
        );
    }
    const Node *owner = Object::cast_to<Node>(gd::object_of(row.owner_id));
    const bool returned = row.withheld && owner != nullptr
        && gd::instance_id(owner->get_parent()) == row.parent_id;
    switch (outcome) {
        case entity::Outcome::MOVE:
            if (!returned) {
                entity_commit_move(row);
            }
            break;
        case entity::Outcome::DEATH:
            entity_commit_death(row);
            break;
        case entity::Outcome::HIDE:
            entity_commit_hide(row);
            break;
    }
    if (outcome != entity::Outcome::MOVE && row.promise.is_valid()
        && !row.promise->get_is_settled()) {
        row.promise->reject(
            ERR_UNAVAILABLE,
            String("the entity left the tree before its move was sent")
        );
    }
    persist_settle_departure(row.owner_id, outcome == entity::Outcome::DEATH);
#if defined(NETW_TESTS)
    membership_audit_held -= 1;
    membership_audit();
#endif
    if (SyncPipeline *pipeline = sync_pipeline()) {
        pipeline->reconcile_dropped_state_timelines();
    }
    if (!entity_departures.has(row.entity.get_id())
        && views.departing_of(row.entity.get_id()) == held) {
        views.departing.erase(row.entity.get_id());
    }
}

void NetwMultiplayer::entity_release_residencies(
    const EntityDeparture &p_row,
    const RID &p_keep
) {
    const Ref<NetwEntity> wrapper = views.departing_of(p_row.entity.get_id());
    for (uint32_t at = 0; at < p_row.residencies.size(); at++) {
        const DepartedResidency &resided = p_row.residencies[at];
        if (resided.scene == p_keep) {
            continue;
        }
        scene_release_departed(
            resided.scene,
            p_row.entity,
            wrapper.is_valid() && wrapper->get_owner() != nullptr,
            resided.peer
        );
    }
}

void NetwMultiplayer::entity_commit_move(const EntityDeparture &p_row) {
    if (p_row.route > 0 && !is_host()) {
        entity_settle_foreign_move(p_row);
    } else if (p_row.route > 0) {
        if (ReplicationCore *plane = get_replication_plane()) {
            plane->get_spawn_pipeline()->settle_move(p_row.route, p_row.author);
        }
        if (p_row.author != 0) {
            const uint64_t landed = liveness_route_anchor(p_row.route);
            lifecycle_decide(
                p_row.route,
                p_row.author,
                lifecycle::Kind::REPARENT,
                landed > 0 ? landed - 1 : 0,
                OK
            );
        }
    }
    entity_relocate_spatial_state(
        p_row,
        clock_engine().get_configured() ? clock_engine().get_tick() : 0
    );
    entity_refresh_moved_body(p_row.entity, p_row.route);
    const Ref<NetwEntity> wrapper = views.departing_of(p_row.entity.get_id());
    if (wrapper.is_valid()) {
        wrapper->announce_reparented(this);
    }
}

void NetwMultiplayer::entity_relocate_spatial_state(
    const EntityDeparture &p_row,
    int64_t p_tick
) {
    const Ref<NetwEntity> wrapper = views.departing_of(p_row.entity.get_id());
    Node *owner = wrapper.is_valid() ? wrapper->get_owner() : nullptr;
    lagcomp_core.timeline_trim_before(
        lagcomp_core.timeline_slot_of(entity_get_view(p_row.entity)),
        p_tick + 1
    );
    if (owner != nullptr) {
        owner->reset_physics_interpolation();
    }
}

void NetwMultiplayer::entity_refresh_moved_body(
    const RID &p_entity,
    int64_t p_route
) {
    if (NetwPredictSlotEngine *engine = predict_engine_for(p_entity)) {
        engine->on_reparented();
    }
    if (p_route > 0) {
        display_refresh_moved(p_route);
    }
}

void NetwMultiplayer::entity_commit_death(const EntityDeparture &p_row) {
    const Ref<NetwEntity> wrapper = views.departing_of(p_row.entity.get_id());
    const bool routed = p_row.route > 0
        && liveness_core.route_state(int(p_row.route))
            != NetwLivenessCore::STATE_DEAD;
    if (routed) {
        if (ReplicationCore *plane = get_replication_plane()) {
            spawn::Pipeline *spawns = plane->get_spawn_pipeline();
            const spawn::Record *recipe = spawns->spawned_record(p_row.route);
            if (recipe != nullptr && wrapper.is_valid()
                && gd::object_of(p_row.owner_id) != nullptr) {
                NetwEntityRecord *const record = wrapper->record_in(this);
                NETW_BUG_COND(record == nullptr);
                record->keep_readmission(*recipe);
            }
            transfer_note_death(p_row.route);
            spawns->settle_death(p_row.route);
        }
        liveness_drop_hooks(p_row.route);
        liveness_release(p_row.route);
    }
    entity_release_residencies(p_row, RID());
    entity_release_body(
        p_row.entity,
        wrapper.is_valid() ? wrapper->record_in(this) : nullptr,
        p_row.route,
        false
    );
    if (wrapper.is_valid()) {
        NetwEntityRecord *const record = wrapper->record_in(this);
        NETW_BUG_COND(record == nullptr);
        wrapper->complete_departure(*record);
    }
    if (routed) {
        liveness_announce_dead(p_row.route);
    }
}

void NetwMultiplayer::entity_release_body(
    const RID &p_entity,
    NetwEntityRecord *p_record,
    int64_t p_route,
    bool p_owner_live
) {
    const RID held = p_record != nullptr ? p_record->get_handle() : RID();
    membership_drop_entity(
        held,
        p_record != nullptr ? p_record->get_peer_id() : 0
    );
    interest_release_body(held);
    if (p_record != nullptr && predict_is_declared(held)) {
        p_record->set_predicted_at_departure(true);
    }
    unregister_prediction(held);
    lagcomp_timeline_undeclare(p_entity);
    if (p_route > 0) {
        replication_clear_route(p_route);
        if (p_owner_live) {
            display_release_route(p_route);
        } else {
            display_retire_route(p_route);
        }
    }
}

void NetwMultiplayer::entity_commit_hide(const EntityDeparture &p_row) {
    if (p_row.hidden || p_row.route <= 0) {
        return;
    }
    const Ref<NetwEntity> wrapper = views.departing_of(p_row.entity.get_id());
    if (entity_get_view(p_row.entity) != wrapper) {
        return;
    }
    if (ReplicationCore *plane = get_replication_plane()) {
        plane->get_spawn_pipeline()->settle_absence(p_row.route);
    }
    entity_release_body(
        p_row.entity,
        wrapper.is_valid() ? wrapper->record_in(this) : nullptr,
        p_row.route,
        true
    );
    liveness_hide(p_row.route);
}

void NetwMultiplayer::liveness_owner_despawning(Object *p_wrapper) {
    NetwEntity *entity = Object::cast_to<NetwEntity>(p_wrapper);
    if (entity == nullptr) {
        return;
    }
    NetwEntityRecord *const record = record_of(p_wrapper);
    const int64_t route = liveness_route_of(p_wrapper);
    if (record == nullptr || route <= 0) {
        return;
    }
    event_record(
        EventPlane::DESPAWNING,
        record->get_handle(),
        route,
        record->get_peer_id()
    );
    const Ref<NetwDespawnOpts> opts = entity->get_active_despawn_opts();
    if (opts.is_null() || !opts->get_linger()) {
        return;
    }
    liveness_linger(record->get_handle());
}

void NetwMultiplayer::liveness_drop_hooks(int64_t p_route) {
    const Ref<NetwEntity> wrapper = wrapper_for_route(p_route);
    if (wrapper.is_null()) {
        return;
    }
    wrapper->watch_despawning(false);
}

RID NetwMultiplayer::entity_create() {
    return liveness_core.entity_create();
}

Error NetwMultiplayer::entity_bind_node(const RID &p_entity, Node *p_node) {
    if (!liveness_core.entity_is_valid(p_entity)) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_node == nullptr) {
        return ERR_INVALID_DATA;
    }

    const Ref<NetwEntity> held = entity_get_view(p_entity);
    if (held.is_valid()) {
        return held->get_owner() == p_node ? OK : ERR_ALREADY_EXISTS;
    }
    const int64_t route = entity_get_route(p_entity);
    if (route <= 0) {
        return ERR_INVALID_DATA;
    }
    const Ref<NetwEntity> wrapper = NetwEntity::ensure(p_node);
    if (scene_take_pending_facet(p_entity)) {
        wrapper->set_declares_scene(true);
    }
    if (wrapper->get_multiplayer() == nullptr) {
        wrapper->arm(this);
    }
    liveness_bind_route(route, wrapper.ptr());
    return OK;
}

int64_t NetwMultiplayer::entity_admit(const RID &p_entity) {
    if (!is_host()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwMultiplayer.entity_admit}"}
        );
        return 0;
    }
    if (!liveness_core.entity_is_valid(p_entity)) {
        return 0;
    }
    const int64_t standing = entity_get_route(p_entity);
    if (standing > 0) {
        return standing;
    }
    const int64_t route = liveness_reserve_route();
    return entity_bind_route(p_entity, route) == OK ? route : 0;
}

Error NetwMultiplayer::entity_bind_route(const RID &p_entity, int64_t p_route) {
    if (!liveness_core.entity_is_valid(p_entity)) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_route <= 0) {
        return ERR_INVALID_DATA;
    }
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    const bool bound = wrapper.is_valid()
        ? liveness_bind_route(p_route, wrapper.ptr())
        : liveness_core.bind_route(p_entity, int(p_route));
    return bound ? OK : ERR_ALREADY_IN_USE;
}

int64_t NetwMultiplayer::entity_get_route(const RID &p_entity) const {
    const int route = liveness_core.route_of(p_entity);
    if (route > 0) {
        return route;
    }
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? wrapper->get_route() : 0;
}

Node *NetwMultiplayer::entity_get_node(const RID &p_entity) const {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? wrapper->get_owner() : nullptr;
}

RID NetwMultiplayer::entity_get_parent(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return RID();
    }
    const Ref<NetwEntity> parent = wrapper->parent_entity();
    if (parent.is_null() || parent->get_owner() == nullptr) {
        return RID();
    }
    return entity_of(parent->get_owner());
}

RID NetwMultiplayer::entity_get_scene(const RID &p_entity) const {
    return scene_of(p_entity);
}

PackedInt32Array NetwMultiplayer::entity_get_recipients(const RID &p_entity) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() ? rpc_recipients(wrapper->get_rid_handle())
                              : PackedInt32Array();
}

const NetwEntityRecord *NetwMultiplayer::entity_record_any(
    const RID &p_entity
) const {
    const NetwEntityRecord *record = entity_record(p_entity);
    if (record != nullptr) {
        return record;
    }
    const Ref<NetwEntity> live = entity_get_view(p_entity);
    return live.is_valid() ? live->record_in(this) : nullptr;
}

int64_t NetwMultiplayer::entity_get_controller(const RID &p_entity) const {
    const NetwEntityRecord *record = entity_record_any(p_entity);
    return record != nullptr
        ? entity::Control::public_peer(record->get_control()->resolve())
        : 0;
}

int64_t NetwMultiplayer::entity_get_player(const RID &p_entity) const {
    const NetwEntityRecord *record = entity_record_any(p_entity);
    return record != nullptr ? record->get_player_id() : 0;
}

Error NetwMultiplayer::entity_set_controller(
    const RID &p_entity,
    int64_t p_peer
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    return control_give(p_entity, p_peer);
}

Ref<NetwPromise> NetwMultiplayer::entity_request_control(
    const RID &p_entity,
    entity::Control::Hold p_hold
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return NetwPromise::rejected(
            ERR_DOES_NOT_EXIST,
            "the entity is not in this session"
        );
    }
    return control_claim(p_entity, p_hold, 0);
}

Ref<NetwPromise> NetwMultiplayer::entity_release_control(
    const RID &p_entity,
    int64_t p_successor
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return NetwPromise::rejected(
            ERR_DOES_NOT_EXIST,
            "the entity is not in this session"
        );
    }
    return control_release(p_entity, p_successor);
}

NetwMultiplayer::ControlState NetwMultiplayer::entity_get_control_state(
    const RID &p_entity
) const {
    const NetwEntityRecord *record = entity_record_any(p_entity);
    if (record == nullptr) {
        return CONTROL_STATE_SETTLED;
    }
    const entity::Control *control = record->get_control();
    if (control->has_pending()) {
        return CONTROL_STATE_PENDING;
    }
    return control->hold != entity::Control::HOLD_NONE
        ? CONTROL_STATE_HELD
        : CONTROL_STATE_SETTLED;
}

Variant NetwMultiplayer::entity_get_param(
    const RID &p_entity,
    EntityParam p_param
) const {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return Variant();
    }
    const entity::Control *control = wrapper->control();
    switch (p_param) {
        case ENTITY_PARAM_AUTHORITY:
            return control->get_authority();
        case ENTITY_PARAM_TRANSFER:
            return control->get_transfer();
        case ENTITY_PARAM_ON_CONTROLLER_DISCONNECT:
            return control->get_on_disconnect();
        case ENTITY_PARAM_ON_PARENT_DESPAWN:
            return control->get_on_parent_despawn();
        case ENTITY_PARAM_HOLD:
            return control->hold;
        case ENTITY_PARAM_REPLICATION_PRIORITY:
            return wrapper->get_record()->get_replication_priority();
        default:
            return Variant();
    }
}

Error NetwMultiplayer::entity_set_param(
    const RID &p_entity,
    EntityParam p_param,
    const Variant &p_value
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    return control_set_param(p_entity, p_param, p_value);
}

NetwMultiplayer::EntityState NetwMultiplayer::entity_get_state(
    const RID &p_entity
) const {
    return EntityState(liveness_core.state_of(p_entity));
}

Error NetwMultiplayer::spawn_admit_frame(
    int64_t p_sender,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_payload
) {
    NETW_ZONE_NC("spawn admit frame", colors::LIVENESS);
    lifecycle::Kind kind = lifecycle::Kind::SPAWN;
    if (p_channel == wire::CHANNEL_SPAWN && is_host()
        && p_sender != SERVER_PEER) {
        return liveness_core.lease_holds(int(p_sender), int(p_route))
            ? OK
            : ERR_UNAUTHORIZED;
    }
    if (p_channel == wire::CHANNEL_REPARENT) {
        kind = lifecycle::Kind::REPARENT;
    } else if (
        p_channel == wire::CHANNEL_DESPAWN || p_channel == wire::CHANNEL_HIDE
    ) {
        kind = lifecycle::Kind::DESPAWN;
    }
    const lifecycle::Ruling ruling
        = lifecycle::rule(lifecycle_frame_facts(p_sender, kind, p_payload));
    if (!ruling.admitted()) {
        return ruling.code;
    }
    if (p_channel != wire::CHANNEL_SPAWN && p_channel != wire::CHANNEL_DESPAWN
        && p_channel != wire::CHANNEL_HIDE
        && p_channel != wire::CHANNEL_REPARENT) {
        return ERR_INVALID_DATA;
    }
    if (p_payload.is_empty()) {
        return ERR_INVALID_DATA;
    }
    return OK;
}

Error NetwMultiplayer::spawn_declare(const RID &p_entity) {
    if (entity_get_view(p_entity).is_valid()) {
        return OK;
    }
    NETW_TRACE(
        sys::SPAWN,
        "Declaring an entity that has no NetwEntity on this peer."
    );
    return ERR_DOES_NOT_EXIST;
}

bool NetwMultiplayer::spawn_locally_desired(int64_t p_peer_id, Node *p_node) {
    if (p_node == nullptr) {
        return false;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    if (entity.is_valid()) {
        if (entity->steering() == p_peer_id) {
            return true;
        }
        if (interest_has_committed_intent(entity->get_rid_handle())) {
            return interest_wire_admits(p_peer_id, entity->get_rid_handle());
        }
        if (interest_is_filtered(entity_of(p_node))
            && !interest_wire_admits(p_peer_id, entity->get_rid_handle())) {
            return false;
        }
    }
    return synchronizers::visibility_verdict(
        p_node,
        p_peer_id,
        get_multiplayer_peer().is_valid() ? int64_t(get_unique_id()) : 1
    );
}

bool NetwMultiplayer::spawn_visible_to(
    int64_t p_route,
    int64_t p_peer_id,
    Node *p_node
) {
    spawn::Book *book = spawn_held_book();
    if (book == nullptr || !book->parent_admits(p_route, p_peer_id)
        || persist_withholds(p_node)) {
        return false;
    }
    const spawn::Record *record = book->spawned_of(p_route);
    if (record != nullptr && record->has_failed(int(p_peer_id))) {
        return false;
    }
    return spawn_locally_desired(p_peer_id, p_node);
}

Ref<NetwEntity> NetwMultiplayer::spawn_arm_identity(
    spawn::Record *p_record,
    Node *p_node,
    const Callable &p_declare
) {
    if (p_record == nullptr || p_node == nullptr) {
        return Ref<NetwEntity>();
    }
    const Ref<NetwEntity> entity = NetwEntity::ensure(p_node);
    if (entity.is_null()) {
        return entity;
    }
    const Ref<NetwPlayer> represented = entity->unbound_player();
    const int64_t held_route = entity->get_route();
    const StringName held_id = entity->get_name();
    const StringName held_node_name = p_node->get_name();
    const int64_t held_peer = entity->get_peer_id();
    const int64_t held_player_id = entity->get_player_id();
    const int64_t held_controller = entity->steering();

    int64_t route = liveness_route_of(entity.ptr());
    if (route <= 0) {
        route = entity->get_route();
    }
    if (route <= 0) {
        route = liveness_reserve_route();
    }
    entity->set_route(route);
    if (entity->get_name() == StringName()) {
        const StringName claimed = represented.is_valid()
            ? represented->get_username()
            : StringName();
        entity->set_name(
            claimed != StringName()
                ? claimed
                : spawn::Book::default_entity_id(p_record, p_node, route)
        );
        const String seated = String(p_node->get_name());
        if (!p_node->is_inside_tree()
            && (seated.is_empty() || seated != seated.validate_node_name())) {
            const String identity
                = String(entity->get_name()).validate_node_name();
            p_node->set_name(identity);
        }
    }
    if (!is_host()) {
        entity->seed_controller(get_unique_id());
    }
    p_record->set_route(route);
    p_record->bind_node(p_node);
    p_record->set_entity_id(entity->get_name());
    p_record->set_peer_id(entity->get_peer_id());
    p_record->set_controller(entity->steering());

    Dictionary facts;
    facts[StringName("recipe")] = p_record->get_recipe();
    facts[StringName("route")] = route;
    facts[StringName("entity_id")] = p_record->get_entity_id();
    facts[StringName("peer")] = p_record->get_peer_id();
    facts[StringName("controller")] = entity->get_controller();
    if (p_declare.is_valid()
        && int64_t(p_declare.call(entity_of(p_node), facts)) != int64_t(OK)) {
        entity->set_route(held_route);
        entity->set_name(held_id);
        p_node->set_name(String(held_node_name));
        entity->set_peer_id(held_peer);
        entity->set_player_id(held_player_id);
        entity->write_controller(held_controller);
        return Ref<NetwEntity>();
    }
    return entity;
}

PackedInt32Array NetwMultiplayer::rpc_recipients(const RID &p_entity) {
    NETW_ZONE_NC("session live peers", colors::WIRE);
    const Ref<MultiplayerPeer> peer = inner.is_valid()
        ? inner->get_multiplayer_peer()
        : Ref<MultiplayerPeer>();
    if (peer.is_null() || peer->is_class("OfflineMultiplayerPeer")) {
        PackedInt32Array offline;
        offline.push_back(1);
        return offline;
    }
    store_refresh();
    const uint32_t slot = store_slot_of(p_entity);
    if (slot == store::Table::NO_SLOT) {
        return send_recipients(p_entity, store_roster_ids());
    }
    if (store().slot_stale(slot)) {
        store_evaluate_slot(slot, p_entity);
    }
    return store_reach(slot);
}

void NetwMultiplayer::spawn_execute_plan(
    const LocalVector<repl::SpawnOp> &p_plan,
    int64_t p_spawn_channel,
    int64_t p_hide_channel
) {
    NETW_ZONE_NC("session spawn execute plan", colors::LIVENESS);
    spawn::Book *book = spawn_held_book();
    if (book == nullptr) {
        return;
    }
    HashMap<int64_t, PackedByteArray> spawn_frames;
    HashMap<int64_t, PackedByteArray> hide_frames;
    for (const repl::SpawnOp &operation : p_plan) {
        const int64_t route = operation.route;
        const int64_t peer_id = operation.peer;
        spawn::Record *record = book->spawned_of(route);
        if (record == nullptr) {
            continue;
        }
        Node *node = record->node();
        if (node == nullptr || !node->is_inside_tree()) {
            continue;
        }
        if (operation.action == repl::SpawnAction::SPAWN) {
            HashMap<int64_t, PackedByteArray>::Iterator held
                = spawn_frames.find(route);
            if (!held) {
                held = spawn_frames.insert(
                    route,
                    spawn_encode_spawn_frame(record->get_route(), node)
                );
            }
            if (held->value.is_empty()) {
                continue;
            }
            record->add_recipient(int(peer_id));
            store().stale_route(route);
            world.wire.note_subject(record->get_route());
            send_to(
                peer_id,
                0,
                p_spawn_channel,
                held->value,
                true,
                0,
                String()
            );
            continue;
        }
        const Ref<NetwEntity> entity = NetwEntity::of(node);
        if (entity.is_valid()) {
            Dictionary decision;
            decision[StringName("hide")] = operation.decision.hide;
            decision[StringName("custom")] = operation.decision.custom;
            interest_leave_commit(
                entity->get_rid_handle(),
                peer_id,
                decision,
                operation.forced
            );
        }
        if (operation.action == repl::SpawnAction::RETAIN) {
            continue;
        }
        HashMap<int64_t, PackedByteArray>::Iterator held
            = hide_frames.find(route);
        if (!held) {
            wire::WriteStream stream;
            if (!verb_head_write(stream, route) || !stream.align_verify()) {
                continue;
            }
            held = hide_frames.insert(route, stream.to_bytes());
        }
        record->remove_recipient(int(peer_id));
        store().stale_route(route);
        world.wire.note_subject(route);
        send_to(
            peer_id,
            0,
            p_hide_channel,
            held->value,
            true,
            0,
            String()
        );
    }
}

spawn::Record *NetwMultiplayer::spawn_issue_armed(int64_t p_route) {
    spawn::Book *book = spawn_held_book();
    spawn::Record armed;
    if (book == nullptr || !book->take_armed(p_route, armed)) {
        return nullptr;
    }
    Node *node = armed.node();
    if (node == nullptr || !node->is_inside_tree()) {
        NETW_TRACE(
            sys::SPAWN,
            "Route %d left the tree before its spawn was sent, so the spawn "
            "was dropped.",
            int(p_route)
        );
        return nullptr;
    }
    armed.set_node_name(String(node->get_name()));
    const Ref<NetwEntity> above = NetwEntity::of(node->get_parent());
    armed.set_parent_route(
        above.is_valid() ? liveness_route_of(above.ptr()) : 0
    );
    const Ref<NetwEntity> issued = NetwEntity::of(node);
    const RID entity = issued.is_valid() ? issued->get_rid_handle() : RID();
    interest_sync_scene_membership(entity);
    membership_place_entity(entity);
    store().stale_route(p_route);
    return book->issue(armed);
}

PackedInt32Array NetwMultiplayer::spawn_fan_out(
    spawn::Record *p_record,
    Node *p_node,
    const PackedByteArray &p_payload,
    const PackedInt32Array &p_connected,
    int64_t p_channel,
    int64_t p_first
) {
    PackedInt32Array recipients;
    if (spawn_held_book() == nullptr || p_record == nullptr
        || p_node == nullptr || p_payload.is_empty()) {
        return recipients;
    }
    for (int at = 0; at < p_connected.size(); at++) {
        if (int64_t(p_connected[at]) != p_first
            && spawn_visible_to(
                p_record->get_route(),
                int64_t(p_connected[at]),
                p_node
            )) {
            recipients.push_back(p_connected[at]);
        }
    }
    p_record->set_recipients(recipients);
    if (p_first > 0) {
        p_record->add_recipient(int(p_first));
    }
    store().stale_route(p_record->get_route());
    for (int at = 0; at < recipients.size(); at++) {
        world.wire.note_subject(p_record->get_route());
        send_to(
            int64_t(recipients[at]),
            0,
            p_channel,
            p_payload,
            true,
            0,
            String()
        );
    }
    return recipients;
}

bool NetwMultiplayer::spawn_applying_remote_frame() const {
    return applying_remote_frame;
}

void NetwMultiplayer::spawn_place_node(Node *p_parent, Node *p_node) {
    if (p_parent == nullptr || p_node == nullptr) {
        return;
    }
    const bool was_applying = applying_remote_frame;
    applying_remote_frame = true;
    p_parent->add_child(p_node);
    applying_remote_frame = was_applying;
}

void NetwMultiplayer::lifecycle_undo(
    const RID &p_entity,
    Node *p_parent,
    const AnchorRevision &p_revision
) {
    NetwEntity *const entity = views.holder_of(p_entity.get_id());
    const int64_t route = liveness_route_of(entity);
    Node *owner = entity->get_owner();
    row_streams_reopen(route);
    if (owner != nullptr && p_parent != nullptr
        && owner->get_parent() != p_parent && owner->is_inside_tree()) {
        spawn_replace_keeping_pose(owner, p_parent);
    }
    if (route > 0 && p_revision.revision > 0) {
        entity_install_anchor(route, p_revision.revision, p_revision.author);
    }
}

void NetwMultiplayer::spawn_replace_keeping_pose(Node *p_node, Node *p_parent) {
    const Variant pose = world_pose_of(p_node);
    spawn_unplace_node(p_node);
    spawn_place_node(p_parent, p_node);
    world_pose_restore(p_node, pose);
}

void NetwMultiplayer::spawn_unplace_node(Node *p_node) {
    Node *parent = p_node != nullptr ? p_node->get_parent() : nullptr;
    if (parent == nullptr) {
        return;
    }
    const bool was_applying = applying_remote_frame;
    applying_remote_frame = true;
    parent->remove_child(p_node);
    applying_remote_frame = was_applying;
}

bool NetwMultiplayer::spawn_reparent_node(
    Node *p_node,
    Node *p_parent,
    const Callable &p_adopt,
    int64_t p_route,
    uint64_t p_anchor,
    uint64_t p_anchor_author,
    int64_t p_author
) {
    if (p_node == nullptr || p_parent == nullptr) {
        return false;
    }
    if (scene_outer_of(p_node)->get_parent() == p_parent) {
        if (p_route > 0 && p_anchor > 0) {
            entity_install_anchor(p_route, p_anchor, p_anchor_author);
        }
        return false;
    }
    spawn_carry_begin(
        p_node,
        p_parent,
        p_adopt,
        p_route,
        p_anchor,
        p_anchor_author,
        p_author
    );
    return true;
}

Node *NetwMultiplayer::spawn_carry_pending_parent(int64_t p_route) const {
    uint64_t newest = 0;
    Node *parent = nullptr;
    for (const KeyValue<int64_t, SpawnCarry> &flying : spawn_carries) {
        if (flying.value.route == p_route && !flying.value.moved
            && flying.value.anchor >= newest) {
            newest = flying.value.anchor;
            parent = Object::cast_to<Node>(gd::object_of(flying.value.parent));
        }
    }
    return parent;
}

NetwMultiplayer::AnchorRevision NetwMultiplayer::anchor_standing(
    int64_t p_route
) const {
    AnchorRevision standing = anchor_installed(p_route);
    for (const KeyValue<int64_t, SpawnCarry> &flying : spawn_carries) {
        if (flying.value.route == p_route && !flying.value.moved
            && flying.value.anchor > standing.revision) {
            standing.revision = flying.value.anchor;
            standing.author = flying.value.anchor_author;
        }
    }
    return standing;
}

void NetwMultiplayer::spawn_carry_land_out_of(Node *p_leaving) {
    if (p_leaving == nullptr) {
        return;
    }
    LocalVector<int64_t> leaving;
    for (const KeyValue<int64_t, SpawnCarry> &flying : spawn_carries) {
        Node *node = Object::cast_to<Node>(gd::object_of(flying.value.node));
        Node *parent
            = Object::cast_to<Node>(gd::object_of(flying.value.parent));
        if (flying.value.moved || node == nullptr || parent == nullptr) {
            continue;
        }
        if (p_leaving->is_ancestor_of(node) && parent != p_leaving
            && !p_leaving->is_ancestor_of(parent)) {
            leaving.push_back(flying.key);
        }
    }
    for (const int64_t id : leaving) {
        spawn_carry_advance(id);
    }
}

void NetwMultiplayer::spawn_carry_begin(
    Node *p_node,
    Node *p_parent,
    const Callable &p_adopt,
    int64_t p_route,
    uint64_t p_anchor,
    uint64_t p_anchor_author,
    int64_t p_author
) {
    NETW_ZONE_NC("NetwMultiplayer spawn_carry_begin", colors::SCENE);
    const ObjectID node = gd::instance_id(p_node);
    for (KeyValue<int64_t, SpawnCarry> &flying : spawn_carries) {
        if (flying.value.node == node && !flying.value.moved) {
            flying.value.parent = gd::instance_id(p_parent);
            flying.value.adopt = p_adopt;
            flying.value.route = p_route;
            flying.value.anchor = p_anchor;
            flying.value.anchor_author = p_anchor_author;
            flying.value.author = p_author;
            return;
        }
    }
    SpawnCarry carry;
    carry.node = node;
    carry.parent = gd::instance_id(p_parent);
    carry.adopt = p_adopt;
    carry.route = p_route;
    carry.anchor = p_anchor;
    carry.anchor_author = p_anchor_author;
    carry.author = p_author;
    carry.hush = guard_hush_areas(p_node);
    carry.guard = guard_hold(p_node);
    const int64_t id = ++spawn_carry_next;
    spawn_carries.insert(id, carry);
    spawn_carry_open(id);
}

void NetwMultiplayer::spawn_carry_open(int64_t p_id) {
    if (!spawn_carry_reachable(p_id)) {
        spawn_carry_lose(p_id);
        return;
    }
    const SpawnCarry *carry = spawn_carries.getptr(p_id);
    guard_then(
        carry->guard,
        GUARD_WINDOW_FRAMES,
        callable_mp(this, &NetwMultiplayer::spawn_carry_advance).bind(p_id)
    );
}

bool NetwMultiplayer::spawn_carry_reachable(int64_t p_id) {
    const SpawnCarry *carry = spawn_carries.getptr(p_id);
    return carry != nullptr && gd::object_of(carry->node) != nullptr
        && gd::object_of(carry->parent) != nullptr;
}

void NetwMultiplayer::spawn_carry_advance(int64_t p_id) {
    NETW_ZONE_NC("NetwMultiplayer spawn_carry_advance", colors::SCENE);
    if (!spawn_carry_reachable(p_id)) {
        spawn_carry_lose(p_id);
        return;
    }
    SpawnCarry *carry = spawn_carries.getptr(p_id);
    if (carry->moved) {
        spawn_carry_finish(p_id);
        return;
    }
    carry->moved = true;
    Node *node = Object::cast_to<Node>(gd::object_of(carry->node));
    Node *parent = Object::cast_to<Node>(gd::object_of(carry->parent));
    const Callable adopt = carry->adopt;
    const RID guard = carry->guard;
    const int64_t route = carry->route;
    const uint64_t anchor = carry->anchor;
    const uint64_t anchor_author = carry->anchor_author;
    const Variant pose = world_pose_of(node);
    Node *placed = scene_outer_of(node);
    Node *held = placed->get_parent();
    const bool was_applying = applying_remote_frame;
    const int64_t was_author = admitting_author;
    const int64_t was_route = admitting_route;
    applying_remote_frame = true;
    admitting_author = carry->author;
    admitting_route = carry->author != 0 ? route : 0;
    if (held != nullptr) {
        held->remove_child(placed);
    }
    spawn_place_node(parent, placed);
    applying_remote_frame = was_applying;
    admitting_author = was_author;
    admitting_route = was_route;
    world_pose_restore(node, pose);
    if (route > 0 && anchor > 0) {
        entity_install_anchor(route, anchor, anchor_author);
    }
    reparent_guards.resume_processing(guard);
    if (adopt.is_valid()) {
        adopt.call(entity_of(node));
    }
    spawn_carry_open(p_id);
}

void NetwMultiplayer::spawn_carry_abandon(int64_t p_id) {
    const SpawnCarry *found = spawn_carries.getptr(p_id);
    if (found == nullptr) {
        return;
    }
    const SpawnCarry carry = *found;
    spawn_carries.erase(p_id);
    guard_let_go(carry.guard);
    guard_hush_release(carry.hush);
    NETW_TRACE(
        sys::SPAWN,
        "Reparent %d was abandoned before it finished.",
        int(p_id)
    );
}

void NetwMultiplayer::spawn_carry_lose(int64_t p_id) {
    const SpawnCarry *found = spawn_carries.getptr(p_id);
    if (found == nullptr) {
        return;
    }
    const SpawnCarry carry = *found;
    spawn_carry_abandon(p_id);
    if (carry.author == 0 || carry.moved || carry.route <= 0
        || carry.anchor == 0) {
        return;
    }
    const bool node_gone = gd::object_of(carry.node) == nullptr;
    lifecycle_decide(
        carry.route,
        carry.author,
        lifecycle::Kind::REPARENT,
        carry.anchor - 1,
        node_gone ? ERR_UNAVAILABLE : ERR_INVALID_PARAMETER
    );
}

void NetwMultiplayer::spawn_carry_sweep() {
    while (!spawn_carries.is_empty()) {
        spawn_carry_abandon(spawn_carries.begin()->key);
    }
}

void NetwMultiplayer::spawn_carry_finish(int64_t p_id) {
    const SpawnCarry *found = spawn_carries.getptr(p_id);
    if (found == nullptr) {
        return;
    }
    const SpawnCarry carry = *found;
    spawn_carries.erase(p_id);
    guard_let_go(carry.guard);
    guard_hush_release(carry.hush);
}

bool NetwMultiplayer::spawn_send_reparent(
    spawn::Record *p_record,
    Node *p_node,
    const PackedInt32Array &p_connected,
    int64_t p_channel,
    int64_t p_skip
) {
    if (spawn_held_book() == nullptr || p_record == nullptr
        || p_node == nullptr) {
        return false;
    }
    wire::WriteStream stream;
    AnchorRevision installed = anchor_installed(p_record->get_route());
    if (!verb_head_write(stream, p_record->get_route())
        || !anchor_encode(stream, scene_outer_of(p_node)->get_parent())
        || !stream.varuint(installed.revision, 5)
        || !stream.varuint(installed.author, 5) || !stream.align_verify()) {
        fault::raise(
            this,
            NETW_FAULT_ENTITY_MOVED,
            {String(p_node->get_name()),
             "a parent outside the multiplayer root",
             "a parent inside the multiplayer root"},
            gd::instance_id(p_node)
        );
        return false;
    }
    const PackedByteArray payload = stream.to_bytes();
    const PackedInt32Array recipients = p_record->recipients();
    for (int at = 0; at < recipients.size(); at++) {
        const int64_t peer_id = int64_t(recipients[at]);
        if (peer_id == p_skip || !p_connected.has(recipients[at])) {
            continue;
        }
        if (!spawn_visible_to(p_record->get_route(), peer_id, p_node)) {
            continue;
        }
        world.wire.note_subject(p_record->get_route());
        send_to(peer_id, 0, p_channel, payload, true, 0, String());
    }
    return true;
}

bool NetwMultiplayer::spawn_despawn_route(
    int64_t p_route,
    const PackedInt32Array &p_connected,
    int64_t p_channel,
    Node *p_within
) {
    spawn::Book *book = spawn_held_book();
    if (book == nullptr) {
        return false;
    }
    const PackedInt64Array plan = book->despawn_order(p_route);
    if (plan.is_empty()) {
        return false;
    }
    for (int at = 0; at < plan.size(); at++) {
        const int64_t doomed = plan[at];
        spawn::Record *record = book->spawned_of(doomed);
        if (record == nullptr) {
            continue;
        }
        Node *held = record->node();
        if (p_within != nullptr && held != p_within
            && (held == nullptr || !p_within->is_ancestor_of(held))) {
            continue;
        }
        const PackedInt32Array recipients = record->recipients();
        book->drop_spawned(doomed);
        store().stale_route(doomed);
        if (p_connected.is_empty()) {
            continue;
        }
        const Ref<NetwEntity> dying = wrapper_for_route(doomed);
        const NetwEntityRecord *const dying_record
            = dying.is_valid() ? dying->record_in(this) : nullptr;
        NETW_BUG_COND_V(dying.is_valid() && dying_record == nullptr, true);
        String reason = dying_record != nullptr
            ? String(dying_record->get_despawn_reason())
            : String();
        wire::WriteStream stream;
        if (!verb_head_write(stream, doomed)
            || !wire::string_field(stream, reason) || !stream.align_verify()) {
            continue;
        }
        const PackedByteArray payload = stream.to_bytes();
        for (int index = 0; index < recipients.size(); index++) {
            if (!p_connected.has(recipients[index])) {
                continue;
            }
            world.wire.note_subject(doomed);
            send_to(
                int64_t(recipients[index]),
                0,
                p_channel,
                payload,
                true,
                0,
                String()
            );
        }
    }
    return true;
}

void NetwMultiplayer::spawn_detach_before_despawn(const RID &p_dying) {
    NetwEntity *const dying = views.holder_of(p_dying.get_id());
    Node *owner = dying != nullptr ? dying->get_owner() : nullptr;
    Node *above = owner != nullptr ? owner->get_parent() : nullptr;
    ReplicationCore *plane = get_replication_plane();
    if (above == nullptr || !owner->is_inside_tree() || plane == nullptr) {
        return;
    }
    spawn::Book *book = plane->get_spawn_pipeline()->get_spawn_book();
    const int64_t route = liveness_route_of(dying);
    const PackedInt64Array doomed = book->despawn_order(route);
    PackedInt64Array detaching;
    for (int at = 0; at < doomed.size(); ++at) {
        const spawn::Record *record = book->spawned_of(doomed[at]);
        const Ref<NetwEntity> entity
            = NetwEntity::of(record != nullptr ? record->node() : nullptr);
        if (entity.is_valid()
            && entity->get_on_parent_despawn()
                == NetwEntity::PARENT_DESPAWN_DETACH) {
            detaching.push_back(doomed[at]);
        }
    }
    const PackedInt64Array roots = book->detach_roots(route, detaching);
    for (int at = 0; at < roots.size(); ++at) {
        entity_move(book->spawned_of(roots[at])->node(), above);
    }
}

void NetwMultiplayer::spawn_report_failed(
    int64_t p_route,
    const String &p_reason
) {
    if (is_host() || p_route <= 0) {
        return;
    }
    session::SpawnFailed frame;
    frame.route = p_route;
    frame.reason = p_reason;
    send_to(
        SERVER_PEER,
        0,
        wire::CHANNEL_SESSION_SPAWN_FAILED,
        session::frame_write(frame),
        true,
        0,
        String()
    );
}

void NetwMultiplayer::spawn_tell_player_failed(
    int64_t p_player,
    const StringName &p_method
) {
    const int64_t peer = player_get_peer(p_player);
    if (!is_host() || !player_is_active(p_player) || peer <= 0
        || peer == get_unique_id()) {
        return;
    }
    session::SpawnFailed frame;
    frame.route = 0;
    frame.reason = String(p_method);
    send_to(
        peer,
        0,
        wire::CHANNEL_SESSION_SPAWN_FAILED,
        session::frame_write(frame),
        true,
        0,
        String()
    );
}

void NetwMultiplayer::spawn_receive_failed_frame(
    const PackedByteArray &p_payload,
    int p_sender
) {
    spawn::Pipeline *spawns = spawn_plane();
    session::SpawnFailed frame;
    if (!session::frame_read(p_payload, frame)) {
        return;
    }
    if (!is_host() && p_sender == SERVER_PEER && frame.route == 0) {
        fault::raise(
            this,
            NETW_FAULT_PLAYER_SPAWN_FAILED,
            {frame.reason, String(), String(), SERVER_PEER}
        );
    }
    if (!is_host() || spawns == nullptr) {
        return;
    }
    spawn::Record *record = spawns->get_spawn_book()->spawned_of(frame.route);
    if (record == nullptr || !record->has_recipient(p_sender)
        || !record->mark_failed(p_sender)) {
        return;
    }
    const String method = String(
        record->get_recipe() == spawn::Book::RECIPE_FN_REGISTRY
            ? record->get_fn_registry_id()
            : record->get_fn_method()
    );
    const Ref<NetwPlayer> player = player_of(p_sender);
    fault::raise(
        this,
        NETW_FAULT_SPAWN_REFUSED,
        {vformat(
             "The node for player '%s'",
             player.is_valid() ? String(player->get_username())
                               : String::num_int64(p_sender)
         ),
         vformat("'%s', which %s on their peer", method, frame.reason),
         vformat("'%s' returning a Node on every peer", method),
         p_sender}
    );
    const RID entity = liveness_core.rid_from_route(int(frame.route));
    if (scene_is_declared(entity)) {
        scene_sync_viewer(entity, p_sender);
    }
}

bool NetwMultiplayer::spawn_failed_for(
    int64_t p_route,
    int64_t p_peer
) const {
    spawn::Pipeline *spawns = spawn_plane();
    const spawn::Record *record = spawns != nullptr && p_route > 0
        ? spawns->get_spawn_book()->spawned_of(p_route)
        : nullptr;
    return record != nullptr && record->has_failed(int(p_peer));
}

void NetwMultiplayer::spawn_reanchor(
    spawn::Record *p_record,
    Node *p_node,
    spawn::SpawnerRoster *p_roster
) {
    if (p_record == nullptr || p_roster == nullptr || p_node == nullptr) {
        return;
    }
    if (p_record->get_recipe() != spawn::Book::RECIPE_SPAWNER
        || !p_node->is_inside_tree()) {
        return;
    }
    Node *parent = p_node->get_parent();
    MultiplayerSpawner *current = p_record->spawner();
    if (current != nullptr && current->is_inside_tree()
        && current->get_node_or_null(current->get_spawn_path()) == parent) {
        return;
    }
    Node *root = session_get_root();
    if (root == nullptr) {
        return;
    }
    const LocalVector<Object *> live = p_roster->live();
    for (uint32_t at = 0; at < live.size(); at++) {
        MultiplayerSpawner *spawner
            = Object::cast_to<MultiplayerSpawner>(live[at]);
        if (spawner == nullptr || !spawner->is_inside_tree()
            || !root->is_ancestor_of(spawner)) {
            continue;
        }
        if (spawner->get_node_or_null(spawner->get_spawn_path()) != parent) {
            continue;
        }
        if (p_record->get_scene_index() >= 0) {
            const int index
                = spawn::SpawnerRoster::scene_index_for(spawner, p_node);
            if (index < 0) {
                continue;
            }
            p_record->set_scene_index(index);
        } else if (!spawner->get_spawn_function().is_valid()) {
            continue;
        }
        p_record->bind_spawner(spawner);
        return;
    }
    NETW_TRACE(
        sys::SPAWN,
        "Route %d moved under '%s', which has no matching spawner, so it "
        "keeps its original parent.",
        int(p_record->get_route()),
        parent != nullptr ? String(parent->get_name()).utf8().get_data() : ""
    );
}

void NetwMultiplayer::spawn_refresh_anchor(
    spawn::Record *p_record,
    Node *p_node,
    spawn::SpawnerRoster *p_roster
) {
    if (p_record == nullptr || p_node == nullptr) {
        return;
    }
    const Ref<NetwEntity> above = NetwEntity::of(p_node->get_parent());
    p_record->set_parent_route(
        above.is_valid() ? liveness_route_of(above.ptr()) : 0
    );
    const Ref<NetwEntity> anchored = NetwEntity::of(p_node);
    const RID entity
        = anchored.is_valid() ? anchored->get_rid_handle() : RID();
    interest_sync_scene_membership(entity);
    membership_place_entity(entity);
    spawn_reanchor(p_record, p_node, p_roster);
}

void NetwMultiplayer::spawn_refresh_anchors(spawn::SpawnerRoster *p_roster) {
    NETW_ZONE_NC("session spawn refresh anchors", colors::LIVENESS);
    spawn::Book *book = spawn_held_book();
    if (book == nullptr) {
        return;
    }
    const PackedInt64Array routes = book->spawned_routes();
    for (int at = 0; at < routes.size(); at++) {
        spawn::Record *record = book->spawned_of(routes[at]);
        if (record == nullptr) {
            continue;
        }
        Node *node = record->node();
        if (node != nullptr && node->is_inside_tree()) {
            spawn_refresh_anchor(record, node, p_roster);
        }
    }
}

LocalVector<repl::SpawnRow> NetwMultiplayer::spawn_reconcile_rows(
    const PackedInt32Array &p_peers
) {
    NETW_ZONE_NC("session spawn reconcile rows", colors::LIVENESS);
    LocalVector<repl::SpawnRow> rows;
    spawn::Book *book = spawn_held_book();
    if (book == nullptr) {
        return rows;
    }
    const PackedInt64Array routes = book->ancestry_order();
    for (int at = 0; at < routes.size(); at++) {
        const int64_t route = routes[at];
        spawn::Record *record = book->spawned_of(route);
        if (record == nullptr) {
            continue;
        }
        Node *node = record->node();
        if (node == nullptr || !node->is_inside_tree()
            || persist_withholds(node)) {
            continue;
        }
        const Ref<NetwEntity> entity = NetwEntity::of(node);
        repl::SpawnRow row;
        for (int index = 0; index < p_peers.size(); index++) {
            const int64_t peer_id = int64_t(p_peers[index]);
            const bool desired = spawn_locally_desired(peer_id, node);
            row.local_desired[peer_id] = desired;
            if (entity.is_valid() && !desired
                && record->has_recipient(int(peer_id))) {
                const Dictionary resolved
                    = interest_leave_resolve(entity->get_rid_handle(), peer_id);
                repl::LeaveDecision decision;
                decision.hide = resolved.get(StringName("hide"), true);
                decision.custom = resolved.get(StringName("custom"), Array());
                row.leave.insert(peer_id, decision);
            }
        }
        row.route = route;
        row.parent_route = record->get_parent_route();
        row.recipients = record->recipients();
        rows.push_back(row);
    }
    return rows;
}

Array NetwMultiplayer::live_scenes() const {
    return scene_core.live_scenes();
}

NetwMultiplayer::EntitySpace NetwMultiplayer::entity_space_of(
    Node *p_node
) const {
    EntitySpace out;
    if (p_node == nullptr || !p_node->is_inside_tree()) {
        return out;
    }
    if (Node3D *spatial = Object::cast_to<Node3D>(p_node)) {
        const Ref<World3D> world = spatial->get_world_3d();
        if (world.is_valid()) {
            out.space = world->get_space();
            out.dimension = 3;
        }
        return out;
    }
    if (CanvasItem *canvas = Object::cast_to<CanvasItem>(p_node)) {
        const Ref<World2D> world = canvas->get_world_2d();
        if (world.is_valid()) {
            out.space = world->get_space();
            out.dimension = 2;
        }
    }
    return out;
}

Dictionary NetwMultiplayer::entity_space(const RID &p_entity) const {
    const EntitySpace held = entity_space_of(entity_get_node(p_entity));
    Dictionary out;
    out[StringName("space")] = held.space;
    out[StringName("dimension")] = held.dimension;
    return out;
}

bool NetwMultiplayer::verb_head_write(
    wire::WriteStream &p_stream,
    int64_t p_route
) {
    if (p_route <= 0) {
        return false;
    }
    const int64_t held = liveness_route_epoch(p_route);
    spawn::VerbHead head;
    head.route = uint64_t(p_route);
    head.epoch = uint64_t(held > 0 ? held : 0);
    return spawn::VerbHead::wire.run(p_stream, head);
}

bool NetwMultiplayer::verb_head_read(
    wire::ReadStream &p_stream,
    int64_t &r_route,
    int64_t &r_epoch
) {
    spawn::VerbHead head;
    if (!spawn::VerbHead::wire.run(p_stream, head) || head.route == 0) {
        return false;
    }
    r_route = int64_t(head.route);
    r_epoch = int64_t(head.epoch);
    return true;
}

bool NetwMultiplayer::anchor_encode(
    wire::WriteStream &p_stream,
    Node *p_target
) {
    Node *target = p_target;
    if (target == nullptr) {
        return false;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(target);
    const int64_t route
        = entity.is_valid() ? liveness_route_of(entity.ptr()) : int64_t(0);
    if (entity.is_valid() && route > 0) {
        Node *owner = entity->get_owner();
        if (owner != nullptr) {
            bool entity_relative = true;
            uint64_t staged = uint64_t(route);
            String subpath = String(owner->get_path_to(target));
            return p_stream.bool1(entity_relative)
                && p_stream.varuint(staged, 5)
                && wire::string_field(p_stream, subpath);
        }
    }
    Node *root = session_get_root();
    if (root == nullptr || !(target == root || root->is_ancestor_of(target))) {
        return false;
    }
    bool entity_relative = false;
    String path = String(root->get_path_to(target));
    return p_stream.bool1(entity_relative)
        && wire::string_field(p_stream, path);
}

bool NetwMultiplayer::anchor_decode(
    wire::ReadStream &p_stream,
    spawn::Anchor &r_anchor
) const {
    bool entity_relative = false;
    if (!p_stream.bool1(entity_relative)) {
        return false;
    }
    uint64_t route = 0;
    if (entity_relative && !p_stream.varuint(route, 5)) {
        return false;
    }
    String path;
    if (!wire::string_field(p_stream, path)) {
        return false;
    }
    r_anchor.entity_relative = entity_relative;
    r_anchor.route = entity_relative ? int64_t(route) : int64_t(0);
    r_anchor.path = path;
    return true;
}

Node *NetwMultiplayer::anchor_resolve(const spawn::Anchor &p_anchor) {
    const String &path = p_anchor.path;
    if (p_anchor.entity_relative) {
        const Ref<NetwEntity> entity = wrapper_for_route(p_anchor.route);
        if (entity.is_null()) {
            return nullptr;
        }
        Node *owner = entity->get_owner();
        return owner != nullptr ? owner->get_node_or_null(NodePath(path))
                                : nullptr;
    }
    Node *root = session_get_root();
    return root != nullptr ? root->get_node_or_null(NodePath(path)) : nullptr;
}

void NetwMultiplayer::liveness_when_live(
    int64_t p_route,
    const Callable &p_callback,
    int64_t p_timeout_ticks,
    const Callable &p_on_timeout
) {
    const Clock &clock = clock_engine();
    const bool clocked = clock.get_configured();
    int64_t timeout = p_timeout_ticks;
    if (timeout == 0) {
        timeout = clocked ? clock.get_tickrate() : CLOCKLESS_TICKRATE;
    }
    const int64_t origin
        = clocked ? clock.get_ticks_run() : liveness_core.frame();
    liveness_schedule_when_live(
        p_route,
        p_callback,
        origin,
        timeout,
        clocked,
        p_on_timeout
    );
}

PackedInt64Array NetwMultiplayer::table_claim_routes(
    const RID &p_table,
    int p_count
) {
    PackedInt64Array out;
    if (!is_host()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwMultiplayer.table_claim_routes}"}
        );
        return out;
    }

    if (!array_tables.is_valid(p_table) || p_count <= 0) {
        return out;
    }
    out.resize(p_count);
    for (int at = 0; at < p_count; at++) {
        out.set(at, liveness_reserve_route());
    }
    liveness_bind_routes_data(out);
    return out;
}

Error NetwMultiplayer::table_release_routes(
    const RID &p_table,
    const PackedInt64Array &p_routes
) {
    if (!is_host()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwMultiplayer.table_release_routes}"}
        );
        return ERR_UNCONFIGURED;
    }

    if (!array_tables.is_valid(p_table)) {
        return ERR_DOES_NOT_EXIST;
    }
    if (p_routes.is_empty()) {
        return OK;
    }
    liveness_tombstone_routes_data(p_routes);
    array_tables.retire_routes(p_routes, false);
    array_tables.queue_lifecycle_removals(p_routes);
    return OK;
}

Error NetwMultiplayer::entity_despawn(
    const RID &p_entity,
    const Ref<NetwDespawnOpts> &p_opts
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    const bool held = wrapper.is_valid();
    Node *const owner = held ? wrapper->get_owner() : nullptr;
    String refusal;
    const lifecycle::Ruling ruling = lifecycle_judge_verb(
        lifecycle_local_facts(
            held ? wrapper->get_record() : nullptr,
            owner,
            held ? wrapper->synchronizers()
                 : TypedArray<MultiplayerSynchronizer>(),
            lifecycle::Kind::DESPAWN
        ),
        String("Netw.despawn"),
        owner,
        refusal
    );
    if (!ruling.admitted()) {
        return ruling.code;
    }
    if (wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    wrapper->despawn_admitted(p_opts);
    return OK;
}

Error NetwMultiplayer::entity_add_property_set(
    const RID &p_entity,
    const RID &p_set,
    int64_t p_comp
) {
    if (!liveness_core.entity_is_valid(p_entity)
        || !property_sets.rid_is_valid(p_set)) {
        return ERR_DOES_NOT_EXIST;
    }
    const PropertySet *record = property_set_of(p_set);
    if (record == nullptr || !record->get_sealed()) {
        return ERR_INVALID_DATA;
    }
    if (entity_get_view(p_entity).is_null()) {
        return ERR_UNAVAILABLE;
    }
    Node *node = entity_component_node(p_entity, p_comp);
    if (node == nullptr) {
        return ERR_UNAVAILABLE;
    }
    SyncPipeline *pipeline = sync_pipeline();
    if (pipeline == nullptr) {
        return ERR_UNCONFIGURED;
    }
    const Error verdict
        = pipeline->register_property_set(node, property_set_id_of(p_set));
    if (verdict != OK) {
        return verdict;
    }
    entity_property_sets[p_entity.get_id()][p_comp] = p_set;
    return OK;
}

void NetwMultiplayer::entity_remove_property_set(
    const RID &p_entity,
    int64_t p_comp
) {
    HashMap<int64_t, RID> *attached
        = entity_property_sets.getptr(p_entity.get_id());
    if (attached == nullptr || !attached->has(p_comp)) {
        return;
    }
    Node *node = entity_component_node(p_entity, p_comp);
    SyncPipeline *pipeline = sync_pipeline();
    if (node != nullptr && pipeline != nullptr) {
        pipeline->unregister_derived(node);
        pipeline->reconcile_state_timeline(node);
    }
    attached->erase(p_comp);
    if (attached->is_empty()) {
        entity_property_sets.erase(p_entity.get_id());
    }
}

Variant NetwMultiplayer::entity_get_property(
    const RID &p_entity,
    int64_t p_comp,
    int p_column
) {
    const HashMap<int64_t, RID> *attached
        = entity_property_sets.getptr(p_entity.get_id());
    if (attached == nullptr) {
        return Variant();
    }
    const RID *held = attached->getptr(p_comp);
    if (held == nullptr) {
        return Variant();
    }
    const PropertySet *record = property_set_of(*held);
    if (record == nullptr) {
        return Variant();
    }
    if (p_column < 0 || p_column >= int(record->columns.size())) {
        return Variant();
    }
    Node *node = entity_component_node(p_entity, p_comp);
    if (node == nullptr) {
        return Variant();
    }
    return port_get(node, record->columns[uint32_t(p_column)].shape.key);
}

void NetwMultiplayer::observe_node_added(Node *p_node) {
    observe_node_entity(p_node);
    settle_observe(p_node);
}

void NetwMultiplayer::entity_watch_authority_joins() {
    tree_watch_authority_joins();
}

void NetwMultiplayer::entity_note_node_added(Node *p_node) {
    if (tree::is_entity_root(p_node)) {
        return;
    }
    const Ref<NetwEntity> entity = wrapper_at(p_node->get_parent());
    const NetwEntityRecord *record
        = entity.is_valid() ? entity->get_record() : nullptr;
    if (record == nullptr || !record->projects_to_server()) {
        return;
    }
    entity->project_control_descendant(p_node);
}

void NetwMultiplayer::observe_node_entity_ref(const Variant &p_node_ref) {
    Node *node = Object::cast_to<Node>(gd::held_by_weak_ref(p_node_ref));
    if (node == nullptr) {
        return;
    }
    observe_node_entity(node);
}

void NetwMultiplayer::observe_node_entity(Node *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    if (entity.is_null()) {
        return;
    }
    if (entity->get_name() != StringName()) {
        effect_adopt(entity->get_name());
    }
}

void NetwMultiplayer::observe_entity_spawned(int64_t p_wrapper_id) {
    NetwEntity *entity = Object::cast_to<NetwEntity>(
        gd::object_of(godot::ObjectID(uint64_t(p_wrapper_id)))
    );
    if (entity == nullptr || entity->get_name() == StringName()) {
        return;
    }
    effect_adopt(entity->get_name());
}

} // namespace netw
