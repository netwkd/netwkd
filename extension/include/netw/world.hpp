#pragma once

#include "netw/clock.hpp"
#include "netw/entity/rows.hpp"
#include "netw/store/store.hpp"
#include "netw/wire/wire.hpp"

namespace netw {

struct World {
    Clock clock;
    wire::Wire wire;
    store::Store store;
    entity::Rows entities;
};

} // namespace netw
