#pragma once

#include <cstdint>

#include "netw/table/schema_record.hpp"
#include "netw/wire/plan.hpp"

namespace netw::store {

enum class Coder : uint8_t { VARIANT, RAW, SCALAR, ANGLE, TURN };

struct ColumnCoder {
    Coder kind = Coder::VARIANT;
    int type = 0;
    wire::ColumnPlan slot;
    int bits = 0;
    double low = 0.0;
    double high = 0.0;
};

ColumnCoder coder_of(
    const netw::table::SchemaColumn &p_column,
    const wire::ColumnPlan &p_slot
);

bool code_value(
    const ColumnCoder &p_coder,
    const uint8_t *p_value,
    uint64_t *r_codes
);

void canonical_value(
    const ColumnCoder &p_coder,
    const uint64_t *p_codes,
    uint8_t *r_value
);

uint64_t read_code(
    const uint64_t *p_words,
    const wire::ColumnPlan &p_slot,
    int p_element
);

void write_code(
    uint64_t *r_words,
    const wire::ColumnPlan &p_slot,
    int p_element,
    uint64_t p_code
);

} // namespace netw::store
