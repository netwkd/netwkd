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

Ref<NetwPlayer> seat(const Ref<NetwMultiplayer> &p_core, int64_t p_peer) {
    Ref<NetwPlayer> player;
    player.instantiate();
    p_core->player_adopt(p_peer, player);
    REQUIRE(p_core->player_admit(p_peer));
    return player;
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
    "[Networked][Session][Hosted] SC2 a kick request reaches the server as "
    "kick_requested at the next poll, naming the player who asked and the "
    "player named, whether the host asked or a client's frame did"
) {
    struct Row {
        const char *label;
        bool from_client;
    };
    const Row rows[] = {{"host asks", false}, {"client frame", true}};
    for (const Row &row : rows) {
        INFO(row.label);
        const Ref<NetwMultiplayer> core = authority();
        const Ref<NetwPlayer> host = seat(core, 1);
        const Ref<NetwPlayer> client = seat(core, 4);
        const Ref<NetwPlayer> named = seat(core, 9);
        const CallLog seen;
        core->get_session()->connect(
            StringName("kick_requested"),
            seen.callable("kick")
        );

        if (row.from_client) {
            const int64_t channel = kick_request_channel();
            core->session_publish_control(
                channel,
                4,
                netw::session::frame_write(
                    netw::session::KickRequest{9, String("griefing")}
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
        core->NETW_API_VIRTUAL(poll)();

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
    "[Networked][Session][Hosted] SC4 a client whose tickrate differs from "
    "the server's emits tickrate_mismatch with the server's tickrate when "
    "the game chose to decide, at the end of the next tick"
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
    core->clock_step(1);

    NETW_CHECK_EQ(seen.count("mismatch"), 1);
    const Array heard = seen.args("mismatch");
    REQUIRE(heard.size() == 1);
    NETW_CHECK_EQ(int(heard[0]), 30);
}

} // namespace TestSessionControlVerbLaws
