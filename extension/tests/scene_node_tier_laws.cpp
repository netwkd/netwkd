#include "support/fault_log.h"
#include "support/internals.h"
#include "support/netw_test.h"

#include "godot/node.hpp"
#include "godot/packed_scene.hpp"
#include "godot/viewport.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/scene.hpp"
#include "netw/api/session.hpp"
#include "netw/scene_core.hpp"
#include "support/netw_call_log.h"
#include "support/netw_recorder.h"

namespace TestNetwSceneNodeTierLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw::NetwSceneCore;
using netw_test::CallLog;
using netw_test::Internals;
using netw_test::Recorder;

struct Mounted {
    Node *root = nullptr;
    Ref<netw::NetwEntity> entity;
    RID scene;
};

Node *named_scene_node(
    const Ref<NetwMultiplayer> &p_core,
    const StringName &p_stem
) {
    return p_core->scene_get_node(p_core->scene_named(p_stem));
}

Mounted mount(
    const Ref<NetwMultiplayer> &p_core,
    Node *p_root,
    const StringName &p_stem,
    bool p_owns_its_world
) {
    Mounted made;
    made.root = p_root;
    if (!String(p_stem).is_empty()) {
        made.root->set_name(p_stem);
    }
    made.entity.instantiate();
    made.entity->attach_to(made.root);
    made.entity->set_scene_isolation(
        p_owns_its_world ? int64_t(NetwSceneCore::ISOLATION_OWN_WORLD)
                         : int64_t(NetwSceneCore::ISOLATION_NONE)
    );
    made.scene = Internals::get_liveness_core(p_core)->entity_create();
    made.entity->get_record()->adopt_handle(made.scene);
    REQUIRE(p_core->entity_of(made.root) == made.scene);
    Internals::get_scene_core(p_core)
        ->scene_enter(made.scene, p_stem, p_owns_its_world);
    REQUIRE(named_scene_node(p_core, p_stem) == made.root);
    return made;
}

Mounted mount_plain(
    const Ref<NetwMultiplayer> &p_core,
    Node *p_parent,
    const StringName &p_stem
) {
    Node *root = memnew(Node);
    if (p_parent != nullptr) {
        p_parent->add_child(root);
    }
    return mount(p_core, root, p_stem, false);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT1 an unwrapped scene root is its own outer "
    "node, so nothing above it competes for the scene's identity"
) {
    Node *arena = memnew(Node);
    arena->set_name(StringName("Arena"));

    NETW_CHECK_EQ(NetwMultiplayer::scene_outer_of(arena), arena);
    NETW_CHECK_EQ(NetwMultiplayer::scene_outer_of(nullptr), nullptr);
    NETW_CHECK_EQ(NetwMultiplayer::scene_world_of(arena), nullptr);
    NETW_CHECK_EQ(NetwMultiplayer::scene_inner_of(arena), arena);

    Node *plain = memnew(Node);
    plain->add_child(arena);
    NETW_CHECK_EQ(NetwMultiplayer::scene_outer_of(arena), arena);
    NETW_CHECK_EQ(NetwMultiplayer::scene_inner_of(plain), plain);

    memdelete(plain);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT2 scene_containing answers the container "
    "the session MOUNTED, and answers null for a node in no scene at all"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));

    Node *deep = memnew(Node);
    arena.root->add_child(deep);

    NETW_CHECK_EQ(core->scene_containing(deep), arena.root);
    NETW_CHECK_EQ(core->scene_containing(arena.root), arena.root);
    NETW_CHECK_EQ(core->scene_containing(arena.root), arena.root);

    Node *outside = memnew(Node);
    root->add_child(outside);
    NETW_CHECK_EQ(core->scene_containing(outside), nullptr);
    NETW_CHECK_EQ(core->scene_containing(root), nullptr);
    NETW_CHECK_EQ(core->scene_containing(nullptr), nullptr);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT3 a scene the book stopped holding live is "
    "no longer what scene_containing answers, so a retired container stops "
    "standing in for its own subtree"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));

    NETW_CHECK_EQ(core->scene_containing(arena.root), arena.root);

    Internals::get_scene_core(core)->scene_exit(arena.scene);

    NETW_CHECK_EQ(core->scene_containing(arena.root), nullptr);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT9 a scene leaving the live book leaves its "
    "node mounted where the game put it, so forgetting a scene is a book "
    "edit and never a free"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));
    const Mounted annex = mount_plain(core, root, StringName("Annex"));

    NETW_CHECK_EQ(Internals::get_scene_core(core)->live_count(), 2);

    core->scene_forget(arena.root);

    NETW_CHECK_EQ(Internals::get_scene_core(core)->live_count(), 1);
    NETW_CHECK_EQ(named_scene_node(core, StringName("Arena")), nullptr);
    NETW_CHECK_EQ(arena.root->get_parent(), root);
    NETW_CHECK_EQ(annex.root->get_parent(), root);
    CHECK_FALSE(core->scene_presented().is_valid());

    core->scene_forget(annex.root);

    NETW_CHECK_EQ(Internals::get_scene_core(core)->live_count(), 0);
    CHECK_FALSE(core->scene_presented().is_valid());

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted][SceneTree] SNT10 a scene owns its world by "
    "what it declares, whether or not a viewport hosts it, and an unknown "
    "scene owns none"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);

    const Mounted declared
        = mount(core, memnew(Node), StringName("Arena"), true);
    root->add_child(declared.root);
    const Mounted shared
        = mount(core, memnew(Node), StringName("Lobby"), false);
    root->add_child(shared.root);

    CHECK(core->scene_owns_its_world(declared.scene));
    CHECK_FALSE(core->scene_owns_its_world(shared.scene));

    SubViewport *isolated = memnew(SubViewport);
    isolated->set_meta(NetwMultiplayer::scene_container_meta(), true);
    root->add_child(isolated);
    Node *annex = memnew(Node);
    isolated->add_child(annex);
    const Mounted hosted = mount(core, annex, StringName("Annex"), true);

    CHECK(core->scene_owns_its_world(hosted.scene));
    CHECK_FALSE(core->scene_owns_its_world(RID()));

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT14 forgetting a container emits "
    "scene_removed with its scene at the next poll, and a null container or "
    "a node the session never held emits nothing"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));
    REQUIRE(core->liveness_bind(
        arena.scene,
        Internals::get_liveness_core(core)->reserve_route(),
        arena.entity,
        arena.entity->get_record(),
        arena.root
    ));
    const Ref<netw::NetwSession> session = core->get_session();
    const Ref<netw::NetwScene> scene = core->scene_handle_of(arena.scene);
    REQUIRE(scene.is_valid());
    Recorder removals(
        session.ptr(),
        Vector<StringName>{StringName("scene_removed")}
    );
    Node *stranger = memnew(Node);
    root->add_child(stranger);

    core->scene_forget(nullptr);
    core->scene_forget(stranger);
    core->scene_forget(arena.root);
    core->NETW_API_VIRTUAL(poll)();

    NETW_CHECK_EQ(removals.count(StringName("scene_removed")), 1);
    const Array removed = removals.args(StringName("scene_removed"), 0);
    REQUIRE(removed.size() == 1);
    CHECK(bool(Object::cast_to<netw::NetwScene>(removed[0]) == scene.ptr()));
    NETW_CHECK_EQ(session->get_scenes().size(), 0);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT16 remembering a scene root enters it "
    "under its own name, and a node the session seated no entity on is "
    "refused rather than entered"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);

    Node *arena = memnew(Node);
    arena->set_name(StringName("Arena"));
    root->add_child(arena);
    Ref<netw::NetwEntity> seated;
    seated.instantiate();
    seated->attach_to(arena);
    seated->get_record()->adopt_handle(
        Internals::get_liveness_core(core)->entity_create()
    );

    Node *bare = memnew(Node);
    bare->set_name(StringName("Bare"));
    root->add_child(bare);

    CHECK_FALSE(core->scene_remember(nullptr));
    CHECK_FALSE(core->scene_remember(bare));
    NETW_CHECK_EQ(named_scene_node(core, StringName("Bare")), nullptr);

    CHECK(core->scene_remember(arena));
    NETW_CHECK_EQ(named_scene_node(core, StringName("Arena")), arena);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT17 an arriving move resolves the waiting "
    "promise with OK"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));
    const Mounted annex = mount_plain(core, root, StringName("Annex"));

    Node *body = memnew(Node);
    arena.root->add_child(body);
    Ref<netw::NetwEntity> mover;
    mover.instantiate();
    mover->attach_to(body);

    Ref<netw::NetwPromise> settled;
    settled.instantiate();
    core->scene_arrive(mover, arena.root, annex.root, settled);

    CHECK(settled->get_is_settled());
    NETW_CHECK_EQ(settled->get_code(), int(OK));

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT18 a joining player spends the "
    "admission parked for it against every live scene and is announced as a "
    "viewer there, and a player nothing parked for is announced nowhere"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));
    const CallLog seen;
    core->scene_observe(
        arena.scene,
        NetwMultiplayer::SCENE_EVENT_VIEWER,
        seen.callable("viewer")
    );

    Ref<netw::NetwPlayer> waiting;
    waiting.instantiate();
    waiting->bind_to(core.ptr(), 9);
    core->player_adopt(9, waiting);
    core->player_admit(9);
    CHECK(Internals::get_scene_core(core)->admission_park(arena.scene, 9));
    CHECK(Internals::get_scene_core(core)->admission_is_parked(arena.scene, 9));

    core->scene_on_player_joined(waiting);

    CHECK_FALSE(
        Internals::get_scene_core(core)->admission_is_parked(arena.scene, 9)
    );
    NETW_CHECK_EQ(seen.count(StringName("viewer")), 1);

    Ref<netw::NetwPlayer> stranger;
    stranger.instantiate();
    stranger->bind_to(core.ptr(), 11);
    core->player_adopt(11, stranger);
    core->player_admit(11);

    core->scene_on_player_joined(stranger);

    NETW_CHECK_EQ(seen.count(StringName("viewer")), 1);

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT19 opening a scene's admission on a server "
    "announces every peer its boundary already reaches, opens the row once so "
    "a second open announces nothing, and closing it forgets the viewers"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Ref<netw::LocalMultiplayerPeer> peer;
    peer.instantiate();
    peer->create_server();
    core->NETW_API_VIRTUAL(set_multiplayer_peer)(peer);
    const CallLog flushed;
    core->set_interest_flush(flushed.callable("flush"));
    Node *root = memnew(Node);
    const Mounted arena = mount_plain(core, root, StringName("Arena"));
    const CallLog seen;
    core->scene_observe(
        arena.scene,
        NetwMultiplayer::SCENE_EVENT_VIEWER,
        seen.callable("viewer")
    );

    REQUIRE(core->scene_admit_peer(arena.scene, 7));
    Ref<netw::NetwPlayer> watching;
    watching.instantiate();
    watching->bind_to(core.ptr(), 7);
    core->player_adopt(7, watching);
    core->player_admit(7);
    const int announced = seen.count(StringName("viewer"));

    core->scene_open_admission(arena.root);

    NETW_CHECK_EQ(seen.count(StringName("viewer")), announced + 1);
    CHECK(core->scene_subscribes(arena.scene, 7));

    core->scene_open_admission(arena.root);

    NETW_CHECK_EQ(seen.count(StringName("viewer")), announced + 1);

    core->scene_close_admission(arena.scene);

    NETW_CHECK_EQ(
        int(Internals::membership_book(core).members_of(arena.scene).size()),
        0
    );

    memdelete(root);
}

TEST_CASE(
    "[Networked][Scene][Hosted] SNT20 adopting a reparented entity admits its "
    "peer to the scene that now encloses it, and admits nobody for a prop, "
    "for an entity already its own scene, or on a peer that is not authority"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    core->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    const CallLog flushed;
    core->set_interest_flush(flushed.callable("flush"));
    Node *root = memnew(Node);

    Node *arena = memnew(Node);
    arena->set_name(StringName("Arena"));
    root->add_child(arena);
    Ref<netw::NetwEntity> arena_wrapper;
    arena_wrapper.instantiate();
    arena_wrapper->attach_to(arena);
    arena_wrapper->set_declares_scene(true);
    const RID scene = Internals::get_liveness_core(core)->entity_create();
    arena_wrapper->get_record()->adopt_handle(scene);
    REQUIRE(core->entity_of(arena) == scene);
    REQUIRE(core->liveness_bind(
        scene,
        61,
        arena_wrapper,
        arena_wrapper->get_record(),
        arena
    ));

    Node *body = memnew(Node);
    arena->add_child(body);
    Ref<netw::NetwEntity> player;
    player.instantiate();
    player->attach_to(body);
    const RID mover = Internals::get_liveness_core(core)->entity_create();
    player->get_record()->adopt_handle(mover);
    REQUIRE(core->entity_of(body) == mover);
    REQUIRE(core->liveness_bind(mover, 62, player, player->get_record(), body));
    const int64_t host = MultiplayerPeer::TARGET_PEER_SERVER;
    player->set_peer_id(host);

    REQUIRE(core->scene_of(mover) == scene);
    CHECK_FALSE(core->scene_subscribes(scene, host));

    core->scene_adopt_entity(mover);

    CHECK(core->scene_subscribes(scene, host));

    SUBCASE("a prop carrying no peer reaches nowhere") {
        core->scene_release_peer(scene, host);
        player->set_peer_id(0);

        core->scene_adopt_entity(mover);

        CHECK_FALSE(core->scene_subscribes(scene, host));
    }

    SUBCASE("an entity that is its own scene is already where it belongs") {
        core->scene_release_peer(scene, host);
        arena_wrapper->set_peer_id(host);

        core->scene_adopt_entity(scene);

        CHECK_FALSE(core->scene_subscribes(scene, host));
    }

    memdelete(root);
}

#if defined(NETW_TIER_HOSTED)
#endif

} // namespace TestNetwSceneNodeTierLaws
