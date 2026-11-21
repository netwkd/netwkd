#pragma once

#include <cstdint>

#include "godot/string_name.hpp"
#include "netw/slot_rows.hpp"

namespace netw {
class NetwEntityRecord;
} // namespace netw

namespace netw::entity {

struct Row {
    NetwEntityRecord *record = nullptr;
    bool routed = false;
    uint64_t node = 0;
    bool viewed = false;
    godot::StringName pinned_label;
    uint32_t pins = 0;
    bool swept = false;

    bool vacant() const {
        return pins == 0 && record == nullptr && node == 0 && !viewed;
    }

    NetwEntityRecord *routed_record() const {
        return routed ? record : nullptr;
    }
};

struct Rows : SlotRows<Row> {
    struct Unpin {
        bool unpinned = false;
        bool last = false;
        bool swept = false;
    };

    NetwEntityRecord *record(uint64_t p_id) const;
    NetwEntityRecord *routed_record(uint64_t p_id) const;
    bool settle(uint64_t p_id, bool p_unviewed);
    void pin(uint64_t p_id, const godot::StringName &p_label);
    Unpin unpin(uint64_t p_id);
};

} // namespace netw::entity
