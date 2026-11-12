#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceStockNodes {

using namespace godot;
using namespace netw_test;

constexpr int FIRST = 0;
constexpr int LATE = 1;
constexpr int SECOND = 1;

World harbor() {
    return World("worlds/harbor.tscn", 1)
        .watch("Buoy", "paint")
        .watch("Buoy", "turns")
        .watch("Buoy", "turns_heard")
        .at(2, call("Level", "launch", Array::make("red")))
        .each(4, 20, call("Buoy", "turn"))
        .at(12, join("cy"));
}

NETW_FRAME_SCENARIO(WorldFrames, harbor_frames, &harbor, 30);

TEST_CASE(
    "[Networked][Compliance][Frame] G1 a stock MultiplayerSynchronizer "
    "carries its spawn and synced properties, emits synchronized with the "
    "value applied, and heals a late joiner"
) {
    const Seen &seen = harbor_frames.seen();
    for (int peer = SERVER; peer <= LATE; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Buoy", "paint", "red"));
        NETW_HOLDS(reads(seen, peer, "Buoy", "turns", 17));
        NETW_HOLDS(reads(seen, peer, "Buoy", "parent", "Level/Dock"));
    }
    for (const int client : {FIRST, LATE}) {
        NETW_HOLDS(reads(seen, client, "Buoy", "turns_heard", 17));
        NETW_HOLDS(same_node(seen, client, "Buoy"));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr int ROUGH = 0;
constexpr int CLEAN = 1;

World lighthouse() {
    return World("worlds/lighthouse.tscn", 2)
        .watch("Lamp", "turns")
        .watch("Lamp", "turns_back")
        .watch("Lamp", "position")
        .each(2, 90, call("Lamp", "turn"))
        .at(12, impair(ROUGH, rough(7, TICK_MS)))
        .at(72, heal(ROUGH));
}

TEST_CASE(
    "[Networked][Compliance] G2 a stock MultiplayerSynchronizer on a link "
    "with loss, duplication and reordering never moves a value backward and "
    "ends on the server's value"
) {
    const Seen seen = lighthouse().run(100);
    NETW_HOLDS(reads(seen, SERVER, "Lamp", "turns", 89));
    for (const int client : {ROUGH, CLEAN}) {
        NETW_HOLDS(reads(seen, client, "Lamp", "turns_back", 0));
        NETW_HOLDS(agrees(seen, client));
    }
    NETW_HOLDS(quiet(seen));
}

World beacons() {
    return World("worlds/beacons.tscn", 1)
        .watch("Lamp", "turns")
        .watch("Bulb", "turns")
        .watch("Mast", "turns")
        .watch("Flag", "turns")
        .each(2, 24, call("Level", "turn"))
        .at(4, join("cy"))
        .at(14, call("Level", "show_lamp_to", Array::make("cy")));
}

TEST_CASE(
    "[Networked][Compliance] G3 a synchronizer's visibility hides a peer and "
    "heals it when shown, and nested synchronizers each honour their own "
    "(godot#68508)"
) {
    const Seen seen = beacons().run(30);
    for (const int client : {FIRST, LATE}) {
        NETW_HOLDS(reads(seen, client, "Lamp", "turns", 0, 13));
        NETW_HOLDS(reads(seen, client, "Bulb", "turns", 0, 13));
        NETW_HOLDS(reads(seen, client, "Mast", "turns", 23));
        NETW_HOLDS(reads(seen, client, "Flag", "turns", 0));
    }
    NETW_HOLDS(reads(seen, FIRST, "Lamp", "turns", 0));
    NETW_HOLDS(reads(seen, FIRST, "Bulb", "turns", 0));
    NETW_HOLDS(reads(seen, LATE, "Lamp", "turns", 23));
    NETW_HOLDS(reads(seen, LATE, "Bulb", "turns", 23));
    NETW_HOLDS(quiet(seen));
}

World pilots() {
    const Step hand = call("Level", "hand_input_to", Array::make("cy"));
    return World("worlds/pilots.tscn", 1)
        .watch("InputFirst", "steering")
        .watch("InputFirst", "position")
        .watch("ServerFirst", "steering")
        .watch("ServerFirst", "position")
        .at(2, join("cy"))
        .at(6, hand)
        .at(6, on(FIRST, hand))
        .at(6, on(LATE, hand))
        .each(8, 17, on(LATE, call("Level", "steer")))
        .each(8, 24, call("Level", "drive"));
}

TEST_CASE(
    "[Networked][Compliance] G4 a player with a server position "
    "synchronizer and a client input synchronizer on one root works in "
    "either sibling order (godot#105117, godot#75884)"
) {
    const Seen seen = pilots().run(30);
    for (const char *pilot : {"InputFirst", "ServerFirst"}) {
        for (int peer = SERVER; peer <= LATE; ++peer) {
            NETW_HOLDS(reads(seen, peer, pilot, "position", Vector3(10, 0, 0)));
        }
        NETW_HOLDS(reads(seen, SERVER, pilot, "steering", 10));
        NETW_HOLDS(reads(seen, FIRST, pilot, "steering", 10));
    }
    NETW_HOLDS(quiet(seen));
}

World slipway() {
    return World("worlds/slipway.tscn", 2)
        .watch("Red", "label")
        .watch("Blue", "label")
        .watch("Gold", "label")
        .watch("Green", "label")
        .hear("Spawner", "spawned")
        .hear("Spawner", "despawned")
        .at(1, call("Level", "launch", Array::make("red")))
        .at(3, call("Level", "launch", Array::make("blue")))
        .at(4, call("Level", "moor"))
        .at(6,
            on(FIRST,
               call("Level", "seize_and_launch", Array::make("green"))))
        .at(8, call("Level", "launch", Array::make("gold")))
        .at(10, call("Level", "sink", Array::make("Blue")));
}

NETW_FRAME_SCENARIO(WorldFrames, slipway_frames, &slipway, 20);

TEST_CASE(
    "[Networked][Compliance][Frame] G6 a stock MultiplayerSpawner spawns "
    "through its spawn function with its argument, fires spawned and "
    "despawned on each client, and refuses a client's spawn"
) {
    const Seen &seen = slipway_frames.seen();
    for (int peer = SERVER; peer <= SECOND; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Red", "label", "red"));
        NETW_HOLDS(reads(seen, peer, "Red", "parent", "Level/Dock"));
        NETW_HOLDS(reads(seen, peer, "Blue", "label", "blue", 9));
        NETW_HOLDS(absent(seen, peer, "Blue", 11));
        NETW_HOLDS(reads(seen, peer, "Gold", "label", "gold"));
        NETW_HOLDS(absent(seen, peer, "Green", 8));
    }
    for (const int client : {FIRST, SECOND}) {
        NETW_HOLDS(hears(seen, client, "Spawner", "spawned", 4));
        NETW_HOLDS(hears(seen, client, "Spawner", "despawned", ONCE));
    }
    NETW_HOLDS(absent(seen, SERVER, "Green", 1));
    NETW_HOLDS(absent(seen, SECOND, "Green", 1));
    REQUIRE(seen.faults.size() == 1);
    CHECK(seen.faults[0].contains("[NW0012]"));
}

World quay() {
    return World("worlds/quay.tscn", 2)
        .at(1, call("Level", "build_in_workshop", Array::make("Crate")))
        .at(3, call("Level", "open_annex_and_launch", Array::make("Ferry")));
}

NETW_FRAME_SCENARIO(WorldFrames, quay_frames, &quay, 170);

TEST_CASE(
    "[Networked][Compliance][Frame] G7 a spawn for a level a client has not "
    "loaded waits and lands when the level arrives, or is dropped with one "
    "warning when it never does (godot#78692)"
) {
    const Seen &seen = quay_frames.seen();
    for (const int client : {FIRST, SECOND}) {
        NETW_HOLDS(
            reads(seen, client, "Ferry", "parent", "Level/Dock/Annex/Berth")
        );
        NETW_HOLDS(same_node(seen, client, "Ferry"));
        NETW_HOLDS(absent(seen, client, "Crate", 1));
    }
    REQUIRE(seen.faults.size() == 2);
    for (const String &fault : seen.faults) {
        CHECK(fault.contains("[NW0026]"));
    }
}

World radio() {
    return World("worlds/radio.tscn", 2)
        .capture()
        .watch("Radio", "hails")
        .watch("Radio", "honest_hails")
        .watch("Radio", "orders")
        .at(3, on(FIRST, call("Radio", "hail_another_client")))
        .at(6, on(FIRST, call("Radio", "hail_everyone")))
        .at(9, call("Radio", "give_order"))
        .at(12, on(FIRST, call("Radio", "give_order")));
}

TEST_CASE(
    "[Networked][Compliance] G9 an @rpc from one client to another runs once "
    "under the caller's id, a broadcast runs once everywhere, and an "
    "authority RPC refuses a client"
) {
    const Seen seen = radio().run(20);
    NETW_HOLDS(reads(seen, SECOND, "Radio", "hails", ONCE, 5));
    NETW_HOLDS(reads(seen, SECOND, "Radio", "hails", 2));
    NETW_HOLDS(reads(seen, SECOND, "Radio", "honest_hails", 2));
    NETW_HOLDS(reads(seen, SERVER, "Radio", "hails", ONCE));
    NETW_HOLDS(reads(seen, SERVER, "Radio", "honest_hails", ONCE));
    NETW_HOLDS(reads(seen, FIRST, "Radio", "hails", NEVER));
    NETW_HOLDS(reads(seen, SERVER, "Radio", "orders", NEVER));
    for (const int client : {FIRST, SECOND}) {
        NETW_HOLDS(reads(seen, client, "Radio", "orders", ONCE));
    }
    REQUIRE(seen.faults.size() == 2);
    for (const String &fault : seen.faults) {
        CHECK(fault.contains("'order'"));
        CHECK(fault.contains("[NW0021]"));
        CHECK(fault.contains("@rpc(\"any_peer\")"));
    }
    NETW_HOLDS(golden(seen, "G9"));
}

World tiller() {
    const Step hand = call("Level", "hand_helm_to", Array::make("cy"));
    return World("worlds/tiller.tscn", 1)
        .watch("Helm", "turns")
        .each(2, 10, call("Helm", "turn"))
        .at(4, join("cy"))
        .at(14, hand)
        .at(14, on(FIRST, hand))
        .at(14, on(LATE, hand))
        .each(16, 25, on(LATE, call("Helm", "turn")));
}

TEST_CASE(
    "[Networked][Compliance] G5 moving a synchronizer's authority to a "
    "client moves its stream, so every peer reads what that client writes"
) {
    const Seen seen = tiller().run(32);
    for (int peer = SERVER; peer <= LATE; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Helm", "turns", 9, 13));
        NETW_HOLDS(reads(seen, peer, "Helm", "turns", 19));
    }
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceStockNodes

#endif
