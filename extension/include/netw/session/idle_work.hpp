#pragma once

#include <cstdint>

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

} // namespace netw::session
