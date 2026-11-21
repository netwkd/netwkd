#include "support/netw_test.h"

#include "godot/multiplayer.hpp"
#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/context.hpp"
#include "netw/api/link_conditions.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/session_config.hpp"

namespace TestNetwConfigureSession {

using namespace godot;
using netw::MultiplayerViewport;
using netw::Netw;
using netw::NetwMultiplayer;

namespace {

struct Mounted {
    MultiplayerViewport *tree = nullptr;

    explicit Mounted(const char *p_name) {
        tree = memnew(MultiplayerViewport);
        tree->set_name(StringName(p_name));
        netw::gd::scene_root()->add_child(tree);
    }

    ~Mounted() {
        api()->NETW_API_VIRTUAL(set_multiplayer_peer)(Ref<MultiplayerPeer>());
        tree->get_parent()->remove_child(tree);
        memdelete(tree);
    }

    NetwMultiplayer *api() const {
        return tree->get_api().ptr();
    }

    Ref<MultiplayerPeer> peer() const {
        return api()->NETW_API_VIRTUAL(get_multiplayer_peer)();
    }

    void assign(const Ref<MultiplayerPeer> &p_peer) const {
        api()->NETW_API_VIRTUAL(set_multiplayer_peer)(p_peer);
    }
};

Node *child_named(Node *p_parent, const char *p_name) {
    Node *node = memnew(Node);
    node->set_name(StringName(p_name));
    p_parent->add_child(node);
    return node;
}

int64_t tag_of(const char *p_app) {
    return NetwMultiplayer::session_app_tag(StringName(p_app));
}

} // namespace

TEST_CASE(
    "[Netw][Session][Hosted][SceneTree] CS3 the fluent tail reaches the "
    "auth app tag and not only the stored app id, because both readers are "
    "handed the same owned value at consumption"
) {
    Mounted branch("CS3Tree");
    Netw::configure_session(child_named(branch.tree, "Session"))
        ->app(StringName("tagged-arena"));

    branch.api()->config_settle();

    NETW_CHECK_EQ(branch.api()->auth_app_tag_of(), tag_of("tagged-arena"));
}

#if defined(NETW_TIER_HOSTED)

TEST_CASE(
    "[Netw][Session][SceneTree] CS9 a shared link declaration shapes "
    "clients and shapes the host only with its own profile"
) {
    const Ref<netw::NetwLinkConditions> client_conditions
        = netw::NetwLinkConditions::poor_3g();
    const Ref<netw::NetwLinkConditions> host_conditions
        = netw::NetwLinkConditions::wifi();
    for (const Ref<netw::NetwLinkConditions> &host_profile :
         {Ref<netw::NetwLinkConditions>(), host_conditions}) {
        Ref<netw::LocalLoopbackSession> loop;
        loop.instantiate();
        const Ref<MultiplayerPeer> host_peer = loop->get_server_peer();
        const Ref<MultiplayerPeer> client_peer = loop->create_client_peer();
        Mounted host("CS9Host");
        Mounted client("CS9Client");

        Netw::configure_session(child_named(host.tree, "Session"))
            ->link(client_conditions, host_profile);
        Netw::configure_session(child_named(client.tree, "Session"))
            ->link(client_conditions, host_profile);
        host.assign(host_peer);
        client.assign(client_peer);

        const Ref<MultiplayerPeer> host_effective
            = host.api()->session_get_inner()->get_multiplayer_peer();
        const Ref<MultiplayerPeer> client_effective
            = client.api()->session_get_inner()->get_multiplayer_peer();
        REQUIRE(host_effective.is_valid());
        REQUIRE(client_effective.is_valid());
        CHECK(host.peer() == host_peer);
        CHECK(client.peer() == client_peer);
        CHECK((host_effective != host_peer) == host_profile.is_valid());
        CHECK(client_effective != client_peer);
    }
}

#endif

} // namespace TestNetwConfigureSession
