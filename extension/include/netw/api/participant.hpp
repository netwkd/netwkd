#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/rid.hpp"
#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/enums.hpp"
#include "netw/api/promise.hpp"
#include "netw/api/scene.hpp"

namespace netw {

class Netw;
class NetwEntity;
class NetwMultiplayer;

class NetwPlayer : public godot::RefCounted, public enums::NetwPlayer {
    GDCLASS(NetwPlayer, godot::RefCounted)

    friend class Netw;
    friend class NetwEntity;
    friend class NetwMultiplayer;

    godot::ObjectID session_id;
    int64_t incarnation = 0;
    bool row_dropped = false;

protected:
    static void _bind_methods();

public:
    NetwMultiplayer *core() const;
    int64_t get_id() const;
    bool belongs_to(const NetwMultiplayer *p_session) const;

    int64_t get_peer_id() const;
    bool get_is_active() const;
    godot::StringName get_username() const;
    godot::String get_account() const;
    godot::String get_account_name() const;
    godot::TypedArray<NetwEntity> get_entities() const;
    godot::Ref<NetwScene> get_scene() const;

    void announce_scene_changed(
        const godot::Ref<NetwScene> &p_from,
        const godot::Ref<NetwScene> &p_to
    );
};

} // namespace netw
