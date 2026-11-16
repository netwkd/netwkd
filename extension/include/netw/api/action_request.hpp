#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"
#include "netw/api/request.hpp"

namespace netw {

class NetwMultiplayer;

class NetwActionRequest : public NetwRequest {
    GDCLASS(NetwActionRequest, NetwRequest)

    godot::ObjectID session_id;
    int64_t view_tick = 0;
    int64_t requested_tick = 0;
    int64_t execution_tick = 0;
    godot::StringName key;
    bool bound = false;

    NetwMultiplayer *session() const;

protected:
    static void _bind_methods();
    void denied() override;

public:
    void open(
        NetwMultiplayer *p_session,
        int64_t p_requester,
        int64_t p_view_tick,
        int64_t p_requested_tick,
        int64_t p_execution_tick,
        const godot::StringName &p_key
    );

    int64_t get_view_tick() const {
        return view_tick;
    }
    int64_t get_requested_tick() const {
        return requested_tick;
    }
    int64_t get_execution_tick() const {
        return execution_tick;
    }

    void bind_result(godot::Node *p_node);
    void confirm_unanswered();

    bool answered() const {
        return is_denied() || bound;
    }
};

} // namespace netw
