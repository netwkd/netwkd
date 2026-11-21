#pragma once

#include <cstdint>

#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"

namespace netw {

class NetwRequest : public godot::RefCounted {
    GDCLASS(NetwRequest, godot::RefCounted)

protected:
    int64_t peer_id = 0;
    godot::Error code = godot::OK;
    godot::String reason;

    static void _bind_methods();
    virtual void denied() {
    }

public:
    void deny(
        const godot::String &p_reason = godot::String(),
        godot::Error p_code = godot::ERR_UNAUTHORIZED
    );

    int64_t get_peer_id() const {
        return peer_id;
    }
    godot::Error get_code() const {
        return code;
    }
    godot::String get_reason() const {
        return reason;
    }
    bool is_denied() const {
        return code != godot::OK;
    }
};

} // namespace netw
