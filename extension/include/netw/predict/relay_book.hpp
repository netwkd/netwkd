#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "godot/local_vector.hpp"
#include "godot/variant.hpp"

namespace netw::predict {

class RelayBook {
    godot::HashMap<int64_t, godot::LocalVector<int64_t>> rows;
    godot::HashMap<int64_t, uint32_t> generations;
    uint32_t newest_generation = 0;

public:
    void set_subscribed(
        int64_t p_entity_slot,
        int64_t p_peer,
        bool p_subscribed
    );

    bool subscribed(int64_t p_entity_slot, int64_t p_peer) const;

    godot::PackedInt64Array peers(int64_t p_entity_slot) const;

    int peer_count(int64_t p_entity_slot) const;

    void audience_changed(int64_t p_entity_slot);

    uint32_t generation(int64_t p_entity_slot) const;

    void release(int64_t p_entity_slot);

    int slot_count() const;

    static godot::PackedByteArray request_bytes(bool p_subscribed);

    static int request_of(const godot::PackedByteArray &p_bytes);
};

} // namespace netw::predict
