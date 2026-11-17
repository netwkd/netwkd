#include "support/fault_log.h"
#include "support/internals.h"
#include "support/netw_test.h"

#include "netw/api/clock_config.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/participant.hpp"
#include "netw/session/frames.hpp"
#include "netw/wire/registry.hpp"
#include "support/netw_call_log.h"

namespace TestSessionControlVerbLaws {

using namespace godot;
using netw::NetwMultiplayer;
using netw::NetwPlayer;
using netw_test::CallLog;
using netw_test::Internals;

Ref<NetwMultiplayer> authority() {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Internals::clock_engine(core).set_configured(true);
    core->session_set_role(NetwMultiplayer::ROLE_LISTEN_SERVER);
    return core;
}

int64_t kick_request_channel() {
    const netw::wire::WireRegistry registry
        = netw::wire::WireRegistry::create_default();
    const netw::wire::ChannelDecl *decl
        = registry.find_channel_by_name(StringName("SESSION_KICK_REQUEST"));
    REQUIRE(decl != nullptr);
    return int64_t(decl->id);
}

TEST_CASE(
    "[Netw][Session][Hosted] SC2 a kick request read outside a step "
    "reaches the server as kick_requested at once, naming the player who "
    "asked and the player named, whether the host asked or a client's frame "
    "did, and not at all when the player named already left or belongs to "
    "another session"
) {
    struct Row {
        const char *label;
        bool from_client;
        bool target_leaves;
        bool foreign;
    };
    const Row rows[] = {
        {"host asks", false, false, false},
        {"client frame", true, false, false},
        {"target already left", true, true, false},
        {"another session's player", false, false, true},
    };
    for (const Row &row : rows) {
        INFO(row.label);
        netw_test::FaultWatch faults;
        faults.expect_error("*request_kick*[NW0032]*", row.foreign ? 1 : 0);
        const Ref<NetwMultiplayer> core = authority();
        const Ref<NetwMultiplayer> other = authority();
        const Ref<NetwPlayer> host = Internals::seat_player(core, 1);
        const Ref<NetwPlayer> client = Internals::seat_player(core, 4);
        const Ref<NetwPlayer> named
            = Internals::seat_player(row.foreign ? other : core, 9);
        const CallLog seen;
        core->get_session()->connect(
            StringName("kick_requested"),
            seen.callable("kick")
        );
        const int64_t named_id = named->get_id();
        if (row.target_leaves) {
            core->session_settle_peer_loss(9);
        }

        if (row.from_client) {
            const int64_t channel = kick_request_channel();
            core->session_publish_control(
                channel,
                4,
                netw::session::frame_write(
                    netw::session::KickRequest{
                        uint64_t(named_id),
                        String("griefing")
                    }
                )
            );
            core->session_publish_control(
                channel,
                4,
                netw::session::frame_write(
                    netw::session::SessionReason{String("griefing")}
                )
            );
        } else {
            core->get_session()->request_kick(named, String("griefing"));
        }

        if (row.target_leaves || row.foreign) {
            NETW_CHECK_EQ(seen.count("kick"), 0);
            continue;
        }
        NETW_CHECK_EQ(seen.count("kick"), 1);
        const Array heard = seen.args("kick");
        REQUIRE(heard.size() == 3);
        const Ref<NetwPlayer> asker = row.from_client ? client : host;
        CHECK(bool(Object::cast_to<NetwPlayer>(heard[0]) == asker.ptr()));
        CHECK(bool(Object::cast_to<NetwPlayer>(heard[1]) == named.ptr()));
        CHECK(String(heard[2]) == String("griefing"));
    }
}

TEST_CASE(
    "[Netw][Session][Hosted] SC4 a client whose tickrate differs from "
    "the server's emits tickrate_mismatch with the server's tickrate when "
    "the game chose to decide, at once when the reply is read outside a step"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Ref<netw::NetwClockConfig> config;
    config.instantiate();
    config->set("tickrate", 60);
    NETW_REQUIRE_EQ(int(core->clock_initialize(config)), int(OK));
    Internals::clock_engine(core).set_manual_tick(true);
    core->session_set_role(NetwMultiplayer::ROLE_CLIENT);
    core->clock_set_mismatch_action(NetwMultiplayer::MISMATCH_ACTION_SIGNAL);
    const CallLog seen;
    core->get_clock()->connect(
        StringName("tickrate_mismatch"),
        seen.callable("mismatch")
    );

    core->clock_receive_handshake_reply(
        netw::session::frame_write(netw::session::ClockRate{30}),
        1
    );

    NETW_CHECK_EQ(seen.count("mismatch"), 1);
    const Array heard = seen.args("mismatch");
    REQUIRE(heard.size() == 1);
    NETW_CHECK_EQ(int(heard[0]), 30);
}

} // namespace TestSessionControlVerbLaws
