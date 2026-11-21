#include "netw/sealed.hpp"

#include "netw/entity/rows.hpp"

namespace netw::entity {

NetwEntityRecord *Rows::record(uint64_t p_id) const {
    const entity::Row *row = find(p_id);
    return row != nullptr ? row->record : nullptr;
}

NetwEntityRecord *Rows::routed_record(uint64_t p_id) const {
    const entity::Row *row = find(p_id);
    return row != nullptr ? row->routed_record() : nullptr;
}

bool Rows::settle(uint64_t p_id, bool p_unviewed) {
    entity::Row *row = find(p_id);
    if (row == nullptr) {
        return false;
    }
    if (p_unviewed) {
        row->viewed = false;
    }
    if (!row->vacant()) {
        return false;
    }
    erase(p_id);
    return true;
}

void Rows::pin(uint64_t p_id, const godot::StringName &p_label) {
    entity::Row *row = find(p_id);
    if (!p_label.is_empty()) {
        row->pinned_label = p_label;
    }
    row->pins += 1;
}

Rows::Unpin Rows::unpin(uint64_t p_id) {
    Unpin step;
    entity::Row *row = find(p_id);
    if (row == nullptr || row->pins == 0) {
        return step;
    }
    step.unpinned = true;
    row->pins -= 1;
    if (row->pins == 0) {
        step.last = true;
        row->pinned_label = godot::StringName();
        step.swept = row->swept;
        row->swept = false;
    }
    return step;
}

} // namespace netw::entity
