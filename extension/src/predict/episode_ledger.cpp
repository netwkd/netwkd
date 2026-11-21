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

namespace {

NetwPredictStats::Book *facts_in(predict::Decl *p_decl) {
    return p_decl != nullptr ? &p_decl->facts : nullptr;
}

bool broadcast_set_of(Node *p_owner, PropertySet &r_set) {
    Node *owner = p_owner;
    return owner != nullptr
        && property_set_builder::from_property_configs(
               r_set,
               netw::script::model::get_declared_property_configs(owner),
               PropertySet::RECORD_BROADCAST
        );
}

int64_t counted(const Dictionary &p_record, const char *p_key) {
    const Array held = p_record.get(StringName(p_key), Array());
    return int64_t(held.size());
}

Dictionary last_attempt(const Dictionary &p_record) {
    Dictionary out;
    const Array writes = p_record.get(StringName("writes"), Array());
    if (!writes.is_empty()) {
        const Dictionary write = writes[writes.size() - 1];
        out[StringName("operator")] = int(write.get(StringName("operator"), 0));
        out[StringName("basis")] = int(write.get(StringName("basis"), -1));
        out[StringName("outcome")] = int(write.get(StringName("outcome"), -1));
        return out;
    }
    const Array decisions = p_record.get(StringName("decisions"), Array());
    if (!decisions.is_empty()) {
        const Dictionary decision = decisions[decisions.size() - 1];
        out[StringName("operator")]
            = int(decision.get(StringName("operator"), 0));
        out[StringName("basis")] = int(decision.get(StringName("basis"), -1));
        out[StringName("outcome")] = -1;
    }
    return out;
}

} // namespace

bool NetwPredictionEngine::is_replaying() const {
    return replay_steps > 0;
}

int64_t NetwPredictionEngine::mutations_refused_count() const {
    return mutations_refused;
}

#if defined(DEBUG_ENABLED)
int64_t NetwPredictionEngine::replayed_tick_count() const {
    return replayed_ticks;
}

int64_t NetwPredictionEngine::held_fact_total(int p_fact) const {
    int64_t total = 0;
    for (const KeyValue<int64_t, Seat> &seated : seat_by_slot) {
        total += held_fact_of(seated.key, p_fact);
    }
    return total;
}
#endif

int64_t NetwPredictionEngine::cooldown_until_tick_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->lane_cursor.cooldown_until_tick;
}

void NetwPredictionEngine::set_cooldown_until_tick(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->lane_cursor.cooldown_until_tick = p_value;
    }
}

void NetwPredictionEngine::seed_recovery_ledger(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || state_binding_in(*row) == nullptr
        || state_binding_in(*row)->node() == nullptr
        || state_binding_in(*row)->get_set() == nullptr) {
        return;
    }
    for (uint32_t at = 0; at < row->wiring.causal.size(); ++at) {
        if (row->wiring.causal[at] != 0) {
            row->recovery_ledger.seed(int(at));
        }
    }
}

void NetwPredictionEngine::ledger_seed(
    int64_t p_slot,
    const StringName &p_key
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->recovery_ledger.seed(row->wiring.fields.index_of(p_key));
    }
}

void NetwPredictionEngine::ledger_bump_triggered(
    int64_t p_slot,
    const StringName &p_key
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->recovery_ledger.bump_triggered(row->wiring.fields.index_of(p_key));
    }
}

void NetwPredictionEngine::ledger_bump_contracted(
    int64_t p_slot,
    const StringName &p_key
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->recovery_ledger.bump_contracted(
            row->wiring.fields.index_of(p_key)
        );
    }
}

PackedInt64Array NetwPredictionEngine::ledger_counts(
    int64_t p_slot,
    const StringName &p_key
) const {
    PackedInt64Array out = gd::zeroed<PackedInt64Array>(6);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const int field = row->wiring.fields.index_of(p_key);
    if (field < 0) {
        return out;
    }
    int64_t *values = out.ptrw();
    values[0] = row->recovery_ledger.triggered[uint32_t(field)];
    values[1] = row->recovery_ledger.repaired[uint32_t(field)];
    values[2] = row->recovery_ledger.contracted[uint32_t(field)];
    return out;
}

Array NetwPredictionEngine::ledger_fields(int64_t p_slot) const {
    Array out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    for (int at = 0; at < row->wiring.count(); ++at) {
        if (row->recovery_ledger.has(at)) {
            out.push_back(row->wiring.fields.name_at(at));
        }
    }
    return out;
}

int64_t NetwPredictionEngine::out_of_domain_until(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->out_of_domain_until : -1;
}

bool NetwPredictionEngine::out_of_domain_at(
    int64_t p_slot,
    int64_t p_label
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->out_of_domain_until >= 0
        && p_label < row->out_of_domain_until;
}

void NetwPredictionEngine::open_out_of_domain_window(
    int64_t p_slot,
    int64_t p_label,
    int p_cooldown
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->out_of_domain_until = prediction_core::window_after(
            p_label,
            p_cooldown,
            row->out_of_domain_until
        );
    }
}

void NetwPredictionEngine::clear_out_of_domain_window(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->out_of_domain_until = -1;
    }
}

bool NetwPredictionEngine::adopt_environment_epoch(
    int64_t p_slot,
    int64_t p_epoch,
    int64_t p_label,
    int p_cooldown
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || row->environment_epoch == p_epoch) {
        return false;
    }
    if (row->environment_epoch != -1) {
        row->out_of_domain_until = prediction_core::window_after(
            p_label,
            p_cooldown,
            row->out_of_domain_until
        );
    }
    row->environment_epoch = p_epoch;
    return true;
}

void NetwPredictionEngine::clear_environment_epoch(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->environment_epoch = -1;
    }
}

void NetwPredictionEngine::ledger_note_comparison(
    int64_t p_slot,
    bool p_corrected,
    int64_t p_ack,
    double p_correction_threshold
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    if (row->contraction_ledger.armed()) {
        const PackedInt32Array settled
            = ledger_settle(p_slot, p_ack, divergence_columns(p_slot));
        for (int at = 0; at < settled.size(); ++at) {
            row->recovery_ledger.bump_contracted(settled[at]);
        }
    }
    if (!p_corrected) {
        return;
    }
    const double absent = std::numeric_limits<double>::infinity();
    for (int at = 0; at < row->wiring.count(); ++at) {
        if (!row->report.divergence.has(at)
            || row->wiring.trigger_exclude[uint32_t(at)] != 0
            || row->wiring.causal[uint32_t(at)] == 0) {
            continue;
        }
        const double declared = row->wiring.epsilon[uint32_t(at)];
        const double epsilon
            = declared < 0.0 ? p_correction_threshold : declared;
        if (row->report.divergence.at(at, absent) <= epsilon) {
            continue;
        }
        row->recovery_ledger.bump_triggered(at);
    }
}

bool NetwPredictionEngine::ledger_armed(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->contraction_ledger.armed();
}

PackedInt32Array NetwPredictionEngine::ledger_settle(
    int64_t p_slot,
    int64_t p_ack,
    const PackedFloat64Array &p_divergence
) {
    PackedInt32Array contracted;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return contracted;
    }
    const LocalVector<int> settled
        = row->contraction_ledger.settle(p_ack, p_divergence);
    for (uint32_t at = 0; at < settled.size(); ++at) {
        contracted.push_back(settled[at]);
    }
    return contracted;
}

void NetwPredictionEngine::ledger_clear(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->contraction_ledger.clear();
    }
}

void NetwPredictionEngine::record_episode_decision(
    int64_t p_slot,
    int p_operator,
    int64_t p_basis,
    bool p_eligible,
    bool p_applied,
    const Dictionary &p_eligibility
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    predict::EpisodeDecision decision;
    decision.op = predict::Operator(p_operator);
    decision.basis = p_basis;
    decision.eligible = p_eligible;
    decision.applied = p_applied;
    decision.eligibility = p_eligibility;
    row->episode.record_decision(decision);
}

bool NetwPredictionEngine::open_episode(
    int64_t p_slot,
    int64_t p_transition,
    int p_attribution
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    predict::Episode &episode = row->episode;
    if (episode.active && episode.state == predict::EpisodeState::OPEN) {
        return false;
    }
    episode
        .open(row->journal, p_transition, predict::Attribution(p_attribution));
    return true;
}

void NetwPredictionEngine::record_episode_divergence(
    int64_t p_slot,
    int64_t p_transition
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->episode.record_divergence(row->journal, p_transition);
    }
}

void NetwPredictionEngine::stamp_episode_write_delta(
    int64_t p_slot,
    int p_delta_fp
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->episode.stamp_write_delta(int32_t(p_delta_fp));
    }
}

int NetwPredictionEngine::record_episode_write(
    int64_t p_slot,
    int p_operator,
    int64_t p_basis,
    int p_delta_fp,
    const StringName &p_target,
    int p_ack_age,
    int p_trigger_shape,
    bool p_evidence_free,
    bool p_null_operator
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return 0;
    }
    return row->episode.record_write(
        predict::Operator(p_operator),
        p_basis,
        int32_t(p_delta_fp),
        p_target,
        p_ack_age,
        predict::TriggerShape(p_trigger_shape),
        p_evidence_free,
        p_null_operator
    );
}

bool NetwPredictionEngine::episode_active(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->episode.active;
}

int NetwPredictionEngine::episode_state(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->episode.active ? int(row->episode.state) : -1;
}

PackedInt64Array NetwPredictionEngine::episode_stats(int64_t p_slot) const {
    PackedInt64Array out;
    out.resize(STAT_EPISODE_COUNT);
    const predict::Slot *row = row_of(p_slot);
    const predict::Episode absent;
    const predict::Episode &episode = row != nullptr ? row->episode : absent;
    int64_t *values = out.ptrw();
    values[STAT_EPISODE_ID] = episode.id;
    values[STAT_EPISODE_STATE] = episode.active ? int(episode.state) : -1;
    values[STAT_EPISODE_OPENED] = episode.opened_transition;
    values[STAT_EPISODE_ATTRIBUTION] = int(episode.attribution);
    values[STAT_EPISODE_NON_CONTRACTION] = episode.non_contraction_used;
    values[STAT_EPISODE_WITHHELD_NC] = episode.withheld_non_contractions;
    values[STAT_EPISODE_EVIDENCE_FREE_NC]
        = episode.evidence_free_non_contractions;
    values[STAT_EPISODE_NO_TRIGGER_NC] = episode.nc_no_trigger;
    values[STAT_EPISODE_MIXED_TRIGGER_NC] = episode.nc_mixed_trigger;
    values[STAT_EPISODE_CLOSURE_USED] = episode.closure_used;
    values[STAT_EPISODE_AGREEMENT_RUN] = episode.agreement_run;
    values[STAT_EPISODE_LAST_COMPARISON] = episode.last_comparison_transition;
    values[STAT_EPISODE_AGREEMENT_WRITE_ID] = episode.agreement_write_id;
    values[STAT_EPISODE_LAST_WRITE_ID] = episode.last_write_id;
    values[STAT_EPISODE_EVIDENCE_DROPPED] = episode.evidence_dropped;
    values[STAT_EPISODE_WRITE_COUNT] = int64_t(episode.writes.size());
    values[STAT_EPISODE_COMPARISON_COUNT] = int64_t(episode.comparisons.size());
    values[STAT_EPISODE_ACTIVE] = episode.active ? 1 : 0;
    values[STAT_EPISODE_TRANSPORT_DECIDED] = episode.transport_decided ? 1 : 0;
    values[STAT_EPISODE_DISSIPATE_DECIDED] = episode.dissipate_decided ? 1 : 0;
    return out;
}

bool NetwPredictionEngine::episode_budget_exhausted(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->episode.budget_exhausted();
}

bool NetwPredictionEngine::episode_operator_pending(
    int64_t p_slot,
    int p_operator
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr
        && row->episode.newest_write_pending(predict::Operator(p_operator));
}

int64_t NetwPredictionEngine::held_fact_of(int64_t p_slot, int p_fact) const {
    const NetwPredictStats::Book *counters = facts_in(decl_of(p_slot));
    return counters != nullptr ? counters->get_int_fact(p_fact) : 0;
}

void NetwPredictionEngine::set_held_fact(
    int64_t p_slot,
    int p_fact,
    int64_t p_value
) {
    NetwPredictStats::Book *counters = facts_in(decl_of(p_slot));
    if (counters != nullptr) {
        counters->set_int_fact(p_fact, p_value);
    }
}

void NetwPredictionEngine::add_held_fact(
    int64_t p_slot,
    int p_fact,
    int64_t p_delta
) {
    NetwPredictStats::Book *counters = facts_in(decl_of(p_slot));
    if (counters != nullptr) {
        counters->set_int_fact(
            p_fact,
            counters->get_int_fact(p_fact) + p_delta
        );
    }
}

void NetwPredictionEngine::ledger_note_rows(
    int64_t p_slot,
    const predict::StateRow &p_deltas
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int64_t frontier = drive_frontier(p_slot);
    row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const int count
        = std::min(row->wiring.count(), int(p_deltas.values.size()));
    for (int at = 0; at < count; ++at) {
        if (!p_deltas.has(at) || row->wiring.causal[uint32_t(at)] == 0) {
            continue;
        }
        row->recovery_ledger.bump_repaired(at);
        if (row->report.divergence.has(at)) {
            row->contraction_ledger
                .arm(at, row->report.divergence.at(at, INFINITY), frontier);
        }
    }
}

bool NetwPredictionEngine::state_has(int64_t p_slot, int p_field) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->state.has(p_field);
}

Variant NetwPredictionEngine::state_at(int64_t p_slot, int p_field) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->state.has(p_field)
        ? row->state.values[uint32_t(p_field)]
        : Variant();
}

EpisodeReport NetwPredictionEngine::episode(int64_t p_slot) const {
    EpisodeReport out;
    const predict::Slot *row = row_of(p_slot);
    if (row != nullptr) {
        out.episode = row->episode;
    }
    return out;
}

void NetwPredictionEngine::announce_episode(int64_t p_slot, int64_t p_event) {
    const int change = episode_change(p_event);
    if (change < 0) {
        return;
    }
    const Dictionary report = episode_record(p_slot);
    if (reports_to_test(decl_of(p_slot))) {
        NetwMultiplayer::Announcement announced;
        announced.kind = NetwMultiplayer::AnnouncementKind::EPISODE_CHANGED;
        announced.code = change;
        announced.report = report;
        announce_owner(p_slot, announced);
    }
    const Dictionary generator
        = report.get(StringName("generator"), Dictionary());
    record_predict(
        p_slot,
        p_event,
        int64_t(generator.get(StringName("transition"), -1))
    );
}

void NetwPredictionEngine::report_quantum_declaration() {
    NetwMultiplayer *host = session;
    if (host != nullptr) {
        host->warn_pacing_rate_mismatch_once();
    }
}

void NetwPredictionEngine::report_authority_model(int64_t p_slot) {
    if (owner_record(p_slot) == nullptr) {
        return;
    }
    Node *seated = owner_node(p_slot);
    PropertySet compiled;
    const PropertySet *broadcast
        = broadcast_set_of(seated, compiled) ? &compiled : nullptr;
    if (broadcast != nullptr && !broadcast->columns.is_empty()) {
        PackedStringArray keys;
        for (const PropertyColumn &column : broadcast->columns) {
            keys.push_back(String(column.get_key()));
        }
        fault::raise(
            session,
            NETW_FAULT_BROADCAST_UNCORRECTED,
            {subject_name(seated), String(), String(", ").join(keys)},
            subject_of(seated)
        );
    }
    const PropertyBindingPtr input = input_binding_of(p_slot);
    const PropertySet *declared = input != nullptr ? input->get_set() : nullptr;
    if (declared == nullptr || declared->columns.is_empty()) {
        return;
    }
    predict::CommandFrameRecord planned;
    const SchemaRecord *seated_schema = input_schema(p_slot);
    static const SchemaRecord unplannable;
    if (!predict::CommandFrameRecord::open(
            seated_schema != nullptr ? *seated_schema : unplannable,
            planned
        )) {
        fault::raise(
            session,
            NETW_FAULT_INPUT_UNTYPED,
            {subject_name(seated)},
            subject_of(seated)
        );
    }
}

int64_t NetwPredictionEngine::property_class_report_hash(
    int64_t p_slot,
    const PropertySet *p_set
) const {
    PackedStringArray parts;
    for (const PropertyColumn &column : p_set->columns) {
        Array row;
        row.push_back(String(column.get_key()));
        row.push_back(column.unchecked ? 1 : 0);
        row.push_back(column.get_quantizer().is_valid() ? 1 : 0);
        row.push_back(column.correction_threshold);
        row.push_back(column.explicit_teleport_only ? 1 : 0);
        row.push_back(String(column.rate_property));
#if defined(NETW_MODULE)
        parts.push_back(String("%s:%d:%d:%.4f:%d:%s").sprintf(row, nullptr));
#else
        parts.push_back(String("%s:%d:%d:%.4f:%d:%s") % row);
#endif
    }
    const NetwEntityRecord *seated = owner_record(p_slot);
    PropertySet compiled;
    if (broadcast_set_of(owner_node(p_slot), compiled)) {
        const PropertySet *broadcast = &compiled;
        for (const PropertyColumn &column : broadcast->columns) {
            parts.push_back(String("broadcast:") + String(column.get_key()));
        }
    }
    parts.push_back(vformat(
        "controller:%d",
        seated != nullptr && seated->get_control()->resolve() > 0 ? 1 : 0
    ));
    return int64_t(String("\n").join(parts).hash());
}

int64_t NetwPredictionEngine::episode_revision(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->episode_revision : 0;
}

void NetwPredictionEngine::stamp_episode_revision(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->episode_revision += 1;
    }
}

void NetwPredictionEngine::sync_episode(int64_t p_slot) {
    if (episode_stats(p_slot)[STAT_EPISODE_ID] > 0) {
        stamp_episode_revision(p_slot);
    }
}

void NetwPredictionEngine::close_episode(int64_t p_slot) {
    stamp_episode_revision(p_slot);
    announce_episode(p_slot, EventPlane::EPISODE_CLOSE);
}

Dictionary NetwPredictionEngine::episode_digest(int64_t p_slot) const {
    const Dictionary record = episode_record(p_slot);
    if (record.is_empty()) {
        return Dictionary();
    }
    const Dictionary generator_row
        = record.get(StringName("generator_row_copy"), Dictionary());
    const Array comparisons = record.get(StringName("comparisons"), Array());

    Dictionary generator;
    generator[StringName("transition")] = int(generator_row.get(
        StringName("transition"),
        record.get(StringName("opened_transition"), -1)
    ));
    generator[StringName("boundary")]
        = int(record.get(StringName("attribution"), 0));

    Dictionary evidence;
    evidence[StringName("comparisons")] = counted(record, "comparisons");
    evidence[StringName("writes")] = counted(record, "writes");
    evidence[StringName("decisions")] = counted(record, "decisions");
    evidence[StringName("taint")] = counted(record, "taint");
    evidence[StringName("secondary_generators")]
        = counted(record, "secondary_generators");
    evidence[StringName("dropped")]
        = int(record.get(StringName("evidence_dropped"), 0));

    Dictionary out;
    out[StringName("id")] = int(record.get(StringName("id"), 0));
    out[StringName("revision")] = episode_revision(p_slot);
    out[StringName("last_comparison_transition")]
        = int(record.get(StringName("last_comparison_transition"), -1));
    out[StringName("last_meter")] = comparisons.is_empty()
        ? 0
        : int(Dictionary(comparisons[comparisons.size() - 1])
                  .get(StringName("meter"), 0));
    out[StringName("generator")] = generator;
    out[StringName("last_operator")] = last_attempt(record);
    out[StringName("disposition")]
        = record.get(StringName("disposition"), Dictionary());
    out[StringName("evidence")] = evidence;
    return out;
}

Dictionary NetwPredictionEngine::episode_record(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return Dictionary();
    }
    return episode(p_slot).to_dictionary(row->witness_details.rows());
}

PackedInt64Array NetwPredictionEngine::drive_cursors(int64_t p_slot) const {
    PackedInt64Array out;
    out.resize(CURSOR_COUNT);
    const predict::Slot *row = row_of(p_slot);
    const predict::Slot absent;
    const predict::Slot &slot = row != nullptr ? *row : absent;
    int64_t *values = out.ptrw();
    values[CURSOR_TAPE_EPOCH] = slot.tape_epoch;
    values[CURSOR_NEXT_TAPE_ENTRY] = slot.next_tape_entry_index;
    values[CURSOR_LAST_DRIVEN_ENTRY] = slot.last_driven_entry_index;
    values[CURSOR_LATEST_INPUT_TICK] = slot.recorded_input_tick;
    values[CURSOR_LAST_DRIVEN_INPUT_TICK] = slot.drive_input_tick;
    values[CURSOR_LAST_FRAME_TRANSITION_TICK] = slot.drive_frame_tick;
    return out;
}

int64_t NetwPredictionEngine::drive_stat(
    int64_t p_slot,
    int p_column,
    int64_t p_absent
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return p_absent;
    }
    const predict::DriveStats &stats = row->stats;
    switch (p_column) {
        case STAT_DRIVE_SEQ:
            return stats.drive_seq;
        case STAT_LAST_DRIVE_LABEL:
            return stats.last_drive_label;
        case STAT_LAST_DRIVE_KIND:
            return int64_t(stats.last_drive_kind);
        case STAT_TAPE_EPOCH:
            return stats.tape_epoch;
        case STAT_TAPE_INDEX:
            return stats.tape_index;
        case STAT_ACK_AGE_TICKS:
            return stats.ack_age_ticks;
        case STAT_AUTHORING_CLAMPED:
            return stats.authoring_clamped;
        case STAT_SPECULATION_HELD:
            return stats.speculation_held;
        case STAT_CHAIN_BREAKS:
            return stats.chain_breaks;
        case STAT_QUANTUM_STEPS:
            return stats.quantum_steps;
        case STAT_QUANTUM_DECLARED:
            return stats.quantum_declared;
        case STAT_QUANTUM_FAULTS:
            return stats.quantum_faults;
        case STAT_OWNER_LOST:
            return row->port.lost;
        case STAT_MAX_REPLAY_DEPTH:
            return stats.max_replay_depth;
        default:
            return p_absent;
    }
}

} // namespace netw
