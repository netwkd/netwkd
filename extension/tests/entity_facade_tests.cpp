#include "support/fault_log.h"
#include "support/netw_test.h"
#if defined(NETW_TIER_HOSTED)
#include "support/minted_script.h"
#endif

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/context.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"

namespace TestNetwEntityFacade {

using namespace godot;
using netw::NetwEntity;

TEST_CASE(
    "[Networked][Entity][Hosted] F1 an identity is stamped onto the entity "
    "and the node keeps the name it was given"
) {
    Node *root = memnew(Node);
    root->set_name("Player");

    const Ref<NetwEntity> entity = NetwEntity::ensure(root);
    REQUIRE(entity.is_valid());
    entity->set_name("valeria");
    entity->set_peer_id(42);

    CHECK(root->get_name() == StringName("Player"));
    CHECK(entity->get_name() == StringName("valeria"));
    NETW_CHECK_EQ(entity->get_peer_id(), 42);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Entity][Hosted] F2 asking about a node with no record answers "
    "null, and minting one makes the same ask answer it"
) {
    Node *clean = memnew(Node);
    CHECK(NetwEntity::of(clean).is_null());
    CHECK(NetwEntity::of(nullptr).is_null());

    const Ref<NetwEntity> minted = NetwEntity::ensure(clean);
    REQUIRE(minted.is_valid());
    CHECK(NetwEntity::of(clean) == minted);
    CHECK(NetwEntity::ensure(clean) == minted);

    memdelete(clean);
}

TEST_CASE(
    "[Networked][Entity][Hosted] F3 a child inside an entity is part of it "
    "until it is given a record of its own"
) {
    Node *root = memnew(Node);
    Node *child = memnew(Node);
    root->add_child(child);
    const Ref<NetwEntity> owned = NetwEntity::ensure(root);
    REQUIRE(owned.is_valid());

    CHECK(NetwEntity::of(child) == owned);

    const Ref<NetwEntity> own = NetwEntity::ensure(child);
    REQUIRE(own.is_valid());
    CHECK(own != owned);
    CHECK(NetwEntity::of(child) == own);
    CHECK(NetwEntity::of(root) == owned);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Entity][Hosted] F4 resolving a detached branch provisions its "
    "outermost node, because that is the one an enclosing entity will find"
) {
    Node *parent = memnew(Node);
    Node *child = memnew(Node);
    Node *grandchild = memnew(Node);
    parent->add_child(child);
    child->add_child(grandchild);

    const Ref<NetwEntity> provisioned = NetwEntity::resolve(grandchild);
    REQUIRE(provisioned.is_valid());
    CHECK(NetwEntity::of(parent) == provisioned);
    CHECK(NetwEntity::of(grandchild) == provisioned);
    CHECK(NetwEntity::resolve(child) == provisioned);

    CHECK(NetwEntity::resolve(nullptr).is_null());

    memdelete(parent);
}

TEST_CASE(
    "[Networked][Entity][Hosted][SceneTree] IT3 a mounted child cannot "
    "declare its ancestor as its own entity root"
) {
    Node *root = memnew(Node);
    root->set_name("Root");
    Node *child = memnew(Node);
    child->set_name("Child");
    const Ref<NetwEntity> entity = netw::Netw::configure_entity(root);
    root->add_child(child);
    netw::gd::scene_root()->add_child(root);
    netw_test::FaultWatch faults;
    faults.expect_error("*'Child'*[NW0036]*", 1);
    CHECK(netw::Netw::configure_entity(child).is_null());
    CHECK(netw::Netw::configure_entity(root) == entity);
    CHECK(NetwEntity::of(child) == entity);
    root->get_parent()->remove_child(root);
    memdelete(root);
}

TEST_CASE(
    "[Networked][Entity][Hosted][SceneTree] IT4 a node a scene places inside "
    "an entity configures that entity, and anything else is its own"
) {
    struct Row {
        const char *label;
        bool level;
        bool placed;
        bool instanced;
        bool started;
        double root_priority;
        const char *part_name;
        bool folds;
        const char *fault;
        bool error;
    };
    const Row rows[] = {
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
    for (const Row &row : rows) {
        NETW_FORMAT_TEXT(netw_row, row.label);
        CAPTURE(netw_row);
        netw_test::FaultWatch faults;
        if (row.fault == nullptr) {
            faults.expect_warning("*[NW10*", 0);
        } else if (row.error) {
            faults.expect_error(row.fault, 1);
        } else {
            faults.expect_warning(row.fault, 1);
        }
        Node *root = memnew(Node);
        root->set_name("Stekh");
        const Ref<NetwEntity> above = netw::Netw::configure_entity(root);
        above->set_declares_scene(row.level);
        if (row.root_priority > 0.0) {
            above->set_replication_priority(row.root_priority);
        }
        if (row.started) {
            netw::gd::scene_root()->add_child(root);
        }
        Node *scene = row.started ? memnew(Node) : root;
        Node *child = memnew(Node);
        child->set_name("StekhPhysics");
        const Ref<NetwEntity> own = netw::Netw::configure_entity(child);
        own->set_authority(NetwEntity::AUTHORITY_CONTROLLER);
        own->set_replication_priority(3.0);
        own->get_simulation()->set_claim_on_contact(true);
        if (row.part_name != nullptr) {
            own->set_name(row.part_name);
        }
        scene->add_child(child);
        if (row.placed) {
            child->set_owner(scene);
        }
        if (row.instanced) {
            child->set_scene_file_path("res://stekh_physics.tscn");
        }
        if (row.started) {
            root->add_child(scene);
        } else {
            netw::gd::scene_root()->add_child(root);
        }

        const Ref<NetwEntity> holder = row.folds ? above : own;
        CHECK(NetwEntity::of(child) == holder);
        CHECK(holder->get_authority() == NetwEntity::AUTHORITY_CONTROLLER);
        CHECK(holder->get_simulation()->get_claim_on_contact());
        CHECK(
            holder->get_replication_priority()
            == (row.root_priority > 0.0 ? row.root_priority : 3.0)
        );
        if (row.part_name != nullptr) {
            CHECK(above->get_name() != StringName(row.part_name));
        }
        root->get_parent()->remove_child(root);
        memdelete(root);
    }
}

#if defined(NETW_TIER_HOSTED)
TEST_CASE(
    "[Networked][Entity][SceneTree] IT5 copies of one scene whose part sets "
    "a name report it once"
) {
    const Ref<Script> script = netw_test::minted_script("extends Node\n");
    REQUIRE(script.is_valid());
    Node *roots[2];
    netw_test::FaultWatch faults;
    faults.expect_error("*'Wheel'*[NW1028]*", 1);
    for (int at = 0; at < 2; ++at) {
        roots[at] = memnew(Node);
        roots[at]->set_name(at == 0 ? "CarA" : "CarB");
        const Ref<NetwEntity> car = netw::Netw::configure_entity(roots[at]);
        Node *child = memnew(Node);
        child->set_name("Wheel");
        child->set_script(script);
        netw::Netw::configure_entity(child)->set_name("wheel");
        roots[at]->add_child(child);
        child->set_owner(roots[at]);
        netw::gd::scene_root()->add_child(roots[at]);
        CHECK(NetwEntity::of(child) == car);
    }
    for (Node *root : roots) {
        root->get_parent()->remove_child(root);
        memdelete(root);
    }
}
#endif

} // namespace TestNetwEntityFacade
