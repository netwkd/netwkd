#include "support/netw_test.h"

#if defined(NETW_TIER_HOSTED)

#include "support/fault_log.h"
#include "support/internals.h"
#include "support/loopback_rig.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"
#include "support/value_flow_stand.h"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/scene_tree.hpp"
#include "godot/spatial_node.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/interest_layer.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/simulation.hpp"

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/node3d.hpp>

namespace TestEntity {

using namespace godot;
using netw::Netw;
using netw::NetwEntity;
using netw::NetwInterestLayer;
using netw::NetwMultiplayer;
using netw_test::FaultWatch;
using netw_test::Internals;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::LoopbackRig;

constexpr int TICKRATE = 30;
constexpr int SETTLE = 6;
constexpr int FOREIGN = 5;
constexpr int SERVER = -1;
constexpr int CLIENT = 0;
constexpr int64_t LIVE = int64_t(NetwMultiplayer::ENTITY_STATE_LIVE);
constexpr int64_t ABSENT = int64_t(NetwMultiplayer::ENTITY_STATE_ABSENT);
constexpr const char *GATE_LAYER = "gate";

constexpr const char *CRATE_SOURCE = R"(extends Node3D

func _init() -> void:
	Netw.configure_entity(self)
	Netw.configure_property(self, &"position").broadcast().heartbeat(60)
)";

constexpr const char *HELD_CRATE_SOURCE = R"(extends Node3D

func _init() -> void:
	Netw.configure_entity(self).authority = NetwEntity.AUTHORITY_CONTROLLER
	Netw.configure_property(self, &"position").broadcast().heartbeat(60)
)";

struct Part {
    const char *name = nullptr;
    bool level = false;
    bool placed = false;
    bool instanced = false;
    bool started = false;
    double root_priority = 0.0;
    const char *part_name = nullptr;
    bool folds = false;
    const char *fault = nullptr;
    bool error = false;
};

struct Stand : Part {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

Array one_string(const char *p_value) {
    Array out;
    out.push_back(String(p_value));
    return out;
}

Array string_type() {
    Array out;
    out.push_back(int(Variant::STRING));
    return out;
}

int64_t route_of(NetwMultiplayer *p_api, Node *p_node) {
    const Ref<NetwEntity> entity = NetwEntity::of(p_node);
    return entity.is_null() ? 0
                            : p_api->entity_get_route(entity->get_rid_handle());
}

Node *build_scripted(const Variant &p_name, const Variant &p_script) {
    const Ref<Script> script = p_script;
    Node *made
        = Object::cast_to<Node>(netw::gd::live_object(script->call("new")));
    made->set_name(String(p_name));
    return made;
}

Node *spawn_crate(LoopbackRig &p_rig, const char *p_id, const char *p_source) {
    const Ref<Script> script = netw_test::minted_script(p_source);
    REQUIRE(script.is_valid());
    const int route = p_rig.spawn_registered(
        StringName(p_id),
        callable_mp_static(&build_scripted).bind(script),
        one_string("Crate"),
        string_type()
    );
    p_rig.step_ticks(SETTLE);
    Node *crate = p_rig.route_node(route, SERVER);
    REQUIRE(crate != nullptr);
    return crate;
}

Node *a_node(const char *p_name, int p_authority) {
    Node *made = memnew(Node);
    made->set_name(StringName(p_name));
    made->set_multiplayer_authority(p_authority, false);
    return made;
}

LawVerdict foreign_reset(bool p_player_controls) {
    FaultWatch faults;
    faults.expect_error("*[NW1012]*", 4);
    LoopbackRig rig(1);
    rig.mount();
    netw_test::flow_clocks(rig, TICKRATE);
    Node *crate = spawn_crate(rig, "authority_join_crate", CRATE_SOURCE);
    if (p_player_controls) {
        NetwEntity::of(crate)->set_controller(rig.peer_id(CLIENT));
    }
    if (crate->get_multiplayer_authority() != 1) {
        return law_broken("the crate left the server's authority");
    }

    Node *child = a_node("Child", FOREIGN);
    crate->add_child(child);
    Node *grandchild = a_node("Grandchild", FOREIGN);
    child->add_child(grandchild);
    Node *branch = a_node("Branch", FOREIGN);
    Node *leaf = a_node("Leaf", FOREIGN);
    branch->add_child(leaf);
    crate->add_child(branch);
    for (Node *joined : {child, grandchild, branch, leaf}) {
        if (joined->get_multiplayer_authority() != 1) {
            return law_broken(
                "'%s' kept its own authority",
                String(joined->get_name()).utf8().get_data()
            );
        }
    }
    return law_held();
}

LawVerdict foreign_under_server(const Stand &) {
    return foreign_reset(false);
}

LawVerdict foreign_under_player(const Stand &) {
    return foreign_reset(true);
}

LawVerdict joins_controller(const Stand &) {
    LoopbackRig rig(1);
    rig.mount();
    netw_test::flow_clocks(rig, TICKRATE);
    Node *crate = spawn_crate(rig, "authority_held_crate", HELD_CRATE_SOURCE);
    const int peer = rig.peer_id(CLIENT);
    NetwEntity::of(crate)->set_controller(peer);

    Node *child = a_node("Child", 1);
    crate->add_child(child);
    Node *grandchild = a_node("Grandchild", 1);
    child->add_child(grandchild);
    for (Node *joined : {crate, child, grandchild}) {
        if (joined->get_multiplayer_authority() != peer) {
            return law_broken(
                "'%s' is not on the controller's authority",
                String(joined->get_name()).utf8().get_data()
            );
        }
    }
    return law_held();
}

LawVerdict control_returns(const Stand &) {
    FaultWatch faults;
    faults.expect_error("*'Late'*[NW1012]*", 1);
    LoopbackRig rig(1);
    rig.mount();
    netw_test::flow_clocks(rig, TICKRATE);
    Node *crate = spawn_crate(rig, "authority_back_crate", HELD_CRATE_SOURCE);
    const Ref<NetwEntity> entity = NetwEntity::of(crate);
    Node *child = a_node("Child", 1);
    crate->add_child(child);
    entity->set_controller(rig.peer_id(CLIENT));
    entity->set_controller(1);

    Node *late = a_node("Late", FOREIGN);
    child->add_child(late);
    for (Node *held : {crate, child, late}) {
        if (held->get_multiplayer_authority() != 1) {
            return law_broken(
                "'%s' is not on the server's authority",
                String(held->get_name()).utf8().get_data()
            );
        }
    }
    return law_held();
}

Node *build_watched(const Variant &p_name) {
    Node2D *made = memnew(Node2D);
    made->set_name(String(p_name));
    return made;
}

struct Watching {
    LoopbackRig rig;
    Ref<NetwInterestLayer> gate;
    int64_t watcher = 0;
    int route = 0;
    RID standing;
    Ref<NetwEntity> wrapper;

    Watching() : rig(1) {
        rig.mount();
        Node *arena = rig.mirror_child("Arena");
        route = rig.spawn_registered(
            StringName("watched_body"),
            callable_mp_static(&build_watched),
            one_string("Watched"),
            string_type(),
            arena,
            0,
            false
        );
        gate = rig.server()->interest_layer(StringName(GATE_LAYER));
        REQUIRE(gate.is_valid());
        gate->set_default_leave_policy(NetwMultiplayer::LEAVE_POLICY_HIDE);
        gate->add_entity(NetwEntity::of(rig.route_node(route)));
        watcher = rig.peer_id(CLIENT);
        show();
        REQUIRE(held() != nullptr);
        standing = Internals::entity_at(client(), route);
        wrapper = NetwEntity::of(held());
    }

    NetwMultiplayer *client() const {
        return rig.client(CLIENT);
    }

    Node *held() const {
        return rig.route_node(route, CLIENT);
    }

    int64_t state() const {
        return client()->liveness_route_state(route);
    }

    bool layer_holds() const {
        const Ref<NetwInterestLayer> local
            = client()->interest_layer(StringName(GATE_LAYER));
        return local->has_entity(wrapper);
    }

    bool server_holds() const {
        return rig.server()->liveness_route_state(route) == LIVE
            && rig.route_node(route) != nullptr;
    }

    void show() {
        gate->add_viewer(watcher);
        rig.flush_interest();
        rig.pump(10);
    }

    void conceal() {
        gate->remove_viewer(watcher);
        rig.flush_interest();
        rig.pump(10);
    }

    LawVerdict returns() {
        show();
        if (state() != LIVE || held() == nullptr) {
            return law_broken("the shown entity is not live with a node");
        }
        if (Internals::entity_at(client(), route) != standing) {
            return law_broken("the shown entity is a new identity");
        }
        return law_held();
    }
};

LawVerdict hidden_copy(const Stand &) {
    Watching stand;
    if (!stand.layer_holds()) {
        return law_broken("the client's layer never held the entity");
    }
    stand.conceal();
    if (stand.state() != ABSENT || stand.held() != nullptr) {
        return law_broken("the hidden copy is still live on the client");
    }
    if (stand.layer_holds()) {
        return law_broken("the client's layer still holds the hidden copy");
    }
    if (Internals::entity_at(stand.client(), stand.route)
        != stand.standing) {
        return law_broken("the route lost the hidden identity");
    }
    if (!stand.server_holds()) {
        return law_broken("the server lost the entity it hid");
    }
    return stand.returns();
}

LawVerdict deleted_copy(const Stand &) {
    Watching stand;
    memdelete(stand.held());
    stand.client()->session_flush_deferred();
    if (stand.state() != ABSENT) {
        return law_broken("the deleted copy is not absent");
    }
    if (Internals::entity_at(stand.client(), stand.route)
        != stand.standing) {
        return law_broken("the route lost the deleted identity");
    }
    if (!stand.server_holds()) {
        return law_broken("the server heard a death it did not author");
    }
    stand.conceal();
    return stand.returns();
}

struct Crate {
    LoopbackRig rig;
    Node *crate = nullptr;
    Ref<NetwEntity> entity;

    Crate() : rig(1) {
        rig.mount();
        netw_test::flow_clocks(rig, TICKRATE);
        crate = spawn_crate(rig, "readmitted_crate", CRATE_SOURCE);
        entity = NetwEntity::of(crate);
    }

    bool client_holds(int64_t p_route) const {
        return rig.client(CLIENT)->liveness_route_state(p_route) == LIVE
            && rig.route_node(int(p_route), CLIENT) != nullptr;
    }
};

LawVerdict server_readds(const Stand &) {
    Crate stand;
    const int64_t first = route_of(stand.rig.server(), stand.crate);
    const StringName id = stand.entity->get_name();
    Node *home = stand.crate->get_parent();
    home->remove_child(stand.crate);
    stand.rig.step_ticks(SETTLE);
    if (stand.rig.route_node(int(first), CLIENT) != nullptr) {
        return law_broken("the client kept the departed crate");
    }

    const FaultWatch faults;
    home->add_child(stand.crate);
    stand.rig.step_ticks(SETTLE);
    const int64_t second = route_of(stand.rig.server(), stand.crate);
    if (faults.errors() != 0) {
        return law_broken("the re-add raised %d errors", int(faults.errors()));
    }
    if (NetwEntity::of(stand.crate) != stand.entity
        || stand.entity->get_name() != id) {
        return law_broken("the re-added crate lost its configuration");
    }
    if (second <= 0 || !stand.client_holds(second)) {
        return law_broken("the client does not hold the re-added crate");
    }
    return law_held();
}

LawVerdict client_readds(const Stand &) {
    Crate stand;
    const int64_t route = route_of(stand.rig.server(), stand.crate);
    Node *copy = stand.rig.route_node(int(route), CLIENT);
    if (copy == nullptr) {
        return law_broken("the client holds no copy");
    }
    Node *home = copy->get_parent();
    home->remove_child(copy);
    stand.rig.step_ticks(SETTLE);

    const FaultWatch faults;
    home->add_child(copy);
    stand.rig.step_ticks(SETTLE);
    if (faults.errors() != 0) {
        return law_broken("the re-add raised %d errors", int(faults.errors()));
    }
    if (!stand.client_holds(route)
        || stand.rig.route_node(int(route), CLIENT) != copy) {
        return law_broken("the copy was not admitted again on its route");
    }
    return law_held();
}

template <typename Spatial, typename Vec>
LawVerdict reparent_keeps(const Vec &p_offset, const Vec &p_global) {
    LoopbackRig rig(0);
    rig.mount();
    netw_test::flow_clocks(rig, TICKRATE);
    Spatial *home = memnew(Spatial);
    home->set_name("Home");
    Spatial *away = memnew(Spatial);
    away->set_name("Away");
    away->set_position(p_offset);
    rig.branch(SERVER)->add_child(home);
    rig.branch(SERVER)->add_child(away);
    Spatial *body = memnew(Spatial);
    body->set_name("Traveller");
    Netw::configure_entity(body);
    home->add_child(body);
    body->set_owner(rig.branch(SERVER));
    body->set_global_position(p_global);

    Netw::reparent(body, away);
    rig.server()->session_flush_deferred();
    if (body->get_parent() != away) {
        return law_broken("the body is not under its new parent");
    }
    if (body->get_global_position() != p_global) {
        return law_broken("the body moved in the world");
    }
    return law_held();
}

LawVerdict reparent_2d(const Stand &) {
    return reparent_keeps<Node2D>(Vector2(100, 0), Vector2(5, 5));
}

LawVerdict reparent_3d(const Stand &) {
    return reparent_keeps<Node3D>(Vector3(100, 0, 0), Vector3(5, 6, 7));
}

LawVerdict mounted_child_declares(const Stand &) {
    Node *root = memnew(Node);
    root->set_name("Root");
    Node *child = memnew(Node);
    child->set_name("Child");
    const Ref<NetwEntity> entity = Netw::configure_entity(root);
    root->add_child(child);
    netw::gd::scene_root()->add_child(root);
    LawVerdict verdict = law_held();
    {
        FaultWatch faults;
        faults.expect_error("*'Child'*[NW0036]*", 1);
        if (Netw::configure_entity(child).is_valid()) {
            verdict = law_broken("the child declared an entity of its own");
        } else if (
            Netw::configure_entity(root) != entity
            || NetwEntity::of(child) != entity
        ) {
            verdict = law_broken("the root's entity changed");
        }
    }
    root->get_parent()->remove_child(root);
    memdelete(root);
    return verdict;
}

LawVerdict part_configures(const Stand &p_stand) {
    FaultWatch faults;
    if (p_stand.fault == nullptr) {
        faults.expect_warning("*[NW10*", 0);
    } else if (p_stand.error) {
        faults.expect_error(p_stand.fault, 1);
    } else {
        faults.expect_warning(p_stand.fault, 1);
    }
    Node *root = memnew(Node);
    root->set_name("Stekh");
    const Ref<NetwEntity> above = Netw::configure_entity(root);
    if (p_stand.level) {
        root->set_script(netw_test::minted_script("extends Node\n"));
        Netw::configure_scene(root);
    }
    if (p_stand.root_priority > 0.0) {
        above->set_replication_priority(p_stand.root_priority);
    }
    if (p_stand.started) {
        netw::gd::scene_root()->add_child(root);
    }
    Node *scene = p_stand.started ? memnew(Node) : root;
    Node *child = memnew(Node);
    child->set_name("StekhPhysics");
    const Ref<NetwEntity> own = Netw::configure_entity(child);
    own->set_authority(NetwEntity::AUTHORITY_CONTROLLER);
    own->set_replication_priority(3.0);
    const Ref<netw::NetwSimulation> drafted = own->get_simulation();
    drafted->set_claim_on_contact(true);
    if (p_stand.part_name != nullptr) {
        own->set_name(p_stand.part_name);
    }
    scene->add_child(child);
    if (p_stand.placed) {
        child->set_owner(scene);
    }
    if (p_stand.instanced) {
        child->set_scene_file_path("res://stekh_physics.tscn");
    }
    if (p_stand.started) {
        root->add_child(scene);
    } else {
        netw::gd::scene_root()->add_child(root);
    }

    const Ref<NetwEntity> holder = p_stand.folds ? above : own;
    const double priority
        = p_stand.root_priority > 0.0 ? p_stand.root_priority : 3.0;
    LawVerdict verdict = law_held();
    if (NetwEntity::of(child) != holder) {
        verdict = law_broken("the part configures the wrong entity");
    } else if (
        holder->get_authority() != NetwEntity::AUTHORITY_CONTROLLER
        || !holder->get_simulation()->get_claim_on_contact()
        || holder->get_replication_priority() != priority
    ) {
        verdict = law_broken("the entity lost the part's options");
    } else if (!p_stand.folds && holder->get_simulation() != drafted) {
        verdict = law_broken("the entity answers a different part");
    } else if (
        p_stand.part_name != nullptr
        && above->get_name() == StringName(p_stand.part_name)
    ) {
        verdict = law_broken("the part renamed the entity above");
    }
    root->get_parent()->remove_child(root);
    memdelete(root);
    return verdict;
}

LawVerdict predicted_without_state(const Stand &) {
    LoopbackRig rig(1);
    rig.mount();
    FaultWatch faults;
    faults.expect_error("*Ghost*[NW1013]*", 1);
    Node3D *root = memnew(Node3D);
    root->set_name("Ghost");
    root->set_script(netw_test::minted_script(
        "extends Node3D\nfunc _network_process(_delta):\n\tpass\n"
    ));
    const Ref<NetwEntity> entity = Netw::configure_entity(root);
    const Ref<netw::NetwPrediction> prediction = entity->get_prediction();
    const bool declared = prediction.is_valid();
    if (declared) {
        prediction->set_mode(netw::NetwPrediction::MODE_ROLLBACK);
    }
    rig.branch()->add_child(root);
    root->get_parent()->remove_child(root);
    memdelete(root);
    if (!declared) {
        return law_broken("the body declared no prediction");
    }
    return law_held();
}

LawVerdict named_part_copies(const Stand &) {
    const Ref<Script> script = netw_test::minted_script("extends Node\n");
    FaultWatch faults;
    faults.expect_error("*'Wheel'*[NW1028]*", 1);
    Node *roots[2];
    bool folded = true;
    for (int at = 0; at < 2; ++at) {
        roots[at] = memnew(Node);
        roots[at]->set_name(at == 0 ? "CarA" : "CarB");
        const Ref<NetwEntity> car = Netw::configure_entity(roots[at]);
        Node *child = memnew(Node);
        child->set_name("Wheel");
        child->set_script(script);
        Netw::configure_entity(child)->set_name("wheel");
        roots[at]->add_child(child);
        child->set_owner(roots[at]);
        netw::gd::scene_root()->add_child(roots[at]);
        folded = folded && NetwEntity::of(child) == car;
    }
    for (Node *root : roots) {
        root->get_parent()->remove_child(root);
        memdelete(root);
    }
    if (!folded) {
        return law_broken("a wheel did not fold into its car");
    }
    return law_held();
}

const Row ROWS[] = {
    {"nodes of a foreign authority joining a server entity the server "
     "controls",
     "each child and grandchild is reset to the server and reported",
     &foreign_under_server},
    {"nodes of a foreign authority joining a server entity a player controls",
     "each child and grandchild is reset to the server and reported",
     &foreign_under_player},
    {"nodes joining an AUTHORITY_CONTROLLER entity a player controls",
     "each child and grandchild takes the controller's authority",
     &joins_controller},
    {"control handed to a player and back, then a foreign node joins",
     "every node is on the server's authority and the joiner is reported",
     &control_returns},
    {"an entity a client stops seeing, then sees again",
     "the copy goes absent and leaves the layer while the identity and the "
     "server's entity stand, and it returns as the same identity",
     &hidden_copy},
    {"a client that deletes its own copy, then is shown it again",
     "the identity waits absent, the server's entity stands, and the next "
     "showing seats a node on it",
     &deleted_copy},
    {"a node the server removes and adds back after it settled",
     "it comes back without an error as a fresh spawn on every peer, "
     "keeping its entity and name",
     &server_readds},
    {"a copy a client removes and adds back after it settled",
     "it is admitted again on the route the server holds",
     &client_readds},
    {"a 2D entity reparented under a moved parent",
     "it keeps its place in the world",
     &reparent_2d},
    {"a 3D entity reparented under a moved parent",
     "it keeps its place in the world",
     &reparent_3d},
    {"a mounted child declaring an entity under its ancestor's",
     "it is refused and reported, and stays part of the ancestor's",
     &mounted_child_declares},
    {"two copies of a car whose wheel part sets a name",
     "the wheels fold into their cars and the name is reported once",
     &named_part_copies},
    {"a predicted body with no state property entering the tree",
     "prediction is off and that is reported once",
     &predicted_without_state},
};

const Row PART = {
    "a part that configures an entity in _init",
    "a node a scene places inside an entity configures that entity, and "
    "anything else is its own",
    &part_configures,
};

const Part PARTS[] = {
    {"placed under an entity",
     false,
     true,
     false,
     false,
     0.0,
     nullptr,
     true,
     nullptr,
     false},
    {"placed under a level",
     true,
     true,
     false,
     false,
     0.0,
     nullptr,
     false,
     nullptr,
     false},
    {"an instanced scene root",
     false,
     true,
     true,
     false,
     0.0,
     nullptr,
     false,
     nullptr,
     false},
    {"made in code",
     false,
     false,
     false,
     false,
     0.0,
     nullptr,
     false,
     nullptr,
     false},
    {"both set one option",
     false,
     true,
     false,
     false,
     2.0,
     nullptr,
     true,
     "*'StekhPhysics'*'Stekh'*replication_priority*[NW1027]*",
     false},
    {"a part sets a name",
     false,
     true,
     false,
     false,
     0.0,
     "physics",
     true,
     "*'StekhPhysics'*'Stekh'*name*[NW1028]*",
     true},
    {"the entity above already started",
     false,
     true,
     false,
     true,
     0.0,
     nullptr,
     false,
     "*'StekhPhysics'*'Stekh'*[NW1024]*",
     false},
};

Stand stand_of(const Part &p_part, const char *p_label) {
    Stand stand;
    static_cast<Part &>(stand) = p_part;
    stand.label = String(p_label);
    return stand;
}

TEST_CASE(
    "[Netw][Entity][SceneTree] a node joining an entity takes its "
    "authority, a lost copy waits for its next admission, and a part "
    "configures the entity it is placed in"
) {
    const Stand stand = stand_of(Part(), "a loopback session");
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
    for (const Part &part : PARTS) {
        const Stand placed = stand_of(part, part.name);
        NETW_CELL(PART, placed);
        NETW_LAW_HOLDS(PART, placed);
    }
}

} // namespace TestEntity

#endif
