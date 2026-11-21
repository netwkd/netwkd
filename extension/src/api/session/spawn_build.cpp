#include "netw/api/netw_multiplayer.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "godot/utility.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/property_config.hpp"
#include "netw/call_args.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/lifecycle/rule.hpp"
#include "netw/object_port.hpp"
#include "netw/script/model.hpp"
#include "netw/session/event_plane.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"
#include "netw/spawn/spawner_compat.hpp"
#include "netw/value_variant.hpp"

using namespace godot;

namespace netw {

spawn::Built::Outcome NetwMultiplayer::spawn_build(
    spawn::Built &r_built,
    int64_t p_sender,
    spawn::Ops &r_ops
) {
    using spawn::Book;
    using spawn::Built;
    using spawn::Park;
    using spawn::SpawnFrame;

    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return Built::DROPPED;
    }
    SpawnFrame &frame = r_built.frame;
    const int64_t route = frame.route;
    const Dictionary &header = frame.header;
    const int64_t recipe = frame.recipe;
    const spawn::Anchor &parent_anchor = frame.parent_anchor;
    const int64_t action_spawn_tick
        = int64_t(header[StringName("action_spawn_tick")]);
    const String node_name = header[StringName("node_name")];

    Node *node = nullptr;
    Node *elided_parent = nullptr;
    MultiplayerSpawner *recv_spawner = nullptr;
    bool adopted = false;

    if (recipe == Book::RECIPE_ADOPT) {
        const spawn::Anchor &found_under
            = frame.moved_from_origin ? frame.origin_anchor : parent_anchor;
        Node *adopt_parent = anchor_resolve(found_under);
        node = adopt_parent != nullptr
            ? adopt_parent->get_node_or_null(NodePath(node_name))
            : nullptr;
        spawn_record_construct(node);
        if (node == nullptr) {
            r_built.wait = Park::WAIT_ADOPT;
            return Built::WAITS;
        }
        r_built.origin = adopt_parent;
        adopted = true;
    } else if (recipe == Book::RECIPE_SPAWNER) {
        recv_spawner = Object::cast_to<MultiplayerSpawner>(
            anchor_resolve(frame.spawner_anchor)
        );
        if (recv_spawner == nullptr) {
            r_built.wait = Park::WAIT_SPAWNER;
            return Built::WAITS;
        }
        const int64_t scene_index = frame.scene_index;
        if (scene_index < 0) {
            if (!value::decode_packed(frame.custom, r_built.data)) {
                r_built.data = Variant();
                FaultValues values;
                values.name = vformat("'%s'", recv_spawner->get_name());
                values.peer = p_sender;
                fault::raise(this, NETW_FAULT_VALUE_UNREADABLE, values);
            }
        } else if (
            scene_index < int64_t(recv_spawner->get_spawnable_scene_count())
        ) {
            const Built::Outcome loaded = pipeline->wait_for_scene(
                recv_spawner->get_spawnable_scene(int(scene_index)),
                r_built,
                r_ops
            );
            if (loaded != Built::BUILT) {
                return loaded;
            }
        }
        if (frame.parent_is_spawn_target) {
            elided_parent = recv_spawner->get_node_or_null(
                recv_spawner->get_spawn_path()
            );
            if (elided_parent == nullptr) {
                fault::raise(
                    this,
                    NETW_FAULT_SPAWN_MISMATCH,
                    {String(),
                     String(),
                     String("the node at its MultiplayerSpawner spawn_path"),
                     p_sender,
                     route},
                    uint64_t(gd::instance_id(recv_spawner))
                );
                pipeline->drops_spawn_unresolved += 1;
                return Built::DROPPED;
            }
        }
        spawn::SpawnerCompat *adapter = spawner_adapter();
        node = adapter != nullptr
            ? adapter->instantiate(recv_spawner, int(scene_index), r_built.data)
            : nullptr;
        spawn_record_construct(node);
        if (node == nullptr) {
            fault::raise(
                this,
                NETW_FAULT_SPAWN_RETURN,
                {String("MultiplayerSpawner"),
                 String("no returned Node"),
                 String("a spawner that builds a node on every peer")}
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
    } else if (recipe == Book::RECIPE_FN_REGISTRY) {
        const StringName id = StringName(frame.fn_id);
        const Callable fn = spawn_constructor_of(id);
        if (!fn.is_valid()) {
            fault::raise(
                this,
                NETW_FAULT_SPAWN_MISMATCH,
                {String(),
                 String(),
                 String("spawn constructor '") + String(id) + String("'"),
                 p_sender,
                 route}
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
        const Variant schema = spawn_registry_schema(id);
        if (schema.get_type() == Variant::NIL) {
            fault::raise(
                this,
                NETW_FAULT_SPAWN_MISMATCH,
                {String(),
                 String(),
                 String("the argument types of spawn constructor '")
                     + String(id) + String("'"),
                 p_sender,
                 route}
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
        const LocalVector<call_args::Slot> encoded
            = spawn::Pipeline::read_fn_args(frame.args, schema);
        const Variant resolved = spawn_resolve_args(encoded, r_built);
        if (resolved.get_type() == Variant::NIL) {
            if (r_built.names_a_freed_node) {
                pipeline->drops_spawn_unresolved += 1;
                return Built::DROPPED;
            }
            r_built.wait = Park::WAIT_ROUTE;
            return Built::WAITS;
        }
        r_built.args = resolved;
        bool called = true;
        const Variant made = game_call(
            fn,
            Array(resolved),
            game_calls::SPAWN_CONSTRUCTOR,
            &called
        );
        const bool waited = is_waiting(made);
        if (waited) {
            free_when_returned(made);
        } else {
            node = Object::cast_to<Node>(gd::live_object(made));
        }
        spawn_record_construct(node);
        if (node == nullptr) {
            if (waited) {
                fault::raise(
                    this,
                    NETW_FAULT_SPAWN_RETURN,
                    {String("spawn constructor '") + String(id) + String("'"),
                     String("an await"),
                     String(
                         "a constructor that returns the node without "
                         "waiting"
                     )}
                );
            } else if (called) {
                fault::raise(
                    this,
                    NETW_FAULT_SPAWN_RETURN,
                    {String("spawn constructor '") + String(id) + String("'"),
                     String("no returned Node"),
                     String("a constructor that returns a Node")}
                );
            }
            spawn_report_failed(
                route,
                waited ? String("awaited") : String("returned no Node")
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
    } else if (recipe == Book::RECIPE_FN) {
        Node *host = anchor_resolve(frame.host_anchor);
        if (host == nullptr) {
            fault::raise(
                this,
                NETW_FAULT_SPAWN_MISMATCH,
                {String(),
                 String(),
                 String("the node holding its spawn function"),
                 p_sender,
                 route}
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
        r_built.host = host;
        const StringName method = StringName(frame.method);
        const Variant schema = spawn_script_schema(host->get_script(), method);
        if (schema.get_type() == Variant::NIL) {
            fault::raise(
                this,
                NETW_FAULT_SPAWN_MISMATCH,
                {String(),
                 String(),
                 String("spawn function '") + String(method) + String("'"),
                 p_sender,
                 route},
                uint64_t(gd::instance_id(host))
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
        const LocalVector<call_args::Slot> encoded
            = spawn::Pipeline::read_fn_args(frame.args, schema);
        const Variant resolved = spawn_resolve_args(encoded, r_built);
        if (resolved.get_type() == Variant::NIL) {
            if (r_built.names_a_freed_node) {
                pipeline->drops_spawn_unresolved += 1;
                return Built::DROPPED;
            }
            r_built.wait = Park::WAIT_ROUTE;
            return Built::WAITS;
        }
        r_built.args = resolved;
        bool called = true;
        bool waited = false;
        Variant waiting;
        const spawn::Build *held = pipeline->builds.getptr(route);
        if (held != nullptr && held->scene.is_empty()) {
            node = pipeline->take_ready(route);
        } else {
            const Variant made = game_call(
                Callable(host, method),
                Array(resolved),
                game_calls::SPAWN_FUNCTION,
                &called
            );
            waited = is_waiting(made);
            if (waited) {
                waiting = made;
            } else {
                node = Object::cast_to<Node>(gd::live_object(made));
            }
        }
        spawn_record_construct(node);
        if (node == nullptr && waited && frame.fn_waits && !r_built.strict) {
            const Built::Outcome settled = spawn_wait_for_function(
                waiting,
                host,
                method,
                r_built,
                r_ops
            );
            if (settled != Built::BUILT) {
                return settled;
            }
            node = pipeline->take_ready(route);
        } else if (waited) {
            free_when_returned(waiting);
        }
        if (node == nullptr) {
            if (waited) {
                fault::raise(
                    this,
                    NETW_FAULT_SPAWN_RETURN,
                    {String("spawn function '") + String(method) + String("'"),
                     String("an await"),
                     String(
                         "{Netw.spawn_async} for a spawn function that "
                         "awaits"
                     )}
                );
            } else if (called) {
                fault::raise(
                    this,
                    NETW_FAULT_SPAWN_RETURN,
                    {String("spawn function '") + String(method) + String("'"),
                     String("no returned Node"),
                     String("a function that returns a Node")}
                );
            }
            spawn_report_failed(
                route,
                waited ? String("awaited") : String("returned no Node")
            );
            pipeline->drops_spawn_unresolved += 1;
            return Built::DROPPED;
        }
    }

    Node *parent = elided_parent != nullptr ? elided_parent
                                            : anchor_resolve(parent_anchor);
    if (parent == nullptr) {
        fault::raise(
            this,
            NETW_FAULT_SPAWN_MISMATCH,
            {String(),
             String(),
             String("its parent '") + parent_anchor.path + String("'"),
             p_sender,
             route}
        );
        pipeline->drops_spawn_unresolved += 1;
        if (!adopted && node != nullptr) {
            node->queue_free();
        }
        return Built::DROPPED;
    }

    if (!adopted) {
        NetwEntity::settle_parts(node, true);
    }
    NetwEntity::stamp_header(node, header, this);
    if (action_spawn_tick >= 0) {
        pipeline->note_action_gate();
    }

    for (uint32_t at = 0; at < frame.state.size(); ++at) {
        const SpawnFrame::StateEntry &entry = frame.state[at];
        Node *target = entry.by_path
            ? node->get_node_or_null(NodePath(entry.path))
            : node;
        const Variant prop_token
            = netw::script::model::token_of_bytes(entry.token);
        const PackedByteArray &value_bytes = entry.values;
        if (target == nullptr) {
            continue;
        }
        StringName prop;
        if (prop_token.get_type() == Variant::INT) {
            prop = netw::script::model::get_property_name_by_id(
                target->get_script(),
                int64_t(prop_token)
            );
        } else {
            prop = StringName(prop_token);
        }
        if (String(prop).is_empty() || !gd::has_property(target, prop)) {
            continue;
        }
        Array quantizers;
        Variant quantizer;
        const Dictionary configs
            = netw::script::model::get_node_property_configs(target);
        if (configs.has(prop)) {
            const Ref<NetwPropertyConfig> cfg = configs[prop];
            if (cfg.is_valid() && !cfg->get_quantizers().is_empty()) {
                quantizer = cfg->get_quantizers()[0];
            }
        }
        quantizers.push_back(quantizer);
        Array types;
        types.push_back(
            netw::script::model::get_node_property_type(target, prop)
        );
        wire::ReadStream value_stream(value_bytes);
        Array values;
        if (!call_args::values_read(value_stream, quantizers, types, values)
            || !value_stream.align_verify()
            || value_stream.bits_remaining() != 0) {
            continue;
        }
        if (!values.is_empty()) {
            port_set(target, prop, values[0]);
        }
    }

    for (uint32_t at = 0; at < frame.native.size(); ++at) {
        const String &path = frame.native[at].path;
        if (spawn::Record::apply_native_entry(
                node,
                path,
                frame.native[at].value
            )) {
            continue;
        }
        const String joint = path.begins_with(":") ? "" : "/";
        FaultValues values;
        values.name = vformat("'%s%s%s'", node->get_name(), joint, path);
        values.peer = p_sender;
        fault::raise(this, NETW_FAULT_VALUE_UNREADABLE, values);
    }
    if (pipeline->sync_compat != nullptr) {
        pipeline->sync_compat->note_schema(
            route,
            spawn::Record::descriptors_of_bytes(frame.consumed)
        );
    }
    sync_pipeline_note_derived_schema(
        route,
        spawn::Record::descriptors_of_bytes(frame.derived)
    );

    if (!adopted && !node_name.is_empty()) {
        if (parent->has_node(NodePath(node_name))) {
            node->set_name(vformat("%s@%d", node_name, route));
        } else {
            node->set_name(node_name);
        }
    }

    r_built.node = node;
    r_built.parent = parent;
    r_built.spawner = recv_spawner;
    r_built.adopted = adopted;
    return Built::BUILT;
}

void NetwMultiplayer::spawn_record_construct(Node *p_node) {
    event_record(
        EventPlane::SPAWN_CONSTRUCT,
        RID(),
        0,
        0,
        p_node != nullptr ? OK : ERR_CANT_CREATE
    );
}

Variant NetwMultiplayer::spawn_resolve_args(
    const LocalVector<call_args::Slot> &p_slots,
    spawn::Built &r_built
) {
    Array args;
    for (uint32_t at = 0; at < p_slots.size(); ++at) {
        const call_args::Slot &slot = p_slots[at];
        if (!slot.addresses_node) {
            args.push_back(slot.value);
            continue;
        }
        if (spawn::Park::anchor_parks(liveness_route_state(slot.node.route))) {
            r_built.wait_route = slot.node.route;
            return Variant();
        }
        Node *arg_node = replication_resolve_comp_node(
            liveness_core.rid_from_route(int(slot.node.route)),
            slot.node.comp,
            slot.node.path
        );
        if (arg_node == nullptr && r_built.strict) {
            r_built.names_a_freed_node = true;
            return Variant();
        }
        args.push_back(arg_node);
    }
    return args;
}

spawn::Built::Outcome NetwMultiplayer::spawn_wait_for_function(
    const Variant &p_waiting,
    Node *p_host,
    const StringName &p_method,
    spawn::Built &r_built,
    spawn::Ops &r_ops
) {
    spawn::Pipeline *pipeline = spawn_plane();
    const int64_t route = r_built.frame.route;
    const int64_t serial
        = pipeline->build_waits(route, p_waiting, p_host, p_method, r_ops);

    const uint64_t plane_id = uint64_t(gd::instance_id(this));
    Object *waiting = gd::live_object(p_waiting);
    if (NetwPromise *promised = Object::cast_to<NetwPromise>(waiting)) {
        promised->then(
            callable_mp_static(&NetwMultiplayer::spawn_build_returned)
                .bind(p_waiting, plane_id, route, serial)
        );
        promised->catch_error(
            callable_mp_static(&NetwMultiplayer::spawn_build_failed)
                .bind(p_waiting, plane_id, route, serial)
        );
    } else {
        waiting->connect(
            StringName("completed"),
            callable_mp_static(&NetwMultiplayer::spawn_build_returned)
                .bind(p_waiting, plane_id, route, serial),
            Object::CONNECT_ONE_SHOT
        );
    }

    return pipeline->build_settled(serial, r_built);
}

void NetwMultiplayer::spawn_rule_request(
    const spawn::NodeOp &p_op,
    spawn::Built::Outcome p_outcome,
    spawn::Built &r_built
) {
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    const int64_t route = p_op.route;
    const int64_t sender = p_op.sender;
    if (p_outcome != spawn::Built::BUILT) {
        pipeline->refuse_controller_spawn(
            route,
            sender,
            ERR_INVALID_PARAMETER,
            true
        );
        return;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(r_built.node);
    const bool held = entity.is_valid();
    lifecycle::Facts facts = lifecycle_sender_facts(
        held ? entity->get_record() : nullptr,
        held ? entity->get_owner() : nullptr,
        held ? entity->synchronizers() : TypedArray<MultiplayerSynchronizer>(),
        lifecycle::Kind::SPAWN,
        sender
    );
    facts.destination = lifecycle_destination(r_built.node, r_built.parent);
    const Error ruled = lifecycle::rule(facts).code;
    if (ruled != OK) {
        r_built.node->queue_free();
        pipeline->refuse_controller_spawn(route, sender, ruled, true);
        return;
    }
    String reason;
    Error denied = OK;
    if (lifecycle_filter_denies(
            lifecycle::Kind::SPAWN,
            sender,
            entity->get_rid_handle(),
            r_built.parent,
            reason,
            denied
        )) {
        r_built.node->queue_free();
        pipeline->refuse_controller_spawn(route, sender, denied, true, reason);
        return;
    }
    entity->seed_controller(sender);
    const spawn::SpawnFrame &frame = r_built.frame;
    spawn::Record record;
    record.set_recipe(frame.recipe);
    if (frame.recipe == spawn::Book::RECIPE_FN_REGISTRY) {
        record.set_fn_registry_id(StringName(frame.fn_id));
        record.set_fn_args(r_built.args);
    } else if (frame.recipe == spawn::Book::RECIPE_FN) {
        record.bind_fn_host(r_built.host);
        record.set_fn_method(StringName(frame.method));
        record.set_fn_args(r_built.args);
    }
    if (spawn_arm(&record, r_built.node).is_null()) {
        pipeline->drop_armed(route);
        r_built.node->queue_free();
        pipeline->refuse_controller_spawn(route, sender, ERR_UNAVAILABLE, true);
        return;
    }
    pipeline->note_requested(route, sender);
    spawn_place_node(r_built.parent, r_built.node);
    lifecycle_decide(route, sender, lifecycle::Kind::SPAWN, 0, OK);
}

void NetwMultiplayer::spawn_place(spawn::Ops &p_ops) {
    spawn::Pipeline *pipeline = spawn_plane();
    const spawn::Built &built = p_ops.build();
    const spawn::SpawnFrame &frame = built.frame;
    const int64_t route = frame.route;
    Node *node = built.node;
    Node *parent = built.parent;
    const Ref<NetwEntity> entity = NetwEntity::of(node);
    MultiplayerSpawner *recv_spawner = built.spawner;
    spawn::SpawnerCompat *adapter = spawner_adapter();
    const uint64_t anchor_revision
        = uint64_t(int64_t(frame.header.get(StringName("anchor_revision"), 0)));
    const uint64_t anchor_author
        = uint64_t(int64_t(frame.header.get(StringName("anchor_author"), 0)));

    liveness_core.defer_flushes();
    pipeline->enroll_received(built);
    if (built.adopted) {
        liveness_bind_route(route, entity.ptr());
        liveness_adopt_epoch(route, frame.epoch);
        if (frame.moved_from_origin) {
            spawn_reparent_node(
                node,
                parent,
                callable_mp(this, &NetwMultiplayer::scene_adopt_entity),
                route,
                anchor_revision,
                anchor_author
            );
        } else {
            entity_install_anchor(route, anchor_revision, anchor_author);
        }
        sync_pipeline_recapture_entity(entity->get_rid_handle());
        predict_reconcile_declaration(
            *entity->get_record(),
            entity->get_owner()
        );
    } else {
        liveness_adopt_epoch(route, frame.epoch);
        if (recv_spawner != nullptr && adapter != nullptr) {
            adapter->note_recv(route, recv_spawner);
        }
        spawn_place_node(parent, scene_wrap_world(node));
        entity_install_anchor(route, anchor_revision, anchor_author);
        if (recv_spawner != nullptr && adapter != nullptr) {
            adapter->emit_spawned(recv_spawner, node);
        }
    }
    spawn::Ops kept;
    pipeline->kept_reparent(route, kept);
    spawn_act(kept);
    liveness_core.resume_flushes();
}

void NetwMultiplayer::spawn_place_held() {
#ifdef NETW_TESTS
    spawn::Pipeline *pipeline = spawn_plane();
    if (pipeline == nullptr) {
        return;
    }
    while (true) {
        spawn::Ops ops;
        if (!pipeline->take_held_placement(ops)) {
            return;
        }
        spawn_act(ops);
    }
#endif
}

} // namespace netw
