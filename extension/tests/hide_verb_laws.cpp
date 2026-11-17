#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/interest_layer.hpp"
#include "netw/api/lag_compensation_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/interest/decl.hpp"

#include <godot_cpp/classes/node2d.hpp>

namespace TestHideVerbLaws {

using namespace godot;
using namespace netw_test;
using netw::NetwEntity;
using netw::NetwMultiplayer;

const char *SHOWN_ID = "shown_body";

Node *build_shown_body(const Variant &p_name) {
    Node2D *made = memnew(Node2D);
    made->set_name(String(p_name));
    return made;
}

Array named(const char *p_name) {
    Array out;
    out.push_back(String(p_name));
    return out;
}

Array one_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

int a_body_the_client_holds(LoopbackRig &p_rig, Node *p_parent) {
    p_rig.register_constructor(
        p_rig.server(),
        StringName(SHOWN_ID),
        callable_mp_static(&build_shown_body),
        one_type()
    );
    for (int at = 0; at < p_rig.count(); ++at) {
        p_rig.register_constructor(
            p_rig.client(at),
            StringName(SHOWN_ID),
            callable_mp_static(&build_shown_body),
            one_type()
        );
    }
    const int route = p_rig.spawn_registered(
        StringName(SHOWN_ID),
        callable_mp_static(&build_shown_body),
        named("Shown"),
        one_type(),
        p_parent,
        0,
        false
    );
    p_rig.pump(10);
    REQUIRE(p_rig.route_node(route, 0) != nullptr);
    return route;
}

TEST_CASE(
    "[Netw][Liveness] HV4 a hidden body keeps the prediction "
    "declaration its game made, because a declaration is scoped to the "
    "identity and a hide loses only the body"
) {
    LoopbackRig rig(1);
    rig.mount();
    Node *arena = rig.mirror_child("Arena");
    const int route = a_body_the_client_holds(rig, arena);

    const Ref<NetwEntity> entity = NetwEntity::of(rig.route_node(route, 0));
    REQUIRE(entity.is_valid());
    NetwMultiplayer *held = rig.client(0);
    NETW_CHECK_EQ(int(held->lagcomp_initialize(8, 12)), int(godot::OK));
    NETW_CHECK_EQ(
        int(held->predict_declare(entity->get_rid_handle())),
        int(godot::OK)
    );
    const Ref<netw::NetwPrediction> handle = entity->get_prediction();
    REQUIRE(handle.is_valid());
    REQUIRE(handle->is_registered());

    entity->_remote_hide();

    CHECK(handle->is_registered());
}

} // namespace TestHideVerbLaws

#endif
