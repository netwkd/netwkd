#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"

namespace netw::wire {

struct Tail {
    godot::LocalVector<uint8_t> bytes;
    uint32_t holders = 0;
};

class TailRef {
    Tail *tail = nullptr;

    void release();

public:
    TailRef() = default;
    TailRef(const TailRef &p_other);
    TailRef(TailRef &&p_other);
    TailRef &operator=(const TailRef &p_other);
    TailRef &operator=(TailRef &&p_other);
    ~TailRef();

    static TailRef of(const uint8_t *p_bytes, uint32_t p_size);

    bool is_empty() const {
        return tail == nullptr;
    }

    const uint8_t *ptr() const {
        return tail != nullptr ? tail->bytes.ptr() : nullptr;
    }

    uint32_t size() const {
        return tail != nullptr ? tail->bytes.size() : 0;
    }

    uint32_t holders() const {
        return tail != nullptr ? tail->holders : 0;
    }

    bool same_bytes(const uint8_t *p_bytes, uint32_t p_size) const;

    bool operator==(const TailRef &p_other) const;

    bool operator!=(const TailRef &p_other) const {
        return !(*this == p_other);
    }
};

} // namespace netw::wire
