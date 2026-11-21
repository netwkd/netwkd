#include "netw/predict/engine.hpp"

#include "netw/api/predict_slot_engine.hpp"
#include "netw/script/model.hpp"

#include "godot/engine.hpp"
#include "src/predict/engine_support.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertyColumn;
using repl::PropertySet;

namespace {

void fill_readings(
    predict::Slot &p_row,
    predict::FieldReadings &p_into,
    const Dictionary &p_by_field
) {
    p_into.resize(p_row.wiring.count());
    const Array keys = p_by_field.keys();
    for (int at = 0; at < keys.size(); ++at) {
        const StringName key(keys[at]);
        p_into.note(p_row.wiring.fields.index_of(key), double(p_by_field[key]));
    }
}

void fill_meter_tolerances(
    const predict::Slot &p_row,
    int p_domain,
    double p_fallback_epsilon,
    LocalVector<double> &r_out
) {
    const uint32_t count = uint32_t(p_row.wiring.count());
    if (r_out.size() != count) {
        r_out.resize(count);
    }
    for (uint32_t field = 0; field < count; ++field) {
        const double declared = p_row.wiring.epsilon[field];
        if (p_row.wiring.trigger_exclude[field] != 0) {
            r_out[field] = p_row.wiring.causal[field] != 0 && declared >= 0.0
                ? declared
                : -1.0;
            continue;
        }
        if (p_row.wiring.withheld[field] != 0 && declared >= 0.0) {
            r_out[field] = declared;
            continue;
        }
        if (p_domain == int(predict::Domain::OUT_OF_DOMAIN)) {
            r_out[field] = declared >= 0.0 ? declared : p_fallback_epsilon;
            continue;
        }
        const Ref<NetwQuantize> &quantizer
            = p_row.wiring.codec.quantizers[field];
        r_out[field] = quantizer.is_valid()
            ? quantizer->max_error(
                  static_cast<Variant::Type>(p_row.wiring.codec.types[field])
              )
            : 0.0;
    }
}

PackedFloat64Array packed(const LocalVector<double> &p_values) {
    PackedFloat64Array out;
    out.resize(int64_t(p_values.size()));
    for (uint32_t at = 0; at < p_values.size(); ++at) {
        out.set(int64_t(at), p_values[at]);
    }
    return out;
}

Dictionary read_readings(
    const predict::Slot *p_row,
    const predict::FieldReadings &p_from
) {
    Dictionary out;
    if (p_row == nullptr) {
        return out;
    }
    for (int at = 0; at < p_row->wiring.count(); ++at) {
        if (p_from.has(at)) {
            out[p_row->wiring.fields.name_at(at)] = p_from.at(at, 0.0);
        }
    }
    return out;
}

} // namespace

bool NetwPredictionEngine::is_angle_field(
    int64_t p_slot,
    const StringName &p_key
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    const int field = row->wiring.fields.index_of(p_key);
    return field >= 0 && row->wiring.angle[uint32_t(field)] != 0;
}

bool NetwPredictionEngine::is_trigger_excluded_field(
    int64_t p_slot,
    const StringName &p_key
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    const int field = row->wiring.fields.index_of(p_key);
    return field >= 0 && row->wiring.trigger_exclude[uint32_t(field)] != 0;
}

double NetwPredictionEngine::epsilon_for_field(
    int64_t p_slot,
    const StringName &p_key,
    double p_fallback
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return p_fallback;
    }
    const int field = row->wiring.fields.index_of(p_key);
    if (field < 0 || row->wiring.epsilon[uint32_t(field)] < 0.0) {
        return p_fallback;
    }
    return row->wiring.epsilon[uint32_t(field)];
}

Dictionary NetwPredictionEngine::compared_state_of(
    int64_t p_slot,
    const Dictionary &p_payload
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return p_payload;
    }
    bool any_causal = false;
    for (int at = 0; at < row->wiring.count(); ++at) {
        any_causal = any_causal || row->wiring.causal[uint32_t(at)] != 0;
    }
    if (!any_causal) {
        return p_payload;
    }
    Dictionary out;
    for (int at = 0; at < row->wiring.count(); ++at) {
        if (row->wiring.causal[uint32_t(at)] == 0) {
            continue;
        }
        const StringName &key = row->wiring.fields.name_at(at);
        if (p_payload.has(key)) {
            out[key] = p_payload[key];
        }
    }
    return out;
}

Dictionary NetwPredictionEngine::projection_map(int64_t p_slot) const {
    Dictionary out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    for (int at = 0; at < row->wiring.count(); ++at) {
        const int channel = row->wiring.projection[uint32_t(at)];
        if (channel >= 0) {
            out[row->wiring.fields.name_at(at)]
                = row->wiring.fields.name_at(channel);
        }
    }
    return out;
}

Dictionary NetwPredictionEngine::teleport_distances(int64_t p_slot) const {
    Dictionary out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    for (int at = 0; at < row->wiring.count(); ++at) {
        if (row->wiring.teleport[uint32_t(at)] >= 0.0) {
            out[row->wiring.fields.name_at(at)]
                = row->wiring.teleport[uint32_t(at)];
        }
    }
    return out;
}

bool NetwPredictionEngine::teleport_reached_of(
    int64_t p_slot,
    const Dictionary &p_pose_errors,
    double p_fallback
) const {
    const predict::Slot *row = row_of(p_slot);
    const Array keys = p_pose_errors.keys();
    for (int at = 0; at < keys.size(); ++at) {
        const Variant key = keys[at];
        double threshold = p_fallback;
        if (row != nullptr) {
            const int field = row->wiring.fields.index_of(StringName(key));
            if (field >= 0 && row->wiring.teleport[uint32_t(field)] >= 0.0) {
                threshold = row->wiring.teleport[uint32_t(field)];
            }
        }
        if (double(p_pose_errors[key]) >= threshold) {
            return true;
        }
    }
    return false;
}

Dictionary NetwPredictionEngine::transport_of(
    int64_t p_slot,
    const Dictionary &p_predicted,
    const Dictionary &p_authority,
    const Dictionary &p_current
) const {
    Dictionary invalid;
    invalid[StringName("restore")] = Dictionary();
    invalid[StringName("delta")] = Dictionary();
    invalid[StringName("valid")] = false;

    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return invalid;
    }

    Dictionary restore;
    Dictionary deltas;
    for (int at = 0; at < row->wiring.count(); ++at) {
        if (row->wiring.pose[uint32_t(at)] == 0) {
            continue;
        }
        const StringName &key = row->wiring.fields.name_at(at);
        if (!p_predicted.has(key) || !p_authority.has(key)
            || !p_current.has(key)) {
            return invalid;
        }
        const Variant delta = prediction_core::pose_delta(
            p_authority[key],
            p_predicted[key],
            row->wiring.angle[uint32_t(at)] != 0
        );
        if (delta.get_type() == Variant::NIL) {
            return invalid;
        }
        deltas[key] = delta;
        restore[key] = prediction_core::pose_advance(p_current[key], delta);
    }

    Dictionary out;
    out[StringName("restore")] = restore;
    out[StringName("delta")] = deltas;
    out[StringName("valid")] = !restore.is_empty();
    return out;
}

void NetwPredictionEngine::note_breach_source(
    int64_t p_slot,
    const StringName &p_source
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->breach_source = p_source;
    }
}

StringName NetwPredictionEngine::breach_source_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || row->breach_source == StringName()) {
        return StringName("default");
    }
    return row->breach_source;
}

void NetwPredictionEngine::clear_divergence(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->report.divergence.clear();
    }
}

Dictionary NetwPredictionEngine::divergence_report(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return read_readings(
        row,
        row == nullptr ? predict::FieldReadings() : row->report.divergence
    );
}

void NetwPredictionEngine::note_tier_errors(
    int64_t p_slot,
    const Dictionary &p_by_field
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        fill_readings(*row, row->report.tier_error, p_by_field);
    }
}

void NetwPredictionEngine::clear_tier_errors(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->report.tier_error.clear();
    }
}

Dictionary NetwPredictionEngine::tier_error_report(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return read_readings(
        row,
        row == nullptr ? predict::FieldReadings() : row->report.tier_error
    );
}

void NetwPredictionEngine::note_verdict_reason(int64_t p_slot, int p_reason) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->report.verdict_reason = p_reason;
    }
}

int NetwPredictionEngine::verdict_reason_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->report.verdict_reason;
}

void NetwPredictionEngine::note_attribution(
    int64_t p_slot,
    int p_attribution,
    int64_t p_transition
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->report.attribution = p_attribution;
        row->report.attributed_transition = p_transition;
    }
}

int NetwPredictionEngine::attribution_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->report.attribution;
}

int64_t NetwPredictionEngine::attributed_transition_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->report.attributed_transition;
}

void NetwPredictionEngine::note_compare_staleness(int64_t p_slot, int p_ticks) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->report.compare_staleness = p_ticks;
    }
}

int NetwPredictionEngine::compare_staleness_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? -1 : row->report.compare_staleness;
}

void NetwPredictionEngine::note_reconciling(int64_t p_slot, bool p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->report.reconciling = p_value;
    }
}

bool NetwPredictionEngine::reconciling(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->report.reconciling;
}

double NetwPredictionEngine::epsilon_of_slot(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0.0 : row->config.epsilon;
}

double NetwPredictionEngine::teleport_threshold_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0.0 : row->config.teleport_threshold;
}

int NetwPredictionEngine::correction_cooldown_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->config.correction_cooldown_ticks;
}

void NetwPredictionEngine::record_owner_claims(
    int64_t p_slot,
    const predict::CommandFrameRecord &p_frame
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const predict::CommandFrame &frame = p_frame.wire();
    const uint32_t claimed_count
        = MIN(frame.evidence.size(), frame.transitions.size());
    for (uint32_t at = 0; at < claimed_count; ++at) {
        const int64_t claimed = frame.transitions[at].index;
        if (claimed < 0) {
            continue;
        }
        row->owner_claims.record(claimed, frame.evidence[at]);
    }
    row->owner_claims.trim(predict::TAPE_HISTORY_LIMIT);
}

void NetwPredictionEngine::clear_owner_claims(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->owner_claims.clear();
    }
}

int NetwPredictionEngine::owner_claim_count(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->owner_claims.size() : 0;
}

PackedInt64Array NetwPredictionEngine::judge_owner_claim(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_fingerprint
) {
    PackedInt64Array verdict;
    verdict.push_back(0);
    verdict.push_back(0);
    verdict.push_back(-1);

    predict::Slot *row = mutable_row_of(p_slot);
    predict::CommandEvidenceWire claim;
    if (row == nullptr || !row->owner_claims.take(p_transition, claim)) {
        return verdict;
    }
    verdict.set(0, 1);
    if (int64_t(claim.post_fp) == p_fingerprint) {
        verdict.set(1, 1);
        return verdict;
    }

    const int at = row->journal.index_of(p_transition);
    if (at < 0) {
        return verdict;
    }

    const uint8_t local_evidence = row->journal.evidence_mask_at(at);
    const bool substituted
        = (row->journal.flags_at(at) & predict::ROW_SUBSTITUTED) != 0;
    const bool both_witness = (local_evidence & predict::EVIDENCE_WITNESS) != 0
        && (claim.evidence_mask & predict::EVIDENCE_WITNESS) != 0;
    const int32_t witness_fp = row->journal.witness_fp_at(at);
    row->journal.mark_witness_match(
        p_transition,
        both_witness && claim.witness_fp == witness_fp
    );

    const int attribution = prediction_core::attribute(
        claim.pre_fp == row->journal.pre_fp_at(at),
        !substituted,
        claim.e_digest == row->journal.e_digest_at(at),
        claim.topo_fp == row->journal.topo_fp_at(at),
        claim.raw_fp == row->journal.raw_fp_at(at),
        claim.witness_fp == witness_fp,
        local_evidence,
        claim.evidence_mask,
        true
    );
    row->journal.mark_attribution(
        p_transition,
        predict::Attribution(attribution)
    );

    PackedInt32Array local_family;
    PackedInt32Array peer_family;
    if (attribution == int(predict::Attribution::PRE_STATE)) {
        const predict::FamilyFingerprints families
            = row->journal.pre_families_at(at);
        local_family.push_back(families.pose);
        local_family.push_back(families.momentum);
        local_family.push_back(families.controller);
        peer_family.push_back(claim.pre_pose_fp);
        peer_family.push_back(claim.pre_momentum_fp);
        peer_family.push_back(claim.pre_controller_fp);
    } else if (attribution == int(predict::Attribution::CLOSURE)) {
        const predict::FamilyFingerprints families
            = row->journal.post_families_at(at);
        local_family.push_back(families.pose);
        local_family.push_back(families.momentum);
        local_family.push_back(families.controller);
        peer_family.push_back(claim.post_pose_fp);
        peer_family.push_back(claim.post_momentum_fp);
        peer_family.push_back(claim.post_controller_fp);
    }
    row->journal.mark_differing_family(
        p_transition,
        predict::DifferingFamily(
            prediction_core::differing_family(local_family, peer_family)
        )
    );

    verdict.set(2, attribution);
    return verdict;
}

PackedFloat64Array NetwPredictionEngine::divergence_columns(
    int64_t p_slot
) const {
    PackedFloat64Array row_out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || state_binding_in(*row) == nullptr
        || state_binding_in(*row)->get_set() == nullptr) {
        return row_out;
    }
    const int count = row->wiring.count();
    row_out.resize(count);
    double *values = row_out.ptrw();
    for (int at = 0; at < count; ++at) {
        values[at] = row->report.divergence.at(
            at,
            std::numeric_limits<double>::infinity()
        );
    }
    return row_out;
}

int NetwPredictionEngine::trigger_shape_of(
    int64_t p_slot,
    const PackedFloat64Array &p_field_errors,
    double p_fallback_epsilon
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return int(predict::TriggerShape::NONE);
    }
    LocalVector<double> errors;
    errors.resize(uint32_t(p_field_errors.size()));
    for (uint32_t at = 0; at < errors.size(); ++at) {
        errors[at] = p_field_errors[int(at)];
    }
    return int(predict::trigger_shape(row->wiring, errors, p_fallback_epsilon));
}

void NetwPredictionEngine::report_owner_claim(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_fingerprint
) {
    const PackedInt64Array verdict
        = judge_owner_claim(p_slot, p_transition, p_fingerprint);
    if (verdict[CLAIM_JUDGED] == 0) {
        return;
    }
    add_held_fact(p_slot, NetwPredictStats::FACT_CLIENT_FP_VERIFIED, 1);
    predict::Slot *row = mutable_row_of(p_slot);
    const bool mismatched_before
        = row != nullptr && row->owner_claim_mismatched;
    if (row != nullptr) {
        row->owner_claim_mismatched = verdict[CLAIM_MATCHED] == 0;
    }
    if (verdict[CLAIM_MATCHED] != 0) {
        return;
    }
    if (!mismatched_before) {
        add_held_fact(p_slot, NetwPredictStats::FACT_CLIENT_MISMATCHES, 1);
    }
}

predict::Judged NetwPredictionEngine::judge_state(
    int64_t p_slot,
    int64_t p_ack,
    int64_t p_domain,
    int64_t p_row_flags,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_payload
) {
    const predict::Decl *decl = decl_of(p_slot);
    const double epsilon = decl != nullptr ? decl->correction_threshold : 0.0;
    const predict::StateVerdict *verdict = compare_rows(
        p_slot,
        p_ack,
        p_predicted,
        p_payload,
        int(p_domain),
        epsilon
    );
    predict::Slot *row = mutable_row_of(p_slot);
    predict::Judged out;
    if (verdict != nullptr) {
        note_errors(*row, row->report.divergence, verdict->all_field_errors);
        out.divergence = verdict->divergence;
        out.corrected = verdict->corrected;
        out.settled = verdict->settled;
        out.meter = verdict->meter;
    } else {
        const int exact_verdict = (p_row_flags & predict::ROW_ACKED) == 0
            ? int(NetwPrediction::EXACT_VERDICT_UNJUDGED)
            : ((p_row_flags & predict::ROW_MATCHED) != 0
                   ? int(NetwPrediction::EXACT_VERDICT_EQUAL)
                   : int(NetwPrediction::EXACT_VERDICT_UNEQUAL));
        Judgement judged;
        if (row != nullptr) {
            bool diverged = true;
            fill_correction_tolerances(
                *row,
                row->config.epsilon,
                row->correction_tolerances
            );
            judged.divergence = predict::judge_rows(
                row->wiring,
                p_predicted,
                p_payload,
                row->correction_tolerances,
                row->judged_errors,
                diverged
            );
            const bool exact = p_domain == int64_t(predict::Domain::IN_DOMAIN)
                && exact_verdict != int(NetwPrediction::EXACT_VERDICT_UNJUDGED);
            judged.corrected = exact
                ? exact_verdict == int(NetwPrediction::EXACT_VERDICT_UNEQUAL)
                : diverged;
            note_errors(*row, row->report.divergence, row->judged_errors);
            fill_meter_tolerances(
                *row,
                int(p_domain),
                epsilon,
                row->meter_tolerances
            );
            out.meter
                = predict::measure(row->judged_errors, row->meter_tolerances);
        } else {
            judged.divergence = std::numeric_limits<double>::infinity();
            judged.corrected = true;
        }
        out.divergence = judged.divergence;
        out.corrected = judged.corrected;
        out.settled = p_domain == int64_t(predict::Domain::OUT_OF_DOMAIN)
            || exact_verdict != int(NetwPrediction::EXACT_VERDICT_UNJUDGED);
    }
    record_predict(p_slot, EventPlane::PREDICT_EVALUATE, p_ack);
    if (row != nullptr && !out.corrected
        && row->journal.slot_of(p_ack) >= 0) {
        predict::LaneCursor &lane = row->lane_cursor;
        lane.tolerated_at = p_ack;
        lane.tolerated_local_fp = row->journal.post_fp_of(p_ack);
        lane.tolerated_peer_known = false;
    }
    if (out.corrected && out.meter == 0) {
        out.meter = 1;
    } else if (!out.corrected) {
        out.meter = 0;
    }
    return out;
}

void NetwPredictionEngine::charge_divergence(
    int64_t p_slot,
    const predict::AckFrame &p_frame,
    int64_t p_transition,
    int p_index,
    bool p_complete,
    int p_attribution
) {
    const bool named
        = p_index >= 0 && uint32_t(p_index) < p_frame.records.size();
    const predict::JournalRow row = journal_row(p_slot, p_transition);
    const int local_evidence = row.present ? int(row.evidence_mask) : 0;
    const int peer_evidence
        = p_complete && named ? int(p_frame.records[p_index].evidence_mask) : 0;
    const bool both_witness = p_complete
        && (local_evidence & int(predict::EVIDENCE_WITNESS)) != 0
        && (peer_evidence & int(predict::EVIDENCE_WITNESS)) != 0;
    const bool witness_equal = both_witness && row.present && named
        && row.witness_fp == p_frame.records[p_index].witness_fp;
    if (journal_has(p_slot, p_transition)) {
        mark_witness_match(p_slot, p_transition, witness_equal);
    }
    note_attribution(p_slot, p_attribution, p_transition);
}

void NetwPredictionEngine::mark_witness_match(
    int64_t p_slot,
    int64_t p_transition,
    bool p_matched
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->journal.mark_witness_match(p_transition, p_matched);
}

void NetwPredictionEngine::declare_skipped(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_label
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->declare_skipped(p_transition, p_label);
    }
}

void NetwPredictionEngine::mark_aligned_error(
    int64_t p_slot,
    int64_t p_transition,
    double p_error
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->journal.mark_aligned_error(p_transition, p_error);
    }
}

void NetwPredictionEngine::acknowledge(
    int64_t p_slot,
    int64_t p_transition,
    bool p_matched
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->acknowledge(p_transition, p_matched);
    }
}

const predict::StateVerdict *NetwPredictionEngine::compare_rows(
    int64_t p_slot,
    int64_t p_transition,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_authority,
    int p_domain,
    double p_correction_threshold
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !journal_has(p_slot, p_transition)) {
        return nullptr;
    }
    fill_correction_tolerances(
        *row,
        p_correction_threshold,
        row->correction_tolerances
    );
    fill_meter_tolerances(
        *row,
        p_domain,
        p_correction_threshold,
        row->meter_tolerances
    );
    return &row->compare(
        p_transition,
        p_transition,
        p_predicted,
        p_authority,
        row->correction_tolerances,
        row->meter_tolerances,
        p_correction_threshold,
        row->latches.stream_reconstructed,
        row->lane_cursor.ack_domain_confirmed
    );
}

const predict::StateRow &NetwPredictionEngine::compared_row(
    int64_t p_slot
) const {
    static const predict::StateRow NONE;
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->compared : NONE;
}

const predict::StateRow &NetwPredictionEngine::received_row(
    int64_t p_slot
) const {
    static const predict::StateRow NONE;
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->received : NONE;
}

void NetwPredictionEngine::announce_evaluation(
    NetwPrediction *p_handle,
    int64_t p_slot,
    int64_t p_recv_tick,
    int64_t p_ack,
    double p_divergence,
    bool p_corrected
) {
#if defined(DEBUG_ENABLED)
    if (predict::Slot *row = mutable_row_of(p_slot)) {
        predict::EvaluationRecord &record = row->last_evaluation;
        record.recv_tick = p_recv_tick;
        record.divergence = p_divergence;
        record.corrected = p_corrected;
        ++record.serial;
    }
#endif
    if (p_handle == nullptr || !reports_to_test(decl_of(p_slot))) {
        return;
    }
    NetwMultiplayer::Announcement announced;
    announced.kind = NetwMultiplayer::AnnouncementKind::STATE_EVALUATED;
    announced.from = p_recv_tick;
    announced.to = p_ack;
    announced.amount = p_divergence;
    announced.flag = p_corrected;
    announce_owner(p_slot, announced);
}

#if defined(DEBUG_ENABLED)
const predict::EvaluationRecord *NetwPredictionEngine::last_evaluation_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? &row->last_evaluation : nullptr;
}

const predict::Slot *NetwPredictionEngine::diag_row_of(int64_t p_slot) const {
    return row_of(p_slot);
}
#endif

void NetwPredictionEngine::announce_divergence(
    int64_t p_slot,
    int64_t p_transition,
    int64_t p_attribution
) {
    if (reports_to_test(decl_of(p_slot))) {
        NetwMultiplayer::Announcement announced;
        announced.kind = NetwMultiplayer::AnnouncementKind::DIVERGENCE_DETECTED;
        announced.from = p_transition;
        announced.code = p_attribution;
        announce_owner(p_slot, announced);
    }
    record_predict(p_slot, EventPlane::DIVERGENCE, p_transition);
}

PackedFloat64Array NetwPredictionEngine::correction_tolerances(
    int64_t p_slot,
    double p_fallback_epsilon
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return PackedFloat64Array();
    }
    LocalVector<double> values;
    fill_correction_tolerances(*row, p_fallback_epsilon, values);
    return packed(values);
}

PackedFloat64Array NetwPredictionEngine::meter_tolerances(
    int64_t p_slot,
    int p_domain,
    double p_fallback_epsilon
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return PackedFloat64Array();
    }
    LocalVector<double> values;
    fill_meter_tolerances(*row, p_domain, p_fallback_epsilon, values);
    return packed(values);
}

int NetwPredictionEngine::settle_comparison(
    int64_t p_slot,
    int64_t p_recv_tick,
    int64_t p_ack,
    bool p_reconstructed,
    bool p_corrected,
    bool p_settled,
    double p_divergence,
    bool p_probation_before,
    int64_t p_episode_state_before,
    NetwPredictSlotEngine &p_engine
) {
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    const predict::Decl *decl = decl_of(p_slot);
    const double epsilon = decl != nullptr ? decl->correction_threshold : 0.0;
    if (p_reconstructed) {
        ledger_note_comparison(p_slot, p_corrected, p_ack, epsilon);
    }
    const int attribution = attribution_for(p_slot, p_ack);

    if (p_probation_before && p_reconstructed && p_corrected) {
        if (open_episode(p_slot, p_ack, attribution)) {
            sync_episode(p_slot);
        }
        note_verdict_reason(
            p_slot,
            predict::VERDICT_REASON_PROBATION_REQUARANTINE
        );
        announce_divergence(p_slot, p_ack, attribution);
        p_engine.enter_fallback(p_ack, attribution, false);
        announce_evaluation(
            handle,
            p_slot,
            p_recv_tick,
            p_ack,
            p_divergence,
            true
        );
        return SETTLE_REQUARANTINED;
    }

    if (p_corrected) {
        if (episode_state(p_slot) == NetwPrediction::EPISODE_STATE_OPEN) {
            record_episode_divergence(p_slot, p_ack);
            sync_episode(p_slot);
        } else if (open_episode(p_slot, p_ack, attribution)) {
            sync_episode(p_slot);
        }
        announce_divergence(p_slot, p_ack, attribution);
    }
    if (p_settled) {
        mark_aligned_error(p_slot, p_ack, p_divergence);
    }
    record_episode_comparison(p_slot, p_episode_state_before);

    const int refusal = refuse_recovery(p_slot, p_corrected);
    if (refusal == predict::VERDICT_REASON_NONE) {
        return SETTLE_PROCEED;
    }
    if (refusal == predict::VERDICT_REASON_EVIDENCE_EXHAUSTED) {
        p_engine.enter_fallback(p_ack, attribution, false);
    }
    announce_evaluation(
        handle,
        p_slot,
        p_recv_tick,
        p_ack,
        p_divergence,
        p_corrected
    );
    return SETTLE_REFUSED;
}

void NetwPredictionEngine::record_episode_comparison(
    int64_t p_slot,
    int64_t p_previous_state
) {
    const int64_t state = episode_state(p_slot);
    if (p_previous_state == NetwPrediction::EPISODE_STATE_OPEN
        && state == NetwPrediction::EPISODE_STATE_CLOSED) {
        close_episode(p_slot);
        return;
    }
    sync_episode(p_slot);
    if (p_previous_state != NetwPrediction::EPISODE_STATE_OPEN
        && state == NetwPrediction::EPISODE_STATE_OPEN) {
        announce_episode(p_slot, EventPlane::EPISODE_OPEN);
    }
}

ComparisonChoice NetwPredictionEngine::select_comparison(
    int64_t p_slot,
    int64_t p_ack
) {
    ComparisonChoice chosen;
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return chosen;
    }
    if (row->config.schedule == int(Schedule::FRAME)) {
        const int64_t label = tape_label_of(p_slot, p_ack);
        if (label < 0 || !row->book.carries_fields(p_ack + 1)) {
            return chosen;
        }
        note_compare_staleness(p_slot, 0);
        chosen.label = label;
        chosen.key = p_ack + 1;
        chosen.chosen = true;
        return chosen;
    }
    if (!row->lane_open) {
        return chosen;
    }
    const int64_t compared = row->book.newest_at_or_before(p_ack + 1);
    note_compare_staleness(
        p_slot,
        compared < 0 ? -1 : int(p_ack + 1 - compared)
    );
    chosen.label = p_ack;
    chosen.key = compared;
    chosen.chosen = true;
    return chosen;
}

OpenedComparison NetwPredictionEngine::open_state_comparison(
    int64_t p_slot,
    int64_t p_recv_tick,
    int64_t p_ack,
    const predict::Arrival *p_arrival,
    const predict::StateRow *p_held
) {
    OpenedComparison opened;
    if (p_ack < 0) {
        return opened;
    }
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    if (ack_domain_confirmed_of(p_slot)) {
        const int age = mark_authority_ack(p_slot, p_ack);
        predict::Decl *decl = decl_of(p_slot);
        if (age >= 0 && decl != nullptr) {
            decl->ack_age_ticks = age;
        }
    }
    const int admission = admit_state(p_slot, p_ack);
    if (admission == ADMIT_REALIGN) {
        opened.realign = true;
        return opened;
    }
    if (admission == ADMIT_REALIGN_PENDING
        || admission == ADMIT_RESEED_IGNORED) {
        announce_evaluation(handle, p_slot, p_recv_tick, p_ack, 0.0, false);
        return opened;
    }
    if (admission == ADMIT_CLOSED) {
        return opened;
    }
    if (admission == ADMIT_PREDATES_WRITE) {
        trim_history(p_slot, p_ack);
        return opened;
    }
    const ComparisonChoice chosen = select_comparison(p_slot, p_ack);
    predict::Slot *row = mutable_row_of(p_slot);
    if (!chosen.chosen || row == nullptr) {
        return opened;
    }
    opened = open_comparison(p_slot, p_ack);
    opened.label = chosen.label;
    opened.key = chosen.key;
    opened.opened = true;
    row->compare_stats.comparisons_ran += opened.reconstructed ? 1 : 0;
    row->compare_stats.comparisons_skipped += opened.reconstructed ? 0 : 1;
    if (chosen.key < 0
        || !row->book.read_row(row->wiring.codec, chosen.key, row->compared)) {
        row->compared.resize(row->wiring.count());
    }
    take_received(p_slot, p_arrival, p_held);
    return opened;
}

OpenedComparison NetwPredictionEngine::open_comparison(
    int64_t p_slot,
    int64_t p_ack
) {
    OpenedComparison opened;
    opened.domain = int(predict::Domain::OUT_OF_DOMAIN);
    opened.episode_state = -1;
    if (row_of(p_slot) == nullptr) {
        return opened;
    }
    const int at = journal_slot_of(p_slot, p_ack);
    if (at >= 0) {
        opened.domain = journal_domain_at(p_slot, at);
        opened.row_flags = journal_flags_at(p_slot, at);
    }
    opened.episode_state = episode_state(p_slot);
    opened.probation = probation_pending(p_slot);
    const bool reconstructed = stream_reconstructed_of(p_slot);
    opened.reconstructed = reconstructed;
    if (!reconstructed) {
        clear_divergence(p_slot);
        note_verdict_reason(
            p_slot,
            int(predict::VERDICT_REASON_AWAITING_RECONSTRUCTION)
        );
    }
    return opened;
}

int NetwPredictionEngine::attribution_for(
    int64_t p_slot,
    int64_t p_transition
) const {
    const int unknown = int(predict::Attribution::UNKNOWN);
    if (row_of(p_slot) == nullptr) {
        return unknown;
    }
    const int at = journal_slot_of(p_slot, p_transition);
    if (at >= 0) {
        const int charged = journal_attribution_at(p_slot, at);
        if (charged != unknown) {
            return charged;
        }
    }
    if (attributed_transition_of(p_slot) == p_transition) {
        return attribution_of(p_slot);
    }
    if (at >= 0
        && (journal_flags_at(p_slot, at) & predict::ROW_SUBSTITUTED) != 0) {
        return int(predict::Attribution::COMMAND);
    }
    return unknown;
}

int NetwPredictionEngine::trigger_shape(
    int64_t p_slot,
    double p_fallback_epsilon
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return int(predict::TriggerShape::NONE);
    }
    LocalVector<double> errors;
    errors.resize(uint32_t(row->wiring.count()));
    for (uint32_t at = 0; at < errors.size(); ++at) {
        errors[at] = row->report.divergence.at(int(at), -1.0);
    }
    return int(predict::trigger_shape(row->wiring, errors, p_fallback_epsilon));
}

PackedInt64Array NetwPredictionEngine::compare_stats(int64_t p_slot) const {
    PackedInt64Array out = gd::zeroed<PackedInt64Array>(STAT_COMPARE_COUNT);
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    const predict::CompareStats &stats = row->compare_stats;
    int64_t *values = out.ptrw();
    values[STAT_COMPARISONS_RAN] = stats.comparisons_ran;
    values[STAT_COMPARISONS_SKIPPED] = stats.comparisons_skipped;
    values[STAT_FP_VERIFIED] = stats.fp_verified;
    values[STAT_FP_MISMATCHES] = stats.fp_mismatches;
    values[STAT_FIRST_DIVERGENT_TRANSITION] = stats.first_divergent_transition;
    return out;
}

} // namespace netw
