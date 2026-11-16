#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"

namespace netw::sim {

enum class Pick : int8_t {
    AUTOMATIC = -1,
    EXCLUDED = 0,
    CHOSEN = 1,
};

struct Tenure {
    int64_t begin = -1;
    int64_t end = -1;

    bool contains(int64_t p_transition) const;
};

struct Candidate {
    int64_t key = 0;
    int64_t order_key = 0;
    Pick pick = Pick::AUTOMATIC;
    bool eligible = false;
};

struct Selected {
    int64_t key = 0;
    int64_t order_key = 0;
    Tenure tenure;
    bool chosen = false;
    bool promoted = false;
    bool lingering = false;
    bool present = false;
    bool eligible = false;
};

struct Selection {
    godot::LocalVector<Selected> members;
    int64_t owner_order_key = 0;
    bool all = false;

    void commit(
        const godot::LocalVector<Candidate> &p_candidates,
        int64_t p_frontier
    );
    void release_lingering(int64_t p_floor);
    const Selected *member(int64_t p_key) const;
    int present_count() const;
    int promoted_count() const;
    int lingering_count() const;
};

struct IdSet {
    godot::LocalVector<uint64_t> ids;

    bool note(uint64_t p_id);
    bool erase(uint64_t p_id);

    uint32_t count() const {
        return ids.size();
    }
};

} // namespace netw::sim
