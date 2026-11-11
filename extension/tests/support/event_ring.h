#pragma once

#include "netw_test.h"

#include "godot/variant.hpp"
#include "netw/session/event_plane.hpp"

namespace netw_test {

class EventRing {
    godot::PackedInt64Array drained;

    int64_t column_at(int p_index, int p_column) const {
        if (p_index < 0 || p_index >= size()) {
            return -1;
        }
        return drained
            [int64_t(p_index) * netw::EventPlane::ROW_WIDTH + p_column];
    }

public:
    EventRing() = default;
    explicit EventRing(const godot::PackedInt64Array &p_drained)
        : drained(p_drained) {
    }

    int size() const {
        return int(drained.size() / netw::EventPlane::ROW_WIDTH);
    }

    bool is_empty() const {
        return size() == 0;
    }

    int64_t event_at(int p_index) const {
        return column_at(p_index, 0);
    }

    int64_t tick_at(int p_index) const {
        return column_at(p_index, 1);
    }

    int64_t entity_at(int p_index) const {
        return column_at(p_index, 2);
    }

    int64_t route_at(int p_index) const {
        return column_at(p_index, 3);
    }

    int64_t peer_at(int p_index) const {
        return column_at(p_index, 4);
    }

    int64_t verdict_at(int p_index) const {
        return column_at(p_index, 5);
    }

    int64_t a_at(int p_index) const {
        return column_at(p_index, 6);
    }

    int64_t b_at(int p_index) const {
        return column_at(p_index, 7);
    }

    int count_of(int64_t p_event) const {
        int seen = 0;
        for (int index = 0; index < size(); index++) {
            seen += event_at(index) == p_event ? 1 : 0;
        }
        return seen;
    }

    int count_of(int64_t p_event, int64_t p_route) const {
        int seen = 0;
        for (int index = 0; index < size(); index++) {
            seen += event_at(index) == p_event && route_at(index) == p_route
                ? 1
                : 0;
        }
        return seen;
    }

    int first_of(int64_t p_event) const {
        for (int index = 0; index < size(); index++) {
            if (event_at(index) == p_event) {
                return index;
            }
        }
        return -1;
    }
};

} // namespace netw_test
