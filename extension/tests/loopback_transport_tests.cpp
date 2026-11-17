#include "support/link.h"

#include "support/netw_cells.h"
#include "support/netw_test.h"

#include "netw/api/loopback.hpp"

namespace TestNetwLoopbackTransport {

using namespace godot;
using netw::LocalLoopbackSession;
using netw::LocalMultiplayerPeer;
using netw::NetwLinkConditions;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

constexpr double PERIOD_MS = 1000.0 / 60.0;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

struct Pair {
    Ref<LocalLoopbackSession> session;
    Ref<LocalMultiplayerPeer> server;
    Ref<LocalMultiplayerPeer> client;
};

Pair connected_pair() {
    Pair pair;
    pair.session.instantiate();
    pair.server = pair.session->get_server_peer();
    pair.client = pair.session->create_client_peer();
    pair.session->poll();
    pair.client->set_target_peer(1);
    return pair;
}

PackedByteArray one_byte(int p_value) {
    PackedByteArray bytes;
    bytes.push_back(uint8_t(p_value));
    return bytes;
}

void send(const Ref<LocalMultiplayerPeer> &p_peer, int p_value) {
    p_peer->put_packet(one_byte(p_value));
}

void poll(const Pair &p_pair, int p_count) {
    for (int at = 0; at < p_count; ++at) {
        p_pair.session->poll();
    }
}

int waiting(const Pair &p_pair) {
    return p_pair.server->get_available_packet_count();
}

String drain(const Ref<LocalMultiplayerPeer> &p_peer) {
    String text;
    while (p_peer->get_available_packet_count() > 0) {
        if (!text.is_empty()) {
            text += String(",");
        }
        text += String::num_int64(p_peer->get_packet()[0]);
    }
    return text;
}

LawVerdict sequence_is(
    const char *p_what,
    const String &p_read,
    const char *p_expected
) {
    if (p_read == String(p_expected)) {
        return law_held();
    }
    return law_broken(
        "%s read %s, not %s",
        p_what,
        p_read.utf8().get_data(),
        p_expected
    );
}

Ref<NetwLinkConditions> three_periods() {
    return netw_test::polls(3);
}

LawVerdict delays(const Stand &) {
    Pair pair = connected_pair();
    send(pair.client, 1);
    pair.session->set_link_conditions(pair.server.ptr(), three_periods());
    send(pair.client, 2);

    if (waiting(pair) != 0) {
        return law_broken("a packet queued before the link was set arrived");
    }
    poll(pair, 2);
    if (waiting(pair) != 0) {
        return law_broken("a packet arrived two polls into three periods");
    }
    poll(pair, 1);
    return sequence_is("three periods later", drain(pair.server), "1,2");
}

LawVerdict loses(const Stand &) {
    Pair reliable = connected_pair();
    Ref<NetwLinkConditions> always_lost = netw_test::link(10);
    always_lost->set_loss_chance(1.0);
    always_lost->set_latency_ms(10.0);
    reliable.session->set_link_conditions(reliable.server.ptr(), always_lost);
    send(reliable.client, 4);
    poll(reliable, 3);
    const LawVerdict resent
        = sequence_is("a lost reliable packet", drain(reliable.server), "4");
    if (!resent.held) {
        return resent;
    }

    Pair unreliable = connected_pair();
    Ref<NetwLinkConditions> dropped = netw_test::link(10);
    dropped->set_loss_chance(1.0);
    unreliable.session->set_link_conditions(unreliable.server.ptr(), dropped);
    unreliable.client->set_transfer_mode(
        MultiplayerPeer::TRANSFER_MODE_UNRELIABLE
    );
    send(unreliable.client, 5);
    poll(unreliable, 4);
    return sequence_is(
        "a lost unreliable packet",
        drain(unreliable.server),
        ""
    );
}

LawVerdict keeps_reliable_order(const Stand &) {
    Pair pair = connected_pair();
    Ref<NetwLinkConditions> shuffling = netw_test::link(123);
    shuffling->set_jitter_ms(6.0 * PERIOD_MS);
    shuffling->set_reorder_chance(1.0);
    pair.session->set_link_conditions(pair.server.ptr(), shuffling);
    for (int value = 0; value < 12; ++value) {
        send(pair.client, value);
        poll(pair, 1);
    }
    poll(pair, 12);
    return sequence_is(
        "reliable packets under jitter",
        drain(pair.server),
        "0,1,2,3,4,5,6,7,8,9,10,11"
    );
}

String late_packet(MultiplayerPeer::TransferMode p_mode) {
    Pair pair = connected_pair();
    pair.session->set_link_conditions(pair.server.ptr(), netw_test::polls(4));
    pair.client->set_transfer_mode(p_mode);
    pair.client->set_transfer_channel(2);
    send(pair.client, 0);
    pair.client->set_transfer_channel(3);
    send(pair.client, 2);
    const Ref<LocalMultiplayerPeer> other = pair.session->create_client_peer();
    other->set_target_peer(1);
    other->set_transfer_mode(p_mode);
    other->set_transfer_channel(2);
    send(other, 3);
    pair.session->set_link_conditions(pair.server.ptr(), netw_test::link());
    pair.client->set_transfer_channel(2);
    send(pair.client, 1);
    poll(pair, 5);
    return drain(pair.server);
}

LawVerdict orders_unreliable(const Stand &) {
    const LawVerdict plain = sequence_is(
        "unreliable",
        late_packet(MultiplayerPeer::TRANSFER_MODE_UNRELIABLE),
        "1,0,2,3"
    );
    if (!plain.held) {
        return plain;
    }
    return sequence_is(
        "ordered unreliable",
        late_packet(MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED),
        "1,2,3"
    );
}

LawVerdict duplicates(const Stand &) {
    Pair pair = connected_pair();
    Ref<NetwLinkConditions> doubling = netw_test::link(10);
    doubling->set_duplicate_chance(1.0);
    pair.session->set_link_conditions(pair.server.ptr(), doubling);
    pair.client->set_transfer_mode(MultiplayerPeer::TRANSFER_MODE_UNRELIABLE);
    send(pair.client, 7);
    poll(pair, 2);
    return sequence_is("a duplicated packet", drain(pair.server), "7,7");
}

LawVerdict stalls(const Stand &) {
    for (const auto mode :
         {MultiplayerPeer::TRANSFER_MODE_RELIABLE,
          MultiplayerPeer::TRANSFER_MODE_UNRELIABLE}) {
        for (const bool queued : {false, true}) {
            Pair pair = connected_pair();
            Ref<NetwLinkConditions> conditions = netw_test::link(17);
            pair.client->set_transfer_mode(mode);
            if (queued) {
                conditions->set_latency_ms(3.0 * PERIOD_MS);
                pair.session->set_link_conditions(
                    pair.server.ptr(),
                    conditions
                );
                send(pair.client, 0);
                poll(pair, 1);
                conditions->set_latency_ms(0.0);
            }
            conditions->set_stall_chance(1.0);
            conditions->set_stall_ms(4.0 * PERIOD_MS);
            pair.session->set_link_conditions(pair.server.ptr(), conditions);
            send(pair.client, 1);
            for (int at = 0; at < 3; ++at) {
                poll(pair, 1);
                if (waiting(pair) != 0) {
                    return law_broken(
                        "a packet arrived %d polls into a four period stall",
                        at + 1
                    );
                }
            }
            poll(pair, 1);
            const LawVerdict released = sequence_is(
                "the stall's end",
                drain(pair.server),
                queued ? "0,1" : "1"
            );
            if (!released.held) {
                return released;
            }
        }
    }
    return law_held();
}

LawVerdict shapes_one_sender(const Stand &) {
    Pair pair = connected_pair();
    const Ref<LocalMultiplayerPeer> other = pair.session->create_client_peer();
    other->set_target_peer(1);
    pair.session->poll();
    pair.session->set_link_conditions(
        pair.server.ptr(),
        three_periods(),
        pair.client->get_unique_id()
    );
    send(pair.client, 1);
    send(other, 2);

    poll(pair, 1);
    const LawVerdict untouched
        = sequence_is("the other sender", drain(pair.server), "2");
    if (!untouched.held) {
        return untouched;
    }
    poll(pair, 2);
    return sequence_is("the delayed sender", drain(pair.server), "1");
}

const Row ROWS[] = {
    {"a link with latency, set while a packet waits",
     "both packets arrive after as many polls as the latency is periods long",
     &delays},
    {"a link that loses every packet",
     "a reliable packet arrives after it is resent, and an unreliable one "
     "never does",
     &loses},
    {"reliable packets on a link that reorders",
     "they arrive in the order they were sent",
     &keeps_reliable_order},
    {"a late packet on an unreliable or an ordered unreliable channel",
     "unreliable delivers it late, and ordered unreliable drops it only for "
     "the sender and channel that already delivered newer",
     &orders_unreliable},
    {"a link that duplicates every packet",
     "each packet arrives twice",
     &duplicates},
    {"a link that stalls, with and without a packet already on it",
     "reliable and unreliable packets wait out the same stall, then arrive",
     &stalls},
    {"a latency set for one sender",
     "another sender's packets are not delayed",
     &shapes_one_sender},
};

TEST_CASE(
    "[Netw][Transport] a local link drops, delays and reorders what its "
    "conditions say"
) {
    const Stand stand = {String("a LocalLoopbackSession")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwLoopbackTransport
