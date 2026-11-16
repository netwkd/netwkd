#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"

namespace netw {

class NetwActionConfig : public godot::RefCounted {
    GDCLASS(NetwActionConfig, godot::RefCounted)

public:
    enum Timing {
        TIMING_TICK_ALIGNED = 0,
        TIMING_TICK_ALIGNED_STATE_READY = 1,
        TIMING_IMMEDIATE = 2,
    };

private:
    godot::Callable authority_handler;
    godot::Callable predict_handler;
    godot::Callable confirm_handler;
    godot::Callable revert_handler;
    Timing timing_mode = TIMING_IMMEDIATE;
    int64_t timeout = 0;
    bool refused = false;

protected:
    static void _bind_methods();

public:
    void refuse() {
        refused = true;
    }
    bool is_refused() const {
        return refused;
    }

    godot::Ref<NetwActionConfig> predict(const godot::Callable &p_handler);
    godot::Ref<NetwActionConfig> confirm(const godot::Callable &p_handler);
    godot::Ref<NetwActionConfig> revert(const godot::Callable &p_handler);
    godot::Ref<NetwActionConfig> timing(Timing p_mode);
    godot::Ref<NetwActionConfig> timeout_ticks(int64_t p_ticks);

    void set_authority(const godot::Callable &p_method) {
        authority_handler = p_method;
    }
    godot::Callable get_authority() const {
        return authority_handler;
    }
    godot::Callable get_predict() const {
        return predict_handler;
    }
    godot::Callable get_confirm() const {
        return confirm_handler;
    }
    godot::Callable get_revert() const {
        return revert_handler;
    }
    Timing get_timing() const {
        return timing_mode;
    }
    int64_t get_timeout_ticks() const {
        return timeout;
    }
};

godot::Ref<NetwActionConfig> action_declared_on(
    godot::Node *p_node,
    const godot::StringName &p_method
);

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwActionConfig::Timing);
