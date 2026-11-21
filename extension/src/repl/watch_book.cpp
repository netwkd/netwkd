#include "netw/sealed.hpp"

#include "netw/repl/watch_book.hpp"

#include "netw/colors.hpp"
#include "netw/profile.hpp"

namespace netw::repl {

using namespace godot;

namespace {

bool readable_at(const LocalVector<uint8_t> &p_readable, uint32_t p_index) {
    return p_index >= p_readable.size() || p_readable[p_index] != 0;
}

} // namespace

WatchBook::Stream *WatchBook::stream_for(int64_t p_key) {
    HashMap<int64_t, Stream>::Iterator found = streams.find(p_key);
    return found == streams.end() ? nullptr : &found->value;
}

const WatchBook::Stream *WatchBook::stream_for(int64_t p_key) const {
    HashMap<int64_t, Stream>::ConstIterator found = streams.find(p_key);
    return found == streams.end() ? nullptr : &found->value;
}

void WatchBook::poll(
    int64_t p_key,
    const LocalVector<wire::TailRef> &p_values,
    const LocalVector<uint8_t> &p_readable
) {
    NETW_ZONE_LEAF("Watch poll", colors::WIRE, profile::SUBSYSTEM_WIRE);
    Stream &stream = streams[p_key];
    if (!stream.inited) {
        stream.change_counter = 1;
        for (uint32_t i = 0; i < p_values.size(); ++i) {
            const bool ok = readable_at(p_readable, i);
            stream.values.push_back(ok ? p_values[i] : wire::TailRef());
            stream.stamps.push_back(ok ? 1 : 0);
        }
        stream.inited = true;
        return;
    }
    for (uint32_t i = 0; i < p_values.size() && i < stream.values.size(); ++i) {
        if (!readable_at(p_readable, i) || p_values[i] == stream.values[i]) {
            continue;
        }
        stream.change_counter += 1;
        stream.values[i] = p_values[i];
        stream.stamps[i] = stream.change_counter;
    }
}

void WatchBook::mask_for(
    int64_t p_key,
    int64_t p_peer,
    uint64_t &r_mask,
    LocalVector<wire::TailRef> &r_values
) const {
    r_mask = 0;
    r_values.clear();
    const Stream *stream = stream_for(p_key);
    if (stream == nullptr || !stream->inited) {
        return;
    }
    HashMap<int64_t, int64_t>::ConstIterator found
        = stream->baselines.find(p_peer);
    const int64_t baseline
        = found == stream->baselines.end() ? 0 : found->value;
    for (uint32_t i = 0; i < stream->stamps.size(); ++i) {
        if (stream->stamps[i] > baseline) {
            r_mask |= uint64_t(1) << i;
            r_values.push_back(stream->values[i]);
        }
    }
}

void WatchBook::commit(int64_t p_key, int64_t p_peer) {
    Stream *stream = stream_for(p_key);
    if (stream != nullptr) {
        stream->baselines[p_peer] = stream->change_counter;
    }
}

void WatchBook::reset(int64_t p_key) {
    streams.erase(p_key);
}

void WatchBook::clear_baselines(int64_t p_key) {
    Stream *stream = stream_for(p_key);
    if (stream != nullptr) {
        stream->baselines.clear();
    }
}

void WatchBook::clear_peer(int64_t p_peer) {
    for (KeyValue<int64_t, Stream> &entry : streams) {
        entry.value.baselines.erase(p_peer);
    }
}

void WatchBook::retain_baselines(
    int64_t p_key,
    const int32_t *p_recipients,
    int64_t p_count
) {
    Stream *stream = stream_for(p_key);
    if (stream == nullptr) {
        return;
    }
    LocalVector<int64_t> doomed;
    for (const KeyValue<int64_t, int64_t> &entry : stream->baselines) {
        bool kept = false;
        for (int64_t i = 0; i < p_count; ++i) {
            if (int64_t(p_recipients[i]) == entry.key) {
                kept = true;
                break;
            }
        }
        if (!kept) {
            doomed.push_back(entry.key);
        }
    }
    for (int64_t peer : doomed) {
        stream->baselines.erase(peer);
    }
}

bool WatchBook::is_inited(int64_t p_key) const {
    const Stream *stream = stream_for(p_key);
    return stream != nullptr && stream->inited;
}

void WatchBook::clear() {
    streams.clear();
}

} // namespace netw::repl
