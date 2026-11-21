#pragma once

#include <cstdint>

#include "godot/rid.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/api/prediction.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/call_args.hpp"
#include "netw/predict/decl.hpp"
#include "netw/value_variant.hpp"

namespace netw_test {

struct Internals {
    static netw::NetwEntityRecord *record(const netw::NetwEntity *p_entity) {
        return p_entity->get_record();
    }

    static godot::Error value_encode(
        const godot::Variant &p_value,
        netw::value::Bytes &r_bytes
    ) {
        return netw::value::encode(p_value, r_bytes);
    }

    static godot::PackedByteArray value_sent(const godot::Variant &p_value) {
        return netw::call_args::value_bytes(
            p_value,
            godot::String("Value 0"),
            nullptr
        );
    }

    static bool value_unpacked(
        const godot::PackedByteArray &p_bytes,
        godot::Variant &r_value
    ) {
        return netw::value::decode_packed(p_bytes, r_value);
    }

    static godot::PackedByteArray value_packed(const godot::Variant &p_value) {
        godot::PackedByteArray bytes;
        netw::value::encode_packed(p_value, bytes);
        return bytes;
    }

    static int64_t value_decode(
        const netw::value::Bytes &p_bytes,
        godot::Variant &r_value
    ) {
        return netw::value::decode(p_bytes.ptr(), p_bytes.size(), r_value);
    }

    static int64_t value_validate(const netw::value::Bytes &p_bytes) {
        return netw::value::validate(p_bytes.ptr(), p_bytes.size());
    }

    static int value_compare(
        const netw::value::Bytes &p_left,
        const netw::value::Bytes &p_right
    ) {
        return netw::value::compare(
            p_left.ptr(),
            p_left.size(),
            p_right.ptr(),
            p_right.size()
        );
    }

    static uint32_t value_hash_of(const godot::Variant &p_value) {
        return netw::value::hash_variant(2166136261u, p_value);
    }

    static uint32_t value_hash(const netw::value::Bytes &p_bytes) {
        return netw::value::hash(2166136261u, p_bytes.ptr(), p_bytes.size());
    }

    static netw::predict::Decl &predict_declaration(
        const netw::NetwEntity *p_entity
    ) {
        return p_entity->get_record()->predict_declaration();
    }

    static netw::predict::Decl &predict_declaration(
        const netw::NetwPrediction *p_handle
    ) {
        return predict_declaration(p_handle->get_entity().ptr());
    }

    static void adopt_handle(
        const godot::Ref<netw::NetwEntity> &p_entity,
        const godot::RID &p_handle
    ) {
        if (p_entity->home != nullptr) {
            p_entity->rekey(p_entity->home, p_handle);
            return;
        }
        p_entity->draft->adopt_handle(p_handle);
    }

    template <typename Session>
    static void attach_slot_engine(
        netw::NetwPredictSlotEngine &r_engine,
        const Session &p_session,
        const godot::Ref<netw::NetwEntity> &p_entity
    ) {
        const godot::Ref<netw::NetwPrediction> handle
            = p_entity->get_prediction();
        netw::NetwEntityRecord *held = p_entity->record_in(p_session.ptr());
        netw::predict::Decl *decl
            = held != nullptr ? &held->predict_declaration() : nullptr;
        if (handle.is_valid() && decl != nullptr) {
            decl->facts.bind_slot(&p_session->prediction_engine, p_entity);
        }
        r_engine.attach(
            p_session.ptr(),
            p_entity->get_record(),
            p_entity->get_owner(),
            handle,
            decl,
            &p_session->liveness_core,
            &p_session->prediction_engine,
            &p_session->predict_runner,
            &p_session->sim_rows,
            &p_session->predict_engines
        );
    }

    static int64_t seat(
        netw::NetwPredictionEngine *p_pool,
        const godot::Ref<netw::NetwEntity> &p_entity
    ) {
        return p_pool->slot_register(
            *p_entity->get_record(),
            p_entity->get_owner(),
            p_entity->get_prediction().ptr()
        );
    }

    static void unseat(
        netw::NetwPredictionEngine *p_pool,
        const godot::Ref<netw::NetwEntity> &p_entity
    ) {
        p_pool->slot_unregister(p_entity->get_record());
    }

    template <typename Session>
    static netw::interest::Engine &interest_plane(const Session &p_session) {
        return p_session->interest_plane();
    }

    template <typename Session>
    static godot::RID entity_at(const Session &p_session, int64_t p_route) {
        return p_session->liveness_core.rid_from_route(int(p_route));
    }

    template <typename Session>
    static godot::RID entity_of(
        const Session &p_session,
        godot::Object *p_node
    ) {
        return p_session->entity_of(p_node);
    }

    template <typename Session>
    static int64_t route_of(
        const Session &p_session,
        godot::Object *p_wrapper
    ) {
        return p_session->liveness_route_of(p_wrapper);
    }

    template <typename Session>
    static decltype(auto) connect_plane(const Session &p_session) {
        return (p_session->connect_plane());
    }

    template <typename Session>
    static void traffic_advance(const Session &p_session, double p_seconds) {
        p_session->wire().advance_traffic(
            p_seconds,
            p_session->packet_overhead()
        );
    }

    template <typename Session>
    static void traffic_window(const Session &p_session, double p_seconds) {
        p_session->wire().traffic.set_window(p_seconds);
    }

    template <typename Session>
    static decltype(auto) clock_engine(const Session &p_session) {
        return p_session->clock_engine();
    }

    template <typename Session>
    static void frame_begin(const Session &p_session) {
        p_session->frame_begin();
    }

    template <typename Session>
    static void run_ticks(const Session &p_session, int64_t p_count) {
        netw::Clock &clock = p_session->clock_engine();
        const int64_t count = clock.is_held() ? 0 : p_count;
        for (int64_t at = 0; at < count; ++at) {
            p_session->run_tick();
        }
        clock.resolve_simulation_gate(p_session->clock_reading());
    }

    template <typename Session>
    static void frame_end(const Session &p_session) {
        p_session->frame_end();
    }

    template <typename Session>
    static decltype(auto) get_liveness_core(const Session &p_session) {
        return p_session->get_liveness_core();
    }

    template <typename Session>
    static decltype(auto) get_prediction_engine(const Session &p_session) {
        return p_session->get_prediction_engine();
    }

    template <typename Session>
    static decltype(auto) store(const Session &p_session) {
        return p_session->store();
    }

    template <typename Session>
    static decltype(auto) idle_work(const Session &p_session) {
        return (p_session->idle_work);
    }

    template <typename Session>
    static decltype(auto) property_set_book(const Session &p_session) {
        return p_session->property_set_book();
    }

    template <typename Session>
    static decltype(auto) property_binding_book(const Session &p_session) {
        return p_session->property_binding_book();
    }

    template <typename Session>
    static decltype(auto) store_arrays(const Session &p_session) {
        return p_session->store_arrays();
    }

    template <typename Session>
    static decltype(auto) get_channel_book(const Session &p_session) {
        return p_session->get_channel_book();
    }

    template <typename Session>
    static decltype(auto) predict_engine_for(
        const Session &p_session,
        const godot::RID &p_entity
    ) {
        return p_session->predict_engine_for(p_entity);
    }

    template <typename Session>
    static decltype(auto) predict_runner_seated(const Session &p_session) {
        return p_session->predict_runner_seated();
    }

    template <typename Session>
    static decltype(auto) profiler(const Session &p_session) {
        return p_session->profiler();
    }

    template <typename Session>
    static decltype(auto) profiler_outgoing_subject(const Session &p_session) {
        return p_session->profiler_outgoing_subject();
    }

    template <typename Session>
    static decltype(auto) get_scene_core(const Session &p_session) {
        return p_session->get_scene_core();
    }

    template <typename Session>
    static decltype(auto) sync_pipeline(const Session &p_session) {
        return p_session->sync_pipeline();
    }

    template <typename Session>
    static decltype(auto) fault_book(const Session &p_session) {
        return p_session->fault_book();
    }

    template <typename Session>
    static decltype(auto) declaration_book(const Session &p_session) {
        return p_session->declaration_book();
    }

    template <typename Session>
    static decltype(auto) spawn_plane(const Session &p_session) {
        return p_session->spawn_plane();
    }

    static void control_record(netw::NetwEntity *p_entity, int64_t p_value) {
        p_entity->write_controller(p_value);
    }

    static void control_seed(
        netw::NetwEntity *p_entity,
        uint64_t p_revision,
        uint64_t p_tenure,
        int64_t p_hold
    ) {
        p_entity->seed_control(p_revision, p_tenure, p_hold);
    }

    static netw::ReplicationSend *row_send_of(netw::ReplicationCore *p_plane) {
        return netw::NetwMultiplayer::row_send_of(p_plane);
    }

    template <typename Session>
    static decltype(auto) session_plane(const Session &p_session) {
        return p_session->session_plane();
    }

    template <typename Session>
    static decltype(auto) session_prepared_join(const Session &p_session) {
        return p_session->session_prepared_join();
    }

    template <typename Session>
    static auto take_submitted_join(const Session &p_session) {
        return p_session->session_take_submitted_join_under_test();
    }

    template <typename Session>
    static const netw::sim::Row *sim_row_of(
        const Session &p_session,
        const godot::RID &p_entity
    ) {
        return p_session->sim_row_of(p_entity);
    }

    template <typename Session>
    static netw::ReplicationCore *replication_plane(const Session &p_session) {
        return p_session->get_replication_plane();
    }

    template <typename Session>
    static decltype(auto) sim_row(
        const Session &p_session,
        const godot::RID &p_entity
    ) {
        return p_session->sim_row(p_entity);
    }

    template <typename Session>
    static void retire_entity_view(
        const Session &p_session,
        const godot::RID &p_entity
    ) {
        p_session->liveness_unindex_wrapper(p_entity);
    }

    template <typename Session>
    static decltype(auto) get_display_book(const Session &p_session) {
        return p_session->get_display_book();
    }

    template <typename Session>
    static decltype(auto) capture_writer(const Session &p_session) {
        return p_session->capture_writer();
    }

    template <typename Session>
    static decltype(auto) get_rpc_park(const Session &p_session) {
        return p_session->get_rpc_park();
    }

    template <typename Session>
    static int parked_call_count(const Session &p_session) {
        return int(p_session->rpc_deferred.size());
    }

    template <typename Session>
    static decltype(auto) display_runtime_for(
        const Session &p_session,
        int64_t p_route,
        const godot::Ref<netw::NetwEntity> &p_entity
    ) {
        return p_session->display_runtime_for(
            p_route,
            p_entity->get_rid_handle()
        );
    }

    template <typename Session>
    static decltype(auto) get_databases(const Session &p_session) {
        return p_session->get_databases();
    }

    template <typename Session>
    static decltype(auto) get_lagcomp_core(const Session &p_session) {
        return p_session->get_lagcomp_core();
    }

    template <typename Session>
    static decltype(auto) attribution_book(const Session &p_session) {
        return (p_session->wire().attribution);
    }

    template <typename Session>
    static decltype(auto) membership_book(const Session &p_session) {
        return p_session->membership_book();
    }

    template <typename Session>
    static decltype(auto) property_set_of(
        const Session &p_session,
        const godot::RID &p_set
    ) {
        return p_session->property_set_of(p_set);
    }

    template <typename Session>
    static int &max_future_action_ticks(const Session &p_session) {
        return p_session->max_future_action_ticks;
    }

    template <typename Session>
    static decltype(auto) resync_tamper(const Session &p_session) {
        return (p_session->resync_tamper);
    }

    template <typename Session>
    static godot::Ref<netw::NetwPlayer> seat_player(
        const Session &p_session,
        int64_t p_peer
    ) {
        netw::session::AcceptFrame accepted;
        accepted.peer_id = p_peer;
        accepted.player_id = uint64_t(p_session->player_mint_id());
        p_session->player_ensure(accepted);
        p_session->player_admit(p_peer);
        return p_session->player_of(p_peer);
    }
};

} // namespace netw_test
