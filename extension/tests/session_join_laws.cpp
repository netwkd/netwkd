#include "support/fault_log.h"
#include "support/netw_test.h"

#include "netw/api/entity.hpp"
#include "netw/api/join_request.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/session.hpp"
#include "support/loopback_rig.h"
#include "support/netw_recorder.h"

#if defined(NETW_TIER_HOSTED)

namespace TestNetwSessionJoinLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw_test::LoopbackRig;

Ref<netw::NetwPlayer> player_of(NetwMultiplayer *p_api, int p_peer) {
    return p_api->player_joined_of(p_peer);
}

TEST_CASE(
    "[Networked][Session] a declared scene is not a live one until it "
    "enters"
) {
    LoopbackRig rig(1);
    rig.declare_scene(StringName("Arena"));
    rig.pump(4);

    NetwMultiplayer *scenes = rig.server();
    REQUIRE(scenes != nullptr);
    NETW_CHECK_EQ(int(scenes->scene_list().size()), 0);

    rig.enter_scene(StringName("Arena"));

    NETW_CHECK_EQ(int(scenes->scene_list().size()), 1);
    CHECK(scenes->scene_find(StringName("Arena")).is_valid());
}

TEST_CASE(
    "[Networked][Session] two instances of one stem are two live "
    "scenes and the stem answers with the later one"
) {
    LoopbackRig rig(1);
    rig.declare_scene(StringName("first"), StringName("Arena"));
    rig.declare_scene(StringName("second"), StringName("Arena"));
    rig.pump(4);

    rig.enter_scene(StringName("first"));
    rig.enter_scene(StringName("second"));

    NetwMultiplayer *scenes = rig.server();
    REQUIRE(scenes != nullptr);
    NETW_CHECK_EQ(int(scenes->scene_list().size()), 2);
    NETW_CHECK_EQ(
        int(rig.server()->scene_find_all(StringName("Arena")).size()),
        2
    );
    CHECK(
        scenes->scene_find(StringName("Arena"))
        == rig.entity_of(StringName("second"))
    );
}

TEST_CASE(
    "[Networked][Session] a client mirrors a scene without being "
    "admitted to it, and holds no seat there"
) {
    LoopbackRig rig(0);
    rig.declare_scene(StringName("Arena"));
    rig.pump(4);
    rig.enter_scene(StringName("Arena"));
    rig.join(-1, StringName("host"));
    rig.arrive(StringName("alice"));
    rig.mirror_scene(0, StringName("Arena"));
    rig.pump(8);

    NetwMultiplayer *client_scenes = rig.client(0);
    REQUIRE(client_scenes != nullptr);
    const Ref<netw::NetwPlayer> seated = rig.client(0)->player_local();
    REQUIRE(seated.is_valid());

    NETW_CHECK_EQ(int(client_scenes->scene_list().size()), 1);
    CHECK(client_scenes->scene_find(StringName("Arena")).is_valid());
    CHECK(seated->get_entities().is_empty());
    CHECK_FALSE(client_scenes->scene_presented().is_valid());
}

TEST_CASE(
    "[Networked][Session] a client declares a scene only for a route the "
    "server admitted it"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*Setting the scene*[NW0012]*", 1);

    LoopbackRig rig(0);
    rig.declare_scene(StringName("Arena"));
    rig.pump(4);
    rig.enter_scene(StringName("Arena"));
    rig.join(-1, StringName("host"));
    rig.arrive(StringName("alice"));

    netw::NetwMultiplayer *api = rig.client(0);
    const RID invented = api->entity_create();
    REQUIRE(invented.is_valid());
    NETW_CHECK_EQ(int(api->scene_declare(invented)), int(ERR_UNAUTHORIZED));
    CHECK_FALSE(api->scene_is_declared(invented));

    const RID mirror = rig.mirror_scene(0, StringName("Arena"));
    CHECK(api->scene_is_declared(mirror));
}

TEST_CASE(
    "[Networked][Session] a mirrored scene is the server's scene, not a "
    "second one"
) {
    LoopbackRig rig(1);
    rig.declare_scene(StringName("Arena"));
    rig.pump(4);
    rig.enter_scene(StringName("Arena"));

    const RID origin = rig.entity_of(StringName("Arena"));
    const RID mirror = rig.mirror_scene(0, StringName("Arena"));

    NETW_CHECK_EQ(
        int(rig.client(0)->entity_get_route(mirror)),
        int(rig.server()->entity_get_route(origin))
    );
}

TEST_CASE(
    "[Networked][Session][Hosted] W0 a join whose build differs is refused by "
    "the part that differs"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    core->auth_set_app_tag(
        NetwMultiplayer::session_app_tag(StringName("gated-arena"))
    );

    netw::JoinRequest agreeing;
    agreeing.username = StringName("ada");
    agreeing.app_tag = core->auth_app_tag_of();
    agreeing.wire_identity = netw::SessionCore::compute_wire_identity();
    CHECK(core->session_identity_differs(agreeing) == nullptr);

    netw::JoinRequest other_app = agreeing;
    other_app.app_tag += 1;
    CHECK(String(core->session_identity_differs(other_app)) == String("app"));

    netw::JoinRequest other_wire = agreeing;
    other_wire.wire_identity += 1;
    CHECK(String(core->session_identity_differs(other_wire)) == String("wire"));

    core->embed_dispose();
}

TEST_CASE(
    "[Networked][Session] a peer refused for another build is told why with "
    "ERR_UNCONFIGURED, and a peer that floods joins has the extra ones "
    "dropped without being refused, so a seated peer keeps its seat"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_warning("*wire identity*[NW0025]*", 1);
    expected_faults.expect_warning("*[NW0028]*", 2);

    LoopbackRig rig(3);
    rig.pump(4);

    NetwMultiplayer *host = rig.server();
    const int stranger = rig.peer_id(0);
    const int waiting = rig.peer_id(1);
    const int seated = rig.peer_id(2);
    netw_test::Recorder stranger_heard(
        rig.client(0)->get_session().ptr(),
        {StringName("join_failed")}
    );
    netw_test::Recorder waiting_heard(
        rig.client(1)->get_session().ptr(),
        {StringName("join_failed")}
    );
    netw_test::Recorder seated_heard(
        rig.client(2)->get_session().ptr(),
        {StringName("join_failed")}
    );

    netw::JoinRequest older;
    older.username = StringName("v10");
    older.app_tag = host->auth_app_tag_of();
    older.wire_identity = netw::SessionCore::compute_wire_identity() ^ 0xA5A5;
    netw::JoinRequest current = older;
    current.username = StringName("v11");
    current.wire_identity = netw::SessionCore::compute_wire_identity();

    host->session_receive_join(current.serialize(), seated);
    rig.pump(4);
    REQUIRE(player_of(host, seated).is_valid());
    for (int sent = 0; sent < 12; ++sent) {
        host->session_receive_join(current.serialize(), seated);
    }

    host->session_receive_join(older.serialize(), stranger);

    const Ref<netw::NetwPlayer> waiting_before = player_of(host, waiting);
    host->structure_halt_enter();
    for (int sent = 0; sent < 12; ++sent) {
        host->session_receive_join(current.serialize(), waiting);
    }
    rig.pump(4);

    const StringName failed("join_failed");
    NETW_REQUIRE_EQ(stranger_heard.count(failed), 1);
    NETW_CHECK_EQ(
        int64_t(stranger_heard.args(failed)[0]),
        int64_t(ERR_UNCONFIGURED)
    );
    CHECK(String(stranger_heard.args(failed)[1]).contains("wire"));
    NETW_CHECK_EQ(waiting_heard.count(failed), 0);
    NETW_CHECK_EQ(seated_heard.count(failed), 0);
    CHECK(player_of(host, stranger).is_null());
    CHECK(player_of(host, waiting) == waiting_before);
    CHECK(player_of(host, seated).is_valid());
}

} // namespace TestNetwSessionJoinLaws

#endif
