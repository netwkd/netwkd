#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/local_vector.hpp"
#include "godot/rid.hpp"
#include "godot/variant.hpp"

namespace netw {

struct GameCall;

class NetwEffectLedger {
    struct Entry {
        godot::Callable revert;
        int64_t deadline_tick = 0;
    };

    struct Watcher {
        godot::Callable confirmed;
        godot::Callable denied;
    };

    struct Settled {
        godot::Callable call;
        const GameCall *site = nullptr;
    };

    godot::HashMap<godot::StringName, Entry> entries;
    godot::HashMap<godot::StringName, Watcher> watchers;
    godot::LocalVector<Settled> settled;
    bool draining = false;

    void resolve(const godot::StringName &p_key, bool p_keep);

public:
    void arm(
        const godot::StringName &p_key,
        const godot::Callable &p_revert,
        int64_t p_deadline_tick
    );
    bool watch(
        const godot::StringName &p_key,
        const godot::Callable &p_confirmed,
        const godot::Callable &p_denied
    );
    void adopt(const godot::StringName &p_key);
    void discard(const godot::StringName &p_key);
    bool pending(const godot::StringName &p_key) const;
    int64_t count() const;
    void sweep(int64_t p_tick);
    void drain();
};

} // namespace netw
