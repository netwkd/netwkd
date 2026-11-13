#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/context.hpp"
#include "netw/api/lag_compensation_config.hpp"
#include "netw/api/session_config.hpp"

namespace TestService {

using namespace godot;
using netw_test::EntityDecl;
using netw_test::FaultWatch;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;
using netw_test::WorldDecl;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

RID target(LoopbackRig &p_rig) {
    WorldDecl world;
    world.clocked(60, 0);
    p_rig.declare_world(world);
    return p_rig.declare_entity(
        EntityDecl()
            .named("Target")
            .on_schema("target")
            .synced(StringName("position"))
            .placed_at(Vector2(4.0, 0.0))
    );
}

Node *child_of(Node *p_parent, const char *p_name) {
    Node *made = memnew(Node);
    made->set_name(StringName(p_name));
    p_parent->add_child(made);
    return made;
}

LawVerdict rival_lagcomp(const Stand &) {
    FaultWatch watch;
    watch.expect_error(
        "*Netw.configure_lagcomp*/root/RigServer/First*[NW0034]*",
        1
    );
    LoopbackRig rig(0);
    rig.mount();
    const RID entity = target(rig);
    netw::Netw::configure_lagcomp(child_of(rig.branch(), "First"));
    netw::Netw::configure_lagcomp(child_of(rig.branch(), "Second"));

    const Error declared = rig.server()->predict_declare(entity);
    if (declared != ERR_ALREADY_EXISTS) {
        return law_broken("predict_declare answered %d", int(declared));
    }
    if (rig.server()->predict_is_declared(entity)) {
        return law_broken("two rival settings still seated prediction");
    }
    return law_held();
}

LawVerdict lagcomp_node_left(const Stand &) {
    FaultWatch watch;
    watch.expect_error("*'First'*[NW0041]*", 1);
    LoopbackRig rig(0);
    rig.mount();
    const RID entity = target(rig);
    Node *first = child_of(rig.branch(), "First");
    netw::Netw::configure_lagcomp(first);
    rig.branch()->remove_child(first);

    const Error declared = rig.server()->predict_declare(entity);
    const bool seated = rig.server()->predict_is_declared(entity);
    memdelete(first);
    if (declared != ERR_UNAVAILABLE) {
        return law_broken("predict_declare answered %d", int(declared));
    }
    if (seated) {
        return law_broken("settings whose node left still seated prediction");
    }
    return law_held();
}

LawVerdict configuration_door(const Stand &) {
    FaultWatch watch;
    watch.expect_error("*A NetwClockConfig*[NW1020]*", 1);
    watch.expect_error("*A NetwSessionConfig*[NW1020]*", 1);
    watch.expect_error("*A NetwLagCompensationConfig*[NW1020]*", 1);
    LoopbackRig rig(1);
    MultiplayerAPI *api = rig.server();

    Ref<netw::NetwClockConfig> clock;
    clock.instantiate();
    clock->set("tickrate", 31);
    Ref<netw::NetwSessionConfig> session;
    session.instantiate();
    Ref<netw::NetwLagCompensationConfig> lagcomp;
    lagcomp.instantiate();
    const Ref<RefCounted> configs[] = {clock, session, lagcomp};

    Node *plain = memnew(Node);
    int accepted = 0;
    for (const Ref<RefCounted> &config : configs) {
        accepted += api->object_configuration_add(plain, config)
            != ERR_UNAVAILABLE;
        accepted += api->object_configuration_remove(plain, config)
            != ERR_UNAVAILABLE;
    }
    memdelete(plain);
    if (accepted > 0) {
        return law_broken("%d calls were not refused", accepted);
    }
    if (rig.server()->lagcomp_is_configured()) {
        return law_broken("a refused config turned lag compensation on");
    }
    return law_held();
}

const Row ROWS[] = {
    {"two nodes call Netw.configure_lagcomp, then an entity is predicted",
     "predict_declare answers ERR_ALREADY_EXISTS, NW0034 names the first "
     "node, and nothing is predicted",
     &rival_lagcomp},
    {"the node that called Netw.configure_lagcomp leaves the tree",
     "predict_declare answers ERR_UNAVAILABLE with NW0041",
     &lagcomp_node_left},
    {"object_configuration_add with a clock, session or lag compensation "
     "config",
     "each add and remove is refused with NW1020 naming the config",
     &configuration_door},
};

TEST_CASE(
    "[Networked][Services] lag compensation and session settings that "
    "cannot apply are refused with the reason"
) {
    const Stand stand = {String("a server session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestService

#endif
