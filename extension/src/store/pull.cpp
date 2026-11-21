#include "netw/store/pull.hpp"

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/script.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/held_bytes.hpp"
#include "netw/object_port.hpp"
#include "netw/profile.hpp"
#include "netw/store/array.hpp"
#include "netw/store/table_value.hpp"
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

bool in_sweep(uint32_t p_row, uint32_t p_from, uint32_t p_count) {
    return (p_row + p_count - p_from) % p_count < Store::sweep_share(p_count);
}

} // namespace

const NodeCell &Pull::resident(Table &p_table, uint32_t p_row) {
    ResidentRows &rows = residents.rows(p_table.index);
    return residents.resolve(
        rows.nodes[p_row],
        rows.cells[p_row],
        store->validate_every > 0
    );
}

bool Pull::readable(Table &p_table, uint32_t p_row, Object *p_node) {
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

bool Pull::read_columns(
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
        store->work.body += p_keep && p_body.root ? 1 : 0;
#endif
    }
    int64_t engine = 0;
    int64_t script = 0;
    int64_t coded = 0;
    for (uint32_t at = 0; at < p_table.columns.size(); ++at) {
        const Column &column = p_table.columns[at];
        if (column.port == Port::BODY && !p_body_columns) {
            continue;
        }
        const StringName &key = p_table.schema.at(int(at))->key;
        if (column.kind != Kind::FIXED) {
            const Variant value = column.port == Port::BODY
                ? body_value(p_body, column.body, p_node, key)
                : object_value(p_node, key);
            engine += column.port == Port::ENGINE ? 1 : 0;
            script += column.port == Port::SCRIPT ? 1 : 0;
            coded += 1;
            if (p_keep) {
                write_value(p_table, p_row, at, value);
            }
            if (!encode(p_table, at, value, r_codes, store->session)) {
                return false;
            }
            continue;
        }
        const ColumnCoder &coder = p_table.coders[at];
        uint8_t *into = p_keep
            ? p_table.values[at].ptr() + uint64_t(p_row) * column.value_bytes
            : p_table.exact.ptr() + p_table.value_offsets[at];
        if (column.port != Port::BODY
            || !body_typed(p_body, column.body, coder.type, into)) {
            engine += column.port == Port::ENGINE ? 1 : 0;
            script += column.port == Port::SCRIPT ? 1 : 0;
            const Variant value = column.port == Port::BODY
                ? body_value(p_body, column.body, p_node, key)
                : object_value(p_node, key);
            if (!typed_from_variant(coder, value, into)) {
                fault::raise(
                    nullptr,
                    NETW_FAULT_UNSENDABLE_VALUE,
                    {vformat("Column %d", int(at)),
                     String("a value of the wrong type"),
                     String("the type its schema declares")}
                );
                return false;
            }
        }
        uint64_t *element_codes
            = p_table.pulled_codes.ptr() + p_table.code_offsets[at];
        if (!code_value(coder, into, element_codes)) {
            return false;
        }
        for (int element = 0; element < coder.slot.stride; ++element) {
            if (!r_codes.write(coder.slot, element, element_codes[element])) {
                return false;
            }
        }
    }
#if defined(NETW_TESTS)
    if (p_keep) {
        store->work.engine += engine;
        store->work.script += script;
        store->work.coded += coded;
    }
#else
    (void)engine;
    (void)script;
    (void)coded;
#endif
    return true;
}

void Pull::pull(uint32_t p_table, int64_t p_tick) {
    NETW_ZONE_NC("Store pull", colors::TABLE);
    Table &table = store->table(p_table);
    NETW_ZONE_TEXT_F("table %u %s", p_table, family_name(table.family));
    table.pulled_tick = p_tick;
    const int64_t every = store->validate_every;
    const bool validating = every > 0 && p_tick % every == 0;
#if defined(NETW_TESTS)
    store->work.pulls += 1;
    const int64_t allocated = allocations_seen();
#endif
    pull_rows(table, p_tick, validating);
#if defined(NETW_TESTS)
    store->work.allocations += allocations_seen() - allocated;
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

bool Pull::read_live(const RowAddress &p_at, LocalVector<Variant> &r_values) {
    r_values.clear();
    if (!p_at.valid()) {
        return false;
    }
    Table &table = store->table(p_at.table);
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

void Pull::pull_rows(Table &p_table, int64_t p_tick, bool p_validating) {
    Table &table = p_table;
    const bool whole_body
        = table.body_columns > 0 && table.body_columns == table.columns.size();
    const uint32_t count = table.row_count();
    const uint32_t from = store->sweep(table, whole_body);
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

void Pull::pull_row(
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
    store->work.visits += 1;
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
            && store->watched.has(residents.node_of(table.index, row))) {
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
    if (resting && p_gated
        && store->watched.has(residents.node_of(table.index, row))) {
        table.working.set(row, false);
    }
    if (!fresh_body) {
#if defined(NETW_TESTS)
        store->work.rested += 1;
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
        uint64_t &known = residents.rows(table.index).scripts[row];
        if (script != known) {
            known = script;
            flags &= ~uint8_t(Table::READABLE);
            if (!readable(table, row, node)) {
                flags |= Table::UNGATHERED;
                return;
            }
        }
    }
#if defined(NETW_TESTS)
    store->work.rows += 1;
#endif
    if (fresh_body) {
        table.scratch.clear();
    } else {
        table.scratch
            .assign(table.plan, table.codes_of(row), table.tails_of(row));
    }
    wire::CodeRow &fresh = table.scratch;
    if (!read_columns(table, row, node, body, fresh_body, fresh, true)) {
        flags |= Table::UNENCODED;
        return;
    }
    if (table.settle(row, table.scratch, p_tick)) {
#if defined(NETW_TESTS)
        store->work.changed += 1;
#endif
        table.record_rest(row, p_tick);
        table.record_codes(row, table.scratch, p_tick);
        store->owe_changed(table, row);
    } else if (table.history_count(row) == 0) {
        table.record_codes(row, table.scratch, p_tick);
    }
    if (!fresh_body && validating) {
        check_codes(table, row, node, body, p_tick, "rested");
    }
}

void Pull::audit_quiet(Table &p_table, int64_t p_tick) {
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
        const uint64_t id = residents.node_of(table.index, row);
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
            && store->watched.has(id) && !moved;
        if (quiet) {
            if (table.sources[row] == uint8_t(Source::LOCAL)) {
                check_codes(table, row, node, body, p_tick, "rested");
            }
            continue;
        }
        const int64_t *since = table.lagging.getptr(id);
        const int64_t first = since != nullptr ? *since : p_tick;
        still.insert(id, first);
        if (p_tick - first > int64_t(Store::SWEEP_TICKS)) {
            store->note_miss(table, row, 0, "swept", p_tick);
        }
    }
    table.lagging = still;
}

void Pull::check_codes(
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
    store->work.validated += 1;
    validation_audit::ledger().checks += 1;
#endif
    if (!read_columns(p_table, p_row, p_node, p_body, true, probe, false)) {
        return;
    }
    const int differs = p_table.first_difference(p_row, probe);
    if (differs >= 0) {
        store->note_miss(p_table, p_row, differs, p_gate, p_tick);
    }
}

void Pull::note_rest(Table &p_table, uint32_t p_row, const NodeCell &p_cell) {
    BodyState body;
    if (p_cell.object != nullptr) {
        body_probe(p_cell, body);
    }
    follow_rest(p_table, p_row, body);
}

void Pull::follow_authority(Table &p_table, uint32_t p_row, Object *p_node) {
    const Node *node = static_cast<Node *>(p_node);
    if (node->get_multiplayer_authority() != p_table.authorities[p_row]) {
        p_table.unsourced.set(p_row, true);
    }
}

void Pull::follow_rest(
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
    const uint64_t node = residents.node_of(p_table.index, p_row);
    if (!store->dozing.is_empty() && store->dozing.has(node)) {
        store->dozing.erase(node);
        store->woken.push_back(node);
    }
}

RowAddress Pull::add_row(
    uint32_t p_table,
    uint64_t p_owner,
    uint32_t p_lane,
    uint64_t p_node,
    uint8_t p_comp
) {
    const RowAddress at = store->add_row(p_table, p_owner, p_lane, p_comp);
    residents.add_row(p_table, p_node);
    return at;
}

void Pull::remove_row(const RowAddress &p_at) {
    if (!p_at.valid()) {
        return;
    }
    const Table &held = store->table(p_at.table);
    if (held.sources[p_at.row] != uint8_t(Source::NONE)) {
        count_probe(p_at, false);
    }
    store->remove_row(p_at);
    residents.swap_remove(p_at.table, p_at.row);
}

void Pull::remove_owner(uint64_t p_owner) {
    for (uint32_t lane = 0; lane < 2; ++lane) {
        remove_row(store->row_of(p_owner, lane));
    }
}

void Pull::set_source(const RowAddress &p_at, Source p_source) {
    const uint8_t was = store->table(p_at.table).sources[p_at.row];
    store->set_source(p_at, p_source);
    const bool reads = p_source != Source::NONE;
    if (reads != (was != uint8_t(Source::NONE))) {
        count_probe(p_at, reads);
    }
}

void Pull::count_probe(const RowAddress &p_at, bool p_reads) {
    const Table &held = store->table(p_at.table);
    if (held.body_columns == 0 || !held.scalar) {
        return;
    }
    const uint64_t node = residents.node_of(p_at.table, p_at.row);
    uint32_t *probed = body_rows.getptr(node);
    if (p_reads) {
        if (probed != nullptr) {
            *probed += 1;
            return;
        }
        body_rows.insert(node, 1);
        reprobed.push_back(node);
        return;
    }
    if (probed == nullptr) {
        return;
    }
    if (*probed > 1) {
        *probed -= 1;
        return;
    }
    body_rows.erase(node);
    reprobed.push_back(node);
}

void Pull::clear() {
    store->arrays->forget_slots();
    store->clear();
    body_rows.clear();
    reprobed.clear();
    residents.clear();
    residents.drop_rows();
}

#if defined(NETW_PROFILING) || defined(DEBUG_ENABLED)
int64_t Pull::held_bytes() const {
    return held::map(body_rows) + probe.held_bytes() + held::vector(visiting)
        + residents.held_bytes() + held::vector(reprobed);
}
#endif

} // namespace netw::store
