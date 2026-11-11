#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_call_log.h"
#include "support/netw_cells.h"
#include "support/stock_stand.h"

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "godot/script.hpp"
#include "godot/variant.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/group_promise.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/promise.hpp"

namespace TestRpc {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwGroupPromise;
using netw::NetwMultiplayer;
using netw::NetwPromise;
using netw_test::CallLog;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr const char *SINK = R"(extends Node

var calls := 0
var last_tag := 0
var last_sender := 0
var pointed: Node

@rpc("any_peer", "call_remote", "reliable")
func note_call(tag: int) -> void:
	calls += 1
	last_tag = tag
	last_sender = multiplayer.get_remote_sender_id()

@rpc("any_peer", "call_remote", "reliable")
func double_tag(tag: int) -> int:
	calls += 1
	last_tag = tag
	last_sender = multiplayer.get_remote_sender_id()
	return tag * 2

@rpc("any_peer", "call_remote", "reliable")
func point_at(target: Node) -> void:
	pointed = target
)";

constexpr const char *LOCAL_SINK = R"(extends Node

var calls := 0
var last_tag := 0

func _init() -> void:
	Netw.configure_rpc(note_call).call_local()

func note_call(tag: int) -> void:
	calls += 1
	last_tag = tag
)";

constexpr const char *CONTROLLER_SINK = R"(extends Node

var calls := 0
var last_tag := 0

func _init() -> void:
	Netw.configure_rpc(note_call).controller()

func note_call(tag: int) -> void:
	calls += 1
	last_tag = tag
)";

constexpr const char *ANSWERING_SINK = R"(extends Node

@rpc("authority", "call_remote", "reliable")
func guarded(tag: int) -> int:
	return tag

@rpc("any_peer", "call_remote", "reliable")
func echo(tag: int) -> int:
	return tag * 2

@rpc("any_peer", "call_remote", "reliable")
func closed(_tag: int) -> NetwPromise:
	return NetwPromise.rejected(ERR_BUSY, "the shop is closed")
)";

constexpr const char *UNDECLARED_SINK = R"(extends Node

func guarded(tag: int) -> int:
	return tag

func echo(tag: int) -> int:
	return tag * 2

func closed(_tag: int) -> NetwPromise:
	return NetwPromise.rejected(ERR_BUSY, "the shop is closed")
)";

constexpr const char *DEADLINE_SINK = R"(extends Node

var held: NetwPromise
var hp := 0

func _init() -> void:
	Netw.configure_rpc(patient).timeout(60.0)

@rpc("any_peer", "call_remote", "reliable")
func patient(_tag: int) -> NetwPromise:
	held = NetwPromise.new()
	return held

@rpc("any_peer", "call_remote", "reliable")
func hasty(_tag: int) -> NetwPromise:
	held = NetwPromise.new()
	return held

@rpc("authority", "call_local", "reliable")
func everyone(tag: int) -> int:
	return tag * 2

@rpc("authority", "call_remote", "reliable")
func others(tag: int) -> int:
	return tag * 2
)";

constexpr const char *SIGNAL_SINK = R"(extends Node

signal changed(next: int)

func _init() -> void:
	Netw.configure_signal(changed).call_remote()
)";

constexpr const char *LAMBDA_SINK = R"(extends Node

var declared := true

func _init() -> void:
	declared = Netw.configure_rpc(func(): pass) != null
)";

constexpr const char *CALL_SINK = R"(extends Node

signal rang(tag: int)

var calls := 0
var level := 0

func _init() -> void:
	Netw.configure_signal(rang)

@rpc("any_peer", "call_remote", "reliable")
func note_call(tag: int) -> void:
	calls += 1

@rpc("any_peer", "call_remote", "reliable")
func double_tag(tag: int) -> int:
	calls += 1
	return tag * 2
)";

Node *sink_of(const char *p_source, const char *p_name = "Sink") {
    const Ref<Script> script = netw_test::script_from(p_source);
    REQUIRE(script.is_valid());
    Node *made = Object::cast_to<Node>(script->call("new"));
    REQUIRE(made != nullptr);
    made->set_name(p_name);
    return made;
}

Array tagged(int p_tag) {
    Array args;
    args.push_back(p_tag);
    return args;
}

struct Star {
    LoopbackRig rig;
    Node *nodes[3] = {nullptr, nullptr, nullptr};
    RID at_hub;
    Vector<Node *> owned;

    Star(const char *p_hub, const char *p_first, const char *p_second)
        : rig(2) {
        rig.mount();
        const char *sources[3] = {p_hub, p_first, p_second};
        at_hub = seat(sources, "Sink", nodes);
    }

    explicit Star(const char *p_source) : Star(p_source, p_source, p_source) {
    }

    ~Star() {
        for (Node *node : owned) {
            if (node->get_parent() != nullptr) {
                node->get_parent()->remove_child(node);
            }
            memdelete(node);
        }
    }

    RID seat(const char *p_sources[3], const char *p_name, Node *r_nodes[3]) {
        for (int side = -1; side < 2; ++side) {
            r_nodes[side + 1] = sink_of(p_sources[side + 1], p_name);
            owned.push_back(r_nodes[side + 1]);
            rig.branch(side)->add_child(r_nodes[side + 1]);
        }
        NetwMultiplayer *server = rig.server();
        const RID made = server->entity_create();
        const int64_t route = server->entity_admit(made);
        REQUIRE(route > 0);
        REQUIRE(server->entity_bind_node(made, r_nodes[0]) == OK);
        for (int index = 0; index < 2; ++index) {
            NetwMultiplayer *client = rig.client(index);
            const RID here = client->entity_create();
            REQUIRE(client->entity_bind_route(here, route) == OK);
            REQUIRE(client->entity_bind_node(here, r_nodes[index + 1]) == OK);
        }
        rig.pump(4);
        return made;
    }

    Node *at(int p_side) const {
        return nodes[p_side + 1];
    }

    int read(int p_side, const char *p_name) const {
        return int(at(p_side)->get(StringName(p_name)));
    }

    Callable method(int p_side, const char *p_name) const {
        return Callable(at(p_side), StringName(p_name));
    }
};

struct World {
    String label;
};

typedef netw_test::LawRowFor<World> Row;

LawVerdict call_local_declared(const World &) {
    const FaultWatch faults;
    Star star(LOCAL_SINK);
    Netw::rpc(star.method(-1, "note_call"), tagged(5));
    star.rig.pump(6);

    for (int side = -1; side < 2; ++side) {
        if (star.read(side, "calls") != 1) {
            return law_broken(
                "peer %d ran it %d times",
                side,
                star.read(side, "calls")
            );
        }
    }
    if (star.read(0, "last_tag") != 5) {
        return law_broken("client 0 read tag %d", star.read(0, "last_tag"));
    }
    if (faults.warnings_containing("declares both") != 0) {
        return law_broken("call_local was read as two settings");
    }
    return law_held();
}

LawVerdict controller_declared(const World &) {
    const FaultWatch faults;
    Star star(CONTROLLER_SINK);
    star.rig.server()->entity_grant_control(star.at_hub, star.rig.peer_id(0));
    star.rig.pump(4);

    Netw::rpc_id(1, star.method(0, "note_call"), tagged(7));
    star.rig.pump(6);
    if (star.read(-1, "calls") != 1 || star.read(-1, "last_tag") != 7) {
        return law_broken(
            "the controller's call ran %d times at the server",
            star.read(-1, "calls")
        );
    }
    if (faults.warnings_containing("[NW0021]") != 0) {
        return law_broken("the controller's call was reported");
    }

    Netw::rpc_id(1, star.method(1, "note_call"), tagged(7));
    star.rig.pump(6);
    if (star.read(-1, "calls") != 1) {
        return law_broken("a client that does not control it ran the call");
    }
    if (faults.warnings_containing("[NW0021]") != 1) {
        return law_broken(
            "the refused call was reported %d times",
            int(faults.warnings_containing("[NW0021]"))
        );
    }
    return law_held();
}

LawVerdict request_to_a_client(const World &) {
    Star star(SINK);
    const Ref<NetwPromise> asked = Netw::request_id(
        star.rig.peer_id(1),
        star.method(0, "double_tag"),
        tagged(21)
    );
    star.rig.pump(8);

    if (asked.is_null() || !asked->get_is_completed()) {
        return law_broken("the request never completed");
    }
    if (int(asked->get_result()) != 42) {
        return law_broken("the request answered %d", int(asked->get_result()));
    }
    if (star.read(1, "last_sender") != star.rig.peer_id(0)) {
        return law_broken(
            "the answering client read sender %d",
            star.read(1, "last_sender")
        );
    }
    if (star.read(-1, "calls") != 0) {
        return law_broken("the server ran a request addressed to a client");
    }
    return law_held();
}

LawVerdict node_argument(const World &) {
    Star star(SINK);
    const char *plain[3] = {SINK, SINK, SINK};
    Node *targets[3] = {nullptr, nullptr, nullptr};
    star.seat(plain, "Target", targets);

    Array args;
    args.push_back(targets[1]);
    star.rig.client(0)->rpc_call(star.method(0, "point_at"), args, 0);
    star.rig.pump(6);

    const Object *at_server = star.at(-1)->get(StringName("pointed"));
    if (at_server != targets[0]) {
        return law_broken("the server's handler read another node");
    }
    const Object *at_client = star.at(1)->get(StringName("pointed"));
    if (at_client != targets[2]) {
        return law_broken("client 1's handler read another node");
    }
    return law_held();
}

struct Refusal {
    const char *hub;
    const char *second;
    bool group;
    const char *method;
    Error code;
    const char *detail;
    const char *receiver_warning;
};

LawVerdict refused_request(const Refusal &p_row) {
    FaultWatch faults;
    if (p_row.receiver_warning != nullptr) {
        faults.expect_warning(p_row.receiver_warning, 1);
    }
    Star star(p_row.hub, ANSWERING_SINK, p_row.second);
    const int caller = p_row.group ? -1 : 0;
    const Callable method = star.method(caller, p_row.method);
    const Ref<NetwPromise> single
        = p_row.group ? Ref<NetwPromise>() : Netw::request(method, tagged(3));
    const Ref<NetwGroupPromise> group = p_row.group
        ? Netw::request_all(method, tagged(3))
        : Ref<NetwGroupPromise>();
    const CallLog caught;
    if (group.is_valid()) {
        group->catch_error(caught.callable(StringName("caught")));
    }
    star.rig.pump(6);

    if (single.is_valid()) {
        if (!single->get_is_failed() || single->get_code() != p_row.code) {
            return law_broken(
                "the request answered code %d",
                int(single->get_code())
            );
        }
        if (p_row.detail != nullptr
            && single->get_detail() != String(p_row.detail)) {
            return law_broken("the reason did not reach the caller");
        }
        return law_held();
    }
    if (group.is_null() || !group->get_is_failed()
        || group->get_code() != p_row.code) {
        return law_broken("the group did not fail with the refusal");
    }
    if (caught.count(StringName("caught")) != 1) {
        return law_broken("the group's failure was not reported once");
    }
    const Dictionary results = group->get_results();
    if (results.size() != 1 || int(results.get(star.rig.peer_id(0), 0)) != 6) {
        return law_broken("the answering peer's value did not join the group");
    }
    return law_held();
}

LawVerdict refused_by_sender_rule(const World &) {
    return refused_request(
        {ANSWERING_SINK,
         ANSWERING_SINK,
         false,
         "guarded",
         ERR_UNAUTHORIZED,
         nullptr,
         "*'guarded' on 'Sink'*[NW0021]*"}
    );
}

LawVerdict refused_not_an_rpc(const World &) {
    return refused_request(
        {UNDECLARED_SINK,
         ANSWERING_SINK,
         false,
         "echo",
         ERR_METHOD_NOT_FOUND,
         nullptr,
         "*'Sink'*[NW0025]*"}
    );
}

LawVerdict refused_by_its_promise(const World &) {
    return refused_request(
        {ANSWERING_SINK,
         ANSWERING_SINK,
         false,
         "closed",
         ERR_BUSY,
         "the shop is closed",
         nullptr}
    );
}

LawVerdict refused_in_a_group(const World &) {
    return refused_request(
        {ANSWERING_SINK,
         UNDECLARED_SINK,
         true,
         "echo",
         ERR_METHOD_NOT_FOUND,
         nullptr,
         "*'Sink'*[NW0025]*"}
    );
}

LawVerdict declared_timeout(const World &) {
    Star star(DEADLINE_SINK);
    const Ref<NetwPromise> asked
        = Netw::request(star.method(0, "patient"), tagged(3));
    star.rig.pump(300);
    if (asked->get_is_settled()) {
        return law_broken("the request settled before its declared timeout");
    }
    const Ref<NetwPromise> held = star.at(-1)->get("held");
    if (held.is_null()) {
        return law_broken("the server never ran the method");
    }
    held->resolve(9);
    star.rig.pump(4);
    if (!asked->get_is_completed() || int(asked->get_result()) != 9) {
        return law_broken("the late answer did not reach the caller");
    }
    return law_held();
}

LawVerdict default_timeout(const World &) {
    {
        Star star(DEADLINE_SINK);
        const Ref<NetwPromise> asked
            = Netw::request(star.method(0, "hasty"), tagged(3));
        star.rig.pump(135);
        if (asked->get_is_settled()) {
            return law_broken("the request settled before 5 seconds");
        }
    }
    Star star(DEADLINE_SINK);
    const Ref<NetwPromise> asked
        = Netw::request(star.method(0, "hasty"), tagged(3));
    star.rig.pump(165);
    if (!asked->get_is_failed() || asked->get_code() != ERR_TIMEOUT) {
        return law_broken("the request did not time out after 5 seconds");
    }
    return law_held();
}

LawVerdict timeout_misused(const World &) {
    FaultWatch faults;
    faults.expect_warning("*'hp'*[NW0038]*", 1);
    faults.expect_warning("*'others'*[NW0038]*", 1);
    faults.expect_error("*'hasty'*[NW0032]*", 1);
    Node *sink = sink_of(DEADLINE_SINK);
    Netw::configure_property(sink, StringName("hp"))->timeout(2.0);
    Netw::configure_spawn(Callable(sink, StringName("others")))->timeout(2.0);
    Netw::configure_rpc(Callable(sink, StringName("hasty")))->timeout(-1.0);
    memdelete(sink);
    return law_held();
}

LawVerdict request_all_asks_caller(const World &) {
    const char *methods[2] = {"everyone", "others"};
    for (int at = 0; at < 2; ++at) {
        const bool asks_caller = at == 0;
        Star star(DEADLINE_SINK);
        const Ref<NetwGroupPromise> group
            = Netw::request_all(star.method(-1, methods[at]), tagged(3));
        star.rig.pump(6);
        if (!group->get_is_completed()) {
            return law_broken("request_all of %s never completed", methods[at]);
        }
        const Dictionary results = group->get_results();
        if (results.size() != (asks_caller ? 3 : 2)
            || results.has(1) != asks_caller) {
            return law_broken(
                "request_all of %s answered %d values",
                methods[at],
                int(results.size())
            );
        }
        for (int side = 0; side < 2; ++side) {
            if (int(results.get(star.rig.peer_id(side), 0)) != 6) {
                return law_broken(
                    "client %d's value did not join under its id",
                    side
                );
            }
        }
        if (asks_caller && int(results.get(1, 0)) != 6) {
            return law_broken("the caller's own value did not join under 1");
        }
    }
    return law_held();
}

LawVerdict unsteered_controller(const World &) {
    Star star(SINK);
    if (NetwEntity::of(star.at(0))->get_controller() != 1) {
        return law_broken(
            "an unsteered entity names controller %d",
            int(NetwEntity::of(star.at(0))->get_controller())
        );
    }

    Netw::rpc_controller(star.method(0, "note_call"), tagged(3));
    const Ref<NetwPromise> asked
        = Netw::request_controller(star.method(0, "double_tag"), tagged(3));
    star.rig.pump(6);
    if (star.read(-1, "calls") != 2
        || star.read(-1, "last_sender") != star.rig.peer_id(0)) {
        return law_broken(
            "the server ran %d of a client's two calls",
            star.read(-1, "calls")
        );
    }
    if (asked.is_null() || int(asked->get_result()) != 6) {
        return law_broken("the client's request did not answer");
    }

    Netw::rpc_controller(star.method(-1, "note_call"), tagged(3));
    const Ref<NetwPromise> own
        = Netw::request_controller(star.method(-1, "double_tag"), tagged(3));
    if (star.read(-1, "calls") != 4) {
        return law_broken("the server's own calls did not run at once");
    }
    if (own.is_null() || !own->get_is_completed()
        || int(own->get_result()) != 6) {
        return law_broken("the server's own request did not answer at once");
    }
    if (star.read(0, "calls") != 0) {
        return law_broken("a call to the controller ran on a client");
    }
    return law_held();
}

LawVerdict bad_declaration(const World &) {
    Node *sink = sink_of(SIGNAL_SINK);
    {
        const FaultWatch faults;
        if (Netw::configure_rpc(Callable(sink, StringName("nope"))).is_valid()
            || faults.errors_containing("'nope'") != 1) {
            memdelete(sink);
            return law_broken("a missing method was declared or not reported");
        }
    }
    {
        const FaultWatch faults;
        Node *declarer = sink_of(LAMBDA_SINK);
        const bool declared = declarer->get(StringName("declared"));
        memdelete(declarer);
        if (declared || faults.errors() != 1) {
            memdelete(sink);
            return law_broken("a lambda was declared or not reported");
        }
    }
    {
        const FaultWatch faults;
        if (Netw::configure_spawn(Callable(sink, StringName("nope"))).is_valid()
            || faults.errors() != 1) {
            memdelete(sink);
            return law_broken("a missing spawn function was declared");
        }
    }
    {
        const FaultWatch faults;
        if (Netw::configure_signal(Signal(sink, StringName("nope"))).is_valid()
            || faults.errors_containing("'nope'") != 1) {
            memdelete(sink);
            return law_broken("a missing signal was declared or not reported");
        }
    }
    memdelete(sink);
    return law_held();
}

LawVerdict mounted_declaration(const World &) {
    const Ref<Script> script = netw_test::minted_script(
        "extends Node\nfunc hit() -> void:\n\tpass\n"
    );
    Node *node = Object::cast_to<Node>(script->call("new"));
    node->set_name("LateRpc");
    netw::gd::scene_root()->add_child(node);
    FaultWatch faults;
    faults.expect_error("*'LateRpc'*[NW0036]*", 1);
    const bool declared = Netw::configure_rpc(Callable(node, "hit")).is_valid();
    node->get_parent()->remove_child(node);
    memdelete(node);
    if (declared) {
        return law_broken("a mounted node declared an RPC");
    }
    return law_held();
}

const Row ROWS[] = {
    {"an RPC declared call_local, called by the server",
     "it runs once on the server and once on every client, with no warning",
     &call_local_declared},
    {"an RPC declared controller, called by the controller and by another "
     "client",
     "the controller's call runs at the server, the other is refused and "
     "reported once",
     &controller_declared},
    {"a request one client addresses to another",
     "it settles with that client's answer, which reads the caller's id",
     &request_to_a_client},
    {"a call whose argument is a node of an entity",
     "each receiver's handler reads its own copy of that node",
     &node_argument},
    {"a request to a method whose sender rule refuses the caller",
     "it fails with ERR_UNAUTHORIZED and the receiver warns",
     &refused_by_sender_rule},
    {"a request to a method that is not an RPC on the receiver",
     "it fails with ERR_METHOD_NOT_FOUND and the receiver warns",
     &refused_not_an_rpc},
    {"a request to a method whose promise rejects",
     "it fails with the method's code and reason",
     &refused_by_its_promise},
    {"request_all where one peer does not declare the method",
     "the group fails once with that code and keeps the other answer",
     &refused_in_a_group},
    {"a request to a method declared with timeout(60), answered at 10 "
     "seconds",
     "it is still waiting and takes the late answer",
     &declared_timeout},
    {"a request to a method with no timeout declared",
     "it waits at 4.5 seconds and fails with ERR_TIMEOUT at 5.5",
     &default_timeout},
    {"timeout on a property, on a spawn function, and below zero",
     "the first two warn that it does nothing and the third is refused",
     &timeout_misused},
    {"request_all of a call_local method and of a call_remote one",
     "the caller answers too only for call_local, under peer 1",
     &request_all_asks_caller},
    {"rpc_controller and request_controller on an entity no peer steers",
     "they reach the server from a client and run at once on the server",
     &unsteered_controller},
    {"configure_rpc, configure_spawn and configure_signal naming nothing, "
     "and configure_rpc on a lambda",
     "each reports one error and declares nothing",
     &bad_declaration},
    {"configure_rpc on a node already in the tree",
     "it is reported and declares nothing",
     &mounted_declaration},
};

TEST_CASE(
    "[Networked][Rpc][SceneTree] an RPC runs where its declaration says, "
    "once, under the caller's id, and a request always answers"
) {
    const World world = {String("two clients and a server on a loopback")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, world);
        NETW_LAW_HOLDS(row, world);
    }
}

Array one_tag() {
    return tagged(3);
}

Variant via_rpc(Node *p_node) {
    Netw::rpc(Callable(p_node, "note_call"), one_tag());
    return Variant();
}

Variant via_rpc_id(Node *p_node) {
    Netw::rpc_id(1, Callable(p_node, "note_call"), one_tag());
    return Variant();
}

Variant via_rpc_controller(Node *p_node) {
    Netw::rpc_controller(Callable(p_node, "note_call"), one_tag());
    return Variant();
}

Variant via_request(Node *p_node) {
    return Netw::request(Callable(p_node, "double_tag"), one_tag());
}

Variant via_request_id(Node *p_node) {
    return Netw::request_id(1, Callable(p_node, "double_tag"), one_tag());
}

Variant via_request_all(Node *p_node) {
    return Netw::request_all(Callable(p_node, "double_tag"), one_tag());
}

Variant via_request_controller(Node *p_node) {
    return Netw::request_controller(Callable(p_node, "double_tag"), one_tag());
}

Variant via_rpc_call(Node *p_node) {
    Netw::of(p_node)->rpc_call(Callable(p_node, "note_call"), one_tag(), 0);
    return Variant();
}

Variant via_rpc_request_call(Node *p_node) {
    return Netw::of(p_node)
        ->rpc_request_call(1, Callable(p_node, "double_tag"), one_tag(), 5.0);
}

Variant via_rpc_request_call_group(Node *p_node) {
    return Netw::of(p_node)->rpc_request_call_group(
        Callable(p_node, "double_tag"),
        one_tag(),
        5.0
    );
}

Variant via_entity_call(Node *p_node) {
    const Ref<NetwMultiplayer> session = Netw::of(p_node);
    return int64_t(session->entity_call(
        session->entity_of(p_node),
        0,
        StringName("note_call"),
        one_tag(),
        0
    ));
}

Variant via_sync_property(Node *p_node) {
    Netw::sync_property(p_node, StringName("level"));
    return Variant();
}

Variant via_emit_entity_signal(Node *p_node) {
    Netw::emit_entity_signal(Signal(p_node, StringName("rang")), one_tag());
    return Variant();
}

struct Door {
    const char *label;
    bool names_an_entity;
    bool answers;
    bool signal;
    Variant (*call)(Node *p_node);
};

const Door DOORS[] = {
    {"Netw.rpc", false, false, false, &via_rpc},
    {"Netw.rpc_id", false, false, false, &via_rpc_id},
    {"Netw.rpc_controller", false, false, false, &via_rpc_controller},
    {"Netw.request", false, true, false, &via_request},
    {"Netw.request_id", false, true, false, &via_request_id},
    {"Netw.request_all", false, true, false, &via_request_all},
    {"Netw.request_controller", false, true, false, &via_request_controller},
    {"rpc_call", false, false, false, &via_rpc_call},
    {"rpc_request_call", false, true, false, &via_rpc_request_call},
    {"rpc_request_call_group", false, true, false, &via_rpc_request_call_group},
    {"entity_call", true, true, false, &via_entity_call},
    {"Netw.sync_property", false, false, false, &via_sync_property},
    {"Netw.emit_entity_signal", false, false, true, &via_emit_entity_signal},
};

struct Fault {
    const char *label;
    int client;
    bool configured;
    Error code;
    const char *text;
};

const Fault FAULTS[] = {
    {"a node in no entity", -1, false, ERR_UNCONFIGURED, "[NW1010]"},
    {"an entity not live yet", 0, true, ERR_UNAVAILABLE, "[NW0037]"},
};

struct Answer {
    bool answered = false;
    bool failed = false;
    int64_t code = OK;
};

Answer answer_of(const Variant &p_answer) {
    Answer out;
    if (p_answer.get_type() == Variant::INT) {
        out.answered = true;
        out.code = int64_t(p_answer);
        out.failed = out.code != OK;
        return out;
    }
    Object *held = p_answer;
    NetwPromise *single = Object::cast_to<NetwPromise>(held);
    NetwGroupPromise *group = Object::cast_to<NetwGroupPromise>(held);
    if (single != nullptr) {
        out.answered = true;
        out.failed = single->get_is_failed();
        out.code = single->get_code();
    } else if (group != nullptr) {
        out.answered = true;
        out.failed = group->get_is_failed();
        out.code = group->get_code();
    }
    return out;
}

TEST_CASE(
    "[Networked][Rpc][SceneTree] a call no peer can route fails at the call "
    "through every door, is reported once per node, and a request answers "
    "a failed promise"
) {
    for (const Fault &fault : FAULTS) {
        LoopbackRig rig(1);
        rig.mount();
        for (const Door &door : DOORS) {
            if (door.names_an_entity && !fault.configured) {
                continue;
            }
            const String label = vformat("%s x %s", fault.label, door.label);
            NETW_FORMAT_TEXT(netw_cell_text, label.utf8().get_data());
            CAPTURE(netw_cell_text);

            Node *node = netw_test::scripted_root(
                CALL_SINK,
                String(door.label).replace(".", "_")
            );
            if (fault.configured) {
                Netw::configure_entity(node);
            }
            rig.branch(fault.client)->add_child(node);
            CallLog heard;
            node->connect(StringName("rang"), heard.callable("rang"));
            Answer first;
            Answer second;
            int64_t reported = 0;
            {
                FaultWatch faults;
                faults.expect_error(String("*") + fault.text + String("*"), 1);
                first = answer_of(door.call(node));
                second = answer_of(door.call(node));
                reported = faults.errors_containing(fault.text);
            }
            NETW_CHECK_EQ(reported, int64_t(1));
            if (door.answers) {
                CHECK(first.answered);
                CHECK(second.answered);
                CHECK(second.failed);
                NETW_CHECK_EQ(first.code, int64_t(fault.code));
            }
            if (door.signal) {
                NETW_CHECK_EQ(heard.count("rang"), 2);
            }
            node->get_parent()->remove_child(node);
            memdelete(node);
        }
    }
}

} // namespace TestRpc

#endif
