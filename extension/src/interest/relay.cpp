#include "netw/sealed.hpp"

#include "netw/interest/relay.hpp"

using namespace godot;

namespace netw::interest {

namespace {

bool is_kind(int64_t value) {
    return value == Awareness::ENTER || value == Awareness::EXIT;
}

} // namespace

Awareness Awareness::layer_edge(
    int64_t route,
    const StringName &layer_id,
    int kind
) {
    Awareness edge;
    if (admits(route, layer_id, int64_t(kind))) {
        edge.route = route;
        edge.layer_id = layer_id;
        edge.kind = int32_t(kind);
    }
    return edge;
}

bool Awareness::admits(
    int64_t route,
    const StringName &layer_id,
    int64_t kind
) {
    return route > 0 && !layer_id.is_empty() && is_kind(kind);
}

namespace {

constexpr int EDGE_COUNT_BYTES = 2;

bool edge_is_coherent(const AwarenessEdge &edge) {
    return edge.route > 0 && !edge.layer_id.is_empty();
}

AwarenessEdge edge_of(const Awareness &source) {
    AwarenessEdge out;
    out.route = uint64_t(source.route);
    out.layer_id = source.layer_id;
    out.entered = source.kind == Awareness::ENTER;
    return out;
}

Awareness awareness_of(const AwarenessEdge &edge) {
    Awareness out;
    out.route = int64_t(edge.route);
    out.layer_id = edge.layer_id;
    out.kind = edge.entered ? Awareness::ENTER : Awareness::EXIT;
    return out;
}

} // namespace

PackedByteArray awareness_encode(const LocalVector<Awareness> &edges) {
    wire::WriteStream stream;
    uint64_t count = uint64_t(edges.size());
    if (!stream.varuint(count, EDGE_COUNT_BYTES)) {
        return PackedByteArray();
    }
    for (uint32_t at = 0; at < edges.size(); ++at) {
        AwarenessEdge edge = edge_of(edges[at]);
        if (!AwarenessEdge::wire.run(stream, edge)) {
            return PackedByteArray();
        }
    }
    if (!stream.align_verify()) {
        return PackedByteArray();
    }
    return stream.to_bytes();
}

bool awareness_decode(
    const PackedByteArray &bytes,
    LocalVector<Awareness> &r_edges
) {
    wire::ReadStream stream(bytes);
    uint64_t count = 0;
    if (!stream.varuint(count, EDGE_COUNT_BYTES)) {
        return false;
    }
    LocalVector<Awareness> staged;
    staged.reserve(uint32_t(count));
    for (uint64_t at = 0; at < count; ++at) {
        AwarenessEdge edge;
        if (!AwarenessEdge::wire.run(stream, edge) || !edge_is_coherent(edge)) {
            return false;
        }
        staged.push_back(awareness_of(edge));
    }
    if (!stream.align_verify() || stream.bits_remaining() != 0) {
        return false;
    }
    r_edges = staged;
    return true;
}

void Relay::append(int64_t peer_id, const Awareness &edge) {
    if (edge.route <= 0) {
        return;
    }
    if (!pending.has(peer_id)) {
        pending.insert(peer_id, LocalVector<Awareness>());
        peer_order.push_back(peer_id);
    }
    pending[peer_id].push_back(edge);
}

PackedInt64Array Relay::targets() const {
    PackedInt64Array out;
    for (uint32_t index = 0; index < peer_order.size(); ++index) {
        out.push_back(peer_order[index]);
    }
    return out;
}

LocalVector<Awareness> Relay::wire_for(int64_t peer_id) const {
    LocalVector<Awareness> out;
    const HashMap<int64_t, LocalVector<Awareness>>::ConstIterator found
        = pending.find(peer_id);
    if (!found) {
        return out;
    }
    out = found->value;
    return out;
}

void Relay::forget(int64_t peer_id) {
    if (!pending.has(peer_id)) {
        return;
    }
    pending.erase(peer_id);
    for (uint32_t index = 0; index < peer_order.size(); ++index) {
        if (peer_order[index] == peer_id) {
            peer_order.remove_at(index);
            return;
        }
    }
}

bool Relay::is_empty() const {
    return pending.is_empty();
}

void Relay::clear() {
    pending.clear();
    peer_order.clear();
}

} // namespace netw::interest
