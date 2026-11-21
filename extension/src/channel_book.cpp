#include "netw/channel_book.hpp"

#include "godot/rid.hpp"

using namespace godot;

namespace netw {

const NetwChannelBook::Row *NetwChannelBook::row_of(int64_t channel) const {
    const HashMap<int64_t, Row>::ConstIterator found = rows.find(channel);
    return found ? &found->value : nullptr;
}

bool NetwChannelBook::register_channel(
    int64_t channel,
    const Callable &handler,
    bool defers_when_unknown
) {
    const HashMap<int64_t, Row>::Iterator found = rows.find(channel);
    if (found) {
        found->value.handler = handler;
        found->value.defers = defers_when_unknown;
        return true;
    }
    Row row;
    row.handler = handler;
    row.defers = defers_when_unknown;
    rows.insert(channel, row);
    return false;
}

bool NetwChannelBook::unregister_channel(int64_t channel) {
    return rows.erase(channel);
}

Callable NetwChannelBook::handler_of(int64_t channel) const {
    const Row *row = row_of(channel);
    return row ? row->handler : Callable();
}

bool NetwChannelBook::defers(int64_t channel) const {
    const Row *row = row_of(channel);
    return row && row->defers;
}

int NetwChannelBook::size() const {
    return int(rows.size());
}

void NetwChannelBook::clear() {
    rows.clear();
}

} // namespace netw
