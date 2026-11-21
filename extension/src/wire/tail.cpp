#include "netw/sealed.hpp"

#include "netw/wire/tail.hpp"

#include <cstring>

namespace netw::wire {

void TailRef::release() {
    if (tail == nullptr) {
        return;
    }
    tail->holders -= 1;
    if (tail->holders == 0) {
        delete tail;
    }
    tail = nullptr;
}

TailRef::TailRef(const TailRef &p_other) : tail(p_other.tail) {
    if (tail != nullptr) {
        tail->holders += 1;
    }
}

TailRef::TailRef(TailRef &&p_other) : tail(p_other.tail) {
    p_other.tail = nullptr;
}

TailRef &TailRef::operator=(const TailRef &p_other) {
    if (tail == p_other.tail) {
        return *this;
    }
    release();
    tail = p_other.tail;
    if (tail != nullptr) {
        tail->holders += 1;
    }
    return *this;
}

TailRef &TailRef::operator=(TailRef &&p_other) {
    if (this == &p_other) {
        return *this;
    }
    release();
    tail = p_other.tail;
    p_other.tail = nullptr;
    return *this;
}

TailRef::~TailRef() {
    release();
}

TailRef TailRef::of(const uint8_t *p_bytes, uint32_t p_size) {
    TailRef made;
    if (p_size == 0) {
        return made;
    }
    made.tail = new Tail();
    made.tail->bytes.resize(p_size);
    memcpy(made.tail->bytes.ptr(), p_bytes, p_size);
    made.tail->holders = 1;
    return made;
}

bool TailRef::same_bytes(const uint8_t *p_bytes, uint32_t p_size) const {
    return size() == p_size
        && (p_size == 0 || memcmp(ptr(), p_bytes, p_size) == 0);
}

bool TailRef::operator==(const TailRef &p_other) const {
    return tail == p_other.tail || p_other.same_bytes(ptr(), size());
}

} // namespace netw::wire
