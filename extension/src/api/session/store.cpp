#include "godot/node.hpp"
#include "godot/object.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/replication_core.hpp"
#include "netw/colors.hpp"
#include "netw/profile.hpp"
#include "netw/repl/property_binding.hpp"
#include "netw/repl/set_model.hpp"
#include "netw/spawn/book.hpp"
#include "netw/spawn/pipeline.hpp"
#include "netw/stage.hpp"
#include "netw/store/schedule.hpp"
#include "netw/store/validate.hpp"
#include "netw/wire/registry.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBinding;
using repl::PropertyBindingPtr;
using repl::PropertySet;

namespace {

void follow_set(store::Table &r_table, const PropertySet *p_set) {
    r_table.heartbeat = p_set->heartbeat;
    r_table.window = uint32_t(p_set->window > 0 ? p_set->window : 1);
    if (r_table.family == store::Family::RETAINED) {
        r_table.channel = wire::CHANNEL_SYNC_ROW_DELTA;
    } else if (r_table.family == store::Family::WINDOW) {
        r_table.channel = wire::CHANNEL_SYNC_ROW_WINDOW;
    } else {
        r_table.channel = wire::CHANNEL_SYNC_ROW;
    }
}

struct SourceImage {
    uint8_t source = 0;
    uint8_t offer = 0;
    uint8_t comp = 0;
    uint8_t ordinal = 0;
    uint8_t marks = 0;
    float weight = 0.0f;
    int64_t route = 0;
    uint32_t slot = 0;
    uint64_t admitted = 0;
    wire::StreamTenure tenure;
};

SourceImage source_image(
    const store::Store &p_world,
    const store::Table &p_table,
    uint32_t p_row
) {
    SourceImage image;
    image.source = p_table.sources[p_row];
    image.offer = p_table.offers[p_row];
    image.comp = p_table.comps[p_row];
    image.ordinal = p_table.ordinals[p_row];
    image.route = p_table.routes[p_row];
    image.slot = p_table.slots[p_row];
    image.marks = p_world.marks_of(image.slot);
    image.weight = p_world.weight_of(image.slot);
    image.tenure = p_table.tenures[p_row];
    for (uint32_t peer = 0; peer < p_table.peers.size() && peer < 64; ++peer) {
        if (p_table.peers[peer].admitted.get(p_row)) {
            image.admitted |= uint64_t(1) << peer;
        }
    }
    return image;
}

const char *source_drift(const SourceImage &p_was, const SourceImage &p_now) {
    if (p_was.source != p_now.source) {
        return "source";
    }
    if (p_was.offer != p_now.offer) {
        return "offer";
    }
    if (p_was.comp != p_now.comp || p_was.ordinal != p_now.ordinal
        || p_was.route != p_now.route || p_was.slot != p_now.slot) {
        return "lane";
    }
    if (p_was.marks != p_now.marks || p_was.weight != p_now.weight) {
        return "marks";
    }
    if (p_was.admitted != p_now.admitted) {
        return "admitted";
    }
    if (!(p_was.tenure == p_now.tenure)) {
        return "tenure";
    }
    return nullptr;
}

} // namespace

void NetwMultiplayer::store_admit(const PropertyBindingPtr &p_binding) {
    Node *node = p_binding != nullptr ? p_binding->node() : nullptr;
    PropertySet *set = node != nullptr ? p_binding->get_set() : nullptr;
    if (set == nullptr) {
        return;
    }
    const uint64_t owner = p_binding->id;
    const uint64_t set_id = set->id;
    const bool windowed = p_binding->is_windowed();
    const table::SchemaRecord *lanes[2]
        = {&set->get_volatile_schema(), &set->get_retained_schema()};
    const store::Family families[2] = {
        windowed ? store::Family::WINDOW : store::Family::VOLATILE,
        store::Family::RETAINED,
    };
    for (uint32_t lane = 0; lane < 2; ++lane) {
        if (lanes[lane]->column_count() == 0) {
            continue;
        }
        const uint32_t table
            = store().table_for(set_id, families[lane], *lanes[lane]);
        store::resolve_ports(
            store().table(table),
            store_pull.residents.rows(table),
            node
        );
        follow_set(store().table(table), set);
        store_pull.add_row(
            table,
            owner,
            lane,
            uint64_t(node->get_instance_id()),
            uint8_t(p_binding->comp)
        );
    }
    store_place(p_binding);
    display_follow_binding(p_binding);
}

void NetwMultiplayer::store_place(const PropertyBindingPtr &p_binding) {
    if (p_binding == nullptr) {
        return;
    }
    const uint64_t owner = p_binding->id;
    const int64_t route = p_binding->route;
    const int64_t epoch = route > 0 ? liveness_route_epoch(route) : 0;
    for (uint32_t lane = 0; lane < 2; ++lane) {
        const store::RowAddress at = store().row_of(owner, lane);
        if (at.valid()) {
            store().place(at, route, epoch);
            store_bind(at);
        }
    }
}

void NetwMultiplayer::store_bind(const store::RowAddress &p_at) {
    const store::Table &table = store().table(p_at.table);
    const uint32_t slot = table.slots[p_at.row];
    if (slot == store::Table::NO_SLOT) {
        return;
    }
    const RID entity
        = liveness_core.rid_from_route(int(table.routes[p_at.row]));
    if (entity_get_view(entity).is_valid()) {
        store_bind_entity(entity);
    }
}

void NetwMultiplayer::store_bind_entity(const RID &p_entity) {
    const Ref<NetwEntity> view = slot_pinned_entity(p_entity);
    if (view.is_null()) {
        return;
    }
    const int64_t predicted = prediction_engine.slot_of(view);
    const int64_t rewound = lagcomp_core.timeline_slot_of(view);
    if (predicted < 0 && rewound < 0) {
        return;
    }
    const uint32_t slot = store_slot_of(p_entity);
    const store::EntityRef held = store().ref_of(slot);
    if (predicted >= 0) {
        if (slot == store::Table::NO_SLOT) {
            prediction_engine.unbind_store(predicted);
        } else {
            prediction_engine.bind_store(predicted, held);
        }
    }
    if (rewound >= 0) {
        if (slot == store::Table::NO_SLOT) {
            lagcomp_core.unbind_store(rewound);
        } else {
            lagcomp_core.bind_store(rewound, held);
        }
    }
}

void NetwMultiplayer::store_sources(
    NetwSyncModel *p_model,
    ReplicationSend *p_send,
    uint32_t p_table,
    int64_t p_tick
) {
    NETW_ZONE_NC("Store sources", colors::TABLE);
    store::Table &table = store().table(p_table);
    NETW_ZONE_TEXT_F("table %u %s", p_table, store::family_name(table.family));
    store_walk.clear();
    for (uint32_t word = 0; word < table.unsourced.words.size(); ++word) {
        uint64_t bits = table.unsourced.words[word];
        while (bits != 0) {
            store_walk.push_back(word * 64 + store::Bits::lowest(bits));
            bits &= bits - 1;
        }
    }
    table.unsourced.clear();
    NETW_ZONE_VALUE(store_walk.size());
#if defined(NETW_TESTS)
    idle_work.sources += 1;
    idle_work.sourced += int64_t(store_walk.size());
#endif
    for (uint32_t at = 0; at < store_walk.size(); ++at) {
        const store::RowAddress row{p_table, store_walk[at]};
        NETW_ZONE_LEAF_F(
            "Store source entity",
            colors::TABLE,
            profile::SUBSYSTEM_ENTITY,
            "entity %lld",
            static_cast<long long>(table.routes[row.row])
        );
        table.working.set(row.row, true);
        if (store_source_row(p_model, p_send, row, p_tick)) {
            table.unsourced.set(row.row, true);
        }
    }
    const int64_t every = store().validate_every;
    if (every > 0 && p_tick >= 0 && p_tick % every == 0) {
        NETW_ZONE_NC("Store sources audit", colors::TABLE);
        uint32_t walked = 0;
        for (uint32_t row = 0; row < table.row_count(); ++row) {
            if (walked < store_walk.size() && store_walk[walked] == row) {
                walked += 1;
                continue;
            }
            store_source_check(p_model, p_send, {p_table, row}, p_tick);
        }
    }
    p_model->note_offers(
        int64_t(table.offer_counts[store::Table::OFFER_UNAUTHORED]),
        int64_t(table.offer_counts[store::Table::OFFER_UNREACHED])
    );
}

void NetwMultiplayer::store_source_check(
    NetwSyncModel *p_model,
    ReplicationSend *p_send,
    const store::RowAddress &p_at,
    int64_t p_tick
) {
    store::Table &table = store().table(p_at.table);
#if defined(NETW_TESTS)
    store::validation_audit::ledger().checks += 1;
#endif
    const SourceImage was = source_image(store(), table, p_at.row);
    const Node *node = Object::cast_to<Node>(gd::object_of(
        ObjectID(store_pull.residents.node_of(p_at.table, p_at.row))
    ));
    const bool unpulled = node != nullptr
        && node->get_multiplayer_authority() != table.authorities[p_at.row];
    const bool stays = store_source_row(p_model, p_send, p_at, p_tick);
    const char *drift = stays
        ? "quiet"
        : source_drift(was, source_image(store(), table, p_at.row));
    if (stays) {
        table.unsourced.set(p_at.row, true);
    }
    if (drift == nullptr || (unpulled && !stays)) {
        return;
    }
    store::Miss missed;
    missed.route = table.routes[p_at.row];
    missed.slot = table.slots[p_at.row];
    missed.tick = p_tick;
    missed.column = StringName(drift);
    missed.gate = "sources";
    store().misses.push_back(missed);
    store().miss_count += 1;
    store::report_miss(missed);
}

bool NetwMultiplayer::store_source_row(
    NetwSyncModel *p_model,
    ReplicationSend *p_send,
    const store::RowAddress &p_at,
    int64_t p_tick
) {
    store::Table &table = store().table(p_at.table);
    const uint32_t row = p_at.row;
    const uint64_t owner = table.owners[row];
    PropertyBinding *binding = binding_book.live(owner);
    if (binding == nullptr) {
        store_pull.set_source(p_at, store::Source::NONE);
        store_clear_row(p_at);
        table.set_offer(row, store::Table::OFFER_NONE);
        return false;
    }
    const bool primary
        = table.lanes[row] == 0 || !store().row_of(owner, 0).valid();
    Node *node = binding->node();
    if (node == nullptr) {
        sync_flush.skips_invalid_node += primary ? 1 : 0;
        store_pull.set_source(p_at, store::Source::NONE);
        store_clear_row(p_at);
        table.set_offer(row, store::Table::OFFER_NONE);
        return true;
    }
    const Ref<NetwEntity> entity = NetwEntity::of(node);
    if (entity.is_null()) {
        sync_flush.skips_no_entity += primary ? 1 : 0;
        store_pull.set_source(p_at, store::Source::NONE);
        store_clear_row(p_at);
        table.set_offer(row, store::Table::OFFER_NONE);
        return true;
    }
    if (entity_left_tree(entity->get_rid_handle(), entity->get_owner())) {
        store_pull.set_source(p_at, store::Source::NONE);
        table.set_offer(row, store::Table::OFFER_NONE);
        return true;
    }
    if (binding->route <= 0) {
        sync_pipeline_bind_declaration(int64_t(binding->id));
    }
    const int64_t route = binding->route;
    const repl::SetRow *declared = route > 0
        ? p_model->row_for(
              route,
              int64_t(NetwSyncModel::KIND_DERIVED),
              binding->order_key,
              binding->get_set()->record
          )
        : nullptr;
    if (declared == nullptr) {
        sync_flush.skips_no_route += primary ? 1 : 0;
        store_pull.set_source(p_at, store::Source::NONE);
        store_clear_row(p_at);
        table.set_offer(row, store::Table::OFFER_NONE);
        return true;
    }
    table.comps[row] = uint8_t(binding->comp);
    const int64_t epoch = liveness_route_epoch(route);
    if (table.routes[row] != route
        || store().epoch_of(table.slots[row]) != epoch) {
        store().place(p_at, route, epoch);
        store_bind(p_at);
    }
    const int64_t ordinal = declared->ordinal;
    store().name_lane(p_at, uint8_t(ordinal));
    const bool interacting
        = store_mark(table.slots[row], *entity->get_record(), p_tick);
    const int32_t held_authority = node->get_multiplayer_authority();
    table.authorities[row] = held_authority;
    const int64_t authority
        = node->is_inside_tree() ? int64_t(held_authority) : 0;
    const bool authors = p_model->authors(
        route,
        ordinal,
        int64_t(get_unique_id()),
        authority,
        entity->steering()
    );
    const store::Source source = store_source(table, row, binding, authors);
    store_pull.set_source(p_at, source);
    const bool external = binding->input_fed;
    const bool server_audience
        = declared->audience == repl::SET_AUDIENCE_SERVER_ONLY;
    store_admit_row(p_send, p_at, entity->get_rid_handle(), server_audience);
    const bool admitted = store_admits_any(p_at);
    uint8_t offer = store::Table::OFFER_NONE;
    if (primary) {
        offer = !authors ? store::Table::OFFER_UNAUTHORED
            : !admitted  ? store::Table::OFFER_UNREACHED
                         : store::Table::OFFER_MADE;
    }
    table.set_offer(row, offer);
    if (!authors || !admitted) {
        return interacting || external;
    }
    wire::StreamTenure &tenure = table.tenures[row];
    tenure.bound = repl::record_follows_tenure(declared->record);
    tenure.tenure = tenure.bound ? entity->get_control_tenure() : 0;
    const AnchorRevision installed = anchor_installed(route);
    tenure.anchor = installed.revision;
    tenure.anchor_author = installed.author;
    return interacting || external;
}

void NetwMultiplayer::store_stamp_thunk(
    void *p_context,
    uint32_t p_table,
    uint32_t p_row
) {
    static_cast<NetwMultiplayer *>(p_context)->store_stamp_row(p_table, p_row);
}

void NetwMultiplayer::store_stamp_row(uint32_t p_table, uint32_t p_row) {
    store::Table &table = store().table(p_table);
    PropertyBinding *binding = binding_book.live(table.owners[p_row]);
    const PropertySet *set = binding != nullptr ? binding->get_set() : nullptr;
    if (set == nullptr) {
        return;
    }
    const int64_t authored = binding->authored_tick;
    const int64_t tick = authored >= 0 ? authored : table.stamp_tick;
    follow_set(table, set);
    if (table.family == store::Family::RETAINED) {
        table.frame_ticks[p_row] = -1;
        table.reconcile_acks[p_row] = -1;
        return;
    }
    table.frame_ticks[p_row] = tick;
    table.reconcile_acks[p_row] = binding->reconcile_ack;
}

void NetwMultiplayer::store_resource_route(int64_t p_route) {
    ReplicationCore *plane = get_replication_plane();
    if (plane == nullptr || p_route <= 0) {
        return;
    }
    const LocalVector<PropertyBindingPtr> bindings
        = plane->derived_group(p_route);
    for (const PropertyBindingPtr &binding : bindings) {
        if (binding != nullptr) {
            store().unsource_owner(binding->id);
        }
    }
}

void NetwMultiplayer::store_work_route(int64_t p_route) {
    ReplicationCore *plane = get_replication_plane();
    if (plane == nullptr || p_route <= 0) {
        return;
    }
    const LocalVector<PropertyBindingPtr> bindings
        = plane->derived_group(p_route);
    for (const PropertyBindingPtr &binding : bindings) {
        if (binding != nullptr) {
            store().mark_working(store().row_of(binding->id, 0));
            store().mark_working(store().row_of(binding->id, 1));
        }
    }
}

void NetwMultiplayer::store_restate(uint64_t p_binding, int64_t p_peer) {
    ReplicationSend *send = row_send_of(get_replication_plane());
    if (send != nullptr && p_binding != 0) {
        send->restate(
            p_binding,
            p_peer,
            datagram_base_tick(),
            session_elapsed_ms()
        );
    }
}

void NetwMultiplayer::store_resource_entity(
    const NetwEntityRecord &p_record
) {
    const int64_t live = liveness_core.route_of(p_record.get_handle());
    const int64_t route = live > 0 ? live : p_record.get_route();
    store().stale_route(route);
    store_resource_route(route);
}

bool NetwMultiplayer::store_mark(
    uint32_t p_slot,
    const NetwEntityRecord &p_record,
    int64_t p_tick
) {
    const entity::Control *control = p_record.get_control();
    store().set_weight(p_slot, float(p_record.get_replication_priority()));
    const int64_t changed
        = store().note_tenure(p_slot, control->tenure, p_tick);
    const int64_t second = int64_t(clock_engine().get_tickrate());
    const bool interacting = control->claims_pending()
        || control->hold != 0
        || (changed >= 0 && p_tick - changed < (second > 0 ? second : 60));
    store().set_marks(
        p_slot,
        uint8_t(
            (p_record.get_peer_id() != 0 ? store::MARK_PLAYER : 0)
            | (interacting ? store::MARK_INTERACTING : 0)
        )
    );
    return interacting;
}

store::Source NetwMultiplayer::store_source(
    const store::Table &p_table,
    uint32_t p_row,
    const PropertyBinding *p_binding,
    bool p_authors
) const {
    if (p_table.lanes[p_row] == 0 && p_binding->volatile_external) {
        return store::Source::NONE;
    }
    return p_authors ? store::Source::LOCAL : store::Source::REMOTE;
}

bool NetwMultiplayer::store_admits_any(const store::RowAddress &p_at) const {
    const store::Table &table = store().table(p_at.table);
    for (uint32_t peer = 0; peer < table.peers.size(); ++peer) {
        if (store().peer_joined(peer)
            && table.peers[peer].admitted.get(p_at.row)) {
            return true;
        }
    }
    return false;
}

PackedInt32Array NetwMultiplayer::store_roster_ids() const {
    PackedInt32Array out;
    out.resize(int64_t(store_roster.size()));
    for (uint32_t at = 0; at < store_roster.size(); ++at) {
        out.set(int64_t(at), store_roster[at]);
    }
    return out;
}

void NetwMultiplayer::store_refresh() {
    NETW_STAGE(INTEREST, "Store interest", colors::INTEREST);
    const Ref<MultiplayerPeer> peer = inner.is_valid()
        ? inner->get_multiplayer_peer()
        : Ref<MultiplayerPeer>();
    const bool offline
        = peer.is_null() || peer->is_class("OfflineMultiplayerPeer");
    const int64_t local = int64_t(get_unique_id());
    const bool host = is_host();
    bool all = false;
    if (local != store_local || host != store_host
        || offline != store_offline) {
        store_local = local;
        store_host = host;
        store_offline = offline;
        all = true;
    }
    store_roster.clear();
    if (!offline) {
        const PackedInt32Array known = reachable_peer_ids();
        for (int64_t at = 0; at < known.size(); ++at) {
            if (int64_t(known[at]) != local) {
                store_roster.push_back(known[at]);
            }
        }
        if (local != SERVER_PEER) {
            all = store().peer_of(int(SERVER_PEER)) == store::Table::NO_SLOT
                || all;
            store().peer_join(int(SERVER_PEER));
        }
    }
    for (uint32_t at = 0; at < store_roster.size(); ++at) {
        const uint32_t index = store().peer_join(store_roster[at]);
        all = store().set_reachable(index, true) || all;
    }
    for (uint32_t index = 0; index < store().peer_capacity(); ++index) {
        if (!store().peer_reachable(index)) {
            continue;
        }
        const int id = store().peer_id(index);
        bool listed = false;
        for (uint32_t at = 0; at < store_roster.size() && !listed; ++at) {
            listed = store_roster[at] == id;
        }
        if (!listed) {
            all = store().set_reachable(index, false) || all;
        }
    }
    LocalVector<int32_t> routes;
    if (liveness_core.take_moved(routes)) {
        all = true;
    }
    for (uint32_t at = 0; at < routes.size(); ++at) {
        store().stale_route(routes[at]);
        store_resource_route(routes[at]);
    }
    store_note_interest();
    if (all) {
        store().stale_all();
        store().unsource_all();
    }
    store().take_stale(store_pending);
    for (uint32_t at = 0; at < store_pending.size(); ++at) {
        const uint32_t slot = store_pending[at];
        const int64_t route = store().route_of(slot);
        if (route <= 0) {
            store().settle_slot(slot);
            continue;
        }
        const Ref<NetwEntity> entity = wrapper_for_route(route);
        if (entity.is_null() && array_tables.holds_route(route)) {
            store_evaluate_nodeless(slot);
            continue;
        }
        if (entity.is_null()) {
            store().keep_stale(slot);
            continue;
        }
        store_evaluate_slot(slot, entity->get_rid_handle());
    }
}

void NetwMultiplayer::store_evaluate_nodeless(uint32_t p_slot) {
    store().set_live(p_slot, true);
    for (uint32_t peer = 0; peer < store().peer_capacity(); ++peer) {
        if (store().peer_joined(peer)) {
            store().see(p_slot, peer, true);
            store().hold(p_slot, peer, true);
        }
    }
    store().settle_slot(p_slot);
}

void NetwMultiplayer::store_note_interest() {
    LocalVector<int64_t> keys;
    if (interest_engine.take_moved(keys)) {
        store().stale_all();
    }
    for (uint32_t at = 0; at < keys.size(); ++at) {
        store_stale_key(keys[at]);
    }
}

void NetwMultiplayer::store_stale_key(int64_t p_key) {
    const Ref<NetwEntity> entity = wrapper_for_id(p_key);
    store_stale_entity(entity.is_valid() ? entity->get_rid_handle() : RID());
}

void NetwMultiplayer::store_stale_entity(const RID &p_entity) {
    const int64_t route = liveness_core.route_of(p_entity);
    if (route > 0) {
        store().stale_route(route);
    }
}

void NetwMultiplayer::store_evaluate_slot(
    uint32_t p_slot,
    const RID &p_entity
) {
    const bool live = p_entity.is_valid()
        && entity_get_state(p_entity) == ENTITY_STATE_LIVE;
    store().set_live(p_slot, live);
    const bool filtered
        = p_entity.is_valid() && interest_entity_has_filter(p_entity);
    spawn::Pipeline *spawns = spawn_plane();
    spawn::Book *book = spawns != nullptr ? spawns->get_spawn_book() : nullptr;
    const spawn::Record *record = book != nullptr
        ? book->spawned_of(store().route_of(p_slot))
        : nullptr;
    for (uint32_t peer = 0; peer < store().peer_capacity(); ++peer) {
        if (!store().peer_joined(peer)) {
            continue;
        }
        const int64_t id = int64_t(store().peer_id(peer));
        const bool visible = p_entity.is_valid()
            && (!filtered || interest_wire_admits(id, p_entity));
        store().see(p_slot, peer, visible);
        store().hold(
            p_slot,
            peer,
            record == nullptr || record->has_recipient(int(id))
        );
    }
    store().settle_slot(p_slot);
    store_resource_route(store().route_of(p_slot));
}

uint32_t NetwMultiplayer::store_slot_of(const RID &p_entity) const {
    const int64_t route = liveness_core.route_of(p_entity);
    if (route <= 0) {
        return store::Table::NO_SLOT;
    }
    const uint32_t slot = store().slot_of_route(route);
    if (slot == store::Table::NO_SLOT
        || store().epoch_of(slot) != liveness_route_epoch(route)) {
        return store::Table::NO_SLOT;
    }
    return slot;
}

void NetwMultiplayer::store_admit_row(
    ReplicationSend *p_send,
    const store::RowAddress &p_at,
    const RID &p_entity,
    bool p_server_audience
) {
    store::Table &table = store().table(p_at.table);
    const uint32_t slot = table.slots[p_at.row];
    if (slot == store::Table::NO_SLOT) {
        store_clear_row(p_at);
        return;
    }
    if (store().slot_stale(slot)) {
        store_evaluate_slot(slot, p_entity);
    }
    const uint8_t audience
        = p_server_audience ? uint8_t(store::Table::SERVER_AUDIENCE) : 0;
    const uint64_t stamp = store().slot_stamp(slot);
    uint8_t &flags = table.flags[p_at.row];
    if (table.admit_stamps[p_at.row] == stamp
        && (flags & store::Table::SERVER_AUDIENCE) == audience) {
        return;
    }
    table.admit_stamps[p_at.row] = stamp;
    flags = uint8_t((flags & ~store::Table::SERVER_AUDIENCE) | audience);
    const int64_t local = int64_t(get_unique_id());
    wire::StreamLane lane;
    lane.route = table.routes[p_at.row];
    lane.ordinal = table.ordinals[p_at.row];
    lane.family = wire::StreamFamily(uint8_t(table.family));
    for (uint32_t peer = 0; peer < store().peer_capacity(); ++peer) {
        if (!store().peer_joined(peer)) {
            continue;
        }
        const int64_t id = int64_t(store().peer_id(peer));
        const bool admitted = p_server_audience
            ? id == SERVER_PEER && local != SERVER_PEER
            : id != local && store().reaches(slot, peer)
                && store().holds_copy(slot, peer);
        const bool was = table.peers[peer].admitted.get(p_at.row);
        store().admit(p_at, peer, admitted);
        if (was && !admitted && p_send != nullptr) {
            p_send->hide(id, lane);
        }
    }
}

void NetwMultiplayer::store_clear_row(const store::RowAddress &p_at) {
    store::Table &table = store().table(p_at.table);
    table.admit_stamps[p_at.row] = 0;
    for (uint32_t peer = 0; peer < store().peer_capacity(); ++peer) {
        if (store().peer_joined(peer)) {
            store().admit(p_at, peer, false);
        }
    }
}

PackedInt32Array NetwMultiplayer::store_reach(uint32_t p_slot) const {
    PackedInt32Array out;
    for (uint32_t at = 0; at < store_roster.size(); ++at) {
        const uint32_t peer = store().peer_of(store_roster[at]);
        if (peer != store::Table::NO_SLOT
            && store().reaches(p_slot, peer)) {
            out.push_back(store_roster[at]);
        }
    }
    return out;
}

} // namespace netw
