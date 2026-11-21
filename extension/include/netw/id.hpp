#pragma once

#include <cstdint>

#include "godot/rid.hpp"

#define NETW_ID(p_name) \
    struct p_name { \
        godot::RID rid; \
\
        p_name() = default; \
        explicit p_name(const godot::RID &p_rid) : rid(p_rid) { \
        } \
\
        uint64_t get_id() const { \
            return rid.get_id(); \
        } \
        bool is_valid() const { \
            return rid.is_valid(); \
        } \
    }

namespace netw::id {

NETW_ID(Entity);
NETW_ID(Layer);

} // namespace netw::id

#undef NETW_ID
