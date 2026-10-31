#pragma once

#include "loopback_rig.h"
#include "minted_script.h"
#include "scenario.h"

#include "godot/hash_map.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "godot/variant.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/schema_model.hpp"
#include "netw/api/session.hpp"
#include "netw/schema_model.hpp"

namespace netw_test {

struct ScenarioVerbs {
    LoopbackRig &rig;
    godot::HashMap<godot::String, godot::RID> spawned;
    godot::HashMap<godot::String, godot::ObjectID> lingering;
    godot::HashMap<godot::String, godot::Ref<godot::Script>> scripts;
    bool declares_tables = false;

    ScenarioVerbs(LoopbackRig &p_rig, const Scenario &p_scenario)
        : rig(p_rig),
          declares_tables(p_scenario.declares(godot::StringName("table"))) {
        if (p_scenario.declares(godot::StringName("spawn"))
            || p_scenario.declares(godot::StringName("admit"))) {
            rig.mount();
        }
    }

    ~ScenarioVerbs() {
        if (declares_tables) {
            netw::schema_model::clear();
        }
    }
};

namespace scenario_verbs {

constexpr const char *CAR_SOURCE = R"(extends Node3D


func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").state()
)";

constexpr const char *WHEEL_SOURCE = R"(extends Node3D


func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"rotation").state()
)";

constexpr const char *KART_SOURCE = R"(extends Node3D

var honks := 0


func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").state()
	Netw.configure_rpc(honk)
	Netw.configure_rpc(ask).any_peer()


func honk(times: int) -> void:
	honks += times


func ask(value: int) -> int:
	return value + 1
)";

constexpr const char *PILOT_SOURCE = R"(extends Node3D

var stunned := false


func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").state()
	Netw.configure_property(self, &"stunned").state().retained()
	Netw.configure_property(self, &"rotation").input()
)";

constexpr const char *GATE_SOURCE = R"(extends Node

var refused := ""


func admit(request: NetwJoinRequest) -> Error:
	return ERR_UNAUTHORIZED if request.username == refused else OK
)";

struct SceneRow {
    const char *scene;
    const char *source;
};

constexpr SceneRow SCENES[] = {
    {"car", CAR_SOURCE},
    {"wheel", WHEEL_SOURCE},
    {"kart", KART_SOURCE},
    {"pilot", PILOT_SOURCE},
};

struct ModeRow {
    const char *mode;
    godot::MultiplayerPeer::TransferMode transfer;
};

constexpr ModeRow MODES[] = {
    {"reliable", godot::MultiplayerPeer::TRANSFER_MODE_RELIABLE},
    {"unreliable", godot::MultiplayerPeer::TRANSFER_MODE_UNRELIABLE},
    {"ordered", godot::MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED},
};

inline void refuse(const godot::String &p_why) {
    FAIL(p_why.utf8().get_data());
}

inline godot::Node *build(
    const godot::Variant &p_name,
    const godot::Variant &p_script
) {
    const godot::Ref<godot::Script> script = p_script;
    godot::Node *made = godot::Object::cast_to<godot::Node>(
        netw::gd::live_object(script->call("new"))
    );
    made->set_name(godot::String(p_name));
    return made;
}

inline godot::Node *nothing(const godot::Variant &) {
    return nullptr;
}

inline godot::Variant parsed(const godot::String &p_text) {
    if (p_text == "true" || p_text == "false") {
        return p_text == "true";
    }
    const godot::PackedStringArray parts = p_text.split(",");
    for (const godot::String &part : parts) {
        if (!part.is_valid_float()) {
            return p_text;
        }
    }
    if (parts.size() == 1) {
        return p_text.is_valid_int() ? godot::Variant(p_text.to_int())
                                     : godot::Variant(p_text.to_float());
    }
    if (parts.size() == 2) {
        return godot::Vector2(parts[0].to_float(), parts[1].to_float());
    }
    if (parts.size() == 3) {
        return godot::Vector3(
            parts[0].to_float(),
            parts[1].to_float(),
            parts[2].to_float()
        );
    }
    return p_text;
}

inline int side_of(const godot::String &p_subject, int p_clients) {
    if (p_subject == "server") {
        return -1;
    }
    const godot::String index = p_subject.substr(1);
    if (p_subject.begins_with("c") && index.is_valid_int()
        && index.to_int() >= 1 && index.to_int() <= p_clients) {
        return int(index.to_int()) - 1;
    }
    refuse(godot::vformat(
        "'%s' names no peer. Write server, or c1 to c%d for a client.",
        p_subject,
        p_clients
    ));
    return -1;
}

inline netw::NetwMultiplayer *api_of(ScenarioVerbs &p_verbs, int p_side) {
    return p_side < 0 ? p_verbs.rig.server() : p_verbs.rig.client(p_side);
}

inline godot::RID entity_of(
    ScenarioVerbs &p_verbs,
    const godot::String &p_name
) {
    if (!p_verbs.spawned.has(p_name)) {
        refuse(godot::vformat(
            "No spawn row made '%s'. Spawn it on an earlier tick.",
            p_name
        ));
    }
    return p_verbs.spawned[p_name];
}

inline godot::Node *parent_of(
    ScenarioVerbs &p_verbs,
    const godot::String &p_parent
) {
    if (p_verbs.spawned.has(p_parent)) {
        return p_verbs.rig.server()->entity_get_node(p_verbs.spawned[p_parent]);
    }
    godot::Node *found
        = p_verbs.rig.branch(-1)->get_node_or_null(godot::NodePath(p_parent));
    if (found != nullptr) {
        return found;
    }
    if (p_parent.contains("/")) {
        refuse(godot::vformat(
            "The parent '%s' is not under the server's root. Spawn it on an "
            "earlier tick, or name one node to make on every peer.",
            p_parent
        ));
    }
    return p_verbs.rig.mirror_child(p_parent);
}

inline void join(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int next = p_verbs.rig.count() + 1;
    if (p_subject != godot::vformat("c%d", next)) {
        refuse(godot::vformat(
            "A client joins when it connects, so a join row names the next "
            "client, c%d.",
            next
        ));
    }
    p_verbs.rig.arrive(godot::String(p_values["name"]));
}

inline void leave(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &
) {
    api_of(p_verbs, side_of(p_subject, p_verbs.rig.count()))->session_leave();
}

inline void kick(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int side = side_of(p_subject, p_verbs.rig.count());
    if (side < 0) {
        refuse("A kick row names a client, such as c1.");
    }
    p_verbs.rig.server()->peer_kick(
        p_verbs.rig.peer_id(side),
        godot::String(p_values.get("reason", godot::String()))
    );
}

inline void admit(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    if (p_subject != "server" || p_verbs.scripts.has("gate")) {
        refuse("One admit row names the server, which decides who joins.");
    }
    p_verbs.scripts["gate"] = minted_script(GATE_SOURCE);
    godot::Node *gate = build("Gate", p_verbs.scripts["gate"]);
    gate->set("refused", godot::String(p_values["refuse"]));
    p_verbs.rig.branch(-1)->add_child(gate);
    netw::Netw::configure_admission(
        godot::Callable(gate, godot::StringName("admit"))
    );
}

inline void transfer(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int to = side_of(p_values["to"], p_verbs.rig.count());
    if (p_subject != "server" || to < 0) {
        refuse("A transfer row names the server and the client that takes "
               "over, such as server to:c1.");
    }
    const godot::Error answered
        = p_verbs.rig.server()->get_session()->transfer_server(
            p_verbs.rig.peer_id(to)
        );
    if (answered != godot::OK) {
        refuse(godot::vformat(
            "The server refused to hand over to %s.",
            godot::String(p_values["to"])
        ));
    }
}

inline void pause(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const godot::String paused = p_values["paused"];
    if (paused != "true" && paused != "false") {
        refuse("A pause row needs paused:true or paused:false.");
    }
    netw::NetwMultiplayer *api
        = api_of(p_verbs, side_of(p_subject, p_verbs.rig.count()));
    if (paused == "true") {
        api->session_pause(godot::String(p_values.get("reason", "")));
    } else {
        api->session_unpause();
    }
}

inline void send_bytes(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int from = side_of(p_subject, p_verbs.rig.count());
    const int to = side_of(p_values["to"], p_verbs.rig.count());
    const godot::String bytes = p_values["bytes"];
    const godot::String mode = p_values.get("mode", "reliable");
    const godot::String channel = p_values.get("channel", "0");
    if (bytes.is_empty() || bytes.length() % 2 != 0
        || !bytes.is_valid_hex_number()) {
        refuse("A send_bytes row needs bytes as pairs of hex digits, such "
               "as bytes:07ff.");
    }
    if (!channel.is_valid_int()) {
        refuse("A send_bytes channel is a whole number, such as channel:0.");
    }
    for (const ModeRow &row : MODES) {
        if (mode == row.mode) {
            api_of(p_verbs, from)->send_bytes(
                bytes.hex_decode(),
                p_verbs.rig.peer_id(to),
                row.transfer,
                int(channel.to_int())
            );
            return;
        }
    }
    refuse("A send_bytes mode is reliable, unreliable or ordered.");
}

inline void despawn(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const godot::RID entity = entity_of(p_verbs, p_subject);
    godot::Node *leaving = p_verbs.rig.node_of(entity);
    godot::Ref<netw::NetwDespawnOpts> opts;
    if (p_values.has("linger")) {
        const godot::String seconds = p_values["linger"];
        if (!seconds.is_valid_float() || seconds.to_float() <= 0.0) {
            refuse("A despawn linger is a number of seconds above zero, such "
                   "as linger:1.");
        }
        opts = netw::NetwDespawnOpts::create(godot::StringName());
        opts->set_linger(true);
        opts->set_linger_seconds(seconds.to_float());
    }
    const godot::Error refused
        = p_verbs.rig.server()->entity_despawn(entity, opts);
    if (refused != godot::OK) {
        refuse(
            godot::vformat("The server refused to despawn '%s'.", p_subject)
        );
    }
    if (opts.is_valid()) {
        p_verbs.lingering[p_subject] = netw::gd::instance_id(leaving);
        return;
    }
    leaving->get_parent()->remove_child(leaving);
}

inline void remove(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &
) {
    godot::Node *leaving = p_verbs.lingering.has(p_subject)
        ? godot::Object::cast_to<godot::Node>(
              netw::gd::instance_from_id(p_verbs.lingering[p_subject])
          )
        : nullptr;
    if (leaving == nullptr || leaving->get_parent() == nullptr) {
        refuse(godot::vformat(
            "'%s' is not lingering in the tree. Despawn it with linger on an "
            "earlier tick.",
            p_subject
        ));
    }
    p_verbs.lingering.erase(p_subject);
    leaving->get_parent()->remove_child(leaving);
}

inline void host_delay(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int side = side_of(p_subject, p_verbs.rig.count());
    const godot::String ticks = p_values["ticks"];
    if (side < 0) {
        refuse("A host_delay row names a client, such as c1.");
    }
    if (!ticks.is_valid_int() || ticks.to_int() < 1) {
        refuse("A host_delay row ends with a whole number of ticks above "
               "zero, such as host_delay c1 car 1.");
    }
    const godot::RID entity = entity_of(p_verbs, p_values["entity"]);
    p_verbs.rig.spawn_plane(side)->delay_placement(
        p_verbs.rig.server()->entity_get_route(entity),
        ticks.to_int()
    );
}

inline void reparent(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    p_verbs.rig.server()->entity_reparent(
        entity_of(p_verbs, p_subject),
        parent_of(p_verbs, p_values["parent"])
    );
}

inline void spawn(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    if (p_verbs.spawned.has(p_subject)) {
        refuse(godot::vformat(
            "'%s' is already spawned. Give each spawn row its own subject.",
            p_subject
        ));
    }
    const godot::String scene = p_values["scene"];
    for (const SceneRow &row : SCENES) {
        if (scene != row.scene) {
            continue;
        }
        if (!p_verbs.scripts.has(scene)) {
            p_verbs.scripts[scene] = minted_script(row.source);
        }
        godot::Array args;
        args.push_back(godot::String(p_values["name"]));
        godot::Array types;
        types.push_back(int(godot::Variant::STRING));
        p_verbs.rig.spawn_registered(
            godot::StringName(scene),
            callable_mp_static(&build).bind(p_verbs.scripts[scene]),
            args,
            types,
            parent_of(p_verbs, p_values["parent"]),
            p_values.has("owner")
                ? godot::Variant(p_verbs.rig.player(
                      side_of(p_values["owner"], p_verbs.rig.count())
                  ))
                : godot::Variant(),
            false
        );
        p_verbs.spawned[p_subject] = p_verbs.rig.spawned_entity();
        return;
    }
    refuse(godot::vformat(
        "No scene is named '%s'. Use car, wheel, kart or pilot.",
        scene
    ));
}

inline void refuse_scene(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int side = side_of(p_subject, p_verbs.rig.count());
    const godot::String scene = p_values["scene"];
    if (side < 0) {
        refuse("A refuse row names a client, such as c2.");
    }
    if (!p_verbs.scripts.has(scene)) {
        refuse(godot::vformat(
            "No spawn row made a '%s'. Refuse a scene after its spawn row.",
            scene
        ));
    }
    godot::Array types;
    types.push_back(int(godot::Variant::STRING));
    p_verbs.rig.register_constructor(
        p_verbs.rig.client(side),
        godot::StringName(scene),
        callable_mp_static(&nothing),
        types
    );
}

inline godot::Callable member_of(
    ScenarioVerbs &p_verbs,
    int p_side,
    const godot::Dictionary &p_values
) {
    const int64_t route = p_verbs.rig.server()->entity_get_route(
        entity_of(p_verbs, p_values["entity"])
    );
    netw::NetwMultiplayer *api = api_of(p_verbs, p_side);
    godot::Node *node = api->entity_get_node(api->entity_from_route(route));
    if (node == nullptr) {
        refuse(godot::vformat(
            "'%s' is not on that peer yet. Call it on a later tick.",
            godot::String(p_values["entity"])
        ));
    }
    return godot::Callable(node, godot::StringName(p_values["method"]));
}

inline godot::Array args_of(const godot::Dictionary &p_values) {
    godot::Array args;
    if (p_values.has("arg")) {
        args.push_back(parsed(p_values["arg"]));
    }
    return args;
}

inline void rpc(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    netw::Netw::rpc(
        member_of(p_verbs, side_of(p_subject, p_verbs.rig.count()), p_values),
        args_of(p_values)
    );
}

inline void request(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const int side = side_of(p_subject, p_verbs.rig.count());
    if (side < 0) {
        refuse("A request row names the client that asks the server, such as "
               "c1.");
    }
    netw::Netw::request(member_of(p_verbs, side, p_values), args_of(p_values));
}

inline void table(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const godot::String rows = p_values["rows"];
    const godot::String hp = p_values["hp"];
    if (!rows.is_valid_int() || rows.to_int() < 1 || !hp.is_valid_int()) {
        refuse("A table row needs rows above zero and a whole hp, such as "
               "rows:2 hp:90.");
    }
    netw::NetwSchema::declare(p_subject)->i32("hp", 1);
    netw::NetwMultiplayer *host = p_verbs.rig.server();
    const godot::RID published = host->table_find_or_adopt(p_subject);
    for (int index = 0; index < p_verbs.rig.count(); ++index) {
        p_verbs.rig.client(index)->table_find_or_adopt(p_subject);
    }
    const godot::PackedInt64Array routes
        = host->liveness_claim_routes(rows.to_int());
    godot::PackedInt32Array hps;
    for (int64_t at = 0; at < routes.size(); ++at) {
        hps.push_back(int32_t(hp.to_int()));
    }
    host->table_write_routes(published, routes);
    host->table_write_column(published, 0, hps);
    if (host->table_commit(published) != godot::OK) {
        refuse(godot::vformat("The server refused to commit '%s'.", p_subject));
    }
}

inline void set(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const godot::PackedStringArray named = p_subject.split("@");
    const godot::RID entity = entity_of(p_verbs, named[0]);
    godot::Node *node = p_verbs.rig.server()->entity_get_node(entity);
    if (named.size() > 1) {
        netw::NetwMultiplayer *api = api_of(
            p_verbs,
            side_of(named[1], p_verbs.rig.count())
        );
        const int64_t route = p_verbs.rig.server()->entity_get_route(entity);
        node = api->entity_get_node(api->entity_from_route(route));
        if (node == nullptr) {
            refuse(godot::vformat(
                "'%s' is not on that peer yet. Set it on a later tick.",
                p_subject
            ));
        }
    }
    for (const godot::Variant &key : p_values.keys()) {
        node->set(godot::StringName(key), parsed(p_values[key]));
    }
}

inline void layer(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    if (!p_values.has("entity") && !p_values.has("viewer")) {
        refuse("A layer row needs an entity or a viewer, such as entity:car "
               "or viewer:c1.");
    }
    netw::NetwMultiplayer *host = p_verbs.rig.server();
    const godot::RID opened
        = host->interest_layer_create(godot::StringName(p_subject));
    if (p_values.has("entity")
        && host->interest_layer_add_entity(
               opened,
               entity_of(p_verbs, p_values["entity"])
           ) != godot::OK) {
        refuse(godot::vformat("The layer '%s' refused the entity.", p_subject));
    }
    if (p_values.has("viewer")
        && host->interest_layer_add_viewer(
               opened,
               p_verbs.rig.peer_id(
                   side_of(p_values["viewer"], p_verbs.rig.count())
               )
           ) != godot::OK) {
        refuse(godot::vformat("The layer '%s' refused the viewer.", p_subject));
    }
}

inline void unlayer(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    netw::NetwMultiplayer *host = p_verbs.rig.server();
    const godot::RID opened
        = host->interest_layer_find(godot::StringName(p_subject));
    if (!opened.is_valid()) {
        refuse(godot::vformat(
            "No layer row made '%s'. Add to it on an earlier tick.",
            p_subject
        ));
    }
    if (!p_values.has("entity") && !p_values.has("viewer")) {
        refuse("An unlayer row needs an entity or a viewer, such as "
               "entity:car or viewer:c1.");
    }
    if (p_values.has("entity")) {
        host->interest_layer_remove_entity(
            opened,
            entity_of(p_verbs, p_values["entity"])
        );
    }
    if (p_values.has("viewer")) {
        host->interest_layer_remove_viewer(
            opened,
            p_verbs.rig.peer_id(
                side_of(p_values["viewer"], p_verbs.rig.count())
            )
        );
    }
}

struct Leg {
    int client;
    bool to_server;
};

inline Leg leg_of(const godot::String &p_subject) {
    const bool to_server = p_subject.ends_with(">s");
    const godot::String client = to_server ? p_subject.trim_suffix(">s")
                                           : p_subject.trim_prefix("s>");
    const godot::String ordinal = client.substr(1);
    if ((to_server || p_subject.begins_with("s>")) && client.begins_with("c")
        && ordinal.is_valid_int() && ordinal.to_int() >= 1) {
        return {int(ordinal.to_int()), to_server};
    }
    refuse(godot::vformat(
        "'%s' names no leg. Write s>c1 for server to client 1, or c1>s for "
        "client 1 to server.",
        p_subject
    ));
    return {0, false};
}

inline uint64_t unsent_index(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const Leg &p_leg,
    const godot::Dictionary &p_values
) {
    const godot::String index = p_values["index"];
    if (!index.is_valid_int() || index.to_int() < 0) {
        refuse("A datagram index is a whole number from 0, such as index:0.");
    }
    const uint64_t sent
        = p_verbs.rig.session()->datagrams_on(p_leg.client, p_leg.to_server);
    if (uint64_t(index.to_int()) < sent) {
        refuse(godot::vformat(
            "Datagram %s on %s was sent before this row. Name index %d or "
            "later, or join the client after this row.",
            index,
            p_subject,
            int64_t(sent)
        ));
    }
    return uint64_t(index.to_int());
}

inline void drop(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const Leg leg = leg_of(p_subject);
    p_verbs.rig.session()->script_drop(
        leg.client,
        leg.to_server,
        unsent_index(p_verbs, p_subject, leg, p_values)
    );
}

inline void delay(
    ScenarioVerbs &p_verbs,
    const godot::String &p_subject,
    const godot::Dictionary &p_values
) {
    const Leg leg = leg_of(p_subject);
    const godot::String ticks = p_values["ticks"];
    if (!ticks.is_valid_int() || ticks.to_int() < 1) {
        refuse("A delay row needs ticks as a whole number above zero, such as "
               "ticks:2.");
    }
    p_verbs.rig.session()->script_delay(
        leg.client,
        leg.to_server,
        unsent_index(p_verbs, p_subject, leg, p_values),
        double(ticks.to_int()) * p_verbs.rig.tick_ms()
    );
}

struct VerbRow {
    const char *verb;
    const char *keys;
    const char *required;
    void (*run)(
        ScenarioVerbs &,
        const godot::String &,
        const godot::Dictionary &
    );
    bool positional = false;
};

constexpr VerbRow VERBS[] = {
    {"join", "name", "name", join},
    {"leave", "", "", leave},
    {"kick", "reason", "", kick},
    {"admit", "refuse", "refuse", admit},
    {"transfer", "to", "to", transfer},
    {"pause", "paused reason", "paused", pause},
    {"send_bytes", "to bytes mode channel", "to bytes", send_bytes},
    {"despawn", "linger", "", despawn},
    {"remove", "", "", remove},
    {"reparent", "parent", "parent", reparent},
    {"spawn", "scene parent name owner", "scene parent name", spawn},
    {"refuse", "scene", "scene", refuse_scene},
    {"set", nullptr, "", set},
    {"rpc", "entity method arg", "entity method", rpc},
    {"request", "entity method arg", "entity method", request},
    {"table", "rows hp", "rows hp", table},
    {"layer", "entity viewer", "", layer},
    {"unlayer", "entity viewer", "", unlayer},
    {"drop", "index", "index", drop},
    {"delay", "index ticks", "index ticks", delay},
    {"host_delay", "entity ticks", "entity ticks", host_delay, true},
};

inline godot::Dictionary values_of(
    const VerbRow &p_row,
    const godot::String &p_value
) {
    godot::Dictionary out;
    const godot::String accepted = p_row.keys != nullptr ? p_row.keys : "";
    const godot::PackedStringArray keys = accepted.split(" ", false);
    if (p_row.positional) {
        const godot::PackedStringArray tokens = p_value.split(" ", false);
        if (tokens.size() != keys.size()) {
            refuse(godot::vformat(
                "A %s row ends with %s, one word each.",
                p_row.verb,
                accepted
            ));
        }
        for (int64_t at = 0; at < tokens.size(); ++at) {
            out[keys[at]] = tokens[at];
        }
        return out;
    }
    for (const godot::String &token : p_value.split(" ", false)) {
        const int colon = token.find(":");
        const godot::String key = token.substr(0, colon);
        if (colon <= 0 || colon == token.length() - 1) {
            refuse(godot::vformat(
                "'%s' is not a key:value pair. Write it as key:value, such as "
                "name:Car.",
                token
            ));
        }
        if (p_row.keys != nullptr && !keys.has(key)) {
            refuse(godot::vformat(
                "A %s row takes no '%s'. It takes %s.",
                p_row.verb,
                key,
                keys.is_empty() ? godot::String("no value") : accepted
            ));
        }
        out[key] = token.substr(colon + 1);
    }
    const godot::String required = p_row.required;
    for (const godot::String &key : required.split(" ", false)) {
        if (!out.has(key)) {
            refuse(godot::vformat(
                "A %s row needs %s. Add %s:<value> to the row.",
                p_row.verb,
                p_row.required,
                key
            ));
        }
    }
    return out;
}

inline bool run(ScenarioVerbs &p_verbs, const Scenario::Step &p_step) {
    for (const VerbRow &row : VERBS) {
        if (p_step.verb == godot::StringName(row.verb)) {
            row.run(
                p_verbs,
                godot::String(p_step.subject),
                values_of(row, godot::String(p_step.value))
            );
            return true;
        }
    }
    return false;
}

} // namespace scenario_verbs

} // namespace netw_test
