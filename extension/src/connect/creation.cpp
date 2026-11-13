#include "netw/connect/creation.hpp"

#include "godot/local_vector.hpp"

using namespace godot;

namespace netw::connect {

namespace {

LocalVector<PeerOffer> &offers() {
    static LocalVector<PeerOffer> instance;
    return instance;
}

} // namespace

void offer_open(const PeerOffer &p_offer) {
    offers().push_back(p_offer);
}

PeerOffer *offer_of_peer(const Ref<MultiplayerPeer> &p_peer) {
    if (p_peer.is_null()) {
        return nullptr;
    }
    LocalVector<PeerOffer> &held = offers();
    for (uint32_t at = 0; at < held.size(); at++) {
        if (held[at].peer == p_peer) {
            return &held[at];
        }
    }
    return nullptr;
}

PeerOffer *offer_of_ticket(const RID &p_ticket) {
    if (!p_ticket.is_valid()) {
        return nullptr;
    }
    LocalVector<PeerOffer> &held = offers();
    for (uint32_t at = 0; at < held.size(); at++) {
        if (held[at].ticket == p_ticket) {
            return &held[at];
        }
    }
    return nullptr;
}

void offer_close(const RID &p_ticket) {
    LocalVector<PeerOffer> &held = offers();
    for (uint32_t at = held.size(); at > 0; at--) {
        if (held[at - 1].ticket == p_ticket) {
            held.remove_at(at - 1);
        }
    }
}

} // namespace netw::connect
