#include "support/loopback_rig.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/display/book.hpp"
#include "netw/entity/ids.hpp"
#include "support/netw_call_log.h"

namespace TestEntityReparentSettleLaws {

using namespace godot;
using namespace netw_test;
using netw::NetwEntity;
using netw::NetwMultiplayer;

struct Stand {
    LoopbackRig rig;
    Node *scene = nullptr;
    Node *home = nullptr;
    Node *away = nullptr;
    Node *body = nullptr;
    Ref<NetwEntity> entity;
    CallLog seen;

    Stand() : rig(0) {
        scene = memnew(Node);
        scene->set_name("SettleStand");
        netw::gd::scene_root()->add_child(scene);
        netw::gd::scene_tree()->set_multiplayer(
            Ref<godot::MultiplayerAPI>(rig.server()),
            scene->get_path()
        );
        home = memnew(Node);
        home->set_name("Home");
        scene->add_child(home);
        away = memnew(Node);
        away->set_name("Away");
        scene->add_child(away);

        body = memnew(Node);
        body->set_name("Traveller");
        body->connect(StringName("ready"), seen.callable("ready"));
        entity = NetwEntity::ensure(body);
        entity->set_name(StringName("traveller"));
        entity->arm(
            godot::Object::cast_to<netw::NetwMultiplayer>(rig.server())
        );
        entity->connect(StringName("reparented"), seen.callable("reparented"));
        home->add_child(body);
        body->set_owner(scene);
    }

    ~Stand() {
        home->get_parent()->remove_child(home);
        away->get_parent()->remove_child(away);
        memdelete(home);
        memdelete(away);
        netw::gd::scene_tree()->set_multiplayer(
            Ref<godot::MultiplayerAPI>(),
            scene->get_path()
        );
        scene->get_parent()->remove_child(scene);
        memdelete(scene);
    }

    void settle() {
        rig.server()->session_flush_deferred();
    }
};

TEST_CASE(
    "[Netw][Entity] a move reports at the settle and not where "
    "it happens, because the subtree is still rebuilding there, and it "
    "reports the completed fact rather than the options that asked"
) {
    Stand stand;

    NETW_CHECK_EQ(stand.seen.count("reparented"), 0);

    NetwMultiplayer::entity_move(stand.body, stand.away);

    const bool moved = stand.body->get_parent() == stand.away;
    CHECK(moved);
    NETW_CHECK_EQ(stand.seen.count("reparented"), 0);

    stand.settle();

    NETW_CHECK_EQ(stand.seen.count("reparented"), 1);
    NETW_CHECK_EQ(stand.seen.args("reparented").size(), 0);
}

TEST_CASE(
    "[Netw][Entity] several hops before one settle are one move, "
    "because the report answers where the owner ended up and a parent it "
    "never rested under is not a fact about it"
) {
    Stand stand;

    NetwMultiplayer::entity_move(stand.body, stand.away);
    NetwMultiplayer::entity_move(stand.body, stand.home);

    stand.settle();

    NETW_CHECK_EQ(stand.seen.count("reparented"), 1);
    const bool landed = stand.body->get_parent() == stand.home;
    CHECK(landed);
}

TEST_CASE(
    "[Netw][Entity] a move settles once after the entity handle changes"
) {
    Stand stand;
    NetwMultiplayer::entity_move(stand.body, stand.away);
    NETW_CHECK_EQ(stand.seen.count("reparented"), 0);

    const RID prior = stand.entity->get_rid_handle();
    netw::entity::retain(prior);
    const RID handle = netw::entity::mint();
    Internals::adopt_handle(stand.entity, handle);
    stand.rig.server()->entity_settle_captured(handle);
    const int heard_on_settle = stand.seen.count("reparented");
    const bool found_by_route
        = stand.rig.server()->wrapper_for_route(stand.entity->get_route())
        == stand.entity;
    const bool displayed
        = Internals::get_display_book(stand.rig.server())->decl_ptr(handle)
        != nullptr;
    Internals::adopt_handle(stand.entity, prior);
    netw::entity::release(handle);
    netw::entity::release(prior);

    stand.settle();
    NETW_CHECK_EQ(heard_on_settle, 1);
    CHECK(found_by_route);
    CHECK(displayed);
    NETW_CHECK_EQ(stand.seen.count("reparented"), 1);
}

TEST_CASE(
    "[Netw][Entity] a moved owner is initialized once and reports no "
    "move for standing up, because _ready is where a node builds itself "
    "and neither arriving nor moving is a rebirth"
) {
    Stand stand;
    NETW_CHECK_EQ(stand.seen.count("ready"), 1);
    NETW_CHECK_EQ(stand.seen.count("reparented"), 0);

    NetwMultiplayer::entity_move(stand.body, stand.away);
    stand.settle();

    NETW_CHECK_EQ(stand.seen.count("ready"), 1);
    NETW_CHECK_EQ(stand.seen.count("reparented"), 1);
}

} // namespace TestEntityReparentSettleLaws

#endif
