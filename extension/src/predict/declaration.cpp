#include "netw/predict/engine.hpp"

#include "netw/api/predict_slot_engine.hpp"
#include "netw/script/model.hpp"

#include "godot/engine.hpp"
#include "godot/os.hpp"
#include "netw/predict/decl.hpp"
#include "src/predict/engine_support.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBindingPtr;
using repl::PropertyColumn;
using repl::PropertySet;

namespace {

LocalVector<double> tolerance_row(
    const PackedFloat64Array &p_values,
    int p_width
) {
    LocalVector<double> out;
    out.resize(uint32_t(p_width));
    for (int at = 0; at < p_width; ++at) {
        out[uint32_t(at)] = at < p_values.size() ? p_values[at] : -1.0;
    }
    return out;
}

bool claims_exact(
    const NetwEntityRecord &p_record,
    Node *p_owner,
    int p_schedule
) {
    return p_schedule == NetwSimulation::SCHEDULE_STEPPED
        && !NetwMultiplayer::sim_physics_driven(p_record, p_owner);
}

bool declared_angle(Node *p_node, const StringName &p_key) {
    if (p_node == nullptr) {
        return false;
    }
    const Ref<NetwInterpolate> spec
        = netw::script::model::get_node_property_interpolator(p_node, p_key);
    return spec.is_valid() && spec->get_mode() == NetwInterpolate::MODE_ANGLE;
}

int64_t declared_type(Node *p_node, const StringName &p_key) {
    return p_node != nullptr
        ? netw::script::model::get_node_property_type(p_node, p_key)
        : int64_t(Variant::NIL);
}

LocalVector<predict::FieldDecl> field_declaration(
    const PropertyBindingPtr &p_binding
) {
    LocalVector<predict::FieldDecl> out;
    const PropertySet *declared
        = p_binding != nullptr ? p_binding->get_set() : nullptr;
    if (declared == nullptr) {
        return out;
    }
    Node *node = p_binding->node();
    for (const PropertyColumn &column : declared->columns) {
        const StringName key = column.get_key();
        out.push_back(field_decl(
            key,
            int(
                column.unchecked ? predict::PropertyClass::DERIVED
                                 : predict::PropertyClass::CAUSAL
            ),
            column.rate_property,
            column.correction_weight,
            column.explicit_teleport_only,
            column.unchecked,
            column.correction_threshold,
            column.teleport_threshold,
            declared_angle(node, key),
            column.get_quantizer(),
            int(declared_type(node, key))
        ));
        out[out.size() - 1].column_type = column.shape.type;
    }
    return out;
}

StringName part_key(const String &p_path, const StringName &p_key) {
    return StringName(p_path + String(":") + String(p_key));
}

bool owner_declares(
    const LocalVector<predict::FieldDecl> &p_declared,
    uint32_t p_owned,
    const StringName &p_key
) {
    if (gd::indexed_path(p_key) == nullptr) {
        return false;
    }
    for (uint32_t at = 0; at < p_owned; ++at) {
        if (p_declared[at].key == p_key) {
            return true;
        }
    }
    return false;
}

void declare_parts(
    NetwMultiplayer *p_session,
    NetwEntityRecord *p_record,
    const PropertyBindingPtr &p_state,
    repl::BindingBook *p_book,
    LocalVector<predict::FieldDecl> &r_declared,
    LocalVector<predict::StatePart> &r_parts
) {
    NetwEntityRecord *record = p_record;
    Node *root = p_state->node();
    if (record == nullptr || root == nullptr || p_book == nullptr) {
        return;
    }
    const uint32_t owned = r_declared.size();
    for (const NetwEntityRecord::HeldBinding &held :
         record->get_held_bindings()) {
        repl::PropertyBinding *child = p_book->live(held.binding);
        if (child == nullptr || child == p_state || child->node() == nullptr) {
            continue;
        }
        const repl::PropertySet *declared = child->get_set();
        if (declared == nullptr
            || declared->get_record() != repl::SET_RECORD_STATE) {
            continue;
        }
        predict::StatePart part;
        part.binding = child->id;
        part.node = child->node()->get_instance_id();
        part.path = String(root->get_path_to(child->node()));
        part.first = r_declared.size();
        for (predict::FieldDecl decl : field_declaration(child)) {
            decl.key = part_key(part.path, decl.key);
            if (owner_declares(r_declared, owned, decl.key)) {
                fault::raise(
                    p_session,
                    NETW_FAULT_DECLARED_TWICE,
                    {vformat("'%s'", String(decl.key)),
                     String(root->get_name())},
                    uint64_t(gd::instance_id(child->node()))
                );
                continue;
            }
            if (decl.rate_property != StringName()) {
                decl.rate_property = part_key(part.path, decl.rate_property);
            }
            r_declared.push_back(decl);
        }
        part.count = r_declared.size() - part.first;
        if (part.count > 0) {
            r_parts.push_back(part);
        }
    }
}

StringName class_name_of(int p_property_class) {
    if (p_property_class == int(predict::PropertyClass::DERIVED)) {
        return StringName("DERIVED");
    }
    if (p_property_class == int(predict::PropertyClass::COSMETIC)) {
        return StringName("COSMETIC");
    }
    return StringName("CAUSAL");
}

String joined(const PackedStringArray &p_names) {
    return String(", ").join(p_names);
}

Dictionary finding_of(
    const char *p_code,
    const char *p_severity,
    const PackedStringArray &p_fields,
    const String &p_message
) {
    Dictionary out;
    out[StringName("code")] = StringName(p_code);
    out[StringName("severity")] = StringName(p_severity);
    out[StringName("fields")] = p_fields;
    out[StringName("message")] = p_message;
    return out;
}

} // namespace

predict::RecoveryRequest recovery_request(
    int p_width,
    const predict::StateRow &p_predicted,
    const predict::StateRow &p_authority,
    const predict::StateRow &p_current,
    const PackedFloat64Array &p_field_errors,
    int64_t p_basis,
    int64_t p_current_label,
    int p_policy,
    double p_fallback_epsilon,
    double p_fallback_teleport,
    int p_max_restore_ticks,
    int p_ack_age_ticks,
    int p_correction_cooldown_ticks,
    double p_tick_delta,
    int p_domain,
    int p_attribution,
    bool p_contact_window,
    bool p_suppressed,
    bool p_pose_unmeasured
) {
    predict::RecoveryRequest out;
    if (!recovery_request_in_range(
            p_policy,
            p_domain,
            p_attribution,
            p_tick_delta
        )) {
        return out;
    }
    out.predicted = p_predicted;
    out.authority = p_authority;
    out.current = p_current;
    out.field_errors = tolerance_row(p_field_errors, p_width);
    out.basis = p_basis;
    out.current_label = p_current_label;
    out.policy = p_policy;
    out.fallback_epsilon = p_fallback_epsilon;
    out.fallback_teleport = p_fallback_teleport;
    out.max_restore_ticks = p_max_restore_ticks;
    out.ack_age_ticks = p_ack_age_ticks;
    out.correction_cooldown_ticks = p_correction_cooldown_ticks;
    out.tick_delta = p_tick_delta;
    out.domain = predict::Domain(p_domain);
    out.attribution = predict::Attribution(p_attribution);
    out.contact_window = p_contact_window;
    out.suppressed = p_suppressed;
    out.pose_unmeasured = p_pose_unmeasured;
    return out;
}

predict::FieldDecl field_decl(
    const StringName &p_key,
    int p_property_class,
    const StringName &p_rate_property,
    double p_correction_weight,
    bool p_teleport_only,
    bool p_trigger_exclude,
    double p_correction_threshold,
    double p_teleport_threshold,
    bool p_angle,
    const Ref<NetwQuantize> &p_quantizer,
    int p_type
) {
    predict::FieldDecl decl;
    decl.key = p_key;
    decl.property_class = p_property_class;
    decl.rate_property = p_rate_property;
    decl.correction_weight = p_correction_weight;
    decl.teleport_only = p_teleport_only;
    decl.trigger_exclude = p_trigger_exclude;
    decl.correction_threshold = p_correction_threshold;
    decl.teleport_threshold = p_teleport_threshold;
    decl.angle = p_angle;
    decl.quantizer = p_quantizer;
    decl.type = p_type;
    return decl;
}

void NetwPredictionEngine::rewire(
    int64_t p_slot,
    const LocalVector<predict::FieldDecl> &p_declaration,
    const LocalVector<predict::FieldDecl> &p_input
) {
    if (!roster_open()) {
        return;
    }
    predict::Slot *found = mutable_row_of(p_slot);
    if (found == nullptr) {
        return;
    }
    predict::Wiring compiled = predict::compile(p_declaration);
    if (slot_kind(p_slot).tolerates_quantizer_steps) {
        predict::tolerate_quantizer_steps(compiled);
    }
    found->rewire(compiled);
    found->input_codec.clear();
    for (uint32_t at = 0; at < p_input.size(); ++at) {
        found->input_codec.append(
            p_input[at].key,
            p_input[at].quantizer,
            p_input[at].type,
            p_input[at].column_type
        );
    }
    found->input_codec.seal();
    if (found->port.bound()) {
        resolve_reach(*found, resolve_owner(*found));
    }
}

bool NetwPredictionEngine::last_correction_teleported_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.last_correction_teleported;
}

void NetwPredictionEngine::set_last_correction_teleported(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.last_correction_teleported = p_value;
    }
}

int64_t NetwPredictionEngine::validated_class_hash_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 0 : row->latches.validated_class_hash;
}

void NetwPredictionEngine::set_validated_class_hash(
    int64_t p_slot,
    int64_t p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.validated_class_hash = p_value;
    }
}

bool NetwPredictionEngine::stream_reconstructed_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.stream_reconstructed;
}

void NetwPredictionEngine::set_stream_reconstructed(
    int64_t p_slot,
    bool p_value
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.stream_reconstructed = p_value;
    }
}

bool NetwPredictionEngine::fallback_latched_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->latches.fallback_latched;
}

void NetwPredictionEngine::set_fallback_latched(int64_t p_slot, bool p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->latches.fallback_latched = p_value;
    }
}

int NetwPredictionEngine::declared_quantum_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? 1 : row->declared_quantum;
}

void NetwPredictionEngine::set_declared_quantum(int64_t p_slot, int p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->declared_quantum = p_value;
    }
}

bool NetwPredictionEngine::raw_fingerprints_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? false : row->cursor.raw_fingerprints;
}

bool NetwPredictionEngine::owner_evidence_from_environment() {
    return !OS::get_singleton()
                ->get_environment("NETW_PREDICT_OWNER_EVIDENCE")
                .is_empty();
}

void NetwPredictionEngine::set_owner_evidence(bool p_value) {
    owner_evidence = p_value;
}

void NetwPredictionEngine::set_raw_fingerprints(int64_t p_slot, bool p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->cursor.raw_fingerprints = p_value;
    }
}

int NetwPredictionEngine::role_for_axes(int p_input_source, int p_sim_mode) {
    return predict::role_for_axes(p_input_source, p_sim_mode);
}

int NetwPredictionEngine::correction_for_mode(int p_policy) {
    return predict::correction_for_mode(p_policy);
}

bool NetwPredictionEngine::reconfigure_from(
    int64_t p_slot,
    const Ref<NetwPrediction> &p_handle
) {
    const predict::Slot *row = row_of(p_slot);
    const predict::Decl *decl = decl_of(p_slot);
    if (row == nullptr || p_handle.is_null() || decl == nullptr) {
        return false;
    }
    NetwMultiplayer *host = session;
    const RID entity = owner_rid(p_slot);
    const sim::Row *standing
        = host != nullptr ? sim_rows->row_of(entity) : nullptr;
    const bool declared = host != nullptr && host->sim_chooses(entity);
    const bool selected = standing != nullptr
        && standing->selected_by.count() > 0 && !sim::leads(standing->mode);
    int seat = ISLAND_NONE;
    if (rolls_back_together(p_slot)) {
        seat = ISLAND_JOINT;
    } else if (declared || selected) {
        seat = ISLAND_DECLARED;
    }
    const NetwEntityRecord &record = *owner_record(p_slot);
    Node *owner = owner_node(p_slot);
    const int schedule = NetwMultiplayer::predict_schedule_of(record, owner);
    return configure(
        p_slot,
        resolved_schedule(p_slot, schedule),
        row->config.role,
        row->config.correction,
        NetwMultiplayer::sim_snap_restore_of(record),
        NetwMultiplayer::sim_max_restore_ticks_of(record),
        seat,
        slot_kind(p_slot).samples_contacts,
        declared,
        declared && !claims_exact(record, owner, schedule),
        decl->correction_threshold,
        decl->teleport_threshold,
        decl->correction_cooldown_ticks
    );
}

bool NetwPredictionEngine::configure(
    int64_t p_slot,
    int p_schedule,
    int p_role,
    int p_correction,
    int p_restore,
    int p_max_restore_ticks,
    int p_island,
    bool p_witness,
    bool p_island_declared,
    bool p_island_approximate,
    double p_epsilon,
    double p_teleport_threshold,
    int p_correction_cooldown_ticks
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr
        || !supports(p_schedule, p_role, p_correction, p_restore, p_island)) {
        return false;
    }
    row->config.schedule = p_schedule;
    const uint32_t depth = predict::book_depth_for(p_schedule);
    row->book.set_depth(depth);
    row->journal.set_capacity(int(depth));
    row->config.role = p_role;
    row->config.correction = p_correction;
    row->config.restore = p_restore;
    row->config.max_restore_ticks = std::max(0, p_max_restore_ticks);
    row->config.island = p_island;
    row->config.witness = p_witness;
    row->config.island_declared = p_island_declared;
    row->config.island_approximate = p_island_approximate;
    row->config.epsilon = std::max(0.0, p_epsilon);
    row->config.teleport_threshold = std::max(0.0, p_teleport_threshold);
    row->config.correction_cooldown_ticks
        = std::max(0, p_correction_cooldown_ticks);
    return true;
}

int NetwPredictionEngine::schedule_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->config.schedule : -1;
}

int NetwPredictionEngine::role_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->config.role : -1;
}

int NetwPredictionEngine::correction_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row == nullptr ? int(CorrectionMode::REPLAY)
                          : row->config.correction;
}

void NetwPredictionEngine::set_role(int64_t p_slot, int p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->config.role = p_value;
    }
}

void NetwPredictionEngine::set_correction(int64_t p_slot, int p_value) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->config.correction = p_value;
    }
}

int NetwPredictionEngine::field_count(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->wiring.count() : 0;
}

int NetwPredictionEngine::field_slot(
    int64_t p_slot,
    const StringName &p_key
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->wiring.fields.index_of(p_key) : -1;
}

StringName NetwPredictionEngine::field_name(int64_t p_slot, int p_field) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr ? row->wiring.fields.name_at(p_field) : StringName();
}

#define NETW_PREDICT_TABLE_READ(m_name, m_column, m_type, m_absent) \
    m_type NetwPredictionEngine::m_name(int64_t p_slot, int p_field) const { \
        const predict::Slot *row = row_of(p_slot); \
        if (row == nullptr || p_field < 0 || p_field >= row->wiring.count()) { \
            return m_absent; \
        } \
        return m_type(row->wiring.m_column[uint32_t(p_field)]); \
    }

NETW_PREDICT_TABLE_READ(projection_of, projection, int, -1)
NETW_PREDICT_TABLE_READ(state_family_of, state_family, int, 0)
NETW_PREDICT_TABLE_READ(converge_rate_of, converge_rate, double, -1.0)
NETW_PREDICT_TABLE_READ(epsilon_of, epsilon, double, -1.0)
NETW_PREDICT_TABLE_READ(teleport_of, teleport, double, -1.0)
NETW_PREDICT_TABLE_READ(is_pose, pose, bool, false)
NETW_PREDICT_TABLE_READ(is_withheld, withheld, bool, false)
NETW_PREDICT_TABLE_READ(is_trigger_excluded, trigger_exclude, bool, false)
NETW_PREDICT_TABLE_READ(is_vote_excluded, vote_exclude, bool, false)
NETW_PREDICT_TABLE_READ(is_angle, angle, bool, false)
NETW_PREDICT_TABLE_READ(is_causal, causal, bool, false)

#undef NETW_PREDICT_TABLE_READ

sim::Facts NetwPredictionEngine::resolution_facts(
    int64_t p_slot,
    const predict::Decl *p_decl
) const {
    sim::Facts facts;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || p_decl == nullptr) {
        return facts;
    }
    facts.declared = true;
    facts.predicted = true;
    facts.input_rows = !row->axes.inputless;
    facts.server_here = row->axes.authority;
    facts.controller_here = row->axes.controlled_locally;
    facts.fallback_latched = row->latches.fallback_latched;
    facts.delay_closed = p_decl->mode == NetwPrediction::MODE_DISABLED;
    facts.selection_count = uint32_t(simulation_subject_count(p_slot));
    return facts;
}

int NetwPredictionEngine::apply_mode(
    int64_t p_slot,
    predict::Decl *p_decl,
    sim::Mode p_mode
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || p_decl == nullptr) {
        return int(NetwPrediction::ROLE_REMOTE);
    }
    const bool local = row->axes.controlled_locally;
    NetwPrediction::InputSource source = NetwPrediction::INPUT_SOURCE_NONE;
    NetwPrediction::SimMode simulation = NetwPrediction::SIM_MODE_DISPLAY;
    switch (p_mode) {
        case sim::Mode::AUTHORITY: {
            source = local ? NetwPrediction::INPUT_SOURCE_LOCAL
                           : NetwPrediction::INPUT_SOURCE_RECEIVED;
            simulation = NetwPrediction::SIM_MODE_AUTHORITATIVE;
        } break;
        case sim::Mode::PREDICT: {
            source = NetwPrediction::INPUT_SOURCE_LOCAL;
            simulation = NetwPrediction::SIM_MODE_SPECULATIVE;
        } break;
        case sim::Mode::ACTIVE: {
            source = NetwPrediction::INPUT_SOURCE_PREDICTED;
            simulation = NetwPrediction::SIM_MODE_SPECULATIVE;
        } break;
        case sim::Mode::NONE:
        case sim::Mode::PROXY: {
            source = local ? NetwPrediction::INPUT_SOURCE_LOCAL
                           : NetwPrediction::INPUT_SOURCE_NONE;
        } break;
    }
    if (row->axes.inputless) {
        source = NetwPrediction::INPUT_SOURCE_NONE;
    }
    p_decl->input_source = source;
    p_decl->sim_mode = simulation;
    return role_for_axes(source, simulation);
}

int NetwPredictionEngine::resolve_axes(int64_t p_slot, predict::Decl *p_decl) {
    return apply_mode(
        p_slot,
        p_decl,
        sim::resolve(resolution_facts(p_slot, p_decl))
    );
}

int NetwPredictionEngine::resolve_correction(
    int64_t p_slot,
    int p_declared
) const {
    const NetwPrediction *handle
        = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    if (p_declared == int(CorrectionMode::AUTO)) {
        if (handle != nullptr) {
            return predict::correction_for_mode(
                NetwMultiplayer::predict_resolve_mode(
                    *owner_record(p_slot),
                    owner_node(p_slot)
                )
            );
        }
        const predict::Slot *row = row_of(p_slot);
        const bool solves = row != nullptr && row->owner_solves;
        return solves ? int(CorrectionMode::SNAP) : int(CorrectionMode::REPLAY);
    }
    if (handle == nullptr) {
        return p_declared;
    }
    const NetwEntityRecord &record = *owner_record(p_slot);
    Node *owner = owner_node(p_slot);
    return predict::integrable_correction(
        p_declared,
        NetwMultiplayer::predict_schedule_of(record, owner),
        NetwMultiplayer::sim_physics_driven(record, owner)
    );
}

int NetwPredictionEngine::settle_correction(int64_t p_slot, int p_declared) {
    const int correction = resolve_correction(p_slot, p_declared);
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || correction == p_declared
        || p_declared != int(CorrectionMode::REPLAY)
        || row->latches.integration_refusal_reported) {
        return correction;
    }
    row->latches.integration_refusal_reported = true;
    Node *seated = owner_node(p_slot);
    fault::raise(
        session,
        NETW_FAULT_PREDICTION_DOWNGRADED,
        {subject_name(seated)},
        subject_of(seated)
    );
    return correction;
}

int NetwPredictionEngine::adopt_declaration(
    int64_t p_slot,
    const PropertyBindingPtr &p_state,
    const PropertyBindingPtr &p_input,
    int p_schedule,
    int p_role,
    int p_declared_correction,
    int p_restore,
    int p_max_restore_ticks,
    int p_island,
    bool p_witness
) {
    const int64_t slot = p_slot;
    LocalVector<predict::FieldDecl> declared = field_declaration(p_state);
    if (owner_record(slot) == nullptr || declared.is_empty()) {
        return -1;
    }
    LocalVector<predict::StatePart> parts;
    declare_parts(
        session,
        seated_record(slot),
        p_state,
        binding_book,
        declared,
        parts
    );
    rewire(slot, declared, field_declaration(p_input));
    set_parts(slot, parts);

    Node *node = p_state->node();
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(slot));
    const predict::Decl *decl = decl_of(slot);
    if (node != nullptr && handle != nullptr && decl != nullptr) {
        bind_owner(slot, node);
        const int64_t route = session != nullptr
            ? liveness_core->route_of(owner_rid(slot))
            : -1;
        set_order_key(slot, route > 0 ? route : -1);
        set_simulate(
            slot,
            NetwMultiplayer::sim_step_of(*owner_record(slot), owner_node(slot))
        );
        set_corridor(slot, decl->correction_path);
        const Dictionary sensors = decl->sensors;
        const Array named = sensors.keys();
        for (int at = 0; at < named.size(); ++at) {
            set_sensor(slot, named[at], sensors[named[at]]);
        }
    }

    const int correction = settle_correction(slot, p_declared_correction);
    if (!configure(
            slot,
            p_schedule,
            p_role,
            correction,
            p_restore,
            p_max_restore_ticks,
            p_island,
            p_witness
        )) {
        return -1;
    }
    return correction;
}

predict::Feed NetwPredictionEngine::role_feed(
    int p_role,
    bool p_fallback_latched
) const {
    predict::Feed feed;
    switch (p_role) {
        case int(Role::PREDICT):
            feed.input_volatile_external = true;
            feed.state_write_gate = false;
            feed.state_listener = predict::Listener::STATE;
            break;
        case int(Role::CONSUME):
            feed.input_write_gate = false;
            feed.input_listener = predict::Listener::INPUT;
            break;
        case int(Role::SIMULATE):
            feed.input_volatile_external = true;
            feed.state_write_gate = false;
            feed.state_listener = predict::Listener::SIMULATED;
            break;
        case int(Role::REMOTE):
            if (p_fallback_latched) {
                feed.input_volatile_external = true;
                feed.state_listener = predict::Listener::QUARANTINE;
            }
            break;
        default:
            break;
    }
    return feed;
}

void NetwPredictionEngine::validate_declaration(
    int64_t p_slot,
    const PropertySet *p_set,
    NetwPredictSlotEngine &p_engine
) {
    if (p_set == nullptr) {
        return;
    }
    for (const PropertyColumn &column : p_set->columns) {
        if (column.get_lane() != PropertySet::RETAINED) {
            continue;
        }
        fault::raise(
            session,
            NETW_FAULT_SETTING_REFUSED,
            {String("{NetwPropertyConfig.retained} on '")
                 + String(column.get_key()) + String("'"),
             String("a predicted entity"),
             String("the property without retained")},
            subject_of(owner_node(p_slot))
        );
    }

    const int64_t config = property_class_report_hash(p_slot, p_set);
    if (config == validated_class_hash_of(p_slot)) {
        return;
    }
    set_validated_class_hash(p_slot, config);

    const Dictionary report = reachability_report_of(p_slot);
    p_engine.emit_reachability_findings(
        report.get(StringName("findings"), Array())
    );
    report_authority_model(p_slot);
}

Dictionary NetwPredictionEngine::reachability_report_of(int64_t p_slot) const {
    NetwPrediction *handle = Object::cast_to<NetwPrediction>(handle_of(p_slot));
    const predict::Decl *decl = decl_of(p_slot);
    if (handle == nullptr || decl == nullptr) {
        return Dictionary();
    }
    const NetwEntityRecord *seated = owner_record(p_slot);
    NetwMultiplayer *host = session;
    int claim = 0;
    if (host != nullptr && seated != nullptr
        && host->sim_chooses(seated->get_handle())) {
        const NetwEntityRecord &record = *owner_record(p_slot);
        Node *owner = owner_node(p_slot);
        claim = claims_exact(
                    record,
                    owner,
                    NetwMultiplayer::predict_schedule_of(record, owner)
                )
            ? 1
            : 2;
    }
    return reachability_report(
        p_slot,
        owner_name(p_slot),
        decl->correction_threshold,
        decl->teleport_threshold,
        decl->pause_on_unpredicted_contact,
        breach_source_of(p_slot),
        claim,
        decl->correction_cooldown_ticks,
        decl->correction_path.is_valid()
    );
}

Dictionary NetwPredictionEngine::reachability_report(
    int64_t p_slot,
    const StringName &p_entity_id,
    double p_correction_threshold,
    double p_teleport_threshold,
    bool p_pause_on_unpredicted_contact,
    const StringName &p_breach_source,
    int p_island_claim,
    int p_contact_window_ticks,
    bool p_has_corridor
) const {
    Dictionary out;
    Array findings;
    Dictionary fields;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        out[StringName("entity")] = p_entity_id;
        out[StringName("fields")] = fields;
        out[StringName("findings")] = findings;
        return out;
    }
    const bool frame_tier = row->config.schedule == int(Schedule::FRAME);

    PackedStringArray unquantized;
    PackedStringArray uncompared;
    PackedStringArray transport_without_epsilon;
    PackedStringArray unobserved;
    PackedStringArray unrepairable;
    PackedStringArray tier_inheritors;
    PackedStringArray rate_inheritors;

    for (int at = 0; at < row->wiring.count(); ++at) {
        const uint32_t field = uint32_t(at);
        const StringName key = row->wiring.fields.name_at(at);
        const String name = String(key);
        const bool causal = row->wiring.causal[field] != 0;
        const StringName channel = row->wiring.rate_property[field];
        const bool trigger_excluded = row->wiring.trigger_exclude[field] != 0;
        const bool teleport_only = row->wiring.withheld[field] != 0;
        const double threshold_override = row->wiring.epsilon[field];
        const double teleport_override = row->wiring.teleport[field];
        const bool enrolled = row->wiring.pose[field] != 0;

        StringName model = StringName("none");
        if (channel != StringName()) {
            model = StringName("channel");
        }

        bool live = false;
        String why = String("no forward model is declared");
        if (model == StringName("channel")) {
            live = row->wiring.projection[field] >= 0;
            why = live ? String()
                       : String("extrapolate(&\"") + String(channel)
                    + String("\") names a column this set does not hold, so "
                             "the channel cannot be read at the tick it would "
                             "advance");
        }

        PackedStringArray operators;
        if (!teleport_only) {
            operators.push_back("sub_teleport_restore");
        }
        if (row->wiring.converge_rate[field] > 0.0) {
            operators.push_back("converge");
        }
        if (live) {
            operators.push_back("carry_advance");
        }
        operators.push_back("full_closure");

        Dictionary forward_model;
        forward_model[StringName("kind")] = model;
        forward_model[StringName("live")] = live;
        forward_model[StringName("why")] = why;

        Dictionary judged;
        judged[StringName("class")]
            = class_name_of(row->wiring.property_class[field]);
        judged[StringName("triggers")] = !trigger_excluded && causal;
        judged[StringName("tolerance")] = threshold_override >= 0.0
            ? threshold_override
            : p_correction_threshold;
        judged[StringName("tolerance_declared")] = threshold_override >= 0.0;
        judged[StringName("teleport_threshold")] = teleport_override >= 0.0
            ? teleport_override
            : p_teleport_threshold;
        judged[StringName("teleport_threshold_declared")]
            = teleport_override >= 0.0;
        judged[StringName("in_tier")] = enrolled;
        judged[StringName("operators")] = operators;
        judged[StringName("forward_model")] = forward_model;
        fields[key] = judged;

        const bool quantized = row->wiring.codec.quantizers[field].is_valid();
        if (causal && !quantized) {
            unquantized.push_back(name);
        }
        if (causal && trigger_excluded && threshold_override < 0.0) {
            unobserved.push_back(name);
        }
        if (causal && teleport_only && !trigger_excluded
            && model == StringName("none")) {
            unrepairable.push_back(name);
        }
        if (!causal && row->wiring.wrote_epsilon[field] != 0) {
            uncompared.push_back(name);
        }
        if (p_has_corridor && causal
            && row->wiring.state_family[field]
                != int(predict::StateFamily::POSE)
            && threshold_override < 0.0) {
            transport_without_epsilon.push_back(name);
        }
        if (enrolled && teleport_override < 0.0) {
            tier_inheritors.push_back(name);
        }
        if (causal && threshold_override < 0.0
            && row->wiring.state_family[field]
                == int(predict::StateFamily::MOMENTUM)) {
            rate_inheritors.push_back(name);
        }
    }

    if (!unquantized.is_empty()) {
        findings.push_back(finding_of(
            "unquantized",
            "debug",
            unquantized,
            String("NetwLagCompensation: causal state properties [")
                + joined(unquantized)
                + String(
                    "] have no quantizer, so their canonical form is "
                    "their raw bits. Legal, but two peers agree only if "
                    "they produce those bits identically."
                )
        ));
    }
    if (!uncompared.is_empty()) {
        findings.push_back(finding_of(
            "uncompared",
            "error",
            uncompared,
            String("Prediction: non-causal state properties [")
                + joined(uncompared)
                + String(
                    "] hold a correction_threshold(). Only a checked field "
                    "is compared, so the threshold decides nothing: it "
                    "cannot raise a correction, and the meter reads its "
                    "own tolerance row. Drop unchecked() if the next "
                    "step reads them, or drop the correction_threshold()."
                )
        ));
    }
    if (!transport_without_epsilon.is_empty()) {
        findings.push_back(finding_of(
            "transport_without_epsilon",
            "debug",
            transport_without_epsilon,
            String(
                "NetwLagCompensation: transport reads causal non-pose "
                "properties ["
            ) + joined(transport_without_epsilon)
                + String(
                    "] without declared epsilons. Their units fall back "
                    "to the entity default, so the transport gate has no "
                    "world-scale tolerance."
                )
        ));
    }
    if (!unobserved.is_empty()) {
        findings.push_back(finding_of(
            "unobserved",
            "error",
            unobserved,
            String("Prediction: causal state properties [") + joined(unobserved)
                + String(
                    "] are unchecked() and declare no "
                    "correction_threshold(), so "
                    "nothing observes them. An unchecked field cannot "
                    "trigger a correction, and without a declared scale "
                    "it does not reach the convergence meter either, "
                    "while the next transition still reads it. A field "
                    "like that drifts until some other field crosses on "
                    "its behalf, which charges the divergence to the "
                    "wrong place. Give each a correction_threshold() at its "
                    "own world scale, or drop unchecked() so it can answer for "
                    "itself."
                )
        ));
    }
    if (!unrepairable.is_empty()) {
        findings.push_back(finding_of(
            "unrepairable",
            "warning",
            unrepairable,
            String("Prediction: causal state properties [")
                + joined(unrepairable)
                + String(
                    "] are teleport_only() and can still trigger, so they "
                    "demand recoveries that no sub-teleport restore may "
                    "write, and each one is answered by writing other "
                    "fields instead. The promoted full closure is the "
                    "only operator that reaches them, and without "
                    "extrapolate() it writes the "
                    "acknowledged value, which a moving field has already "
                    "left. Declare it if the field can be "
                    "advanced, or unchecked() so its drift stops "
                    "asking for recoveries that answer elsewhere. "
                    "NetwPrediction.field_recovery counts which "
                    "happened."
                )
        ));
    }
    if (tier_inheritors.size() >= 2) {
        findings.push_back(finding_of(
            "shared_teleport_threshold",
            "warning",
            tier_inheritors,
            String("NetwLagCompensation: state properties [")
                + joined(tier_inheritors)
                + String(
                    "] are enrolled in the teleport tier and none "
                    "declares teleport_threshold(), so all are measured "
                    "against the entity's "
                )
                + String::num(p_teleport_threshold, 4)
                + String(
                    ". Each advances by its own forward model, so they "
                    "are different quantities sharing one distance, and a "
                    "rotation rate read against a length teleports on "
                    "noise while a length read against a rate never "
                    "teleports at all. Give each one teleport_threshold() "
                    "in its own units."
                )
        ));
    }
    if (!rate_inheritors.is_empty()) {
        findings.push_back(finding_of(
            "shared_correction_threshold",
            "warning",
            rate_inheritors,
            String("NetwLagCompensation: state properties [")
                + joined(rate_inheritors)
                + String(
                    "] are rate properties, so each is the rate of the "
                    "field it advances, and none declares "
                    "correction_threshold(). They fall back to the entity's "
                )
                + String::num(p_correction_threshold, 4)
                + String(
                    ", which is the tolerance a position was sized for. A "
                    "derivative compared at its own quantity's scale "
                    "reports an error in the wrong units. Give each a "
                    "correction_threshold() at the scale its rate moves."
                )
        ));
    }

    Dictionary breach;
    breach[StringName("response")] = p_pause_on_unpredicted_contact
        ? StringName("DEMOTE")
        : StringName("PREDICT_THROUGH");
    breach[StringName("declared_by")] = p_breach_source;

    Dictionary domain;
    domain[StringName("island")] = p_island_claim == 2
        ? StringName("approximate")
        : (p_island_claim == 1 ? StringName("exact") : StringName("none"));
    domain[StringName("contact_window_ticks")] = p_contact_window_ticks;

    out[StringName("entity")] = p_entity_id;
    out[StringName("schedule")]
        = frame_tier ? StringName("FRAME") : StringName("TICK");
    out[StringName("breach")] = breach;
    out[StringName("domain")] = domain;
    out[StringName("transport")] = p_has_corridor;
    out[StringName("fields")] = fields;
    out[StringName("findings")] = findings;
    return out;
}

bool NetwPredictionEngine::wire_role_timeline(
    int64_t p_slot,
    int p_role,
    bool p_fallback_latched
) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND_V(row == nullptr, false);
    const RID entity = owner_rid(p_slot);
    const bool speculates = p_role == NetwPrediction::ROLE_PREDICT
        || p_role == NetwPrediction::ROLE_SIMULATE
        || (p_role == NetwPrediction::ROLE_REMOTE && p_fallback_latched);
    const bool reads_registry = p_role == NetwPrediction::ROLE_CONSUME
        || p_role == NetwPrediction::ROLE_HOST_LOCAL;
    if (!speculates && !reads_registry) {
        return false;
    }
    if (speculates || register_rewind(entity) >= 0) {
        row->open_lane();
    } else {
        row->close_lane();
    }
    if (p_role == NetwPrediction::ROLE_PREDICT) {
        set_latest_input_tick(p_slot, -1);
    } else if (p_role == NetwPrediction::ROLE_CONSUME) {
        set_next_input_tick(p_slot, -1);
        set_ack(p_slot, -1);
        forget_last_input(p_slot);
    }
    return true;
}

void NetwPredictionEngine::declare_axes(
    int64_t p_slot,
    bool p_authority,
    bool p_controlled_locally,
    bool p_inputless
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->axes.authority = p_authority;
        row->axes.controlled_locally = p_controlled_locally;
        row->axes.inputless = p_inputless;
    }
}

void NetwPredictionEngine::read_parts(
    const predict::Slot &p_row,
    predict::StateRow &r_row
) const {
    NetwMultiplayer *host = p_row.parts.is_empty() ? nullptr : session;
    if (host == nullptr) {
        return;
    }
    const store::Store &world = *store_world;
    const uint32_t fields
        = std::min(uint32_t(p_row.wiring.codec.count()), r_row.values.size());
    for (const predict::StatePart &part : p_row.parts) {
        const store::RowAddress at = world.row_of(part.binding, 0);
        if (!at.valid()) {
            continue;
        }
        const store::Table &table = world.table(at.table);
        if (at.row >= table.row_count() || !table.holds_values(at.row)) {
            continue;
        }
        const int columns = table.schema.column_count();
        for (uint32_t field = part.first;
             field < part.first + part.count && field < fields;
             ++field) {
            const StringName prop
                = StringName(String(p_row.wiring.codec.keys[field])
                                 .substr(part.path.length() + 1));
            for (int column = 0; column < columns; ++column) {
                if (table.schema.at(column)->key == prop) {
                    r_row.set(
                        int(field),
                        store::read_value(table, at.row, uint32_t(column))
                    );
                    break;
                }
            }
        }
    }
}

void NetwPredictionEngine::set_parts(
    int64_t p_slot,
    const LocalVector<predict::StatePart> &p_parts
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    const predict::Feed released;
    for (const predict::StatePart &held : row->parts) {
        bool kept = false;
        for (const predict::StatePart &part : p_parts) {
            kept = kept || part.binding == held.binding;
        }
        repl::PropertyBinding *dropped
            = kept ? nullptr : binding_by_id(held.binding);
        if (dropped != nullptr) {
            dropped->apply_state_feed(released);
        }
    }
    row->parts = p_parts;
    if (row->port.bound()) {
        resolve_reach(*row, resolve_owner(*row));
    }
}

void NetwPredictionEngine::apply_part_feed(
    int64_t p_slot,
    const predict::Feed &p_feed
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return;
    }
    predict::Feed gated;
    gated.state_write_gate = p_feed.state_listener != predict::Listener::STATE
        || p_feed.state_write_gate;
    for (const predict::StatePart &part : row->parts) {
        repl::PropertyBinding *binding = binding_by_id(part.binding);
        if (binding != nullptr) {
            binding->apply_state_feed(gated);
        }
    }
}

void NetwPredictionEngine::reset_for_rewire(
    int64_t p_slot,
    bool p_was_wired,
    bool p_same_stream,
    bool p_raw_fingerprints,
    const predict::StateRow &p_stall
) {
    if (row_of(p_slot) == nullptr) {
        return;
    }
    set_cooldown_until_tick(p_slot, -1);
    predict::Slot *rewired = mutable_row_of(p_slot);
    rewired->compare_stats.fp_verified = 0;
    rewired->compare_stats.fp_mismatches = 0;
    rewired->compare_stats.first_divergent_transition = -1;
    ledger_clear(p_slot);
    set_pending_provenance(p_slot, Dictionary());
    clear_witness_details(p_slot);
    set_open_topology(p_slot, Dictionary());
    set_previous_witness_sleeping(p_slot, false);
    set_has_previous_witness(p_slot, false);
    set_joint_refusal_reported(p_slot, false);
    clear_authority_witness_classes(p_slot);
    drop_deferred_operator(p_slot);
    set_raw_fingerprints(p_slot, p_raw_fingerprints);
    set_latest_input_tick(p_slot, -1);
    set_last_driven_input_tick(p_slot, -1);
    set_last_frame_transition_tick(p_slot, -1);
    set_last_recorded_input_tick(p_slot, -1);
    rewired = mutable_row_of(p_slot);
    rewired->frame_command.resize(rewired->input_codec.count());
    rewired->stall = p_stall;
    set_tape_epoch(p_slot, (tape_epoch_of(p_slot) + 1) & 0xFF);
    set_next_tape_entry_index(p_slot, 0);
    set_last_driven_entry_index(p_slot, -1);
    set_last_recorded_entry_index(p_slot, -1);
    if (!p_same_stream) {
        set_stream_reconstructed(p_slot, false);
        const NetwEntityRecord *seated = owner_record(p_slot);
        if (session != nullptr && seated != nullptr) {
            session->sim_forget_reconstruction(seated->get_handle());
        }
    }
    clear_out_of_domain_window(p_slot);
    clear_environment_epoch(p_slot);
    set_command_epoch(p_slot, -1);
    set_ack_of_acks(p_slot, -1);
    if (p_was_wired) {
        set_ack_domain_confirmed(p_slot, false);
    }
    set_owner_ack_floor(p_slot, -1);
    note_attribution(p_slot, int(predict::Attribution::UNKNOWN), -1);
    set_replay_cursor(p_slot, -1);
    set_last_replayed_label(p_slot, -1);
    set_last_replayed_fresh(p_slot, false);
}

bool NetwPredictionEngine::declared_authority_of(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->axes.authority;
}

bool NetwPredictionEngine::declared_controlled_locally_of(
    int64_t p_slot
) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->axes.controlled_locally;
}

} // namespace netw
