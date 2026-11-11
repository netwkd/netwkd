#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceJoining {

using namespace godot;
using namespace netw_test;

constexpr const char *LOBBY = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


func _init() -> void:
	Netw.configure_admission(admit)


func admit(request: NetwJoinRequest) -> Error:
	if request.username != &"":
		request.account = "test:" + request.username
	return OK
)";

constexpr int WATCHER = 0;
constexpr int FIRST = 1;
constexpr int AGAIN = 2;
constexpr int PAUSED = 16;
constexpr int RUNNING = 24;

World paused_lobby() {
    return World(LOBBY, 1)
        .watch("Crate", "position")
        .hear("Session", "paused_changed")
        .hear("Session", "player_left")
        .at(1, spawn("Crate", "Crate"))
        .at(3, set("Crate", "position", Vector3(3, 0, 1)))
        .at(5, pause("menu"))
        .at(5, pause("menu"))
        .at(7, join("ana"))
        .at(10, join("ana"))
        .at(20, unpause())
        .at(26, pause("closing"))
        .at(28, leave());
}

TEST_CASE(
    "[Networked][Compliance] J1 a player who joins a paused game gets the "
    "roster, the pause and the world as they are now, a second login of "
    "the account replaces the first, and a game that ends paused unfreezes"
) {
    const Seen seen = paused_lobby().run(36);
    const Array menu = Array::make(true, "menu");
    const String roster = "client 0, client 2";
    for (const int peer : {SERVER, WATCHER, AGAIN}) {
        NETW_HOLDS(hears(seen, peer, "Session", "paused_changed", 1, 1, 19));
        NETW_HOLDS(hears_with(seen, peer, "Session", "paused_changed", menu));
        NETW_HOLDS(reads(seen, peer, "Session", "frozen", true, PAUSED));
        NETW_HOLDS(reads(seen, peer, "Session", "paused", true, PAUSED));
        NETW_HOLDS(reads(seen, peer, "Session", "players", roster, PAUSED));
        NETW_HOLDS(reads(seen, peer, "Session", "paused", false, RUNNING));
        NETW_HOLDS(reads(seen, peer, "Session", "frozen", false));
    }
    const Vector3 parked(3, 0, 1);
    NETW_HOLDS(reads(seen, AGAIN, "Crate", "position", parked, PAUSED));
    NETW_HOLDS(agrees(seen, AGAIN, PAUSED));
    NETW_HOLDS(agrees(seen, WATCHER, RUNNING));
    NETW_HOLDS(reads(seen, FIRST, "Session", "online", false, PAUSED));
    NETW_HOLDS(hears(seen, SERVER, "Session", "player_left", ONCE, 1, PAUSED));
    NETW_HOLDS(hears_with(
        seen,
        SERVER,
        "Session",
        "player_left",
        Array::make("ana", netw::NetwPlayer::LEAVE_DROPPED)
    ));
    NETW_HOLDS(reads(seen, WATCHER, "Session", "online", false));
    NETW_HOLDS(hears(seen, WATCHER, "Session", "paused_changed", 2, 26));
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceJoining

#endif
