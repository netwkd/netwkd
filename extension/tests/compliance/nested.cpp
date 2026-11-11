#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace ComplianceNested {

using namespace godot;
using namespace netw_test;

constexpr const char *CARPARK = R"(
class Part extends Node3D:
	func nest(part: Node3D, called: StringName) -> void:
		part.name = called
		part.scene_file_path = "res://%s.tscn" % called
		add_child(part)
		part.owner = self


class Car extends Part:
	func _init() -> void:
		Netw.configure_entity(self)
		nest(Wheel.new(), &"Wheel")


class Wheel extends Part:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"rotation").on_spawn()
		nest(Hubcap.new(), &"Hubcap")


class Hubcap extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
		Netw.configure_property(self, &"position").state()


class Jack extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self)
)";

constexpr int LATE = 2;
constexpr const char *PARTS[] = {"Car", "Wheel", "Hubcap", "Jack"};

World parked() {
    return World(CARPARK, 2)
        .watch("Wheel", "rotation")
        .watch("Hubcap", "position")
        .at(1, spawn("Car", "Car"))
        .at(4, set("Wheel", "rotation", Vector3(0, 2, 0)))
        .at(4, set("Hubcap", "position", Vector3(1, 0, 3)))
        .at(8, join("cy"));
}

TEST_CASE(
    "[Networked][Compliance] N1 spawning a root brings every entity inside "
    "it, each with its own spawn values, on every peer and on a late joiner"
) {
    const Seen seen = parked().run(16);
    for (int peer = SERVER; peer <= LATE; ++peer) {
        NETW_HOLDS(reads(seen, peer, "Wheel", "parent", "Level/Car"));
        NETW_HOLDS(reads(seen, peer, "Hubcap", "parent", "Level/Car/Wheel"));
        NETW_HOLDS(reads(seen, peer, "Hubcap", "position", Vector3(1, 0, 3)));
        NETW_HOLDS(same_node(seen, peer, "Wheel"));
        NETW_HOLDS(same_node(seen, peer, "Hubcap"));
    }
    NETW_HOLDS(reads(seen, LATE, "Wheel", "rotation", Vector3(0, 2, 0)));
    NETW_HOLDS(agrees(seen, LATE));
    NETW_HOLDS(quiet(seen));
}

constexpr int SCRAPPED = 6;

LawVerdict before(
    const Seen &p_seen,
    int p_peer,
    const char *p_first,
    const char *p_then
) {
    for (const Heard &row : p_seen.heard) {
        if (row.peer != p_peer) {
            continue;
        }
        if (row.node == String(p_first)) {
            return law_held();
        }
        if (row.node == String(p_then)) {
            break;
        }
    }
    return law_broken(
        "%s did not hear '%s' before '%s'",
        who(p_peer).c_str(),
        p_first,
        p_then
    );
}

World scrapped() {
    World world(CARPARK, 2);
    for (const char *part : PARTS) {
        world.hear(part, "despawning");
    }
    return world.at(1, spawn("Car", "Car"))
        .at(2, spawn("Jack", "Jack", "Wheel"))
        .at(SCRAPPED, despawn("Car", "scrapped"))
        .at(SCRAPPED + 3, join("cy"));
}

NETW_FRAME_SCENARIO(WorldFrames, scrapped_frames, &scrapped, 16);

TEST_CASE(
    "[Networked][Compliance][Frame] N2 despawning a parent despawns its "
    "children first on every peer"
) {
    const Seen &seen = scrapped_frames.seen();
    for (int peer = SERVER; peer <= LATE; ++peer) {
        for (const char *part : PARTS) {
            NETW_HOLDS(absent(seen, peer, part, SCRAPPED + 2));
        }
    }
    NETW_HOLDS(hears(seen, SERVER, "Car", "despawning", ONCE));
    for (int client = 0; client < 2; ++client) {
        for (const char *part : PARTS) {
            NETW_HOLDS(hears(seen, client, part, "despawning", ONCE));
        }
        NETW_HOLDS(before(seen, client, "Jack", "Wheel"));
        NETW_HOLDS(before(seen, client, "Hubcap", "Wheel"));
        NETW_HOLDS(before(seen, client, "Wheel", "Car"));
    }
    NETW_HOLDS(quiet(seen));
}

constexpr const char *SHOWROOM = R"(
class Car extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self).interest.join(&"cars")


class Wheel extends Node3D:
	func _init() -> void:
		Netw.configure_entity(self).interest.join(&"wheels")
)";

Step viewer(const char *p_layer, int p_client, bool p_admitted) {
    return [=](Live &p_live, int p_peer) {
        Node *level = p_live.need(p_peer, "Level");
        if (level == nullptr) {
            return;
        }
        const Ref<netw::NetwInterestLayer> layer
            = netw::Netw::session(level)->interest_layer(p_layer);
        const int64_t peer_id = p_live.rig.peer_id(p_client);
        if (p_admitted) {
            layer->add_viewer(peer_id);
        } else {
            layer->remove_viewer(peer_id);
        }
    };
}

constexpr int EARLY = 0;
constexpr int SHUT_OUT = 1;

World showroom() {
    return World(SHOWROOM, 2)
        .at(1, spawn("Car", "Car"))
        .at(1, spawn("Wheel", "Wheel", "Car"))
        .at(1, viewer("cars", EARLY, true))
        .at(1, viewer("wheels", EARLY, true))
        .at(1, viewer("wheels", SHUT_OUT, true))
        .at(6, viewer("cars", SHUT_OUT, true))
        .at(10, viewer("cars", EARLY, false))
        .at(14, viewer("wheels", SHUT_OUT, false));
}

TEST_CASE(
    "[Networked][Compliance] N4 a child is never visible to a peer that "
    "cannot see its parent, whatever order visibility changes in"
) {
    const Seen seen = showroom().run(18);
    NETW_HOLDS(reads(seen, EARLY, "Wheel", "parent", "Level/Car", 4));
    NETW_HOLDS(agrees(seen, EARLY, 4));
    NETW_HOLDS(reads(seen, SHUT_OUT, "Wheel", "parent", "Level/Car", 8));
    NETW_HOLDS(agrees(seen, SHUT_OUT, 8));
    NETW_HOLDS(absent(seen, EARLY, "Car", 11));
    NETW_HOLDS(absent(seen, EARLY, "Wheel", 11));
    NETW_HOLDS(reads(seen, SHUT_OUT, "Car", "parent", "Level"));
    NETW_HOLDS(absent(seen, SHUT_OUT, "Wheel", 15));
    NETW_HOLDS(quiet(seen));
}

} // namespace ComplianceNested

#endif
