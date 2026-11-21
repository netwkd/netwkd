#include "netw/predict/engine.hpp"

#include "netw/api/interpolate.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "netw/property_set_builder.hpp"
#include "netw/repl/sync_pipeline.hpp"
#include "netw/script/model.hpp"
#include "netw/wire/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/object.hpp"
#include "godot/os.hpp"
#include "godot/spatial_node.hpp"
#include "netw/colliders.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/object_port.hpp"
#include "netw/predict/axes.hpp"
#include "netw/predict/decl.hpp"
#include "netw/predict/frames.hpp"
#include "netw/predict/tick_work.hpp"
#include "netw/profile.hpp"
#include "netw/project.hpp"
#include "netw/stage.hpp"
#include "netw/store/port.hpp"
#include "src/predict/engine_support.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertyColumn;
using repl::PropertySet;

int64_t NetwPredictionEngine::tape_epoch_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->tape_epoch;
}

void NetwPredictionEngine::set_tape_epoch(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        if (row->tape_epoch != p_value) {
            row->written_through = -1;
        }
        row->tape_epoch = p_value;
    }
}

int64_t NetwPredictionEngine::command_epoch_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.command_epoch;
}

void NetwPredictionEngine::set_command_epoch(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.command_epoch = p_value;
    }
}

int64_t NetwPredictionEngine::relayed_epoch_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.relayed_epoch;
}

void NetwPredictionEngine::set_relayed_epoch(int64_t p_slot, int64_t p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.relayed_epoch = p_value;
    }
}

TypedArray<Dictionary> NetwPredictionEngine::tape_transitions(
    int64_t p_slot
) const {
    TypedArray<Dictionary> out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const StringName index_key("index");
    const StringName label_key("label");
    const StringName fresh_key("fresh");
    if (row->config.role == int(Role::PREDICT)) {
        const PackedInt64Array span = tape_span(p_slot);
        for (int64_t at = span[0]; at <= span[1]; ++at) {
            Dictionary entry;
            entry[index_key] = at;
            entry[label_key] = tape_label_of(p_slot, at);
            entry[fresh_key] = tape_is_fresh(p_slot, at);
            out.push_back(entry);
        }
        return out;
    }
    const PackedInt64Array held = command_transitions(p_slot);
    for (int at = 0; at < held.size(); ++at) {
        Dictionary entry;
        entry[index_key] = held[at];
        entry[label_key] = command_label_of(p_slot, held[at]);
        entry[fresh_key] = command_is_fresh(p_slot, held[at]);
        out.push_back(entry);
    }
    return out;
}

Dictionary NetwPredictionEngine::pending_provenance(int64_t p_slot) const {
    Dictionary out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || !row->pending_provenance.held) {
        return out;
    }
    const predict::PendingProvenance &pending = row->pending_provenance;
    out[StringName("episode")] = pending.episode;
    out[StringName("write_id")] = pending.write_id;
    out[StringName("operator")] = pending.op;
    out[StringName("basis")] = pending.basis;
    return out;
}

void NetwPredictionEngine::set_pending_provenance(
    int64_t p_slot,
    const Dictionary &p_provenance
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    predict::PendingProvenance &pending = row->pending_provenance;
    pending = predict::PendingProvenance();
    if (p_provenance.is_empty()) {
        return;
    }
    pending.episode = int64_t(p_provenance.get(StringName("episode"), 0));
    pending.write_id = int64_t(p_provenance.get(StringName("write_id"), 0));
    pending.op = int64_t(p_provenance.get(StringName("operator"), 0));
    pending.basis = int64_t(p_provenance.get(StringName("basis"), -1));
    pending.held = true;
}

void NetwPredictionEngine::stamp_pending_provenance(
    int64_t p_slot,
    int64_t p_transition
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const predict::PendingProvenance staged = row->pending_provenance;
    const int write_id = staged.held ? int(staged.write_id) : 0;
    if (write_id <= 0) {
        return;
    }
    mark_provenance(
        p_slot,
        p_transition,
        int(staged.episode),
        write_id,
        int(staged.op),
        staged.basis
    );
}

void NetwPredictionEngine::mark_domain(
    int64_t p_slot,
    int64_t p_transition,
    int p_domain
) {
    NETW_BUG_COND(
        p_domain < int(predict::Domain::IN_DOMAIN)
            || p_domain > int(predict::Domain::OUT_OF_DOMAIN)
    );
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->journal.mark_domain(p_transition, predict::Domain(p_domain));
    }
}

void NetwPredictionEngine::mark_chain_broken(
    int64_t p_slot,
    int64_t p_transition
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->journal.mark_chain_broken(p_transition);
}

void NetwPredictionEngine::mark_provenance(
    int64_t p_slot,
    int64_t p_transition,
    int p_episode_id,
    int p_write_id,
    int p_operator,
    int64_t p_basis
) {
    NETW_BUG_COND(
        p_operator < int(predict::Operator::NONE)
            || p_operator > int(predict::Operator::JOINT_REBASE)
    );
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    const int index = row->journal.index_of(p_transition);
    const bool cleared_chain_break = index >= 0
        && (row->journal.flags_at(index) & predict::ROW_CHAIN_BROKEN) != 0;
    row->journal.mark_provenance(
        p_transition,
        p_episode_id,
        p_write_id,
        predict::Operator(p_operator),
        p_basis
    );
    if (cleared_chain_break) {
        row->stats.chain_breaks -= 1;
    }
}

void NetwPredictionEngine::mark_attribution(
    int64_t p_slot,
    int64_t p_transition,
    int p_attribution
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->journal.mark_attribution(
        p_transition,
        predict::Attribution(p_attribution)
    );
}

void NetwPredictionEngine::mark_differing_family(
    int64_t p_slot,
    int64_t p_transition,
    int p_family
) {
    NETW_BUG_COND(
        p_family < int(predict::DifferingFamily::NONE)
            || p_family > int(predict::DifferingFamily::CONTROLLER_LATCH)
    );
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->journal.mark_differing_family(
        p_transition,
        predict::DifferingFamily(p_family)
    );
}

void NetwPredictionEngine::mark_solve(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_topo_fp,
    int64_t p_witness_fp,
    int p_evidence_mask,
    int p_witness_class_bits
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->journal.mark_solve(
        p_transition,
        int32_t(p_topo_fp),
        int32_t(p_witness_fp),
        uint8_t(p_evidence_mask),
        uint8_t(p_witness_class_bits)
    );
}

PackedInt32Array NetwPredictionEngine::journal_pre_families_at(
    int64_t p_slot,
    int p_index
) const {
    PackedInt32Array out = gd::zeroed<PackedInt32Array>(3);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const predict::FamilyFingerprints families
        = row->journal.pre_families_at(p_index);
    int32_t *values = out.ptrw();
    values[0] = families.pose;
    values[1] = families.momentum;
    values[2] = families.controller;
    return out;
}

PackedInt32Array NetwPredictionEngine::journal_post_families_at(
    int64_t p_slot,
    int p_index
) const {
    PackedInt32Array out = gd::zeroed<PackedInt32Array>(3);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const predict::FamilyFingerprints families
        = row->journal.post_families_at(p_index);
    int32_t *values = out.ptrw();
    values[0] = families.pose;
    values[1] = families.momentum;
    values[2] = families.controller;
    return out;
}

int NetwPredictionEngine::journal_size(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.size() : 0;
}

int NetwPredictionEngine::transition_span(
    int64_t p_slot,
    int64_t p_basis
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->transition_span(p_basis) : 0;
}

void NetwPredictionEngine::journal_snapshot(
    int64_t p_slot,
    predict::JournalSnapshot &r_snapshot
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        r_snapshot.adopt(predict::Journal(), Dictionary());
        return;
    }
    r_snapshot.adopt(row->journal, row->witness_details.rows());
}

void NetwPredictionEngine::attach_journal_readers(bool p_attached) {
    journal_readers = p_attached;
    for (predict::Slot *row : rows) {
        if (row != nullptr) {
            row->journal.arm_diagnostics(p_attached);
        }
    }
}

void NetwPredictionEngine::attach_journal_reader(
    int64_t p_slot,
    bool p_attached
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->journal.arm_diagnostics(p_attached || journal_readers);
    }
}

bool NetwPredictionEngine::journal_diagnostics_armed(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->journal.diagnostics_armed();
}

uint32_t NetwPredictionEngine::book_depth(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->book.capacity() : 0;
}

int NetwPredictionEngine::journal_capacity(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.capacity_limit() : 0;
}

void NetwPredictionEngine::record_predict(
    int64_t p_slot,
    int64_t p_kind,
    int64_t p_a,
    int64_t p_b
) {
    NetwMultiplayer *host = session;
    if (host == nullptr || !host->event_records(p_kind)) {
        return;
    }
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (seated == nullptr) {
        return;
    }
    host->event_record(
        p_kind,
        seated->get_handle(),
        liveness_core->route_of(seated->get_handle()),
        0,
        OK,
        p_a,
        p_b
    );
}

int NetwPredictionEngine::episode_change(int64_t p_event) {
    if (p_event == EventPlane::EPISODE_OPEN) {
        return NetwPrediction::EPISODE_CHANGE_OPENED;
    }
    if (p_event == EventPlane::EPISODE_CLOSE) {
        return NetwPrediction::EPISODE_CHANGE_CLOSED;
    }
    if (p_event == EventPlane::EPISODE_FALLBACK) {
        return NetwPrediction::EPISODE_CHANGE_FALLBACK;
    }
    return -1;
}

void NetwPredictionEngine::hold_deferred_operator(
    int64_t p_slot,
    int64_t p_basis,
    int64_t p_recv_tick,
    const predict::StateRow &p_payload
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->deferred_operator.hold(p_basis, p_recv_tick, p_payload);
    }
}

int64_t NetwPredictionEngine::deferred_operator_basis(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->deferred_operator.basis : -1;
}

int64_t NetwPredictionEngine::deferred_operator_recv_tick(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->deferred_operator.recv_tick : -1;
}

bool NetwPredictionEngine::release_deferred_operator(
    int64_t p_slot,
    predict::StateRow &r_payload
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !row->deferred_operator.held()) {
        r_payload.resize(0);
        return false;
    }
    r_payload = row->deferred_operator.payload;
    row->deferred_operator.release();
    return true;
}

int NetwPredictionEngine::defer_operator_for_witness(
    int64_t p_slot,
    int64_t p_recv_tick,
    int64_t p_basis,
    const predict::StateRow &p_payload,
    bool p_has_corridor,
    bool p_observing
) {
    const int verdict
        = defer_verdict(p_slot, p_basis, p_has_corridor, p_observing);
    if (verdict == DEFER_HELD) {
        hold_deferred_operator(p_slot, p_basis, p_recv_tick, p_payload);
    }
    return verdict;
}

int NetwPredictionEngine::defer_verdict(
    int64_t p_slot,
    int64_t p_basis,
    bool p_has_corridor,
    bool p_observing
) {
    const PackedInt64Array stats = episode_stats(p_slot);
    const int64_t judged = journal_has(p_slot, p_basis) ? p_basis : -1;
    if (stats[STAT_EPISODE_ACTIVE] == 0 || witness_judged(p_slot, judged)) {
        return DEFER_REFUSED;
    }
    const bool transport_waits
        = p_has_corridor && stats[STAT_EPISODE_TRANSPORT_DECIDED] == 0;
    const bool dissipate_waits = dissipate_declared(p_slot)
        && stats[STAT_EPISODE_DISSIPATE_DECIDED] == 0 && !p_observing;
    if (!transport_waits && !dissipate_waits) {
        return DEFER_REFUSED;
    }
    const int64_t held = deferred_operator_basis(p_slot);
    if (held >= 0 && held != p_basis) {
        Dictionary unaligned;
        unaligned[StringName("witness_aligned")] = false;
        if (transport_waits && stats[STAT_EPISODE_TRANSPORT_DECIDED] == 0) {
            record_episode_decision(
                p_slot,
                int(predict::Operator::TRANSPORT_DELTA),
                held,
                false,
                false,
                unaligned
            );
        }
        if (dissipate_waits && stats[STAT_EPISODE_DISSIPATE_DECIDED] == 0) {
            record_episode_decision(
                p_slot,
                int(predict::Operator::DISSIPATE),
                held,
                false,
                false,
                unaligned
            );
        }
        drop_deferred_operator(p_slot);
        return DEFER_DISPLACED;
    }
    return DEFER_HELD;
}

void NetwPredictionEngine::drop_deferred_operator(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->deferred_operator.release();
    }
}

int64_t NetwPredictionEngine::journal_epoch(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.epoch() : -1;
}

PackedInt64Array NetwPredictionEngine::journal_transitions(
    int64_t p_slot
) const {
    PackedInt64Array out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    out.resize(row->journal.size());
    int64_t *values = out.ptrw();
    for (int at = 0; at < out.size(); ++at) {
        values[at] = row->journal.transition_at(at);
    }
    return out;
}

predict::JournalRow NetwPredictionEngine::journal_row(
    int64_t p_slot,
    int64_t p_transition
) const {
    predict::JournalRow out;
    const predict::Slot *slot = row_of(p_slot);
    if (slot == nullptr) {
        return out;
    }
    const int at = slot->journal.index_of(p_transition);
    if (at < 0) {
        return out;
    }
    out.present = true;
    out.transition = slot->journal.transition_at(at);
    out.label = slot->journal.label_at(at);
    out.kind = slot->journal.kind_at(at);
    out.c_hash = slot->journal.c_hash_at(at);
    out.e_digest = slot->journal.e_digest_at(at);
    out.pre_fp = slot->journal.pre_fp_at(at);
    out.topo_fp = slot->journal.topo_fp_at(at);
    out.raw_fp = slot->journal.raw_fp_at(at);
    out.witness_fp = slot->journal.witness_fp_at(at);
    out.witness_class_bits = slot->journal.witness_class_bits_at(at);
    out.aligned_error = slot->journal.aligned_error_at(at);
    out.evidence_mask = slot->journal.evidence_mask_at(at);
    out.pre_families = slot->journal.pre_families_at(at);
    out.post_fp = slot->journal.post_fp_at(at);
    out.post_families = slot->journal.post_families_at(at);
    out.episode_id = slot->journal.episode_id_at(at);
    out.write_id = slot->journal.write_id_at(at);
    out.op = slot->journal.operator_at(at);
    out.basis = slot->journal.basis_at(at);
    out.differing_family = slot->journal.differing_family_at(at);
    out.domain = slot->journal.domain_at(at);
    out.attribution = slot->journal.attribution_at(at);
    out.flags = slot->journal.flags_at(at);
    return out;
}

int NetwPredictionEngine::journal_slot_of(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.index_of(p_transition) : -1;
}

int64_t NetwPredictionEngine::journal_last_closed(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.last_closed() : -1;
}

int64_t NetwPredictionEngine::journal_first_unmatched(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.first_unmatched() : -1;
}

int64_t NetwPredictionEngine::journal_first_chain_break(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->journal.first_chain_break() : -1;
}

void NetwPredictionEngine::journal_clear(int64_t p_slot, int64_t p_epoch) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->journal.clear(p_epoch);
}

void NetwPredictionEngine::tape_reset(int64_t p_slot, int64_t p_epoch) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->reset_tape(p_epoch);
}

int NetwPredictionEngine::tape_size(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->tape.size() : 0;
}

PackedInt64Array NetwPredictionEngine::tape_span(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    PackedInt64Array span;
    span.resize(2);
    int64_t *values = span.ptrw();
    values[0] = row != nullptr ? row->tape.oldest_index() : -1;
    values[1] = row != nullptr ? row->tape.newest_index() : -1;
    return span;
}

int64_t NetwPredictionEngine::tape_label_of(
    int64_t p_slot,
    int64_t p_index
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->tape.label_of(p_index) : -1;
}

bool NetwPredictionEngine::tape_is_fresh(
    int64_t p_slot,
    int64_t p_index
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->tape.is_fresh(p_index);
}

const predict::TransitionBook *NetwPredictionEngine::predicted_book(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->book : nullptr;
}

const predict::TransitionBook *NetwPredictionEngine::input_book(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->inputs : nullptr;
}

const predict::Wiring *NetwPredictionEngine::wiring_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->wiring : nullptr;
}

const predict::FieldCodec *NetwPredictionEngine::input_codec_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->input_codec : nullptr;
}

void NetwPredictionEngine::set_declared_epoch(int64_t p_slot, int64_t p_epoch) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->axes.epoch = p_epoch;
    }
}

int64_t NetwPredictionEngine::declared_epoch_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->axes.epoch;
}

bool NetwPredictionEngine::journal_has(
    int64_t p_slot,
    int64_t p_transition
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->journal.slot_of(p_transition) >= 0;
}

} // namespace netw
