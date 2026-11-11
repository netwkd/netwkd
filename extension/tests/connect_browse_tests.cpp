#include "support/directory_stubs.h"
#include "support/minted_script.h"
#include "support/netw_cells.h"

#if defined(NETW_TIER_HOSTED)

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/connection.hpp"
#include "netw/api/context.hpp"
#include "netw/api/endpoint.hpp"
#include "netw/api/loopback.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/multiplayer_viewport.hpp"
#include "netw/api/server_info.hpp"
#include "netw/api/session.hpp"
#include "netw/api/session_config.hpp"
#include "netw/connect/transports.hpp"

namespace TestConnectBrowse {

using namespace godot;
using netw::MultiplayerViewport;
using netw::Netw;
using netw::NetwConnection;
using netw::NetwEndpoint;
using netw::NetwMultiplayer;
using netw::NetwServerInfo;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

constexpr const char *PEER_CLASS = "BrowseTestPeer";

constexpr const char *PROBED_TRANSPORT = R"(extends NetwTransport

static var tickets: Array[RID] = []
static var standing := 0

func _peer_class() -> StringName:
	return &"BrowseTestPeer"

func _display_name() -> String:
	return "Browse"

func _can_probe() -> bool:
	return true

func _make_peer(
	ticket: RID,
	_mode: NetwConnection.TransportMode,
	_address: String,
	_settings: Dictionary,
) -> void:
	fail(ticket, ERR_UNCONFIGURED, "browse")

func _probe(ticket: RID, _address: String) -> void:
	standing = get_instance_id()
	tickets.append(ticket)

static func answer(count: int, app_id: StringName) -> void:
	var transport := instance_from_id(standing) as NetwTransport
	for at in mini(count, tickets.size()):
		var info := NetwServerInfo.new()
		info.app_id = app_id
		transport.deliver_probe(tickets.pop_front(), info)
)";

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

class AppDirectory : public netw_test::StubDirectory {
public:
    PackedStringArray apps;

    void list_lobbies() override {
        browse_calls += 1;
        TypedArray<NetwServerInfo> infos;
        for (int at = 0; at < rows.size(); at++) {
            Ref<NetwServerInfo> info;
            info.instantiate();
            info->set_app_id(StringName(at < apps.size() ? apps[at] : ""));
            infos.push_back(info);
        }
        publish_lobbies(rows, rows, infos);
    }
};

struct Browser {
    MultiplayerViewport *tree = memnew(MultiplayerViewport);
    Node *probe = memnew(Node);
    Ref<Script> transport = netw_test::minted_script(PROBED_TRANSPORT);
    AppDirectory *directory = memnew(AppDirectory);
    Ref<NetwConnection> connection;
    NetwMultiplayer *api = nullptr;

    explicit Browser(const char *p_app_id = "") {
        tree->set_name(StringName("BrowseTree"));
        probe->set_name(StringName("Probe"));
        tree->add_child(probe);
        netw::gd::scene_root()->add_child(tree);
        api = Object::cast_to<NetwMultiplayer>(tree->get_api().ptr());
        REQUIRE(api != nullptr);
        if (String(p_app_id) != String()) {
            Netw::configure_session(probe)->app(StringName(p_app_id));
            Ref<netw::LocalMultiplayerPeer> hosting;
            hosting.instantiate();
            REQUIRE(hosting->create_server() == OK);
            api->set_multiplayer_peer(hosting);
        }
        api->poll();
        connection = Netw::session(probe)->get_connection();
        REQUIRE(transport.is_valid());
        REQUIRE(connection->register_transport(transport));
        api->service_register(directory, nullptr);
    }

    ~Browser() {
        connection->set_browsing(false);
        api->set_multiplayer_peer(Ref<MultiplayerPeer>());
        api->service_unregister(directory, nullptr);
        memdelete(directory);
        tree->get_parent()->remove_child(tree);
        memdelete(tree);
    }

    Ref<NetwEndpoint> add(const String &p_address) {
        return connection->add(StringName(PEER_CLASS), p_address, String());
    }

    void poll(int p_times = 1) {
        for (int at = 0; at < p_times; at++) {
            api->poll();
        }
    }

    int probing() const {
        const Array tickets = transport->get(StringName("tickets"));
        return int(tickets.size());
    }

    void answer(int p_count, const char *p_app_id = "") {
        transport->call(StringName("answer"), p_count, StringName(p_app_id));
    }

    String order() {
        String out;
        const TypedArray<NetwEndpoint> shown = connection->endpoints();
        for (int at = 0; at < shown.size(); at++) {
            const Ref<NetwEndpoint> row = shown[at];
            out += (at > 0 ? String(" ") : String()) + row->get_address();
        }
        return out;
    }
};

LawVerdict caller_rows_kept(const Stand &) {
    Browser browser;
    browser.add(String("kept-1"));
    browser.add(String("kept-2"));
    browser.directory->rows.push_back(String("lobby-a"));
    browser.directory->rows.push_back(String("lobby-b"));

    browser.connection->set_browsing(true);
    browser.poll();
    const String listed = browser.order();
    browser.api->service_unregister(browser.directory, nullptr);
    browser.poll();
    const String left = browser.order();

    if (listed != String("kept-1 kept-2 lobby-a lobby-b")) {
        return law_broken("the list read %s", listed.utf8().get_data());
    }
    browser.api->service_register(browser.directory, nullptr);
    if (left != String("kept-1 kept-2")) {
        return law_broken("then the list read %s", left.utf8().get_data());
    }
    return law_held();
}

LawVerdict foreign_app(const Stand &) {
    Browser browser("arena-v3");
    const Ref<NetwEndpoint> added = browser.add(String("probed"));
    browser.directory->rows.push_back(String("old-build"));
    browser.directory->rows.push_back(String("same-build"));
    browser.directory->apps.push_back(String("arena-v2"));
    browser.directory->apps.push_back(String("arena-v3"));

    browser.connection->set_browsing(true);
    browser.poll();
    browser.answer(1, "arena-v2");
    browser.poll();

    const StringName lobby("StubLobbyPeer");
    const Ref<NetwEndpoint> old_build
        = browser.connection->find(lobby, String("old-build"));
    const Ref<NetwEndpoint> same_build
        = browser.connection->find(lobby, String("same-build"));
    if (old_build.is_null() || same_build.is_null()) {
        return law_broken("the directory rows were not listed");
    }
    if (old_build->get_status() != NetwEndpoint::STATUS_INCOMPATIBLE) {
        return law_broken("a listed lobby of another build was joinable");
    }
    if (same_build->get_status() != NetwEndpoint::STATUS_ONLINE) {
        return law_broken("a listed lobby of this build was refused");
    }
    if (added->get_status() != NetwEndpoint::STATUS_INCOMPATIBLE) {
        return law_broken("a probed server of another build was joinable");
    }
    return law_held();
}

LawVerdict probe_cap(const Stand &) {
    Browser browser;
    Ref<NetwEndpoint> first;
    for (int at = 0; at < 10; at++) {
        const Ref<NetwEndpoint> row = browser.add(vformat("host-%d", at));
        first = at == 0 ? row : first;
    }

    first->probe();
    browser.connection->set_browsing(true);
    int most = browser.probing();
    browser.answer(1);
    for (int at = 0; at < 4; at++) {
        browser.poll();
        most = MAX(most, browser.probing());
    }

    if (most != 6) {
        return law_broken("%d probes ran at once", most);
    }
    return law_held();
}

LawVerdict listed_after_browsing(const Stand &) {
    Browser browser;
    const Ref<NetwEndpoint> added = browser.add(String("probed"));
    browser.directory->rows.push_back(String("lobby-a"));
    browser.connection->set_browsing(true);
    browser.poll();
    browser.answer(1);
    browser.poll();
    const Ref<NetwServerInfo> answered = added->get_info();

    browser.connection->set_browsing(false);
    browser.poll(3);

    if (browser.order() != String("probed lobby-a")) {
        return law_broken("rows left the list when browsing stopped");
    }
    if (added->get_status() != NetwEndpoint::STATUS_ONLINE
        || added->get_info() != answered) {
        return law_broken("a row lost its last answer when browsing stopped");
    }
    return law_held();
}

LawVerdict last_colon(const Stand &) {
    struct Split {
        const char *address;
        const char *host;
        int64_t port;
    };
    const Split rows[] = {
        {"203.0.113.42:9000", "203.0.113.42", 9000},
        {"fe80::1:9000", "fe80::1", 9000},
        {"example.org", "example.org", 21253},
        {"", "localhost", 21253},
    };
    for (const Split &row : rows) {
        String host;
        int64_t port = 0;
        netw::connect::split_host_port(String(row.address), 21253, host, port);
        if (host != String(row.host) || port != row.port) {
            return law_broken(
                "'%s' split to %s and %d",
                row.address,
                host.utf8().get_data(),
                int(port)
            );
        }
    }
    return law_held();
}

LawVerdict websocket_url(const Stand &) {
    const char *rows[][2] = {
        {"", "ws://localhost:21253"},
        {"localhost", "ws://localhost:21253"},
        {"127.0.0.1", "ws://localhost:21253"},
        {"ws://example.org:9000", "ws://example.org:9000"},
        {"wss://example.org", "wss://example.org"},
        {"example.org", "wss://example.org"},
    };
    for (const auto &row : rows) {
        const String url
            = netw::connect::WebSocketTransport::build_url(String(row[0]));
        if (url != String(row[1])) {
            return law_broken("'%s' became %s", row[0], url.utf8().get_data());
        }
    }
    return law_held();
}

const Row ROWS[] = {
    {"two rows the player added, then a directory lists two and leaves",
     "the player's rows list first and stay when the directory leaves",
     &caller_rows_kept},
    {"a host of arena-v3 browsing lobbies and a probed server of arena-v2",
     "rows of another build read incompatible and this build's read online",
     &foreign_app},
    {"ten rows, one probed by hand, then browsing starts",
     "no more than six probes run at once",
     &probe_cap},
    {"browsing stops after a probe and a listing answered",
     "both rows stay listed and the probed row keeps its answer",
     &listed_after_browsing},
    {"an ENet address with and without a port",
     "it splits on its last colon, and a missing port or host defaults",
     &last_colon},
    {"a WebSocket address typed bare, local or with a scheme",
     "a remote bare host becomes wss, a local one ws, a scheme is kept",
     &websocket_url},
};

TEST_CASE(
    "[Networked][Connect][SceneTree] the browser keeps the player's rows, "
    "marks another build incompatible and caps its probes"
) {
    const Stand stand = {String("a browsing session")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestConnectBrowse

#endif
