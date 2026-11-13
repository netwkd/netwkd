#include "netw/api/entity_record.hpp"

#include "godot/node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/entity/ids.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/predict/decl.hpp"
#include "netw/scene_decl.hpp"
#include "netw/script/model.hpp"
#include "netw/sim/row.hpp"
#include "netw/spawn/record.hpp"
#include "netw/synchronizers.hpp"
#include "netw/tree_hook.hpp"

using namespace godot;

namespace netw {

namespace {

Callable *part_factories() {
    static Callable table[NetwEntityRecord::PART_MAX];
    return table;
}

} // namespace

NetwEntityRecord::NetwEntityRecord() {
}

NetwEntityRecord::~NetwEntityRecord() {
    if (interest_facet != nullptr) {
        memdelete(interest_facet);
    }
    if (display_decl != nullptr) {
        memdelete(display_decl);
    }
    if (predict_decl != nullptr) {
        memdelete(predict_decl);
    }
    if (sim_decl != nullptr) {
        memdelete(sim_decl);
    }
    if (readmission != nullptr) {
        memdelete(readmission);
    }
    entity::release(handle);
}

void NetwEntityRecord::keep_readmission(const spawn::Record &p_recipe) {
    if (readmission == nullptr) {
        readmission = memnew(spawn::Record(p_recipe));
        return;
    }
    *readmission = p_recipe;
}

spawn::Record *NetwEntityRecord::take_readmission() {
    spawn::Record *taken = readmission;
    readmission = nullptr;
    return taken;
}

interest::Facet &NetwEntityRecord::get_interest_facet() {
    if (interest_facet == nullptr) {
        interest_facet = memnew(interest::Facet);
    }
    return *interest_facet;
}

display::Decl &NetwEntityRecord::display_declaration() {
    if (display_decl == nullptr) {
        display_decl = memnew(display::Decl);
    }
    return *display_decl;
}

predict::Decl &NetwEntityRecord::predict_declaration() {
    if (predict_decl == nullptr) {
        predict_decl = memnew(predict::Decl);
    }
    return *predict_decl;
}

int64_t NetwEntityRecord::predict_declaration_bytes() const {
    return predict_decl != nullptr ? int64_t(sizeof(predict::Decl)) : 0;
}

sim::Decl &NetwEntityRecord::sim_declaration() {
    if (sim_decl == nullptr) {
        sim_decl = memnew(sim::Decl);
    }
    return *sim_decl;
}

int64_t NetwEntityRecord::sim_declaration_bytes() const {
    return sim_decl != nullptr ? int64_t(sizeof(sim::Decl)) : 0;
}

Variant NetwEntityRecord::minted_part(int64_t p_part) const {
    for (const MintedPart &held : parts) {
        if (held.part == p_part) {
            return held.value;
        }
    }
    return Variant();
}

bool NetwEntityRecord::adopt_handle(const RID &p_handle) {
    if (p_handle == handle) {
        return true;
    }
    if (!entity::retain(p_handle)) {
        return false;
    }
    entity::release(handle);
    handle = p_handle;
    return true;
}

Variant NetwEntityRecord::part(int64_t p_part, Object *p_wrapper) {
    NETW_BUG_COND_V(p_part < 0 || p_part >= PART_MAX, Variant());
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
    const Variant made = factory.call(p_wrapper);
    const Ref<RefCounted> minted = made;
    NETW_BUG_COND_V(minted.is_null(), Variant());
    NETW_TRACE(sys::ENTITY, "Made entity part %d.", int(p_part));
    keep_part(p_part, made);
    return made;
}

void NetwEntityRecord::keep_part(int64_t p_part, const Variant &p_value) {
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

void NetwEntityRecord::set_part_factory(
    int64_t p_part,
    const Callable &p_factory
) {
    NETW_BUG_COND(p_part < 0 || p_part >= PART_MAX);
    part_factories()[p_part] = p_factory;
}

bool NetwEntityRecord::has_part_factory(int64_t p_part) {
    if (p_part < 0 || p_part >= PART_MAX) {
        return false;
    }
    return part_factories()[p_part].is_valid();
}

Callable NetwEntityRecord::part_factory(int64_t p_part) {
    if (p_part < 0 || p_part >= PART_MAX) {
        return Callable();
    }
    return part_factories()[p_part];
}

void NetwEntityRecord::clear_part_factories() {
    for (int at = 0; at < PART_MAX; at++) {
        part_factories()[at] = Callable();
    }
}

bool NetwEntityRecord::advance(int64_t p_stage) {
    if (!entity::stage_edge_is_legal(stage, p_stage)) {
        return false;
    }
    stage = p_stage;
    return true;
}

bool NetwEntityRecord::transition(int64_t p_stage) {
    if (advance(p_stage)) {
        return true;
    }
    NETW_BUG_V(false);
}

void NetwEntityRecord::deactivate(Node *p_owner) {
    Node *owner = p_owner;
    if (owner == nullptr) {
        return;
    }
    owner->set_process_mode(Node::PROCESS_MODE_DISABLED);
    owner->set(StringName("visible"), false);
    synchronizers::sync_only_server(owner);
}

StringName NetwEntityRecord::entity_meta() {
    return StringName("netw_entity");
}

NetwEntityRecord::Projection NetwEntityRecord::projection_to(
    int64_t p_follower
) const {
    Projection made;
    made.follower = p_follower;
    made.prior_follower
        = projected_follower != 0 ? projected_follower : SERVER_PEER;
    return made;
}

void NetwEntityRecord::write_authority(
    Node *p_node,
    int64_t p_to,
    int64_t p_prior
) {
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

void NetwEntityRecord::project_node(
    Node *p_owner,
    Node *p_node,
    const Projection &p_projection,
    uint64_t p_watch
) {
    if (p_node != p_owner && tree::is_entity_root(p_node)) {
        return;
    }
    write_authority(p_node, p_projection.follower, p_projection.prior_follower);
    const bool watched = p_projection.follower != SERVER_PEER;
    if (watched && p_watch != 0) {
        tree::watch(p_node, p_watch);
    }
    const int children = p_node->get_child_count(true);
    for (int at = 0; at < children; ++at) {
        project_node(
            p_owner,
            p_node->get_child(at, true),
            p_projection,
            p_watch
        );
    }
}

void NetwEntityRecord::project_entered(
    Node *p_owner,
    Node *p_node,
    uint64_t p_watch
) {
    if (p_owner == nullptr || p_node == nullptr || projected_follower == 0
        || !p_owner->is_ancestor_of(p_node)) {
        return;
    }
    Projection entered;
    entered.follower = projected_follower;
    project_node(p_owner, p_node, entered, p_watch);
}

void NetwEntityRecord::apply_control(
    Object *p_wrapper,
    Node *p_owner,
    uint64_t p_watch
) {
    Node *owner = p_owner;
    if (owner == nullptr) {
        return;
    }
    const int64_t steerer
        = entity::Control::public_peer(control.get_controller());
    const int64_t follower
        = control.authority_follows_controller() ? steerer : SERVER_PEER;
    project_node(owner, owner, projection_to(follower), p_watch);
    projected_follower = follower;
    const int64_t was
        = projected_steerer != 0 ? projected_steerer : SERVER_PEER;
    projected_steerer = steerer;
    if (was != steerer && p_wrapper != nullptr) {
        NETW_TRACE(
            sys::ENTITY,
            "Control moved from peer %d to peer %d.",
            int(was),
            int(steerer)
        );
        NetwMultiplayer::Announcement row;
        row.kind = NetwMultiplayer::AnnouncementKind::CONTROL_CHANGED;
        row.from = was;
        row.to = steerer;
        NetwMultiplayer::entity_announce(p_wrapper, row);
    }
}

namespace {

SceneDecl declared_scene_of(Node *p_owner) {
    if (p_owner == nullptr) {
        return SceneDecl();
    }
    Node *owner = p_owner;
    return netw::script::model::get_scene_decl(
        Ref<Script>(Object::cast_to<Script>(owner->get_script()))
    );
}

} // namespace

StringName NetwEntityRecord::scene_label_of(Node *p_owner) {
    if (scene_label != StringName()) {
        return scene_label;
    }
    const SceneDecl decl = declared_scene_of(p_owner);
    return decl.declared && decl.label.is_empty() && p_owner != nullptr
        ? p_owner->get_name()
        : decl.label;
}

int64_t NetwEntityRecord::scene_isolation_of(Node *p_owner) {
    if (scene_isolation >= 0) {
        return scene_isolation;
    }
    return declared_scene_of(p_owner).isolation;
}

void NetwEntityRecord::hydrate_identity(Node *p_owner) {
    Node *owner = p_owner;
    if (owner == nullptr) {
        return;
    }
    const SceneDecl decl = declared_scene_of(owner);
    if (!decl.declared) {
        return;
    }
    declares_scene = true;
    if (scene_label == StringName()) {
        scene_label = decl.label.is_empty() ? owner->get_name() : decl.label;
    }
    if (scene_isolation < 0) {
        scene_isolation = decl.isolation;
    }
}

bool NetwEntityRecord::set_controller(Object *p_wrapper, int64_t p_peer) {
    const int64_t previous = control.get_controller();
    if (!control.set_controller(p_peer)) {
        return false;
    }
    if (p_wrapper != nullptr) {
        NetwMultiplayer::Announcement row;
        row.kind = NetwMultiplayer::AnnouncementKind::CONTROL_CHANGED;
        row.from = previous;
        row.to = p_peer;
        NetwMultiplayer::entity_announce(p_wrapper, row);
    }
    return true;
}

int64_t NetwEntityRecord::admit_control_request(
    Object *p_wrapper,
    int64_t p_requester,
    int64_t p_hold
) {
    if (!control.admits_request()) {
        const NetwEntity *named = Object::cast_to<NetwEntity>(p_wrapper);
        fault::raise(
            nullptr,
            NETW_FAULT_CONTROL_REFUSED,
            {named != nullptr ? String(named->get_name()) : String(),
             String(),
             String(),
             p_requester},
            p_wrapper != nullptr ? uint64_t(gd::instance_id(p_wrapper)) : 0
        );
        return 0;
    }
    Ref<NetwControlRequest> request;
    request.instantiate();
    request->requester = p_requester;
    request->hold = p_hold;
    if (p_wrapper != nullptr) {
        p_wrapper->emit_signal(
            StringName("control_requested"),
            p_requester,
            request
        );
    }
    if (request->denied) {
        NETW_DEBUG(
            sys::ENTITY,
            "The control_requested handler denied peer %d.",
            int(p_requester)
        );
        return 0;
    }
    return p_requester;
}

bool NetwEntityRecord::publish_stage(Object *p_wrapper, int64_t p_from) {
    if (p_from == stage || p_wrapper == nullptr) {
        return true;
    }
    NetwEntity *wrapper = Object::cast_to<NetwEntity>(p_wrapper);
    if (wrapper != nullptr
        && NetwMultiplayer::record_of_wrapper(wrapper) == this) {
        return wrapper->publish_stage(p_from, stage);
    }
    return true;
}

bool NetwEntityRecord::begin_despawn(
    Object *p_wrapper,
    Node *p_owner,
    const Ref<NetwDespawnOpts> &p_opts
) {
    const Ref<RefCounted> keep(Object::cast_to<RefCounted>(p_wrapper));
    if (!entity::stage_can_begin_despawn(stage)) {
        return false;
    }
    Ref<NetwDespawnOpts> opts = p_opts;
    if (opts.is_null()) {
        opts.instantiate();
    }
    despawning_opts = opts;
    despawn_reason = opts->get_reason();
    const int64_t from = stage;
    transition(int64_t(entity::Stage::DESPAWNING));
    if (!publish_stage(p_wrapper, from)) {
        despawning_opts = Ref<NetwDespawnOpts>();
        return false;
    }
    NETW_TRACE(sys::ENTITY, "Despawning because of '%s'.", opts->get_reason());
    if (p_wrapper != nullptr) {
        NetwEntity *wrapper = Object::cast_to<NetwEntity>(p_wrapper);
        const ObjectID owner = gd::instance_id(
            wrapper != nullptr ? wrapper->get_owner() : p_owner
        );
        const Ref<MultiplayerAPI> session = wrapper != nullptr
            ? wrapper->get_multiplayer()
            : Ref<MultiplayerAPI>();
        const RID captured_handle = handle;
        tree::despawning(p_wrapper);
        if (wrapper == nullptr
            || (NetwMultiplayer::record_of_wrapper(wrapper) == this
                && handle == captured_handle
                && wrapper->get_multiplayer() == session
                && gd::instance_id(wrapper->get_owner()) == owner
                && stage == int64_t(entity::Stage::DESPAWNING))) {
            NetwMultiplayer::Announcement row;
            row.kind = NetwMultiplayer::AnnouncementKind::DESPAWNING;
            row.reason = opts->get_reason();
            NetwMultiplayer::entity_announce(p_wrapper, row);
        }
    }
    despawning_opts = Ref<NetwDespawnOpts>();
    return true;
}

bool NetwEntityRecord::finish_teardown(Object *p_wrapper) {
    const Ref<RefCounted> keep(Object::cast_to<RefCounted>(p_wrapper));
    const int64_t from = stage;
    if (!advance(int64_t(entity::Stage::FREED))) {
        return false;
    }
    if (!publish_stage(p_wrapper, from)) {
        return true;
    }
    if (p_wrapper != nullptr) {
        NetwMultiplayer::Announcement row;
        row.kind = NetwMultiplayer::AnnouncementKind::DESPAWNED;
        NetwMultiplayer::entity_announce(p_wrapper, row);
    }
    return true;
}

bool NetwEntityRecord::complete_departure(Object *p_wrapper) {
    const Ref<RefCounted> keep(Object::cast_to<RefCounted>(p_wrapper));
    if (stage == int64_t(entity::Stage::FREED)) {
        return false;
    }
    if (entity::stage_can_begin_despawn(stage)) {
        const int64_t from = stage;
        advance(int64_t(entity::Stage::DESPAWNING));
        if (!publish_stage(p_wrapper, from)) {
            return true;
        }
    }
    return finish_teardown(p_wrapper);
}

void NetwEntityRecord::readmit() {
    stage = int64_t(entity::Stage::UNBOUND);
    route = 0;
    entity::release(handle);
    handle = RID();
    despawning_opts = Ref<NetwDespawnOpts>();
    despawn_reason = StringName();
    projected_follower = 0;
}

} // namespace netw
