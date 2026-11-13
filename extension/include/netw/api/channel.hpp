#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"

namespace netw {

class NetwMultiplayer;

class NetwChannel : public godot::RefCounted {
    GDCLASS(NetwChannel, godot::RefCounted)

    int64_t id = 0;
    godot::ObjectID session;

    NetwMultiplayer *multiplayer() const;

protected:
    static void _bind_methods();

public:
    static godot::Ref<NetwChannel> over(
        int64_t p_id,
        NetwMultiplayer *p_session
    );
    static godot::Ref<NetwChannel> of(godot::Node *p_node, int64_t p_id);

    void set_id(int64_t p_id) {
        id = p_id;
    }
    int64_t get_id() const {
        return id;
    }

    void send(
        int64_t p_peer_id,
        const godot::PackedByteArray &p_payload,
        bool p_reliable,
        bool p_batched
    );
    void broadcast(
        const godot::PackedByteArray &p_payload,
        bool p_reliable,
        bool p_batched
    );
    void register_handler(const godot::Callable &p_handler);
};

} // namespace netw
