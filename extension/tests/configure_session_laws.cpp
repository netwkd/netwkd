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

Ref<MultiplayerPeer> a_server() {
    Ref<netw::LocalMultiplayerPeer> made;
    made.instantiate();
    NETW_CHECK_EQ(int(made->create_server()), int(OK));
    return made;
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
    "[Netw][Session][SceneTree] CS9 declared link conditions shape the "
    "peer the same assignment installs"
) {
    Mounted branch("CS9Tree");
    Ref<netw::NetwLinkConditions> conditions;
    conditions.instantiate();

    Netw::configure_session(child_named(branch.tree, "Session"))
        ->link(conditions);
    const Ref<MultiplayerPeer> offered = a_server();

    branch.assign(offered);

    CHECK(branch.peer() == offered);
    const Ref<MultiplayerPeer> polled
        = branch.api()->session_get_inner()->get_multiplayer_peer();
    REQUIRE(polled.is_valid());
    CHECK(polled != offered);
}

#endif

} // namespace TestNetwConfigureSession
