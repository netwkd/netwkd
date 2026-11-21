#include "netw/api/predict_slot_engine.hpp"

#include <algorithm>

#include "godot/callable.hpp"
#include "godot/class_db.hpp"
#include "godot/engine.hpp"
#include "godot/math.hpp"
#include "godot/utility.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/predict_stats.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/scene.hpp"
#include "netw/colliders.hpp"
#include "netw/colors.hpp"
#include "netw/fault_book.hpp"
#include "netw/game_call.hpp"
#include "netw/log.hpp"
#include "netw/predict/axes.hpp"
#include "netw/predict/decl.hpp"
#include "netw/predict/runner.hpp"
#include "netw/predict/tick_work.hpp"
#include "netw/prediction_core.hpp"
#include "netw/profile.hpp"
#include "netw/session/event_plane.hpp"
#include "netw/sim/row.hpp"
#include "netw/stage.hpp"
#include "netw/subsystems.hpp"

using namespace godot;

namespace netw {

using repl::PropertyBindingPtr;

namespace {

constexpr int ARRIVAL_BUCKET_MAX = NetwPredictStats::ARRIVAL_BUCKETS - 1;
constexpr int REPLAY_DEPTH_BUCKET_MAX
    = NetwPredictStats::REPLAY_DEPTH_BUCKETS - 1;

const Dictionary &no_sample() {
    static const Dictionary empty;
    return empty;
}

const PackedStringArray &no_participants() {
    static const PackedStringArray empty;
    return empty;
}

const StringName &field_native_core() {
    static const StringName name("_native_core");
    return name;
}

const StringName &key_label() {
    static const StringName name("label");
    return name;
}

const StringName &key_fresh() {
    static const StringName name("fresh");
    return name;
}

const StringName &key_transition() {
    static const StringName name("transition");
    return name;
}

const StringName &key_tick() {
    static const StringName name("tick");
    return name;
}

const StringName &key_ack() {
    static const StringName name("ack");
    return name;
}

const StringName &key_payload() {
    static const StringName name("payload");
    return name;
}

const StringName &key_whole() {
    static const StringName name("whole");
    return name;
}

const StringName &key_samples() {
    static const StringName name("samples");
    return name;
}

const StringName &key_breach() {
    static const StringName name("breach");
    return name;
}

const StringName &key_witness_fp() {
    static const StringName name("witness_fp");
    return name;
}

const StringName &key_code() {
    static const StringName name("code");
    return name;
}

const StringName &key_fields() {
    static const StringName name("fields");
    return name;
}

bool role_drives_island(int p_role) {
    return p_role == NetwPrediction::ROLE_PREDICT
        || p_role == NetwPrediction::ROLE_CONSUME
        || p_role == NetwPrediction::ROLE_HOST_LOCAL;
}

Ref<NetwEntity> prediction_entity(const Ref<NetwPrediction> &p_handle) {
    return p_handle.is_valid() ? p_handle->get_entity() : Ref<NetwEntity>();
}

Callable released_simulation_step(const Ref<NetwPrediction> &p_handle) {
    const Ref<NetwEntity> entity = prediction_entity(p_handle);
    return entity.is_valid()
        ? NetwMultiplayer::sim_step_of(*entity->get_record(), entity->get_owner())
        : Callable();
}

} // namespace

NetwMultiplayer *NetwPredictSlotEngine::core_seated() const {
    return session;
}

NetwPredictionEngine *NetwPredictSlotEngine::seated_pool() const {
    return pool;
}

bool NetwPredictSlotEngine::is_seated() const {
#if defined(NETW_TESTS)
    NETW_BUG_COND_V(
        record != nullptr && !NetwEntityRecord::is_live(record),
        false
    );
#endif
    return record != nullptr;
}

RID NetwPredictSlotEngine::seated_rid() const {
    return is_seated() ? record->get_handle() : RID();
}

NetwEntityRecord *NetwPredictSlotEngine::held_record() const {
    return is_seated() ? record : nullptr;
}

Node *NetwPredictSlotEngine::seated_node() const {
    return is_seated() ? Object::cast_to<Node>(gd::object_of(node)) : nullptr;
}

const NetwEntityRecord &NetwPredictSlotEngine::seated_record() const {
    static const NetwEntityRecord unseated;
    NETW_BUG_COND_V(!is_seated(), unseated);
    return *record;
}

void NetwPredictSlotEngine::unseat() {
    record = nullptr;
    node = ObjectID();
    decl = nullptr;
}

int64_t NetwPredictSlotEngine::native_slot() const {
    return pool != nullptr ? seat : -1;
}

NetwPredictSlotEngine *NetwPredictSlotEngine::sibling(
    const RID &p_member
) const {
    NetwMultiplayer *seated = core_seated();
    if (seated == nullptr || !p_member.is_valid()) {
        return nullptr;
    }
    NetwPredictSlotEngine *const *held
        = predict_engines->getptr(p_member.get_id());
    return held != nullptr ? *held : nullptr;
}

NetwPredictSlotEngine::Declaration NetwPredictSlotEngine::
    resolved_declaration() const {
    if (is_seated()) {
        Node *owner = seated_node();
        const PropertyBindingPtr state = session->entity_record_binding(
            record,
            owner,
            repl::SET_RECORD_STATE,
            seated_rid()
        );
        const PropertyBindingPtr input = session->entity_record_binding(
            record,
            owner,
            repl::SET_RECORD_INPUT,
            seated_rid()
        );
        const entity::Control *control = record->get_control();
        const int64_t local = owner != nullptr ? session->sim_local_peer() : 0;
        Declaration decided;
        decided.state = state != nullptr ? state->id : 0;
        decided.input = input != nullptr ? input->id : 0;
        decided.authority = session->is_host();
        decided.controlled_locally = control->controlled_by(local)
            || control->runs_ahead()
            || (decided.authority
                && control->resolve() == entity::Control::SERVER);
        return decided;
    }
    return declaration;
}

int NetwPredictSlotEngine::role() const {
    return pool != nullptr ? pool->role_of(native_slot())
                           : int(NetwPrediction::ROLE_REMOTE);
}

void NetwPredictSlotEngine::set_role_column(int p_role) {
    if (pool != nullptr) {
        pool->set_role(native_slot(), p_role);
    }
}

int NetwPredictSlotEngine::correction() const {
    return pool != nullptr ? pool->correction_of(native_slot())
                           : int(CorrectionMode::REPLAY);
}

void NetwPredictSlotEngine::set_correction_column(int p_correction) {
    if (pool != nullptr) {
        pool->set_correction(native_slot(), p_correction);
    }
}

PropertyBindingPtr NetwPredictSlotEngine::state_binding() const {
    return pool != nullptr ? pool->state_binding_of(native_slot())
                           : PropertyBindingPtr();
}

PropertyBindingPtr NetwPredictSlotEngine::input_binding() const {
    return pool != nullptr ? pool->input_binding_of(native_slot())
                           : PropertyBindingPtr();
}

bool NetwPredictSlotEngine::lane_open() const {
    return pool != nullptr && pool->input_lane_open(native_slot());
}

double NetwPredictSlotEngine::tick_delta() const {
    return pool != nullptr ? pool->tick_delta_of(native_slot()) : 1.0 / 60.0;
}

bool NetwPredictSlotEngine::fallback_latched() const {
    return pool != nullptr && pool->fallback_latched_of(native_slot());
}

int64_t NetwPredictSlotEngine::schedule() const {
    const int64_t seat = pool != nullptr ? native_slot() : -1;
    const int seated = seat >= 0 ? pool->schedule_of(seat) : -1;
    if (seated >= 0) {
        return seated;
    }
    if (is_seated()) {
        return NetwMultiplayer::predict_schedule_of(
            seated_record(),
            seated_node()
        );
    }
    const Ref<NetwEntity> entity = prediction_entity(handle);
    return entity.is_valid()
        ? NetwMultiplayer::predict_schedule_of(
              *entity->get_record(),
              entity->get_owner()
          )
        : int(NetwSimulation::SCHEDULE_TICK);
}

int64_t NetwPredictSlotEngine::ack() const {
    return pool != nullptr ? pool->ack_of(native_slot()) : -1;
}

bool NetwPredictSlotEngine::ack_advanced() const {
    return pool != nullptr && pool->ack_advanced_of(native_slot());
}

int64_t NetwPredictSlotEngine::route() const {
    NetwMultiplayer *seated = core_seated();
    if (seated == nullptr || !is_seated()) {
        return -1;
    }
    return liveness_core->route_of(record->get_handle());
}

void NetwPredictSlotEngine::attach(
    Object *p_iface,
    NetwEntityRecord *p_record,
    Node *p_node,
    const Ref<NetwPrediction> &p_handle,
    predict::Decl *p_decl,
    NetwLivenessCore *p_liveness,
    NetwPredictionEngine *p_prediction,
    NetwPredictRunner *p_runner,
    const sim::Rows *p_sims,
    const HashMap<int64_t, NetwPredictSlotEngine *> *p_engines
) {
    NETW_ZONE_NC("Predict attach", colors::PREDICTION);
    if (p_iface == nullptr || p_record == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction did not attach engine %d to entity %d.",
            int(p_iface != nullptr),
            int(p_record != nullptr)
        );
        return;
    }
    NetwMultiplayer *seated = Object::cast_to<NetwMultiplayer>(
        Object::cast_to<Object>(p_iface->get(field_native_core()))
    );
    if (seated == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction did not attach, because the engine has no session."
        );
        return;
    }
    session = seated;
    liveness_core = p_liveness;
    predict_runner = p_runner;
    sim_rows = p_sims;
    predict_engines = p_engines;
    pool = p_prediction;
    record = p_record;
    node = gd::instance_id(p_node);
    seat = pool != nullptr ? pool->slot_of(record) : -1;
    handle = p_handle;
    decl = p_decl;
    if (pool == nullptr || handle.is_null() || decl == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction attached to group %d with handle %d.",
            int(pool != nullptr),
            int(handle.is_valid() && decl != nullptr)
        );
        return;
    }
    rewire_on(resolved_declaration());
}

void NetwPredictSlotEngine::release() {
    NETW_ZONE_NC("Predict release", colors::PREDICTION);
    if (pool == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction released an engine that has no group."
        );
        return;
    }
    refresh_simulation_gate(true);
    clear_island_promotions();
    unregister_from_loop();
    held_recovery.held = false;
    const predict::Feed feed;
    const PropertyBindingPtr state = state_binding();
    if (state != nullptr) {
        state->apply_state_feed(feed);
    }
    const PropertyBindingPtr input = input_binding();
    if (input != nullptr) {
        input->apply_input_feed(feed);
    }
    pool->unbind_owner(native_slot());
    pool->set_parts(native_slot(), LocalVector<predict::StatePart>());
    pool->seat_arrivals(native_slot(), feed, nullptr);
    follows_control = false;
    unseat();
}

void NetwPredictSlotEngine::refresh_simulation_gate(bool p_force_release) {
    pool->refresh_simulation_gate(native_slot(), p_force_release);
}

void NetwPredictSlotEngine::rewire() {
    rewire_on(resolved_declaration());
}

void NetwPredictSlotEngine::publish_topology_roster() {
    pool->publish_topology_roster(native_slot());
}

void NetwPredictSlotEngine::network_process(
    const NetwPredictTiming &p_timing
) {
    NETW_ZONE_LEAF_F(
        "Predict slot tick",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    adopt_timing(p_timing);
    if (schedule() == NetwSimulation::SCHEDULE_TICK) {
        simulate_tick(p_timing);
        return;
    }
    switch (role()) {
        case NetwPrediction::ROLE_PREDICT:
            predict_author_tick(p_timing.get_tick());
            break;
        case NetwPrediction::ROLE_HOST_LOCAL:
            host_local_author_tick(p_timing.get_tick());
            break;
        case NetwPrediction::ROLE_REMOTE:
            if (fallback_latched()) {
                fallback_author_tick(p_timing.get_tick());
            }
            break;
        default:
            break;
    }
}

void NetwPredictSlotEngine::simulate_tick(const NetwPredictTiming &p_timing) {
    adopt_timing(p_timing);
    switch (role()) {
        case NetwPrediction::ROLE_PREDICT:
            predict_step(p_timing.get_delta(), p_timing.get_tick());
            break;
        case NetwPrediction::ROLE_CONSUME:
            consume_step(p_timing.get_delta(), p_timing.get_tick());
            break;
        case NetwPrediction::ROLE_HOST_LOCAL:
            host_local_step(p_timing.get_delta(), p_timing.get_tick());
            break;
        case NetwPrediction::ROLE_REMOTE:
            if (fallback_latched()) {
                fallback_author_step(p_timing.get_tick());
            }
            break;
        case NetwPrediction::ROLE_SIMULATE:
            simulated_step(p_timing.get_delta(), p_timing.get_tick());
            break;
        default:
            break;
    }
}

void NetwPredictSlotEngine::simulate_frame(const NetwPredictTiming &p_timing) {
    NETW_ZONE_LEAF_F(
        "Predict slot frame",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    adopt_timing(p_timing);
    if (schedule() != NetwSimulation::SCHEDULE_FRAME) {
        return;
    }
    simulate_solver(p_timing);
}

void NetwPredictSlotEngine::simulate_stepped(
    const NetwPredictTiming &p_timing
) {
    NETW_ZONE_LEAF_F(
        "Predict slot stepped",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    adopt_timing(p_timing);
    if (schedule() != NetwSimulation::SCHEDULE_STEPPED) {
        return;
    }
    simulate_solver(p_timing);
}

void NetwPredictSlotEngine::simulate_solver(const NetwPredictTiming &p_timing) {
    const int64_t frame_floor
        = pool->last_frame_transition_tick_of(native_slot());
    switch (role()) {
        case NetwPrediction::ROLE_PREDICT:
            if (!p_timing.get_simulating()
                || p_timing.get_tick() <= frame_floor) {
                charge_authoring_clamp();
                send_command_frame();
                return;
            }
            predict_frame_step(p_timing);
            break;
        case NetwPrediction::ROLE_CONSUME:
            consume_frame_step(p_timing);
            break;
        case NetwPrediction::ROLE_HOST_LOCAL:
            host_local_frame_step(p_timing);
            break;
        case NetwPrediction::ROLE_REMOTE:
            if (fallback_latched()) {
                if (p_timing.get_tick() <= frame_floor) {
                    charge_authoring_clamp();
                    send_command_frame();
                    return;
                }
                fallback_author_frame_step(p_timing);
            }
            break;
        case NetwPrediction::ROLE_SIMULATE:
            if (p_timing.get_simulating()) {
                simulated_frame_step(p_timing);
            }
            break;
        default:
            break;
    }
}

void NetwPredictSlotEngine::prepare_island(int p_schedule) {
    if (!pass_admits(p_schedule, schedule()) || !role_drives_island(role())) {
        return;
    }
    pool->refresh_selection(
        native_slot(),
        role() == NetwPrediction::ROLE_PREDICT
    );
}

void NetwPredictSlotEngine::adopt_timing(const NetwPredictTiming &p_timing) {
    const int64_t slot = native_slot();
    if (p_timing.get_ticktime() > 0.0) {
        pool->set_tick_delta(slot, p_timing.get_ticktime());
    }
    pool->set_frame_index(slot, p_timing.get_frame());
    pool->set_declared_quantum(slot, std::max(1, p_timing.get_quantum()));
}

void NetwPredictSlotEngine::finalize_frame_state() {
    NETW_ZONE_LEAF_F(
        "Predict finalize frame state",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    if (schedule() != NetwSimulation::SCHEDULE_FRAME
        || role() != NetwPrediction::ROLE_PREDICT) {
        return;
    }
    finalize_solver_state();
}

void NetwPredictSlotEngine::finalize_stepped_state() {
    NETW_ZONE_LEAF_F(
        "Predict finalize stepped state",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    if (schedule() != NetwSimulation::SCHEDULE_STEPPED) {
        return;
    }
    if (simulated_awaiting_step >= 0) {
        close_captured_row(simulated_awaiting_step);
        simulated_awaiting_step = -1;
    }
    if (role() == NetwPrediction::ROLE_PREDICT) {
        finalize_solver_state();
    }
}

void NetwPredictSlotEngine::finalize_solver_state() {
    const int64_t slot = native_slot();
    const int64_t driven_entry = pool->last_driven_entry_index_of(slot);
    const bool closes = driven_entry > pool->last_recorded_entry_index_of(slot);
    if (closes) {
        close_captured_row(driven_entry);
        pool->set_last_recorded_entry_index(slot, driven_entry);
    }
    const int64_t driven_tick = pool->last_driven_input_tick_of(slot);
    if (driven_tick > pool->last_recorded_input_tick_of(slot)) {
        if (schedule() != NetwSimulation::SCHEDULE_FRAME
            && (!closes || driven_tick != driven_entry)) {
            pool->capture_predicted(slot, driven_tick + 1);
        }
        pool->set_last_recorded_input_tick(slot, driven_tick);
    }
}

bool NetwPredictSlotEngine::runs_in_pass(int p_pass_schedule) const {
    return pass_admits(p_pass_schedule, int(schedule()));
}

void NetwPredictSlotEngine::charge_speculation_hold() {
    if (core_seated() != nullptr) {
        pool->record_speculation_hold(native_slot());
    }
}

void NetwPredictSlotEngine::charge_authoring_clamp() {
    if (core_seated() != nullptr) {
        pool->record_authoring_clamp(native_slot());
    }
}

void NetwPredictSlotEngine::sync_episode() {
    pool->sync_episode(native_slot());
}

bool NetwPredictSlotEngine::defer_operator_for_witness(
    int64_t p_recv_tick,
    int64_t p_basis,
    const predict::StateRow &p_payload
) {
    const int verdict = pool->defer_operator_for_witness(
        native_slot(),
        p_recv_tick,
        p_basis,
        p_payload,
        decl->correction_path.is_valid(),
        NetwMultiplayer::predict_resolve_mode(seated_record(), seated_node())
            == NetwPrediction::MODE_OBSERVE
    );
    if (verdict == NetwPredictionEngine::DEFER_DISPLACED) {
        sync_episode();
    }
    return verdict == NetwPredictionEngine::DEFER_HELD;
}

void NetwPredictSlotEngine::retry_deferred_operator(int64_t p_basis) {
    const int64_t slot = native_slot();
    if (pool->deferred_operator_basis(slot) != p_basis) {
        NETW_TRACE(
            sys::PREDICTION,
            "Tick %d holds no deferred step.",
            int(p_basis)
        );
        return;
    }
    const int64_t recv_tick = pool->deferred_operator_recv_tick(slot);
    predict::StateRow held;
    pool->release_deferred_operator(slot, held);
    on_state(recv_tick, p_basis, nullptr, &held);
}

void NetwPredictSlotEngine::enter_fallback(
    int64_t p_transition,
    int p_attribution,
    bool p_demoted
) {
    NETW_ZONE_NC("Predict enter fallback", colors::PREDICTION);
    const int64_t slot = native_slot();
    const bool stream_was_reconstructed = pool->stream_reconstructed_of(slot);
    pool->set_fallback_latched(slot, true);
    if (core_seated() != nullptr) {
        pool->enter_quarantine(
            slot,
            p_transition,
            stream_was_reconstructed,
            p_attribution,
            p_demoted
        );
    }
    sync_episode();
    pool->announce_episode(slot, EventPlane::EPISODE_FALLBACK);
    if (pool->is_open(slot)) {
        rewire_on(resolved_declaration());
    }
}

void NetwPredictSlotEngine::on_quarantine_state_frame(
    const predict::Arrival &p_arrival
) {
    if (!fallback_latched() || core_seated() == nullptr) {
        return;
    }
    const int64_t slot = native_slot();
    predict::StateRow payload;
    pool->read_arrival(slot, p_arrival, payload);
    const predict::WritePlan plan = pool->quarantine_state(
        slot,
        p_arrival.tick,
        p_arrival.ack,
        payload,
        p_arrival.whole
    );
    sync_episode();
    begin_reseed(plan);
}

void NetwPredictSlotEngine::apply_quarantine_witness(int64_t p_basis) {
    if (!fallback_latched() || core_seated() == nullptr) {
        return;
    }
    const int64_t slot = native_slot();
    const predict::WritePlan plan = pool->quarantine_witness(
        slot,
        p_basis,
        pool->authority_witness_class(slot, p_basis)
    );
    sync_episode();
    begin_reseed(plan);
}

void NetwPredictSlotEngine::begin_reseed(const predict::WritePlan &p_plan) {
    if (p_plan.skip) {
        return;
    }
    const int64_t slot = native_slot();
    const int64_t basis = p_plan.basis;
    restore_row(
        p_plan.write,
        int(predict::Operator::RESEED),
        basis,
        nullptr,
        -1,
        true,
        true
    );
    const Dictionary reseed_provenance
        = pool->pending_provenance(slot).duplicate(true);
    pool->set_fallback_latched(slot, false);
    rewire_on(resolved_declaration());
    pool->set_pending_provenance(native_slot(), reseed_provenance);
    sync_episode();
}

void NetwPredictSlotEngine::finish_reseed_alignment(
    int64_t p_recv_tick,
    int64_t p_ack,
    const predict::StateRow &p_payload
) {
    pool->finish_reseed_alignment(
        native_slot(),
        p_recv_tick,
        p_ack,
        p_payload,
        witness_sample(),
        island_participant_ids(),
        this
    );
}

void NetwPredictSlotEngine::send_command_frame() {
    pool->send_command_frame(native_slot());
}

void NetwPredictSlotEngine::send_ack_frame() {
    pool->send_ack_frame(native_slot());
}

void NetwPredictSlotEngine::receive_relayed_command_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    pool->admit_relayed_payload(native_slot(), p_payload, p_sender);
}

void NetwPredictSlotEngine::receive_command_frame(
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    pool->admit_command_payload(native_slot(), p_payload, p_sender);
}

void NetwPredictSlotEngine::file_command_cell(
    int64_t p_transition,
    const predict::StateRow &p_command,
    int p_origin
) {
    const int64_t slot = native_slot();
    pool->set_newest_matrix_transition(
        slot,
        std::max(pool->newest_matrix_transition_of(slot), p_transition)
    );
    if (core_seated() == nullptr || p_transition < 0
        || pool->command_of(slot) == nullptr) {
        return;
    }
    pool->joint_record(
        slot,
        p_transition,
        predict::StateRow(),
        p_command,
        false,
        p_origin == NetwPrediction::COMMAND_ORIGIN_RELAYED,
        p_origin == NetwPrediction::COMMAND_ORIGIN_PREDICTED
    );
}

void NetwPredictSlotEngine::receive_ack_frame(
    const PackedByteArray &p_payload
) {
    NETW_ZONE_LEAF_F(
        "Predict receive ack",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    if (role() != NetwPrediction::ROLE_PREDICT && !fallback_latched()) {
        NETW_TRACE(
            sys::PREDICTION,
            "Role %d does not take acknowledgements.",
            role()
        );
        return;
    }
    const int64_t slot = native_slot();
    const PackedInt64Array counts = pool->receive_ack_frame(
        slot,
        p_payload,
        pool->tape_epoch_of(slot),
        core_seated() != nullptr,
        this
    );
    NetwPredictStats::Book *stats = &decl->facts;
    const int64_t receipt = counts[NetwPredictionEngine::ACK_RUN_RECEIPT];
    if (receipt == NetwPredictionEngine::ACK_RECEIPT_UNDECODED) {
        stats->set_int_fact(
            NetwPredictStats::FACT_FRAMES_DROPPED_INVALID,
            stats->get_int_fact(NetwPredictStats::FACT_FRAMES_DROPPED_INVALID)
                + 1
        );
        NETW_TRACE(
            sys::PREDICTION,
            "An acknowledgement of %d bytes did not decode.",
            int(p_payload.size())
        );
        return;
    }
    if (receipt == NetwPredictionEngine::ACK_RECEIPT_FOREIGN_EPOCH) {
        NETW_TRACE(
            sys::PREDICTION,
            "An acknowledgement names input history from another epoch."
        );
        return;
    }
    pool->set_ack_domain_confirmed(slot, true);
    stats->set_int_fact(
        NetwPredictStats::FACT_SUBSTITUTED,
        stats->get_int_fact(NetwPredictStats::FACT_SUBSTITUTED)
            + counts[NetwPredictionEngine::ACK_RUN_SUBSTITUTED]
    );
    stats->set_int_fact(
        NetwPredictStats::FACT_ACK_CONFIRMED,
        counts[NetwPredictionEngine::ACK_RUN_OF_ACKS]
    );
    publish_ack_age(counts[NetwPredictionEngine::ACK_RUN_AGE]);
}

void NetwPredictSlotEngine::finalize_recorded_state() {
    NETW_ZONE_LEAF_F(
        "Predict finalize recorded state",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION,
        "entity %lld",
        static_cast<long long>(route())
    );
    if (role() == NetwPrediction::ROLE_PREDICT) {
        return;
    }
    const int64_t slot = native_slot();
    const int64_t transition = recorded_transition();
    if (transition >= 0) {
        pool->capture_into_book(slot, transition + 1);
    }
    const int64_t fingerprint = pool->book_fingerprint(slot, transition + 1);
    close_journal_row(transition);
    judge_owner_claim(transition, fingerprint);
}

void NetwPredictSlotEngine::judge_owner_claim(
    int64_t p_transition,
    int64_t p_fingerprint
) {
    pool->report_owner_claim(native_slot(), p_transition, p_fingerprint);
}

int64_t NetwPredictSlotEngine::recorded_transition() const {
    if (schedule() != NetwSimulation::SCHEDULE_TICK
        && role() == NetwPrediction::ROLE_CONSUME) {
        return ack();
    }
    return decl->facts.get_int_fact(
        NetwPredictStats::FACT_LAST_DRIVE_LABEL
    );
}

predict::SlotCursors NetwPredictSlotEngine::cursors() const {
    predict::SlotCursors read;
    read.role = role();
    read.schedule = int(schedule());
    read.ack = ack();
    read.ack_advanced = ack_advanced();
    if (pool != nullptr) {
        const int64_t slot = native_slot();
        read.last_replayed_label = pool->last_replayed_label_of(slot);
        read.last_replayed_fresh = pool->last_replayed_fresh_of(slot);
        read.last_driven_input_tick = pool->last_driven_input_tick_of(slot);
    }
    return read;
}

int64_t NetwPredictSlotEngine::order_key() const {
    return predict::order_key_for_route(route());
}

int64_t NetwPredictSlotEngine::history_record_tick(
    int64_t p_fallback_tick
) const {
    return predict::history_record_tick(cursors(), p_fallback_tick);
}

bool NetwPredictSlotEngine::consumed_unslotted_transition() const {
    return predict::consumed_unslotted_transition(cursors());
}

bool NetwPredictSlotEngine::has_consumed_state_tick(
    int64_t p_state_tick
) const {
    return predict::has_consumed_state_tick(cursors(), p_state_tick);
}

int NetwPredictSlotEngine::declared_correction() const {
    return decl != nullptr
        ? predict::correction_for_mode(decl->mode)
        : int(CorrectionMode::AUTO);
}

int NetwPredictSlotEngine::resolve_correction() const {
    if (decl == nullptr) {
        const Ref<NetwEntity> entity = prediction_entity(handle);
        const NetwEntityRecord *record
            = entity.is_valid() ? entity->get_record() : nullptr;
        const predict::Decl *released
            = record != nullptr ? record->find_predict_declaration() : nullptr;
        if (released != nullptr) {
            const int64_t slot = native_slot();
            if (core_seated() == nullptr || slot < 0) {
                return predict::correction_for_mode(
                    NetwMultiplayer::predict_resolve_mode(
                        *record,
                        entity->get_owner()
                    )
                );
            }
            return pool->settle_correction(
                slot,
                predict::correction_for_mode(released->mode)
            );
        }
        return int(CorrectionMode::REPLAY);
    }
    const int64_t slot = native_slot();
    if (core_seated() == nullptr || slot < 0) {
        return predict::correction_for_mode(
            NetwMultiplayer::predict_resolve_mode(
                seated_record(),
                seated_node()
            )
        );
    }
    return pool->settle_correction(slot, declared_correction());
}

void NetwPredictSlotEngine::on_reparented() {
    rewire_on(resolved_declaration());
}

void NetwPredictSlotEngine::rewire_on(const Declaration &p_declaration) {
    NETW_ZONE_NC("Predict rewire", colors::PREDICTION);
    declaration = p_declaration;
    if (seated_node() == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction found no owning node, so it stays unbound."
        );
        return;
    }
    const PropertyBindingPtr state = pool->binding_by_id(p_declaration.state);
    const PropertyBindingPtr input = pool->binding_by_id(p_declaration.input);
    if (state == nullptr) {
        NETW_TRACE(
            sys::PREDICTION,
            "Prediction found no state binding, so it stays unbound."
        );
        return;
    }
    const int64_t slot = native_slot();
    const PropertyBindingPtr seated = state_binding();
    const bool was_wired = seated != nullptr;
    const bool same_stream = seated == state;
    pool->bind_property_sets(slot, state, input);
    pool->declare_axes(
        slot,
        p_declaration.authority,
        p_declaration.controlled_locally,
        input == nullptr
    );
    pool->set_declared_epoch(slot, decl->world_version);

    unregister_from_loop();
    held_recovery.held = false;
    pool->close_lane(slot);
    pool->reset_for_rewire(
        slot,
        was_wired,
        same_stream,
        pool->gathers_owner_evidence(),
        pool->bound_input(slot)
    );
    sync_episode();
    publish_topology_roster();
    NetwPredictStats::Book *stats = &decl->facts;
    stats->set_int_fact(NetwPredictStats::FACT_COMMAND_QUEUE_DEPTH, 0);
    stats->set_int_fact(NetwPredictStats::FACT_ACK_CONFIRMED, -1);
    stats->set_int_fact(NetwPredictStats::FACT_TAPE_EPOCH, -1);
    stats->set_int_fact(NetwPredictStats::FACT_TAPE_INDEX, -1);
    stats->set_int_fact(NetwPredictStats::FACT_TAPE_QUEUE_DEPTH, 0);

    NetwMultiplayer *session = core_seated();
    if (session != nullptr) {
        adopt_timing(session->frame_timing());
    }
    const int resolved_role = resolve_axes();
    refresh_simulation_gate(false);
    set_role_column(resolved_role);
    const bool leading = session != nullptr ? sim::leads(resolved_mode())
                                            : role_drives_island(resolved_role);
    if (!leading) {
        clear_island_promotions();
    } else if (resolved_role != NetwPrediction::ROLE_PREDICT) {
        pool->clear_joint_roster(slot);
    }
    set_correction_column(resolve_correction());
    pool->seed_recovery_ledger(slot);
    if (session != nullptr) {
        const int resolved = pool->adopt_declaration(
            slot,
            state,
            input_binding(),
            NetwMultiplayer::predict_schedule_of(
                seated_record(),
                seated_node()
            ),
            resolved_role,
            declared_correction(),
            NetwMultiplayer::sim_snap_restore_of(seated_record()),
            NetwMultiplayer::sim_max_restore_ticks_of(seated_record()),
            pool_island(),
            pool->slot_kind(slot).samples_contacts
        );
        if (resolved >= 0) {
            set_correction_column(resolved);
        }
        pool->tape_reset(slot, pool->tape_epoch_of(slot));
        reconfigure();
    }
    if (session != nullptr && p_declaration.controlled_locally) {
        pool->validate_declaration(slot, state->get_set(), *this);
    }
    const predict::Feed feed
        = pool->role_feed(resolved_role, fallback_latched());
    if (pool->wire_role_timeline(slot, resolved_role, fallback_latched())) {
        register_with_loop();
    }
    state->apply_state_feed(feed);
    pool->apply_part_feed(slot, feed);
    if (input != nullptr) {
        input->apply_input_feed(feed);
    }
    pool->seat_arrivals(slot, feed, this);
    if (session != nullptr) {
        session->display_mark_role_dirty(seated_rid());
        session->sim_announce(seated_rid());
        const sim::Row *selection = sim_rows->row_of(seated_rid());
        pool->follow_relay_subscription(
            slot,
            selection != nullptr && selection->selected_by.count() > 0
        );
    }
}

void NetwPredictSlotEngine::reconfigure() {
    if (core_seated() != nullptr) {
        pool->admit_rollback_together(native_slot());
    }
}

void NetwPredictSlotEngine::emit_reachability_findings(
    const Array &p_findings
) {
    Node *owner = seated_node();
    const String named = owner != nullptr ? String(owner->get_name())
                                          : String("an entity with no node");
    const uint64_t subject
        = owner != nullptr ? uint64_t(gd::instance_id(owner)) : 0;
    for (int64_t at = 0; at < p_findings.size(); ++at) {
        const Dictionary finding = p_findings[at];
        const String code = String(finding.get(key_code(), StringName()));
        const PackedStringArray fields
            = finding.get(key_fields(), PackedStringArray());
        const String listed = String(", ").join(fields);
        if (code == "uncompared") {
            for (const String &field : fields) {
                fault::raise(
                    core_seated(),
                    NETW_FAULT_CONFLICTING_SETTINGS,
                    {"{NetwPropertyConfig.correction_threshold}",
                     field,
                     "{NetwPropertyConfig.unchecked}"},
                    subject
                );
            }
        } else if (code == "unrepairable") {
            NETW_INFO(
                sys::PREDICTION,
                "'%s' has teleport_only properties [%s] that can cause a "
                "correction, and only a teleport writes them.",
                named,
                listed
            );
        } else if (code == "shared_teleport_threshold") {
            fault::raise(
                core_seated(),
                NETW_FAULT_THRESHOLD_SHARED,
                {listed, named, String(), 0, NETW_SHARED_TELEPORT},
                subject
            );
        } else if (code == "shared_correction_threshold") {
            fault::raise(
                core_seated(),
                NETW_FAULT_THRESHOLD_SHARED,
                {listed, named, String(), 0, NETW_SHARED_CORRECTION},
                subject
            );
        }
    }
}

int NetwPredictSlotEngine::pool_island() const {
    if (pool->rolls_back_together(native_slot())) {
        return NetwPredictionEngine::ISLAND_JOINT;
    }
    NetwMultiplayer *session = core_seated();
    const sim::Row *row = session != nullptr && is_seated()
        ? sim_rows->row_of(seated_rid())
        : nullptr;
    const bool chosen = session != nullptr && is_seated()
        && session->sim_chooses(seated_rid());
    const bool selected = row != nullptr && row->selected_by.count() > 0
        && !sim::leads(row->mode);
    if (chosen || selected) {
        return NetwPredictionEngine::ISLAND_DECLARED;
    }
    return NetwPredictionEngine::ISLAND_NONE;
}

sim::Mode NetwPredictSlotEngine::resolved_mode() const {
    NetwMultiplayer *session = core_seated();
    const sim::Row *row = session != nullptr && is_seated()
        ? sim_rows->row_of(seated_rid())
        : nullptr;
    return row != nullptr ? row->mode : sim::Mode::NONE;
}

int NetwPredictSlotEngine::resolve_axes() {
    const sim::Facts facts = pool->resolution_facts(native_slot(), decl);
    NetwMultiplayer *session = core_seated();
    if (session == nullptr || !is_seated()) {
        return pool->apply_mode(native_slot(), decl, sim::resolve(facts));
    }
    const RID seated = seated_rid();
    const sim::Mode mode = session->sim_resolve(seated, facts);
    session->sim_settle_body_of(seated);
    return pool->apply_mode(native_slot(), decl, mode);
}

void NetwPredictSlotEngine::notify_collision() {
    pool->notify_collision(
        native_slot(),
        pool->samples_contacts(native_slot()),
        decl->correction_cooldown_ticks
    );
}

void NetwPredictSlotEngine::admit_rollback_together() {
    pool->admit_rollback_together(native_slot());
}

void NetwPredictSlotEngine::clear_island_promotions() {
    pool->clear_island_promotions(native_slot());
}

void NetwPredictSlotEngine::predict_author_tick(int64_t p_tick) {
    const int64_t slot = native_slot();
    const int64_t latest = pool->latest_input_tick_of(slot);
    if (latest > pool->last_driven_input_tick_of(slot)) {
        if (lane_open()) {
            pool->capture_predicted(slot, latest + 1);
        } else {
            NETW_TRACE(
                sys::PREDICTION,
                "Tick %d was authored with no input history.",
                int(latest)
            );
        }
        pool->set_last_recorded_input_tick(slot, latest);
    }
    if (!pool->author_command(slot, p_tick)) {
        return;
    }
    pool->hold_frame_command(slot);
}

void NetwPredictSlotEngine::predict_frame_step(
    const NetwPredictTiming &p_timing
) {
    NETW_ZONE_LEAF(
        "Predict frame step",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const int64_t slot = native_slot();
    pool->set_last_frame_transition_tick(slot, p_timing.get_tick());
    if (speculation_horizon_full()) {
        charge_speculation_hold();
        send_command_frame();
        NETW_TRACE(
            sys::PREDICTION,
            "Tick %d is further ahead than prediction may run.",
            int(p_timing.get_tick())
        );
        return;
    }
    const DriveOutcome drove = pool->record_drive_hashed(
        slot,
        pool->next_tape_entry_index_of(slot),
        -1,
        int(NetwPrediction::DRIVE_KIND_NONE),
        0,
        p_timing.get_tick(),
        false,
        true
    );
    if (!drove.ran) {
        send_command_frame();
        NETW_TRACE(
            sys::PREDICTION,
            "Tick %d started no step.",
            int(p_timing.get_tick())
        );
        return;
    }
    const int64_t label = drove.label;
    const bool fresh = drove.fresh;
    const int64_t transition = drove.transition;
    const predict::StateRow &command = *pool->frame_command_of(slot);
    pool->record_authored_command(slot, transition, label, fresh, command);
    adopt_tape_position(transition);
    run_command(command, p_timing.get_delta(), label, fresh);
    if (fresh) {
        pool->set_last_driven_input_tick(slot, label);
    }
    send_command_frame();
}

Fold NetwPredictSlotEngine::drive_fold(int64_t p_frame_tick) {
    const int64_t slot = native_slot();
    return prediction_core::fold(
        pool->latest_input_tick_of(slot),
        pool->last_driven_input_tick_of(slot),
        p_frame_tick
    );
}

int NetwPredictSlotEngine::consume_action(int p_depth, int p_buffer) {
    return prediction_core::consume_action(p_depth, p_buffer);
}

void NetwPredictSlotEngine::fallback_author_tick(int64_t p_tick) {
    predict_author_tick(p_tick);
}

void NetwPredictSlotEngine::fallback_author_frame_step(
    const NetwPredictTiming &p_timing
) {
    const int64_t slot = native_slot();
    pool->set_last_frame_transition_tick(slot, p_timing.get_tick());
    const Fold fold = drive_fold(p_timing.get_tick());
    author_command_entry(fold.label, fold.fresh);
    const predict::StateRow *held = pool->frame_command_of(slot);
    if (held != nullptr) {
        pool->record_authored_command(
            slot,
            pool->last_driven_entry_index_of(slot),
            fold.label,
            fold.fresh,
            *held
        );
    }
    if (fold.fresh) {
        pool->set_last_driven_input_tick(slot, fold.label);
    }
    send_command_frame();
}

void NetwPredictSlotEngine::fallback_author_step(int64_t p_tick) {
    const int64_t slot = native_slot();
    if (!pool->author_command(slot, p_tick)) {
        return;
    }
    prepare_tick_tape(p_tick);
    author_command_entry(p_tick, true);
    pool->set_last_driven_input_tick(slot, p_tick);
    send_command_frame();
}

void NetwPredictSlotEngine::predict_step(double p_delta, int64_t p_tick) {
    NETW_ZONE_LEAF(
        "Predict step",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const int64_t slot = native_slot();
    if (!pool->author_command(slot, p_tick)) {
        return;
    }
    if (speculation_horizon_full()) {
        charge_speculation_hold();
        send_command_frame();
        NETW_TRACE(
            sys::PREDICTION,
            "Tick %d is further ahead than prediction may run.",
            int(p_tick)
        );
        return;
    }
    prepare_tick_tape(p_tick);
    adopt_tape_position(p_tick);
    pool->set_last_driven_input_tick(slot, p_tick);
    pool->record_drive_hashed(
        slot,
        p_tick,
        p_tick,
        NetwPrediction::DRIVE_KIND_FRESH,
        0,
        p_tick,
        true,
        true,
        true
    );
    run_command(*pool->command_of(slot), p_delta, p_tick, true);
    close_captured_row(p_tick);
    send_command_frame();
}

void NetwPredictSlotEngine::simulated_step(double p_delta, int64_t p_tick) {
    const int64_t slot = native_slot();
    if (!pool->predict_command(slot, p_tick)) {
        return;
    }
    const bool joint = pool->rolls_back_together(slot);
    if (joint) {
        pool->record_drive_hashed(
            slot,
            p_tick,
            p_tick,
            NetwPrediction::DRIVE_KIND_SUBSTITUTED,
            pool->command_hash_row(slot, *pool->command_of(slot)),
            p_tick,
            false
        );
        if (pool->joint_provenance_at(slot, p_tick)
            != int(predict::CellProvenance::RELAYED)) {
            file_command_cell(
                p_tick,
                *pool->command_of(slot),
                NetwPrediction::COMMAND_ORIGIN_PREDICTED
            );
        }
    } else {
        mark_idle_drive(p_tick, NetwPrediction::DRIVE_KIND_SUBSTITUTED);
    }
    if (pool->command_of(slot) == nullptr) {
        return;
    }
    run_command(*pool->command_of(slot), p_delta, p_tick, false);
    NetwPredictStats::Book *stats = &decl->facts;
    stats->set_int_fact(
        NetwPredictStats::FACT_SUBSTITUTED,
        stats->get_int_fact(NetwPredictStats::FACT_SUBSTITUTED) + 1
    );
    if (!joint) {
        return;
    }
    if (schedule() == NetwSimulation::SCHEDULE_STEPPED) {
        simulated_awaiting_step = p_tick;
        return;
    }
    close_captured_row(p_tick);
}

void NetwPredictSlotEngine::simulated_frame_step(
    const NetwPredictTiming &p_timing
) {
    const int64_t slot = native_slot();
    if (p_timing.get_tick() <= pool->last_frame_transition_tick_of(slot)) {
        return;
    }
    pool->set_last_frame_transition_tick(slot, p_timing.get_tick());
    simulated_step(p_timing.get_delta(), p_timing.get_tick());
}

void NetwPredictSlotEngine::on_simulated_state_frame(
    const predict::Arrival &p_arrival
) {
    pool->admit_simulated_state(native_slot(), p_arrival);
}

void NetwPredictSlotEngine::prepare_tick_tape(int64_t p_tick) {
    pool->prepare_tick_tape(native_slot(), p_tick);
}

bool NetwPredictSlotEngine::speculation_horizon_full() {
    return pool->speculation_horizon_full(native_slot());
}

void NetwPredictSlotEngine::publish_ack_age(int64_t p_age) {
    if (p_age >= 0) {
        decl->ack_age_ticks = int(p_age);
    }
}

void NetwPredictSlotEngine::on_state_frame(const predict::Arrival &p_arrival) {
    const int64_t slot = native_slot();
    pool->set_stream_reconstructed(
        slot,
        pool->stream_reconstructed_of(slot) || p_arrival.whole
    );
    pool->joint_note_world_tick(slot, p_arrival.tick, p_arrival.ack);
    on_state(p_arrival.tick, p_arrival.ack, &p_arrival, nullptr);
}

void NetwPredictSlotEngine::on_state(
    int64_t p_recv_tick,
    int64_t p_ack,
    const predict::Arrival *p_arrival,
    const predict::StateRow *p_held
) {
    NETW_ZONE_LEAF(
        "Predict state comparison",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const int64_t slot = native_slot();
    const OpenedComparison opened = pool->open_state_comparison(
        slot,
        p_recv_tick,
        p_ack,
        p_arrival,
        p_held
    );
    if (opened.realign) {
        predict::StateRow payload;
        if (p_arrival != nullptr) {
            pool->read_arrival(slot, *p_arrival, payload);
        } else if (p_held != nullptr) {
            payload = *p_held;
        }
        finish_reseed_alignment(p_recv_tick, p_ack, payload);
        return;
    }
    if (!opened.opened) {
        NETW_TRACE(
            sys::PREDICTION,
            "The group made no comparison at acknowledgement %d.",
            int(p_ack)
        );
        return;
    }
    const int64_t ack_label = opened.label;
    const int domain = opened.domain;
    const int64_t episode_state_before = opened.episode_state;
    const bool probation_before = opened.probation;
    const bool reconstructed = opened.reconstructed;
    double divergence = 0.0;
    bool corrected = false;
    bool settled = false;
    int meter = 0;
    const auto announce = [&]() {
        pool->announce_evaluation(
            handle.ptr(),
            slot,
            p_recv_tick,
            p_ack,
            divergence,
            corrected
        );
    };
    if (reconstructed) {
        const predict::Judged judged = pool->judge_state(
            slot,
            p_ack,
            domain,
            opened.row_flags,
            pool->compared_row(slot),
            pool->received_row(slot)
        );
        divergence = judged.divergence;
        corrected = judged.corrected;
        settled = judged.settled;
        meter = judged.meter;
    }
    const int attribution = pool->attribution_for(slot, p_ack);
    if (pool->settle_comparison(
            slot,
            p_recv_tick,
            p_ack,
            reconstructed,
            corrected,
            settled,
            divergence,
            probation_before,
            episode_state_before,
            *this
        )
        != NetwPredictionEngine::SETTLE_PROCEED) {
        return;
    }
    if (!corrected) {
        if (core_seated() != nullptr) {
            pool->trim_history(slot, p_ack);
        }
        announce();
        return;
    }
    const predict::StateRow &payload = pool->received_row(slot);
    if (defer_operator_for_witness(p_recv_tick, p_ack, payload)) {
        pool->note_verdict_reason(
            slot,
            predict::VERDICT_REASON_WITNESS_DEFERRED
        );
        announce();
        return;
    }
    if (pool->rolls_back_together(slot)) {
        pool->note_joint_basis(
            slot,
            p_ack,
            payload,
            NetwPredictionEngine::JOINT_FLOOR_STATE
        );
        NetwPredictStats::Book *stats = &decl->facts;
        stats->set_int_fact(
            NetwPredictStats::FACT_CORRECTIONS,
            stats->get_int_fact(NetwPredictStats::FACT_CORRECTIONS) + 1
        );
    } else if (Engine::get_singleton()->is_in_physics_frame()) {
        held_recovery.held = false;
        recover(
            p_ack,
            ack_label,
            pool->compared_row(slot),
            payload,
            meter,
            domain,
            attribution
        );
        announce();
        return;
    } else {
        held_recovery.held = true;
        held_recovery.ack = p_ack;
        held_recovery.label = ack_label;
        held_recovery.meter = meter;
        held_recovery.domain = domain;
        held_recovery.attribution = attribution;
        held_recovery.compared = pool->compared_row(slot);
        held_recovery.payload = payload;
        announce();
        return;
    }
    if (core_seated() != nullptr) {
        pool->trim_history(slot, p_ack);
    }
    announce();
}

void NetwPredictSlotEngine::recover(
    int64_t p_ack,
    int64_t p_label,
    const predict::StateRow &p_compared,
    const predict::StateRow &p_payload,
    int p_meter,
    int p_domain,
    int p_attribution
) {
    const int64_t slot = native_slot();
    const bool replays = pool->recovery_replays(slot);
    if (!pool->run_recovery_ladder(
            slot,
            p_ack,
            p_label,
            p_compared,
            p_payload,
            p_meter,
            p_domain,
            p_attribution,
            replays ? witness_sample() : no_sample(),
            replays ? island_participant_ids() : no_participants(),
            replays ? this : nullptr
        )) {
        return;
    }
    if (core_seated() != nullptr) {
        pool->trim_history(slot, p_ack);
    }
}

void NetwPredictSlotEngine::run_held_recovery() {
    if (!held_recovery.held) {
        return;
    }
    held_recovery.held = false;
    if (core_seated() == nullptr || pool->rolls_back_together(native_slot())) {
        return;
    }
    NETW_ZONE_LEAF(
        "Predict held recovery",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    recover(
        held_recovery.ack,
        held_recovery.label,
        held_recovery.compared,
        held_recovery.payload,
        held_recovery.meter,
        held_recovery.domain,
        held_recovery.attribution
    );
}

bool NetwPredictSlotEngine::is_steppable() const {
    return pool->slot_is_steppable(native_slot());
}

void NetwPredictSlotEngine::joint_pass(const NetwPredictTiming &p_timing) {
    const int64_t present = p_timing.get_tick() - 1;
    if (!pool->rolls_back_together(native_slot())
        || !role_drives_island(role()) || core_seated() == nullptr
        || present < 0) {
        return;
    }
    NETW_ZONE_LEAF(
        "Predict slot joint pass",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const predict::ReplayPlan plan = pool_joint_pass(present);
    if (plan.floor >= 0) {
        core_seated()->sim_release_lingering(seated_rid(), plan.floor);
    }
    if (!plan.valid) {
        NETW_TRACE(
            sys::PREDICTION,
            "The group replay at tick %d has no plan.",
            int(p_timing.get_tick())
        );
        return;
    }
    const LocalVector<NetwPredictSlotEngine *> members = joint_group();
    const int64_t floor_transition = plan.floor;
    if (plan.heal) {
        for (uint32_t at = 0; at < members.size(); ++at) {
            members[at]->joint_heal();
        }
    } else {
        run_replay(plan, members);
    }
    release_lingering(floor_transition);
    for (uint32_t at = 0; at < members.size(); ++at) {
        const int64_t seat = members[at]->native_slot();
        pool->set_joint_basis(seat, -1);
        pool->set_joint_relay_floor(seat, -1);
        pool->set_joint_epoch_floor(seat, -1);
    }
}

LocalVector<NetwPredictSlotEngine *> NetwPredictSlotEngine::joint_group() {
    LocalVector<NetwPredictSlotEngine *> out;
    out.push_back(this);
    const int64_t slot = native_slot();
    const LocalVector<RID> simulated
        = pool->roster_list(slot, NetwPredictionEngine::ROSTER_SIMULATED);
    for (const RID &member : simulated) {
        append_joint_member(out, member);
    }
    const LocalVector<RID> lingering
        = pool->roster_list(slot, NetwPredictionEngine::ROSTER_JOINT_LINGERING);
    for (const RID &member : lingering) {
        if (!simulated.has(member)) {
            append_joint_member(out, member);
        }
    }
    std::stable_sort(
        out.ptr(),
        out.ptr() + out.size(),
        [](const NetwPredictSlotEngine *a, const NetwPredictSlotEngine *b) {
            return a->order_key() < b->order_key();
        }
    );
    return out;
}

void NetwPredictSlotEngine::append_joint_member(
    LocalVector<NetwPredictSlotEngine *> &r_out,
    const RID &p_member
) {
    NetwPredictSlotEngine *member = sibling(p_member);
    if (member == nullptr || member == this || !member->is_steppable()) {
        NETW_TRACE(
            sys::PREDICTION,
            "A member of a touching group cannot be replayed here."
        );
        return;
    }
    r_out.push_back(member);
}

NetwPredictSlotEngine::SteppedIsland NetwPredictSlotEngine::
    stepped_island() const {
    SteppedIsland out;
    NetwMultiplayer *host = core_seated();
    if (host == nullptr || schedule() != NetwSimulation::SCHEDULE_STEPPED) {
        return out;
    }
    const NetwMultiplayer::EntitySpace held
        = host->entity_space_of(seated_node());
    out.space = held.space;
    out.dimension = held.dimension;
    out.steps = host->predict_space_can_step(held.space, held.dimension);
    out.stepper = host->predict_get_stepper(out.space);
    return out;
}

void NetwPredictSlotEngine::step_island(
    const SteppedIsland &p_island,
    int64_t p_transition
) {
    NetwMultiplayer *host = core_seated();
    if (!p_island.steps || host == nullptr) {
        return;
    }
    host->predict_step_space(
        p_island.space,
        tick_delta(),
        p_transition,
        p_island.dimension
    );
}

void NetwPredictSlotEngine::close_replay_step(
    const predict::ReplayPlan &p_plan,
    const predict::ReplayStep &p_step
) {
    const bool fresh = (p_plan.flags & predict::REPLAY_SAMPLES_EACH_CLOSE) != 0;
    const Dictionary sample = fresh ? witness_sample() : p_plan.sample;
    const PackedStringArray participants
        = fresh ? island_participant_ids() : p_plan.participants;
    Dictionary solve;
    if ((p_step.flags & predict::STEP_CLOSES_ENTRY) != 0) {
        solve = pool->close_replayed_entry(
            p_step.slot,
            p_step.entry,
            sample,
            participants
        );
    } else {
        pool->capture_into_book(p_step.slot, p_step.entry + 1);
        if ((p_step.flags & predict::STEP_SEALS) != 0) {
            solve = pool->seal_transition(
                p_step.slot,
                p_step.entry,
                sample,
                participants
            );
        }
    }
    if (!solve.is_empty() && (p_plan.flags & predict::REPLAY_DEMOTES) != 0) {
        maybe_demote_for_breach(p_step.entry, solve);
    }
}

void NetwPredictSlotEngine::run_replay(
    const predict::ReplayPlan &p_plan,
    const LocalVector<NetwPredictSlotEngine *> &p_members
) {
    NETW_STAGE(ROLLBACK, "Predict replay plan", colors::PREDICTION);
    const bool rebases = (p_plan.flags & predict::REPLAY_REBASES) != 0;
    HashMap<int64_t, NetwPredictSlotEngine *> by_slot;
    for (uint32_t at = 0; at < p_members.size(); ++at) {
        const int64_t seat = p_members[at]->native_slot();
        if (seat >= 0) {
            by_slot.insert(seat, p_members[at]);
        }
    }
    LocalVector<NetwPredictSlotEngine *> stepped;
    LocalVector<predict::StateRow> drawn;
    if (!rebases) {
        for (NetwPredictSlotEngine *member : p_members) {
            stepped.push_back(member);
            pool->hold_live_input(member->native_slot());
        }
    }
    for (int at = 0; rebases && at < int(p_plan.restores.size()); ++at) {
        const int64_t restore_slot = p_plan.restores[uint32_t(at)].slot;
        NetwPredictSlotEngine **held = by_slot.getptr(restore_slot);
        if (held == nullptr) {
            NETW_TRACE(
                sys::PREDICTION,
                "The replay plan restores entry %d, which holds nothing.",
                int(restore_slot)
            );
            continue;
        }
        NetwPredictSlotEngine *member = *held;
        stepped.push_back(member);
        drawn.push_back(predict::StateRow());
        pool->capture_canonical(member->native_slot(), drawn[drawn.size() - 1]);
        pool->hold_live_input(member->native_slot());
        member->restore_row(
            p_plan.restores[uint32_t(at)].state,
            int(predict::Operator::JOINT_REBASE),
            p_plan.floor,
            this,
            p_plan.restores[uint32_t(at)].entry
        );
    }
    if (stepped.is_empty()) {
        return;
    }
    LocalVector<predict::StateRow> before;
    LocalVector<int> ran;
    before.resize(stepped.size());
    for (uint32_t at = 0; at < stepped.size(); ++at) {
        if (rebases) {
            pool->capture_canonical(stepped[at]->native_slot(), before[at]);
        }
        ran.push_back(0);
    }
    const SteppedIsland island
        = p_plan.floor >= 0 ? stepped_island() : SteppedIsland();
    LocalVector<sim::HeldPose> bystanders;
    if (island.steps) {
        LocalVector<RID> replayed;
        for (NetwPredictSlotEngine *member : stepped) {
            replayed.push_back(member->seated_rid());
        }
        core_seated()->sim_hold_poses(island.space, replayed, bystanders);
        if (island.stepper.is_valid()) {
            island.stepper->restore(island.space, p_plan.floor);
        }
    }
    struct Awaiting {
        NetwPredictSlotEngine *member = nullptr;
        uint32_t step = 0;
    };
    int64_t open_entry = -1;
    LocalVector<Awaiting> awaiting_step;
    const auto step_and_close = [&](int64_t p_entry) {
        step_island(island, p_entry);
        for (const Awaiting &ran_step : awaiting_step) {
            ran_step.member->close_replay_step(
                p_plan,
                p_plan.steps[ran_step.step]
            );
        }
        awaiting_step.clear();
    };
    predict::StateRow read;
    for (int at = 0; at < int(p_plan.steps.size()); ++at) {
        const predict::ReplayStep &step = p_plan.steps[uint32_t(at)];
        NetwPredictSlotEngine **held = by_slot.getptr(step.slot);
        if (held == nullptr) {
            NETW_TRACE(
                sys::PREDICTION,
                "The replay plan steps entry %d, which holds nothing.",
                int(step.slot)
            );
            continue;
        }
        NetwPredictSlotEngine *member = *held;
        if (open_entry >= 0 && step.entry != open_entry) {
            step_and_close(open_entry);
            open_entry = -1;
            if (!island.steps
                && (p_plan.flags & predict::REPLAY_SETTLES) != 0) {
                for (NetwPredictSlotEngine *settled : stepped) {
                    colliders::settle(settled->seated_node());
                }
            }
        }
        const predict::StateRow *command = &step.command;
        if ((step.flags & predict::STEP_READS_INPUT) != 0) {
            if (!pool->authored_command(step.slot, step.input_key, read)) {
                break;
            }
            command = &read;
        }
        open_entry = step.entry;
        if ((p_plan.flags & predict::REPLAY_RUNS_THROUGH_SEAT) != 0) {
            member->run_command(
                *command,
                member->tick_delta(),
                step.tick,
                false
            );
        } else {
            pool->run_command(
                step.slot,
                *command,
                pool->tick_delta_of(step.slot),
                step.tick,
                false
            );
        }
        if (island.steps) {
            awaiting_step.push_back({member, uint32_t(at)});
        } else {
            member->close_replay_step(p_plan, step);
        }
        for (uint32_t seat = 0; seat < stepped.size(); ++seat) {
            if (stepped[seat] == member) {
                ran[seat] += 1;
            }
        }
    }
    if (open_entry >= 0) {
        step_and_close(open_entry);
    }
    sim::restore_poses(bystanders);
    const int64_t present = p_plan.present;
    for (uint32_t at = 0; at < stepped.size(); ++at) {
        pool->restore_live_input(stepped[at]->native_slot());
        if (!rebases) {
            continue;
        }
        pool->note_replay_depth(stepped[at]->native_slot(), ran[at]);
        stepped[at]->note_joint_writes(before[at]);
        pool->announce_joint_moved(
            stepped[at]->native_slot(),
            present,
            drawn[at]
        );
    }
    if (!rebases) {
        return;
    }
    const int64_t depth = present - p_plan.floor;
    NetwPredictStats::Book *stats = &decl->facts;
    Dictionary joint_depth
        = stats->get_dict_fact(NetwPredictStats::FACT_JOINT_DEPTH);
    joint_depth[depth] = int64_t(joint_depth.get(depth, 0)) + 1;
    stats->set_dict_fact(NetwPredictStats::FACT_JOINT_DEPTH, joint_depth);
}

void NetwPredictSlotEngine::release_lingering(int64_t p_floor_transition) {
    decl->facts.set_int_fact(
        NetwPredictStats::FACT_LINGER_HELD,
        pool->release_lingering(native_slot(), p_floor_transition)
    );
}

void NetwPredictSlotEngine::joint_heal() {
    const int64_t frontier = ledger_drive_frontier();
    predict::StateRow newest;
    if (!pool->read_predicted(native_slot(), frontier + 1, newest)
        || !newest.any()) {
        NETW_TRACE(
            sys::PREDICTION,
            "A group repair found no recorded state to snap to."
        );
        return;
    }
    restore_row(
        newest,
        int(predict::Operator::JOINT_REBASE),
        -1,
        nullptr,
        frontier + 1
    );
}

void NetwPredictSlotEngine::note_joint_writes(
    const predict::StateRow &p_before
) {
    const int64_t slot = native_slot();
    predict::StateRow deltas;
    if (pool->write_deltas_rows(slot, p_before, predict::StateRow(), deltas)) {
        pool->ledger_note_rows(slot, deltas);
    }
}

int64_t NetwPredictSlotEngine::ledger_drive_frontier() const {
    return pool->drive_frontier(native_slot());
}

predict::ReplayPlan NetwPredictSlotEngine::pool_joint_pass(
    int64_t p_present
) {
    const int64_t slot = native_slot();
    if (slot < 0
        || pool->island_of(slot) == NetwPredictionEngine::ISLAND_NONE) {
        return predict::ReplayPlan();
    }
    return pool->joint_pass(slot, p_present);
}

void NetwPredictSlotEngine::host_local_author_tick(int64_t p_tick) {
    const int64_t slot = native_slot();
    pool->capture_command(slot);
    const predict::StateRow *input = pool->command_of(slot);
    if (input == nullptr) {
        return;
    }
    pool->record_authored_command(slot, p_tick, p_tick, true, *input);
    pool->set_latest_input_tick(slot, p_tick);
    pool->hold_frame_command(slot);
    const PropertyBindingPtr input_set = input_binding();
    if (input_set != nullptr) {
        input_set->set_authored_tick(p_tick);
    } else {
        NETW_TRACE(
            sys::PREDICTION,
            "Tick %d was authored with no input binding.",
            int(p_tick)
        );
    }
    pool->record_command_input(slot, p_tick, *pool->command_of(slot));
}

void NetwPredictSlotEngine::host_local_frame_step(
    const NetwPredictTiming &p_timing
) {
    const int64_t slot = native_slot();
    if (!p_timing.get_simulating()
        || p_timing.get_tick() <= pool->last_frame_transition_tick_of(slot)) {
        charge_authoring_clamp();
        return;
    }
    pool->set_last_frame_transition_tick(slot, p_timing.get_tick());
    const Fold fold = drive_fold(p_timing.get_tick());
    const int64_t label = fold.label;
    const bool fresh = fold.fresh;
    const predict::StateRow *held = pool->frame_command_of(slot);
    if (held == nullptr) {
        return;
    }
    const predict::StateRow &command = *held;
    pool->record_drive_hashed(
        slot,
        label,
        label,
        int(fold.kind),
        pool->command_hash_row(slot, command),
        p_timing.get_tick(),
        true,
        true
    );
    run_command(command, p_timing.get_delta(), label, fresh);
    pool->relay_ran_input(slot, p_timing.get_tick(), command);
    pool->set_ack_advanced(slot, fresh);
    if (!fresh) {
        return;
    }
    pool->set_last_driven_input_tick(slot, label);
    const PropertyBindingPtr state = state_binding();
    if (state != nullptr) {
        state->set_authored_tick(label);
        state->set_reconcile_ack(label);
    } else {
        NETW_TRACE(
            sys::PREDICTION,
            "Step %d ran with no state binding.",
            int(label)
        );
    }
}

void NetwPredictSlotEngine::host_local_step(double p_delta, int64_t p_tick) {
    pool->host_local_step(native_slot(), p_delta, p_tick);
}

void NetwPredictSlotEngine::on_input_frame(const predict::Arrival &p_arrival) {
    if (!lane_open()) {
        NETW_TRACE(sys::PREDICTION, "An input arrived with no input history.");
        return;
    }
    const int64_t slot = native_slot();
    const int64_t oldest = pool->record_arrived_inputs(
        slot,
        p_arrival,
        schedule() != NetwSimulation::SCHEDULE_FRAME
    );
    if (pool->next_input_tick_of(slot) < 0 && oldest >= 0) {
        pool->set_next_input_tick(slot, oldest);
    }
}

void NetwPredictSlotEngine::consume_step(
    double p_delta,
    int64_t p_server_tick
) {
    NETW_ZONE_LEAF(
        "Predict consume step",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const int64_t slot = native_slot();
    const int64_t previous_ack = ack();
    NetwPredictStats::Book *stats = &decl->facts;
    if (pool->next_input_tick_of(slot) >= 0) {
        resync_if_stranded();
        const int depth = int(queued_span());
        const int buffer = pool->input_buffer(decl->input_buffer_ticks);
        int action = consume_action(depth, buffer);
        const bool granted = action == NetwPrediction::CONSUME_ACTION_STARVED
            && pool->holds_input(slot);
        const bool holds = granted
            && pool->next_input_tick_of(slot) + buffer
                    + pool->command_window(slot)
                <= p_server_tick;
        if (holds) {
            action = NetwPrediction::CONSUME_ACTION_REPLAY;
        } else if (granted) {
            action = NetwPrediction::CONSUME_ACTION_HOLD;
            hold_behind = std::min(hold_behind + 1, HOLD_BEHIND_MAX);
        } else if (action == NetwPrediction::CONSUME_ACTION_REPLAY) {
            consume_primed = true;
        } else if (action == NetwPrediction::CONSUME_ACTION_STARVED) {
            consume_primed = false;
        } else if (consume_primed) {
            action = NetwPrediction::CONSUME_ACTION_REPLAY;
        }
        pool->report_consume(slot, depth, buffer);
        switch (action) {
            case NetwPrediction::CONSUME_ACTION_REPLAY:
                consume_one(p_delta, holds);
                consume_backlog(p_delta, buffer, holds ? 0 : hold_behind);
                hold_behind = holds ? hold_behind : 0;
                break;
            case NetwPrediction::CONSUME_ACTION_HOLD:
                stats->set_int_fact(
                    NetwPredictStats::FACT_HELD,
                    stats->get_int_fact(NetwPredictStats::FACT_HELD) + 1
                );
                mark_idle_drive(ack(), NetwPrediction::DRIVE_KIND_HOLD);
                NETW_TRACE(
                    sys::PREDICTION,
                    "Tick %d has %d inputs queued.",
                    int(p_server_tick),
                    depth
                );
                break;
            case NetwPrediction::CONSUME_ACTION_STARVED:
                stats->set_int_fact(
                    NetwPredictStats::FACT_STARVED,
                    stats->get_int_fact(NetwPredictStats::FACT_STARVED) + 1
                );
                mark_idle_drive(ack(), NetwPrediction::DRIVE_KIND_STARVED);
                NETW_TRACE(
                    sys::PREDICTION,
                    "Tick %d ran with no queued input.",
                    int(p_server_tick)
                );
                break;
            default:
                break;
        }
    }
    pool->set_ack_advanced(slot, ack() != previous_ack);
    if (ack_advanced() && pool->command_of(slot) != nullptr) {
        pool->relay_ran_input(slot, p_server_tick, *pool->command_of(slot));
    }
    const PropertyBindingPtr state = state_binding();
    if (state != nullptr) {
        state->set_authored_tick(p_server_tick);
        if (ack_advanced() || pool->sent_last_tick()) {
            state->set_reconcile_ack(ack_advanced() ? ack() : -1);
        }
    } else {
        NETW_TRACE(
            sys::PREDICTION,
            "Tick %d was used with no state binding.",
            int(p_server_tick)
        );
    }
    decl->ack_age_ticks = lane_open()
        ? int(std::max(int64_t(0), pool->newest_authored_input(slot) - ack()))
        : 0;
    send_ack_frame();
}

void NetwPredictSlotEngine::consume_frame_step(
    const NetwPredictTiming &p_timing
) {
    NETW_ZONE_LEAF(
        "Predict consume frame step",
        colors::PREDICTION,
        profile::SUBSYSTEM_PREDICTION
    );
    const int64_t slot = native_slot();
    const int64_t previous_ack = ack();
    pool->set_last_replayed_fresh(slot, false);
    const int buffer = std::max(0, decl->input_buffer_ticks);
    pool->admit_missing_commands(slot, buffer);
    const int depth = replay_depth();
    NetwPredictStats::Book *stats = &decl->facts;
    const int64_t drove_before
        = stats->get_int_fact(NetwPredictStats::FACT_DRIVE_SEQ);
    record_consume_cadence(depth);
    const int action = consume_action(depth, buffer);
    pool->report_consume(slot, depth, buffer);
    switch (action) {
        case NetwPrediction::CONSUME_ACTION_REPLAY:
            resync_tape_if_stranded(depth, buffer);
            replay_tape_entry(p_timing.get_delta(), p_timing.get_tick());
            break;
        case NetwPrediction::CONSUME_ACTION_HOLD:
            stats->set_int_fact(
                NetwPredictStats::FACT_HELD,
                stats->get_int_fact(NetwPredictStats::FACT_HELD) + 1
            );
            mark_idle_drive(
                pool->last_replayed_label_of(slot),
                NetwPrediction::DRIVE_KIND_HOLD
            );
            NETW_TRACE(
                sys::PREDICTION,
                "Frame %d has %d inputs recorded.",
                int(p_timing.get_tick()),
                depth
            );
            break;
        case NetwPrediction::CONSUME_ACTION_STARVED:
            stats->set_int_fact(
                NetwPredictStats::FACT_STARVED,
                stats->get_int_fact(NetwPredictStats::FACT_STARVED) + 1
            );
            mark_idle_drive(
                pool->last_replayed_label_of(slot),
                NetwPrediction::DRIVE_KIND_STARVED
            );
            NETW_TRACE(
                sys::PREDICTION,
                "Frame %d ran with no recorded input.",
                int(p_timing.get_tick())
            );
            break;
        default:
            break;
    }
    bump_consume_shape(
        stats->get_int_fact(NetwPredictStats::FACT_DRIVE_SEQ) - drove_before
    );
    pool->set_ack_advanced(slot, ack() != previous_ack);
    const PropertyBindingPtr state = state_binding();
    if (state != nullptr) {
        const int64_t tick = p_timing.get_tick();
        const bool same_tick = state->get_authored_tick() == tick;
        state->set_authored_tick(tick);
        if (ack_advanced()) {
            state->set_reconcile_ack(ack());
        } else if (!same_tick) {
            state->set_reconcile_ack(-1);
        }
    } else {
        NETW_TRACE(
            sys::PREDICTION,
            "Frame %d was used with no state binding.",
            int(p_timing.get_tick())
        );
    }
    refresh_tape_diagnostics();
    decl->ack_age_ticks = replay_depth();
    send_ack_frame();
}

void NetwPredictSlotEngine::record_consume_cadence(int p_depth) {
    const int64_t slot = native_slot();
    const int arrivals
        = std::min(pool->arrivals_this_frame_of(slot), ARRIVAL_BUCKET_MAX);
    pool->set_arrivals_this_frame(slot, 0);
    NetwPredictStats::Book *stats = &decl->facts;
    PackedInt32Array arrival_buckets
        = stats->get_buckets_fact(NetwPredictStats::FACT_ARRIVALS);
    if (arrivals >= 0 && arrivals < arrival_buckets.size()) {
        arrival_buckets.set(arrivals, arrival_buckets[arrivals] + 1);
        stats->set_buckets_fact(
            NetwPredictStats::FACT_ARRIVALS,
            arrival_buckets
        );
    }
    PackedInt32Array depth_buckets
        = stats->get_buckets_fact(NetwPredictStats::FACT_REPLAY_DEPTH);
    const int bucket = std::min(p_depth, REPLAY_DEPTH_BUCKET_MAX);
    if (bucket >= 0 && bucket < depth_buckets.size()) {
        depth_buckets.set(bucket, depth_buckets[bucket] + 1);
        stats->set_buckets_fact(
            NetwPredictStats::FACT_REPLAY_DEPTH,
            depth_buckets
        );
    }
}

void NetwPredictSlotEngine::bump_consume_shape(int64_t p_consumed) {
    NetwPredictStats::Book *stats = &decl->facts;
    const String key
        = String::num_int64(p_consumed) + String(",")
        + String::num_int64(
              stats->get_int_fact(NetwPredictStats::FACT_QUANTUM_STEPS)
        );
    Dictionary shape
        = stats->get_dict_fact(NetwPredictStats::FACT_CONSUME_SHAPE);
    shape[key] = int64_t(shape.get(key, 0)) + 1;
    stats->set_dict_fact(NetwPredictStats::FACT_CONSUME_SHAPE, shape);
}

void NetwPredictSlotEngine::replay_tape_entry(
    double p_delta,
    int64_t p_timing_tick
) {
    pool->replay_tape_entry(native_slot(), p_delta, p_timing_tick);
}

void NetwPredictSlotEngine::resync_tape_if_stranded(int p_depth, int p_buffer) {
    pool->resync_tape_if_stranded(
        native_slot(),
        p_depth,
        p_buffer,
        decl->max_input_lag_ticks
    );
}

int NetwPredictSlotEngine::replay_depth() const {
    if (core_seated() == nullptr) {
        return 0;
    }
    const int64_t slot = native_slot();
    return pool->command_depth_from(slot, pool->replay_cursor_of(slot));
}

void NetwPredictSlotEngine::resync_if_stranded() {
    pool->resync_input_if_stranded(
        native_slot(),
        decl->max_input_lag_ticks,
        pool->input_buffer(decl->input_buffer_ticks)
    );
}

int64_t NetwPredictSlotEngine::queued_span() const {
    return pool->queued_span(native_slot());
}

void NetwPredictSlotEngine::consume_one(double p_delta, bool p_holds) {
    pool->consume_one(native_slot(), p_delta, p_holds);
}

void NetwPredictSlotEngine::consume_backlog(
    double p_delta,
    int p_buffer,
    int p_behind
) {
    const int ceiling = decl->max_inputs_per_tick + p_behind;
    for (int ran = 1; ran < ceiling; ++ran) {
        const int depth = int(queued_span());
        if (consume_action(depth, p_buffer)
            != NetwPrediction::CONSUME_ACTION_REPLAY) {
            return;
        }
        finalize_recorded_state();
        consume_one(p_delta, false);
    }
}

void NetwPredictSlotEngine::mark_idle_drive(int64_t p_label, int p_kind) {
    if (core_seated() != nullptr) {
        pool->record_idle_drive(native_slot(), p_label, p_kind);
    }
}

void NetwPredictSlotEngine::adopt_tape_position(int64_t p_transition) {
    const int64_t slot = native_slot();
    pool->set_last_driven_entry_index(slot, p_transition);
    NetwPredictStats::Book *stats = &decl->facts;
    stats->set_int_fact(
        NetwPredictStats::FACT_TAPE_EPOCH,
        pool->tape_epoch_of(slot)
    );
    stats->set_int_fact(NetwPredictStats::FACT_TAPE_INDEX, p_transition);
    pool->set_next_tape_entry_index(slot, p_transition + 1);
}

void NetwPredictSlotEngine::author_command_entry(
    int64_t p_label,
    bool p_fresh
) {
    const int64_t slot = native_slot();
    const int64_t transition = pool->next_tape_entry_index_of(slot);
    if (core_seated() != nullptr) {
        pool->tape_author(slot, p_label, p_fresh);
    }
    adopt_tape_position(transition);
}

void NetwPredictSlotEngine::refresh_tape_diagnostics() {
    pool->refresh_tape_diagnostics(native_slot());
}

void NetwPredictSlotEngine::run_command(
    const predict::StateRow &p_command,
    double p_delta,
    int64_t p_tick,
    bool p_fresh
) {
    const int64_t slot = native_slot();
    if (core_seated() != nullptr && pool->owner_bound(slot)) {
        pool->run_command(slot, p_command, p_delta, p_tick, p_fresh);
        return;
    }
    pool->apply_command(slot, p_command);
    const Callable step = is_seated()
        ? NetwMultiplayer::sim_step_of(seated_record(), seated_node())
        : released_simulation_step(handle);
    if (step.is_valid()) {
        Array arguments;
        arguments.push_back(p_delta);
        arguments.push_back(p_tick);
        arguments.push_back(p_fresh);
        game_call(step, arguments, game_calls::SIMULATION_STEP);
    } else {
        NETW_TRACE(sys::PREDICTION, "Tick %d has no step to run.", int(p_tick));
    }
}

void NetwPredictSlotEngine::push_simulate() {
    if (core_seated() != nullptr) {
        pool->set_simulate(
            native_slot(),
            is_seated()
                ? NetwMultiplayer::sim_step_of(seated_record(), seated_node())
                : released_simulation_step(handle)
        );
    }
}

Dictionary NetwPredictSlotEngine::capture() const {
    return pool->capture_canonical_state(native_slot());
}

void NetwPredictSlotEngine::close_captured_row(int64_t p_transition) {
    NETW_PREDICT_TICK_SCOPE();
    pool->capture_into_book(native_slot(), p_transition + 1);
    close_journal_row(p_transition);
}

void NetwPredictSlotEngine::close_journal_row(int64_t p_transition) {
    const bool witnessed = pool->samples_contacts(native_slot());
    const Dictionary solve = witnessed ? pool->seal_transition(
                                             native_slot(),
                                             p_transition,
                                             witness_sample(),
                                             island_participant_ids()
                                         )
                                       : pool->seal_transition(
                                             native_slot(),
                                             p_transition,
                                             unsampled,
                                             unparticipated
                                         );
    if (solve.is_empty()) {
        return;
    }
    maybe_demote_for_breach(p_transition, solve);
}

Dictionary NetwPredictSlotEngine::witness_sample() const {
    return pool->witness_sample(native_slot(), decl);
}

PackedStringArray NetwPredictSlotEngine::island_participant_ids() const {
    return pool->live_participant_ids(native_slot());
}

void NetwPredictSlotEngine::maybe_demote_for_breach(
    int64_t p_transition,
    const Dictionary &p_solve
) {
    pool->demote_for_breach(
        native_slot(),
        p_transition,
        bool(p_solve.get(key_breach(), false)),
        int(p_solve.get(key_witness_fp(), 0)),
        *this
    );
}

void NetwPredictSlotEngine::restore_row(
    const predict::StateRow &p_state,
    int p_operator,
    int64_t p_basis,
    NetwPredictSlotEngine *p_provenance_owner,
    int64_t p_entry,
    bool p_evidence_free,
    bool p_pool_planned
) {
    NetwPredictSlotEngine *episode_owner
        = p_provenance_owner != nullptr ? p_provenance_owner : this;
    if (!pool->apply_restore_row(
            native_slot(),
            p_state,
            p_operator,
            p_basis,
            episode_owner->native_slot(),
            is_seated() ? record->get_entity_id() : StringName(),
            decl->ack_age_ticks,
            decl->correction_threshold,
            p_evidence_free,
            p_pool_planned,
            p_entry
        )) {
        NETW_BUG();
    }
    if (p_operator != int(predict::Operator::NONE) && !p_pool_planned) {
        episode_owner->sync_episode();
    }
}

void NetwPredictSlotEngine::register_with_loop() {
    NetwMultiplayer *seated = core_seated();
    NetwPredictRunner *runner = seated != nullptr ? predict_runner : nullptr;
    if (runner != nullptr) {
        runner->register_engine(this);
        pool->set_registered(native_slot(), true);
    } else {
        NETW_TRACE(sys::PREDICTION, "No prediction runner holds this engine.");
    }
}

void NetwPredictSlotEngine::unregister_from_loop() {
    NetwMultiplayer *seated = core_seated();
    NetwPredictRunner *runner = seated != nullptr ? predict_runner : nullptr;
    const int64_t slot = native_slot();
    if (runner != nullptr) {
        runner->unregister_engine(this);
    }
    pool->set_registered(slot, false);
}

} // namespace netw
