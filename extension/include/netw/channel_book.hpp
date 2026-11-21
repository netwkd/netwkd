#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/rid.hpp"
#include "godot/variant.hpp"

namespace netw {

class NetwChannelBook {
    struct Row {
        godot::Callable handler;
        bool defers = false;
    };

    godot::HashMap<int64_t, Row> rows;

    const Row *row_of(int64_t channel) const;

public:
    bool register_channel(
        int64_t channel,
        const godot::Callable &handler,
        bool defers
    );

    bool unregister_channel(int64_t channel);

    godot::Callable handler_of(int64_t channel) const;

    bool defers(int64_t channel) const;

    int size() const;
    void clear();
};

} // namespace netw
