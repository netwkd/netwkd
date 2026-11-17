#include "tests/compliance/laws.h"

#if defined(NETW_TIER_HOSTED)

namespace TestDisplaySession {

using namespace godot;
using namespace netw_test;

constexpr const char *STREET = R"(
class Eye extends Node3D:
	var writes := 0

	func _init() -> void:
		set_notify_transform(true)

	func _notification(what: int) -> void:
		if what == NOTIFICATION_TRANSFORM_CHANGED:
			writes += 1


class Shown extends Node3D:
	signal aimed(to: float)

	var eye := Eye.new()
	var spare := Eye.new()
	var aim := 0.0
	var mark := 0.0
	var x: float:
		get:
			return position.x
	var drawn: float:
		get:
			return eye.global_position.x
	var offset: float:
		get:
			return eye.position.x
	var spare_offset: float:
		get:
			return spare.position.x

	func _init() -> void:
		eye.name = &"Eye"
		spare.name = &"Spare"
		add_child(eye)
		add_child(spare)
		var entity := Netw.configure_entity(self)
		entity.display.visual_root = ^"Eye"
		var glide := NetwInterpolate.new().lerp().smooth(0.0).snap_at(5.0)
		Netw.configure_property(self, &"position").state().interpolate(glide)
		var aiming := NetwInterpolate.new().lerp().smooth(0.0).to(&"aim")
		Netw.configure_rpc(point).interpolate(aiming)
		var marking := NetwInterpolate.new().lerp().smooth(0.0).to(&"mark")
		Netw.configure_signal(aimed).interpolate(marking)

	@rpc("authority", "call_remote", "reliable")
	func point(_to: float) -> void:
		pass

	func slide() -> void:
		position.x += 0.05
		point.rpc(position.x)
		Netw.emit(aimed, position.x)

	func hop() -> void:
		position.x += 50.0

	func use_spare() -> void:
		NetwEntity.of(self).display.visual_root = ^"Spare"

	func sight() -> NetwInterestLayer:
		return Netw.session(self).interest_layer(&"sight")

	func enter_sight() -> void:
		sight().add_entity(NetwEntity.of(self))
		reveal()

	func seclude() -> void:
		for peer in multiplayer.get_peers():
			sight().remove_viewer(peer)

	func reveal() -> void:
		for peer in multiplayer.get_peers():
			sight().add_viewer(peer)


class Glider extends Shown:
	pass


class Sign extends Shown:
	pass


class Flapper extends Shown:
	pass


class Walker extends Node3D:
	var eye := Eye.new()
	var motion := 0
	var x: float:
		get:
			return position.x
	var drawn: float:
		get:
			return eye.global_position.x

	func _init() -> void:
		eye.name = &"Eye"
		add_child(eye)
		var entity := Netw.configure_entity(self)
		entity.display.visual_root = ^"Eye"
		var glide := NetwInterpolate.new().lerp().smooth(0.0)
		Netw.configure_property(self, &"position").state().interpolate(glide)
		Netw.configure_property(self, &"motion").input()

	func _network_process(_delta: float, _tick: int, _is_fresh: bool) -> void:
		position.x += motion * 0.1

	func bump() -> void:
		position.x += 2.0


class Crate extends RigidBody3D:
	var eye := Eye.new()
	var x: float:
		get:
			return position.x
	var drawn: float:
		get:
			return eye.global_position.x
	var offset: float:
		get:
			return eye.position.x
	var writes: int:
		get:
			return eye.writes

	func _init() -> void:
		gravity_scale = 0.0
		collision_layer = 0
		collision_mask = 0
		var hull := CollisionShape3D.new()
		hull.shape = BoxShape3D.new()
		add_child(hull)
		eye.name = &"Eye"
		add_child(eye)
		var entity := Netw.configure_entity(self)
		entity.simulation.remote_mode = NetwSimulation.REMOTE_MODE_ACTIVE
		entity.display.visual_root = ^"Eye"
		var glide := NetwInterpolate.new().lerp().smooth(0.0)
		Netw.configure_property(self, &"position").broadcast().interpolate(glide)

	func nudge() -> void:
		linear_velocity = Vector3(1, 0, 0)

	func settle() -> void:
		linear_velocity = Vector3.ZERO
		sleeping = true


class Puck extends Crate:
	pass


class Bare extends Node3D:
	var x: float:
		get:
			return position.x

	func _init() -> void:
		var glide := NetwInterpolate.new().lerp().smooth(0.0)
		Netw.configure_property(self, &"position").state().interpolate(glide)

	func slide() -> void:
		position.x += 0.05
)";

constexpr int NEAR = 0;
constexpr int FAR = 1;
constexpr int GLIDE = 10;
constexpr int WALK = 30;
constexpr int REPOINT = 40;
constexpr int LOST = 66;
constexpr int STOP = 70;
constexpr int HEALED = 76;
constexpr int BUMP = 100;
constexpr int RESUME = 120;
constexpr int NUDGE = 130;
constexpr int PUSH = 140;
constexpr int SETTLE = 150;
constexpr int SHOWN = 170;
constexpr int HOP = 180;
constexpr int DARK = 210;
constexpr int LIT = 240;
constexpr int END = 280;
constexpr double SLIDE = 0.05;

Ref<netw::NetwLinkConditions> far() {
    const Ref<netw::NetwLinkConditions> conditions = link(11);
    conditions->set_latency_ms(200.0);
    return conditions;
}

Ref<netw::NetwLinkConditions> cut() {
    const Ref<netw::NetwLinkConditions> conditions = link(13);
    conditions->set_loss_chance(1.0);
    return conditions;
}

Step headless() {
    return [](Live &, int) { smooth_when_headless(false); };
}

World street() {
    World world = World(STREET, 2)
                      .paced(60)
                      .at(1, impair(FAR, far()))
                      .watch("Glider", "aim")
                      .watch("Glider", "mark")
                      .watch("Bare", "x")
                      .at(1, spawn("Bare", "Bare"))
                      .each(GLIDE, STOP - 1, call("Bare", "slide"));
    for (const char *name : {"Glider", "Sign", "Flapper"}) {
        for (const char *fact : {"x", "drawn", "offset"}) {
            world.watch(name, fact);
        }
        world.at(1, spawn(name, name))
            .each(GLIDE, STOP - 1, call(name, "slide"));
    }
    for (const char *name : {"Crate", "Puck"}) {
        for (const char *fact : {"x", "drawn", "offset", "writes"}) {
            world.watch(name, fact);
        }
        world.watch(name, "sleeping");
        world.at(1, spawn(name, name));
    }
    return world.watch("Sign", "spare_offset")
        .watch("Walker", "x")
        .watch("Walker", "drawn")
        .at(1, spawn("Walker", "Walker"))
        .at(1, control("Walker", FAR))
        .at(5, call("Flapper", "enter_sight"))
        .at(WALK, on(FAR, set("Walker", "motion", 1)))
        .at(REPOINT, on(NEAR, call("Sign", "use_spare")))
        .at(LOST, impair(NEAR, cut()))
        .at(HEALED, heal(NEAR))
        .each(STOP, END, call("Flapper", "slide"))
        .at(BUMP, call("Walker", "bump"))
        .each(RESUME, RESUME + 19, call("Glider", "slide"))
        .at(NUDGE, on(NEAR, call("Crate", "nudge")))
        .at(PUSH, call("Puck", "nudge"))
        .at(SETTLE, call("Puck", "settle"))
        .at(SETTLE, on(FAR, set("Walker", "motion", 0)))
        .at(SETTLE, call("Flapper", "seclude"))
        .at(SHOWN, call("Flapper", "reveal"))
        .at(HOP, call("Glider", "hop"))
        .each(DARK, LIT - 1, headless())
        .each(DARK, END, call("Glider", "slide"));
}

NETW_FRAME_SCENARIO(WorldFrames, street_frames, &street, END);

LawVerdict missing(int p_peer, const char *p_name, int p_tick) {
    return law_broken(
        "%s holds no '%s' at tick %d",
        who(p_peer).c_str(),
        p_name,
        p_tick
    );
}

double fact(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_tick
) {
    const Sight *held = p_seen.sight(p_peer, p_name, p_tick);
    return held != nullptr ? double(held->facts.get(p_fact, 0.0)) : 0.0;
}

LawVerdict glides(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to,
    double p_lag,
    double p_least = 0.4 * SLIDE,
    const char *p_fact = "drawn"
) {
    for (int tick = p_from; tick <= p_to; ++tick) {
        if (p_seen.sight(p_peer, p_name, tick - 1) == nullptr
            || p_seen.sight(p_peer, p_name, tick) == nullptr) {
            return missing(p_peer, p_name, tick);
        }
        const double was = fact(p_seen, p_peer, p_name, p_fact, tick - 1);
        const double drawn = fact(p_seen, p_peer, p_name, p_fact, tick);
        const double behind = fact(p_seen, p_peer, p_name, "x", tick) - drawn;
        if (drawn - was < p_least || drawn - was > 1.6 * SLIDE) {
            return law_broken(
                "%s moved '%s' %s %f further at tick %d",
                who(p_peer).c_str(),
                p_name,
                p_fact,
                drawn - was,
                tick
            );
        }
        if (behind < -1e-3 || behind > p_lag) {
            return law_broken(
                "%s holds '%s' %s %f behind its body at tick %d",
                who(p_peer).c_str(),
                p_name,
                p_fact,
                behind,
                tick
            );
        }
    }
    return law_held();
}

LawVerdict trails(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to,
    double p_lag,
    double p_most
) {
    for (int tick = p_from; tick <= p_to; ++tick) {
        if (p_seen.sight(p_peer, p_name, tick - 1) == nullptr
            || p_seen.sight(p_peer, p_name, tick) == nullptr) {
            return missing(p_peer, p_name, tick);
        }
        const double was = fact(p_seen, p_peer, p_name, "drawn", tick - 1);
        const double drawn = fact(p_seen, p_peer, p_name, "drawn", tick);
        const double behind = fact(p_seen, p_peer, p_name, "x", tick) - drawn;
        if (drawn - was < -1e-3 || drawn - was > p_most) {
            return law_broken(
                "%s moved '%s' drawn %f further at tick %d",
                who(p_peer).c_str(),
                p_name,
                drawn - was,
                tick
            );
        }
        if (behind < -1e-3 || behind > p_lag) {
            return law_broken(
                "%s holds '%s' drawn %f behind its body at tick %d",
                who(p_peer).c_str(),
                p_name,
                behind,
                tick
            );
        }
    }
    return law_held();
}

LawVerdict lands(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    int p_from,
    int p_to
) {
    for (int tick = p_from; tick <= p_to; ++tick) {
        const double server = fact(p_seen, SERVER, p_name, "x", tick);
        const double drawn = fact(p_seen, p_peer, p_name, "drawn", tick);
        if (Math::abs(server - drawn) > 1e-3) {
            return law_broken(
                "%s drew '%s' at %f and the server holds %f at tick %d",
                who(p_peer).c_str(),
                p_name,
                drawn,
                server,
                tick
            );
        }
    }
    return law_held();
}

LawVerdict keeps(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_from,
    int p_to
) {
    const double held = fact(p_seen, p_peer, p_name, p_fact, p_from);
    for (int tick = p_from; tick <= p_to; ++tick) {
        const double now = fact(p_seen, p_peer, p_name, p_fact, tick);
        if (Math::abs(now - held) > 1e-4) {
            return law_broken(
                "%s changed '%s' %s from %f to %f at tick %d",
                who(p_peer).c_str(),
                p_name,
                p_fact,
                held,
                now,
                tick
            );
        }
    }
    return law_held();
}

LawVerdict changes(
    const Seen &p_seen,
    int p_peer,
    const char *p_name,
    const char *p_fact,
    int p_from,
    int p_to
) {
    const double held = fact(p_seen, p_peer, p_name, p_fact, p_from);
    const double now = fact(p_seen, p_peer, p_name, p_fact, p_to);
    if (Math::abs(now - held) <= 1e-4) {
        return law_broken(
            "%s kept '%s' %s at %f from tick %d to %d",
            who(p_peer).c_str(),
            p_name,
            p_fact,
            held,
            p_from,
            p_to
        );
    }
    return law_held();
}

TEST_CASE(
    "[Netw][Display][Frame] DS1 a client draws a body the server moves "
    "a little further every frame, a short way behind the body, and the "
    "smoothed arguments of its RPC and signal follow it, with no fault"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(glides(seen, NEAR, "Glider", GLIDE + 8, LOST - 1, 0.3));
    for (const char *argument : {"aim", "mark"}) {
        NETW_HOLDS(glides(
            seen, NEAR, "Glider", GLIDE + 8, LOST - 1, 0.3, 0.4 * SLIDE,
            argument
        ));
    }
    NETW_HOLDS(glides(
        seen, NEAR, "Bare", GLIDE + 8, LOST - 1, 0.3, 0.4 * SLIDE, "x"
    ));
    NETW_HOLDS(glides(seen, NEAR, "Sign", GLIDE + 8, REPOINT - 1, 0.3));
    NETW_HOLDS(quiet(seen));
}

TEST_CASE(
    "[Netw][Display][Frame] DS2 a client behind a slow link draws a "
    "moving body on every frame once its snapshots arrive steadily"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(glides(seen, FAR, "Flapper", BUMP - 10, SETTLE - 1, 0.7));
}

TEST_CASE(
    "[Netw][Display][Frame] DS3 the server draws a body a client "
    "steers a tick behind its own copy, never stepping back or jumping"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(glides(seen, SERVER, "Walker", WALK + 20, BUMP - 1, 0.35, 0.0));
    NETW_HOLDS(lands(seen, SERVER, "Walker", SHOWN, END));
}

TEST_CASE(
    "[Netw][Display][Frame] DS4 the client that steers a predicted "
    "body draws a short way behind its own prediction while the server's "
    "snapshots run late, and absorbs a correction over frames"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(trails(seen, FAR, "Walker", WALK + 10, BUMP - 1, 0.15, 0.1));
    NETW_HOLDS(trails(seen, FAR, "Walker", BUMP, SETTLE, 2.2, 0.3));
    const double drawn = fact(seen, FAR, "Walker", "drawn", END);
    const double x = fact(seen, FAR, "Walker", "x", END);
    CHECK(Math::abs(drawn - x) < 1e-3);
}

TEST_CASE(
    "[Netw][Display][Frame] DS5 when the last snapshot before a stop "
    "is lost, a client holds the body short and then draws it at the "
    "server's final pose"
) {
    const Seen &seen = street_frames.seen();
    const double server = fact(seen, SERVER, "Glider", "x", STOP);
    CHECK(fact(seen, NEAR, "Glider", "x", HEALED) < server - 0.1);
    for (int tick = LOST; tick < RESUME; ++tick) {
        const double drawn = fact(seen, NEAR, "Glider", "drawn", tick);
        CHECK(drawn <= fact(seen, NEAR, "Glider", "x", tick) + 1e-3);
    }
    NETW_HOLDS(lands(seen, NEAR, "Glider", RESUME - 10, RESUME - 1));
}

TEST_CASE(
    "[Netw][Display][Frame] DS6 a body that moves again after resting "
    "is drawn moving within a few frames, without a jump"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(keeps(seen, NEAR, "Glider", "drawn", RESUME - 10, RESUME));
    NETW_HOLDS(changes(seen, NEAR, "Glider", "drawn", RESUME, RESUME + 6));
    NETW_HOLDS(glides(seen, NEAR, "Glider", RESUME + 6, RESUME + 19, 0.3));
}

TEST_CASE(
    "[Netw][Display][Frame] DS7 a body moved further than its snap "
    "distance is drawn at the new pose in one frame, never between"
) {
    const Seen &seen = street_frames.seen();
    const double from = fact(seen, SERVER, "Glider", "x", HOP - 1);
    const double to = fact(seen, SERVER, "Glider", "x", HOP);
    for (int client : {NEAR, FAR}) {
        for (int tick = HOP; tick < DARK; ++tick) {
            const double drawn = fact(seen, client, "Glider", "drawn", tick);
            const bool either = Math::abs(drawn - from) < 1e-3
                || Math::abs(drawn - to) < 1e-3;
            CAPTURE(client);
            CAPTURE(tick);
            CHECK(either);
        }
        NETW_HOLDS(lands(seen, client, "Glider", DARK - 1, DARK - 1));
    }
}

TEST_CASE(
    "[Netw][Display][Frame] DS8 a visual root changed while the body "
    "moves draws the new visual, and the old one stops being written"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(keeps(seen, NEAR, "Sign", "offset", REPOINT + 4, END));
    NETW_HOLDS(changes(seen, NEAR, "Sign", "spare_offset", REPOINT, LOST));
    for (int tick = REPOINT + 6; tick < LOST; ++tick) {
        const double offset = fact(seen, NEAR, "Sign", "spare_offset", tick);
        CHECK(offset < 0.0);
        CHECK(offset > -0.3);
    }
}

TEST_CASE(
    "[Netw][Display][Frame] DS9 a resting body's visual is never "
    "written, and is written again on the frame the body wakes"
) {
    const Seen &seen = street_frames.seen();
    for (int client : {NEAR, FAR}) {
        NETW_HOLDS(reads(seen, client, "Crate", "sleeping", true, WALK + 30));
        NETW_HOLDS(keeps(seen, client, "Crate", "writes", WALK + 30, NUDGE));
    }
    NETW_HOLDS(keeps(seen, SERVER, "Crate", "writes", WALK + 30, END));
    NETW_HOLDS(changes(seen, NEAR, "Crate", "offset", NUDGE, NUDGE + 3));
}

TEST_CASE(
    "[Netw][Display][Frame] DS10 the server's visual of a body put to "
    "sleep while moving ends at the body's final pose"
) {
    const Seen &seen = street_frames.seen();
    NETW_HOLDS(changes(seen, SERVER, "Puck", "drawn", PUSH, PUSH + 8));
    NETW_HOLDS(reads(seen, SERVER, "Puck", "sleeping", true, SETTLE));
    const double rest = fact(seen, SERVER, "Puck", "x", END);
    for (int tick = SETTLE + 10; tick <= END; ++tick) {
        const double drawn = fact(seen, SERVER, "Puck", "drawn", tick);
        CAPTURE(tick);
        CHECK(Math::abs(drawn - rest) < 1e-3);
    }
}

TEST_CASE(
    "[Netw][Display][Frame] DS11 a client without a display writes no "
    "smoothed value until smooth_when_headless is set"
) {
    const Seen &seen = street_frames.seen();
    for (int client : {NEAR, FAR}) {
        for (const char *name : {"Glider", "Flapper"}) {
            NETW_HOLDS(changes(seen, client, name, "x", DARK + 2, LIT - 1));
            NETW_HOLDS(keeps(seen, client, name, "offset", DARK + 2, LIT - 1));
            NETW_HOLDS(
                changes(seen, client, name, "offset", LIT - 1, LIT + 6)
            );
        }
    }
}

TEST_CASE(
    "[Netw][Display][Frame] DS12 a body that leaves a client's "
    "interest and returns is drawn moving again"
) {
    const Seen &seen = street_frames.seen();
    bool gone = false;
    for (int tick = SETTLE; tick <= SHOWN; ++tick) {
        gone = gone || seen.sight(NEAR, "Flapper", tick) == nullptr;
    }
    CHECK(gone);
    NETW_HOLDS(glides(seen, NEAR, "Flapper", SHOWN + 15, DARK - 1, 0.3));
    NETW_HOLDS(glides(seen, FAR, "Flapper", LIT + 10, END, 0.7));
}

} // namespace TestDisplaySession

#endif
