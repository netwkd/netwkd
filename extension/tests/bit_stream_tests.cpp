#include "support/fault_log.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include <cstdint>

#include "godot/variant.hpp"
#include "netw/api/bit_stream.hpp"

namespace TestNetwBitStream {

using namespace godot;
using netw::NetwBitStream;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

void describe(const Ref<NetwBitStream> &p_stream) {
    p_stream->bits(0b1011, 4);
    p_stream->varuint(4242, 3);
    p_stream->string("hero");
    p_stream->align_verify();
}

LawVerdict round_trip(const Stand &) {
    const Ref<NetwBitStream> writing = NetwBitStream::writer();
    describe(writing);
    const Ref<NetwBitStream> reading
        = NetwBitStream::reader(writing->to_bytes());
    const Ref<NetwBitStream> measuring = NetwBitStream::measurer();
    describe(measuring);

    if (reading->bits(0, 4) != 0b1011 || reading->varuint(0, 3) != 4242
        || reading->string(String()) != String("hero")
        || !reading->align_verify()) {
        return law_broken("the reader did not read back what was written");
    }
    if (measuring->bit_length() != writing->bit_length()
        || reading->bit_length() != writing->bit_length()) {
        return law_broken("the three modes spent different bit counts");
    }
    if (reading->bits_remaining() != 0) {
        return law_broken("the reader left bits unread");
    }
    return law_held();
}

LawVerdict out_of_bounds(const Stand &) {
    netw_test::FaultWatch expected_faults;
    expected_faults.expect_error("*more than its 3 byte limit*[NW0032]*", 1);
    expected_faults.expect_error("*outside*[NW0032]*", 1);

    const Ref<NetwBitStream> writing = NetwBitStream::writer();
    writing->varuint(int64_t(1) << 40, 3);
    if (writing->ok()) {
        return law_broken("a varuint too large for its bytes was written");
    }

    const Ref<NetwBitStream> ranged = NetwBitStream::writer();
    ranged->int_range(9, 0, 3);
    if (ranged->ok()) {
        return law_broken("a value outside its range was written");
    }

    const Ref<NetwBitStream> measuring = NetwBitStream::measurer();
    if (!measuring->ok() || !measuring->to_bytes().is_empty()) {
        return law_broken("a measurer refused or produced bytes");
    }
    return law_held();
}

const Row ROWS[] = {
    {"a row written, read back and measured",
     "the reader answers every value and all three spend the same bits",
     &round_trip},
    {"a value too large to spell or outside its range",
     "the writer refuses it with one error and writes nothing valid",
     &out_of_bounds},
};

TEST_CASE("[Netw][Wire] a bit stream reads back what it wrote") {
    const Stand stand = {String("a game's own bit stream")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwBitStream
