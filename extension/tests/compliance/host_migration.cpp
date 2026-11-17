#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceHostMigration {

using namespace godot;
using namespace netw_test;

constexpr const char *YARD = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").broadcast()

	func nudge(by: Vector3) -> void:
		position += by
)";

constexpr int HEIR = 2;
constexpr int DRIVER = 1;
constexpr int WATCHER = 0;
constexpr int MOVE = 14;

struct Exit {
    const char *label;
    Transfer mode;
    bool lost;
};

const Exit EXITS[] = {
    {"H1.lost.rejoin", netw::NetwSession::TRANSFER_REJOIN, true},
    {"H1.handed.rejoin", netw::NetwSession::TRANSFER_REJOIN, false},
    {"H1.lost.continue", netw::NetwSession::TRANSFER_CONTINUE, true},
    {"H1.handed.continue", netw::NetwSession::TRANSFER_CONTINUE, false},
};

World yard(const Exit &p_exit) {
    const Array east = Array::make(Vector3(1, 0, 0));
    const Array north = Array::make(Vector3(0, 0, 1));
    return World(YARD, 3)
        .capture()
        .migrates(p_exit.mode)
        .watch("Crate", "position")
        .watch("Kart", "position")
        .hear("Session", "server_transferred")
        .at(1, spawn("Crate", "Crate"))
        .at(1, spawn("Crate", "Kart"))
        .at(1, spawn("Crate", "Cart"))
        .at(2, control("Kart", DRIVER))
        .at(2, control("Cart", HEIR))
        .each(4, 10, call("Crate", "nudge", east))
        .each(4, 10, on(DRIVER, call("Kart", "nudge", north)))
        .at(MOVE, p_exit.lost ? lose_server() : transfer(HEIR))
        .each(30, 34, call("Crate", "nudge", east))
        .each(30, 34, on(DRIVER, call("Kart", "nudge", north)));
}

TEST_CASE(
    "[Netw][Compliance] H1 when the server moves, by hand or by loss, "
    "every survivor holds the new server's world with the same owners and "
    "controllers, and the world moves again"
) {
    for (const Exit &exit : EXITS) {
        const std::string label = exit.label;
        CAPTURE(label);
        const Seen seen = yard(exit).run(48);
        const Vector3 driven(0, 0, 12);
        const char *players = "client 0, client 1, the server";
        NETW_HOLDS(reads(seen, SERVER, "Session", "players", players));
        NETW_HOLDS(reads(seen, SERVER, "Session", "online", true));
        NETW_HOLDS(reads(seen, SERVER, "Crate", "position", Vector3(12, 0, 0)));
        NETW_HOLDS(reads(seen, SERVER, "Kart", "controller", "client 1"));
        NETW_HOLDS(reads(seen, SERVER, "Cart", "controller", "the server"));
        NETW_HOLDS(reads(seen, SERVER, "Crate", "authority", "the server"));
        NETW_HOLDS(reads(seen, SERVER, "Kart", "position", driven));
        NETW_HOLDS(hears(seen, SERVER, "Session", "server_transferred", ONCE));
        for (const int client : {WATCHER, DRIVER, HEIR}) {
            NETW_HOLDS(agrees(seen, client, MOVE - 1));
            if (client != HEIR || !exit.lost) {
                NETW_HOLDS(agrees(seen, client));
                NETW_HOLDS(
                    hears(seen, client, "Session", "server_transferred", ONCE)
                );
            }
        }
        NETW_HOLDS(quiet(seen));
        NETW_HOLDS(golden(seen, exit.label));
    }
}

constexpr const char *SALVAGE = R"(
class Crate extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)


func _init() -> void:
	Netw.configure_transfer(keep)


func keep(node: Node) -> bool:
	return node.name != &"Doomed"
)";

constexpr int STAYED = 0;
constexpr int SWAPPED = 1;

TEST_CASE(
    "[Netw][Compliance] H2 in rejoin mode the new server keeps only "
    "the entities its configure_transfer handler answers true for"
) {
    const Seen seen = World(SALVAGE, 2)
                          .migrates(netw::NetwSession::TRANSFER_REJOIN)
                          .at(1, spawn("Crate", "Kept"))
                          .at(1, spawn("Crate", "Doomed"))
                          .at(MOVE, transfer(SWAPPED))
                          .run(30);
    for (const int peer : {SERVER, STAYED, SWAPPED}) {
        NETW_HOLDS(reads(seen, peer, "Doomed", "parent", "Level", MOVE - 1));
        NETW_HOLDS(reads(seen, peer, "Kept", "parent", "Level"));
        NETW_HOLDS(absent(seen, peer, "Doomed", MOVE + 4));
    }
    NETW_HOLDS(agrees(seen, STAYED));
    NETW_HOLDS(agrees(seen, SWAPPED));
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceHostMigration

#endif
