#include "netw/display/runtime.hpp"

#include <limits>

#include "godot/object.hpp"
#include "netw/api/netw_multiplayer.hpp"
#include "netw/display/history.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"
#include "netw/subsystems.hpp"

using namespace godot;

namespace netw::display {

Runtime::Runtime() {
    display_offset_limit = std::numeric_limits<double>::infinity();
}

Runtime::~Runtime() {
    clear_channels();
}

Channel *Runtime::add_channel() {
    Channel *channel = memnew(Channel);
    states.push_back(channel);
    writes.push_back(channel);
    return channel;
}

void Runtime::order_writes() {
    writes.clear();
    for (Channel *channel : states) {
        if (channel->display_port().turns()) {
            writes.push_back(channel);
        }
    }
    for (Channel *channel : states) {
        if (!channel->display_port().turns()) {
            writes.push_back(channel);
        }
    }
}

void Runtime::clear_channels() {
    writes.clear();
    for (Channel *channel : states) {
        memdelete(channel);
    }
    states.clear();
}

void Runtime::bind(const RID &p_entity, Node *p_owner) {
    bound_entity = p_entity;
    owner_port.bind(p_owner);
}

void Runtime::rekey(const RID &p_entity) {
    for (Channel *channel : states) {
        if (channel->get_entity() == bound_entity) {
            channel->set_entity(p_entity);
        }
    }
    bound_entity = p_entity;
}

Node *Runtime::owner() {
    return Object::cast_to<Node>(owner_port.resolve(sys::INTERPOLATION));
}

Channel *Runtime::channel_named(const StringName &p_name) const {
    if (tracks.is_ambiguous(p_name)) {
        Node *held = Object::cast_to<Node>(gd::object_of(owner_port.id));
        fault::raise(
            NetwMultiplayer::session_holding(held),
            NETW_FAULT_INTERPOLATION_IGNORED,
            {String(p_name),
             String(),
             String(),
             0,
             NETW_INTERPOLATION_NAME_SHARED},
            uint64_t(owner_port.id)
        );
    }
    const int at = tracks.by_name(p_name);
    if (at < 0 || at >= int(states.size())) {
        return nullptr;
    }
    return states[at];
}

Channel *Runtime::channel_following(
    uint64_t p_owner,
    uint32_t p_lane,
    uint32_t p_column
) const {
    for (Channel *channel : states) {
        if (channel->display_history().follows(p_owner, p_lane, p_column)) {
            return channel;
        }
    }
    return nullptr;
}

void Runtime::snap_named(const StringName &p_name, const Variant &p_value) {
    Channel *channel = channel_named(p_name);
    if (channel != nullptr) {
        channel->snap(p_value);
        playhead.forget_arrivals();
    }
}

void Runtime::reset(int p_display_offset, int p_recommended_display_offset) {
    playhead.settle(config, p_display_offset, p_recommended_display_offset);
    for (Channel *channel : writes) {
        channel->display_history().clear();
        channel->render_offset().clear();
        channel->set_last_written(channel->current_source_value());
        channel->write(channel->get_last_written());
    }
}

int64_t Runtime::authoring_tick() const {
    if (!authored_by_state || playhead.get_display_tick() < 0) {
        return -1;
    }
    for (Channel *channel : states) {
        if (!channel->get_authoring_ticks()) {
            continue;
        }
        const History &history = channel->display_history();
        const int64_t previous
            = history.bracketing_ticks(playhead.get_display_tick()).x;
        if (previous >= 0) {
            return previous;
        }
    }
    return -1;
}

const History *Runtime::history_of(const StringName &p_track) const {
    if (pump_mode == netw::display::PUMP_CHASE) {
        return nullptr;
    }
    const Channel *channel = channel_named(p_track);
    return channel != nullptr ? &channel->display_history() : nullptr;
}

Variant Runtime::track_stat(const StringName &p_track, int p_stat) const {
    const Channel *channel
        = p_track.is_empty() ? nullptr : channel_named(p_track);
    switch (p_stat) {
        case TRACK_SLEEPING:
            return channel != nullptr
                && channel->display_history().is_sleeping();
        case TRACK_OFFSET_ARMED:
            return channel != nullptr && channel->render_offset().armed;
        case TRACK_OFFSET_HELD:
            return channel != nullptr && channel->render_offset().is_held();
        case TRACK_NEWEST_TICK: {
            const History *history = history_of(p_track);
            return history != nullptr ? history->newest_tick() : int64_t(-1);
        }
        case TRACK_BUFFER_SIZE: {
            const History *history = history_of(p_track);
            return history != nullptr ? history->size() : int64_t(0);
        }
        case TRACK_CHANNELS:
            return int64_t(states.size());
        case TRACK_AMBIGUOUS:
            return tracks.ambiguous_count();
        case TRACK_PUMPED_FRAMES:
            return pumped;
        case TRACK_PUMP_MODE:
            return pump_mode;
        case TRACK_STARVATION_TICKS:
            return playhead.get_starvation_ticks();
        case TRACK_DISPLAY_LAG:
            return playhead.get_display_lag();
        case TRACK_ROLE:
            return role;
        default:
            return Variant();
    }
}

} // namespace netw::display
