#include "support/frame_drive.h"
#include "support/netw_test.h"

#include "godot/class_db.hpp"
#include "godot/multiplayer.hpp"
#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "godot/time.hpp"
#include "netw/api/clock_config.hpp"
#include "netw/api/context.hpp"
#include "netw/api/netw_multiplayer.hpp"

#include <godot_cpp/classes/scene_multiplayer.hpp>

using namespace godot;

namespace NetwClockPumpFrameLaws {

namespace {

constexpr int PORT_RANGE_START = 30400;
constexpr int PORT_RANGE_SIZE = 100;
constexpr int64_t STEP_BUDGET_MSEC = 8000;
constexpr int TICKRATE = 30;

struct PumpEvidence {
    bool driven = false;
    int port = 0;
    bool resynchronized = false;
    int64_t configured_tickrate_after_resync = 0;
};

PumpEvidence &pump_evidence() {
    static PumpEvidence evidence;
    return evidence;
}

class ClockPumpScenario final : public netw_test::FrameScenario {
    int step = 0;
    int64_t step_opened = 0;
    Node *host_branch = nullptr;
    Node *client_branch = nullptr;
    Ref<netw::NetwMultiplayer> host;
    Ref<netw::NetwMultiplayer> client;
    Ref<MultiplayerPeer> client_peer;

    static int64_t now_msec() {
        return int64_t(Time::get_singleton()->get_ticks_msec());
    }

    bool over_budget() const {
        return now_msec() - step_opened > STEP_BUDGET_MSEC;
    }

    void enter(int p_step) {
        step = p_step;
        step_opened = now_msec();
    }

    static Ref<MultiplayerPeer> enet_peer() {
        return Ref<MultiplayerPeer>(
            ClassDB::instantiate(StringName("ENetMultiplayerPeer"))
        );
    }

    Node *mount(const char *p_name, const Ref<netw::NetwMultiplayer> &p_api) {
        Node *branch = memnew(Node);
        branch->set_name(StringName(p_name));
        netw::gd::scene_root()->add_child(branch);
        Ref<SceneMultiplayer> inner;
        inner.instantiate();
        inner->set_root_path(branch->get_path());
        p_api->session_set_inner(inner);
        netw::gd::scene_tree()->set_multiplayer(p_api, branch->get_path());
        return branch;
    }

    void declare_clock(Node *p_branch) {
        netw::Netw::configure_clock(p_branch)->ticks_per_second(TICKRATE);
    }

    int open_host() {
        for (int candidate = PORT_RANGE_START;
             candidate < PORT_RANGE_START + PORT_RANGE_SIZE;
             candidate++) {
            const Ref<MultiplayerPeer> peer = enet_peer();
            if (peer.is_null()) {
                return 0;
            }
            if (Error(int(peer->call(StringName("create_server"), candidate)))
                != OK) {
                continue;
            }
            host.instantiate();
            host_branch = mount("ClockPumpHost", host);
            declare_clock(host_branch);
            host->config_settle();
            host->session_prepare_join(StringName("host"), Array());
            host->NETW_API_VIRTUAL(set_multiplayer_peer)(peer);
            return candidate;
        }
        return 0;
    }

    bool open_client(int p_port) {
        const Ref<MultiplayerPeer> peer = enet_peer();
        if (peer.is_null()) {
            return false;
        }
        if (Error(
                int(peer->call(
                    StringName("create_client"),
                    String("127.0.0.1"),
                    p_port
                ))
            )
            != OK) {
            return false;
        }
        client_peer = peer;
        client.instantiate();
        client_branch = mount("ClockPumpClient", client);
        declare_clock(client_branch);
        client->config_settle();
        client->session_prepare_join(StringName("joiner"), Array());
        client->NETW_API_VIRTUAL(set_multiplayer_peer)(peer);
        return true;
    }

    void pump() {
        if (host.is_valid()) {
            host->NETW_API_VIRTUAL(poll)();
        }
        if (client.is_valid()) {
            client->NETW_API_VIRTUAL(poll)();
        }
    }

    void drop(Node *&r_branch, Ref<netw::NetwMultiplayer> &r_api) {
        if (r_api.is_valid()) {
            r_api->NETW_API_VIRTUAL(set_multiplayer_peer)(
                Ref<MultiplayerPeer>()
            );
        }
        if (r_branch != nullptr) {
            netw::gd::scene_tree()->set_multiplayer(
                Ref<MultiplayerAPI>(),
                r_branch->get_path()
            );
            r_branch->get_parent()->remove_child(r_branch);
            memdelete(r_branch);
            r_branch = nullptr;
        }
        r_api.unref();
        client_peer.unref();
    }

public:
    bool advance() override {
        PumpEvidence &evidence = pump_evidence();
        pump();

        switch (step) {
            case 0: {
                evidence.port = open_host();
                if (evidence.port == 0) {
                    evidence.driven = true;
                    return false;
                }
                enter(1);
                return true;
            }
            case 1: {
                if (host->session_is_online()) {
                    if (!open_client(evidence.port)) {
                        enter(99);
                        return true;
                    }
                    enter(2);
                } else if (over_budget()) {
                    enter(99);
                }
                return true;
            }
            case 2: {
                if (client->clock_is_synchronized()) {
                    client->NETW_API_VIRTUAL(set_multiplayer_peer)(
                        Ref<MultiplayerPeer>()
                    );
                    enter(3);
                } else if (over_budget()) {
                    enter(99);
                }
                return true;
            }
            case 3: {
                client_peer->close();
                if (Error(
                        int(client_peer->call(
                            StringName("create_client"),
                            String("127.0.0.1"),
                            evidence.port
                        ))
                    )
                    != OK) {
                    enter(99);
                    return true;
                }
                client->session_prepare_join(StringName("joiner"), Array());
                client->NETW_API_VIRTUAL(set_multiplayer_peer)(client_peer);
                enter(4);
                return true;
            }
            case 4: {
                if (client->clock_is_synchronized()) {
                    evidence.resynchronized = true;
                    evidence.configured_tickrate_after_resync
                        = int64_t(client->clock_get_param(
                            netw::NetwMultiplayer::CLOCK_PARAM_TICKRATE
                        ));
                    enter(99);
                } else if (over_budget()) {
                    enter(99);
                }
                return true;
            }
            default: {
                drop(client_branch, client);
                drop(host_branch, host);
                evidence.driven = true;
                return false;
            }
        }
    }
};

NETW_FRAME_SCENARIO(ClockPumpScenario, clock_pump_scenario);

} // namespace

TEST_CASE(
    "[Networked][Clock][Frame] CP6 a reconnect re-synchronizes on a fresh "
    "transport without reopening initialization, so the tickrate the session "
    "consumed once is the tickrate it comes back on"
) {
    const PumpEvidence &evidence = pump_evidence();
    REQUIRE(evidence.driven);
    if (evidence.port == 0) {
        return;
    }
    CHECK(evidence.resynchronized);
    NETW_CHECK_EQ(int(evidence.configured_tickrate_after_resync), TICKRATE);
}

} // namespace NetwClockPumpFrameLaws
