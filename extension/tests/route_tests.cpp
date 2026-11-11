#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/loopback_rig.h"
#include "support/netw_cells.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/spatial_node.hpp"
#include "godot/templates.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"

namespace TestRoute {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 30;
constexpr int EACH = 40;
constexpr int SERVER = -1;
constexpr const char *CRATE_ID = "route_crate";

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

Node *build_crate(const Variant &p_name) {
    Node3D *made = memnew(Node3D);
    made->set_name(String(p_name));
    Netw::configure_entity(made)->set_lifecycle(
        NetwEntity::LIFECYCLE_CONTROLLER
    );
    return made;
}

Array one_string(const String &p_value) {
    Array out;
    out.push_back(p_value);
    return out;
}

Array string_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

struct Crates {
    LoopbackRig rig;
    int minted = 0;

    explicit Crates(int p_clients) : rig(p_clients) {
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        rig.mirror_child("Arena");
        for (int side = SERVER; side < rig.count(); ++side) {
            teach(side);
        }
        rig.step_ticks(4);
    }

    NetwMultiplayer *side(int p_side) const {
        return p_side < 0 ? rig.server() : rig.client(p_side);
    }

    void teach(int p_side) {
        rig.register_constructor(
            side(p_side),
            StringName(CRATE_ID),
            callable_mp_static(&build_crate),
            string_type()
        );
    }

    int spawn(int p_side) {
        NetwMultiplayer *author = side(p_side);
        const RID made = author->spawn_registered(
            StringName(CRATE_ID),
            one_string(String("Crate") + String::num_int64(minted++)),
            nullptr
        );
        Node *node = author->entity_get_node(made);
        if (node == nullptr) {
            return 0;
        }
        rig.branch(p_side)
            ->get_node_or_null(NodePath("Arena"))
            ->add_child(node);
        return int(author->entity_get_route(made));
    }

    int seated_at_server(const HashSet<int> &p_routes) const {
        HashSet<Node *> nodes;
        for (const int route : p_routes) {
            Node *held = rig.route_node(route, SERVER);
            if (held != nullptr) {
                nodes.insert(held);
            }
        }
        return int(nodes.size());
    }
};

LawVerdict many_authors(const Stand &) {
    Crates crates(3);
    HashSet<int> routes;
    int refused = 0;
    for (int round = 0; round < EACH; ++round) {
        for (int author = SERVER; author < crates.rig.count(); ++author) {
            const int route = crates.spawn(author);
            refused += route > 0 ? 0 : 1;
            routes.insert(route);
        }
        crates.rig.step_ticks(2);
    }
    crates.rig.step_ticks(8);

    if (refused != 0) {
        return law_broken("%d spawns were refused", refused);
    }
    if (int(routes.size()) != EACH * 4) {
        return law_broken(
            "%d routes for %d spawns",
            int(routes.size()),
            EACH * 4
        );
    }
    const int seated = crates.seated_at_server(routes);
    if (seated != EACH * 4) {
        return law_broken("the server holds %d of %d crates", seated, EACH * 4);
    }
    return law_held();
}

LawVerdict late_author(const Stand &) {
    Crates crates(1);
    int greatest = 0;
    for (int at = 0; at < EACH; ++at) {
        const int route = crates.spawn(SERVER);
        greatest = route > greatest ? route : greatest;
    }
    crates.rig.step_ticks(4);

    const int late = crates.rig.late_client(StringName("late"));
    crates.teach(late);
    crates.rig.mirror_late(late, "Arena");
    crates.rig.step_ticks(8);
    const int first = crates.spawn(late);
    crates.rig.step_ticks(8);

    if (first <= greatest) {
        return law_broken(
            "the late client spawned on %d, under %d",
            first,
            greatest
        );
    }
    if (crates.rig.route_node(first, SERVER) == nullptr) {
        return law_broken("the server never seated the late client's crate");
    }
    if (crates.spawn(SERVER) <= first) {
        return law_broken("the server's next spawn is under the client's");
    }
    return law_held();
}

const Row ROWS[] = {
    {"three clients and the server each spawning forty crates",
     "every crate has its own route and the server seats all of them",
     &many_authors},
    {"a client joining after the server spawned forty crates",
     "its first crate is on a route above all of them, the server seats it, "
     "and the server's next crate is above it",
     &late_author},
};

TEST_CASE(
    "[Networked][Route][SceneTree] every entity has its own route, whoever "
    "spawned it and whenever they joined"
) {
    const Stand stand = {String("a loopback session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestRoute

#endif
