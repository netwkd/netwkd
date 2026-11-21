#include "netw/api/server_info.hpp"

#include <cstring>

#include "godot/class_db.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/call_args.hpp"
#include "netw/fault_book.hpp"
#include "netw/value_variant.hpp"

using namespace godot;

namespace netw {

namespace {

constexpr uint8_t INFO_VERSION = 1;

void put_text(
    value::Bytes &r_out,
    const String &p_text,
    const char *p_name,
    NetwMultiplayer *p_session
) {
    const CharString utf8 = p_text.utf8();
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(utf8.get_data());
    if (!value::is_exact_text(bytes, utf8.length())) {
        fault::raise(
            p_session,
            NETW_FAULT_UNSENDABLE_VALUE,
            {String(p_name),
             String(
                 "text with a NUL, a byte order mark at its start or a "
                 "character UTF-8 cannot hold"
             ),
             String("text without them")}
        );
        value::write_varuint(r_out, 0);
        return;
    }
    value::write_varuint(r_out, uint64_t(utf8.length()));
    value::write_span(r_out, bytes, utf8.length());
}

bool take_text(value::Reader &r_reader, String &r_text) {
    const uint64_t size = r_reader.varuint();
    const uint8_t *utf8 = r_reader.span(int64_t(size));
    if (!r_reader.ok || !value::is_exact_text(utf8, int64_t(size))) {
        return false;
    }
    r_text = String::utf8(reinterpret_cast<const char *>(utf8), int(size));
    return true;
}

} // namespace

PackedByteArray NetwServerInfo::to_payload(const Ref<NetwServerInfo> &p_info) {
    return payload_for(p_info, nullptr);
}

PackedByteArray NetwServerInfo::payload_for(
    const Ref<NetwServerInfo> &p_info,
    NetwMultiplayer *p_session
) {
    if (p_info.is_null()) {
        return PackedByteArray();
    }
    value::Bytes out;
    out.push_back(INFO_VERSION);
    put_text(out, p_info->get_motd(), "The server info's motd", p_session);
    value::write_svarint(out, p_info->get_players());
    value::write_svarint(out, p_info->get_max_players());
    put_text(
        out,
        p_info->get_game_mode(),
        "The server info's game_mode",
        p_session
    );
    put_text(
        out,
        p_info->get_version(),
        "The server info's version",
        p_session
    );
    put_text(out, p_info->get_app_id(), "The server info's app_id", p_session);
    out.push_back(p_info->get_is_local_listener() ? 1 : 0);
    value::write_svarint(out, int64_t(p_info->get_visibility()));
    const PackedByteArray metadata = call_args::value_bytes(
        p_info->get_metadata(),
        String("The server info's metadata"),
        p_session
    );
    value::write_span(out, metadata.ptr(), metadata.size());
    PackedByteArray bytes;
    bytes.resize(int64_t(out.size()));
    std::memcpy(bytes.ptrw(), out.ptr(), out.size());
    return bytes;
}

Ref<NetwServerInfo> NetwServerInfo::from_payload(
    const PackedByteArray &p_bytes
) {
    value::Reader reader(p_bytes.ptr(), p_bytes.size());
    const uint8_t *version = reader.span(1);
    if (!reader.ok || *version != INFO_VERSION) {
        return Ref<NetwServerInfo>();
    }
    String motd;
    String game_mode;
    String game_version;
    String app_id;
    const bool texts = take_text(reader, motd);
    const int64_t players = reader.svarint();
    const int64_t max_players = reader.svarint();
    const bool named = texts && take_text(reader, game_mode)
        && take_text(reader, game_version) && take_text(reader, app_id);
    const uint8_t *local = reader.span(1);
    const int64_t visibility = reader.svarint();
    Variant metadata;
    if (!named || !reader.ok || *local > 1
        || value::decode(reader.at, reader.left(), metadata) != reader.left()) {
        return Ref<NetwServerInfo>();
    }
    Ref<NetwServerInfo> info;
    info.instantiate();
    info->set_motd(motd);
    info->set_visibility(NetwServerInfo::Visibility(visibility));
    info->set_players(players);
    info->set_max_players(max_players);
    info->set_game_mode(StringName(game_mode));
    info->set_version(game_version);
    info->set_app_id(StringName(app_id));
    info->set_is_local_listener(*local == 1);
    info->set_metadata(
        metadata.get_type() == Variant::DICTIONARY ? Dictionary(metadata)
                                                   : Dictionary()
    );
    return info;
}

void NetwServerInfo::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_motd"), &NetwServerInfo::get_motd);
    ClassDB::bind_method(
        D_METHOD("set_motd", "motd"),
        &NetwServerInfo::set_motd
    );
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "motd"), "set_motd", "get_motd");

    ClassDB::bind_method(D_METHOD("get_players"), &NetwServerInfo::get_players);
    ClassDB::bind_method(
        D_METHOD("set_players", "players"),
        &NetwServerInfo::set_players
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "players"),
        "set_players",
        "get_players"
    );

    ClassDB::bind_method(
        D_METHOD("get_max_players"),
        &NetwServerInfo::get_max_players
    );
    ClassDB::bind_method(
        D_METHOD("set_max_players", "max_players"),
        &NetwServerInfo::set_max_players
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "max_players"),
        "set_max_players",
        "get_max_players"
    );

    ClassDB::bind_method(
        D_METHOD("get_game_mode"),
        &NetwServerInfo::get_game_mode
    );
    ClassDB::bind_method(
        D_METHOD("set_game_mode", "game_mode"),
        &NetwServerInfo::set_game_mode
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING_NAME, "game_mode"),
        "set_game_mode",
        "get_game_mode"
    );

    ClassDB::bind_method(D_METHOD("get_version"), &NetwServerInfo::get_version);
    ClassDB::bind_method(
        D_METHOD("set_version", "version"),
        &NetwServerInfo::set_version
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING, "version"),
        "set_version",
        "get_version"
    );

    ClassDB::bind_method(D_METHOD("get_app_id"), &NetwServerInfo::get_app_id);
    ClassDB::bind_method(
        D_METHOD("set_app_id", "app_id"),
        &NetwServerInfo::set_app_id
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::STRING_NAME, "app_id"),
        "set_app_id",
        "get_app_id"
    );

    ClassDB::bind_method(
        D_METHOD("get_is_local_listener"),
        &NetwServerInfo::get_is_local_listener
    );
    ClassDB::bind_method(
        D_METHOD("set_is_local_listener", "is_local_listener"),
        &NetwServerInfo::set_is_local_listener
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "is_local_listener"),
        "set_is_local_listener",
        "get_is_local_listener"
    );

    ClassDB::bind_method(
        D_METHOD("get_metadata"),
        &NetwServerInfo::get_metadata
    );
    ClassDB::bind_method(
        D_METHOD("set_metadata", "metadata"),
        &NetwServerInfo::set_metadata
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::DICTIONARY, "metadata"),
        "set_metadata",
        "get_metadata"
    );

    ClassDB::bind_method(
        D_METHOD("get_latency_ms"),
        &NetwServerInfo::get_latency_ms
    );
    ClassDB::bind_method(
        D_METHOD("set_latency_ms", "latency_ms"),
        &NetwServerInfo::set_latency_ms
    );
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "latency_ms"),
        "set_latency_ms",
        "get_latency_ms"
    );

    ClassDB::bind_method(
        D_METHOD("get_visibility"),
        &NetwServerInfo::get_visibility
    );
    ClassDB::bind_method(
        D_METHOD("set_visibility", "visibility"),
        &NetwServerInfo::set_visibility
    );
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT,
            "visibility",
            PROPERTY_HINT_ENUM,
            "Public,Friends Only,Private",
            PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
            "NetwServerInfo.Visibility"
        ),
        "set_visibility",
        "get_visibility"
    );

    BIND_ENUM_CONSTANT(VISIBILITY_PUBLIC);
    BIND_ENUM_CONSTANT(VISIBILITY_FRIENDS_ONLY);
    BIND_ENUM_CONSTANT(VISIBILITY_PRIVATE);

    ClassDB::bind_static_method(
        "NetwServerInfo",
        D_METHOD("to_payload", "info"),
        &NetwServerInfo::to_payload
    );
    ClassDB::bind_static_method(
        "NetwServerInfo",
        D_METHOD("from_payload", "bytes"),
        &NetwServerInfo::from_payload
    );
}

void NetwServerInfo::copy_values_from(const NetwServerInfo &p_source) {
    motd = p_source.motd;
    players = p_source.players;
    max_players = p_source.max_players;
    game_mode = p_source.game_mode;
    version = p_source.version;
    app_id = p_source.app_id;
    is_local_listener = p_source.is_local_listener;
    latency_ms = p_source.latency_ms;
    visibility = p_source.visibility;
    metadata = p_source.metadata.duplicate(true);
}

} // namespace netw
