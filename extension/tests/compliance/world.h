#pragma once

#include "tests/support/fault_log.h"
#include "tests/support/frame_drive.h"
#include "tests/support/internals.h"
#include "tests/support/link.h"
#include "tests/support/loopback_rig.h"
#include "tests/support/minted_script.h"
#include "tests/support/netw_call_log.h"
#include "tests/support/netw_reset.h"
#include "tests/support/scenario_capture.h"

#include "netw/api/context.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/session.hpp"

#include <godot_cpp/classes/packed_scene.hpp>

#include <climits>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#if defined(NETW_TIER_HOSTED)

namespace netw_test {

constexpr int SERVER = -1;
constexpr int LAST = -1;
constexpr int TICKRATE = 30;
constexpr double TICK_MS = 1000.0 / TICKRATE;
constexpr const char *WORLDS = "res://extension/tests/compliance/";
typedef netw::NetwSession::TransferMode Transfer;

constexpr const char *LEVEL_HEAD = R"(extends Node3D


func make_scene(scene: String, called: String) -> Node:
	var scenes: Dictionary = (get_script() as Script).get_script_constant_map()
	var made: Node = scenes[scene].new()
	made.name = called
	return made

)";

inline std::string who(int p_peer) {
    return p_peer == SERVER ? std::string("the server")
                            : "client " + std::to_string(p_peer);
}

inline godot::Variant plain(const godot::Variant &p_value) {
    if (p_value.get_type() == godot::Variant::STRING_NAME) {
        return godot::String(p_value);
    }
    if (p_value.get_type() != godot::Variant::OBJECT) {
        return p_value;
    }
    godot::Object *held = netw::gd::live_object(p_value);
    if (auto *player = godot::Object::cast_to<netw::NetwPlayer>(held)) {
        return godot::String(player->get_username());
    }
    godot::Node *node = godot::Object::cast_to<godot::Node>(held);
    return node != nullptr ? godot::String(node->get_name())
                           : godot::String("an object");
}

struct Sight {
    int tick = 0;
    int peer = SERVER;
    godot::String name;
    uint64_t node = 0;
    godot::Dictionary facts;
};

struct Heard {
    int tick = 0;
    int peer = SERVER;
    godot::String node;
    godot::String signal;
    godot::Array args;
};

struct Seen {
    int ticks = 0;
    std::vector<Sight> sights;
    std::vector<Heard> heard;
    godot::PackedStringArray faults;
    std::vector<godot::PackedByteArray> wire;

    const Sight *sight(
        int p_peer,
        const godot::String &p_name,
        int p_tick = LAST
    ) const {
        const int tick = p_tick == LAST ? ticks : p_tick;
        for (const Sight &held : sights) {
            if (held.tick == tick && held.peer == p_peer
                && held.name == p_name) {
                return &held;
            }
        }
        return nullptr;
    }

    int times(
        int p_peer,
        const godot::String &p_node,
        const godot::String &p_signal,
        int p_from = 0,
        int p_to = INT_MAX
    ) const {
        int total = 0;
        for (const Heard &row : heard) {
            total += row.peer == p_peer && row.node == p_node
                && row.signal == p_signal && row.tick >= p_from
                && row.tick <= p_to;
        }
        return total;
    }
};

struct Live {
    LoopbackRig rig;
    godot::Ref<godot::Resource> game;
    std::vector<godot::Node *> screens;
    CallLog calls;
    godot::Ref<FaultLog> faults;
    std::set<std::string> wired;
    int drained = 0;

    static LoopbackRig made(int p_clients, bool p_captured, int p_mode) {
        if (p_captured) {
            reset_for_case();
        }
        return LoopbackRig(
            p_clients,
            p_mode >= 0,
            p_captured ? adopt_into_memory : nullptr
        );
    }

    Live(const char *p_game, int p_clients, bool p_captured, int p_mode)
        : rig(made(p_clients, p_captured, p_mode)) {
        faults.instantiate();
        FaultLog::attach(faults);
        const godot::String text(p_game);
        if (text.ends_with(".tscn")) {
            game = godot::ResourceLoader::get_singleton()->load(WORLDS + text);
        } else {
            game = minted_script((LEVEL_HEAD + text).utf8().get_data());
        }
        rig.declare_world(WorldDecl().clocked(TICKRATE));
        for (int peer = SERVER; peer < rig.count(); ++peer) {
            seat(peer);
            if (p_mode >= 0) {
                session(peer)->transfer_set_mode(Transfer(p_mode));
            }
        }
        rig.pump(4);
    }

    ~Live() {
        for (godot::Node *screen : screens) {
            screen->get_parent()->remove_child(screen);
            memdelete(screen);
        }
        netw::gd::scene_tree()->set_pause(false);
        FaultLog::detach(faults);
    }

    int index(int p_peer) const {
        int host = SERVER;
        for (int client = 0; client < rig.count(); ++client) {
            host = rig.peer_id(client) == 1 ? client : host;
        }
        return p_peer == SERVER ? host : p_peer == host ? SERVER : p_peer;
    }

    netw::NetwMultiplayer *session(int p_peer) const {
        const int at = index(p_peer);
        return at == SERVER ? rig.server() : rig.client(at);
    }

    void seat(int p_peer) {
        netw::MultiplayerViewport *screen = memnew(netw::MultiplayerViewport);
        screen->set_name(godot::vformat("Screen%d", p_peer + 1));
        screen->set_api(godot::Ref<netw::NetwMultiplayer>(session(p_peer)));
        netw::gd::scene_root()->add_child(screen);
        screens.push_back(screen);
        godot::PackedScene *packed
            = godot::Object::cast_to<godot::PackedScene>(game.ptr());
        godot::Node *level = packed != nullptr ? packed->instantiate()
            : game.is_valid()
            ? godot::Object::cast_to<godot::Node>(game->call("new"))
            : nullptr;
        if (level == nullptr) {
            ERR_PRINT("The world's game made no level.");
            return;
        }
        level->set_name("Level");
        screen->add_child(level);
        if (level->has_method("make_scene")) {
            netw::Netw::configure_spawn(godot::Callable(level, "make_scene"));
        }
    }

    void admit(const char *p_username) {
        seat(rig.late_client(godot::StringName(p_username)));
    }

    Sight roster(int p_peer, int p_tick) const {
        const godot::Ref<netw::NetwSession> held
            = session(p_peer)->get_session();
        const godot::TypedArray<netw::NetwPlayer> players = held->get_players();
        godot::PackedStringArray names;
        for (int at = 0; at < players.size(); ++at) {
            const godot::Ref<netw::NetwPlayer> player = players[at];
            names.push_back(who_is(player->get_peer_id()));
        }
        names.sort();
        Sight row{p_tick, p_peer, "Session"};
        row.facts["players"] = godot::String(", ").join(names);
        row.facts["paused"] = held->is_paused();
        row.facts["online"] = held->get_is_online();
        row.facts["frozen"] = netw::gd::scene_tree()->is_paused();
        return row;
    }

    void frame(double p_delta) {
        for (int peer = SERVER; peer < rig.count(); ++peer) {
            session(peer)->clock_physics_step(p_delta);
        }
        rig.session()->advance_time(1000.0 * p_delta);
        for (int peer = SERVER; peer < rig.count(); ++peer) {
            session(peer)->poll();
        }
    }

    godot::String who_is(int64_t p_peer_id) const {
        for (int peer = SERVER; peer < rig.count(); ++peer) {
            if (rig.peer_id(index(peer)) == p_peer_id) {
                return who(peer).c_str();
            }
        }
        return "nobody";
    }

    godot::Node *find(int p_peer, const godot::String &p_name) const {
        godot::Node *level
            = screens[index(p_peer) + 1]->get_node_or_null("Level");
        if (level == nullptr || p_name == "Level") {
            return level;
        }
        return level->find_child(p_name, true, false);
    }

    godot::Node *need(int p_peer, const char *p_name) const {
        godot::Node *found = find(p_peer, p_name);
        if (found == nullptr) {
            ERR_PRINT(
                godot::vformat(
                    "A step names '%s', which %s does not hold.",
                    p_name,
                    who(p_peer).c_str()
                )
            );
        }
        return found;
    }

    godot::Ref<netw::NetwEntity> entity(int p_peer, const char *p_name) const {
        godot::Node *node = need(p_peer, p_name);
        const godot::Ref<netw::NetwEntity> held = node != nullptr
            ? netw::NetwEntity::of(node)
            : godot::Ref<netw::NetwEntity>();
        if (node != nullptr && held.is_null()) {
            ERR_PRINT(
                godot::vformat("A step treats '%s' as an entity.", p_name)
            );
        }
        return held;
    }

    godot::Object *emitter(
        int p_peer,
        const godot::String &p_name,
        const godot::String &p_signal
    ) const {
        if (p_name == "Session") {
            return session(p_peer)->get_session().ptr();
        }
        godot::Node *node = find(p_peer, p_name);
        if (node == nullptr || node->has_signal(p_signal)) {
            return node;
        }
        const godot::Ref<netw::NetwEntity> held = netw::NetwEntity::of(node);
        return held.is_valid() && held->has_signal(p_signal) ? held.ptr()
                                                             : nullptr;
    }
};

typedef std::function<void(Live &, int)> Step;

inline Step on(int p_client, const Step &p_step) {
    return [=](Live &p_live, int) { p_step(p_live, p_client); };
}

inline Step spawn(
    const char *p_scene,
    const char *p_name,
    const char *p_parent = "Level"
) {
    return [=](Live &p_live, int p_peer) {
        godot::Node *level = p_live.need(p_peer, "Level");
        godot::Node *parent = p_live.need(p_peer, p_parent);
        if (level == nullptr || parent == nullptr) {
            return;
        }
        godot::Node *made = netw::Netw::spawn(
            godot::Callable(level, "make_scene"),
            godot::Array::make(godot::String(p_scene), godot::String(p_name))
        );
        if (made != nullptr) {
            parent->add_child(made);
        }
    };
}

inline Step despawn(const char *p_name, const char *p_reason = "") {
    return [=](Live &p_live, int p_peer) {
        if (godot::Node *node = p_live.need(p_peer, p_name)) {
            netw::Netw::despawn(
                node,
                godot::String(p_reason).is_empty()
                    ? godot::Ref<netw::NetwDespawnOpts>()
                    : netw::NetwDespawnOpts::create(p_reason)
            );
        }
    };
}

inline Step reparent(const char *p_name, const char *p_parent) {
    return [=](Live &p_live, int p_peer) {
        godot::Node *node = p_live.need(p_peer, p_name);
        godot::Node *parent = p_live.need(p_peer, p_parent);
        if (node != nullptr && parent != nullptr) {
            netw::Netw::reparent(node, parent);
        }
    };
}

inline Step set(
    const char *p_name,
    const char *p_property,
    const godot::Variant &p_value
) {
    return [=](Live &p_live, int p_peer) {
        if (godot::Node *node = p_live.need(p_peer, p_name)) {
            node->set(p_property, p_value);
        }
    };
}

inline Step call(
    const char *p_name,
    const char *p_method,
    const godot::Array &p_args = godot::Array()
) {
    return [=](Live &p_live, int p_peer) {
        if (godot::Node *node = p_live.need(p_peer, p_name)) {
            node->callv(p_method, p_args);
        }
    };
}

inline Step control(const char *p_name, int p_client) {
    return [=](Live &p_live, int p_peer) {
        const godot::Ref<netw::NetwEntity> held = p_live.entity(p_peer, p_name);
        if (held.is_valid()) {
            held->set_controller_player(
                p_client == SERVER ? godot::Ref<netw::NetwPlayer>()
                                   : p_live.rig.player(p_client)
            );
        }
    };
}

inline Step claim(const char *p_name) {
    return [=](Live &p_live, int p_peer) {
        const godot::Ref<netw::NetwEntity> held = p_live.entity(p_peer, p_name);
        if (held.is_valid()) {
            held->claim_control();
        }
    };
}

inline Step release(const char *p_name) {
    return [=](Live &p_live, int p_peer) {
        const godot::Ref<netw::NetwEntity> held = p_live.entity(p_peer, p_name);
        if (held.is_valid()) {
            held->release_control();
        }
    };
}

inline Step join(const char *p_username) {
    return [=](Live &p_live, int) { p_live.admit(p_username); };
}

inline Step leave() {
    return [=](Live &p_live, int p_peer) {
        if (godot::Node *level = p_live.need(p_peer, "Level")) {
            netw::Netw::session(level)->leave();
        }
    };
}

inline Step pause(const char *p_reason) {
    return [=](Live &p_live, int p_peer) {
        p_live.session(p_peer)->get_session()->pause(p_reason);
    };
}

inline Step unpause() {
    return [=](Live &p_live, int p_peer) {
        p_live.session(p_peer)->get_session()->unpause();
    };
}

inline Step impair(
    int p_client,
    const godot::Ref<netw::NetwLinkConditions> &p_conditions
) {
    return [=](Live &p_live, int) {
        LoopbackRig &rig = p_live.rig;
        rig.conditions(p_client, p_conditions, rig.peer_id(SERVER));
        rig.conditions(SERVER, p_conditions, rig.peer_id(p_client));
    };
}

inline Step heal(int p_client) {
    return impair(p_client, link());
}

inline Step transfer(int p_client) {
    return [=](Live &p_live, int p_peer) {
        p_live.session(p_peer)->get_session()->transfer_server(
            p_live.rig.peer_id(p_live.index(p_client))
        );
    };
}

inline Step lose_server() {
    return [](Live &p_live, int) { p_live.rig.session()->lose_server(); };
}

class World {
    struct Timed {
        int first = 0;
        int last = 0;
        Step step;
    };

    typedef std::pair<godot::String, godot::String> Named;

    const char *game = nullptr;
    int clients = 0;
    int pace = 0;
    bool capturing = false;
    int mode = -1;
    std::vector<Timed> steps;
    std::vector<Named> watched;
    std::vector<Named> listened;
    std::shared_ptr<Live> live;
    Seen evidence;

    void wire() {
        for (int peer = SERVER; peer < live->rig.count(); ++peer) {
            for (const Named &row : listened) {
                godot::Object *source
                    = live->emitter(peer, row.first, row.second);
                if (source == nullptr) {
                    continue;
                }
                const godot::String tag = godot::vformat(
                    "%d|%s|%s",
                    live->index(peer),
                    row.first,
                    row.second
                );
                const std::string key = std::string(tag.utf8().get_data())
                    + std::to_string(source->get_instance_id());
                if (live->wired.insert(key).second) {
                    source->connect(row.second, live->calls.callable(tag));
                }
            }
        }
    }

    void collect() {
        const godot::Vector<godot::StringName> order = live->calls.order();
        for (; live->drained < order.size(); ++live->drained) {
            const godot::StringName tag = order[live->drained];
            const godot::PackedStringArray parts
                = godot::String(tag).split("|");
            const int peer = live->index(int(parts[0].to_int()));
            Heard row{evidence.ticks, peer, parts[1], parts[2]};
            int earlier = 0;
            for (int at = 0; at < live->drained; ++at) {
                earlier += order[at] == tag;
            }
            const godot::Array told = live->calls.args(tag, earlier);
            for (int at = 0; at < told.size(); ++at) {
                row.args.push_back(plain(told[at]));
            }
            evidence.heard.push_back(row);
        }
    }

    void look(godot::Node *p_node, godot::Node *p_level, int p_peer) {
        if (p_node->is_queued_for_deletion()) {
            return;
        }
        const godot::Ref<netw::NetwEntity> entity
            = netw::NetwEntity::of(p_node);
        const bool rooted = entity.is_valid() && entity->get_owner() == p_node;
        Sight sight;
        sight.tick = evidence.ticks;
        sight.peer = p_peer;
        sight.name = p_node->get_name();
        sight.node = uint64_t(p_node->get_instance_id());
        for (const Named &row : watched) {
            if (row.first == sight.name) {
                sight.facts[row.second] = p_node->get(row.second);
            }
        }
        if (rooted) {
            const godot::Ref<netw::NetwPlayer> controller
                = entity->get_controller_player();
            sight.facts["controller"] = controller.is_valid()
                ? live->who_is(controller->get_peer_id())
                : godot::String("the server");
            sight.facts["authority"]
                = live->who_is(p_node->get_multiplayer_authority());
        }
        if (rooted || !sight.facts.is_empty()) {
            sight.facts["parent"] = godot::String(
                p_level->get_parent()->get_path_to(p_node->get_parent())
            );
            evidence.sights.push_back(sight);
        }
        for (int at = 0; at < p_node->get_child_count(); ++at) {
            look(p_node->get_child(at), p_level, p_peer);
        }
    }

public:
    World(const char *p_game, int p_clients)
        : game(p_game), clients(p_clients) {
    }

    World &watch(const char *p_node, const char *p_property) {
        watched.push_back({p_node, p_property});
        return *this;
    }

    World &hear(const char *p_node, const char *p_signal) {
        listened.push_back({p_node, p_signal});
        return *this;
    }

    World &at(int p_tick, const Step &p_step) {
        steps.push_back({p_tick, p_tick, p_step});
        return *this;
    }

    World &each(int p_first, int p_last, const Step &p_step) {
        steps.push_back({p_first, p_last, p_step});
        return *this;
    }

    World &paced(int p_physics_fps) {
        pace = p_physics_fps;
        return *this;
    }

    World &capture() {
        capturing = true;
        return *this;
    }

    World &migrates(Transfer p_mode) {
        mode = p_mode;
        return *this;
    }

    int ticks() const {
        return evidence.ticks;
    }

    void advance() {
        if (!live) {
            live = std::make_shared<Live>(game, clients, capturing, mode);
            wire();
        }
        collect();
        evidence.ticks += 1;
        for (const Timed &timed : steps) {
            if (timed.first <= evidence.ticks && evidence.ticks <= timed.last) {
                timed.step(*live, SERVER);
            }
        }
        wire();
        if (pace > 0) {
            live->frame(1.0 / pace);
        } else {
            live->rig.step_ticks(1);
        }
        wire();
        for (int peer = SERVER; peer < live->rig.count(); ++peer) {
            if (godot::Node *level = live->find(peer, "Level")) {
                look(level, level, peer);
            }
            evidence.sights.push_back(live->roster(peer, evidence.ticks));
        }
    }

    Seen finish() {
        collect();
        for (const FaultLine &line : live->faults->recorded()) {
            if (!line.file.ends_with("jolt_job_system.cpp")) {
                evidence.faults.push_back(line.text);
            }
        }
        for (int peer = SERVER; capturing && peer < live->rig.count(); ++peer) {
            evidence.wire.push_back(
                Internals::capture_writer(live->session(peer)).memory()
            );
        }
        live.reset();
        return evidence;
    }

    Seen run(int p_ticks) {
        while (evidence.ticks < p_ticks) {
            advance();
        }
        return finish();
    }
};

class WorldFrames final : public FrameScenario {
    World (*declare)() = nullptr;
    int frames = 0;
    World *world = nullptr;
    Seen *evidence = nullptr;

public:
    WorldFrames(World (*p_declare)(), int p_frames)
        : declare(p_declare), frames(p_frames) {
    }

    bool advance() override {
        if (evidence != nullptr || netw::gd::scene_root() == nullptr) {
            return false;
        }
        if (world == nullptr) {
            world = new World(declare());
        }
        world->advance();
        if (world->ticks() < frames) {
            return true;
        }
        evidence = new Seen(world->finish());
        delete world;
        world = nullptr;
        return false;
    }

    const Seen &seen() const {
        static const Seen *nothing = new Seen();
        CHECK_MESSAGE(evidence != nullptr, "the frame stage never ran");
        return evidence != nullptr ? *evidence : *nothing;
    }
};

} // namespace netw_test

#endif
