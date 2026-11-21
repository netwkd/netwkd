#include "netw/spawn/pipeline.hpp"

#include "godot/packed_scene.hpp"
#include "godot/resource.hpp"
#include "godot/resource_uid.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/utility.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/property_config.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/entity/stage.hpp"
#include "netw/fault_book.hpp"
#include "netw/lifecycle/rule.hpp"
#include "netw/log.hpp"
#include "netw/object_port.hpp"
#include "netw/profile.hpp"
#include "netw/scene_loads.hpp"
#include "netw/script/model.hpp"
#include "netw/session/event_plane.hpp"
#include "netw/stage.hpp"
#include "netw/subsystems.hpp"
#include "netw/synchronizers.hpp"

using namespace godot;

namespace netw::spawn {

namespace {

const char *SESSION_ENDED_MID_SPAWN
    = "The session ended before the spawn function returned.";
const double CLOCKLESS_TICKRATE = 30.0;
constexpr int BLOB_CAP = 4095;

bool authors(lifecycle::Kind p_kind, bool p_is_server) {
    return lifecycle::rule(p_kind, p_is_server).admitted();
}

int64_t route_named_by(const PackedByteArray &p_payload) {
    wire::ReadStream stream(p_payload);
    int64_t route = 0;
    int64_t epoch = 0;
    if (!NetwMultiplayer::verb_head_read(stream, route, epoch)) {
        return 0;
    }
    return route;
}

bool reparent_is_newer(
    const NetwMultiplayer *p_session,
    int64_t p_route,
    uint64_t p_revision
) {
    return p_revision > p_session->anchor_standing(p_route).revision;
}

Book::Received received_of(const Built &p_built) {
    Book::Received out;
    out.node = gd::instance_id(p_built.node);
    out.recipe = int32_t(p_built.frame.recipe);
    switch (Book::Recipe(p_built.frame.recipe)) {
        case Book::RECIPE_FN:
            out.source = gd::instance_id(p_built.host);
            out.fn = StringName(p_built.frame.method);
            out.payload = p_built.args;
            out.fn_waits = p_built.frame.fn_waits;
            break;
        case Book::RECIPE_FN_REGISTRY:
            out.fn = StringName(p_built.frame.fn_id);
            out.payload = p_built.args;
            break;
        case Book::RECIPE_SPAWNER:
            out.source = gd::instance_id(p_built.spawner);
            out.scene_index = int32_t(p_built.frame.scene_index);
            out.payload = p_built.data;
            break;
        case Book::RECIPE_ADOPT:
            out.source = gd::instance_id(p_built.origin);
            break;
    }
    return out;
}

NodeOp node_op(NodeOp::Kind p_kind, ObjectID p_node) {
    NodeOp op;
    op.kind = p_kind;
    op.node = p_node;
    return op;
}

} // namespace

void Pipeline::seat(
    NetwMultiplayer *p_session,
    Clock *p_clock,
    NetwLivenessCore *p_liveness
) {
    session = p_session;
    clock_engine = p_clock;
    liveness_core = p_liveness;
    if (p_session == nullptr) {
        return;
    }
    park.hold_routes_in(p_liveness);
}

void Pipeline::unseat() {
    session = nullptr;
}

void Pipeline::set_sync_adapter(SyncCompat *p_sync) {
    sync_compat = p_sync;
}

void Pipeline::set_channels(
    int64_t p_spawn,
    int64_t p_despawn,
    int64_t p_hide,
    int64_t p_reparent
) {
    channel_spawn = p_spawn;
    channel_despawn = p_despawn;
    channel_hide = p_hide;
    channel_reparent = p_reparent;
}

bool Pipeline::is_server_authority() const {
    return session != nullptr ? session->is_host() : true;
}

bool Pipeline::has_peer() const {
    return session != nullptr && session->session_get_inner().is_valid()
        && session->session_get_inner()->get_multiplayer_peer().is_valid();
}

PackedInt32Array Pipeline::session_peers() const {
    return session != nullptr ? session->spawn_peer_ids() : PackedInt32Array();
}

Error Pipeline::stage_verdict(
    int64_t p_stage,
    Error p_verdict,
    int64_t p_route
) {
    if (session == nullptr) {
        return ERR_UNAVAILABLE;
    }
    return session->finish_stage_verdict(p_stage, p_verdict, p_route);
}

void Pipeline::sink_verdict(Error p_verdict, int64_t p_route) {
    if (session != nullptr) {
        session->sink_verdict(p_verdict, p_route);
    }
}


Error Pipeline::declare_stage(const RID &p_handle, const Dictionary &p_facts) {
    if (session == nullptr) {
        return OK;
    }
    const Error declared = session->spawn_declare(p_handle);
    return stage_verdict(
        EventPlane::SPAWN_DECLARE,
        declared,
        int64_t(p_facts[StringName("route")])
    );
}

void Pipeline::arm(const Record &p_record) {
    spawn_book.arm(p_record);
}

void Pipeline::await_entry(int64_t p_route, Node *p_node) {
    unadded[p_route]
        = Unadded{gd::instance_id(p_node), clock_engine->get_ticks_run()};
}

void Pipeline::entered(int64_t p_route) {
    unadded.erase(p_route);
}

void Pipeline::drop_armed(int64_t p_route) {
    spawn_book.drop_armed(p_route);
}

void Pipeline::note_requested(int64_t p_route, int64_t p_sender) {
    lifecycle_ops_admitted += 1;
    first_recipients[p_route] = p_sender;
}

void Pipeline::count_nested_published() {
    spawn_nested_published += 1;
}

bool Pipeline::take_placed_before_host(int64_t p_route) {
    return placed_before_host.erase(p_route);
}

bool Pipeline::refuse_placed_once(uint64_t p_node) {
    if (refused_placed.has(p_node)) {
        return false;
    }
    refused_placed.insert(p_node);
    return true;
}

void Pipeline::report_unadded(int64_t p_ticks_run) {
    LocalVector<int64_t> settled;
    for (const KeyValue<int64_t, Unadded> &row : unadded) {
        if (row.value.armed_at_ticks_run >= p_ticks_run) {
            continue;
        }
        settled.push_back(row.key);
        Node *node
            = Object::cast_to<Node>(gd::instance_from_id(row.value.node));
        if (node == nullptr || node->is_inside_tree()
            || !spawn_book.has_armed(row.key)) {
            continue;
        }
        fault::raise(
            session,
            NETW_FAULT_SPAWN_NOT_PLACED,
            {String(node->get_name())},
            gd::instance_id(node)
        );
    }
    for (const int64_t route : settled) {
        unadded.erase(route);
    }
}

bool Pipeline::armed_player_left(int64_t p_route) {
    const Ref<NetwEntity> entity = session != nullptr
        ? session->wrapper_for_route(p_route)
        : Ref<NetwEntity>();
    if (entity.is_null() || entity->get_player_id() == 0) {
        return false;
    }
    const Ref<NetwPlayer> seated = session->player_of(entity->get_peer_id());
    if (session->player_holds(seated)
        && seated->get_id() == entity->get_player_id()) {
        return false;
    }
    NETW_INFO(
        sys::SPAWN,
        "The player of '%s' left before it was added, so it was discarded.",
        String(entity->get_name())
    );
    return true;
}

void Pipeline::discard_armed_spawn(int64_t p_route, Ops &r_ops) {
    Record taken;
    if (!spawn_book.take_armed(p_route, taken)) {
        return;
    }
    Node *body = taken.node();
    if (body == nullptr) {
        return;
    }
    r_ops.rows.push_back(node_op(NodeOp::DISCARD, gd::instance_id(body)));
}

bool Pipeline::flush_armed_spawn(int64_t p_route, Ops &r_ops) {
    NETW_STAGE(SPAWN, "Spawn flush armed", colors::LIVENESS);
    NETW_ZONE_VALUE(p_route);
    if (session == nullptr) {
        return false;
    }
    if (!is_server_authority()) {
        send_authored_spawn(p_route);
        return false;
    }
    if (armed_player_left(p_route)) {
        discard_armed_spawn(p_route, r_ops);
        return false;
    }
    Record *record = session->spawn_issue_armed(p_route);
    if (record == nullptr) {
        return false;
    }
    Node *node = record->node();
    int64_t first = 0;
    if (const HashMap<int64_t, int64_t>::Iterator named
        = first_recipients.find(p_route)) {
        first = named->value;
        first_recipients.remove(named);
    }
    if (has_peer()) {
        const PackedByteArray payload = encode_spawn_frame(p_route, node);
        if (payload.is_empty()) {
            return false;
        }
        session->spawn_fan_out(
            record,
            node,
            payload,
            session_peers(),
            channel_spawn,
            first
        );
        session->spawn_defer_carrier_flush();
    }
    return true;
}

void Pipeline::send_authored_spawn(int64_t p_route) {
    Record armed;
    if (session == nullptr || !spawn_book.take_armed(p_route, armed)) {
        return;
    }
    Node *node = armed.node();
    if (node == nullptr || !node->is_inside_tree() || !has_peer()) {
        return;
    }
    armed.set_node_name(String(node->get_name()));
    const PackedByteArray payload = encode_record_frame(&armed, p_route, node);
    if (payload.is_empty()) {
        return;
    }
    Book::Received recipe = Book::received_of(armed);
    recipe.authored = true;
    spawn_book.enroll_recv(p_route, recipe);
    session->lifecycle_send_to_session(p_route, channel_spawn, payload);
    session->spawn_defer_carrier_flush();
}

bool Pipeline::roots_published_entity(Node *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    return entity.is_valid() && entity->get_route() > 0
        && entity->get_owner() == p_node;
}

bool Pipeline::is_booked(int64_t p_route) const {
    return spawn_book.has_spawned(p_route) || spawn_book.has_armed(p_route)
        || spawn_book.is_recv(p_route);
}

bool Pipeline::books_node(Node *p_node) const {
    if (spawn_book.books_node(p_node)) {
        return true;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    return entity.is_valid() && entity->get_owner() == p_node
        && spawn_book.is_recv(entity->get_route());
}

void Pipeline::note_placed_before_host(int64_t p_route) {
    placed_before_host.insert(p_route);
}

bool Pipeline::owns_spawned_route(int64_t p_route) const {
    return spawn_book.has_spawned(p_route) || spawn_book.is_recv(p_route);
}

void Pipeline::settle_move(int64_t p_route, int64_t p_author) {
    if (!authors(lifecycle::Kind::REPARENT, is_server_authority())) {
        return;
    }
    Record *record = spawn_book.spawned_of(p_route);
    if (record == nullptr) {
        return;
    }
    Node *node = record->node();
    if (node == nullptr || !node->is_inside_tree()) {
        return;
    }
    send_reparent(record, node, p_author);
}

void Pipeline::settle_death(int64_t p_route) {
    if (!authors(lifecycle::Kind::DESPAWN, is_server_authority())) {
        settle_absence(p_route);
        return;
    }
    if (!spawn_book.has_spawned(p_route)) {
        return;
    }
    despawn_tracked_route(p_route);
}

void Pipeline::despawn_now(int64_t p_route, Node *p_within) {
    if (!authors(lifecycle::Kind::DESPAWN, is_server_authority())
        || !spawn_book.has_spawned(p_route)) {
        return;
    }
    despawn_tracked_route(p_route, p_within);
}

bool Pipeline::holds_received_route(int64_t p_route) const {
    return spawn_book.is_recv(p_route);
}

bool Pipeline::holds_spawned_route(int64_t p_route) const {
    return spawn_book.has_spawned(p_route);
}

void Pipeline::settle_absence(int64_t p_route) {
    if (!spawn_book.is_recv(p_route)) {
        return;
    }
    spawn_book.drop_recv(p_route);
    if (session != nullptr) {
        session->action_gate_drop(p_route);
    }
}

void Pipeline::despawn_tracked_route(int64_t p_route, Node *p_within) {
    NETW_STAGE(SPAWN, "Spawn despawn route", colors::LIVENESS);
    NETW_ZONE_VALUE(p_route);
    if (session == nullptr) {
        return;
    }
    const bool live = has_peer() && session->session_is_online();
    if (session->spawn_despawn_route(
            p_route,
            live ? session_peers() : PackedInt32Array(),
            channel_despawn,
            p_within
        )
        && live) {
        session->spawn_defer_carrier_flush();
    }
}

void Pipeline::send_reparent(Record *p_record, Node *p_node, int64_t p_author) {
    if (session == nullptr) {
        return;
    }
    session->spawn_refresh_anchor(p_record, p_node, session->spawner_roster());
    if (!has_peer()) {
        return;
    }
    if (!session->session_is_online()) {
        return;
    }
    session->spawn_sweep_now();
    if (!session->spawn_send_reparent(
            p_record,
            p_node,
            session_peers(),
            channel_reparent,
            p_author
        )) {
        return;
    }
    session->spawn_defer_carrier_flush();
    session->spawn_schedule_visibility_sweep();
}

namespace {

LocalVector<call_args::Slot> slots_of_values(const Array &p_values) {
    LocalVector<call_args::Slot> out;
    out.reserve(uint32_t(p_values.size()));
    for (int at = 0; at < p_values.size(); ++at) {
        out.push_back(call_args::of_value(p_values[at]));
    }
    return out;
}

} // namespace

PackedByteArray Pipeline::encode_fn_args(
    const Array &p_schema,
    const Array &p_args
) {
    const LocalVector<call_args::Slot> slots = session != nullptr
        ? session->rpc_encoded_args(p_args)
        : slots_of_values(p_args);
    wire::WriteStream stream;
    if (!call_args::write(stream, slots, p_schema[0], p_schema[1], session)
        || !stream.align_verify()) {
        return PackedByteArray();
    }
    return stream.to_bytes();
}

LocalVector<call_args::Slot> Pipeline::read_fn_args(
    const PackedByteArray &p_bytes,
    const Array &p_schema
) {
    wire::ReadStream stream(p_bytes);
    LocalVector<call_args::Slot> slots;
    if (!call_args::read(stream, p_schema[0], p_schema[1], slots)
        || !stream.align_verify() || stream.bits_remaining() != 0) {
        return LocalVector<call_args::Slot>();
    }
    return slots;
}

#ifdef NETW_TESTS
void Pipeline::arm_spawn_state(const Array &p_rows) {
    armed_spawn_state.clear();
    for (int at = 0; at < p_rows.size(); ++at) {
        armed_spawn_state.push_back(p_rows[at]);
    }
    spawn_state_is_armed = true;
}

void Pipeline::disarm_spawn_state() {
    armed_spawn_state.clear();
    spawn_state_is_armed = false;
}

void Pipeline::delay_placement(int64_t p_route, int64_t p_ticks) {
    placement_delays[p_route] = p_ticks;
}

bool Pipeline::hold_placement(const Built &p_built) {
    const int64_t route = p_built.frame.route;
    const int64_t *ticks = placement_delays.getptr(route);
    if (ticks == nullptr) {
        return false;
    }
    HeldPlacement held;
    held.built = p_built;
    held.release_tick = park_now() + *ticks;
    held_placements.push_back(held);
    placement_delays.erase(route);
    return true;
}

bool Pipeline::take_held_placement(Ops &r_ops) {
    const int64_t now = park_now();
    for (uint32_t at = 0; at < held_placements.size(); ++at) {
        if (held_placements[at].release_tick > now) {
            continue;
        }
        r_ops.build() = held_placements[at].built;
        held_placements.remove_at(at);
        NodeOp op;
        op.kind = NodeOp::PLACE;
        op.route = r_ops.build().frame.route;
        r_ops.rows.push_back(op);
        return true;
    }
    return false;
}
#endif

TypedArray<Dictionary> Pipeline::collect_spawn_state(Node *p_root) {
    NETW_ZONE_NC("Spawn collect state", colors::LIVENESS);
#ifdef NETW_TESTS
    if (spawn_state_is_armed) {
        spawn_state_asked += 1;
        return armed_spawn_state;
    }
#endif
    TypedArray<Dictionary> out;
    if (p_root == nullptr) {
        return out;
    }
    const Ref<NetwEntity> root = NetwEntity::of(p_root);
    const bool client_steers = root.is_valid()
        && (root->get_controller() != SERVER_PEER
            || (session != nullptr
                && session->player_kept_controlled(
                    root->get_rid_handle().get_id()
                )));
    LocalVector<Node *> stack;
    stack.push_back(p_root);
    while (!stack.is_empty()) {
        Node *node = stack[stack.size() - 1];
        stack.remove_at(stack.size() - 1);
        const Dictionary configs
            = netw::script::model::get_node_property_configs(node);
        const Array names = configs.keys();
        for (int at = 0; at < names.size(); ++at) {
            const StringName prop = names[at];
            const Ref<NetwPropertyConfig> cfg = configs[prop];
            if (cfg.is_null() || !gd::has_property(node, prop)) {
                continue;
            }
            const bool rides = cfg->get_is_spawn_state()
                || (client_steers && cfg->get_in_broadcast_set());
            if (!rides) {
                continue;
            }
            Dictionary row;
            row[StringName("node")] = node;
            row[StringName("prop")] = prop;
            row[StringName("cfg")] = cfg;
            out.push_back(row);
        }
        const int children = node->get_child_count();
        for (int at = children - 1; at >= 0; --at) {
            Node *child = node->get_child(at);
            if (!roots_published_entity(child)) {
                stack.push_back(child);
            }
        }
    }
    return out;
}

PackedByteArray Pipeline::encode_spawn_frame(int64_t p_route, Node *p_node) {
    return encode_record_frame(spawn_book.spawned_of(p_route), p_route, p_node);
}

PackedByteArray Pipeline::encode_record_frame(
    Record *p_record,
    int64_t p_route,
    Node *p_node
) {
    NETW_ZONE_NC("Spawn encode frame", colors::LIVENESS);
    if (session == nullptr || p_record == nullptr || p_node == nullptr) {
        return PackedByteArray();
    }
    wire::WriteStream stream;
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    const NetwMultiplayer::AnchorRevision installed
        = session->anchor_installed(p_route);
    if (!session->verb_head_write(stream, p_route)
        || !p_record->encode_header(
            stream,
            entity.ptr(),
            installed.revision,
            installed.author
        )) {
        NETW_BUG_V(PackedByteArray());
    }

    Node *outer = NetwMultiplayer::scene_outer_of(p_node);
    Node *parent = outer->get_parent();
    const int recipe = p_record->get_recipe();
    MultiplayerSpawner *spawner
        = recipe == Book::RECIPE_SPAWNER ? p_record->spawner() : nullptr;
    bool parent_is_spawn_target = spawner != nullptr && parent != nullptr
        && spawner->get_node_or_null(spawner->get_spawn_path()) == parent;

    int64_t recipe_code = int64_t(recipe);
    if (!stream.int_range(recipe_code, 1, 4)
        || !stream.bool1(parent_is_spawn_target)) {
        NETW_BUG_V(PackedByteArray());
    }
    if (!parent_is_spawn_target && !session->anchor_encode(stream, parent)) {
        fault::raise(
            session,
            NETW_FAULT_SPAWN_REFUSED,
            {String("'") + String(p_node->get_name()) + String("'"),
             String("a parent outside the multiplayer root"),
             String("a parent inside the multiplayer root")},
            uint64_t(gd::instance_id(p_node))
        );
        return PackedByteArray();
    }

    if (recipe == Book::RECIPE_ADOPT) {
        Node *origin = p_record->origin();
        wire::WriteStream addressable;
        bool moved = origin != nullptr && origin != parent
            && session->anchor_encode(addressable, origin);
        if (!stream.bool1(moved)
            || (moved && !session->anchor_encode(stream, origin))) {
            return PackedByteArray();
        }
    } else if (recipe == Book::RECIPE_SPAWNER) {
        if (spawner == nullptr || !session->anchor_encode(stream, spawner)) {
            fault::raise(
                session,
                NETW_FAULT_SPAWN_REFUSED,
                {String("route ") + String::num_int64(p_record->get_route()),
                 String("a MultiplayerSpawner that is gone or outside the "
                        "multiplayer root"),
                 String("a MultiplayerSpawner inside the multiplayer root")},
                uint64_t(gd::instance_id(p_node))
            );
            return PackedByteArray();
        }
        uint64_t scene_index = uint64_t(p_record->get_scene_index() + 1);
        if (!stream.varuint(scene_index, 3)) {
            return PackedByteArray();
        }
        if (p_record->get_scene_index() < 0) {
            PackedByteArray bytes = call_args::value_bytes(
                p_record->get_custom_data(),
                String("The spawn's custom data"),
                session
            );
            if (!stream.bytes_capped(bytes, BLOB_CAP)) {
                return PackedByteArray();
            }
        }
    } else if (recipe == Book::RECIPE_FN_REGISTRY) {
        const StringName id = p_record->get_fn_registry_id();
        if (!session->spawn_constructor_known(id)) {
            fault::raise(
                session,
                NETW_FAULT_SPAWN_REFUSED,
                {String("route ") + String::num_int64(p_record->get_route()),
                 String("an unregistered spawn constructor"),
                 String("a spawn constructor registered with "
                        "{NetwMultiplayer.spawn_register_constructor}")},
                uint64_t(gd::instance_id(p_node))
            );
            return PackedByteArray();
        }
        const Variant schema = session->spawn_registry_schema(id);
        if (schema.get_type() == Variant::NIL) {
            fault::raise(
                session,
                NETW_FAULT_SPAWN_REFUSED,
                {String("route ") + String::num_int64(p_record->get_route()),
                 String("a spawn constructor with no argument types"),
                 String("{NetwMultiplayer.spawn_register_constructor} with "
                        "arg_types")},
                uint64_t(gd::instance_id(p_node))
            );
            return PackedByteArray();
        }
        String name = String(id);
        PackedByteArray args = encode_fn_args(schema, p_record->get_fn_args());
        if (!wire::string_field(stream, name)
            || !stream.bytes_capped(args, BLOB_CAP)) {
            return PackedByteArray();
        }
    } else if (recipe == Book::RECIPE_FN) {
        Node *host = p_record->fn_host();
        if (host == nullptr || !session->anchor_encode(stream, host)) {
            fault::raise(
                session,
                NETW_FAULT_SPAWN_REFUSED,
                {String("route ") + String::num_int64(p_record->get_route()),
                 String("a node holding the spawn function that is gone or "
                        "outside the multiplayer root"),
                 String("a node inside the multiplayer root holding the "
                        "spawn function")},
                uint64_t(gd::instance_id(p_node))
            );
            return PackedByteArray();
        }
        const Variant schema = NetwMultiplayer::spawn_script_schema(
            host->get_script(),
            p_record->get_fn_method()
        );
        if (schema.get_type() == Variant::NIL) {
            fault::raise(
                session,
                NETW_FAULT_SPAWN_REFUSED,
                {String("route ") + String::num_int64(p_record->get_route()),
                 String("the unregistered spawn function '")
                     + String(p_record->get_fn_method()) + String("'"),
                 String("{Netw.configure_spawn} on it before calling "
                        "{Netw.spawn}")},
                uint64_t(gd::instance_id(p_node))
            );
            return PackedByteArray();
        }
        String name = String(p_record->get_fn_method());
        bool waits = p_record->get_fn_waits();
        PackedByteArray args = encode_fn_args(schema, p_record->get_fn_args());
        if (!wire::string_field(stream, name) || !stream.bool1(waits)
            || !stream.bytes_capped(args, BLOB_CAP)) {
            return PackedByteArray();
        }
    }

    const TypedArray<Dictionary> entries = collect_spawn_state(p_node);
    uint64_t state_count = uint64_t(entries.size());
    if (!stream.varuint(state_count, 2)) {
        return PackedByteArray();
    }
    for (int at = 0; at < entries.size(); ++at) {
        const Dictionary entry = entries[at];
        Node *source
            = Object::cast_to<Node>(gd::live_object(entry[StringName("node")]));
        const StringName prop = entry[StringName("prop")];
        const Ref<NetwPropertyConfig> cfg = entry[StringName("cfg")];
        bool by_path = source != p_node;
        String path = by_path ? String(p_node->get_path_to(source)) : String();
        if (!stream.bool1(by_path)
            || (by_path && !wire::string_field(stream, path))) {
            return PackedByteArray();
        }
        const Variant token = session->sync_pipeline_encode_prop_val(
            entity->get_rid_handle(),
            source,
            prop
        );
        PackedByteArray token_bytes = netw::script::model::token_bytes(token);

        Array quantizers;
        const Array declared = cfg.is_valid() ? cfg->get_quantizers() : Array();
        quantizers.push_back(declared.is_empty() ? Variant() : declared[0]);
        Array types;
        types.push_back(
            netw::script::model::get_node_property_type(source, prop)
        );
        Array values;
        values.push_back(port_get(source, prop));
        wire::WriteStream value_stream;
        if (!call_args::values_write(
                value_stream,
                values,
                quantizers,
                types,
                session
            )
            || !value_stream.align_verify()) {
            return PackedByteArray();
        }
        PackedByteArray value_bytes = value_stream.to_bytes();

        if (!stream.bytes_capped(token_bytes, wire::STRING_CAP)
            || !stream.bytes_capped(value_bytes, BLOB_CAP)) {
            return PackedByteArray();
        }
    }

    const int64_t local_id = has_peer() ? session->get_unique_id() : 1;
    const Array native_entries = synchronizers::spawn_state(p_node, local_id);
    uint64_t native_count = uint64_t(native_entries.size());
    if (!stream.varuint(native_count, 2)) {
        return PackedByteArray();
    }
    for (int at = 0; at < native_entries.size(); ++at) {
        const Dictionary entry = native_entries[at];
        String path = String(entry[StringName("path")]);
        PackedByteArray bytes = call_args::value_bytes(
            entry[StringName("value")],
            vformat("Spawn property '%s'", path),
            session
        );
        if (!wire::string_field(stream, path)
            || !stream.bytes_capped(bytes, BLOB_CAP)) {
            return PackedByteArray();
        }
    }

    const PackedByteArray no_descriptors
        = Record::write_descriptors(LocalVector<DescriptorRow>());
    PackedByteArray consumed_bytes = sync_compat != nullptr
        ? sync_compat->encode_descriptors(p_record->get_route())
        : no_descriptors;
    PackedByteArray derived_bytes
        = session->sync_pipeline_encode_derived_descriptors(
            p_record->get_route()
        );
    if (!stream.bytes_capped(consumed_bytes, BLOB_CAP)
        || !stream.bytes_capped(derived_bytes, BLOB_CAP)
        || !stream.align_verify()) {
        return PackedByteArray();
    }
    return stream.to_bytes();
}

bool decode_spawn_frame(
    const NetwMultiplayer *p_session,
    const PackedByteArray &p_payload,
    SpawnFrame &r_frame
) {
    NETW_ZONE_NC("Spawn decode frame", colors::LIVENESS);
    if (p_session == nullptr) {
        return false;
    }
    wire::ReadStream stream(p_payload);
    SpawnFrame frame;
    if (!NetwMultiplayer::verb_head_read(stream, frame.route, frame.epoch)
        || !Record::decode_header(stream, frame.header)
        || !stream.int_range(frame.recipe, 1, 4)
        || !stream.bool1(frame.parent_is_spawn_target)) {
        return false;
    }
    frame.header[StringName("route")] = frame.route;
    frame.header[StringName("epoch")] = frame.epoch;
    if (frame.parent_is_spawn_target && frame.recipe != Book::RECIPE_SPAWNER) {
        return false;
    }
    if (!frame.parent_is_spawn_target
        && !p_session->anchor_decode(stream, frame.parent_anchor)) {
        return false;
    }

    if (frame.recipe == Book::RECIPE_ADOPT) {
        if (!stream.bool1(frame.moved_from_origin)
            || (frame.moved_from_origin
                && !p_session->anchor_decode(stream, frame.origin_anchor))) {
            return false;
        }
    } else if (frame.recipe == Book::RECIPE_SPAWNER) {
        uint64_t staged_index = 0;
        if (!p_session->anchor_decode(stream, frame.spawner_anchor)
            || !stream.varuint(staged_index, 3)) {
            return false;
        }
        frame.scene_index = int64_t(staged_index) - 1;
        if (frame.scene_index < 0
            && !stream.bytes_capped(frame.custom, BLOB_CAP)) {
            return false;
        }
    } else if (frame.recipe == Book::RECIPE_FN_REGISTRY) {
        if (!wire::string_field(stream, frame.fn_id)
            || !stream.bytes_capped(frame.args, BLOB_CAP)) {
            return false;
        }
    } else if (frame.recipe == Book::RECIPE_FN) {
        if (!p_session->anchor_decode(stream, frame.host_anchor)
            || !wire::string_field(stream, frame.method)
            || !stream.bool1(frame.fn_waits)
            || !stream.bytes_capped(frame.args, BLOB_CAP)) {
            return false;
        }
    }

    uint64_t state_count = 0;
    if (!stream.varuint(state_count, 2)) {
        return false;
    }
    for (uint64_t at = 0; at < state_count; ++at) {
        SpawnFrame::StateEntry entry;
        if (!stream.bool1(entry.by_path)) {
            return false;
        }
        if (entry.by_path && !wire::string_field(stream, entry.path)) {
            return false;
        }
        if (!stream.bytes_capped(entry.token, wire::STRING_CAP)
            || !stream.bytes_capped(entry.values, BLOB_CAP)) {
            return false;
        }
        frame.state.push_back(entry);
    }

    uint64_t native_count = 0;
    if (!stream.varuint(native_count, 2)) {
        return false;
    }
    for (uint64_t at = 0; at < native_count; ++at) {
        SpawnFrame::NativeEntry entry;
        if (!wire::string_field(stream, entry.path)
            || !stream.bytes_capped(entry.value, BLOB_CAP)) {
            return false;
        }
        frame.native.push_back(entry);
    }

    if (!stream.bytes_capped(frame.consumed, BLOB_CAP)
        || !stream.bytes_capped(frame.derived, BLOB_CAP)
        || !stream.align_verify() || stream.bits_remaining() != 0) {
        return false;
    }
    r_frame = frame;
    return true;
}

bool Pipeline::frame_read(
    bool p_healthy,
    int64_t p_route,
    int64_t p_sender
) {
    if (p_healthy) {
        return true;
    }
    drops_spawn_truncated += 1;
    if (session != nullptr) {
        session->count_verdict(ERR_INVALID_DATA, p_route);
        fault::raise(
            session,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_sender}
        );
    }
    return false;
}

bool Pipeline::admits_frame(
    int64_t p_sender,
    int64_t p_channel,
    const PackedByteArray &p_payload
) {
    return admission(p_sender, p_channel, p_payload) == OK;
}

Error Pipeline::admission(
    int64_t p_sender,
    int64_t p_channel,
    const PackedByteArray &p_payload
) {
    NETW_STAGE(SPAWN, "Spawn admit frame", colors::LIVENESS);
    NETW_ZONE_VALUE(p_payload.size());
    const int64_t route = route_named_by(p_payload);
    Error verdict = ERR_UNAVAILABLE;
    if (session != nullptr) {
        verdict
            = session->spawn_admit_frame(p_sender, route, p_channel, p_payload);
    }
    const Error packet_verdict
        = verdict == ERR_INVALID_PARAMETER ? ERR_INVALID_DATA : verdict;
    const Error staged
        = stage_verdict(EventPlane::GATE_SPAWN, packet_verdict, 0);
    if (staged != OK) {
        if (session != nullptr && p_sender != SERVER_PEER) {
            drops_spawn_bad_sender += 1;
        }
        return verdict != OK ? verdict : staged;
    }
    return OK;
}

void Pipeline::receive_spawn(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    if (!admits_frame(p_sender, channel_spawn, p_payload)) {
        return;
    }
    if (session != nullptr && session->is_host() && p_sender != SERVER_PEER) {
        admit_controller_spawn(p_payload, p_sender, r_ops);
        return;
    }
    decide_spawn(p_payload, p_sender, r_ops);
}

void Pipeline::admit_controller_spawn(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    const int64_t route = route_named_by(p_payload);
    if (session->liveness_spend_lease(p_sender, route) != OK) {
        refuse_controller_spawn(route, p_sender, ERR_UNAUTHORIZED, false);
        return;
    }
    SpawnFrame named;
    if (!decode_spawn_frame(session, p_payload, named)) {
        frame_read(false, route, p_sender);
        refuse_controller_spawn(route, p_sender, ERR_INVALID_DATA, true);
        return;
    }
    if (named.recipe == Book::RECIPE_ADOPT
        || named.recipe == Book::RECIPE_SPAWNER) {
        refuse_controller_spawn(route, p_sender, ERR_UNAVAILABLE, true);
        return;
    }
    Built &built = r_ops.build();
    built.strict = true;
    if (decide_build(p_payload, p_sender, built) != Built::BUILT) {
        refuse_controller_spawn(route, p_sender, ERR_INVALID_PARAMETER, true);
        return;
    }
    NodeOp op;
    op.kind = NodeOp::BUILD;
    op.route = route;
    op.sender = p_sender;
    op.strict = true;
    op.requested = true;
    r_ops.rows.push_back(op);
}

void Pipeline::refuse_controller_spawn(
    int64_t p_route,
    int64_t p_sender,
    Error p_code,
    bool p_holds_route,
    const String &p_reason
) {
    lifecycle_ops_refused += 1;
    if (p_route <= 0) {
        return;
    }
    session->lifecycle_decide(
        p_route,
        p_sender,
        lifecycle::Kind::SPAWN,
        0,
        p_code,
        p_reason
    );
    if (!p_holds_route) {
        return;
    }
    wire::WriteStream stream;
    String unspoken;
    if (!session->verb_head_write(stream, p_route)
        || !wire::string_field(stream, unspoken) || !stream.align_verify()) {
        return;
    }
    session->lifecycle_send_to(
        p_sender,
        p_route,
        channel_despawn,
        stream.to_bytes()
    );
}

void Pipeline::apply_parked(
    int64_t p_route,
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    if (p_payload.is_empty()) {
        return;
    }
    if (!admits_frame(p_sender, channel_spawn, p_payload)) {
        release_build(p_route, r_ops);
        spawn_park_refused += 1;
        fault::raise(
            session,
            NETW_FAULT_MALFORMED_DATAGRAM,
            {String(), String(), String(), p_sender}
        );
        return;
    }
    decide_spawn(p_payload, p_sender, r_ops);
}

void Pipeline::decide_spawn(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    r_ops.payload = p_payload;
    const Built::Outcome decided
        = decide_build(p_payload, p_sender, r_ops.build());
    if (decided != Built::BUILT) {
        settle_build(decided, p_sender, r_ops);
        return;
    }
    NodeOp op;
    op.kind = NodeOp::BUILD;
    op.route = r_ops.build().frame.route;
    op.sender = p_sender;
    op.strict = r_ops.build().strict;
    r_ops.rows.push_back(op);
}

void Pipeline::built(
    const NodeOp &p_op,
    Built::Outcome p_outcome,
    Ops &r_ops
) {
    settle_build(p_outcome, p_op.sender, r_ops);
}

void Pipeline::settle_build(
    Built::Outcome p_outcome,
    int64_t p_sender,
    Ops &r_ops
) {
    Built &built = r_ops.build();
    switch (p_outcome) {
        case Built::DROPPED:
            if (!built.scene.is_empty() || built.from_build) {
                release_build(built.frame.route, r_ops);
            }
            return;
        case Built::WAITS:
            park_built(r_ops.payload, p_sender, built);
            return;
        case Built::BUILT:
            break;
    }
#ifdef NETW_TESTS
    if (hold_placement(built)) {
        return;
    }
#endif
    NodeOp op;
    op.kind = NodeOp::PLACE;
    op.route = built.frame.route;
    op.sender = p_sender;
    r_ops.rows.push_back(op);
}

void Pipeline::enroll_received(const Built &p_built) {
    spawn_book.enroll_recv(p_built.frame.route, received_of(p_built));
}

void Pipeline::park_built(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    const Built &p_built
) {
    const int64_t route = p_built.frame.route;
    switch (p_built.wait) {
        case Park::WAIT_ROUTE:
            park_spawn(p_payload, p_built.wait_route, route, p_sender);
            break;
        case Park::WAIT_ADOPT:
            park_spawn_for_adopt(p_payload, route, p_sender);
            break;
        case Park::WAIT_SPAWNER:
            park_spawn_for_scene(p_payload, route, p_sender);
            break;
        case Park::WAIT_BUILD:
            if (park.park(
                    route,
                    p_payload,
                    Park::WAIT_BUILD,
                    INT64_MAX,
                    p_sender
                )) {
                spawn_deferrals += 1;
            }
            break;
        case Park::WAIT_NONE:
            break;
    }
}

Built::Outcome Pipeline::decide_build(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Built &r_built
) {
    if (session == nullptr) {
        return Built::DROPPED;
    }
    SpawnFrame &frame = r_built.frame;
    if (!decode_spawn_frame(session, p_payload, frame)) {
        frame_read(false, 0, p_sender);
        return Built::DROPPED;
    }

    const int64_t route = frame.route;
    const int64_t recipe = frame.recipe;
    const Anchor &parent_anchor = frame.parent_anchor;
    const Build *held = builds.getptr(route);
    r_built.from_build = held != nullptr && held->scene.is_empty();

    if (!session->liveness_epoch_admits(route, frame.epoch)) {
        drops_spawn_stale_life += 1;
        return Built::DROPPED;
    }
    if (spawn_book
            .spawn_is_duplicate(route, session->liveness_route_state(route))) {
        drops_spawn_duplicate += 1;
        return Built::DROPPED;
    }

    if (!frame.parent_is_spawn_target) {
        const int64_t parent_route = parent_anchor.route;
        if (parent_route > 0
            && Park::anchor_parks(
                session->liveness_route_state(parent_route)
            )) {
            r_built.wait = Park::WAIT_ROUTE;
            r_built.wait_route = parent_route;
            return Built::WAITS;
        }
    }

    int64_t anchor_route = 0;
    if (recipe == Book::RECIPE_ADOPT) {
        anchor_route = frame.moved_from_origin ? frame.origin_anchor.route
                                               : parent_anchor.route;
    } else if (recipe == Book::RECIPE_SPAWNER) {
        anchor_route = frame.spawner_anchor.route;
    } else if (recipe == Book::RECIPE_FN) {
        anchor_route = frame.host_anchor.route;
    }
    if (anchor_route > 0
        && Park::anchor_parks(session->liveness_route_state(anchor_route))) {
        r_built.wait = Park::WAIT_ROUTE;
        r_built.wait_route = anchor_route;
        return Built::WAITS;
    }
    return Built::BUILT;
}

void Pipeline::kept_reparent(int64_t p_route, Ops &r_ops) {
    int64_t sender = 0;
    const PackedByteArray kept = park.take_reparent(p_route, sender);
    if (!kept.is_empty()) {
        handle_reparent_frame(kept, sender, r_ops);
    }
}

void Pipeline::handle_despawn_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    if (admission(p_sender, channel_despawn, p_payload) != OK) {
        return;
    }
    if (session == nullptr) {
        return;
    }
    wire::ReadStream reader(p_payload);
    int64_t route = 0;
    int64_t epoch = 0;
    String reason;
    if (!NetwMultiplayer::verb_head_read(reader, route, epoch)
        || !wire::string_field(reader, reason) || !reader.align_verify()
        || reader.bits_remaining() != 0) {
        frame_read(false, route, p_sender);
        return;
    }
    if (!session->liveness_epoch_admits(route, epoch)) {
        drops_spawn_stale_life += 1;
        return;
    }
    session->liveness_adopt_epoch(route, epoch);
    if (cancel_parked(route, r_ops)) {
        spawn_parked_cancelled += 1;
        return;
    }
    if (!spawn_book.is_recv(route)) {
        drops_despawn_unknown += 1;
        return;
    }
    spawn_book.drop_recv(route);
    NodeOp op;
    op.kind = NodeOp::FREE;
    op.lingers = true;
    op.route = route;
    op.sender = p_sender;
    op.reason = StringName(reason);
    r_ops.rows.push_back(op);
}

void Pipeline::release_received(int64_t p_route, Ops &r_ops) {
    if (session == nullptr) {
        return;
    }
    spawn_book.drop_recv(p_route);
    NodeOp op;
    op.kind = NodeOp::FREE;
    op.route = p_route;
    op.reason = StringName("despawn");
    r_ops.rows.push_back(op);
}

void Pipeline::handle_hide_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    if (!admits_frame(p_sender, channel_hide, p_payload)) {
        return;
    }
    if (session == nullptr) {
        return;
    }
    wire::ReadStream reader(p_payload);
    int64_t route = 0;
    int64_t epoch = 0;
    if (!NetwMultiplayer::verb_head_read(reader, route, epoch)
        || !reader.align_verify() || reader.bits_remaining() != 0) {
        frame_read(false, route, p_sender);
        return;
    }
    if (!session->liveness_epoch_admits(route, epoch)) {
        drops_spawn_stale_life += 1;
        return;
    }
    session->liveness_adopt_epoch(route, epoch);
    if (cancel_parked(route, r_ops)) {
        spawn_parked_cancelled += 1;
        return;
    }
    Record received;
    if (!spawn_book.take_recv(route, received)) {
        drops_hide_unknown += 1;
        return;
    }
    NodeOp op;
    op.kind = NodeOp::HIDE;
    op.keeps_node = received.get_recipe() == Book::RECIPE_ADOPT;
    op.route = route;
    op.sender = p_sender;
    r_ops.rows.push_back(op);
}

void Pipeline::missed(const NodeOp &p_op) {
    switch (p_op.kind) {
        case NodeOp::MOVE:
            drops_spawn_unresolved += 1;
            break;
        case NodeOp::FREE:
            if (p_op.sender != 0) {
                drops_despawn_unknown += 1;
            }
            break;
        case NodeOp::HIDE:
            drops_hide_unknown += 1;
            break;
        case NodeOp::DISCARD:
        case NodeOp::ORPHAN:
            break;
    }
}

void Pipeline::handle_reparent_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender,
    Ops &r_ops
) {
    if (admission(p_sender, channel_reparent, p_payload) != OK) {
        return;
    }
    if (session == nullptr) {
        return;
    }
    wire::ReadStream reader(p_payload);
    int64_t route = 0;
    int64_t epoch = 0;
    Anchor anchor;
    uint64_t revision = 0;
    uint64_t author = 0;
    if (!NetwMultiplayer::verb_head_read(reader, route, epoch)
        || !session->anchor_decode(reader, anchor)
        || !reader.varuint(revision, 5) || !reader.varuint(author, 5)
        || !reader.align_verify()
        || reader.bits_remaining() != 0) {
        frame_read(false, route, p_sender);
        return;
    }
    if (!session->liveness_epoch_admits(route, epoch)) {
        drops_spawn_stale_life += 1;
        return;
    }
    session->liveness_adopt_epoch(route, epoch);

    const Ref<NetwEntity> entity = session->wrapper_for_route(route);
    if (entity.is_null() || entity->get_owner() == nullptr) {
        if (!park.has(route)) {
            drops_spawn_unresolved += 1;
        } else if (!park.keep_reparent(route, p_payload, revision, p_sender)) {
            drops_reparent_stale += 1;
        }
        return;
    }
    if (!reparent_is_newer(session, route, revision)) {
        drops_reparent_stale += 1;
        return;
    }
    const int64_t anchor_route = anchor.route;
    if (anchor_route > 0
        && Park::anchor_parks(session->liveness_route_state(anchor_route))) {
        spawn_deferrals += 1;
        session->spawn_wait_reparent(
            anchor_route,
            p_payload,
            p_sender,
            park_timeout_ticks()
        );
        return;
    }
    NodeOp op;
    op.kind = NodeOp::MOVE;
    op.route = route;
    op.sender = p_sender;
    op.parent = anchor;
    op.revision = revision;
    op.author = author;
    r_ops.rows.push_back(op);
}

Error Pipeline::admit_controller_frame(
    lifecycle::Kind p_kind,
    int64_t p_sender,
    const PackedByteArray &p_payload
) {
    const int64_t channel = p_kind == lifecycle::Kind::DESPAWN
        ? channel_despawn
        : channel_reparent;
    return admission(p_sender, channel, p_payload);
}

void Pipeline::count_lifecycle_op(bool p_admitted) {
    if (p_admitted) {
        lifecycle_ops_admitted += 1;
    } else {
        lifecycle_ops_refused += 1;
    }
}

int64_t Pipeline::park_timeout_ticks() const {
    double tickrate = CLOCKLESS_TICKRATE;
    if (session != nullptr) {
        const Clock &clock = *clock_engine;
        if (clock.get_configured()) {
            tickrate = double(clock.get_tickrate());
        }
    }
    return Clock::pumps_for(park_timeout_seconds, tickrate);
}

int64_t Pipeline::park_now() const {
    if (session == nullptr) {
        return 0;
    }
    const Clock &clock = *clock_engine;
    return clock.get_configured() ? clock.get_ticks_run()
                                  : session->receive_tick();
}

void Pipeline::warn_slow_build(
    const StringName &p_method,
    int64_t p_started_at,
    uint64_t p_host
) const {
    if (park_now() - p_started_at < park_timeout_ticks()) {
        return;
    }
    fault::raise(
        session,
        NETW_FAULT_SPAWN_STILL_WAITING,
        {String(p_method), String(), String::num(park_timeout_seconds)},
        p_host
    );
}
void Pipeline::park_spawn(
    const PackedByteArray &p_payload,
    int64_t p_dep_route,
    int64_t p_route,
    int64_t p_sender
) {
    if (session == nullptr) {
        return;
    }
    spawn_deferrals += 1;
    const int64_t timeout = park_timeout_ticks();
    park.park(
        p_route,
        p_payload,
        Park::WAIT_ROUTE,
        park_now() + timeout,
        p_sender
    );
    session->spawn_wait_route(p_dep_route, p_route, timeout);
}

void Pipeline::retry_parked(int64_t p_route, Ops &r_ops) {
    const int64_t sender = park.sender_of(p_route);
    apply_parked(p_route, park.take(p_route), sender, r_ops);
}

void Pipeline::expire_parked(int64_t p_route, Ops &r_ops) {
    if (cancel_parked(p_route, r_ops)) {
        spawn_park_expired += 1;
    }
}

bool Pipeline::cancel_parked(int64_t p_route, Ops &r_ops) {
    if (!park.cancel(p_route)) {
        return false;
    }
    release_build(p_route, r_ops);
    return true;
}

void Pipeline::park_spawn_for_scene(
    const PackedByteArray &p_payload,
    int64_t p_route,
    int64_t p_sender
) {
    if (session == nullptr) {
        drops_spawn_unresolved += 1;
        return;
    }
    spawn_deferrals += 1;
    park.park(
        p_route,
        p_payload,
        Park::WAIT_SPAWNER,
        park_now() + park_timeout_ticks(),
        p_sender
    );
    retrying_scene_parks = true;
}

void Pipeline::expire_spawner_route(
    int64_t p_route,
    int64_t p_now,
    Ops &r_ops
) {
    if (!park.is_expired(p_route, p_now) || !cancel_parked(p_route, r_ops)) {
        return;
    }
    spawn_park_expired += 1;
    fault::raise(
        session,
        NETW_FAULT_SPAWN_MISMATCH,
        {String(), String(), String("its MultiplayerSpawner"), 0, p_route}
    );
}

void Pipeline::retry_spawner_route(int64_t p_route, Ops &r_ops) {
    if (!park.has(p_route)) {
        return;
    }
    retry_parked(p_route, r_ops);
}

void Pipeline::settle_spawner_retry() {
    if (park.waiting_on(Park::WAIT_SPAWNER).is_empty()) {
        drop_scene_park_retry();
    }
}

void Pipeline::park_spawn_for_adopt(
    const PackedByteArray &p_payload,
    int64_t p_route,
    int64_t p_sender
) {
    if (park.park(
            p_route,
            p_payload,
            Park::WAIT_ADOPT,
            park_now() + park_timeout_ticks(),
            p_sender
        )) {
        spawn_deferrals += 1;
    }
}

String Pipeline::adopt_place_of(const PackedByteArray &p_payload) {
    SpawnFrame frame;
    if (session == nullptr || !decode_spawn_frame(session, p_payload, frame)) {
        return String("A node the server named");
    }
    const Anchor &anchor
        = frame.moved_from_origin ? frame.origin_anchor : frame.parent_anchor;
    const String name = frame.header.get(StringName("node_name"), String());
    String under = anchor.path;
    if (anchor.entity_relative) {
        const Ref<NetwEntity> entity = session->wrapper_for_route(anchor.route);
        Node *holder = entity.is_valid() ? entity->get_owner() : nullptr;
        Node *root = session->session_get_root();
        if (holder != nullptr && root != nullptr
            && root->is_ancestor_of(holder)) {
            under = String(root->get_path_to(holder)).path_join(under);
        }
    }
    const String path = under.is_empty() || under == String(".")
        ? name
        : under.path_join(name);
    return vformat("'%s' at %s", name, path);
}

bool Pipeline::retry_adopt(int64_t p_route, int64_t p_now, Ops &r_ops) {
    if (!park.has(p_route)) {
        return false;
    }
    if (park.is_expired(p_route, p_now)) {
        const String missing = adopt_place_of(park.peek(p_route));
        cancel_parked(p_route, r_ops);
        spawn_park_expired += 1;
        drops_spawn_unresolved += 1;
        fault::raise(
            session,
            NETW_FAULT_SPAWN_MISMATCH,
            {String(), String(), String(missing), 0, p_route}
        );
        return false;
    }
    apply_parked(p_route, park.peek(p_route), park.sender_of(p_route), r_ops);
    return true;
}

void Pipeline::settle_adopt_retry(int64_t p_route) {
    if (spawn_book.is_recv(p_route)) {
        park.take(p_route);
    }
}

void Pipeline::retry_build(int64_t p_route, Ops &r_ops) {
    if (!park.has(p_route)) {
        return;
    }
    Build *held = builds.getptr(p_route);
    if (held != nullptr && held->scene.is_empty()) {
        if (held->ready.is_valid()) {
            retry_parked(p_route, r_ops);
        } else if (gd::instance_from_id(held->host) == nullptr) {
            const StringName method = held->method;
            cancel_parked(p_route, r_ops);
            drops_spawn_unresolved += 1;
            fault::raise(
                session,
                NETW_FAULT_SPAWN_RETURN,
                {String("spawn function '") + String(method) + String("'"),
                 String("a freed holder node"),
                 String("a node holding the spawn function that stays "
                        "until it returns")}
            );
        } else {
            warn_slow_build(
                held->method,
                held->started_at,
                uint64_t(held->host)
            );
        }
        return;
    }
    const String path = held != nullptr ? held->scene : String();
    const SceneLoads::Status status = SceneLoads::shared().status(path);
    if (status == SceneLoads::STATUS_LOADING) {
        return;
    }
    if (status == SceneLoads::STATUS_READY) {
        retry_parked(p_route, r_ops);
        return;
    }
    cancel_parked(p_route, r_ops);
    drop_missing_scene(p_route, path, r_ops);
}

Built::Outcome Pipeline::wait_for_scene(
    const String &p_path,
    Built &r_built,
    Ops &r_ops
) {
    const int64_t route = r_built.frame.route;
    const String path = gd::ensure_path(p_path);
    if (path.is_empty() || !gd::resource_available(path)) {
        return drop_missing_scene(route, p_path, r_ops);
    }
    r_built.scene = path;
    SceneLoads &loads = SceneLoads::shared();
    const SceneLoads::Status status = loads.begin(path);
    if (!builds.has(route)) {
        loads.hold(path);
        Build held;
        held.scene = path;
        builds.insert(route, held);
    }
    if (status == SceneLoads::STATUS_READY) {
        return Built::BUILT;
    }
    if (status == SceneLoads::STATUS_LOADING) {
        r_built.wait = Park::WAIT_BUILD;
        return Built::WAITS;
    }
    return drop_missing_scene(route, path, r_ops);
}

int64_t Pipeline::build_waits(
    int64_t p_route,
    const Variant &p_waiting,
    Node *p_host,
    const StringName &p_method,
    Ops &r_ops
) {
    release_build(p_route, r_ops);
    build_serial += 1;
    const int64_t serial = build_serial;
    Build held;
    held.pending = p_waiting;
    held.host = gd::instance_id(p_host);
    held.method = p_method;
    held.serial = serial;
    held.started_at = park_now();
    builds.insert(p_route, held);
    return serial;
}

Built::Outcome Pipeline::build_settled(
    int64_t p_serial,
    Built &r_built
) const {
    const Build *settled = builds.getptr(r_built.frame.route);
    if (settled == nullptr || settled->serial != p_serial) {
        return Built::DROPPED;
    }
    if (settled->ready.is_valid()) {
        return Built::BUILT;
    }
    r_built.wait = Park::WAIT_BUILD;
    return Built::WAITS;
}

Node *Pipeline::take_ready(int64_t p_route) {
    const HashMap<int64_t, Build>::Iterator held = builds.find(p_route);
    if (!held || !held->value.scene.is_empty()) {
        return nullptr;
    }
    Node *ready
        = Object::cast_to<Node>(gd::instance_from_id(held->value.ready));
    builds.remove(held);
    return ready;
}

bool Pipeline::build_returned(
    const Variant &p_value,
    int64_t p_route,
    int64_t p_serial,
    Ops &r_ops
) {
    Build *held = builds.getptr(p_route);
    if (held == nullptr || !held->scene.is_empty()
        || held->serial != p_serial) {
        return false;
    }
    held->pending = Variant();
    Node *node = Object::cast_to<Node>(gd::live_object(p_value));
    if (node != nullptr && !node->is_inside_tree()) {
        held->ready = gd::instance_id(node);
        return true;
    }
    const StringName method = held->method;
    drop_build(p_route, r_ops);
    fault::raise(
        session,
        NETW_FAULT_SPAWN_RETURN,
        {String("spawn function '") + String(method) + String("'"),
         String("no returned Node"),
         String("a function that returns a Node")}
    );
    if (session != nullptr) {
        session->spawn_report_failed(p_route, String("returned no Node"));
    }
    return true;
}

void Pipeline::build_failed(
    const String &p_detail,
    int64_t p_route,
    int64_t p_serial,
    Ops &r_ops
) {
    const Build *held = builds.getptr(p_route);
    if (held == nullptr || !held->scene.is_empty()
        || held->serial != p_serial) {
        return;
    }
    const StringName method = held->method;
    drop_build(p_route, r_ops);
    fault::raise(
        session,
        NETW_FAULT_SPAWN_RETURN,
        {String("spawn function '") + String(method) + String("'"),
         String("the error '") + p_detail + String("'"),
         String("a function that finishes without an error")}
    );
    if (session != nullptr) {
        session->spawn_report_failed(
            p_route,
            vformat("failed with '%s'", p_detail)
        );
    }
}

void Pipeline::drop_build(int64_t p_route, Ops &r_ops) {
    drops_spawn_unresolved += 1;
    if (!cancel_parked(p_route, r_ops)) {
        release_build(p_route, r_ops);
    }
}

Built::Outcome Pipeline::drop_missing_scene(
    int64_t p_route,
    const String &p_path,
    Ops &r_ops
) {
    release_build(p_route, r_ops);
    fault::raise(
        session,
        NETW_FAULT_SPAWN_MISMATCH,
        {String(),
         String(),
         String("scene '") + p_path + String("'"),
         0,
         p_route}
    );
    drops_spawn_unresolved += 1;
    return Built::DROPPED;
}

void Pipeline::release_build(int64_t p_route, Ops &r_ops) {
    const HashMap<int64_t, Build>::Iterator held = builds.find(p_route);
    if (!held) {
        return;
    }
    if (!held->value.scene.is_empty()) {
        SceneLoads::shared().release(held->value.scene);
    }
    Node *ready
        = Object::cast_to<Node>(gd::instance_from_id(held->value.ready));
    builds.remove(held);
    if (ready != nullptr) {
        r_ops.rows.push_back(node_op(NodeOp::ORPHAN, gd::instance_id(ready)));
    }
}

void Pipeline::drop_scene_park_retry() {
    retrying_scene_parks = false;
}

int64_t Pipeline::gate_display_tick() const {
    if (session == nullptr) {
        return -1;
    }
    const Clock &clock = *clock_engine;
    if (clock.get_configured()) {
        return clock.display_tick();
    }
    return -1;
}

void Pipeline::apply_action_gate(
    int64_t p_route,
    const RID &p_entity
) {
    if (session == nullptr) {
        return;
    }
    const int64_t display_tick = gate_display_tick();
    if (display_tick < 0) {
        return;
    }
    if (session->action_gate_arm(p_route, p_entity, display_tick)) {
        revealing_action_gates = true;
    }
}

void Pipeline::on_action_reveal_tick(double, int64_t p_tick) {
    if (session == nullptr) {
        return;
    }
    int64_t reached = p_tick;
    const Clock &clock = *clock_engine;
    if (clock.get_configured()) {
        const int64_t shifted = p_tick - clock.get_display_offset();
        reached = shifted > 0 ? shifted : 0;
    }
    session->action_gate_sweep(reached);
    settle_action_reveal();
}

void Pipeline::settle_action_reveal() {
    if (session == nullptr || session->action_gate_count() > 0) {
        return;
    }
    revealing_action_gates = false;
}

void Pipeline::note_action_gate() {
    applying_action_gate = session != nullptr;
}

void Pipeline::clear_action_gates() {
    if (session == nullptr) {
        return;
    }
    session->action_gate_clear();
    settle_action_reveal();
    applying_action_gate = false;
}

void Pipeline::fail_pending_spawns(const String &p_detail) {
    if (session != nullptr) {
        session->spawn_fail_pending(p_detail);
    }
}

LocalVector<ObjectID> Pipeline::end_session_nodes() {
    fail_pending_spawns(SESSION_ENDED_MID_SPAWN);
    report_unadded(INT64_MAX);
    return spawn_book.session_nodes();
}

void Pipeline::begin_clear_session() {
    fail_pending_spawns(SESSION_ENDED_MID_SPAWN);
}

void Pipeline::finish_clear_session() {
    spawn_book.clear();
    refused_placed.clear();
    placed_before_host.clear();
    unadded.clear();
    clear_action_gates();
}

LocalVector<int64_t> Pipeline::begin_clear_parks() {
    drop_scene_park_retry();
    LocalVector<int64_t> parked;
    for (const KeyValue<int64_t, Build> &held : builds) {
        if (park.has(held.key)) {
            parked.push_back(held.key);
        }
    }
    return parked;
}

void Pipeline::finish_clear_parks() {
    park.clear();
}

LocalVector<int64_t> Pipeline::build_routes() const {
    LocalVector<int64_t> routes;
    for (const KeyValue<int64_t, Build> &held : builds) {
        routes.push_back(held.key);
    }
    return routes;
}

PackedInt64Array Pipeline::promote_received() {
    PackedInt64Array promoted;
    if (session == nullptr) {
        return promoted;
    }
    const PackedInt64Array routes = spawn_book.recv_routes();
    for (int at = 0; at < routes.size(); ++at) {
        const int64_t route = routes[at];
        Record record;
        if (!spawn_book.take_recv(route, record)) {
            continue;
        }
        Node *node = record.node();
        const Ref<NetwEntity> entity = NetwEntity::of(node);
        if (entity.is_null()) {
            continue;
        }
        const Ref<NetwEntity> above = NetwEntity::of(node->get_parent());
        record.set_node_name(String(node->get_name()));
        record.set_parent_route(
            above.is_valid()
                ? liveness_core->route_of(above->get_rid_handle())
                : 0
        );
        record.set_entity_id(entity->get_name());
        record.set_peer_id(entity->get_peer_id());
        record.set_controller(entity->steering());
        spawn_book.issue(record);
        session->liveness_raise_epoch_to_wire(route);
        promoted.push_back(route);
    }
    return promoted;
}

PackedInt64Array Pipeline::begin_demotion() {
    fail_pending_spawns(
        String("The server changed before the spawn function returned.")
    );
    return spawn_book.armed_routes();
}

void Pipeline::demote_spawned() {
    const PackedInt64Array routes = spawn_book.spawned_routes();
    for (int at = 0; at < routes.size(); ++at) {
        const Record *record = spawn_book.spawned_of(routes[at]);
        const Book::Received recipe = Book::received_of(*record);
        spawn_book.drop_spawned(routes[at]);
        spawn_book.enroll_recv(routes[at], recipe);
    }
}

void Pipeline::clear_route(int64_t p_route, Ops &r_ops) {
    release_build(p_route, r_ops);
    unadded.erase(p_route);
    spawn_book.drop_armed(p_route);
    spawn_book.drop_recv(p_route);
    if (session != nullptr) {
        session->action_gate_drop(p_route);
        settle_action_reveal();
    }
}

Dictionary Pipeline::counters() const {
    Dictionary out;
    out[StringName("drops_spawn_bad_sender")] = drops_spawn_bad_sender;
    out[StringName("drops_spawn_duplicate")] = drops_spawn_duplicate;
    out[StringName("drops_spawn_unresolved")] = drops_spawn_unresolved;
    out[StringName("drops_spawn_truncated")] = drops_spawn_truncated;
    out[StringName("drops_spawn_stale_life")] = drops_spawn_stale_life;
    out[StringName("drops_reparent_stale")] = drops_reparent_stale;
    out[StringName("drops_despawn_unknown")] = drops_despawn_unknown;
    out[StringName("drops_hide_unknown")] = drops_hide_unknown;
    out[StringName("spawn_deferrals")] = spawn_deferrals;
    out[StringName("spawn_parked_cancelled")] = spawn_parked_cancelled;
    out[StringName("spawn_park_expired")] = spawn_park_expired;
    out[StringName("spawn_park_refused")] = spawn_park_refused;
    out[StringName("spawn_nested_published")] = spawn_nested_published;
    out[StringName("lifecycle_ops_admitted")] = lifecycle_ops_admitted;
    out[StringName("lifecycle_ops_refused")] = lifecycle_ops_refused;
    out[StringName("spawn_book_armed")] = spawn_book.armed_count();
    out[StringName("spawn_book_spawned")] = spawn_book.spawned_count();
    out[StringName("spawn_book_recv")] = spawn_book.recv_count();
    return out;
}

} // namespace netw::spawn
