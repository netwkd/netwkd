#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include <godot_cpp/classes/script.hpp>

#include "godot/node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/member_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/script/model.hpp"
#include "support/fault_log.h"
#include "support/minted_script.h"

namespace TestRpcDeclaredConfigLaws {

using namespace godot;
using namespace netw_test;

constexpr const char *SERVER_DECLARED_SINK = R"(extends Node

var calls: int = 0
var last_tag: int = 0

func _init() -> void:
	Netw.configure_rpc(note_call).server()

func note_call(tag: int) -> void:
	calls += 1
	last_tag = tag
)";

struct Seated {
    Node *hub = nullptr;
    Node *first = nullptr;
    Node *second = nullptr;
    RID at_hub;
};

Node *a_sink(const Ref<Script> &p_script) {
    Node *made = Object::cast_to<Node>(p_script->call("new"));
    REQUIRE(made != nullptr);
    made->set_name("Sink");
    return made;
}

Seated seat_star(LoopbackRig &p_rig, const Ref<Script> &p_script) {
    Seated made;
    made.hub = a_sink(p_script);
    made.at_hub = p_rig.server()->entity_create();
    const int64_t route = p_rig.server()->entity_admit(made.at_hub);
    REQUIRE(route > 0);
    NETW_CHECK_EQ(
        int(p_rig.server()->entity_bind_node(made.at_hub, made.hub)),
        int(OK)
    );

    p_rig.branch(-1)->add_child(made.hub);
    made.hub->set_owner(p_rig.branch(-1));
    Node *seats[2] = {nullptr, nullptr};
    for (int index = 0; index < 2; ++index) {
        seats[index] = a_sink(p_script);
        const RID here = p_rig.client(index)->entity_create();
        NETW_CHECK_EQ(
            int(p_rig.client(index)->entity_bind_route(here, route)),
            int(OK)
        );
        NETW_CHECK_EQ(
            int(p_rig.client(index)->entity_bind_node(here, seats[index])),
            int(OK)
        );
        p_rig.branch(index)->add_child(seats[index]);
    }
    made.first = seats[0];
    made.second = seats[1];
    p_rig.pump(4);
    return made;
}

void retire_star(const Seated &p_seated) {
    for (Node *node : {p_seated.hub, p_seated.first, p_seated.second}) {
        if (node != nullptr && node->get_parent() != nullptr) {
            node->get_parent()->remove_child(node);
        }
        memdelete(node);
    }
}

int calls_at(Node *p_node) {
    return int(p_node->get(StringName("calls")));
}

TEST_CASE(
    "[Networked][Rpc] RD3 an RPC declared server through Netw.configure_rpc "
    "refuses the client that controls the entity, warns once however often "
    "it asks, and runs everywhere the server sends it"
) {
    LoopbackRig rig(2);
    rig.mount();
    const FaultWatch faults;
    const Ref<Script> script = script_from(SERVER_DECLARED_SINK);
    REQUIRE(script.is_valid());
    const Seated seated = seat_star(rig, script);
    rig.server()->entity_grant_control(seated.at_hub, rig.peer_id(0));
    rig.pump(4);

    Array args;
    args.push_back(9);
    for (int asked = 0; asked < 3; ++asked) {
        netw::Netw::rpc_id(
            1,
            Callable(seated.first, StringName("note_call")),
            args
        );
    }
    rig.pump(6);

    NETW_CHECK_EQ(calls_at(seated.hub), 0);
    NETW_CHECK_EQ(
        faults.warnings_containing(String("[NW0021]")),
        int64_t(1)
    );

    netw::Netw::rpc(Callable(seated.hub, StringName("note_call")), args);
    rig.pump(6);

    NETW_CHECK_EQ(calls_at(seated.first), 1);
    NETW_CHECK_EQ(calls_at(seated.second), 1);
    NETW_CHECK_EQ(int(seated.second->get(StringName("last_tag"))), 9);

    const Ref<netw::NetwMemberConfig> declared
        = netw::script::model::get_rpc_options(script, StringName("note_call"));
    REQUIRE(declared.is_valid());
    CHECK(declared->is_policy_declared());
    NETW_CHECK_EQ(int(declared->get_write_policy()), 3);

    retire_star(seated);
}

} // namespace TestRpcDeclaredConfigLaws

#endif
