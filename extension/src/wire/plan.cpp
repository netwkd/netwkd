#include "netw/sealed.hpp"

#include "netw/wire/plan.hpp"

#include "netw/colors.hpp"
#include "netw/schema_core.hpp"
#include "netw/profile.hpp"

using namespace godot;

namespace netw::wire {

namespace {

const int ELEMENT_WIDTHS[SchemaCore::COLUMN_TYPE_COUNT] = {
    32,
    64,
    8,
    8,
    16,
    16,
    32,
    64,
    1,
    32,
    32,
    32,
    32,
    32,
    32,
    0,
};

const int ELEMENT_COUNTS[SchemaCore::COLUMN_TYPE_COUNT] = {
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    2,
    3,
    4,
    4,
    4,
    1,
    1,
};

bool type_in_range(int column_type) {
    return column_type >= 0 && column_type < SchemaCore::COLUMN_TYPE_COUNT;
}

} // namespace

bool ColumnPlan::mistypes(const TailRef &p_tail) const {
    if (!variable || text || value_tag == value::TAG_COUNT
        || p_tail.is_empty()) {
        return false;
    }
    const uint8_t *held = p_tail.ptr();
    if (value::kind_of(value::Tag(held[0])) != value_tag) {
        return true;
    }
    if (element_tags[0] == value::TAG_COUNT) {
        return false;
    }
    const bool list = value_tag == value::TAG_ARRAY;
    const uint32_t parts = list ? 1 : 2;
    const uint8_t typed
        = list ? value::TAG_TYPED_ARRAY : value::TAG_TYPED_DICTIONARY;
    if (held[0] != typed || p_tail.size() <= parts) {
        return true;
    }
    return held[1] != element_tags[0]
        || (!list && held[2] != element_tags[1]);
}

int WirePlan::element_width(int column_type) {
    return type_in_range(column_type) ? ELEMENT_WIDTHS[column_type] : 0;
}

int WirePlan::element_count(int column_type) {
    return type_in_range(column_type) ? ELEMENT_COUNTS[column_type] : 0;
}

bool WirePlan::sized_by_value(int column_type) {
    return column_type == SchemaCore::VARIANT
        || column_type == SchemaCore::STRING;
}

WirePlan WirePlan::compile(const SchemaRecord &record) {
    NETW_ZONE_NC("WirePlan compile", colors::WIRE);
    WirePlan plan;
    if (!record.sealed) {
        return plan;
    }
    const int count = record.column_count();
    for (int index = 0; index < count; ++index) {
        const SchemaColumn *column = record.at(index);
        const int declared = column->stride > 0 ? column->stride : 1;
        ColumnPlan slot;
        slot.delta = column->delta;
        if (column->quant.quantized()) {
            slot.width = column->quant.width;
            slot.stride = declared * column->quant.parts;
        } else if (sized_by_value(column->type) && declared == 1) {
            slot.variable = true;
            slot.text = column->type == SchemaCore::STRING;
            slot.value_tag = column->value_tag;
            slot.element_tags[0] = column->element_tags[0];
            slot.element_tags[1] = column->element_tags[1];
            slot.width = 0;
            slot.stride = 0;
            slot.offset = plan.total_bits;
            plan.columns.push_back(slot);
            plan.carries_variable = true;
            continue;
        } else {
            slot.width = element_width(column->type);
            slot.stride = declared * element_count(column->type);
        }
        if (slot.width <= 0 || slot.stride <= 0) {
            return WirePlan();
        }
        slot.offset = plan.total_bits;
        plan.columns.push_back(slot);
        plan.total_bits += slot.bits();
    }
    plan.plannable = true;
    return plan;
}

bool is_scalar_schema(const SchemaRecord &p_schema, const WirePlan &p_plan) {
    if (!p_plan.valid()) {
        return false;
    }
    for (int at = 0; at < p_schema.column_count(); ++at) {
        const SchemaColumn *column = p_schema.at(at);
        if (column->stride != 1) {
            return false;
        }
    }
    return true;
}

} // namespace netw::wire
