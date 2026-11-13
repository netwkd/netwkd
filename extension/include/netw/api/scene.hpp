#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/rid.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/enums.hpp"
#include "netw/api/promise.hpp"

namespace netw {

class NetwEntity;
class NetwMultiplayer;
class NetwPlayer;

class NetwScene : public godot::RefCounted, public enums::NetwScene {
    GDCLASS(NetwScene, godot::RefCounted)

    friend class NetwMultiplayer;

    godot::ObjectID session_id;
    godot::RID scene;

    NetwMultiplayer *core() const;
    NetwMultiplayer *live_core() const;

protected:
    static void _bind_methods();

public:
    godot::RID get_rid() const;
    godot::Ref<NetwEntity> get_entity() const;
    godot::Node *get_root() const;
    godot::Node *get_world() const;

    godot::StringName get_label() const;
    godot::TypedArray<NetwEntity> get_player_entities() const;
    godot::TypedArray<NetwPlayer> get_viewers() const;
    godot::TypedArray<NetwEntity> get_local_player_entities() const;

    godot::TypedArray<NetwEntity> get_entities() const;
    godot::Error watch(const godot::Ref<NetwPlayer> &p_player);
    godot::Error unwatch(const godot::Ref<NetwPlayer> &p_player);
    bool is_watching(const godot::Ref<NetwPlayer> &p_player) const;

    void announce_entity(
        const godot::Ref<NetwEntity> &p_entity,
        bool p_present
    );
    void announce_player_entity(
        const godot::Ref<NetwEntity> &p_entity,
        bool p_present
    );
    void announce_viewer(
        const godot::Ref<NetwPlayer> &p_player,
        bool p_present
    );
};

} // namespace netw
