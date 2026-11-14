#pragma once

#include "netw_test.h"

#include "godot/templates.hpp"
#include "godot/variant.hpp"
#include "netw/api/quantize.hpp"
#include "netw/prediction_core.hpp"

namespace netw_test {

class EntityDecl {
    godot::StringName decl_name;
    godot::StringName decl_schema;
    int decl_route = 0;
    int decl_controller = 0;
    bool decl_wants_owner = true;
    bool decl_predicts = false;
    double decl_prediction_epsilon = 0.0;
    netw::Schedule decl_schedule = netw::Schedule::TICK;
    netw::MissingInput decl_missing_input = netw::MissingInput::STALL;
    netw::RecoveryPolicy decl_recovery = netw::RecoveryPolicy::AUTO;
    int decl_input_buffer_ticks = 0;
    int decl_input_window = -1;
    int decl_consume_lag = 0;
    bool decl_mounted = false;
    bool decl_spawned = false;
    godot::Vector<godot::StringName> decl_synced;
    godot::Vector<godot::Ref<netw::NetwQuantize>> decl_quantizers;
    godot::Variant decl_initial_pose;
    godot::StringName decl_projection;

public:
    EntityDecl &projected_by(const godot::StringName &p_channel) {
        decl_projection = p_channel;
        return *this;
    }

    const godot::StringName &projection() const {
        return decl_projection;
    }

    EntityDecl &named(const godot::StringName &p_name) {
        decl_name = p_name;
        return *this;
    }

    EntityDecl &on_route(int p_route) {
        decl_route = p_route;
        return *this;
    }

    EntityDecl &controlled_by(int p_peer) {
        decl_controller = p_peer;
        return *this;
    }

    EntityDecl &synced(const godot::StringName &p_column) {
        decl_synced.push_back(p_column);
        decl_quantizers.push_back(godot::Ref<netw::NetwQuantize>());
        return *this;
    }

    EntityDecl &quantized(const godot::Ref<netw::NetwQuantize> &p_quantizer) {
        if (!decl_quantizers.is_empty()) {
            decl_quantizers.set(decl_quantizers.size() - 1, p_quantizer);
        }
        return *this;
    }

    EntityDecl &on_schema(const godot::StringName &p_schema) {
        decl_schema = p_schema;
        return *this;
    }

    EntityDecl &placed_at(const godot::Variant &p_pose) {
        decl_initial_pose = p_pose;
        return *this;
    }

    EntityDecl &predicted(double p_epsilon = 0.0) {
        decl_predicts = true;
        decl_prediction_epsilon = p_epsilon;
        return *this;
    }

    EntityDecl &missing_input(netw::MissingInput p_policy) {
        decl_missing_input = p_policy;
        return *this;
    }

    EntityDecl &buffered(int p_depth) {
        decl_input_buffer_ticks = p_depth;
        return *this;
    }

    EntityDecl &windowed(int p_samples) {
        decl_input_window = p_samples;
        return *this;
    }

    int input_window() const {
        return decl_input_window;
    }

    EntityDecl &consume_lag(int p_ticks) {
        decl_consume_lag = p_ticks;
        return *this;
    }

    EntityDecl &mounted() {
        decl_mounted = true;
        return *this;
    }

    bool is_mounted() const {
        return decl_mounted;
    }

    EntityDecl &spawned() {
        decl_spawned = true;
        return *this;
    }

    bool is_spawned() const {
        return decl_spawned;
    }

    EntityDecl &scheduled(netw::Schedule p_schedule) {
        decl_schedule = p_schedule;
        return *this;
    }

    EntityDecl &recovered_by(netw::RecoveryPolicy p_recovery) {
        decl_recovery = p_recovery;
        return *this;
    }

    netw::RecoveryPolicy recovery() const {
        return decl_recovery;
    }

    EntityDecl &wrapperless() {
        decl_wants_owner = false;
        return *this;
    }

    bool wants_owner() const {
        return decl_wants_owner;
    }

    const godot::StringName &name() const {
        return decl_name;
    }

    int route() const {
        return decl_route;
    }

    int controller() const {
        return decl_controller;
    }

    const godot::Vector<godot::StringName> &synced_columns() const {
        return decl_synced;
    }

    const godot::Vector<godot::Ref<netw::NetwQuantize>> &
    synced_quantizers() const {
        return decl_quantizers;
    }

    const godot::StringName &schema() const {
        return decl_schema;
    }

    const godot::Variant &initial_pose() const {
        return decl_initial_pose;
    }

    bool is_predicted() const {
        return decl_predicts;
    }

    double prediction_epsilon() const {
        return decl_prediction_epsilon;
    }

    int input_buffer_ticks() const {
        return decl_input_buffer_ticks;
    }

    int consume_lag_ticks() const {
        return decl_consume_lag;
    }

    netw::MissingInput missing_input_policy() const {
        return decl_missing_input;
    }

    netw::Schedule schedule() const {
        return decl_schedule;
    }
};

} // namespace netw_test
