#include "netw/sealed.hpp"

#include "netw/store/table.hpp"

#include <cstring>
#include <utility>

#include "netw/schema_core.hpp"

using namespace godot;

namespace netw::store {

uint32_t value_bytes_of(int p_type) {
    switch (p_type) {
        case SchemaCore::BOOL:
            return 1;
        case SchemaCore::F32:
        case SchemaCore::F64:
        case SchemaCore::I8:
        case SchemaCore::I16:
        case SchemaCore::I32:
        case SchemaCore::I64:
        case SchemaCore::U8:
        case SchemaCore::U16:
        case SchemaCore::ENTITY:
            return 8;
        case SchemaCore::VECTOR2:
            return sizeof(Vector2);
        case SchemaCore::VECTOR3:
            return sizeof(Vector3);
        case SchemaCore::VECTOR4:
            return sizeof(Vector4);
        case SchemaCore::COLOR:
            return sizeof(Color);
        case SchemaCore::QUATERNION:
            return sizeof(Quaternion);
        default:
            return 0;
    }
}

const char *family_name(Family p_family) {
    switch (p_family) {
        case Family::VOLATILE:
            return "volatile";
        case Family::RETAINED:
            return "retained";
        case Family::WINDOW:
            return "window";
    }
    return "volatile";
}

void Table::compile(const SchemaRecord &p_schema, Family p_family) {
    schema = p_schema;
    family = p_family;
    plan = wire::WirePlan::compile(schema);
    scalar = wire::is_scalar_schema(schema, plan);
    words = plan.valid() ? uint32_t((plan.row_bits() + 63) / 64 + 1) : 0;
    tail_columns = plan.has_variable() ? plan.column_count() : 0;
    scratch = wire::CodeRow::for_plan(plan);
    landing = wire::CodeRow::for_plan(plan);
    base = wire::CodeRow::for_plan(plan);
    columns.resize(uint32_t(schema.column_count()));
    values.resize(columns.size());
    value_offsets.resize(columns.size());
    decode_codes.resize(columns.size());
    row_bytes = 0;
    for (uint32_t at = 0; at < columns.size(); ++at) {
        const table::SchemaColumn *declared = schema.at(int(at));
        columns[at].value_bytes
            = declared->stride == 1 ? value_bytes_of(declared->type) : 0;
        value_offsets[at] = row_bytes;
        row_bytes += columns[at].value_bytes;
    }
    landed.resize(row_bytes);
    exact.resize(row_bytes);
    compile_coders();
}

void Table::compile_coders() {
    coders.resize(columns.size());
    code_offsets.resize(columns.size());
    uint32_t elements = 0;
    for (uint32_t at = 0; at < columns.size(); ++at) {
        coders[at] = plan.valid()
            ? coder_of(*schema.at(int(at)), plan.column(at))
            : ColumnCoder();
        code_offsets[at] = elements;
        elements += uint32_t(coders[at].slot.stride);
        if (plan.valid() && plan.column(at).variable) {
            columns[at].kind = Kind::SIZED;
        } else if (
            coders[at].kind != Coder::VARIANT && columns[at].value_bytes > 0
        ) {
            columns[at].kind = Kind::FIXED;
        } else {
            columns[at].kind = Kind::CODED;
        }
    }
    pulled_codes.resize(elements);
}

uint32_t Table::add_row() {
    const uint32_t row = slots.size();
    slots.push_back(NO_SLOT);
    routes.push_back(0);
    owners.push_back(0);
    authorities.push_back(0);
    comps.push_back(0);
    lanes.push_back(0);
    ordinals.push_back(0);
    sources.push_back(uint8_t(Source::NONE));
    flags.push_back(0);
    changed_ticks.push_back(-1);
    frame_ticks.push_back(-1);
    received_ticks.push_back(-1);
    reconcile_acks.push_back(-1);
    tenures.push_back(wire::StreamTenure());
    admit_stamps.push_back(0);
    unsourced.resize(slots.size());
    unsourced.set(row, true);
    working.resize(slots.size());
    working.set(row, true);
    offers.push_back(OFFER_NONE);
    offer_counts[OFFER_NONE] += 1;
    due.resize(slots.size());
    due_ticks.resize(slots.size() * PENDING_DEPTH);
    due_held.push_back(0);
    due_senders.push_back(0);
    due_tenures.push_back(0);
    for (uint32_t at = 0; at < columns.size(); ++at) {
        const uint32_t size = columns[at].value_bytes;
        LocalVector<uint8_t> &held = values[at];
        const uint32_t from = held.size();
        held.resize(from + size);
        for (uint32_t byte = from; byte < held.size(); ++byte) {
            held[byte] = 0;
        }
    }
    cell_tails.resize(cell_tails.size() + tail_columns);
    const uint32_t first = codes.size();
    codes.resize(first + words);
    for (uint32_t at = first; at < codes.size(); ++at) {
        codes[at] = 0;
    }
    history_ticks.resize(slots.size() * HISTORY_DEPTH);
    history_values.resize(slots.size() * HISTORY_DEPTH * row_bytes);
    history_tails.resize(slots.size() * HISTORY_DEPTH * tail_columns);
    history_next.push_back(0);
    history_held.push_back(0);
    history_appends.push_back(0);
    for (uint32_t at = 0; at < senders.size(); ++at) {
        senders[at].rows.resize(slots.size());
    }
    push_peer_rows();
    return row;
}

void Table::set_offer(uint32_t p_row, uint8_t p_offer) {
    offer_counts[offers[p_row]] -= 1;
    offers[p_row] = p_offer;
    offer_counts[p_offer] += 1;
}

uint32_t Table::swap_remove(uint32_t p_row) {
    const uint32_t last = slots.size() - 1;
    set_offer(p_row, OFFER_NONE);
    offer_counts[OFFER_NONE] -= 1;
    if (p_row != last) {
        offers[p_row] = offers[last];
        unsourced.set(p_row, unsourced.get(last));
        working.set(p_row, working.get(last));
        slots[p_row] = slots[last];
        routes[p_row] = routes[last];
        owners[p_row] = owners[last];
        authorities[p_row] = authorities[last];
        comps[p_row] = comps[last];
        lanes[p_row] = lanes[last];
        ordinals[p_row] = ordinals[last];
        sources[p_row] = sources[last];
        flags[p_row] = flags[last];
        changed_ticks[p_row] = changed_ticks[last];
        frame_ticks[p_row] = frame_ticks[last];
        received_ticks[p_row] = received_ticks[last];
        reconcile_acks[p_row] = reconcile_acks[last];
        tenures[p_row] = tenures[last];
        admit_stamps[p_row] = admit_stamps[last];
        due.set(p_row, due.get(last));
        for (uint32_t at = 0; at < PENDING_DEPTH; ++at) {
            due_ticks[p_row * PENDING_DEPTH + at]
                = due_ticks[last * PENDING_DEPTH + at];
        }
        due_held[p_row] = due_held[last];
        due_senders[p_row] = due_senders[last];
        due_tenures[p_row] = due_tenures[last];
        for (uint32_t at = 0; at < columns.size(); ++at) {
            const uint32_t size = columns[at].value_bytes;
            uint8_t *held = values[at].ptr();
            memcpy(held + p_row * size, held + last * size, size);
        }
        for (uint32_t at = 0; at < tail_columns; ++at) {
            cell_tails[p_row * tail_columns + at]
                = cell_tails[last * tail_columns + at];
        }
        for (uint32_t at = 0; at < words; ++at) {
            codes[p_row * words + at] = codes[last * words + at];
        }
        const uint32_t span = HISTORY_DEPTH * row_bytes;
        memcpy(
            history_values.ptr() + p_row * span,
            history_values.ptr() + last * span,
            span
        );
        for (uint32_t at = 0; at < HISTORY_DEPTH; ++at) {
            history_ticks[p_row * HISTORY_DEPTH + at]
                = history_ticks[last * HISTORY_DEPTH + at];
        }
        for (uint32_t at = 0; at < HISTORY_DEPTH * tail_columns; ++at) {
            history_tails[p_row * HISTORY_DEPTH * tail_columns + at]
                = history_tails[last * HISTORY_DEPTH * tail_columns + at];
        }
        history_next[p_row] = history_next[last];
        history_held[p_row] = history_held[last];
        history_appends[p_row] = history_appends[last];
        for (uint32_t at = 0; at < senders.size(); ++at) {
            senders[at].rows[p_row] = std::move(senders[at].rows[last]);
        }
        move_peer_rows(p_row, last);
    }
    shrink_peer_rows(last);
    slots.resize(last);
    routes.resize(last);
    owners.resize(last);
    authorities.resize(last);
    comps.resize(last);
    lanes.resize(last);
    ordinals.resize(last);
    sources.resize(last);
    flags.resize(last);
    changed_ticks.resize(last);
    frame_ticks.resize(last);
    received_ticks.resize(last);
    reconcile_acks.resize(last);
    tenures.resize(last);
    admit_stamps.resize(last);
    offers.resize(last);
    unsourced.resize(last);
    working.resize(last);
    due.resize(last);
    due_ticks.resize(last * PENDING_DEPTH);
    due_held.resize(last);
    due_senders.resize(last);
    due_tenures.resize(last);
    for (uint32_t at = 0; at < columns.size(); ++at) {
        values[at].resize(last * columns[at].value_bytes);
    }
    cell_tails.resize(last * tail_columns);
    codes.resize(last * words);
    history_ticks.resize(last * HISTORY_DEPTH);
    history_values.resize(last * HISTORY_DEPTH * row_bytes);
    history_tails.resize(last * HISTORY_DEPTH * tail_columns);
    history_next.resize(last);
    history_held.resize(last);
    history_appends.resize(last);
    for (uint32_t at = 0; at < senders.size(); ++at) {
        senders[at].rows.resize(last);
    }
    return p_row != last ? last : NO_SLOT;
}

bool Table::settle(
    uint32_t p_row,
    const wire::CodeRow &p_codes,
    int64_t p_tick
) {
    uint64_t *held = codes.ptr() + uint64_t(p_row) * words;
    const uint64_t *fresh = p_codes.word_data();
    bool changed = false;
    for (uint32_t at = 0; at < words; ++at) {
        if (held[at] != fresh[at]) {
            held[at] = fresh[at];
            changed = true;
        }
    }
    if (tail_columns != 0) {
        const wire::TailRef *given = p_codes.tail_data();
        for (uint32_t at = 0; at < tail_columns; ++at) {
            wire::TailRef &kept = cell_tails[p_row * tail_columns + at];
            if (kept != given[at]) {
                kept = given[at];
                changed = true;
            }
        }
    }
    flags[p_row]
        = (flags[p_row] & ~uint8_t(DIRTY | UNGATHERED | UNENCODED)) | PULLED;
    if (changed) {
        flags[p_row] |= DIRTY;
        changed_ticks[p_row] = p_tick;
    }
    return changed;
}

int Table::first_difference(
    uint32_t p_row,
    const wire::CodeRow &p_codes
) const {
    wire::CodeRow stored;
    stored.assign(plan, codes_of(p_row), tails_of(p_row));
    for (uint32_t index = 0; index < plan.column_count(); ++index) {
        const wire::ColumnPlan &slot = plan.column(index);
        if (slot.variable) {
            if (stored.tail(index) != p_codes.tail(index)) {
                return int(index);
            }
            continue;
        }
        for (int element = 0; element < slot.stride; ++element) {
            if (stored.read(slot, element) != p_codes.read(slot, element)) {
                return int(index);
            }
        }
    }
    return -1;
}

uint32_t Table::dirty_count() const {
    uint32_t count = 0;
    for (uint32_t row = 0; row < flags.size(); ++row) {
        count += (flags[row] & DIRTY) != 0 ? 1 : 0;
    }
    return count;
}

int64_t Table::byte_count() const {
    int64_t bytes = int64_t(slots.size())
        * int64_t(sizeof(uint32_t) + sizeof(int64_t) * 2 + sizeof(uint64_t) * 2
                  + 4);
    for (uint32_t at = 0; at < values.size(); ++at) {
        bytes += int64_t(values[at].size());
    }
    bytes += int64_t(cell_tails.size()) * int64_t(sizeof(wire::TailRef));
    bytes += int64_t(codes.size()) * int64_t(sizeof(uint64_t));
    bytes += int64_t(history_ticks.size()) * int64_t(sizeof(int64_t));
    bytes += int64_t(history_values.size());
    bytes += int64_t(history_tails.size()) * int64_t(sizeof(wire::TailRef));
    for (uint32_t at = 0; at < senders.size(); ++at) {
        for (const Received &held : senders[at].rows) {
            bytes += int64_t(held.revisions.size() + held.images.size())
                * int64_t(sizeof(uint64_t));
            bytes
                += int64_t(held.tails.size()) * int64_t(sizeof(wire::TailRef));
            bytes += int64_t(held.early_edge.size());
        }
    }
    return bytes;
}

#if defined(NETW_PROFILING) || defined(DEBUG_ENABLED)
int64_t Table::held_bytes() const {
    int64_t bytes = int64_t(sizeof(Table)) + held::vector(columns)
        + held::vector(slots) + held::vector(routes) + held::vector(owners)
        + held::vector(authorities) + held::vector(comps) + held::vector(lanes)
        + held::vector(ordinals) + held::vector(sources) + held::vector(flags)
        + held::vector(changed_ticks) + held::vector(frame_ticks)
        + held::vector(received_ticks) + held::vector(reconcile_acks)
        + held::vector(tenures) + held::vector(admit_stamps)
        + held::vectors(values) + held::vector(cell_tails) + held::vector(codes)
        + held::vector(senders) + held::vector(value_offsets)
        + held::vector(history_ticks) + held::vector(history_values)
        + held::vector(history_tails) + held::vector(history_next)
        + held::vector(history_held) + held::vector(history_appends)
        + held::vector(unsourced.words) + held::vector(working.words)
        + held::vector(touched_routes) + held::map(lagging)
        + held::vector(offers) + held::vector(due.words)
        + held::vector(due_ticks) + held::vector(due_held)
        + held::vector(due_senders) + held::vector(due_tenures)
        + held::vector(arrived_window) + held::vector(coders)
        + held::vector(exact) + held::vector(pulled_codes)
        + held::vector(code_offsets) + scratch.held_bytes()
        + landing.held_bytes() + base.held_bytes() + held::vector(landed)
        + held::vector(decode_codes);
    for (const Sender &sender : senders) {
        bytes += held::vector(sender.rows);
        for (const Received &received : sender.rows) {
            bytes += held::vector(received.revisions)
                + held::vector(received.images) + held::vector(received.tails)
                + int64_t(received.early_edge.size());
        }
    }
    for (const repl::WindowSample &sample : arrived_window) {
        bytes += sample.row.held_bytes();
    }
    return bytes;
}

int64_t Table::peer_bytes() const {
    int64_t bytes = held::vector(peers);
    for (const PeerRows &peer : peers) {
        bytes += held::vector(peer.admitted.words)
            + held::vector(peer.sendable.words) + held::vector(peer.owed.words)
            + held::vector(peer.queued.words) + held::vector(peer.due.words)
            + held::vector(peer.stream) + held::vector(peer.request)
            + held::vector(peer.token) + held::vector(peer.origin)
            + held::vector(peer.opened) + held::vector(peer.next_rev)
            + held::vector(peer.acked_rev) + held::vector(peer.exposed_rev)
            + held::vector(peer.rest_rev) + held::vector(peer.baseline)
            + held::vector(peer.baseline_tails) + held::vector(peer.acc)
            + held::vector(peer.last_sent) + held::vector(peer.refresh_at)
            + held::vector(peer.repair_at);
    }
    return bytes;
}
#endif

} // namespace netw::store
