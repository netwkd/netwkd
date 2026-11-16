#pragma once

#include <cstdint>

#include "godot/ref_counted.hpp"
#include "godot/string_name.hpp"
#include "netw/api/request.hpp"
#include "netw/entity/control.hpp"

namespace netw {

class NetwDespawnOpts : public godot::RefCounted {
    GDCLASS(NetwDespawnOpts, godot::RefCounted)

protected:
    static void _bind_methods();

public:
    godot::StringName reason;
    bool flush_save = true;
    bool defer_free = true;
    bool linger = false;
    double linger_seconds = 1.0;

    static godot::Ref<NetwDespawnOpts> create(
        const godot::StringName &p_reason
    );

    godot::StringName get_reason() const {
        return reason;
    }
    void set_reason(const godot::StringName &p_reason) {
        reason = p_reason;
    }
    bool get_flush_save() const {
        return flush_save;
    }
    void set_flush_save(bool p_flush_save) {
        flush_save = p_flush_save;
    }
    bool get_defer_free() const {
        return defer_free;
    }
    void set_defer_free(bool p_defer_free) {
        defer_free = p_defer_free;
    }
    bool get_linger() const {
        return linger;
    }
    void set_linger(bool p_linger) {
        linger = p_linger;
    }
    double get_linger_seconds() const {
        return linger_seconds;
    }
    void set_linger_seconds(double p_seconds) {
        linger_seconds = p_seconds;
    }
};

class NetwControlRequest : public NetwRequest {
    GDCLASS(NetwControlRequest, NetwRequest)

    int64_t hold = entity::Control::HOLD_EXCLUSIVE;

protected:
    static void _bind_methods();

public:
    static godot::Ref<NetwControlRequest> create(
        int64_t p_requester,
        int64_t p_hold
    );

    entity::Control::Hold get_hold() const {
        return entity::Control::Hold(hold);
    }
};

} // namespace netw
