#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "netw/colors.hpp"
#include "netw/object_port.hpp"
#include "netw/profile.hpp"
#include "netw/store/world.hpp"
#include "netw/wire/value_row.hpp"

using namespace godot;

namespace netw::store {

namespace {

uint64_t script_of(Object *p_node) {
    const Ref<Script> held = p_node->get_script();
    return held.is_valid() ? uint64_t(held->get_instance_id()) : 1;
}

bool resources_held(const Table &p_table, Object *p_node) {
    for (uint32_t at = 0; at < p_table.columns.size(); ++at) {
        if (p_table.columns[at].port == Port::INDEXED
            && !named_held(p_node, p_table.schema.at(int(at))->key)) {
            return false;
        }
    }
    return true;
}

uint32_t sweep_share(uint32_t p_count) {
    return (p_count + World::SWEEP_TICKS - 1) / World::SWEEP_TICKS;
}

bool in_sweep(uint32_t p_row, uint32_t p_from, uint32_t p_count) {
    return (p_row + p_count - p_from) % p_count < sweep_share(p_count);
}

} // namespace

const NodeCell &World::resident(Table &p_table, uint32_t p_row) {
    return residents.resolve(
        p_table.nodes[p_row],
        p_table.cells[p_row],
        validate_every > 0
    );
}

bool World::readable(Table &p_table, uint32_t p_row, Object *p_node) {
    if ((p_table.flags[p_row] & Table::READABLE) == 0) {
        for (int at = 0; at < p_table.schema.column_count(); ++at) {
            if (!gd::has_property(p_node, p_table.schema.at(at)->key)) {
                return false;
            }
        }
        p_table.flags[p_row] |= Table::READABLE;
    }
    return p_table.indexed_columns == 0 || resources_held(p_table, p_node);
}

bool World::read_row(
    Table &p_table,
    uint32_t p_row,
    Object *p_node,
    BodyState &p_body,
    bool p_body_columns,
    wire::CodeRow &r_codes,
    bool p_keep
) {
    if (p_body_columns && p_table.body_columns > 0) {
        body_read(p_body);
#if defined(NETW_TESTS)
        work.body += p_keep && p_body.root ? 1 : 0;
#endif
    }
    for (uint32_t at = 0; at < p_table.columns.size(); ++at) {
        const Column &column = p_table.columns[at];
        if (column.port == Port::BODY && !p_body_columns) {
            continue;
        }
        const StringName &key = p_table.schema.at(int(at))->key;
        const Variant value = column.port == Port::BODY
            ? body_value(p_body, column.body, p_node, key)
            : object_value(p_node, key);
#if defined(NETW_TESTS)
        if (p_keep) {
            work.engine += column.port == Port::ENGINE ? 1 : 0;
            work.script += column.port == Port::SCRIPT ? 1 : 0;
            work.coded += 1;
        }
#endif
        if (p_keep) {
            p_table.write_value(p_row, at, value);
        }
        if (!p_table.encode(at, value, r_codes)) {
            return false;
        }
    }
    return true;
}

bool World::read_exact(
    Table &p_table,
    Object *p_node,
    BodyState &p_body,
    bool p_body_columns
) {
    if (p_body_columns && p_table.body_columns > 0) {
        body_read(p_body);
    }
    int64_t engine = 0;
    int64_t script = 0;
    uint8_t *exact = p_table.exact.ptr();
    for (uint32_t at = 0; at < p_table.columns.size(); ++at) {
        const Column &column = p_table.columns[at];
        const ColumnCoder &coder = p_table.coders[at];
        uint8_t *into = exact + p_table.value_offsets[at];
        if (column.port == Port::BODY) {
            if (!p_body_columns) {
                continue;
            }
            if (body_typed(p_body, column.body, coder.type, into)) {
                continue;
            }
        }
        engine += column.port == Port::ENGINE ? 1 : 0;
        script += column.port == Port::SCRIPT ? 1 : 0;
        const StringName &key = p_table.schema.at(int(at))->key;
        const Variant value = column.port == Port::BODY
            ? body_value(p_body, column.body, p_node, key)
            : object_value(p_node, key);
        if (!typed_from_variant(coder, value, into)) {
            return false;
        }
    }
#if defined(NETW_TESTS)
    work.body += p_body_columns && p_table.body_columns > 0 && p_body.root;
    work.engine += engine;
    work.script += script;
#endif
    return true;
}

bool World::pull_exact(
    Table &p_table,
    uint32_t p_row,
    Object *p_node,
    BodyState &p_body,
    bool p_body_columns,
    int64_t p_tick
) {
    if (!p_table.typed
        || !read_exact(p_table, p_node, p_body, p_body_columns)) {
        return false;
    }
    const int settled = p_table.settle_exact(p_row, p_body_columns, p_tick);
    if (settled < 0) {
        return false;
    }
    if (settled > 0) {
#if defined(NETW_TESTS)
        work.changed += 1;
#endif
        p_table.record_rest(p_row, p_tick);
        p_table.record_held(p_row, p_tick);
        owe_changed(p_table, p_row);
    } else if (p_table.history_count(p_row) == 0) {
        p_table.record_held(p_row, p_tick);
    }
    return true;
}

void World::note_miss(
    const Table &p_table,
    uint32_t p_row,
    int p_column,
    const char *p_gate,
    int64_t p_tick
) {
    Miss missed;
    missed.route = p_table.routes[p_row];
    missed.slot = p_table.slots[p_row];
    missed.tick = p_tick;
    missed.column = p_table.schema.at(p_column)->key;
    missed.gate = p_gate;
    if (misses.size() < MISSES_KEPT) {
        misses.push_back(missed);
    }
    miss_count += 1;
    report_miss(missed);
}

void World::pull(uint32_t p_table, int64_t p_tick) {
    NETW_ZONE_NC("Store pull", colors::TABLE);
    Table &table = *tables[p_table];
    NETW_ZONE_TEXT_F("table %u %s", p_table, family_name(table.family));
    table.pulled_tick = p_tick;
    const bool validating = validate_every > 0 && p_tick % validate_every == 0;
#if defined(NETW_TESTS)
    work.pulls += 1;
    const int64_t allocated = allocations_seen();
#endif
    pull_rows(table, p_tick, validating);
#if defined(NETW_TESTS)
    work.allocations += allocations_seen() - allocated;
#endif
    if (NETW_PROFILE_CONNECTED() && table.body_columns > 0) {
        const bool whole_body = table.body_columns == table.columns.size();
        NETW_TALLY(
            profile::names::ROWS_PULLED,
            int64_t(whole_body ? visiting.size() : table.row_count())
        );
        NETW_TALLY(
            profile::names::AWAKE_ROWS,
            int64_t(table.working.population())
        );
        NETW_PLOT(
            profile::plot_name(
                profile::names::SWEEP_POSITION,
                p_table,
                profile::PlotFormat::NUMBER,
                colors::TABLE
            ),
            int64_t(table.sweep_at)
        );
    } else if (NETW_PROFILE_CONNECTED()) {
        NETW_TALLY(profile::names::ROWS_PULLED, int64_t(table.row_count()));
    }
}

bool World::read_live(const RowAddress &p_at, LocalVector<Variant> &r_values) {
    r_values.clear();
    if (!p_at.valid()) {
        return false;
    }
    Table &table = *tables[p_at.table];
    const NodeCell &cell = resident(table, p_at.row);
    Object *node = cell.object;
    if (node == nullptr || !readable(table, p_at.row, node)) {
        return false;
    }
    BodyState body;
    if (table.body_columns > 0) {
        body_probe(cell, body);
        body_read(body);
    }
    r_values.resize(table.columns.size());
    for (uint32_t at = 0; at < table.columns.size(); ++at) {
        const Column &column = table.columns[at];
        const StringName &key = table.schema.at(int(at))->key;
        r_values[at] = column.port == Port::BODY
            ? body_value(body, column.body, node, key)
            : object_value(node, key);
    }
    return true;
}

void World::pull_rows(Table &p_table, int64_t p_tick, bool p_validating) {
    Table &table = p_table;
    const bool whole_body
        = table.body_columns > 0 && table.body_columns == table.columns.size();
    const uint32_t count = table.row_count();
    const uint32_t from = sweep(table, whole_body);
    if (!whole_body) {
        for (uint32_t row = 0; row < count; ++row) {
            const bool swept = in_sweep(row, from, count);
            pull_row(table, row, p_tick, p_validating, false, swept);
        }
        return;
    }
    visiting.clear();
    for (uint32_t word = 0; word < table.working.words.size(); ++word) {
        uint64_t bits = table.working.words[word];
        while (bits != 0) {
            visiting.push_back(word * 64 + Bits::lowest(bits));
            bits &= bits - 1;
        }
    }
    for (uint32_t at = 0; at < visiting.size(); ++at) {
        const uint32_t row = visiting[at];
        const bool swept = in_sweep(row, from, count);
        pull_row(table, row, p_tick, p_validating, true, swept);
    }
    if (p_validating) {
        audit_quiet(table, p_tick);
    }
}

uint32_t World::sweep(Table &p_table, bool p_wake) {
    const uint32_t count = p_table.row_count();
    if (count == 0) {
        return 0;
    }
    if (p_table.sweep_at >= count) {
        p_table.sweep_at = 0;
    }
    const uint32_t from = p_table.sweep_at;
    const uint32_t share = sweep_share(count);
    for (uint32_t at = 0; at < share; ++at) {
        if (p_table.sweep_at >= count) {
            p_table.sweep_at = 0;
        }
        if (p_wake) {
            p_table.working.set(p_table.sweep_at, true);
        }
        p_table.sweep_at += 1;
    }
#if defined(NETW_TESTS)
    if (p_wake) {
        work.swept += int64_t(share);
    }
#endif
    return from;
}

void World::pull_row(
    Table &p_table,
    uint32_t p_row,
    int64_t p_tick,
    bool p_validating,
    bool p_gated,
    bool p_swept
) {
    Table &table = p_table;
    const uint32_t row = p_row;
    const bool validating = p_validating;
    NETW_ZONE_LEAF_F(
        "Store pull entity",
        colors::TABLE,
        profile::SUBSYSTEM_ENTITY,
        "entity %lld",
        static_cast<long long>(table.routes[row])
    );
#if defined(NETW_TESTS)
    work.visits += 1;
#endif
    uint8_t &flags = table.flags[row];
    const uint8_t source = table.sources[row];
    if (source == uint8_t(Source::NONE)) {
        flags &= ~uint8_t(Table::DIRTY | Table::PULLED);
        if (p_gated) {
            table.working.set(row, false);
        }
        return;
    }
    const NodeCell &cell = resident(table, row);
    Object *node = cell.object;
    if (node != nullptr) {
        follow_authority(table, row, node);
    }
    if (source != uint8_t(Source::LOCAL)) {
        flags &= ~uint8_t(Table::DIRTY | Table::PULLED);
        if (table.body_columns > 0) {
            note_rest(table, row, cell);
        }
        if (p_gated && (flags & Table::RESTED) != 0
            && watched.has(table.nodes[row])) {
            table.working.set(row, false);
        }
        return;
    }
    if (!table.scalar) {
        flags |= Table::UNENCODED;
        return;
    }
    if (node == nullptr || !readable(table, row, node)) {
        flags |= Table::UNGATHERED;
        return;
    }
    BodyState body;
    bool resting = false;
    if (table.body_columns > 0) {
        body_probe(cell, body);
        const bool slept
            = (flags & Table::RESTED) != 0 && (flags & Table::PULLED) != 0;
        follow_rest(table, row, body);
        resting = body.rests() && slept;
    }
    const bool fresh_body = p_swept || !resting;
    const bool whole_body
        = table.body_columns > 0 && table.body_columns == table.columns.size();
    if (resting && p_gated && watched.has(table.nodes[row])) {
        table.working.set(row, false);
    }
    if (!fresh_body) {
#if defined(NETW_TESTS)
        work.rested += 1;
#endif
        if (whole_body) {
            flags &= ~uint8_t(Table::DIRTY | Table::UNGATHERED);
            if (validating) {
                check_codes(table, row, node, body, p_tick, "rested");
            }
            return;
        }
    }
    if (table.body_columns < table.columns.size()) {
        const uint64_t script = script_of(node);
        if (script != table.scripts[row]) {
            table.scripts[row] = script;
            flags &= ~uint8_t(Table::READABLE);
            if (!readable(table, row, node)) {
                flags |= Table::UNGATHERED;
                return;
            }
        }
    }
#if defined(NETW_TESTS)
    work.rows += 1;
#endif
    if (pull_exact(table, row, node, body, fresh_body, p_tick)) {
        if (validating) {
            check_codes(
                table,
                row,
                node,
                body,
                p_tick,
                fresh_body ? "typed" : "rested"
            );
        }
        return;
    }
    if (fresh_body) {
        table.scratch.clear();
    } else {
        table.scratch
            .assign(table.plan, table.codes_of(row), table.carried_of(row));
    }
    wire::CodeRow &fresh = table.scratch;
    if (!read_row(table, row, node, body, fresh_body, fresh, true)) {
        flags |= Table::UNENCODED;
        return;
    }
    if (table.settle(row, table.scratch, p_tick)) {
#if defined(NETW_TESTS)
        work.changed += 1;
#endif
        table.record_rest(row, p_tick);
        table.record_codes(row, table.scratch, p_tick);
        owe_changed(table, row);
    } else if (table.history_count(row) == 0) {
        table.record_codes(row, table.scratch, p_tick);
    }
    if (!fresh_body && validating) {
        check_codes(table, row, node, body, p_tick, "rested");
    }
}

void World::audit_quiet(Table &p_table, int64_t p_tick) {
    Table &table = p_table;
    godot::HashMap<uint64_t, int64_t> still;
    uint32_t visited = 0;
    for (uint32_t row = 0; row < table.row_count(); ++row) {
        if (visited < visiting.size() && visiting[visited] == row) {
            visited += 1;
            continue;
        }
        if (table.sources[row] == uint8_t(Source::NONE)) {
            continue;
        }
        const uint64_t id = table.nodes[row];
        const NodeCell cell = Residents::derive(id);
        Object *node = cell.object;
        if (node == nullptr) {
            continue;
        }
#if defined(NETW_TESTS)
        validation_audit::ledger().checks += 1;
#endif
        BodyState body;
        body_probe(cell, body);
        const Node *held = static_cast<Node *>(node);
        const bool moved
            = held->get_multiplayer_authority() != table.authorities[row];
        const bool quiet = body.rests() && (table.flags[row] & Table::RESTED)
            && watched.has(id) && !moved;
        if (quiet) {
            if (table.sources[row] == uint8_t(Source::LOCAL)) {
                check_codes(table, row, node, body, p_tick, "rested");
            }
            continue;
        }
        const int64_t *since = table.lagging.getptr(id);
        const int64_t first = since != nullptr ? *since : p_tick;
        still.insert(id, first);
        if (p_tick - first > int64_t(SWEEP_TICKS)) {
            note_miss(table, row, 0, "swept", p_tick);
        }
    }
    table.lagging = still;
}

void World::check_codes(
    Table &p_table,
    uint32_t p_row,
    Object *p_node,
    BodyState &p_body,
    int64_t p_tick,
    const char *p_gate
) {
    if (!probe.valid_for(p_table.plan)) {
        probe = wire::CodeRow::for_plan(p_table.plan);
    }
    probe.clear();
#if defined(NETW_TESTS)
    work.validated += 1;
    validation_audit::ledger().checks += 1;
#endif
    if (!read_row(p_table, p_row, p_node, p_body, true, probe, false)) {
        return;
    }
    const int differs = p_table.first_difference(p_row, probe);
    if (differs >= 0) {
        note_miss(p_table, p_row, differs, p_gate, p_tick);
    }
}

void World::note_rest(Table &p_table, uint32_t p_row, const NodeCell &p_cell) {
    BodyState body;
    if (p_cell.object != nullptr) {
        body_probe(p_cell, body);
    }
    follow_rest(p_table, p_row, body);
}

void World::follow_authority(Table &p_table, uint32_t p_row, Object *p_node) {
    const Node *node = static_cast<Node *>(p_node);
    if (node->get_multiplayer_authority() != p_table.authorities[p_row]) {
        p_table.unsourced.set(p_row, true);
    }
}

void World::follow_rest(
    Table &p_table,
    uint32_t p_row,
    const BodyState &p_body
) {
    uint8_t &flags = p_table.flags[p_row];
    if (p_body.rests()) {
        flags |= Table::RESTED;
        return;
    }
    flags &= ~uint8_t(Table::RESTED);
    const uint64_t node = p_table.nodes[p_row];
    if (!dozing.is_empty() && dozing.has(node)) {
        dozing.erase(node);
        woken.push_back(node);
    }
}

} // namespace netw::store
