#include "netw/sealed.hpp"

#include "netw/wire/code_row.hpp"

using namespace godot;

namespace netw::wire {

namespace {

uint64_t low_mask(int count) {
    return count >= 64 ? ~uint64_t(0) : ((uint64_t(1) << count) - 1);
}

uint64_t read_run(
    const LocalVector<uint64_t> &words,
    int64_t offset,
    int width
) {
    if (width <= 0) {
        return 0;
    }
    const int64_t word = offset >> 6;
    const int within = int(offset & 63);
    uint64_t value = words[word] >> within;
    const int taken = 64 - within;
    if (taken < width) {
        value |= words[word + 1] << taken;
    }
    return value & low_mask(width);
}

void write_run(
    LocalVector<uint64_t> &words,
    int64_t offset,
    int width,
    uint64_t value
) {
    if (width <= 0) {
        return;
    }
    const int64_t word = offset >> 6;
    const int within = int(offset & 63);
    const uint64_t mask = low_mask(width);
    words[word] &= ~(mask << within);
    words[word] |= (value & mask) << within;
    const int taken = 64 - within;
    if (taken < width) {
        words[word + 1] &= ~(mask >> taken);
        words[word + 1] |= (value & mask) >> taken;
    }
}

bool range_fits(int64_t capacity, int64_t offset, int width) {
    return offset >= 0 && width >= 0 && width <= 64
        && offset <= capacity - width;
}

bool runs_differ(
    const LocalVector<uint64_t> &before,
    const LocalVector<uint64_t> &after,
    int64_t offset,
    int64_t width
) {
    int64_t compared = 0;
    while (compared < width) {
        const int taking = int(width - compared < 64 ? width - compared : 64);
        if (read_run(before, offset + compared, taking)
            != read_run(after, offset + compared, taking)) {
            return true;
        }
        compared += taking;
    }
    return false;
}

uint32_t value_slots(const WirePlan &plan) {
    return plan.has_variable() ? plan.column_count() : 0;
}

} // namespace

CodeRow CodeRow::for_plan(const WirePlan &plan) {
    CodeRow row;
    if (!plan.valid()) {
        return row;
    }
    row.used_bits = plan.row_bits();
    const int64_t needed = (plan.row_bits() + 63) / 64 + 1;
    row.words.resize(uint32_t(needed));
    for (uint32_t index = 0; index < row.words.size(); ++index) {
        row.words[index] = 0;
    }
    row.tails.resize(value_slots(plan));
    row.texts.resize(value_slots(plan));
    for (uint32_t index = 0; index < row.texts.size(); ++index) {
        row.texts[index] = plan.column(index).text ? 1 : 0;
    }
    return row;
}

bool CodeRow::assign(
    const WirePlan &plan,
    const uint64_t *p_words,
    const TailRef *p_tails
) {
    if (p_words == nullptr || !plan.valid()) {
        return false;
    }
    if (!valid_for(plan)) {
        *this = for_plan(plan);
    }
    for (uint32_t index = 0; index < words.size(); ++index) {
        words[index] = p_words[index];
    }
    for (uint32_t index = 0; index < tails.size(); ++index) {
        tails[index] = p_tails != nullptr ? p_tails[index] : TailRef();
    }
    return true;
}

bool CodeRow::valid_for(const WirePlan &plan) const {
    if (!plan.valid() || used_bits != plan.row_bits()
        || words.size() != uint32_t((used_bits + 63) / 64 + 1)
        || tails.size() != value_slots(plan)) {
        return false;
    }
    for (uint32_t index = 0; index < texts.size(); ++index) {
        if ((texts[index] != 0) != plan.column(index).text) {
            return false;
        }
    }
    return true;
}

void CodeRow::clear() {
    for (uint32_t index = 0; index < words.size(); ++index) {
        words[index] = 0;
    }
    clear_values();
}

void CodeRow::clear_values() {
    for (uint32_t index = 0; index < tails.size(); ++index) {
        tails[index] = TailRef();
    }
}

void CodeRow::copy_from(const CodeRow &other) {
    if (words.size() != other.words.size()) {
        words.resize(other.words.size());
    }
    for (uint32_t index = 0; index < words.size(); ++index) {
        words[index] = other.words[index];
    }
    if (tails.size() != other.tails.size()) {
        tails.resize(other.tails.size());
    }
    for (uint32_t index = 0; index < tails.size(); ++index) {
        tails[index] = other.tails[index];
    }
    texts = other.texts;
    used_bits = other.used_bits;
}

bool CodeRow::write(const ColumnPlan &slot, int element, uint64_t code) {
    if (element < 0 || element >= slot.stride || slot.width > 64) {
        return false;
    }
    if ((code & ~low_mask(slot.width)) != 0) {
        return false;
    }
    return write_bits(
        slot.offset + int64_t(element) * slot.width,
        slot.width,
        code
    );
}

uint64_t CodeRow::read(const ColumnPlan &slot, int element) const {
    if (element < 0 || element >= slot.stride || slot.width > 64) {
        return 0;
    }
    return read_bits(slot.offset + int64_t(element) * slot.width, slot.width);
}

bool CodeRow::write_bits(int64_t offset, int width, uint64_t code) {
    if (!range_fits(used_bits, offset, width)
        || (code & ~low_mask(width)) != 0) {
        return false;
    }
    write_run(words, offset, width, code);
    return true;
}

uint64_t CodeRow::read_bits(int64_t offset, int width) const {
    if (!range_fits(used_bits, offset, width)) {
        return 0;
    }
    return read_run(words, offset, width);
}

bool CodeRow::write_tail(uint32_t column, const TailRef &tail) {
    if (column >= tails.size()) {
        return false;
    }
    tails[column] = tail;
    return true;
}

const TailRef &CodeRow::tail(uint32_t column) const {
    static const TailRef empty;
    return column < tails.size() ? tails[column] : empty;
}

bool CodeRow::is_text(uint32_t column) const {
    return column < texts.size() && texts[column] != 0;
}

uint64_t CodeRow::changed_mask(
    const WirePlan &plan,
    const CodeRow &before,
    const CodeRow &after
) {
    if (!before.valid_for(plan) || !after.valid_for(plan)) {
        return plan.full_mask();
    }
    uint64_t mask = 0;
    for (uint32_t index = 0; index < plan.column_count(); ++index) {
        const ColumnPlan &slot = plan.column(index);
        if (slot.variable) {
            if (before.tails[index] != after.tails[index]) {
                mask |= uint64_t(1) << index;
            }
            continue;
        }
        for (int element = 0; element < slot.stride; ++element) {
            const int64_t at = slot.offset + int64_t(element) * slot.width;
            if (runs_differ(before.words, after.words, at, slot.width)) {
                mask |= uint64_t(1) << index;
                break;
            }
        }
    }
    return mask;
}

bool CodeRow::equals(const CodeRow &other) const {
    if (used_bits != other.used_bits || words.size() != other.words.size()
        || tails.size() != other.tails.size()) {
        return false;
    }
    for (uint32_t index = 0; index < words.size(); ++index) {
        if (words[index] != other.words[index]) {
            return false;
        }
    }
    for (uint32_t index = 0; index < tails.size(); ++index) {
        if (tails[index] != other.tails[index]) {
            return false;
        }
    }
    return true;
}

} // namespace netw::wire
