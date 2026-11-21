#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "godot/local_vector.hpp"
#include "netw/wire/tail.hpp"

namespace netw::repl {

class WatchBook {
    struct Stream {
        bool inited = false;
        godot::LocalVector<wire::TailRef> values;
        godot::LocalVector<int64_t> stamps;
        int64_t change_counter = 0;
        godot::HashMap<int64_t, int64_t> baselines;
    };

    godot::HashMap<int64_t, Stream> streams;

    Stream *stream_for(int64_t p_key);
    const Stream *stream_for(int64_t p_key) const;

public:
    static const int FIELD_LIMIT = 64;

    void poll(
        int64_t p_key,
        const godot::LocalVector<wire::TailRef> &p_values,
        const godot::LocalVector<uint8_t> &p_readable
    );

    void mask_for(
        int64_t p_key,
        int64_t p_peer,
        uint64_t &r_mask,
        godot::LocalVector<wire::TailRef> &r_values
    ) const;

    void commit(int64_t p_key, int64_t p_peer);

    void reset(int64_t p_key);

    void clear_baselines(int64_t p_key);

    void clear_peer(int64_t p_peer);

    void retain_baselines(
        int64_t p_key,
        const int32_t *p_recipients,
        int64_t p_count
    );

    bool is_inited(int64_t p_key) const;

    void clear();
};

} // namespace netw::repl
