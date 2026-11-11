#include "support/fault_log.h"
#include "support/frame_drive.h"
#include "support/lan_directory_probe.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"
#include "support/session_state_walk.h"

#include <memory>
#include <vector>

#include "godot/class_db.hpp"
#include "netw/api/connection.hpp"
#include "netw/api/endpoint.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/nodes/lan_lobby_directory.hpp"
#include "netw/api/session.hpp"

using namespace godot;

namespace TestConnectLifecycleMatrix {

namespace {

using netw::NetwEndpoint;
using netw::NetwSession;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;
using netw_test::StateHeard;

constexpr int GAMEPLAY_PORT_START = 30700;
constexpr int GAMEPLAY_PORT_END = 30800;
constexpr int CLOSED_PORT = 30899;
constexpr int DISCOVERY_PORT = 30660;
constexpr double FRAME = 0.05;
constexpr double LIST_INTERVAL = 1.0;
constexpr int STEP_FRAME_BUDGET = 160;
const int ROW_GONE_FRAMES
    = int((2.0 * LIST_INTERVAL + netw::LANLobbyDirectory::BROWSE_WINDOW)
          / FRAME)
    + 2;
constexpr int SIDES = 3;
constexpr int HOST = 0;
constexpr int A = 1;
constexpr int B = 2;
const char *const SIDE_NAMES[SIDES] = {"H", "A", "B"};
constexpr int ONLINE = NetwSession::STATE_ONLINE;
constexpr int OFFLINE = NetwSession::STATE_OFFLINE;

enum Act {
    HOSTS,
    JOINS,
    JOINS_TYPED,
    JOINS_BLANK,
    LEAVES,
    CLOSES_PEER,
    KICKED,
    HIDES_LIST,
    SHOWS_LIST,
    JOINS_CLOSED,
    CANCELS,
    ADDS_ROW,
};

const char *const ACT_NAMES[] = {
    "hosts",
    "joins",
    "joins typed",
    "joins blank",
    "leaves",
    "closes peer",
    "kicked",
    "hides list",
    "shows list",
    "joins closed",
    "cancels",
    "adds row",
};

struct Step {
    Act act;
    int side;
};

struct Scenario {
    const char *label;
    std::vector<Step> steps;
};

const std::vector<Scenario> &scenarios() {
    static const std::vector<Scenario> declared = {
        {"a client joins by LAN and leaves",
         {{HOSTS, HOST}, {JOINS, A}, {LEAVES, A}}},
        {"the host leaves under a client",
         {{HOSTS, HOST}, {JOINS, A}, {LEAVES, HOST}}},
        {"the host closes its peer and hosts again",
         {{HOSTS, HOST},
          {JOINS, A},
          {CLOSES_PEER, HOST},
          {HOSTS, HOST},
          {JOINS, A}}},
        {"the host kicks a client", {{HOSTS, HOST}, {JOINS, A}, {KICKED, A}}},
        {"two clients join and one leaves",
         {{HOSTS, HOST}, {JOINS, A}, {JOINS, B}, {LEAVES, A}}},
        {"a joined client hides and shows the list",
         {{HOSTS, HOST},
          {JOINS, A},
          {HIDES_LIST, A},
          {SHOWS_LIST, A},
          {LEAVES, A}}},
        {"a join to a closed port fails, then a host appears",
         {{JOINS_CLOSED, A}, {HOSTS, HOST}, {JOINS, A}}},
        {"a client joins a typed address and the host leaves",
         {{HOSTS, HOST}, {JOINS_TYPED, A}, {LEAVES, HOST}}},
        {"a cancelled join, then a join",
         {{HOSTS, HOST}, {CANCELS, A}, {JOINS, A}}},
        {"the host closes its peer under two clients",
         {{HOSTS, HOST}, {JOINS, A}, {JOINS, B}, {CLOSES_PEER, HOST}}},
        {"a client leaves and joins again, then the host leaves",
         {{HOSTS, HOST}, {JOINS, A}, {LEAVES, A}, {JOINS, A}, {LEAVES, HOST}}},
        {"a client joins a blank address with a port and leaves",
         {{HOSTS, HOST}, {JOINS_BLANK, A}, {LEAVES, A}}},
        {"a client joins a blank address and the host leaves",
         {{HOSTS, HOST}, {JOINS_BLANK, A}, {LEAVES, HOST}}},
        {"a client adds the host by name and the host leaves",
         {{HOSTS, HOST}, {ADDS_ROW, A}, {LEAVES, HOST}}},
        {"two hosts on one machine are found and a client joins one",
         {{HOSTS, HOST}, {HOSTS, B}, {JOINS, A}}},
    };
    return declared;
}

bool stops_hosting(const Step &p_step) {
    return p_step.side == HOST
        && (p_step.act == LEAVES || p_step.act == CLOSES_PEER);
}

struct Moment {
    String label;
    Step step = {HOSTS, HOST};
    int side = HOST;
    bool settled = false;
    int frames = 0;
    int was = OFFLINE;
    int state = OFFLINE;
    int host_state = OFFLINE;
    bool server_role = false;
    bool admitted = false;
    String join_address;
    String host_address;
    bool has_current = false;
    String current_address;
    std::vector<StateHeard> heard;
    String incomplete;
    String twin;
    String unlisted;
    String lan_online;
    String no_ping;
    PackedStringArray errors;
    PackedStringArray warnings;
};

std::vector<Moment> &moments() {
    static std::vector<Moment> recorded;
    return recorded;
}

int &scenarios_closed() {
    static int closed = 0;
    return closed;
}

struct Browser {
    Ref<NetwSession> door;
    Ref<netw::NetwConnection> connection;
    std::vector<Ref<NetwEndpoint>> shown;
    std::vector<StateHeard> heard;
    bool list_visible = true;
    int failed = 0;
    String incomplete;
    String twin;
    String unlisted;
};

Browser &browser(int64_t p_side) {
    static Browser browsers[SIDES];
    return browsers[p_side];
}

void apply_browsing(int64_t p_side) {
    Browser &held = browser(p_side);
    const bool online = held.door->get_state() >= ONLINE;
    held.connection->set_browsing(held.list_visible && !online);
}

void on_endpoint_added(const Variant &p_endpoint, int64_t p_side) {
    browser(p_side).shown.push_back(p_endpoint);
}

void on_endpoint_removed(const Variant &p_endpoint, int64_t p_side) {
    const Ref<NetwEndpoint> gone = p_endpoint;
    std::vector<Ref<NetwEndpoint>> &shown = browser(p_side).shown;
    for (size_t at = 0; at < shown.size(); at++) {
        if (shown[at] == gone) {
            shown.erase(shown.begin() + at);
            return;
        }
    }
}

void on_state_changed(int64_t p_state, int64_t p_side) {
    Browser &held = browser(p_side);
    held.heard.push_back({p_state, int64_t(held.door->get_state())});
    apply_browsing(p_side);
}

void on_failed(int64_t, const String &, int64_t p_side) {
    browser(p_side).failed += 1;
}

String describe(const Ref<NetwEndpoint> &p_row) {
    return vformat(
        "[%s|%s|%s|%d|%s]",
        String(p_row->get_peer_class()),
        p_row->get_address(),
        p_row->get_source(),
        int(p_row->get_status()),
        p_row->get_is_listed() ? "listed" : "unlisted"
    );
}

void first(String &r_finding, const String &p_what) {
    if (r_finding.is_empty()) {
        r_finding = p_what;
    }
}

struct Side {
    Ref<netw::NetwMultiplayer> session;
    Ref<NetwSession> door;
    Ref<netw::NetwConnection> connection;
    netw::LANLobbyDirectory *directory = nullptr;
    int port = 0;
    int left_id = 0;
};

class ConnectLifecycleMatrix final : public netw_test::FrameScenario {
    Side sides[SIDES];
    size_t scenario_at = 0;
    size_t step_at = 0;
    bool open = false;
    bool acted = false;
    bool cancelled = false;
    int frames = 0;
    int failed_before = 0;
    int was[SIDES] = {OFFLINE, OFFLINE, OFFLINE};
    int next_port = GAMEPLAY_PORT_START;
    std::unique_ptr<netw_test::FaultWatch> faults;
    int64_t errors_mark = 0;
    int64_t warnings_mark = 0;

    const Step &step() const {
        return scenarios()[scenario_at].steps[step_at];
    }

    int state_of(int p_side) const {
        return int(sides[p_side].door->get_state());
    }

    String address_of(int p_side) const {
        return String("127.0.0.1:") + itos(sides[p_side].port);
    }

    bool hosting(int p_side) const {
        return sides[p_side].port > 0 && state_of(p_side) == ONLINE
            && sides[p_side].session->has_server_role();
    }

    void mark_faults() {
        errors_mark = faults->errors_logged();
        warnings_mark = faults->warnings_logged();
    }

    int pick_port() {
        for (; next_port < GAMEPLAY_PORT_END; next_port++) {
            const Ref<MultiplayerPeer> trial(
                ClassDB::instantiate(StringName("ENetMultiplayerPeer"))
            );
            if (trial.is_null()) {
                return 0;
            }
            if (Error(int(trial->call(StringName("create_server"), next_port)))
                == OK) {
                trial->close();
                return next_port++;
            }
        }
        return 0;
    }

    void open_world() {
        mark_faults();
        for (int at = 0; at < SIDES; at++) {
            Side &side = sides[at];
            side.session.instantiate();
            side.door = side.session->get_session();
            side.connection = side.session->get_connection();
            side.port = 0;
            side.directory = memnew(netw::LANLobbyDirectory);
            side.directory->set_port(DISCOVERY_PORT);
            side.directory->set_port_count(SIDES);
            side.directory->set_list_interval(LIST_INTERVAL);
            netw::LANLobbyDirectoryProbe::query_loopback(side.directory);
            side.directory->service_entered(side.session.ptr());
            side.session->service_register(side.directory, nullptr);
            Browser &held = browser(at);
            held = Browser();
            held.door = side.door;
            held.connection = side.connection;
            const Variant who = int64_t(at);
            side.connection->connect(
                StringName("endpoint_added"),
                callable_mp_static(&on_endpoint_added).bind(who)
            );
            side.connection->connect(
                StringName("endpoint_removed"),
                callable_mp_static(&on_endpoint_removed).bind(who)
            );
            side.door->connect(
                StringName("state_changed"),
                callable_mp_static(&on_state_changed).bind(who)
            );
            side.connection->connect(
                StringName("failed"),
                callable_mp_static(&on_failed).bind(who)
            );
            apply_browsing(at);
        }
    }

    void close_world() {
        mark_faults();
        for (int at = SIDES - 1; at >= 0; at--) {
            Side &side = sides[at];
            side.connection->set_browsing(false);
            side.session->service_unregister(side.directory, nullptr);
            side.directory->service_exiting(side.session.ptr());
            memdelete(side.directory);
            side.session->NETW_API_VIRTUAL(set_multiplayer_peer)(
                Ref<MultiplayerPeer>()
            );
            side.session->embed_dispose();
            browser(at) = Browser();
            side = Side();
        }
        moments().back().errors.append_array(faults->error_lines(errors_mark));
        scenarios_closed() += 1;
    }

    void pump() {
        for (Side &side : sides) {
            side.session->NETW_API_VIRTUAL(poll)();
            side.session->connect_plane().on_poll(FRAME);
            netw::LANLobbyDirectoryProbe::tick(side.directory, FRAME);
        }
    }

    Ref<NetwEndpoint> row_of(
        int p_side,
        const String &p_source,
        int p_host
    ) const {
        const String wanted = p_source.is_empty()
            ? String("localhost:") + itos(sides[p_host].port)
            : address_of(p_host);
        const TypedArray<NetwEndpoint> rows
            = sides[p_side].connection->endpoints();
        for (int64_t at = 0; at < rows.size(); at++) {
            const Ref<NetwEndpoint> row = rows[at];
            if (row->get_source() == p_source
                && row->get_address() == wanted) {
                return row;
            }
        }
        return Ref<NetwEndpoint>();
    }

    bool lists_hosts(int p_side) const {
        const TypedArray<NetwEndpoint> rows
            = sides[p_side].connection->endpoints();
        int unmatched = 0;
        for (int64_t at = 0; at < rows.size(); at++) {
            const Ref<NetwEndpoint> row = rows[at];
            unmatched += row->get_source() == String("LAN") ? 1 : 0;
        }
        for (int at = 0; at < SIDES; at++) {
            if (!hosting(at)) {
                continue;
            }
            if (row_of(p_side, "LAN", at).is_null()) {
                return false;
            }
            unmatched -= 1;
        }
        return unmatched == 0;
    }

    bool seats(int p_peer) const {
        return p_peer > 1
            && sides[HOST].session->NETW_API_VIRTUAL(get_peer_ids)().has(
                p_peer
            );
    }

    void prepare(int p_side) {
        sides[p_side].session->session_prepare_join(
            StringName(SIDE_NAMES[p_side]),
            Array()
        );
    }

    Ref<NetwEndpoint> added_row(int p_side, const String &p_address) {
        return sides[p_side].connection->add(
            StringName("ENetMultiplayerPeer"),
            p_address,
            String()
        );
    }

    bool act() {
        const Step &now = step();
        Side &side = sides[now.side];
        failed_before = browser(now.side).failed;
        Dictionary settings;
        switch (now.act) {
            case HOSTS:
                if (side.port == 0) {
                    side.port = pick_port();
                }
                prepare(now.side);
                settings["port"] = side.port;
                side.connection->host(
                    StringName("ENetMultiplayerPeer"),
                    settings
                );
                return true;
            case JOINS: {
                const Ref<NetwEndpoint> row = row_of(now.side, "LAN", HOST);
                if (row.is_null()) {
                    return false;
                }
                prepare(now.side);
                side.connection->join(row, settings);
                return true;
            }
            case JOINS_TYPED:
                prepare(now.side);
                side.connection->join(
                    added_row(now.side, address_of(HOST)),
                    settings
                );
                return true;
            case JOINS_BLANK:
                prepare(now.side);
                settings["port"] = sides[HOST].port;
                side.connection->join(added_row(now.side, String()), settings);
                return true;
            case ADDS_ROW:
                added_row(now.side, "localhost:" + itos(sides[HOST].port));
                return true;
            case LEAVES:
                side.left_id = side.session->NETW_API_VIRTUAL(get_unique_id)();
                side.session->session_leave();
                return true;
            case CLOSES_PEER:
                side.session->NETW_API_VIRTUAL(set_multiplayer_peer)(
                    Ref<MultiplayerPeer>()
                );
                return true;
            case KICKED: {
                side.left_id = side.session->NETW_API_VIRTUAL(get_unique_id)();
                const Ref<netw::NetwPlayer> player
                    = sides[HOST].door->player_of(side.left_id);
                if (player.is_null()) {
                    return false;
                }
                sides[HOST].door->kick(player, String("matrix"));
                return true;
            }
            case HIDES_LIST:
            case SHOWS_LIST:
                browser(now.side).list_visible = now.act == SHOWS_LIST;
                apply_browsing(now.side);
                return true;
            case JOINS_CLOSED:
            case CANCELS:
                cancelled = false;
                prepare(now.side);
                side.connection->join(
                    added_row(now.side, "127.0.0.1:" + itos(CLOSED_PORT)),
                    settings
                );
                return true;
        }
        return true;
    }

    bool step_settled() {
        const Step &now = step();
        const Side &side = sides[now.side];
        if (stops_hosting(now)) {
            return state_of(HOST) == OFFLINE && state_of(A) == OFFLINE
                && state_of(B) == OFFLINE;
        }
        switch (now.act) {
            case HOSTS:
                return state_of(now.side) == ONLINE
                    && !side.connection->get_join_address().is_empty();
            case JOINS:
            case JOINS_TYPED:
            case JOINS_BLANK:
                return state_of(now.side) == ONLINE
                    && seats(side.session->NETW_API_VIRTUAL(get_unique_id)())
                    && side.session->player_local().is_valid();
            case LEAVES:
            case CLOSES_PEER:
            case KICKED:
                return state_of(now.side) == OFFLINE && !seats(side.left_id);
            case HIDES_LIST:
            case SHOWS_LIST:
                return side.connection->is_browsing()
                    == (browser(now.side).list_visible
                        && state_of(now.side) != ONLINE);
            case ADDS_ROW: {
                const Ref<NetwEndpoint> row = row_of(now.side, "", HOST);
                return row.is_valid()
                    && row->get_status() == NetwEndpoint::STATUS_ONLINE;
            }
            case JOINS_CLOSED:
                return state_of(now.side) == OFFLINE
                    && browser(now.side).failed > failed_before;
            case CANCELS:
                if (!cancelled
                    && state_of(now.side) == NetwSession::STATE_CONNECTING) {
                    side.connection->cancel();
                    cancelled = true;
                }
                return cancelled && state_of(now.side) == OFFLINE;
        }
        return true;
    }

    bool views_settled() const {
        for (int at = 0; at < SIDES; at++) {
            if (at != HOST && sides[at].connection->is_browsing()
                && !lists_hosts(at)) {
                return false;
            }
        }
        return true;
    }

    void audit() {
        for (int at = 0; at < SIDES; at++) {
            Browser &held = browser(at);
            const TypedArray<NetwEndpoint> rows
                = sides[at].connection->endpoints();
            for (int64_t row = 0; row < rows.size(); row++) {
                const Ref<NetwEndpoint> one = rows[row];
                if (one->get_peer_class() == StringName()
                    || one->get_address().is_empty()) {
                    first(held.incomplete, describe(one));
                }
                for (int64_t other = row + 1; other < rows.size(); other++) {
                    const Ref<NetwEndpoint> twin = rows[other];
                    if (twin->get_source() == one->get_source()
                        && twin->get_address() == one->get_address()) {
                        first(held.twin, describe(one));
                    }
                }
            }
            for (const Ref<NetwEndpoint> &one : held.shown) {
                if (!one->get_is_listed()) {
                    first(held.unlisted, describe(one));
                }
            }
        }
    }

    void record(bool p_settled) {
        const Step &now = step();
        const PackedStringArray errors = faults->error_lines(errors_mark);
        const PackedStringArray warnings = faults->warning_lines(warnings_mark);
        mark_faults();
        for (int at = 0; at < SIDES; at++) {
            const Side &side = sides[at];
            Browser &held = browser(at);
            Moment seen;
            seen.label = vformat(
                "%s, step %d %s %s, side %s",
                scenarios()[scenario_at].label,
                int64_t(step_at),
                SIDE_NAMES[now.side],
                ACT_NAMES[now.act],
                SIDE_NAMES[at]
            );
            seen.step = now;
            seen.side = at;
            seen.settled = p_settled;
            seen.frames = frames;
            seen.was = was[at];
            seen.state = state_of(at);
            seen.host_state = state_of(HOST);
            seen.server_role = side.session->has_server_role();
            seen.admitted = side.session->player_local().is_valid();
            seen.join_address = side.connection->get_join_address();
            seen.host_address = sides[HOST].connection->get_join_address();
            const Ref<NetwEndpoint> current
                = side.connection->get_current_endpoint();
            seen.has_current = current.is_valid();
            seen.current_address
                = seen.has_current ? current->get_address() : String();
            seen.heard = held.heard;
            seen.incomplete = held.incomplete;
            seen.twin = held.twin;
            seen.unlisted = held.unlisted;
            const TypedArray<NetwEndpoint> rows = side.connection->endpoints();
            for (int64_t row = 0; row < rows.size(); row++) {
                const Ref<NetwEndpoint> one = rows[row];
                if (one->get_status() != NetwEndpoint::STATUS_ONLINE) {
                    continue;
                }
                if (one->get_source() == String("LAN")) {
                    first(seen.lan_online, describe(one));
                }
                if (one->get_source().is_empty()
                    && one->get_info()->get_latency_ms() < 0) {
                    first(seen.no_ping, describe(one));
                }
            }
            if (at == now.side) {
                seen.errors = errors;
                seen.warnings = warnings;
            }
            moments().push_back(seen);
        }
    }

    void begin_step() {
        frames = 0;
        acted = false;
        for (int at = 0; at < SIDES; at++) {
            was[at] = state_of(at);
            Browser &held = browser(at);
            held.heard.clear();
            held.incomplete = String();
            held.twin = String();
            held.unlisted = String();
        }
    }

public:
    bool advance() override {
        if (scenario_at >= scenarios().size()) {
            return false;
        }
        if (!faults) {
            faults = std::make_unique<netw_test::FaultWatch>();
        }
        if (!open) {
            open_world();
            open = true;
            step_at = 0;
            begin_step();
            return true;
        }
        pump();
        frames += 1;
        if (!acted) {
            acted = act();
        }
        audit();
        const bool settled = acted && step_settled() && views_settled();
        if (!settled && frames < STEP_FRAME_BUDGET) {
            return true;
        }
        record(settled);
        step_at += 1;
        if (settled && step_at < scenarios()[scenario_at].steps.size()) {
            begin_step();
            return true;
        }
        close_world();
        open = false;
        scenario_at += 1;
        if (scenario_at < scenarios().size()) {
            return true;
        }
        faults.reset();
        return false;
    }
};

NETW_FRAME_SCENARIO(ConnectLifecycleMatrix, connect_lifecycle_matrix);

LawVerdict found(const String &p_finding) {
    if (p_finding.is_empty()) {
        return law_held();
    }
    return law_broken("%s", p_finding.utf8().get_data());
}

LawVerdict settles(const Moment &p_seen) {
    if (p_seen.settled) {
        return law_held();
    }
    return law_broken("unsettled after %d frames", p_seen.frames);
}

LawVerdict rows_complete(const Moment &p_seen) {
    return found(p_seen.incomplete);
}

LawVerdict quiet(const Moment &p_seen) {
    const int64_t allowed = p_seen.step.act == JOINS_CLOSED ? 1 : 0;
    if (!p_seen.errors.is_empty()) {
        return found(p_seen.errors[0]);
    }
    if (p_seen.warnings.size() > allowed) {
        return found(p_seen.warnings[0]);
    }
    return law_held();
}

int expected_state(const Moment &p_seen) {
    const int actor = p_seen.step.side;
    const bool acting = p_seen.side == actor;
    if (stops_hosting(p_seen.step)) {
        return OFFLINE;
    }
    switch (p_seen.step.act) {
        case HOSTS:
            return acting ? ONLINE : -1;
        case JOINS:
        case JOINS_TYPED:
        case JOINS_BLANK:
            return acting || p_seen.side == HOST ? ONLINE : -1;
        case LEAVES:
        case CLOSES_PEER:
        case KICKED:
            return acting ? OFFLINE : (p_seen.side == HOST ? ONLINE : -1);
        case JOINS_CLOSED:
        case CANCELS:
            return acting ? OFFLINE : -1;
        case ADDS_ROW:
            return p_seen.side == HOST ? ONLINE : -1;
        case HIDES_LIST:
        case SHOWS_LIST:
            return p_seen.was;
    }
    return -1;
}

LawVerdict state_follows(const Moment &p_seen) {
    const int wanted = expected_state(p_seen);
    if (wanted < 0 || p_seen.state == wanted) {
        return law_held();
    }
    return law_broken("state %d, wanted %d", p_seen.state, wanted);
}

LawVerdict join_address(const Moment &p_seen) {
    const bool online = p_seen.state == ONLINE;
    if (!online && !p_seen.join_address.is_empty()) {
        return found("offline at " + p_seen.join_address);
    }
    if (online && p_seen.server_role && p_seen.join_address.is_empty()) {
        return found("hosts with no address");
    }
    if (online && !p_seen.server_role && p_seen.admitted
        && p_seen.host_state == ONLINE
        && p_seen.join_address != p_seen.host_address) {
        return found(
            vformat("%s is not %s", p_seen.join_address, p_seen.host_address)
        );
    }
    return law_held();
}

LawVerdict current_endpoint(const Moment &p_seen) {
    const bool online = p_seen.state == ONLINE;
    if (p_seen.has_current != online) {
        return found("current endpoint disagrees with the state");
    }
    const bool addressed = p_seen.server_role
        ? p_seen.current_address == p_seen.join_address
        : !p_seen.current_address.is_empty();
    if (online && !addressed) {
        return found("current endpoint lost its address");
    }
    return law_held();
}

LawVerdict one_row_per_address(const Moment &p_seen) {
    return found(p_seen.twin);
}

LawVerdict stopped_host_leaves(const Moment &p_seen) {
    if (!stops_hosting(p_seen.step)) {
        return law_held();
    }
    if (!p_seen.lan_online.is_empty()) {
        return found("still lists " + p_seen.lan_online);
    }
    if (p_seen.frames > ROW_GONE_FRAMES) {
        return law_broken("rows left after %d frames", p_seen.frames);
    }
    return law_held();
}

LawVerdict shown_rows_listed(const Moment &p_seen) {
    return found(p_seen.unlisted);
}

LawVerdict added_rows_ping(const Moment &p_seen) {
    return found(p_seen.no_ping);
}

LawVerdict state_changed_walks(const Moment &p_seen) {
    const LawVerdict walk = netw_test::law_state_walk(p_seen.heard, p_seen.was);
    if (!walk.held) {
        return walk;
    }
    const int64_t last
        = p_seen.heard.empty() ? p_seen.was : p_seen.heard.back().emitted;
    if (last != p_seen.state) {
        return law_broken("last heard %d, reads %d", int(last), p_seen.state);
    }
    return law_held();
}

typedef netw_test::LawRowFor<Moment> Law;

const Law LAWS[] = {
    {"V0", "every step acts and settles within its frame budget", &settles},
    {"V1",
     "every row in endpoints has a peer class and an address",
     &rows_complete},
    {"V2", "no step raises an error, and only a failed join warns", &quiet},
    {"V3",
     "the connection state follows the step on every side it reaches",
     &state_follows},
    {"V4",
     "join_address is empty offline, set while hosting, and an admitted "
     "client reads the host's",
     &join_address},
    {"V5",
     "no two rows from one source share an address",
     &one_row_per_address},
    {"V6",
     "a host that stops hosting leaves every browser within two list "
     "intervals",
     &stopped_host_leaves},
    {"V7", "every row a browser shows is listed", &shown_rows_listed},
    {"V8",
     "every ONLINE row added with connection.add reports a ping",
     &added_rows_ping},
    {"V9",
     "a host or client holds one current endpoint while online",
     &current_endpoint},
    {"V10",
     "every state_changed carries the state read at that moment, and the "
     "values climb and then return to STATE_OFFLINE with no repeat",
     &state_changed_walks},
};

} // namespace

TEST_CASE(
    "[Networked][Connect][Frame] hosts and browsers over real ENet and LAN "
    "discovery keep every connection view coherent through each lifecycle "
    "combination"
) {
    NETW_REQUIRE_EQ(scenarios_closed(), int(scenarios().size()));
    for (const Moment &seen : moments()) {
        for (const Law &law : LAWS) {
            NETW_CELL(law, seen);
            NETW_LAW_HOLDS(law, seen);
        }
    }
}

} // namespace TestConnectLifecycleMatrix
