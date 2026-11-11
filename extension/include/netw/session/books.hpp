#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "netw/repl/property_set.hpp"

namespace netw {
class ClockEngine;
class NetwChannelBook;
class NetwLivenessCore;
class NetwPredictionEngine;
class NetwPredictRunner;
class NetwPredictSlotEngine;
class SchemaCore;
} // namespace netw

namespace netw::repl {
class BindingBook;
} // namespace netw::repl

namespace netw::sim {
class Rows;
} // namespace netw::sim

namespace netw::store {
class World;
} // namespace netw::store

namespace netw::wire {
class NetworkProfiler;
} // namespace netw::wire

namespace netw::session {

#if defined(NETW_TESTS)
struct IdleWork {
    int64_t runs = 0;
    int64_t walked = 0;
    int64_t announced = 0;
    int64_t sampled = 0;
    int64_t contacts = 0;
    int64_t contacted = 0;
    int64_t touched = 0;
    int64_t contact_allocations = 0;
    int64_t contact_peer_reads = 0;
    int64_t drowsy = 0;
    int64_t sources = 0;
    int64_t sourced = 0;
    int64_t sends = 0;
    int64_t sent_rows = 0;
    int64_t pump_allocations = 0;
    int64_t send_allocations = 0;
    int64_t frame_allocations = 0;
    int64_t carrier_allocations = 0;
    int64_t transport_allocations = 0;
    int64_t display_allocations = 0;
    int64_t pumped = 0;
    int64_t recorders = 0;
    int64_t received_rows = 0;
    int64_t receive_allocations = 0;
    int64_t role_resolves = 0;
};
#endif

struct Books {
    ClockEngine *clock = nullptr;
    store::World *store = nullptr;
    repl::SetBook *sets = nullptr;
    repl::BindingBook *bindings = nullptr;
    NetwLivenessCore *liveness = nullptr;
    NetwChannelBook *channels = nullptr;
    NetwPredictionEngine *prediction = nullptr;
    NetwPredictRunner *runner = nullptr;
    const sim::Rows *sims = nullptr;
    const godot::HashMap<int64_t, NetwPredictSlotEngine *> *slot_engines
        = nullptr;
    SchemaCore *schemas = nullptr;
#if defined(DEBUG_ENABLED)
    wire::NetworkProfiler *profiler = nullptr;
    uint32_t *profiler_outgoing = nullptr;
#endif
#if defined(NETW_TESTS)
    IdleWork *idle = nullptr;
#endif
};

} // namespace netw::session
