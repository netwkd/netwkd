#include "support/netw_test.h"

#include <cstdint>

#include "godot/node.hpp"
#include "godot/scene_tree.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/spawn/book.hpp"
#include "netw/spawn/record.hpp"

namespace TestNetwSpawnDespawn {

using namespace godot;
using netw::NetwEntity;
using netw::NetwMultiplayer;
using netw::spawn::Book;
using netw::spawn::Record;

const int64_t DESPAWN_CHANNEL = 15;

Record *tracked(
    Book *p_book,
    int64_t p_route,
    int64_t p_parent_route,
    Node *p_node
) {
    Record record;
    record.set_route(p_route);
    record.set_parent_route(p_parent_route);
    record.bind_node(p_node);
    return p_book->issue(record);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] SD1 an untracked route despawns nothing and "
    "says so, so the caller flushes no carrier for it"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Book book;
    Node *node = memnew(Node);
    tracked(&book, 1, 0, node);

    CHECK_FALSE(core->spawn_despawn_route(
        &book,
        9,
        PackedInt32Array(),
        DESPAWN_CHANNEL
    ));
    CHECK(book.has_spawned(1));

    CHECK_FALSE(core->spawn_despawn_route(
        nullptr,
        1,
        PackedInt32Array(),
        DESPAWN_CHANNEL
    ));
    CHECK(book.has_spawned(1));

    memdelete(node);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] SD2 a despawn empties the book of the whole "
    "subtree"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Book book;
    Node *parent = memnew(Node);
    Node *child = memnew(Node);
    Node *grandchild = memnew(Node);
    NetwEntity::ensure(parent);
    NetwEntity::ensure(child);
    NetwEntity::ensure(grandchild);
    tracked(&book, 1, 0, parent);
    tracked(&book, 2, 1, child);
    tracked(&book, 3, 2, grandchild);

    CHECK(core->spawn_despawn_route(
        &book,
        1,
        PackedInt32Array(),
        DESPAWN_CHANNEL
    ));

    CHECK_FALSE(book.has_spawned(1));
    CHECK_FALSE(book.has_spawned(2));
    CHECK_FALSE(book.has_spawned(3));
    NETW_CHECK_EQ(book.spawned_count(), 0);

    memdelete(grandchild);
    memdelete(child);
    memdelete(parent);
}

TEST_CASE(
    "[Netw][Spawn][Hosted] SD3 a record whose node carries no entity is "
    "still dropped, because the book's plan is what the despawn honours"
) {
    Ref<NetwMultiplayer> core;
    core.instantiate();
    Book book;
    Node *bare = memnew(Node);
    Node *carrying = memnew(Node);
    NetwEntity::ensure(carrying);
    tracked(&book, 1, 0, bare);
    tracked(&book, 2, 1, carrying);

    CHECK(core->spawn_despawn_route(
        &book,
        1,
        PackedInt32Array(),
        DESPAWN_CHANNEL
    ));

    NETW_CHECK_EQ(book.spawned_count(), 0);

    memdelete(carrying);
    memdelete(bare);
}

} // namespace TestNetwSpawnDespawn
