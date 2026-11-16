#include "netw/api/interest.hpp"
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
#include "godot/spatial_node.hpp"
#include "godot/time.hpp"
#include "godot/utility.hpp"
#include "godot/viewport.hpp"
#include "godot/world.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/fault_book.hpp"
#include "netw/colors.hpp"
#include "netw/comp_table.hpp"
#include "netw/entity/stage.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/script/model.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/stage.hpp"
#include "netw/synchronizers.hpp"
#include "netw/tree_hook.hpp"
#include "netw/wire/frame.hpp"
#include "netw/wire/registry.hpp"

using namespace godot;
using namespace netw;

namespace netw {

namespace {

const GameCall PERCEPTION_CUSTOM
    = {"The NetwInterest.on_perception_policy custom_callback",
       "func(visible: bool, peer_id: int, layer_id: StringName)",
       3};
const GameCall LAYER_DRIVER
    = {"NetwInterestLayer.driver",
       "func(layer: NetwInterestLayer) -> Array[NetwEntity]",
       1};
const GameCall LAYER_DRIVER_RID
    = {"The NetwMultiplayer.interest_layer_set_driver_callback callback",
       "func(layer: RID) -> Array[RID]",
       1};
const GameCall LAYER_MONITOR
    = {"NetwInterestLayer.monitor",
       "func(entity: NetwEntity, peer_id: int, visible: bool)",
       3};
const GameCall LAYER_MONITOR_RID
    = {"The NetwMultiplayer.interest_layer_set_monitor_callback callback",
       "func(visible: bool, entity: RID, peer_id: int)",
       3};

constexpr const char *SIG_INTEREST_ENTERED = "interest_entered";
constexpr const char *SIG_INTEREST_EXITED = "interest_exited";
constexpr const char *SIG_VISIBILITY_CHANGED = "visibility_changed";
constexpr const char *SIG_VIEWER_ADDED = "viewer_added";
constexpr const char *SIG_VIEWER_REMOVED = "viewer_removed";
constexpr const char *SIG_ENTITY_ADDED = "entity_added";
constexpr const char *SIG_ENTITY_REMOVED = "entity_removed";
constexpr const char *SIG_OBSERVER_ENTERED = "observer_entered";
constexpr const char *SIG_OBSERVER_EXITED = "observer_exited";

} // namespace

interest::Engine &NetwMultiplayer::interest_plane() {
    return interest_engine;
}

void NetwMultiplayer::reset_interest() {
    interest_engine.clear();
}

void NetwMultiplayer::interest_sync_record(NetwEntity *p_wrapper) {
    NetwEntity *current = p_wrapper;
    LocalVector<int64_t> keys;
    LocalVector<int64_t> routes;
    HashSet<int64_t> seen;
    while (current != nullptr) {
        const RID handle = liveness_adopt(current);
        const int64_t key = handle.get_id();
        if (!interest::Engine::is_key(key) || seen.has(key)) {
            break;
        }
        seen.insert(key);
        keys.push_back(key);
        routes.push_back(current->get_route());
        Node *owner = wrapper_owner(handle);
        current = owner == nullptr ? nullptr
                                   : Object::cast_to<NetwEntity>(
                                         wrapper_at(owner->get_parent()).ptr()
                                     );
    }
    for (uint32_t index = keys.size(); index-- > 0;) {
        const int64_t key = keys[index];
        const int64_t parent_key
            = index + 1 < keys.size() ? keys[index + 1] : 0;
        interest_engine.set_parent(key, parent_key);
        int64_t route = routes[index];
        if (route <= 0) {
            route = interest_engine.order_route_for(key);
        }
        interest_engine
            .set_order_key(key, int(keys.size() - 1 - index), int(route));
    }
}

Dictionary NetwMultiplayer::interest_leave_resolve(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id
) {
    if (p_entity.is_null()) {
        return Dictionary();
    }
    const RID handle = p_entity->get_rid_handle();
    StringName freed;
    const Dictionary verdict = interest_leave_engine.resolve(
        handle.get_id(),
        p_peer_id,
        interest_decl_on(p_entity),
        interest_engine,
        &freed
    );
    if (!String(freed).is_empty()) {
        fault::raise(
            this,
            NETW_FAULT_BAD_CALLABLE,
            {vformat("The CUSTOM leave policy of layer '%s'", freed),
             String(),
             String("a callback whose object stays alive while the layer "
                    "uses it")}
        );
    }
    return verdict;
}

void NetwMultiplayer::interest_leave_commit(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id,
    const Dictionary &p_decision,
    bool p_forced
) {
    if (p_entity.is_null()) {
        return;
    }
    interest_leave_engine.commit(
        p_entity->get_rid_handle().get_id(),
        p_peer_id,
        p_decision,
        p_forced
    );
}

void NetwMultiplayer::interest_leave_finish_sweep() {
    interest_leave_engine.finish_sweep();
}

void NetwMultiplayer::interest_retire_entity(const Ref<NetwEntity> &p_entity) {
    if (p_entity.is_null()) {
        return;
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    if (slot == 0 || !interest_engine.has_entity(slot)) {
        return;
    }
    interest_engine.remove_entity(slot);
}

void NetwMultiplayer::interest_release_body(const Ref<NetwEntity> &p_entity) {
    const Ref<NetwMultiplayer> keep(this);
    if (p_entity.is_null()) {
        return;
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    const Array named = interest_engine.memberships(slot);
    for (int at = 0; at < named.size(); ++at) {
        const RID exiting = interest_layer_find(named[at]);
        if (!exiting.is_valid()) {
            continue;
        }
        if (is_host()) {
            layer_remove_entity(exiting, p_entity);
        } else {
            interest_client_untrack(exiting, p_entity);
        }
    }
    interest_retire_entity(p_entity);
    interest_leave_engine.forget_entity(slot);
    interest_engine.set_scene_membership(slot, StringName());
    interest_engine.set_scene_root(slot, StringName());
    interest_clear_perception(p_entity, false);
}

void NetwMultiplayer::interest_refresh_perception(
    const Ref<NetwEntity> &p_entity,
    const Array &p_layer_ids
) {
    const int64_t peer_id = interest_local_player();
    if (peer_id == 0 || p_entity.is_null()
        || p_entity->get_owner() == nullptr) {
        return;
    }
    const RID handle = p_entity->get_rid_handle();
    const int64_t slot = handle.get_id();
    if (is_host()) {
        if (!interest_engine.has_entity(slot)) {
            return;
        }
    } else if (interest_membership_ids(handle).is_empty()) {
        return;
    }
    const bool visible = interest_player_sees(peer_id, p_entity);
    if (!interest_perception.set_visible(slot, visible)) {
        return;
    }
    if (visible) {
        perception_restore(slot);
        perception_dispatch_custom(slot, true, peer_id);
        return;
    }
    Array layers = p_layer_ids;
    if (layers.is_empty()) {
        layers = perception_layers_for(p_entity, peer_id);
    }
    StringName freed;
    const Dictionary verdict = interest_perception.resolve(
        layers,
        interest_decl_on(p_entity),
        interest_engine,
        &freed
    );
    if (!String(freed).is_empty()) {
        fault::raise(
            this,
            NETW_FAULT_BAD_CALLABLE,
            {vformat("The CUSTOM perception policy of layer '%s'", freed),
             String(),
             String("a callback whose object stays alive while the layer "
                    "uses it")}
        );
    }
    if (bool(verdict[StringName("hide")])) {
        perception_hide(slot, p_entity->get_owner());
    }
    const Array actions = verdict[StringName("custom")];
    interest_perception.arm(slot, actions);
    for (int at = 0; at < actions.size(); ++at) {
        const Array action = actions[at];
        const Callable callback = action[0];
        if (callback.is_valid()) {
            Array args;
            args.push_back(false);
            args.push_back(peer_id);
            args.push_back(action[1]);
            game_call(callback, args, PERCEPTION_CUSTOM);
        }
    }
}

void NetwMultiplayer::interest_refresh_all_perception() {
    NETW_ZONE_NC(
        "NetwMultiplayer interest refresh all perception",
        colors::INTEREST
    );
    if (interest_local_player() == 0) {
        return;
    }
    const PackedInt64Array keys = interest_engine.membership_keys();
    for (int at = 0; at < int(keys.size()); ++at) {
        const Ref<NetwEntity> member = Ref<NetwEntity>(
            Object::cast_to<NetwEntity>(wrapper_for_id(keys[at]).ptr())
        );
        if (member.is_valid()) {
            interest_refresh_perception(member, Array());
        }
    }
}

void NetwMultiplayer::interest_reapply_perception(
    const Ref<NetwEntity> &p_entity
) {
    if (p_entity.is_null()) {
        return;
    }
    if (interest_perception.is_known(p_entity->get_rid_handle().get_id())) {
        interest_clear_perception(p_entity, true);
    }
    interest_refresh_perception(p_entity, Array());
}

void NetwMultiplayer::interest_clear_perception(
    const Ref<NetwEntity> &p_entity,
    bool p_restore
) {
    if (p_entity.is_null()) {
        return;
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    if (p_restore) {
        perception_restore(slot);
        perception_dispatch_custom(slot, true, interest_local_player());
    } else {
        perception_snapshots.erase(slot);
        interest_perception.disarm(slot);
    }
    interest_perception.forget(slot);
}

void NetwMultiplayer::interest_clear_all_perception() {
    LocalVector<int64_t> hidden;
    for (const KeyValue<int64_t, LocalVector<PerceptionSnapshot>> &row :
         perception_snapshots) {
        hidden.push_back(row.key);
    }
    for (uint32_t at = 0; at < hidden.size(); ++at) {
        perception_restore(hidden[at]);
    }
    const PackedInt64Array armed = interest_perception.armed_keys();
    const int64_t peer_id = interest_local_player();
    for (int at = 0; at < int(armed.size()); ++at) {
        perception_dispatch_custom(armed[at], true, peer_id);
    }
    interest_perception.clear();
    perception_snapshots.clear();
}

namespace {

constexpr int64_t CLOCKLESS_AWARENESS_TICKRATE = 30;

} // namespace

void NetwMultiplayer::interest_receive_awareness(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    if (p_sender != SERVER_PEER) {
        return;
    }
    LocalVector<interest::Awareness> events;
    if (!interest::awareness_decode(p_payload, events)) {
        NETW_TRACE(
            sys::INTEREST,
            "An awareness update that did not decode fully was refused."
        );
        return;
    }
    for (uint32_t at = 0; at < events.size(); ++at) {
        const interest::Awareness &edge = events[at];
        const bool entered = edge.kind == interest::Awareness::ENTER;
        if (!entered && liveness_route_state(edge.route) != ENTITY_STATE_LIVE) {
            continue;
        }
        const bool clocked = clock_engine().get_configured();
        const int64_t origin
            = clocked ? clock_engine().get_ticks_run() : liveness_core->frame();
        const int64_t timeout = clocked ? clock_engine().get_tickrate()
                                        : CLOCKLESS_AWARENESS_TICKRATE;
        liveness_schedule_when_live(
            edge.route,
            callable_mp(this, &NetwMultiplayer::interest_apply_awareness)
                .bind(edge.route, edge.layer_id, edge.kind),
            origin,
            timeout,
            clocked,
            Callable()
        );
    }
}

void NetwMultiplayer::interest_apply_awareness(
    int64_t p_route,
    const StringName &p_layer_id,
    int p_kind
) {
    const Ref<NetwMultiplayer> keep(this);
    const Ref<NetwEntity> entity = Ref<NetwEntity>(
        Object::cast_to<NetwEntity>(wrapper_for_route(p_route).ptr())
    );
    if (entity.is_null()) {
        return;
    }
    const bool entered = p_kind == interest::Awareness::ENTER;
    const RID event_layer = layer_open(p_layer_id);
    if (!event_layer.is_valid()) {
        return;
    }
    const int64_t value
        = entered ? EventPlane::INTEREST_ENTER : EventPlane::INTEREST_EXIT;
    if (event_records(value)) {
        event_record(
            value,
            entity->get_rid_handle(),
            p_route,
            0,
            OK,
            int64_t(event_layer.get_id())
        );
    }
    if (entered) {
        interest_client_admit(event_layer, entity);
    } else {
        interest_client_revoke(event_layer, entity);
    }
}

bool NetwMultiplayer::interest_can_send_to(int64_t p_peer_id) {
    if (p_peer_id == 0 || p_peer_id == SERVER_PEER) {
        return false;
    }
    if (!has_multiplayer_peer()) {
        return false;
    }
    const Ref<MultiplayerPeer> live = get_multiplayer_peer();
    if (live.is_null() || live->is_class("OfflineMultiplayerPeer")) {
        return false;
    }
    if (live->get_connection_status()
        != MultiplayerPeer::CONNECTION_CONNECTED) {
        return false;
    }
    if (is_host()) {
        return NETW_API_VIRTUAL(get_peer_ids)().has(int32_t(p_peer_id));
    }
    return false;
}

void NetwMultiplayer::interest_queue_layer_awareness(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity,
    int64_t p_observer_peer,
    int p_kind
) {
    if (!is_host() || p_entity.is_null()) {
        return;
    }
    Node *owner = p_entity->get_owner();
    if (owner == nullptr || !owner->is_inside_tree()) {
        return;
    }
    if (!interest_can_send_to(p_observer_peer)) {
        return;
    }
    interest_awareness_queue_layer(
        p_observer_peer,
        liveness_allocate_route(p_entity.ptr()),
        p_layer_id,
        p_kind
    );
    interest_request_flush();
}

void NetwMultiplayer::interest_layer_transition(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id,
    bool p_visible
) {
    const Ref<NetwMultiplayer> keep(this);
    const int64_t slot = p_entity->get_rid_handle().get_id();
    if (p_visible) {
        interest_leave_engine.release(slot, p_peer_id);
    } else {
        interest_leave_engine.record(slot, p_peer_id, p_layer_id);
    }
    const int kind = p_visible ? 1 : 0;
    interest_queue_layer_awareness(p_layer_id, p_entity, p_peer_id, kind);
    interest_observer_edge(p_entity, p_layer_id, p_peer_id, p_visible);
}

void NetwMultiplayer::interest_apply_delta(const interest::Delta &p_delta) {
    NETW_ZONE_NC("NetwMultiplayer interest apply delta", colors::INTEREST);
    const Ref<NetwMultiplayer> keep(this);
    for (int pass = 0; pass < 4; ++pass) {
        const bool visible = pass % 2 == 1;
        const bool held = pass >= 2;
        const Array rows = held
            ? (visible ? p_delta.get_layer_enters() : p_delta.get_layer_exits())
            : (visible ? p_delta.get_layer_shows() : p_delta.get_layer_hides());
        for (int at = 0; at < rows.size(); ++at) {
            const Array row = rows[at];
            const StringName layer_id = row[0];
            const Ref<NetwEntity> entity
                = Ref<NetwEntity>(Object::cast_to<NetwEntity>(
                    wrapper_for_id(int64_t(row[1])).ptr()
                ));
            const int64_t peer_id = interest_engine.peer_of_bit(int(row[2]));
            const RID layer = layer_for_delta(layer_id);
            if (!layer.is_valid() || entity.is_null() || peer_id == 0) {
                continue;
            }
            if (held) {
                layer_report_edge(layer_id, layer, entity, peer_id, visible);
                continue;
            }
            interest_layer_transition(layer_id, entity, peer_id, visible);
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        const bool visible = pass == 1;
        const Array rows = visible ? p_delta.get_shows() : p_delta.get_hides();
        const StringName edge
            = visible ? SIG_INTEREST_ENTERED : SIG_INTEREST_EXITED;
        for (int at = 0; at < rows.size(); ++at) {
            const Array row = rows[at];
            const int64_t slot = int64_t(row[0]);
            const Ref<NetwEntity> entity = Ref<NetwEntity>(
                Object::cast_to<NetwEntity>(wrapper_for_id(slot).ptr())
            );
            if (entity.is_null()) {
                continue;
            }
            const int64_t peer_id = interest_engine.peer_of_bit(int(row[1]));
            if (visible) {
                interest_leave_engine.release(slot, peer_id);
                relay_book.audience_changed(slot);
            }
            const Ref<NetwInterest> facet = entity->held_interest();
            if (facet.is_valid() && peer_id != 0) {
                facet->emit_signal(edge, peer_id);
            }
        }
    }
}

int64_t NetwMultiplayer::interest_local_player() {
    if (!session_is_local_client()) {
        return 0;
    }
    if (has_multiplayer_peer()) {
        return get_unique_id();
    }
    return session_get_role() == ROLE_LISTEN_SERVER ? SERVER_PEER : 0;
}

Array NetwMultiplayer::perception_layers_for(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id
) {
    const RID handle = p_entity->get_rid_handle();
    const Array pending
        = interest_leave_engine.pending_layers(handle.get_id(), p_peer_id);
    if (!pending.is_empty()) {
        return pending;
    }
    return interest_membership_ids(handle);
}

void NetwMultiplayer::perception_hide(int64_t p_slot, Node *p_owner) {
    if (perception_snapshots.has(p_slot) || p_owner == nullptr) {
        return;
    }
    LocalVector<PerceptionSnapshot> snapshot;
    LocalVector<Node *> stack;
    stack.push_back(p_owner);
    while (!stack.is_empty()) {
        Node *node = stack[stack.size() - 1];
        stack.remove_at(stack.size() - 1);
        if (node->is_class("CanvasItem") || node->is_class("Node3D")) {
            const StringName prop = StringName("visible");
            PerceptionSnapshot row;
            row.node = node->get_instance_id();
            row.prop = prop;
            row.value = node->get(prop);
            snapshot.push_back(row);
            node->set(prop, false);
        }
        if (node->is_class("AudioStreamPlayer")
            || node->is_class("AudioStreamPlayer2D")
            || node->is_class("AudioStreamPlayer3D")) {
            const StringName prop = StringName("volume_db");
            PerceptionSnapshot row;
            row.node = node->get_instance_id();
            row.prop = prop;
            row.value = node->get(prop);
            snapshot.push_back(row);
            node->set(prop, -80.0);
        }
        const TypedArray<Node> children = node->get_children();
        for (int at = 0; at < children.size(); ++at) {
            Node *child = Object::cast_to<Node>(children[at]);
            if (child != nullptr && !tree::is_entity_root(child)) {
                stack.push_back(child);
            }
        }
    }
    perception_snapshots[p_slot] = snapshot;
}

void NetwMultiplayer::perception_restore(int64_t p_slot) {
    const LocalVector<PerceptionSnapshot> *held
        = perception_snapshots.getptr(p_slot);
    if (held == nullptr) {
        return;
    }
    LocalVector<PerceptionSnapshot> snapshot;
    for (uint32_t at = 0; at < held->size(); ++at) {
        snapshot.push_back((*held)[at]);
    }
    for (uint32_t at = 0; at < snapshot.size(); ++at) {
        Object *node = ObjectDB::get_instance(snapshot[at].node);
        if (node != nullptr) {
            node->set(snapshot[at].prop, snapshot[at].value);
        }
    }
    perception_snapshots.erase(p_slot);
}

void NetwMultiplayer::perception_dispatch_custom(
    int64_t p_slot,
    bool p_visible,
    int64_t p_peer_id
) {
    const Array actions = interest_perception.disarm(p_slot);
    for (int at = 0; at < actions.size(); ++at) {
        const Array action = actions[at];
        const Callable callback = action[0];
        if (callback.is_valid()) {
            Array args;
            args.push_back(p_visible);
            args.push_back(p_peer_id);
            args.push_back(action[1]);
            game_call(callback, args, PERCEPTION_CUSTOM);
        }
    }
}

interest::Decl *NetwMultiplayer::interest_decl_on(
    const Ref<NetwEntity> &p_entity
) {
    NetwEntityRecord *const record = record_of(p_entity.ptr());
    if (record == nullptr) {
        return nullptr;
    }
    interest::Facet *facet = record->find_interest_facet();
    return facet != nullptr ? facet->declaration() : nullptr;
}

bool NetwMultiplayer::interest_player_sees(
    int64_t p_peer_id,
    const Ref<NetwEntity> &p_entity
) {
    if (p_peer_id == 0 || p_entity.is_null()) {
        return false;
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    if (!is_host()) {
        const interest::Decl *declared = interest_decl_on(p_entity);
        return declared == nullptr
            || interest_engine.projection_admits(slot, declared);
    }
    const int bit = interest_engine.peer_bit_of(p_peer_id);
    if (bit < 0) {
        return false;
    }
    return interest_engine.test(slot, bit);
}

namespace {

Array sorted_names(const Array &p_names) {
    LocalVector<StringName> ordered;
    for (int at = 0; at < p_names.size(); ++at) {
        const StringName id = p_names[at];
        uint32_t seat = ordered.size();
        while (seat > 0 && String(id) < String(ordered[seat - 1])) {
            --seat;
        }
        ordered.insert(seat, id);
    }
    Array out;
    for (uint32_t at = 0; at < ordered.size(); ++at) {
        out.push_back(ordered[at]);
    }
    return out;
}

} // namespace

Array NetwMultiplayer::interest_resolved_layer_ids(
    const Ref<NetwEntity> &p_entity
) {
    if (p_entity.is_null()) {
        return Array();
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    Array named = interest_engine.memberships(slot);
    interest::Decl *decl = is_host() ? nullptr : interest_decl_on(p_entity);
    const Array declared = decl != nullptr ? decl->labels() : Array();
    for (int at = 0; at < declared.size(); ++at) {
        if (!named.has(declared[at])) {
            named.push_back(declared[at]);
        }
    }
    return sorted_names(named);
}

TypedArray<NetwEntity> NetwMultiplayer::interest_shared_entities(
    const Ref<NetwEntity> &p_entity,
    const StringName &p_layer_id
) {
    TypedArray<NetwEntity> out;
    if (p_entity.is_null()) {
        return out;
    }
    const Array resolved = interest_resolved_layer_ids(p_entity);
    Array wanted;
    if (p_layer_id.is_empty()) {
        wanted = resolved;
    } else {
        if (!resolved.has(p_layer_id)) {
            return out;
        }
        wanted.push_back(p_layer_id);
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    LocalVector<Ref<NetwEntity>> found;
    const PackedInt64Array co = interest_engine.co_members(slot, wanted);
    for (int at = 0; at < int(co.size()); ++at) {
        const Ref<NetwEntity> candidate = Ref<NetwEntity>(
            Object::cast_to<NetwEntity>(wrapper_for_id(co[at]).ptr())
        );
        if (candidate.is_valid() && candidate->get_owner() != nullptr) {
            found.push_back(candidate);
        }
    }
    if (!is_host()) {
        HashSet<StringName> asked;
        for (int at = 0; at < wanted.size(); ++at) {
            asked.insert(wanted[at]);
        }
        const TypedArray<NetwEntity> live = liveness_get_entities();
        for (int at = 0; at < live.size(); ++at) {
            const Ref<NetwEntity> candidate = live[at];
            if (candidate.is_null() || candidate == p_entity
                || candidate->get_owner() == nullptr || found.has(candidate)) {
                continue;
            }
            interest::Decl *decl = interest_decl_on(candidate);
            const Array labels = decl != nullptr ? decl->labels() : Array();
            for (int seat = 0; seat < labels.size(); ++seat) {
                if (asked.has(labels[seat])) {
                    found.push_back(candidate);
                    break;
                }
            }
        }
    }
    LocalVector<Ref<NetwEntity>> ordered;
    for (uint32_t at = 0; at < found.size(); ++at) {
        const Ref<NetwEntity> row = found[at];
        const String key = String(row->get_name());
        uint32_t seat = ordered.size();
        while (seat > 0) {
            const Ref<NetwEntity> before = ordered[seat - 1];
            const String other = String(before->get_name());
            const bool after = other < key
                || (other == key
                    && before->get_instance_id() < row->get_instance_id());
            if (after) {
                break;
            }
            --seat;
        }
        ordered.insert(seat, row);
    }
    for (uint32_t at = 0; at < ordered.size(); ++at) {
        out.push_back(ordered[at]);
    }
    return out;
}

Dictionary NetwMultiplayer::interest_monitor_snapshot() {
    Dictionary out;
    out[StringName("layers")] = interest_layers().size();
    out[StringName("entities_filtered")]
        = interest_engine.membership_keys().size();
    out[StringName("visible_edges")] = interest_engine.stats_edges();
    out[StringName("dirty_entities")] = interest_engine.dirty_count();
    out[StringName("relay_backlog")] = liveness_pending_live_count();
    out[StringName("transitions_total")] = interest_engine.transitions_total();
    out[StringName("vanished_dirty_skips")]
        = interest_engine.stats_vanished_dirty_skips();
    return out;
}

bool NetwMultiplayer::interest_wire_admits(
    int64_t p_peer_id,
    const Ref<NetwEntity> &p_entity
) {
    if (p_peer_id == SERVER_PEER
        || (p_entity.is_valid() && p_entity->steering() == p_peer_id)) {
        return true;
    }
    return interest_player_sees(p_peer_id, p_entity);
}

bool NetwMultiplayer::interest_is_visible(const RID &p_entity, int64_t p_peer) {
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (entity.is_null() || p_peer == 0) {
        return false;
    }
    return !interest_entity_has_filter(entity)
        || interest_wire_admits(p_peer, entity);
}

bool NetwMultiplayer::interest_has_committed_intent(
    const Ref<NetwEntity> &p_entity
) {
    if (p_entity.is_null()) {
        return false;
    }
    return interest_engine.had_committed_intent(
        p_entity->get_rid_handle().get_id()
    );
}

bool NetwMultiplayer::interest_bit_admits(
    const Ref<NetwEntity> &p_entity,
    int p_peer_bit
) {
    if (p_entity.is_null()) {
        return false;
    }
    return interest_engine.test(
        p_entity->get_rid_handle().get_id(),
        p_peer_bit
    );
}

bool NetwMultiplayer::interest_check_layer_name(
    NetwMultiplayer *p_session,
    const StringName &p_layer_id,
    const char *p_verb
) {
    if (!p_layer_id.is_empty()) {
        return true;
    }
    fault::raise(
        p_session,
        NETW_FAULT_EMPTY_ARGUMENT,
        {String(p_verb), String(), String("layer name")}
    );
    return false;
}

bool NetwMultiplayer::interest_check_policy(
    NetwMultiplayer *p_session,
    int p_policy,
    int p_last,
    const char *p_verb
) {
    if (p_policy >= 0 && p_policy <= p_last) {
        return true;
    }
    fault::raise(
        p_session,
        NETW_FAULT_BAD_VALUE,
        {String(p_verb),
         String::num_int64(p_policy),
         String("a policy constant")}
    );
    return false;
}

void NetwMultiplayer::interest_forget_layer_row(const StringName &p_layer_id) {
    interest_engine.remove_layer(p_layer_id);
}

int NetwMultiplayer::interest_peer_bit(int64_t p_peer_id) const {
    return interest_engine.peer_bit_of(p_peer_id);
}

bool NetwMultiplayer::interest_has_layer(const StringName &p_layer_id) const {
    return interest_engine.has_layer(p_layer_id);
}

bool NetwMultiplayer::interest_entity_has_filter(
    const Ref<NetwEntity> &p_entity
) {
    if (p_entity.is_null()) {
        return false;
    }
    if (!is_host()) {
        interest::Decl *decl = interest_decl_on(p_entity);
        return decl != nullptr && !decl->labels().is_empty();
    }
    return interest_engine.has_memberships(p_entity->get_rid_handle().get_id());
}

void NetwMultiplayer::interest_peer_connected(int64_t p_peer_id) {
    interest_engine.peer_bit_for(p_peer_id);
    interest_sync_live_peers();
    if (interest_compat_refresh.is_valid()) {
        session_defer(
            interest_compat_refresh,
            StringName("interest_compat_intents")
        );
    }
    interest_request_flush();
}

void NetwMultiplayer::interest_peer_disconnected(int64_t p_peer_id) {
    interest_awareness_forget(p_peer_id);
    interest_leave_engine.forget_peer(p_peer_id);
    interest_engine.forget_peer(p_peer_id);
    interest_sync_live_peers();
    interest_request_flush();
}

void NetwMultiplayer::interest_session_ended() {
    callable_mp(this, &NetwMultiplayer::interest_clear_session).call_deferred();
}

void NetwMultiplayer::interest_clear_session() {
    layer_forget_all();
    interest_leave_engine.clear();
    interest_clear_all_perception();
    interest_awareness_clear();
    reset_interest();
    session_cancel_deferred(interest_flush_key());
    interest_pending = interest::Delta();
    interest_pending_live = false;
}

void NetwMultiplayer::interest_sync_scene_membership(
    const Ref<NetwEntity> &p_entity
) {
    if (!is_host() || p_entity.is_null() || p_entity->get_owner() == nullptr) {
        return;
    }
    liveness_adopt(p_entity.ptr());
    const int64_t slot = p_entity->get_rid_handle().get_id();
    const StringName previous = interest_engine.scene_membership(slot);
    const RID scene = scene_of(entity_of(p_entity->get_owner()));
    const StringName current
        = scene.is_valid() ? scene_layer_id(scene) : StringName();
    interest_engine.set_scene_root(
        slot,
        p_entity->get_declares_scene()
            ? scene_layer_id(p_entity->get_rid_handle())
            : StringName()
    );
    if (!interest_engine.set_scene_membership(slot, current)) {
        return;
    }
    if (!previous.is_empty()) {
        const RID before = interest_layer_find(previous);
        if (before.is_valid()) {
            layer_remove_entity(before, p_entity);
        }
    }
    if (!current.is_empty()) {
        layer_add_entity(layer_open(current), p_entity);
    }
}

PackedInt64Array NetwMultiplayer::interest_known_peers() const {
    return interest_engine.known_peers();
}

void NetwMultiplayer::interest_set_entity_intent(
    const Ref<NetwEntity> &p_entity,
    const PackedInt64Array &p_admitted
) {
    if (p_entity.is_null()) {
        return;
    }
    liveness_adopt(p_entity.ptr());
    interest_sync_record(p_entity.ptr());
    interest_engine.set_intent_for_peers(
        p_entity->get_rid_handle().get_id(),
        p_admitted
    );
    interest_request_flush();
}

void NetwMultiplayer::interest_sync_live_peers() {
    HashSet<int64_t> live;
    if (has_multiplayer_peer()) {
        if (inner.is_valid()) {
            set_peer_ids(gd::api_peer_ids(inner));
        }
        const PackedInt32Array peers = reachable_peer_ids();
        for (int at = 0; at < int(peers.size()); ++at) {
            live.insert(peers[at]);
        }
    }
    const int64_t hosting_player = interest_local_player();
    if (session_get_role() == ROLE_LISTEN_SERVER && hosting_player > 0) {
        live.insert(hosting_player);
    }
    const PackedInt64Array viewers = interest_engine.viewer_peers();
    for (int at = 0; at < int(viewers.size()); ++at) {
        live.insert(viewers[at]);
    }
    PackedInt64Array ids;
    for (const int64_t &peer_id : live) {
        ids.push_back(peer_id);
    }
    if (interest_engine.set_live_peer_ids(ids)
        && interest_compat_refresh.is_valid()) {
        interest_compat_refresh.call();
    }
}

Error NetwMultiplayer::interest_recompute() {
    NETW_STAGE(
        INTEREST,
        "NetwMultiplayer interest recompute",
        colors::INTEREST
    );
    if (interest_committing) {
        interest_request_flush();
        return OK;
    }
    if (!is_host()) {
        interest_pending = interest::Delta();
        interest_pending_live = false;
        return OK;
    }
    interest_sync_live_peers();
    HashSet<int64_t> slots;
    const PackedInt64Array named = interest_engine.membership_keys();
    for (int at = 0; at < int(named.size()); ++at) {
        slots.insert(named[at]);
    }
    const PackedInt64Array intents = interest_engine.intent_keys();
    for (int at = 0; at < int(intents.size()); ++at) {
        slots.insert(intents[at]);
    }
    for (const int64_t &slot : slots) {
        const Ref<NetwEntity> member = Ref<NetwEntity>(
            Object::cast_to<NetwEntity>(wrapper_for_id(slot).ptr())
        );
        if (member.is_null() || member->get_owner() == nullptr) {
            continue;
        }
        if (!interest_engine.has_memberships(slot)
            && !interest_engine.has_intent(slot)) {
            interest_retire_entity(member);
            continue;
        }
        interest_sync_record(member.ptr());
    }
    interest_pending = interest_engine.recompute();
    interest_pending_live = true;
    for (KeyValue<StringName, ClosingLayer> &closing : layer_closing) {
        closing.value.in_pending_delta = true;
    }
    return OK;
}

void NetwMultiplayer::interest_commit() {
    if (!interest_pending_live || interest_committing) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    interest_committing = true;
    interest_apply_delta(interest_pending);
    LocalVector<StringName> reported;
    for (const KeyValue<StringName, ClosingLayer> &closing : layer_closing) {
        if (closing.value.in_pending_delta) {
            reported.push_back(closing.key);
        }
    }
    for (const StringName &name : reported) {
        layer_closing_drop(name);
    }
    interest_engine.commit(interest_pending);
    store_note_interest();
    wrapper_sweep_retired();
    interest_refresh_all_perception();
    interest_pending = interest::Delta();
    interest_pending_live = false;
    interest_committing = false;
    event_record(EventPlane::INTEREST_COMMIT, RID(), 0, 0);
}

void NetwMultiplayer::interest_refresh_compat_intents() {
    ReplicationCore *plane = (get_replication_plane());
    if (plane == nullptr) {
        return;
    }
    plane->get_sync_compat()->refresh_interest_intents();
}

void NetwMultiplayer::interest_send_awareness(
    int64_t p_peer,
    const Array &p_wire
) {
    send_to(
        p_peer,
        0,
        wire::builtin_channel("INTEREST_AWARENESS"),
        interest::awareness_encode(p_wire),
        true,
        0,
        String()
    );
}

namespace {

bool route_stays(NetwMultiplayer *p_plane, int64_t p_route) {
    const Ref<NetwEntity> entity = p_plane->wrapper_for_route(p_route);
    return entity.is_null()
        || entity->get_stage() < int64_t(entity::Stage::DESPAWNING);
}

} // namespace

void NetwMultiplayer::interest_relay_awareness() {
    const Array drained = interest_awareness_drain();
    if (!is_host() || !interest_awareness_send.is_valid()) {
        return;
    }
    for (int at = 0; at < drained.size(); ++at) {
        const Array row = drained[at];
        const int64_t target = row[0];
        if (!interest_can_send_to(target)) {
            continue;
        }
        const Array wire = row[1];
        Array sent;
        for (int edge_at = 0; edge_at < wire.size(); ++edge_at) {
            interest::Awareness edge;
            if (!interest::Awareness::from_array(wire[edge_at], edge)) {
                continue;
            }
            if (edge.kind != interest::Awareness::ENTER
                || route_stays(this, edge.route)) {
                sent.push_back(wire[edge_at]);
            }
        }
        if (!sent.is_empty()) {
            interest_awareness_send.call(target, sent);
        }
    }
}

Error NetwMultiplayer::interest_flush_tail() {
    interest_relay_awareness();
    const Ref<MultiplayerPeer> live = get_multiplayer_peer();
    if (!has_multiplayer_peer() || live.is_null()
        || live->get_connection_status()
            == MultiplayerPeer::CONNECTION_DISCONNECTED) {
        return OK;
    }
    spawn::Pipeline *spawns = spawn_plane();
    if (spawns != nullptr) {
        spawns->schedule_visibility_sweep();
    }
    return OK;
}

StringName NetwMultiplayer::interest_flush_key() {
    return StringName("interest_visibility");
}

void NetwMultiplayer::interest_request_flush() {
    if (!interest_flush.is_valid()) {
        fault::raise(
            this,
            NETW_FAULT_NOT_RUNNING,
            {"Updating interest", String(), "interest"}
        );
        return;
    }
    session_defer(interest_flush, interest_flush_key());
}

void NetwMultiplayer::interest_awareness_queue_layer(
    int64_t p_peer_id,
    int64_t p_route,
    const StringName &p_layer_id,
    int p_kind
) {
    NETW_ZONE_NC("NetwMultiplayer awareness layer edge", colors::INTEREST);
    NETW_BUG_COND(p_peer_id == 0);
    NETW_BUG_COND(p_route == 0);
    NETW_TRACE(
        sys::INTEREST,
        "Awareness layer edge for peer %d on route %d.",
        int(p_peer_id),
        int(p_route)
    );
    interest_relay.append(
        p_peer_id,
        interest::Awareness::layer_edge(p_route, p_layer_id, p_kind)
    );
}

Array NetwMultiplayer::interest_awareness_drain() {
    NETW_ZONE_NC("NetwMultiplayer awareness drain", colors::INTEREST);
    Array out;
    const PackedInt64Array targets = interest_relay.targets();
    for (int at = 0; at < targets.size(); ++at) {
        const Array wire = interest_relay.wire_for(targets[at]);
        if (wire.is_empty()) {
            continue;
        }
        Array row;
        row.push_back(targets[at]);
        row.push_back(wire);
        out.push_back(row);
    }
    interest_relay.clear();
    return out;
}

void NetwMultiplayer::interest_awareness_forget(int64_t p_peer_id) {
    interest_relay.forget(p_peer_id);
}

void NetwMultiplayer::interest_awareness_clear() {
    interest_relay.clear();
}

RID NetwMultiplayer::layer_open(const StringName &p_name) {
    if (p_name.is_empty()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{NetwSession.interest_layer}", String(), "name"}
        );
        return RID();
    }
    const RID *known = layer_by_name.getptr(p_name);
    if (known != nullptr) {
        return *known;
    }
    if (layer_closing.has(p_name)) {
        const Ref<NetwMultiplayer> keep(this);
        interest_flush_now();
        layer_closing_drop(p_name);
        return layer_open(p_name);
    }
    interest_engine.declare_layer(p_name);
    LayerRow row;
    row.name = p_name;
    row.scene_owned = scene_owns_layer(p_name);
    const RID minted = layer_rows.make_rid(row);
    layer_by_name[p_name] = minted;
    Ref<NetwInterestLayer> view;
    view.instantiate();
    view->layer_id = p_name;
    view->session = gd::instance_id(this);
    view->layer = minted;
    views.layer_views[minted] = view;
    return minted;
}

RID NetwMultiplayer::interest_layer_find(const StringName &p_name) const {
    const RID *known = layer_by_name.getptr(p_name);
    return known != nullptr ? *known : RID();
}

StringName NetwMultiplayer::layer_name_of(const RID &p_layer) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    return row != nullptr ? row->name : StringName();
}

Ref<NetwInterestLayer> NetwMultiplayer::layer_view(const RID &p_layer) const {
    const Ref<NetwInterestLayer> *held = views.layer_views.getptr(p_layer);
    return held != nullptr ? *held : Ref<NetwInterestLayer>();
}

RID NetwMultiplayer::interest_layer_create(const StringName &p_name) {
    const RID opened = layer_open(p_name);
    if (!opened.is_valid() || layer_view(opened).is_null()) {
        layer_close(opened);
        return RID();
    }
    return opened;
}

bool NetwMultiplayer::layer_refuses_scene(const RID &p_layer) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr || !row->scene_owned) {
        return false;
    }
    fault::raise(
        const_cast<NetwMultiplayer *>(this),
        NETW_FAULT_UNSUPPORTED,
        {"Changing an interest layer",
         "a layer that belongs to a multiplayer scene",
         "{Netw.scene} to change it"}
    );
    return true;
}

void NetwMultiplayer::interest_layer_free(const RID &p_layer) {
    if (!layer_refuses_scene(p_layer)) {
        layer_free(p_layer);
    }
}

void NetwMultiplayer::layer_free(const RID &p_layer) {
    const Ref<NetwMultiplayer> keep(this);
    layer_monitor_detach(p_layer);
    LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr || row->freeing) {
        return;
    }
    row->freeing = true;
    const StringName name = row->name;
    const TypedArray<NetwEntity> members = layer_get_entities(p_layer);
    for (int at = 0; at < members.size(); at++) {
        layer_remove_entity(p_layer, members[at]);
    }
    const PackedInt64Array viewers = layer_viewer_ids(p_layer);
    for (int at = 0; at < int(viewers.size()); at++) {
        layer_remove_viewer(p_layer, viewers[at]);
    }
    if (layer_rows.get_or_null(p_layer) == nullptr) {
        return;
    }
    if (is_host()) {
        layer_closing_drop(name);
        ClosingLayer closing;
        closing.layer = p_layer;
        layer_closing[name] = closing;
        interest_request_flush();
    }
    interest_forget_layer_row(name);
    layer_close(p_layer);
}

Error NetwMultiplayer::interest_layer_add_viewer(
    const RID &p_layer,
    int64_t p_peer
) {
    if (layer_rows.get_or_null(p_layer) == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    if (layer_refuses_scene(p_layer)) {
        return ERR_LOCKED;
    }
    if (p_peer == 0) {
        return ERR_INVALID_DATA;
    }
    layer_add_viewer(p_layer, p_peer);
    return OK;
}

Error NetwMultiplayer::interest_layer_remove_viewer(
    const RID &p_layer,
    int64_t p_peer
) {
    if (layer_rows.get_or_null(p_layer) == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    if (layer_refuses_scene(p_layer)) {
        return ERR_LOCKED;
    }
    if (p_peer == 0) {
        return ERR_INVALID_DATA;
    }
    layer_remove_viewer(p_layer, p_peer);
    return OK;
}

Error NetwMultiplayer::interest_layer_add_entity(
    const RID &p_layer,
    const RID &p_entity
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (layer_rows.get_or_null(p_layer) == nullptr || wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (layer_refuses_scene(p_layer)) {
        return ERR_LOCKED;
    }
    if (!is_host()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwMultiplayer.interest_layer_add_entity}"},
            gd::instance_id(wrapper->get_owner())
        );
        return ERR_UNAUTHORIZED;
    }
    const bool seated = layer_add_entity(p_layer, wrapper)
        || layer_has_entity(p_layer, wrapper);
    return seated ? OK : ERR_UNAVAILABLE;
}

Error NetwMultiplayer::interest_layer_remove_entity(
    const RID &p_layer,
    const RID &p_entity
) {
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    if (layer_rows.get_or_null(p_layer) == nullptr || wrapper.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (layer_refuses_scene(p_layer)) {
        return ERR_LOCKED;
    }
    if (!is_host()) {
        fault::raise(
            this,
            NETW_FAULT_SERVER_ONLY,
            {"{NetwMultiplayer.interest_layer_remove_entity}"},
            gd::instance_id(wrapper->get_owner())
        );
        return ERR_UNAUTHORIZED;
    }
    layer_remove_entity(p_layer, wrapper);
    return OK;
}

Error NetwMultiplayer::interest_layer_set_param(
    const RID &p_layer,
    LayerParam p_param,
    const Variant &p_value
) {
    if (layer_rows.get_or_null(p_layer) == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    if (layer_refuses_scene(p_layer)) {
        return ERR_LOCKED;
    }
    constexpr const char *VERB = "{NetwMultiplayer.interest_layer_set_param}";
    const int value = int(p_value);
    switch (p_param) {
        case LAYER_PARAM_POLICY:
            if (!interest_check_policy(
                    this,
                    value,
                    LAYER_POLICY_HIDE_FROM_INSIDERS,
                    VERB
                )) {
                return ERR_INVALID_PARAMETER;
            }
            layer_set_policy(p_layer, LayerPolicy(value));
            return OK;
        case LAYER_PARAM_LEAVE_POLICY:
            if (!interest_check_policy(
                    this,
                    value,
                    LEAVE_POLICY_CUSTOM,
                    VERB
                )) {
                return ERR_INVALID_PARAMETER;
            }
            layer_set_leave_policy(p_layer, LeavePolicy(value));
            return OK;
        case LAYER_PARAM_PERCEPTION_POLICY:
            if (!interest_check_policy(
                    this,
                    value,
                    PERCEPTION_POLICY_CUSTOM,
                    VERB
                )) {
                return ERR_INVALID_PARAMETER;
            }
            layer_set_perception_policy(p_layer, PerceptionPolicy(value));
            return OK;
        default:
            fault::raise(
                this,
                NETW_FAULT_BAD_VALUE,
                {"An interest layer setting",
                 vformat("setting %d", int(p_param)),
                 "an interest layer setting that exists"}
            );
            return ERR_INVALID_PARAMETER;
    }
}

Variant NetwMultiplayer::interest_layer_get_param(
    const RID &p_layer,
    LayerParam p_param
) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return Variant();
    }
    switch (p_param) {
        case LAYER_PARAM_POLICY:
            return interest_engine.layer_policy(row->name);
        case LAYER_PARAM_LEAVE_POLICY:
            return interest_engine.layer_leave_policy(row->name);
        case LAYER_PARAM_PERCEPTION_POLICY:
            return interest_engine.layer_perception_policy(row->name);
        default:
            return Variant();
    }
}

StringName NetwMultiplayer::interest_layer_get_name(const RID &p_layer) const {
    return layer_name_of(p_layer);
}

PackedInt64Array NetwMultiplayer::layer_viewer_ids(const RID &p_layer) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    return row != nullptr ? interest_engine.layer_viewers(row->name)
                          : PackedInt64Array();
}

PackedInt32Array NetwMultiplayer::interest_layer_get_viewers(
    const RID &p_layer
) const {
    PackedInt32Array out;
    const PackedInt64Array peers = layer_viewer_ids(p_layer);
    for (int at = 0; at < int(peers.size()); ++at) {
        out.push_back(int32_t(peers[at]));
    }
    return out;
}

bool NetwMultiplayer::interest_layer_has_viewer(
    const RID &p_layer,
    int64_t p_peer_id
) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    return row != nullptr
        && interest_engine.layer_has_viewer(row->name, p_peer_id);
}

TypedArray<NetwEntity> NetwMultiplayer::layer_get_entities(const RID &p_layer) {
    TypedArray<NetwEntity> out;
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return out;
    }
    const PackedInt64Array slots = interest_engine.roster(row->name);
    for (int at = 0; at < int(slots.size()); ++at) {
        const Ref<NetwEntity> member = Ref<NetwEntity>(
            Object::cast_to<NetwEntity>(wrapper_for_id(slots[at]).ptr())
        );
        if (member.is_valid()) {
            out.push_back(member);
        }
    }
    return out;
}

TypedArray<RID> NetwMultiplayer::interest_layer_get_entities(
    const RID &p_layer
) {
    TypedArray<RID> out;
    const TypedArray<NetwEntity> members = layer_get_entities(p_layer);
    for (int at = 0; at < members.size(); ++at) {
        const Ref<NetwEntity> member = members[at];
        out.push_back(member->get_rid_handle());
    }
    return out;
}

bool NetwMultiplayer::layer_has_entity(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity
) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    return row != nullptr && layer_holds(p_entity)
        && interest_engine.roster_has(
            row->name,
            p_entity->get_rid_handle().get_id()
        );
}

bool NetwMultiplayer::interest_layer_has_entity(
    const RID &p_layer,
    const RID &p_entity
) const {
    return layer_has_entity(p_layer, entity_get_view(p_entity));
}

Dictionary NetwMultiplayer::interest_layer_get_stats(
    const RID &p_layer
) const {
    Dictionary out;
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return out;
    }
    const StringName name = row->name;
    out[StringName("viewers")] = interest_engine.layer_viewers(name).size();
    out[StringName("entities")] = interest_engine.roster(name).size();
    out[StringName("visible_edges")] = interest_engine.layer_edge_count(name);
    out[StringName("transitions_total")] = interest_engine.transitions(name);
    return out;
}

void NetwMultiplayer::interest_layer_report_edge(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id,
    bool p_visible
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    layer_report_edge(row->name, p_layer, p_entity, p_peer_id, p_visible);
}

RID NetwMultiplayer::layer_for_delta(const StringName &p_name) const {
    const RID live = interest_layer_find(p_name);
    if (views.layer_views.has(live)) {
        return live;
    }
    const ClosingLayer *closing = layer_closing.getptr(p_name);
    return closing != nullptr && views.layer_views.has(closing->layer)
        ? closing->layer
        : RID();
}

void NetwMultiplayer::layer_closing_drop(const StringName &p_name) {
    const ClosingLayer *closing = layer_closing.getptr(p_name);
    if (closing == nullptr) {
        return;
    }
    if (layer_rows.get_or_null(closing->layer) == nullptr) {
        views.layer_views.erase(closing->layer);
    }
    layer_closing.erase(p_name);
}

void NetwMultiplayer::layer_report_edge(
    const StringName &p_name,
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id,
    bool p_visible
) {
    if (!layer_holds(p_entity)) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    const StringName name = p_name;
    if (interest_engine.has_layer(name)) {
        interest_engine.note_transition(name);
    }
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    const StringName edge
        = p_visible ? SIG_INTEREST_ENTERED : SIG_INTEREST_EXITED;
    view->emit_signal(edge, p_entity, p_peer_id);
    if (p_visible) {
        layer_dispatch_enter(name, p_entity, p_peer_id);
        return;
    }
    layer_dispatch_leave(name, p_entity, p_peer_id);
}

void NetwMultiplayer::interest_client_admit(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    const StringName name = row->name;
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (p_entity.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwInterestLayer.client_admit}"),
             String(),
             String("entity")},
            uint64_t(view->get_instance_id())
        );
        return;
    }
    if (!layer_holds(p_entity)
        || !interest_check_layer_name(
            this,
            name,
            "{NetwInterestLayer.add_entity}"
        )
        || !interest_engine.roster_add(
            name,
            p_entity->get_rid_handle().get_id()
        )) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    layer_track_membership(name, p_entity, true);
    layer_join_label(name, p_entity);
    interest_observer_edge(p_entity, name, layer_local_peer(), true);
    layer_dispatch_enter(name, p_entity, layer_local_peer());
    view->emit_signal(SIG_VISIBILITY_CHANGED, p_entity, true);
    layer_refresh_perception(name, p_entity);
}

void NetwMultiplayer::interest_client_revoke(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    const StringName name = row->name;
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (p_entity.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwInterestLayer.client_revoke}"),
             String(),
             String("entity")},
            uint64_t(view->get_instance_id())
        );
        return;
    }
    if (!layer_holds(p_entity)
        || !interest_engine.roster_remove(
            name,
            p_entity->get_rid_handle().get_id()
        )) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    interest_observer_edge(p_entity, name, layer_local_peer(), false);
    layer_dispatch_leave(name, p_entity, layer_local_peer());
    view->emit_signal(SIG_VISIBILITY_CHANGED, p_entity, false);
    layer_refresh_perception(name, p_entity);
}

void NetwMultiplayer::interest_client_untrack(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    const StringName name = row->name;
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (p_entity.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwInterestLayer.client_untrack_entity}"),
             String(),
             String("entity")},
            uint64_t(view->get_instance_id())
        );
        return;
    }
    if (!layer_holds(p_entity)
        || !interest_engine.roster_remove(
            name,
            p_entity->get_rid_handle().get_id()
        )) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    view->emit_signal(SIG_ENTITY_REMOVED, p_entity);
    layer_track_membership(name, p_entity, false);
    interest_observer_edge(p_entity, name, layer_local_peer(), false);
    layer_dispatch_leave(name, p_entity, layer_local_peer());
    view->emit_signal(SIG_VISIBILITY_CHANGED, p_entity, false);
    layer_refresh_perception(name, p_entity);
}

bool NetwMultiplayer::layer_holds(const Ref<NetwEntity> &p_entity) const {
    return p_entity.is_valid()
        && (p_entity->home == nullptr || p_entity->home == this);
}

int64_t NetwMultiplayer::layer_local_peer() {
    return has_multiplayer_peer() ? get_unique_id() : SERVER_PEER;
}

void NetwMultiplayer::layer_dispatch_enter(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id
) {
    NetwEntityRecord *const record = record_of(p_entity.ptr());
    interest::Facet *facet
        = record != nullptr ? record->find_interest_facet() : nullptr;
    if (facet != nullptr) {
        facet->dispatch_enter(p_layer_id, p_peer_id);
    }
}

void NetwMultiplayer::layer_dispatch_leave(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer_id
) {
    NetwEntityRecord *const record = record_of(p_entity.ptr());
    interest::Facet *facet
        = record != nullptr ? record->find_interest_facet() : nullptr;
    if (facet != nullptr) {
        facet->dispatch_leave(p_layer_id, p_peer_id);
    }
}

void NetwMultiplayer::layer_join_label(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity
) {
    NetwEntityRecord *const record = record_of(p_entity.ptr());
    if (record != nullptr) {
        record->get_interest_facet().join(p_layer_id);
    }
}

void NetwMultiplayer::layer_track_membership(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity,
    bool p_joined
) {
    if (p_joined) {
        liveness_adopt(p_entity.ptr());
        interest_engine.membership_add(
            p_entity->get_rid_handle().get_id(),
            p_layer_id
        );
        liveness_adopt(p_entity.ptr());
    } else {
        interest_engine.membership_remove(
            p_entity->get_rid_handle().get_id(),
            p_layer_id
        );
    }
    const int64_t slot = p_entity->get_rid_handle().get_id();
    if (!interest_engine.has_memberships(slot)
        && !interest_engine.has_intent(slot)) {
        if (slot != 0 && interest_engine.has_entity(slot)) {
            interest_engine.remove_entity(slot);
        }
    } else {
        interest_sync_record(p_entity.ptr());
    }
    interest_request_flush();
}

void NetwMultiplayer::layer_refresh_perception(
    const StringName &p_layer_id,
    const Ref<NetwEntity> &p_entity
) {
    Array layers;
    layers.push_back(p_layer_id);
    interest_refresh_perception(p_entity, layers);
}

bool NetwMultiplayer::layer_set_policy(
    const RID &p_layer,
    LayerPolicy p_policy
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return false;
    }
    const StringName name = row->name;
    constexpr const char *VERB = "{NetwInterestLayer.set_policy}";
    if (!interest_check_layer_name(this, name, VERB)
        || !interest_check_policy(
            this,
            p_policy,
            LAYER_POLICY_HIDE_FROM_INSIDERS,
            VERB
        )
        || !interest_engine.layer_set_policy(name, p_policy)) {
        return false;
    }
    interest_request_flush();
    return true;
}

void NetwMultiplayer::layer_set_leave_policy(
    const RID &p_layer,
    LeavePolicy p_policy
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    const StringName name = row->name;
    constexpr const char *VERB = "{NetwInterestLayer.set_default_leave_policy}";
    if (!interest_check_layer_name(this, name, VERB)
        || !interest_check_policy(this, p_policy, LEAVE_POLICY_CUSTOM, VERB)) {
        return;
    }
    interest_engine.layer_set_leave_policy(name, p_policy);
}

void NetwMultiplayer::layer_set_perception_policy(
    const RID &p_layer,
    PerceptionPolicy p_policy
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    const StringName name = row->name;
    constexpr const char *VERB
        = "{NetwInterestLayer.set_default_perception_policy}";
    if (!interest_check_layer_name(this, name, VERB)
        || !interest_check_policy(
            this,
            p_policy,
            PERCEPTION_POLICY_CUSTOM,
            VERB
        )
        || !interest_engine.layer_set_perception_policy(name, p_policy)) {
        return;
    }
    const PackedInt64Array slots = interest_engine.roster(name);
    for (int at = 0; at < int(slots.size()); ++at) {
        const Ref<NetwEntity> member = Ref<NetwEntity>(
            Object::cast_to<NetwEntity>(wrapper_for_id(slots[at]).ptr())
        );
        if (member.is_valid()) {
            interest_reapply_perception(member);
        }
    }
}

bool NetwMultiplayer::layer_add_viewer(const RID &p_layer, int64_t p_peer_id) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return false;
    }
    const StringName name = row->name;
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (has_multiplayer_peer()
        && p_peer_id != MultiplayerPeer::TARGET_PEER_SERVER
        && interest_peer_bit(p_peer_id) < 0) {
        fault::raise(
            this,
            NETW_FAULT_UNKNOWN_PEER,
            {String("{NetwInterestLayer.add_viewer}"),
             String(),
             String(),
             p_peer_id},
            uint64_t(view->get_instance_id())
        );
    }
    if (!interest_check_layer_name(
            this,
            name,
            "{NetwInterestLayer.add_viewer}"
        )
        || !interest_engine.layer_add_viewer(name, p_peer_id)) {
        return false;
    }
    const Ref<NetwMultiplayer> keep(this);
    view->emit_signal(SIG_VIEWER_ADDED, p_peer_id);
    interest_request_flush();
    return true;
}

bool NetwMultiplayer::layer_remove_viewer(
    const RID &p_layer,
    int64_t p_peer_id
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return false;
    }
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (!interest_engine.layer_remove_viewer(row->name, p_peer_id)) {
        return false;
    }
    const Ref<NetwMultiplayer> keep(this);
    view->emit_signal(SIG_VIEWER_REMOVED, p_peer_id);
    interest_request_flush();
    return true;
}

bool NetwMultiplayer::layer_add_entity(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr || row->freeing) {
        return false;
    }
    const StringName name = row->name;
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (p_entity.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwInterestLayer.add_entity}"),
             String(),
             String("entity")},
            uint64_t(view->get_instance_id())
        );
        return false;
    }
    if (!is_host() || !layer_holds(p_entity)) {
        return false;
    }
    if (!interest_check_layer_name(
            this,
            name,
            "{NetwInterestLayer.add_entity}"
        )
        || !interest_engine.roster_add(
            name,
            p_entity->get_rid_handle().get_id()
        )) {
        return false;
    }
    const Ref<NetwMultiplayer> keep(this);
    view->emit_signal(SIG_ENTITY_ADDED, p_entity);
    if (layer_rows.get_or_null(p_layer) != nullptr) {
        layer_track_membership(name, p_entity, true);
    }
    return true;
}

bool NetwMultiplayer::layer_remove_entity(
    const RID &p_layer,
    const Ref<NetwEntity> &p_entity
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return false;
    }
    const StringName name = row->name;
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (p_entity.is_null()) {
        fault::raise(
            this,
            NETW_FAULT_EMPTY_ARGUMENT,
            {String("{NetwInterestLayer.remove_entity}"),
             String(),
             String("entity")},
            uint64_t(view->get_instance_id())
        );
        return false;
    }
    if (!is_host() || !layer_holds(p_entity)) {
        return false;
    }
    if (!interest_engine.roster_remove(
            name,
            p_entity->get_rid_handle().get_id()
        )) {
        return false;
    }
    const Ref<NetwMultiplayer> keep(this);
    view->emit_signal(SIG_ENTITY_REMOVED, p_entity);
    layer_track_membership(name, p_entity, false);
    return true;
}

void NetwMultiplayer::interest_layer_set_driver_callback(
    const RID &p_layer,
    const Callable &p_callback
) {
    if (!layer_refuses_scene(p_layer)) {
        layer_set_driver(p_layer, p_callback, false);
    }
}

void NetwMultiplayer::layer_set_driver(
    const RID &p_layer,
    const Callable &p_callback,
    bool p_typed
) {
    if (layer_rows.get_or_null(p_layer) == nullptr) {
        return;
    }
    if (p_callback.is_valid()) {
        game_call_declared(
            p_callback,
            p_typed ? LAYER_DRIVER : LAYER_DRIVER_RID
        );
    }
    LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    if (!p_callback.is_valid()) {
        row->driver = LayerDriverRow();
        layer_drivers.erase(p_layer);
        return;
    }
    if (!row->driver.drive.is_valid()) {
        layer_drivers.push_back(p_layer);
    }
    row->driver.drive = p_callback;
    row->driver.typed = p_typed;
}

Callable NetwMultiplayer::layer_get_driver(
    const RID &p_layer,
    bool p_typed
) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    return row != nullptr && row->driver.typed == p_typed ? row->driver.drive
                                                          : Callable();
}

Error NetwMultiplayer::layer_drive(const RID &p_layer) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr || !row->driver.drive.is_valid()) {
        return OK;
    }
    const Ref<NetwMultiplayer> keep(this);
    const Ref<NetwInterestLayer> record = layer_view(p_layer);
    const String layer_name = row->name;
    const LayerDriverRow driver = row->driver;
    Array args;
    if (driver.typed) {
        args.push_back(record);
    } else {
        args.push_back(p_layer);
    }
    bool called = false;
    const Variant answered = game_call(
        driver.drive,
        args,
        driver.typed ? LAYER_DRIVER : LAYER_DRIVER_RID,
        &called
    );
    if (!called) {
        return ERR_INVALID_DATA;
    }
    if (answered.get_type() != Variant::ARRAY) {
        fault::raise(
            this,
            NETW_FAULT_BAD_RETURN,
            {vformat("The driver of layer '%s'", layer_name),
             "something that is not an Array",
             "an Array of the entities that belong in the layer"}
        );
        return ERR_INVALID_DATA;
    }

    const Array wanted = answered;
    LocalVector<Ref<NetwEntity>> desired;
    HashSet<int64_t> desired_ids;
    for (int at = 0; at < wanted.size(); at++) {
        Ref<NetwEntity> entity;
        if (driver.typed) {
            entity = Ref<NetwEntity>(Object::cast_to<NetwEntity>(wanted[at]));
            if (entity.is_valid()
                && entity_get_view(entity->get_rid_handle()) != entity) {
                entity.unref();
            }
        } else if (wanted[at].get_type() == Variant::RID) {
            entity = entity_get_view(wanted[at]);
        }
        if (entity.is_null()) {
            fault::raise(
                this,
                NETW_FAULT_BAD_RETURN,
                {vformat("The driver of layer '%s'", layer_name),
                 "something that is not an entity of this session",
                 "only entities spawned in this session"}
            );
            return ERR_DOES_NOT_EXIST;
        }
        desired.push_back(entity);
        desired_ids.insert(entity->get_rid_handle().get_id());
    }
    const TypedArray<NetwEntity> seated = layer_get_entities(p_layer);
    for (int at = 0; at < seated.size(); at++) {
        const Ref<NetwEntity> held = seated[at];
        if (held.is_valid()
            && !desired_ids.has(held->get_rid_handle().get_id())) {
            layer_remove_entity(p_layer, held);
        }
    }
    for (const Ref<NetwEntity> &entity : desired) {
        layer_add_entity(p_layer, entity);
    }
    return OK;
}

Error NetwMultiplayer::run_layer_drivers() {
    NETW_ZONE_NC("NetwMultiplayer run layer drivers", colors::INTEREST);
    if (layer_drivers.is_empty()) {
        return OK;
    }
    const Ref<NetwMultiplayer> keep(this);
    LocalVector<RID> driven;
    driven.reserve(layer_drivers.size());
    for (const RID &layer : layer_drivers) {
        driven.push_back(layer);
    }
    for (const RID &layer : driven) {
        const Error answered = layer_drive(layer);
        if (answered != OK) {
            return answered;
        }
    }
    return OK;
}

Error NetwMultiplayer::interest_flush_now() {
    if (interest_committing) {
        interest_request_flush();
        return OK;
    }
    const Ref<NetwMultiplayer> keep(this);
    const Error driven = run_layer_drivers();
    if (driven != OK) {
        return driven;
    }
    session_cancel_deferred(interest_flush_key());
    const Error folded = interest_recompute();
    if (folded != OK) {
        return folded;
    }
    interest_commit();
    return interest_flush_tail();
}

bool NetwMultiplayer::interest_admits(const RID &p_entity, int64_t p_peer) {
    const int bit = interest_peer_bit(p_peer);
    if (bit < 0) {
        return false;
    }
    const Ref<NetwEntity> wrapper = entity_get_view(p_entity);
    return wrapper.is_valid() && interest_bit_admits(wrapper, bit);
}

TypedArray<RID> NetwMultiplayer::interest_get_layers(const RID &p_entity) {
    TypedArray<RID> out;
    const Array named = interest_membership_ids(p_entity);
    for (int at = 0; at < named.size(); at++) {
        const RID layer = interest_layer_find(named[at]);
        if (layer.is_valid()) {
            out.push_back(layer);
        }
    }
    return out;
}

bool NetwMultiplayer::scene_owns_layer(const StringName &p_layer) const {
    if (p_layer.is_empty()) {
        return false;
    }
    const Array live = scene_core->live_scenes();
    for (int at = 0; at < live.size(); ++at) {
        const RID scene = live[at];
        if (scene_layer_id(scene) == p_layer) {
            return true;
        }
    }
    return false;
}

void NetwMultiplayer::layer_mark_scene_owned(
    const StringName &p_layer,
    bool p_owned
) {
    LayerRow *row = layer_rows.get_or_null(interest_layer_find(p_layer));
    if (row != nullptr) {
        row->scene_owned = p_owned;
    }
}

void NetwMultiplayer::layer_close(const RID &p_layer) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    const ClosingLayer *closing = layer_closing.getptr(row->name);
    if (closing == nullptr || closing->layer != p_layer) {
        views.layer_views.erase(p_layer);
    }
    layer_by_name.erase(row->name);
    layer_drivers.erase(p_layer);
    layer_rows.free(p_layer);
}

void NetwMultiplayer::layer_monitor_report(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer,
    const RID &p_layer,
    bool p_inside
) {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr || !row->monitor.watch.is_valid()
        || p_entity.is_null()) {
        return;
    }
    const LayerMonitorRow monitor = row->monitor;
    Array args;
    if (monitor.typed) {
        args.push_back(p_entity);
        args.push_back(p_peer);
        args.push_back(p_inside);
        game_call(monitor.watch, args, LAYER_MONITOR);
        return;
    }
    args.push_back(p_inside);
    args.push_back(entity_of(p_entity->get_owner()));
    args.push_back(p_peer);
    game_call(monitor.watch, args, LAYER_MONITOR_RID);
}

void NetwMultiplayer::layer_monitor_enter(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer,
    RID p_layer
) {
    layer_monitor_report(p_entity, p_peer, p_layer, true);
}

void NetwMultiplayer::layer_monitor_exit(
    const Ref<NetwEntity> &p_entity,
    int64_t p_peer,
    RID p_layer
) {
    layer_monitor_report(p_entity, p_peer, p_layer, false);
}

void NetwMultiplayer::layer_monitor_visibility(
    const Ref<NetwEntity> &p_entity,
    bool p_visible,
    RID p_layer
) {
    layer_monitor_report(p_entity, get_unique_id(), p_layer, p_visible);
}

void NetwMultiplayer::layer_monitor_detach(const RID &p_layer) {
    LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row != nullptr) {
        row->monitor = LayerMonitorRow();
    }
}

void NetwMultiplayer::interest_layer_set_monitor_callback(
    const RID &p_layer,
    const Callable &p_callback
) {
    layer_set_monitor(p_layer, p_callback, false);
}

Callable NetwMultiplayer::layer_get_monitor(
    const RID &p_layer,
    bool p_typed
) const {
    const LayerRow *row = layer_rows.get_or_null(p_layer);
    return row != nullptr && row->monitor.typed == p_typed ? row->monitor.watch
                                                           : Callable();
}

void NetwMultiplayer::layer_set_monitor(
    const RID &p_layer,
    const Callable &p_callback,
    bool p_typed
) {
    layer_monitor_detach(p_layer);
    const Ref<NetwInterestLayer> view = layer_view(p_layer);
    if (layer_rows.get_or_null(p_layer) == nullptr || !p_callback.is_valid()) {
        return;
    }
    game_call_declared(p_callback, p_typed ? LAYER_MONITOR : LAYER_MONITOR_RID);
    LayerRow *row = layer_rows.get_or_null(p_layer);
    if (row == nullptr) {
        return;
    }
    row->monitor.watch = p_callback;
    row->monitor.typed = p_typed;
    connect_once(
        Signal(view.ptr(), StringName("interest_entered")),
        callable_mp(this, &NetwMultiplayer::layer_monitor_enter).bind(p_layer),
        0
    );
    connect_once(
        Signal(view.ptr(), StringName("interest_exited")),
        callable_mp(this, &NetwMultiplayer::layer_monitor_exit).bind(p_layer),
        0
    );
    connect_once(
        Signal(view.ptr(), StringName("visibility_changed")),
        callable_mp(this, &NetwMultiplayer::layer_monitor_visibility)
            .bind(p_layer),
        0
    );
}

void NetwMultiplayer::layer_forget_all() {
    LocalVector<RID> owned;
    owned.resize(layer_rows.get_rid_count());
    layer_rows.fill_owned_buffer(owned.ptr());
    for (const RID &layer : owned) {
        layer_rows.free(layer);
    }
    layer_by_name.clear();
    layer_drivers.clear();
    layer_closing.clear();
    views.layer_views.clear();
}

int64_t NetwMultiplayer::layer_driver_count() const {
    return int64_t(layer_drivers.size());
}

Ref<NetwInterestLayer> NetwMultiplayer::interest_layer(
    const StringName &p_name
) {
    const RID layer = layer_open(p_name);
    return layer_refuses_scene(layer) ? Ref<NetwInterestLayer>()
                                      : layer_view(layer);
}

Ref<NetwInterestLayer> NetwMultiplayer::interest_layer_named(
    const StringName &p_name
) const {
    const RID layer = interest_layer_find(p_name);
    return layer_refuses_scene(layer) ? Ref<NetwInterestLayer>()
                                      : layer_view(layer);
}

Array NetwMultiplayer::interest_layers() const {
    Array out;
    for (const KeyValue<StringName, RID> &named : layer_by_name) {
        const Ref<NetwInterestLayer> view = layer_view(named.value);
        if (view.is_valid()) {
            out.push_back(view);
        }
    }
    return out;
}

interest::Facet *NetwMultiplayer::interest_facet_on(
    const Ref<NetwEntity> &p_entity
) {
    NetwEntityRecord *const record = record_of(p_entity.ptr());
    return record != nullptr ? &record->get_interest_facet() : nullptr;
}

interest::Facet *NetwMultiplayer::interest_facet_of(const RID &p_entity) {
    return interest_facet_on(entity_get_view(p_entity));
}

interest::Decl *NetwMultiplayer::interest_decl_of(const RID &p_entity) {
    return interest_decl_on(entity_get_view(p_entity));
}

Error NetwMultiplayer::interest_join(const RID &p_entity, const RID &p_layer) {
    const StringName layer_id = layer_name_of(p_layer);
    if (layer_id.is_empty()) {
        return ERR_DOES_NOT_EXIST;
    }
    return interest_join_label(p_entity, layer_id);
}

Error NetwMultiplayer::interest_leave(const RID &p_entity, const RID &p_layer) {
    const StringName layer_id = layer_name_of(p_layer);
    if (layer_id.is_empty()) {
        return ERR_DOES_NOT_EXIST;
    }
    return interest_leave_label(p_entity, layer_id);
}

Error NetwMultiplayer::interest_on_enter(
    const RID &p_entity,
    const RID &p_layer,
    const Callable &p_callback
) {
    const StringName layer_id = layer_name_of(p_layer);
    if (layer_id.is_empty()) {
        return ERR_DOES_NOT_EXIST;
    }
    return interest_on_enter_label(p_entity, layer_id, p_callback);
}

Error NetwMultiplayer::interest_on_leave(
    const RID &p_entity,
    const RID &p_layer,
    const Callable &p_callback
) {
    const StringName layer_id = layer_name_of(p_layer);
    if (layer_id.is_empty()) {
        return ERR_DOES_NOT_EXIST;
    }
    return interest_on_leave_label(p_entity, layer_id, p_callback);
}

Error NetwMultiplayer::interest_on_leave_policy(
    const RID &p_entity,
    const RID &p_layer,
    LeavePolicy p_policy,
    const Callable &p_custom
) {
    const StringName layer_id = layer_name_of(p_layer);
    if (layer_id.is_empty()) {
        return ERR_DOES_NOT_EXIST;
    }
    return interest_leave_policy_label(p_entity, layer_id, p_policy, p_custom);
}

Error NetwMultiplayer::interest_on_perception_policy(
    const RID &p_entity,
    const RID &p_layer,
    PerceptionPolicy p_policy,
    const Callable &p_custom
) {
    const StringName layer_id = layer_name_of(p_layer);
    if (layer_id.is_empty()) {
        return ERR_DOES_NOT_EXIST;
    }
    return interest_perception_policy_label(
        p_entity,
        layer_id,
        p_policy,
        p_custom
    );
}

Error NetwMultiplayer::interest_join_label(
    const RID &p_entity,
    const StringName &p_layer_id
) {
    interest::Facet *facet = interest_facet_of(p_entity);
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (facet == nullptr || entity.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    entity->get_interest();
    if (!facet->join(p_layer_id)) {
        return p_layer_id.is_empty() ? ERR_INVALID_PARAMETER : OK;
    }
    store_stale_entity(entity);
    RID layer = interest_layer_find(p_layer_id);
    if (!layer.is_valid()) {
        layer = layer_open(p_layer_id);
        if (layer.is_valid() && layer_view(layer).is_null()) {
            layer_close(layer);
            return ERR_UNAVAILABLE;
        }
    }
    if (is_host() && layer.is_valid()) {
        liveness_adopt(entity.ptr());
        layer_add_entity(layer, entity);
    }
    return OK;
}

Error NetwMultiplayer::interest_leave_label(
    const RID &p_entity,
    const StringName &p_layer_id
) {
    interest::Facet *facet = interest_facet_of(p_entity);
    const Ref<NetwEntity> entity = entity_get_view(p_entity);
    if (facet == nullptr || entity.is_null()) {
        return ERR_DOES_NOT_EXIST;
    }
    if (!facet->leave(p_layer_id)) {
        return OK;
    }
    store_stale_entity(entity);
    if (is_host()) {
        layer_remove_entity(interest_layer_find(p_layer_id), entity);
    }
    return OK;
}

Error NetwMultiplayer::interest_on_enter_label(
    const RID &p_entity,
    const StringName &p_layer_id,
    const Callable &p_callback
) {
    interest::Facet *facet = interest_facet_of(p_entity);
    if (facet == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    return facet->on_enter(p_layer_id, p_callback) ? OK
                                                   : ERR_INVALID_PARAMETER;
}

Error NetwMultiplayer::interest_on_leave_label(
    const RID &p_entity,
    const StringName &p_layer_id,
    const Callable &p_callback
) {
    interest::Facet *facet = interest_facet_of(p_entity);
    if (facet == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    return facet->on_leave(p_layer_id, p_callback) ? OK
                                                   : ERR_INVALID_PARAMETER;
}

Error NetwMultiplayer::interest_leave_policy_label(
    const RID &p_entity,
    const StringName &p_layer_id,
    LeavePolicy p_policy,
    const Callable &p_custom
) {
    interest::Facet *facet = interest_facet_of(p_entity);
    if (facet == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    return facet->set_leave_policy(p_layer_id, int(p_policy), p_custom)
        ? OK
        : ERR_INVALID_PARAMETER;
}

Error NetwMultiplayer::interest_perception_policy_label(
    const RID &p_entity,
    const StringName &p_layer_id,
    PerceptionPolicy p_policy,
    const Callable &p_custom
) {
    interest::Facet *facet = interest_facet_of(p_entity);
    if (facet == nullptr) {
        return ERR_DOES_NOT_EXIST;
    }
    if (!facet->set_perception_policy(p_layer_id, int(p_policy), p_custom)) {
        return ERR_INVALID_PARAMETER;
    }
    interest_reapply_perception(entity_get_view(p_entity));
    return OK;
}

void NetwMultiplayer::interest_activate(const Ref<NetwEntity> &p_entity) {
    interest::Facet *facet = interest_facet_on(p_entity);
    if (facet == nullptr) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    const Array labels = facet->declaration()->labels();
    for (int at = 0; at < labels.size(); at++) {
        const StringName layer_id = labels[at];
        RID layer = interest_layer_find(layer_id);
        if (!layer.is_valid()) {
            layer = layer_open(layer_id);
            if (layer.is_valid() && layer_view(layer).is_null()) {
                layer_close(layer);
                layer = RID();
            }
        }
        if (!is_host() || !layer.is_valid()) {
            continue;
        }
        liveness_adopt(p_entity.ptr());
        layer_add_entity(layer, p_entity);
    }
}

void NetwMultiplayer::interest_deactivate(const Ref<NetwEntity> &p_entity) {
    interest::Facet *facet = interest_facet_on(p_entity);
    if (facet == nullptr || !is_host()) {
        return;
    }
    const Ref<NetwMultiplayer> keep(this);
    const Array labels = facet->declaration()->labels();
    for (int at = 0; at < labels.size(); at++) {
        layer_remove_entity(interest_layer_find(labels[at]), p_entity);
    }
}

void NetwMultiplayer::interest_observer_edge(
    const Ref<NetwEntity> &p_entity,
    const StringName &p_layer_id,
    int64_t p_peer,
    bool p_entered
) {
    const Ref<NetwMultiplayer> keep(this);
    const int64_t value
        = p_entered ? EventPlane::OBSERVER_ENTERED : EventPlane::OBSERVER_LEFT;
    if (event_records(value)) {
        event_record(
            value,
            p_entity->get_rid_handle(),
            p_entity->get_route(),
            p_peer,
            OK,
            int64_t(layer_for_delta(p_layer_id).get_id())
        );
    }
    const Ref<NetwInterest> handle = p_entity->held_interest();
    if (handle.is_null()) {
        return;
    }
    NetwEntityRecord *const record = record_of(p_entity.ptr());
    interest::Facet *facet
        = record != nullptr ? record->find_interest_facet() : nullptr;
    if (facet != nullptr && facet->observes()) {
        if (p_entered) {
            facet->dispatch_observed(p_peer);
        } else {
            facet->dispatch_unobserved(p_peer);
        }
    }
    const StringName edge
        = p_entered ? SIG_OBSERVER_ENTERED : SIG_OBSERVER_EXITED;
    if (handle->has_connections(edge)) {
        handle->emit_signal(edge, p_layer_id, p_peer);
    }
}

bool NetwMultiplayer::interest_is_filtered(const RID &p_entity) {
    if (!is_host()) {
        interest::Decl *decl = interest_decl_of(p_entity);
        return decl != nullptr && !decl->labels().is_empty();
    }
    return interest_engine.has_memberships(p_entity.get_id());
}

Array NetwMultiplayer::interest_membership_ids(const RID &p_entity) {
    LocalVector<StringName> ordered;
    const Array committed = interest_engine.memberships(p_entity.get_id());
    Array source = committed;
    if (committed.is_empty() && !is_host()) {
        interest::Decl *decl = interest_decl_of(p_entity);
        source = decl != nullptr ? decl->labels() : Array();
    }
    for (int index = 0; index < source.size(); ++index) {
        const StringName id = source[index];
        uint32_t at = ordered.size();
        while (at > 0 && String(id) < String(ordered[at - 1])) {
            --at;
        }
        ordered.insert(at, id);
    }
    Array out;
    for (uint32_t index = 0; index < ordered.size(); ++index) {
        out.push_back(ordered[index]);
    }
    return out;
}

} // namespace netw
