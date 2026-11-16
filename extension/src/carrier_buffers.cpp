#include "netw/carrier_buffers.hpp"

#include <algorithm>
#include <utility>

#include "netw/log.hpp"
#include "netw/wire/registry.hpp"

namespace netw {

using namespace godot;

void CarrierBatch::take_frame(
    const PackedByteArray &p_frame,
    int64_t p_deadline_ms
) {
    held.append_array(p_frame);
    deadline_ms
        = frames == 0 ? p_deadline_ms : std::min(deadline_ms, p_deadline_ms);
    frames += 1;
}

bool CarrierBatch::rides(int64_t p_now_ms) {
    if (deadline_ms <= 0 || held.is_empty()) {
        return false;
    }
    if (riding_since_ms < 0) {
        riding_since_ms = p_now_ms;
        return true;
    }
    return p_now_ms >= riding_since_ms
        && p_now_ms - riding_since_ms < deadline_ms;
}

void CarrierBatch::attach(const CarrierRow &p_row) {
    bits += p_row.bits;
    for (uint32_t at = 0; at < descriptors.size(); ++at) {
        if (descriptors[at].token != p_row.token
            || descriptors[at].route != p_row.route
            || descriptors[at].comp != p_row.comp) {
            continue;
        }
        if (p_row.revision >= descriptors[at].revision) {
            descriptors[at] = p_row;
        }
        return;
    }
    descriptors.push_back(p_row);
}

void CarrierBatch::close_route(int64_t p_route) {
    uint32_t kept = 0;
    for (uint32_t at = 0; at < descriptors.size(); ++at) {
        if (descriptors[at].route == p_route) {
            continue;
        }
        if (kept != at) {
            descriptors[kept] = descriptors[at];
        }
        ++kept;
    }
    descriptors.resize(kept);
}

void CarrierBatch::release() {
    held.clear();
    descriptors.clear();
    frames = 0;
    bits = 0;
    riding_since_ms = -1;
    deadline_ms = 0;
}

void CarrierBatch::swap(CarrierBatch &r_other) {
    std::swap(held, r_other.held);
    std::swap(descriptors, r_other.descriptors);
    std::swap(frames, r_other.frames);
    std::swap(bits, r_other.bits);
    std::swap(riding_since_ms, r_other.riding_since_ms);
    std::swap(deadline_ms, r_other.deadline_ms);
}

HashMap<int64_t, CarrierBatch> &NetwCarrierBuffers::lane(bool p_reliable) {
    return p_reliable ? reliable : unreliable;
}

const HashMap<int64_t, CarrierBatch> &NetwCarrierBuffers::lane(
    bool p_reliable
) const {
    return p_reliable ? reliable : unreliable;
}

CarrierBatch NetwCarrierBuffers::append(
    int64_t p_peer,
    const PackedByteArray &p_frame,
    bool p_reliable,
    int64_t p_budget,
    const CarrierRow *p_row,
    int64_t p_deadline_ms
) {
    HashMap<int64_t, CarrierBatch> &runs = lane(p_reliable);
    CarrierBatch owed;
    CarrierBatch *run = runs.getptr(p_peer);
    if (run == nullptr) {
        runs.insert(p_peer, CarrierBatch());
        run = runs.getptr(p_peer);
    }

    const bool overflows = !p_reliable && !run->is_empty()
        && run->size() + int64_t(p_frame.size()) > p_budget;
    if (overflows) {
        NETW_TRACE(
            sys::TRANSPORT,
            "Sending early to peer %d, because %d held bytes and a %d byte "
            "frame exceed %d.",
            p_peer,
            int(run->size()),
            p_frame.size(),
            p_budget
        );
        owed.swap(*run);
    }
    run->take_frame(p_frame, p_deadline_ms);
    if (p_row != nullptr) {
        run->attach(*p_row);
    }
    return owed;
}

bool NetwCarrierBuffers::rides(int64_t p_peer, int64_t p_now_ms) {
    CarrierBatch *run = unreliable.getptr(p_peer);
    return run != nullptr && run->rides(p_now_ms);
}

CarrierBatch NetwCarrierBuffers::take(int64_t p_peer, bool p_reliable) {
    HashMap<int64_t, CarrierBatch> &runs = lane(p_reliable);
    CarrierBatch out;
    CarrierBatch *run = runs.getptr(p_peer);
    if (run != nullptr) {
        out.swap(*run);
    }
    return out;
}

void NetwCarrierBuffers::recycle(
    int64_t p_peer,
    bool p_reliable,
    CarrierBatch &r_spent
) {
    CarrierBatch *run = lane(p_reliable).getptr(p_peer);
    if (run == nullptr || !run->is_empty() || !run->rows().is_empty()) {
        return;
    }
    r_spent.release();
    run->swap(r_spent);
}

void NetwCarrierBuffers::close_route(int64_t p_route) {
    for (KeyValue<int64_t, CarrierBatch> &row : unreliable) {
        row.value.close_route(p_route);
    }
    for (KeyValue<int64_t, CarrierBatch> &row : reliable) {
        row.value.close_route(p_route);
    }
}

PackedInt32Array NetwCarrierBuffers::peers(bool p_reliable) const {
    PackedInt32Array out;
    for (const KeyValue<int64_t, CarrierBatch> &row : lane(p_reliable)) {
        if (!row.value.is_empty()) {
            out.push_back(int32_t(row.key));
        }
    }
    out.sort();
    return out;
}

int64_t NetwCarrierBuffers::pending(int64_t p_peer, bool p_reliable) const {
    const HashMap<int64_t, CarrierBatch>::ConstIterator found
        = lane(p_reliable).find(p_peer);
    return found != lane(p_reliable).end() ? found->value.size() : 0;
}

void NetwCarrierBuffers::clear() {
    unreliable.clear();
    reliable.clear();
}

} // namespace netw
