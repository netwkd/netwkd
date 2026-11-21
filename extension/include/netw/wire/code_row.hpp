#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"
#include "netw/held_bytes.hpp"
#include "netw/wire/plan.hpp"
#include "netw/wire/tail.hpp"

namespace netw::wire {

class CodeRow {
    godot::LocalVector<uint64_t> words;
    godot::LocalVector<TailRef> tails;
    godot::LocalVector<uint8_t> texts;
    int64_t used_bits = 0;

public:
    static CodeRow for_plan(const WirePlan &plan);

    uint32_t word_count() const {
        return words.size();
    }

    bool is_empty() const {
        return words.is_empty();
    }

    const uint64_t *word_data() const {
        return words.ptr();
    }

    const TailRef *tail_data() const {
        return tails.ptr();
    }

    int64_t held_bytes() const {
        return held::vector(words) + held::vector(tails) + held::vector(texts);
    }

    bool assign(
        const WirePlan &plan,
        const uint64_t *p_words,
        const TailRef *p_tails
    );

    bool valid_for(const WirePlan &plan) const;

    void clear();
    void clear_values();

    void copy_from(const CodeRow &other);

    bool write(const ColumnPlan &slot, int element, uint64_t code);
    uint64_t read(const ColumnPlan &slot, int element) const;

    bool write_bits(int64_t offset, int width, uint64_t code);
    uint64_t read_bits(int64_t offset, int width) const;

    bool write_tail(uint32_t column, const TailRef &tail);
    const TailRef &tail(uint32_t column) const;
    bool is_text(uint32_t column) const;

    static uint64_t changed_mask(
        const WirePlan &plan,
        const CodeRow &before,
        const CodeRow &after
    );

    bool equals(const CodeRow &other) const;
};

} // namespace netw::wire
