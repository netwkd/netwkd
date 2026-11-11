#pragma once

#include <cstdint>

#include "godot/rid.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/session/books.hpp"

namespace netw_test {

struct Internals {
    template <typename Session>
    static const netw::session::Books &books(const Session &p_session) {
        return p_session->books;
    }

    template <typename Session>
    static decltype(auto) clock_engine(const Session &p_session) {
        return p_session->clock_engine();
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
    static decltype(auto) sim_row(
        const Session &p_session,
        const godot::RID &p_entity
    ) {
        return p_session->sim_row(p_entity);
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
    static decltype(auto) display_runtime_for(
        const Session &p_session,
        int64_t p_route,
        const godot::Ref<netw::NetwEntity> &p_entity
    ) {
        return p_session->display_runtime_for(p_route, p_entity);
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
        return p_session->attribution_book();
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
};

} // namespace netw_test
