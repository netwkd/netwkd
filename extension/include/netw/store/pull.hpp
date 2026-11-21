#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "godot/local_vector.hpp"
#include "godot/object.hpp"
#include "godot/variant.hpp"
#include "netw/store/port.hpp"
#include "netw/store/resident.hpp"
#include "netw/store/store.hpp"

namespace netw::store {

class Pull {
    Store *store = nullptr;
    godot::HashMap<uint64_t, uint32_t> body_rows;
    godot::LocalVector<uint32_t> visiting;
    wire::CodeRow probe;

    const NodeCell &resident(Table &p_table, uint32_t p_row);
    bool readable(Table &p_table, uint32_t p_row, godot::Object *p_node);
    void note_rest(Table &p_table, uint32_t p_row, const NodeCell &p_cell);
    void follow_authority(
        Table &p_table,
        uint32_t p_row,
        godot::Object *p_node
    );
    void follow_rest(Table &p_table, uint32_t p_row, const BodyState &p_body);
    bool read_columns(
        Table &p_table,
        uint32_t p_row,
        godot::Object *p_node,
        BodyState &p_body,
        bool p_body_columns,
        wire::CodeRow &r_codes,
        bool p_keep
    );
    void pull_rows(Table &p_table, int64_t p_tick, bool p_validating);
    void pull_row(
        Table &p_table,
        uint32_t p_row,
        int64_t p_tick,
        bool p_validating,
        bool p_gated,
        bool p_swept
    );
    void audit_quiet(Table &p_table, int64_t p_tick);
    void check_codes(
        Table &p_table,
        uint32_t p_row,
        godot::Object *p_node,
        BodyState &p_body,
        int64_t p_tick,
        const char *p_gate
    );
    void count_probe(const RowAddress &p_at, bool p_reads);

public:
    Residents residents;
    godot::LocalVector<uint64_t> reprobed;

    void seat(Store *p_store) {
        store = p_store;
    }

    void pull(uint32_t p_table, int64_t p_tick);

    bool read_live(
        const RowAddress &p_at,
        godot::LocalVector<godot::Variant> &r_values
    );

    RowAddress add_row(
        uint32_t p_table,
        uint64_t p_owner,
        uint32_t p_lane,
        uint64_t p_node,
        uint8_t p_comp
    );

    void remove_row(const RowAddress &p_at);

    void remove_owner(uint64_t p_owner);

    void set_source(const RowAddress &p_at, Source p_source);

    bool probes(uint64_t p_node) const {
        return body_rows.has(p_node);
    }

    void clear();

#if defined(NETW_PROFILING) || defined(DEBUG_ENABLED)
    int64_t held_bytes() const;
#endif
};

} // namespace netw::store
