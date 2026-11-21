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

void raise_input_type(
    NetwMultiplayer *p_session,
    const SchemaRecord *p_schema,
    int p_column,
    int64_t p_sender
) {
    if (p_column < 0 || p_schema == nullptr) {
        return;
    }
    FaultValues values;
    values.name = vformat("'%s'", String(p_schema->at(p_column)->key));
    values.peer = p_sender;
    fault::raise(p_session, NETW_FAULT_VALUE_TYPE, values);
}

LocalVector<int> command_fields(
    const predict::FieldCodec &p_codec,
    const Array &p_keys
) {
    LocalVector<int> out;
    out.resize(uint32_t(p_keys.size()));
    for (int position = 0; position < p_keys.size(); ++position) {
        const StringName key = p_keys[position];
        out[uint32_t(position)] = -1;
        for (int field = 0; field < p_codec.count(); ++field) {
            if (p_codec.keys[uint32_t(field)] == key) {
                out[uint32_t(position)] = field;
                break;
            }
        }
    }
    return out;
}

bool asks_ack_detail(
    const predict::Slot &p_row,
    int64_t p_newest,
    int p_window
) {
    const predict::LaneCursor &lane = p_row.lane_cursor;
    const predict::Episode &episode = p_row.episode;
    const bool in_episode
        = episode.active && episode.state == predict::EpisodeState::OPEN;
    const bool may_flap = episode.last_closure_was_fallback
        && p_newest - episode.last_closed_transition
            <= predict::EPISODE_FLAP_WINDOW;
    const bool in_contact = p_row.out_of_domain_until >= 0
        && p_row.cursor.latest_input_tick < p_row.out_of_domain_until;
    const bool mismatched = lane.ack_mismatched >= 0
        && lane.ack_of_acks - lane.ack_mismatched
            <= p_row.stats.ack_age_ticks + p_window;
    return p_row.latches.fallback_latched || in_episode || may_flap
        || in_contact || mismatched;
}

bool within_tolerance(
    predict::LaneCursor &r_lane,
    int64_t p_transition,
    int32_t p_local_fp,
    int32_t p_peer_fp
) {
    if (r_lane.tolerated_at < 0 || p_transition < r_lane.tolerated_at
        || p_local_fp != r_lane.tolerated_local_fp) {
        return false;
    }
    if (!r_lane.tolerated_peer_known) {
        r_lane.tolerated_peer_fp = p_peer_fp;
        r_lane.tolerated_peer_known = true;
    }
    return p_peer_fp == r_lane.tolerated_peer_fp;
}

bool cause_changed(const predict::Journal &p_journal, int p_at) {
    if ((p_journal.witness_state_at(p_at) & predict::WITNESS_ROSTER_CHANGED)
        != 0) {
        return true;
    }
    if (p_at <= 0
        || p_journal.transition_at(p_at - 1)
            != p_journal.transition_at(p_at) - 1) {
        return false;
    }
    return p_journal.pre_fp_at(p_at) != p_journal.post_fp_at(p_at - 1)
        || p_journal.e_digest_at(p_at) != p_journal.e_digest_at(p_at - 1);
}

bool row_is_news(const predict::Journal &p_journal, int p_at) {
    constexpr uint8_t replaced
        = predict::ROW_SUBSTITUTED | predict::ROW_SUPERSEDED;
    if ((p_journal.flags_at(p_at) & replaced) != 0
        || cause_changed(p_journal, p_at)) {
        return true;
    }
    if (p_at <= 0
        || p_journal.transition_at(p_at - 1)
            != p_journal.transition_at(p_at) - 1) {
        return true;
    }
    return p_journal.post_fp_at(p_at) != p_journal.post_fp_at(p_at - 1)
        || p_journal.witness_class_bits_at(p_at)
            != p_journal.witness_class_bits_at(p_at - 1);
}

} // namespace

Array NetwPredictionEngine::input_keys(int64_t p_slot) const {
    Array out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || input_binding_in(*row) == nullptr) {
        return out;
    }
    const PropertySet *declared = input_binding_in(*row)->get_set();
    if (declared == nullptr) {
        return out;
    }
    for (const PropertyColumn &field : declared->columns) {
        if (field.get_lane() == int(PropertySet::VOLATILE)) {
            out.push_back(field.get_key());
        }
    }
    return out;
}

const SchemaRecord *NetwPredictionEngine::input_schema(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || input_binding_in(*row) == nullptr
        || input_binding_in(*row)->get_set() == nullptr) {
        return nullptr;
    }
    return &input_binding_in(*row)->get_set()->get_volatile_schema();
}

bool NetwPredictionEngine::decode_command_frame(
    int64_t p_slot,
    const PackedByteArray &p_payload,
    predict::CommandFrameRecord &r_frame,
    int *r_mistyped
) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || input_binding_in(*row) == nullptr
        || input_binding_in(*row)->get_set() == nullptr) {
        return false;
    }
    return predict::CommandFrameRecord::decode(
        input_binding_in(*row)->get_set()->get_volatile_schema(),
        p_payload,
        r_frame,
        r_mistyped
    );
}

PackedInt64Array NetwPredictionEngine::admit_relayed_frame(
    int64_t p_slot,
    const predict::CommandFrameRecord &p_frame,
    const Array &p_keys,
    bool p_rollback_together
) {
    PackedInt64Array counts = gd::zeroed<PackedInt64Array>(RELAY_COLUMN_COUNT);
    if (row_of(p_slot) == nullptr) {
        return counts;
    }
    int64_t *tally = counts.ptrw();
    const int64_t epoch = p_frame.epoch();
    bool bumped = false;
    if (epoch != relayed_epoch_of(p_slot)) {
        bumped = relayed_epoch_of(p_slot) >= 0;
        set_relayed_epoch(p_slot, epoch);
        joint_clear(p_slot);
        set_newest_matrix_transition(p_slot, -1);
    }
    const LocalVector<int> fields
        = command_fields(row_of(p_slot)->input_codec, p_keys);
    predict::StateRow command;
    command.resize(row_of(p_slot)->input_codec.count());
    predict::StateRow guess;
    predict::StateRow filed;
    const int64_t world_offset = joint_world_offset_of(p_slot);
    int fresh_index = 0;
    for (int at = 0; at < p_frame.transition_count(); ++at) {
        const predict::TransitionWire &transition = p_frame.transition_at(at);
        const int64_t index = transition.index - world_offset - 1;
        if (!transition.fresh) {
            continue;
        }
        const int payload = fresh_index;
        fresh_index += 1;
        const int64_t floor = newest_matrix_transition_of(p_slot)
            - predict::TAPE_HISTORY_LIMIT + 1;
        if (index < floor) {
            tally[RELAY_DROPPED_LATE] += 1;
            continue;
        }
        const int standing = joint_provenance_at(p_slot, index);
        if (standing == int(predict::CellProvenance::RELAYED)
            || relayed_at(p_slot, index)) {
            continue;
        }
        p_frame.read_payload(payload, fields, command);
        const bool guessed_wrong
            = row_of(p_slot)->stats.last_drive_label >= index
            && relayed_guess(p_slot, index, guess)
            && command_hash_row(p_slot, guess)
                != command_hash_row(p_slot, command);
        tally[RELAY_GUESSED_WRONG] += guessed_wrong ? 1 : 0;
        const bool guessed
            = standing == int(predict::CellProvenance::SUBSTITUTED)
            && read_command_at(p_slot, index, guess);
        set_newest_matrix_transition(
            p_slot,
            std::max(newest_matrix_transition_of(p_slot), index)
        );
        if (index >= 0) {
            joint_record(
                p_slot,
                index,
                predict::StateRow(),
                command,
                false,
                true,
                false
            );
            note_relayed(p_slot, index);
        }
        const bool displaced = guessed
            && !(read_command_at(p_slot, index, filed)
                 && command_hash_row(p_slot, filed)
                     == command_hash_row(p_slot, guess));
        tally[RELAY_RECORDED] += 1;
        if (!p_rollback_together) {
            continue;
        }
        if (bumped) {
            bumped = false;
            set_joint_epoch_floor(
                p_slot,
                std::max(joint_epoch_floor_of(p_slot), index)
            );
            joint_note_basis(p_slot, index, JOINT_FLOOR_EPOCH);
        }
        if (displaced) {
            const int64_t floored = index - 1;
            const int64_t held = joint_relay_floor_of(p_slot);
            set_joint_relay_floor(
                p_slot,
                held < 0 ? floored : std::min(held, floored)
            );
            joint_note_basis(p_slot, floored, JOINT_FLOOR_RELAY);
        }
    }
    return counts;
}

PackedByteArray NetwPredictionEngine::build_relay_frame(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || row->relay_window.count == 0
        || input_binding_in(*row) == nullptr
        || input_binding_in(*row)->get_set() == nullptr) {
        return PackedByteArray();
    }
    predict::CommandFrameRecord frame;
    if (!predict::CommandFrameRecord::open(
            input_binding_in(*row)->get_set()->get_volatile_schema(),
            frame
        )) {
        return PackedByteArray();
    }
    frame.set_epoch(0);
    frame.set_ack_of_acks(-1);
    const Array keys = input_keys(p_slot);
    const LocalVector<int> fields = command_fields(row->input_codec, keys);
    LocalVector<Variant> values;
    values.resize(uint32_t(keys.size()));
    const predict::RelayWindow &window = row->relay_window;
    for (uint32_t offset = 0; offset < window.count; ++offset) {
        const uint32_t at = window.at_oldest(offset);
        const int64_t tick = window.ticks[at];
        const predict::StateRow &command = window.commands[at];
        if (!frame.append_transition(tick, tick, true)) {
            return PackedByteArray();
        }
        for (int key = 0; key < keys.size(); ++key) {
            const int field = fields[uint32_t(key)];
            values[uint32_t(key)] = field >= 0 && command.has(field)
                ? command.values[uint32_t(field)]
                : Variant();
        }
        if (!frame.append_values(values, session)) {
            return PackedByteArray();
        }
    }
    return frame.to_bytes();
}

PackedByteArray NetwPredictionEngine::build_command_frame_for(int64_t p_slot) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return PackedByteArray();
    }
    const bool authors_commands = row->config.role == int(Role::PREDICT)
        || (row->config.role == int(Role::REMOTE)
            && row->latches.fallback_latched);
    if (!authors_commands || input_binding_in(*row) == nullptr
        || input_binding_in(*row)->get_set() == nullptr) {
        return PackedByteArray();
    }
    PropertySet *declared = input_binding_in(*row)->get_set();
    return build_command_frame(
        p_slot,
        declared->get_volatile_schema(),
        input_keys(p_slot),
        command_window(p_slot)
    );
}

PackedInt64Array NetwPredictionEngine::ack_frontier(
    int64_t p_slot,
    int64_t p_ack
) const {
    PackedInt64Array out;
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || row->config.role != int(Role::CONSUME) || p_ack < 0) {
        return out;
    }
    const int64_t closed = journal_last_closed(p_slot);
    out.push_back(closed);
    out.push_back(MIN(p_ack, closed));
    return out;
}

int64_t NetwPredictionEngine::lane_route(int64_t p_slot) const {
    NetwMultiplayer *host = session;
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (host == nullptr || seated == nullptr
        || host->entity_left_tree(seated->get_handle(), owner_node(p_slot))) {
        return -1;
    }
    return liveness_core->route_of(seated->get_handle());
}

void NetwPredictionEngine::send_lane(
    int64_t p_peer,
    int64_t p_route,
    int64_t p_channel,
    const PackedByteArray &p_bytes
) {
    NetwMultiplayer *host = session;
    if (host == nullptr || p_bytes.is_empty()) {
        return;
    }
    host->send_to(
        p_peer,
        p_route,
        p_channel,
        p_bytes,
        false,
        0,
        String()
    );
}

PackedByteArray NetwPredictionEngine::publish_ack_frame(int64_t p_slot) {
    if (session == nullptr) {
        return PackedByteArray();
    }
    const PackedInt64Array frontier = ack_frontier(p_slot, ack_of(p_slot));
    if (frontier.is_empty()) {
        return PackedByteArray();
    }
    set_held_fact(p_slot, NetwPredictStats::FACT_JOURNAL_CLOSED, frontier[0]);
    set_held_fact(p_slot, NetwPredictStats::FACT_ACK_FRONTIER, frontier[1]);
    if (frontier[1] < 0) {
        return PackedByteArray();
    }
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr || !ack_due(p_slot, frontier[1])) {
        return PackedByteArray();
    }
    predict::LaneCursor &lane = row->lane_cursor;
    int64_t *detailed = lane.ack_detailed;
    int64_t sent_through = -1;
    const PackedByteArray frame = build_ack_frame(
        p_slot,
        int(command_epoch_of(p_slot)),
        ack_of(p_slot),
        lane.guess_sends > 0
            ? std::min(lane.owner_ack_floor, lane.guess_wrong_at - 1)
            : lane.owner_ack_floor,
        detailed[predict::ACK_DETAILED_SENDS - 1],
        row->config.witness || lane.owner_asks_detail,
        &sent_through
    );
    if (!frame.is_empty()) {
        lane.guess_sends = std::max(0, lane.guess_sends - 1);
        for (int at = predict::ACK_DETAILED_SENDS - 1; at > 0; --at) {
            detailed[at] = detailed[at - 1];
        }
        detailed[0] = std::max(detailed[0], sent_through);
    }
    return frame;
}

void NetwPredictionEngine::send_command_frame(int64_t p_slot) {
    const int64_t route = lane_route(p_slot);
    if (route < 0 || !clock_engine->is_send_tick()) {
        return;
    }
    const PackedByteArray bytes = build_command_frame_for(p_slot);
    const predict::Slot *row = row_of(p_slot);
    NetwMultiplayer *host = session;
    if (bytes.is_empty() || row == nullptr || host == nullptr) {
        return;
    }
    add_held_fact(p_slot, NetwPredictStats::FACT_COMMAND_FRAMES_SENT, 1);
    host->send_to(
        1,
        route,
        wire::CHANNEL_PREDICT_COMMAND,
        bytes,
        false,
        0,
        String(),
        row->lane_cursor.command_rides ? wire::Delivery::RIDE
                                       : wire::Delivery::TICK
    );
}

void NetwPredictionEngine::send_ack_frame(int64_t p_slot) {
    const int64_t route = lane_route(p_slot);
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (route < 0 || seated == nullptr || !clock_engine->is_send_tick()) {
        return;
    }
    send_lane(
        seated->get_control()->resolve(),
        route,
        wire::CHANNEL_PREDICT_ACK,
        publish_ack_frame(p_slot)
    );
}

void NetwPredictionEngine::admit_relayed_payload(
    int64_t p_slot,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    const PackedInt64Array counts = receive_relayed_command_frame(
        p_slot,
        p_payload,
        rolls_back_together(p_slot),
        p_sender
    );
    if (counts.is_empty()) {
        add_held_fact(p_slot, NetwPredictStats::FACT_FRAMES_DROPPED_INVALID, 1);
        return;
    }
    add_held_fact(
        p_slot,
        NetwPredictStats::FACT_RELAYED_DROPPED_LATE,
        counts[RELAY_DROPPED_LATE]
    );
    add_held_fact(
        p_slot,
        NetwPredictStats::FACT_RELAYED_RECORDED,
        counts[RELAY_RECORDED]
    );
    add_held_fact(
        p_slot,
        NetwPredictStats::FACT_RELAY_GUESSED_WRONG,
        counts[RELAY_GUESSED_WRONG]
    );
}

void NetwPredictionEngine::declare_skipped_run(
    int64_t p_slot,
    int64_t p_from,
    int64_t p_until
) {
    if (session == nullptr) {
        return;
    }
    for (int64_t transition = p_from; transition < p_until; ++transition) {
        declare_skipped(
            p_slot,
            transition,
            command_label_of(p_slot, transition)
        );
    }
}

void NetwPredictionEngine::admit_command_payload(
    int64_t p_slot,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr || row->config.role != int(Role::CONSUME)) {
        return;
    }
    add_held_fact(p_slot, NetwPredictStats::FACT_COMMAND_FRAMES_RECEIVED, 1);
    const int64_t held = receive_command_frame(p_slot, p_payload, p_sender);
    if (held < 0) {
        add_held_fact(p_slot, NetwPredictStats::FACT_FRAMES_DROPPED_INVALID, 1);
        return;
    }
    set_held_fact(p_slot, NetwPredictStats::FACT_COMMAND_QUEUE_DEPTH, held);
    refresh_tape_diagnostics(p_slot);
    const predict::Slot *heard = row_of(p_slot);
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (heard != nullptr && heard->lane_cursor.owner_asks_detail
        && seated != nullptr && session != nullptr) {
        session->store_restate(
            heard->state_binding_id,
            seated->get_control()->resolve()
        );
    }
}

PackedByteArray NetwPredictionEngine::build_command_frame(
    int64_t p_slot,
    const SchemaRecord &p_schema,
    const Array &p_keys,
    int p_window
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return PackedByteArray();
    }
    const PackedInt64Array span = tape_span(p_slot);
    const int64_t newest = span[1];
    if (newest < 0) {
        return PackedByteArray();
    }
    const bool asks = owner_evidence || asks_ack_detail(*row, newest, p_window);
    const int64_t floor = ack_of_acks_of(p_slot);
    const int64_t unsent = row->lane_cursor.hold_sent_at;
    const int64_t hold = quiet_hold(p_slot, newest, asks);
    predict::CommandFrameRecord frame;
    if (hold < 0 || !predict::CommandFrameRecord::open(p_schema, frame)) {
        return PackedByteArray();
    }
    frame.set_epoch(int(tape_epoch_of(p_slot)));
    frame.set_ack_of_acks(floor);
    frame.set_asks_detail(asks);
    frame.set_hold(hold);

    const bool coded = predict::codes_agree(
        row->input_codec,
        frame.row_plan(),
        p_schema,
        p_keys
    );
    const predict::StateRow &stall = row->stall;
    const LocalVector<int> fields = command_fields(row->input_codec, p_keys);
    predict::StateRow authored;
    LocalVector<Variant> values;
    values.resize(uint32_t(p_keys.size()));
    PackedInt64Array rows;
    const int window = row->lane_cursor.command_rides
        ? p_window / std::max(1, clock_engine->get_send_interval())
        : p_window;
    const int64_t windowed = newest - std::max(1, window) + 1;
    const int64_t oldest = std::max(
        std::max(span[0], floor + 1),
        hold == 0 && unsent > floor ? std::min(windowed, unsent + 1) : windowed
    );
    for (int64_t index = oldest; index <= newest; ++index) {
        const int64_t label = tape_label_of(p_slot, index);
        const bool fresh = tape_is_fresh(p_slot, index);
        if (!frame.append_transition(index, label, fresh)) {
            break;
        }
        rows.append(index);
        if (!fresh) {
            continue;
        }
        if (coded && row->holds_authored(index)
            && row->inputs.coded_whole(index)
            && frame.append_codes(row->inputs.image_of(index))) {
            continue;
        }
        row->unacknowledged_row(index, authored);
        for (int at = 0; at < p_keys.size(); ++at) {
            const int field = fields[uint32_t(at)];
            const predict::StateRow &source
                = field >= 0 && authored.has(field) ? authored : stall;
            values[uint32_t(at)] = field >= 0 && source.has(field)
                ? source.values[uint32_t(field)]
                : Variant();
        }
        frame.append_values(values, session);
    }
    if (rows.is_empty()) {
        return PackedByteArray();
    }
    const int64_t closed
        = owner_evidence ? journal_last_closed(p_slot) : int64_t(-1);
    for (int at = 0; at < rows.size(); ++at) {
        const int64_t index = rows[at];
        if (index > closed) {
            break;
        }
        const int seat = journal_slot_of(p_slot, index);
        if (seat < 0) {
            break;
        }
        const predict::JournalRow held = journal_row(p_slot, index);
        if (!held.present) {
            break;
        }
        PackedInt32Array pre_family;
        pre_family.push_back(held.pre_families.pose);
        pre_family.push_back(held.pre_families.momentum);
        pre_family.push_back(held.pre_families.controller);
        PackedInt32Array post_family;
        post_family.push_back(held.post_families.pose);
        post_family.push_back(held.post_families.momentum);
        post_family.push_back(held.post_families.controller);
        frame.append_evidence(
            held.evidence_mask,
            held.pre_fp,
            held.post_fp,
            held.e_digest,
            held.topo_fp,
            held.witness_fp,
            pre_family,
            post_family,
            held.raw_fp
        );
    }
    const PackedByteArray bytes = frame.to_bytes();
    if (hold > 0 && !bytes.is_empty()) {
        mutable_row_of(p_slot)->lane_cursor.hold_sent_at = newest;
    }
    return bytes;
}

PackedInt64Array NetwPredictionEngine::admit_command_frame(
    int64_t p_slot,
    const predict::CommandFrameRecord &p_frame,
    const Array &p_keys
) {
    const predict::Slot *row = row_of(p_slot);
    if (row == nullptr) {
        return PackedInt64Array();
    }
    const int64_t epoch = p_frame.epoch();
    if (epoch != command_epoch_of(p_slot)) {
        set_command_epoch(p_slot, epoch);
        tape_reset(p_slot, epoch);
        clear_owner_claims(p_slot);
        clear_witness_details(p_slot);
        drop_deferred_operator(p_slot);
        set_owner_ack_floor(p_slot, -1);
        set_replay_cursor(p_slot, -1);
        set_last_replayed_label(p_slot, -1);
        set_last_replayed_fresh(p_slot, false);
        set_ack(p_slot, -1);
        forget_last_input(p_slot);
        forget_commands(p_slot);
        predict::LaneCursor &lane = mutable_row_of(p_slot)->lane_cursor;
        lane.input_heard = -1;
        lane.input_hold_through = -1;
        lane.input_stood_through = -1;
        lane.ack_judged = -1;
        lane.ack_news = -1;
    }
    mutable_row_of(p_slot)->lane_cursor.command_arrived = true;
    const NetwEntityRecord *seated = owner_record(p_slot);
    if (p_frame.hold() > 0 && seated != nullptr && session != nullptr) {
        session->wire().seq_book.await_answer(
            seated->get_control()->resolve()
        );
    }
    set_owner_ack_floor(
        p_slot,
        std::max(owner_ack_floor_of(p_slot), p_frame.ack_of_acks())
    );

    record_owner_claims(p_slot, p_frame);
    mutable_row_of(p_slot)->lane_cursor.owner_asks_detail
        = p_frame.asks_detail();

    const bool ticked = row->config.schedule == int(Schedule::TICK);
    const bool laned = row->lane_open;
    const LocalVector<int> fields = command_fields(row->input_codec, p_keys);
    predict::StateRow command;
    command.resize(row->input_codec.count());
    int fresh_seen = 0;
    int64_t newest = -1;
    const int64_t ran_before
        = ticked && laned ? next_input_tick_of(p_slot) : -1;
    for (int at = 0; at < p_frame.transition_count(); ++at) {
        const predict::TransitionWire &transition = p_frame.transition_at(at);
        const int64_t index = transition.index;
        if (index < 0) {
            continue;
        }
        const bool fresh = transition.fresh;
        const int64_t label = transition.label;
        if (fresh) {
            if (fresh_seen >= p_frame.payload_count()) {
                break;
            }
            p_frame.read_payload(fresh_seen, fields, command);
            fresh_seen += 1;
        } else {
            command.resize(row->input_codec.count());
        }
        newest = fresh ? index : newest;
        if (!command_admit(p_slot, index, label, fresh, command)) {
            continue;
        }
        if (fresh && index < ran_before) {
            judge_held_guess(p_slot, index);
        }
        set_arrivals_this_frame(p_slot, arrivals_this_frame_of(p_slot) + 1);
        if (ticked && fresh && label >= 0 && laned
            && next_input_tick_of(p_slot) < 0) {
            set_next_input_tick(p_slot, label);
        }
    }
    if (ticked && laned) {
        hear_input(p_slot, newest, p_frame.hold());
    }
    const PackedInt64Array held = command_transitions(p_slot);
    if (replay_cursor_of(p_slot) < 0 && !held.is_empty()) {
        set_replay_cursor(p_slot, held[0]);
    }
    return held;
}

int64_t NetwPredictionEngine::receive_command_frame(
    int64_t p_slot,
    const PackedByteArray &p_payload,
    int64_t p_sender
) {
    predict::CommandFrameRecord frame;
    int mistyped = -1;
    if (!decode_command_frame(p_slot, p_payload, frame, &mistyped)) {
        raise_input_type(session, input_schema(p_slot), mistyped, p_sender);
        return -1;
    }
    return admit_command_frame(p_slot, frame, input_keys(p_slot)).size();
}

PackedInt64Array NetwPredictionEngine::receive_relayed_command_frame(
    int64_t p_slot,
    const PackedByteArray &p_payload,
    bool p_rollback_together,
    int64_t p_sender
) {
    predict::CommandFrameRecord frame;
    int mistyped = -1;
    if (!decode_command_frame(p_slot, p_payload, frame, &mistyped)) {
        raise_input_type(session, input_schema(p_slot), mistyped, p_sender);
        return PackedInt64Array();
    }
    return admit_relayed_frame(
        p_slot,
        frame,
        input_keys(p_slot),
        p_rollback_together
    );
}

predict::AckVerdict NetwPredictionEngine::admit_ack(
    int64_t p_slot,
    int64_t p_transition,
    const predict::EvidenceRow &p_evidence,
    bool p_substituted
) {
    predict::Slot *row = mutable_row_of(p_slot);
    return row != nullptr
        ? row->admit_ack(p_transition, p_evidence, p_substituted)
        : predict::AckVerdict();
}

PackedInt64Array NetwPredictionEngine::admit_ack_run(
    int64_t p_slot,
    const predict::AckFrame &p_frame,
    NetwPredictSlotEngine *p_engine
) {
    PackedInt64Array out = gd::zeroed<PackedInt64Array>(ACK_RUN_COLUMN_COUNT);
    int64_t *counts = out.ptrw();
    counts[ACK_RUN_FIRST_DIVERGENT] = -1;
    counts[ACK_RUN_AGE] = -1;
    const int64_t base = int64_t(p_frame.base);
    const int record_count = int(p_frame.records.size());
    for (int at = 0; at < record_count; ++at) {
        predict::Slot *row = mutable_row_of(p_slot);
        if (row == nullptr) {
            return out;
        }
        const predict::AckEvidenceWire &record = p_frame.records[at];
        const int64_t transition = base + at;
        row->lane_cursor.ack_of_acks
            = MAX(row->lane_cursor.ack_of_acks, transition);
        if ((record.flags & (predict::ACK_HELD | predict::ACK_SUBSTITUTED))
            == 0) {
            row->lane_cursor.input_confirmed
                = MAX(row->lane_cursor.input_confirmed, transition);
        }
        const int64_t age = mark_authority_ack(p_slot, transition);
        if (age >= 0) {
            counts[ACK_RUN_AGE] = age;
        }
        record_authority_witness_class(
            p_slot,
            transition,
            predict::ack_witness_class(record.flags)
        );
        if (row->latches.fallback_latched && p_engine != nullptr) {
            p_engine->apply_quarantine_witness(transition);
        }
        const predict::JournalRow judged = journal_row(p_slot, transition);
        if ((record.flags & predict::ACK_SUBSTITUTED) != 0) {
            admit_ack_row(p_slot, p_frame, transition, at, true, true);
            counts[ACK_RUN_SUBSTITUTED] += 1;
            continue;
        }
        if (!judged.present || (judged.flags & predict::ROW_ACKED) != 0) {
            continue;
        }
        const bool detailed = (record.flags & predict::ACK_COMPACT) == 0;
        const predict::AckVerdict verdict
            = admit_ack_row(p_slot, p_frame, transition, at, detailed, false);
        const bool matched = judged.post_fp == record.post_fp;
        counts[ACK_RUN_VERIFIED] += 1;
        if (matched) {
            continue;
        }
        counts[ACK_RUN_MISMATCHED] += 1;
        if (!within_tolerance(
                row->lane_cursor,
                transition,
                judged.post_fp,
                record.post_fp
            )) {
            row->lane_cursor.ack_mismatched
                = MAX(row->lane_cursor.ack_mismatched, transition);
        }
        if (counts[ACK_RUN_FIRST_DIVERGENT] < 0) {
            counts[ACK_RUN_FIRST_DIVERGENT] = transition;
        }
        if (verdict.transition >= 0) {
            charge_divergence(
                p_slot,
                p_frame,
                transition,
                at,
                detailed,
                int(verdict.attribution)
            );
            if (p_engine != nullptr) {
                p_engine->retry_deferred_operator(transition);
            }
        }
    }
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return out;
    }
    row->compare_stats.fp_verified += int(counts[ACK_RUN_VERIFIED]);
    row->compare_stats.fp_mismatches += int(counts[ACK_RUN_MISMATCHED]);
    if (counts[ACK_RUN_FIRST_DIVERGENT] >= 0
        && row->compare_stats.first_divergent_transition < 0) {
        row->compare_stats.first_divergent_transition
            = counts[ACK_RUN_FIRST_DIVERGENT];
    }
    counts[ACK_RUN_OF_ACKS] = row->lane_cursor.ack_of_acks;
    refresh_ack_age(p_slot);
    counts[ACK_RUN_AGE] = drive_stat(p_slot, STAT_ACK_AGE_TICKS);
    counts[ACK_RUN_RECEIPT] = ACK_RECEIPT_ADMITTED;
    return out;
}

PackedInt64Array NetwPredictionEngine::receive_ack_frame(
    int64_t p_slot,
    const PackedByteArray &p_payload,
    int64_t p_tape_epoch,
    bool p_confirm_reseed,
    NetwPredictSlotEngine *p_engine
) {
    PackedInt64Array refused
        = gd::zeroed<PackedInt64Array>(ACK_RUN_COLUMN_COUNT);
    int64_t *counts = refused.ptrw();
    counts[ACK_RUN_FIRST_DIVERGENT] = -1;
    counts[ACK_RUN_AGE] = -1;
    predict::AckFrame frame;
    if (!predict::decode_ack(p_payload, frame)) {
        counts[ACK_RUN_RECEIPT] = ACK_RECEIPT_UNDECODED;
        return refused;
    }
    if (int64_t(frame.epoch) != p_tape_epoch) {
        counts[ACK_RUN_RECEIPT] = ACK_RECEIPT_FOREIGN_EPOCH;
        return refused;
    }
    if (p_confirm_reseed) {
        confirm_reseed_epoch(p_slot);
    }
    return admit_ack_run(p_slot, frame, p_engine);
}

predict::AckVerdict NetwPredictionEngine::admit_ack_row(
    int64_t p_slot,
    const predict::AckFrame &p_frame,
    int64_t p_transition,
    int p_index,
    bool p_complete,
    bool p_substituted
) {
    if (p_index < 0 || uint32_t(p_index) >= p_frame.records.size()
        || !journal_has(p_slot, p_transition)) {
        return predict::AckVerdict();
    }
    const predict::AckEvidenceWire &record = p_frame.records[p_index];
    const predict::EvidenceRow peer = evidence_row(
        p_complete ? record.pre_fp : 0,
        p_complete ? record.c_hash : 0,
        p_complete ? record.e_digest : 0,
        record.post_fp,
        0,
        0,
        0,
        0,
        0,
        0,
        p_complete ? record.topo_fp : 0,
        p_complete ? record.witness_fp : 0,
        p_complete ? record.raw_fp : 0,
        p_complete ? int(record.evidence_mask) : 0,
        p_complete
    );
    return admit_ack(p_slot, p_transition, peer, p_substituted);
}

bool NetwPredictionEngine::ack_due(int64_t p_slot, int64_t p_frontier) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return false;
    }
    predict::LaneCursor &lane = row->lane_cursor;
    const bool answers = lane.command_arrived;
    lane.command_arrived = false;
    const predict::Journal &journal = row->journal;
    int64_t news = -1;
    for (int at = journal.size() - 1;
         at >= 0 && journal.transition_at(at) > lane.ack_judged;
         --at) {
        if (news < 0 && journal.transition_at(at) <= p_frontier
            && row_is_news(journal, at)) {
            news = journal.transition_at(at);
        }
    }
    lane.ack_news = std::max(lane.ack_news, news);
    lane.ack_judged = std::max(lane.ack_judged, p_frontier);
    if (answers || lane.guess_sends > 0
        || row->config.schedule != int(Schedule::TICK)) {
        return true;
    }
    return lane.ack_news > lane.owner_ack_floor
        && p_frontier - lane.ack_news
            < predict::ACK_NEWS_SENDS * clock_engine->get_send_interval();
}

PackedByteArray NetwPredictionEngine::build_ack_frame(
    int64_t p_slot,
    int p_epoch,
    int64_t p_ack,
    int64_t p_owner_ack_floor,
    int64_t p_detailed_through,
    bool p_asked,
    int64_t *r_sent_through
) const {
    const predict::Slot *row = row_of(p_slot);
    NETW_BUG_COND_V(row == nullptr, PackedByteArray());
    if (p_ack < 0) {
        return PackedByteArray();
    }
    const predict::Journal &journal = row->journal;
    const int64_t frontier = std::min(p_ack, journal.last_closed());
    if (frontier < 0) {
        return PackedByteArray();
    }
    const int64_t base = std::max(
        std::max(p_owner_ack_floor + 1, frontier - ACK_WINDOW_MAX + 1),
        journal.transition_at(0)
    );
    if (base > frontier) {
        return PackedByteArray();
    }

    predict::AckFrame frame;
    frame.epoch = uint8_t(p_epoch);
    frame.base = uint64_t(base);
    for (int at = 0; at < journal.size(); ++at) {
        const int64_t transition = journal.transition_at(at);
        if (transition < base) {
            continue;
        }
        if (transition > frontier) {
            break;
        }
        predict::AckEvidenceWire evidence;
        evidence.post_fp = journal.post_fp_at(at);
        if (transition <= p_detailed_through
            || !(p_asked || cause_changed(journal, at))) {
            evidence.flags |= predict::ACK_COMPACT;
        } else {
            evidence.evidence_mask = journal.evidence_mask_at(at);
            evidence.pre_fp = journal.pre_fp_at(at);
            evidence.c_hash = journal.c_hash_at(at);
            evidence.e_digest = journal.e_digest_at(at);
            evidence.topo_fp = journal.topo_fp_at(at);
            evidence.witness_fp = journal.witness_fp_at(at);
            evidence.raw_fp = journal.raw_fp_at(at);
        }
        if ((journal.flags_at(at) & predict::ROW_SUBSTITUTED) != 0) {
            evidence.flags |= predict::ACK_SUBSTITUTED;
        }
        if ((journal.witness_state_at(at) & predict::WITNESS_INPUT_HELD) != 0) {
            evidence.flags |= predict::ACK_HELD;
        }
        evidence.flags |= uint8_t(
            (journal.witness_class_bits_at(at) << predict::ACK_WITNESS_SHIFT)
            & predict::ACK_WITNESS_MASK
        );
        frame.records.push_back(evidence);
    }
    if (frame.records.is_empty()) {
        return PackedByteArray();
    }
    if (r_sent_through != nullptr) {
        *r_sent_through = base - 1 + predict::ack_transitions_carried(frame);
    }
    return predict::encode_ack(frame);
}

void NetwPredictionEngine::open_lane(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    NETW_BUG_COND(row == nullptr);
    row->open_lane();
}

void NetwPredictionEngine::close_lane(int64_t p_slot) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row != nullptr) {
        row->close_lane();
    }
}

bool NetwPredictionEngine::input_lane_open(int64_t p_slot) const {
    const predict::Slot *row = row_of(p_slot);
    return row != nullptr && row->lane_open;
}

int NetwPredictionEngine::mark_authority_ack(
    int64_t p_slot,
    int64_t p_transition
) {
    predict::Slot *row = mutable_row_of(p_slot);
    if (row == nullptr) {
        return -1;
    }
    row->mark_authority_ack(p_transition);
    return int(row->stats.ack_age_ticks);
}

} // namespace netw
