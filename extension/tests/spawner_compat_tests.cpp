#include "support/fault_log.h"
#include "support/internals.h"
#include "support/netw_test.h"

#include "godot/callable.hpp"
#include "godot/multiplayer_spawner.hpp"
#include "godot/node.hpp"
#include "godot/variant.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/spawn/book.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/spawn/record.hpp"
#include "netw/spawn/spawner_compat.hpp"

namespace TestNetwSpawnerCompat {

using namespace godot;
using netw::spawn::SpawnerCompat;

Node *build_probe(const Variant &) {
    return memnew(Node);
}

struct Stand {
    Ref<netw::NetwMultiplayer> core;
    SpawnerCompat *compat = nullptr;
    netw::spawn::Book *book = nullptr;
    MultiplayerSpawner *spawner = nullptr;

    Stand() {
        core.instantiate();
        netw::ReplicationCore *plane
            = netw_test::Internals::replication_plane(core);
        compat = plane->get_spawner_compat();
        book = plane->get_spawn_pipeline()->get_spawn_book();
        spawner = memnew(MultiplayerSpawner);
        spawner->set_spawn_function(callable_mp_static(&build_probe));
    }

    ~Stand() {
        memdelete(spawner);
    }

    Node *spawn_through_wrap(const Variant &p_data) {
        const Variant made = spawner->get_spawn_function().call(p_data);
        return Object::cast_to<Node>(netw::gd::live_object(made));
    }

    int64_t drops() const {
        return int64_t(
            compat->counters()[StringName("drops_uncaptured_custom")]
        );
    }
};

TEST_CASE(
    "[Netw][Spawn][Hosted] a captured argument arms the node it was "
    "spawned with, once"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*[NW1020]*", 1);

    Stand stand;
    stand.compat->wrap_spawner(stand.spawner);

    Node *made = stand.spawn_through_wrap(Variant("carried"));
    REQUIRE(made != nullptr);

    NETW_CHECK_EQ(int(stand.compat->consume(made, stand.spawner)), int(OK));
    CHECK(stand.core->spawn_books_node(made));
    NETW_CHECK_EQ(stand.book->armed_count(), 1);
    NETW_CHECK_EQ(stand.drops(), int64_t(0));

    netw::spawn::Record record;
    REQUIRE(stand.book->take_armed(stand.book->armed_routes()[0], record));
    REQUIRE_FALSE(stand.core->spawn_books_node(made));
    NETW_CHECK_EQ(int(stand.compat->consume(made, stand.spawner)), int(OK));
    NETW_CHECK_EQ(stand.book->armed_count(), 0);
    NETW_CHECK_EQ(stand.drops(), int64_t(1));

    memdelete(made);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a node this peer already books is ignored "
    "rather than given a second identity"
) {
    Stand stand;
    stand.compat->wrap_spawner(stand.spawner);

    Node *made = stand.spawn_through_wrap(Variant("carried"));
    REQUIRE(made != nullptr);
    stand.compat->consume(made, stand.spawner);

    NETW_CHECK_EQ(int(stand.compat->consume(made, stand.spawner)), int(OK));
    NETW_CHECK_EQ(stand.book->armed_count(), 1);
    NETW_CHECK_EQ(stand.drops(), int64_t(0));

    memdelete(made);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a container argument is deep copied, so the "
    "record carries what was spawned rather than what the caller kept"
) {
    Stand stand;
    stand.compat->wrap_spawner(stand.spawner);

    Array carried;
    carried.push_back(7);
    Node *made = stand.spawn_through_wrap(carried);
    REQUIRE(made != nullptr);
    carried.push_back(9);

    stand.compat->consume(made, stand.spawner);

    const PackedInt64Array armed = stand.book->armed_routes();
    REQUIRE(armed.size() == 1);
    netw::spawn::Record record;
    REQUIRE(stand.book->take_armed(armed[0], record));
    const Array recorded = record.get_custom_data();
    NETW_CHECK_EQ(recorded.size(), 1);

    memdelete(made);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a second wrap does not chain, so a receiver "
    "reconstructing through the original captures nothing"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*[NW1020]*", 1);

    Stand stand;
    stand.compat->wrap_spawner(stand.spawner);
    stand.compat->wrap_spawner(stand.spawner);

    Node *made
        = stand.compat->instantiate(stand.spawner, -1, Variant("carried"));
    REQUIRE(made != nullptr);

    NETW_CHECK_EQ(int(stand.compat->consume(made, stand.spawner)), int(OK));
    CHECK_FALSE(stand.core->spawn_books_node(made));
    NETW_CHECK_EQ(stand.drops(), int64_t(1));

    memdelete(made);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a registration with no session to arm through "
    "is refused rather than silently lost"
) {
    Stand stand;
    SpawnerCompat unseated;
    Node *made = memnew(Node);

    NETW_CHECK_EQ(
        int(unseated.consume(made, stand.spawner)),
        int(ERR_UNCONFIGURED)
    );
    NETW_CHECK_EQ(
        int(unseated.consume(nullptr, stand.spawner)),
        int(ERR_INVALID_PARAMETER)
    );
    NETW_CHECK_EQ(
        int(unseated.consume(made, nullptr)),
        int(ERR_INVALID_PARAMETER)
    );

    memdelete(made);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] the receiver-side spawn limit counts what "
    "this peer produced through that spawner"
) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_warning("*MultiplayerSpawner*[NW1018]*", 1);

    Stand stand;
    stand.spawner->set_spawn_limit(1);

    Node *first = stand.compat->instantiate(stand.spawner, -1, Variant());
    REQUIRE(first != nullptr);
    stand.compat->note_recv(31, stand.spawner);

    Node *refused = stand.compat->instantiate(stand.spawner, -1, Variant());
    const bool over_limit_refused = refused == nullptr;
    if (refused != nullptr) {
        memdelete(refused);
    }
    CHECK(over_limit_refused);

    stand.compat->emit_despawned(31, first);
    Node *again = stand.compat->instantiate(stand.spawner, -1, Variant());
    const bool freed_slot_allows = again != nullptr;
    if (again != nullptr) {
        memdelete(again);
    }
    CHECK(freed_slot_allows);

    memdelete(first);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a scene index the spawner does not offer is "
    "refused rather than reconstructed through the spawn function"
) {
    Stand stand;

    const bool empty_list_refused
        = stand.compat->instantiate(stand.spawner, 0, Variant()) == nullptr;
    const bool past_end_refused
        = stand.compat->instantiate(stand.spawner, 3, Variant()) == nullptr;
    const bool no_spawner_refused
        = stand.compat->instantiate(nullptr, -1, Variant()) == nullptr;

    CHECK(empty_list_refused);
    CHECK(past_end_refused);
    CHECK(no_spawner_refused);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a spawner with no spawn function is enrolled "
    "and left unwrapped, so the wrap can wait for it"
) {
    Stand stand;
    stand.spawner->set_spawn_function(Callable());

    stand.compat->register_spawner(stand.spawner);

    NETW_CHECK_EQ(stand.compat->get_roster().size(), 1);
    CHECK_FALSE(stand.spawner->get_spawn_function().is_valid());

    stand.spawner->set_spawn_function(callable_mp_static(&build_probe));
    stand.compat->register_spawner(stand.spawner);

    NETW_CHECK_EQ(stand.compat->get_roster().size(), 1);
    const bool wrapped_now
        = stand.spawner->get_spawn_function().get_object() == stand.core.ptr();
    CHECK(wrapped_now);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] a session that ends gives the spawners back "
    "their own functions and leaves an empty roster, because the peer holds "
    "no claim on a spawner past the session it enrolled it for"
) {
    Stand stand;
    const Callable own = stand.spawner->get_spawn_function();
    stand.compat->register_spawner(stand.spawner);

    NETW_CHECK_EQ(stand.compat->get_roster().size(), 1);
    CHECK(stand.spawner->get_spawn_function().get_object() == stand.core.ptr());

    stand.core->session_announce_ended();

    NETW_CHECK_EQ(stand.compat->get_roster().size(), 1);

    stand.core->settle_drain();

    NETW_CHECK_EQ(stand.compat->get_roster().size(), 0);
    CHECK(stand.spawner->get_spawn_function() == own);
}

} // namespace TestNetwSpawnerCompat
