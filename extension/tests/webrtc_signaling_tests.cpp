#include "support/netw_cells.h"
#include "support/netw_test.h"

#include <cstdint>

#include "godot/variant.hpp"
#include "netw/connect/room_board.hpp"
#include "netw/connect/signaler.hpp"
#include "netw/connect/webrtc_transport.hpp"

namespace TestNetwWebRTCSignaling {

using namespace godot;
using netw::connect::board_hash_of;
using netw::connect::RoomCard;
using netw::connect::TargetRow;
using netw::connect::TrackerSignaler;
using netw::connect::WebRTCTransport;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

PackedStringArray unreachable_tracker() {
    PackedStringArray urls;
    urls.push_back("ws://127.0.0.1:9999");
    return urls;
}

String salted(const String &p_namespace, const String &p_room) {
    return (p_namespace + String(":") + p_room).sha1_text().substr(0, 20);
}

RoomCard card_of(const char *p_hash, const char *p_uid) {
    RoomCard card;
    card.room_hash = String(p_hash);
    card.room_name = "Someone's game";
    card.filter_uid = String(p_uid);
    card.app_id = StringName("build-7");
    card.signaling_namespace = "my_game";
    card.players = 3;
    card.max_players = 8;
    return card;
}

Dictionary server_of(const char *p_first, const char *p_second = nullptr) {
    Array urls;
    urls.push_back(String(p_first));
    if (p_second != nullptr) {
        urls.push_back(String(p_second));
    }
    Dictionary one;
    one["urls"] = urls;
    return one;
}

LawVerdict coded_room(const Stand &) {
    TrackerSignaler host(unreachable_tracker(), "my_game", "ABC");
    host.open(String(), 1);
    const String room = host.room_id();
    const String hash = host.info_hash();
    host.close();

    if (room.length() != 5) {
        return law_broken("the room code has %d glyphs", int(room.length()));
    }
    for (int64_t at = 0; at < room.length(); at++) {
        const String glyph = room.substr(at, 1);
        if (glyph != "A" && glyph != "B" && glyph != "C") {
            return law_broken("the room code left the game's alphabet");
        }
    }
    if (hash != salted("my_game", room)) {
        return law_broken("the room hash is not the salted code");
    }
    return law_held();
}

LawVerdict bare_room(const Stand &) {
    TrackerSignaler host(unreachable_tracker(), String(), String());
    host.open(String(), 1);
    const String room = host.room_id();
    const String hash = host.info_hash();
    host.close();

    if (room.length() != 20 || hash != room) {
        return law_broken("a bare room is not its own twenty glyph hash");
    }
    const String typed = "not-twenty";
    if (TrackerSignaler::derived_info_hash(String(), typed)
        != typed.sha1_text().substr(0, 20)) {
        return law_broken("a short bare room is not hashed");
    }
    return law_held();
}

LawVerdict joiner_reaches_host(const Stand &) {
    TrackerSignaler host(unreachable_tracker(), "my_game", "ABC");
    host.open(String(), 1);
    TrackerSignaler joiner(unreachable_tracker(), "my_game", "XYZ");
    joiner.open(host.room_id(), 7);
    const String host_hash = host.info_hash();
    const String joiner_hash = joiner.info_hash();
    const String host_address = host.local_address();
    const String joiner_address = joiner.local_address();
    host.close();
    joiner.close();

    if (joiner_hash != host_hash) {
        return law_broken("the joiner announced on another swarm");
    }
    if (TrackerSignaler::host_peer_id(joiner_hash) != host_address) {
        return law_broken("the joiner dials an address the host never took");
    }
    if (TrackerSignaler::peer_of_address(host_address) != 1
        || TrackerSignaler::peer_of_address(joiner_address) != 7) {
        return law_broken("an address lost its engine peer id");
    }
    if (TrackerSignaler::peer_of_address("too-short") != 0) {
        return law_broken("a malformed address named a peer");
    }
    return law_held();
}

LawVerdict one_board(const Stand &) {
    if (board_hash_of("netwkd") != board_hash_of("netwkd")) {
        return law_broken("one filter uid gave two boards");
    }
    if (board_hash_of("game-a") == board_hash_of("game-b")) {
        return law_broken("two games share one board");
    }
    if (board_hash_of("netwkd").length() != 20) {
        return law_broken("the board hash is not twenty glyphs");
    }
    return law_held();
}

LawVerdict card_round_trip(const Stand &) {
    const RoomCard sent = card_of("room00000000000000ab", "netwkd");
    RoomCard read;
    if (!RoomCard::from_dictionary(sent.to_dictionary(), read)) {
        return law_broken("a room card was refused");
    }
    if (read.room_hash != sent.room_hash || read.room_name != sent.room_name
        || read.filter_uid != sent.filter_uid || read.app_id != sent.app_id
        || read.signaling_namespace != sent.signaling_namespace
        || read.players != 3 || read.max_players != 8) {
        return law_broken("a room card lost a value on the way");
    }

    const TargetRow row = read.to_row();
    if (row.address != sent.room_hash || !row.advertised.is_valid()) {
        return law_broken("the browse row is not addressed by the room hash");
    }
    if (row.advertised->get_app_id() != StringName("build-7")
        || row.advertised->get_players() != 3) {
        return law_broken("the browse row lost what the host advertised");
    }
    if (String(row.metadata["signaling_namespace"]) != String("my_game")) {
        return law_broken("the browse row lost the namespace");
    }
    return law_held();
}

LawVerdict not_a_card(const Stand &) {
    RoomCard read;
    Dictionary query;
    query["t"] = "query";
    if (RoomCard::from_dictionary(query, read)) {
        return law_broken("a browser's query was read as a room");
    }
    Dictionary hashless;
    hashless["t"] = "room";
    hashless["name"] = "nowhere";
    if (RoomCard::from_dictionary(hashless, read)) {
        return law_broken("a room with no hash was read");
    }
    return law_held();
}

LawVerdict turn_filter(const Stand &) {
    if (WebRTCTransport::is_url_supported_native("turns:relay.example:443")
        || WebRTCTransport::is_url_supported_native(
            "turn:relay.example:443?transport=tcp"
        )
        || WebRTCTransport::is_url_supported_native(
            "TURN:relay.example:443?transport=TLS"
        )) {
        return law_broken("a TLS or TCP relay was kept");
    }
    if (!WebRTCTransport::is_url_supported_native("turn:relay.example:3478")
        || !WebRTCTransport::is_url_supported_native("stun:stun.example:1")) {
        return law_broken("a UDP relay or STUN server was dropped");
    }

    Dictionary mixed
        = server_of("turns:relay.example:443", "turn:relay.example:3478");
    mixed["username"] = "who";
    mixed["credential"] = "secret";
    Array authored;
    authored.push_back(mixed);
    authored.push_back(server_of("turns:only.example:443"));
    authored.push_back(server_of("stun:stun.example:19302"));

    const Array kept = WebRTCTransport::filter_ice_servers(authored);
    if (kept.size() != 2) {
        return law_broken("%d servers were kept, not 2", int(kept.size()));
    }
    const Dictionary trimmed = kept[0];
    const Array surviving = trimmed["urls"];
    if (surviving.size() != 1
        || String(surviving[0]) != String("turn:relay.example:3478")) {
        return law_broken("the dialable url of a mixed server was lost");
    }
    if (String(trimmed["username"]) != String("who")
        || String(trimmed["credential"]) != String("secret")) {
        return law_broken("a kept relay lost its credentials");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a host opening a room under a namespace",
     "the room is a five glyph code and its hash is the code salted by the "
     "namespace",
     &coded_room},
    {"a host opening a room with no namespace, or a short room typed",
     "a twenty glyph room is its own hash and any other is hashed",
     &bare_room},
    {"a joiner typing the host's room code",
     "both announce on one swarm and the joiner dials the host's address",
     &joiner_reaches_host},
    {"every build of one game, and two games, browsing for rooms",
     "one filter uid meets on one board and two never share one",
     &one_board},
    {"a room card read back from its announce",
     "every value survives and the browse row carries what the host "
     "advertised",
     &card_round_trip},
    {"an announce that is not a room card",
     "a query or a card with no room hash is refused",
     &not_a_card},
    {"a TURN url the native stack cannot dial",
     "TLS and TCP relays are dropped, a server left empty goes, and a kept "
     "relay keeps its credentials",
     &turn_filter},
};

TEST_CASE("[Netw][Connect] two builds find one WebRTC room") {
    const Stand stand = {String("a room over a tracker")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwWebRTCSignaling
