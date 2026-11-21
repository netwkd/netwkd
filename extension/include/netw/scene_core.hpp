#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/local_vector.hpp"
#include "godot/node.hpp"
#include "godot/rid.hpp"
#include "godot/templates.hpp"
#include "godot/utility.hpp"
#include "godot/variant.hpp"

namespace netw {

class NetwSceneCore {
public:
    enum Move {
        MOVE_REFUSED,
        MOVE_ALREADY_THERE,
        MOVE_CARRY,
    };

    enum Isolation {
        ISOLATION_NONE = 0,
        ISOLATION_OWN_WORLD = 1,
    };

private:
    struct LiveRow {
        godot::RID scene;
        godot::StringName stem;
        bool owns_its_world = false;
        godot::LocalVector<int64_t> parked;
    };

    godot::LocalVector<LiveRow> live;
    godot::HashMap<godot::StringName, godot::RID> named;

    void promote_survivors();

public:
    NetwSceneCore() = default;
    NetwSceneCore(const NetwSceneCore &) = delete;
    NetwSceneCore &operator=(const NetwSceneCore &) = delete;

    void scene_enter(
        const godot::RID &scene,
        const godot::StringName &stem,
        bool owns_its_world
    );
    void scene_exit(const godot::RID &scene);

    bool is_live(const godot::RID &scene) const;
    godot::RID scene_named(const godot::StringName &stem) const;
    godot::Array scenes_named(const godot::StringName &stem) const;
    godot::Array live_scenes() const;
    godot::StringName stem_of(const godot::RID &scene) const;
    int live_count() const;

    static bool isolation_owns_world(int isolation);

    static Move move_verdict(
        bool mover_live,
        bool target_live,
        bool same_scene
    );

    bool admission_park(const godot::RID &scene, int64_t peer);
    bool admission_unpark(const godot::RID &scene, int64_t peer);
    bool admission_is_parked(const godot::RID &scene, int64_t peer) const;

    void clear();
};

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwSceneCore::Isolation);
VARIANT_ENUM_CAST(netw::NetwSceneCore::Move);
