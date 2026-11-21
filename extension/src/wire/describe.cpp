#include "netw/sealed.hpp"

#include "netw/wire/describe.hpp"

namespace netw::wire::detail {

const char *spec_kind_name(SpecKind kind) {
    switch (kind) {
        case SpecKind::BITS:
            return "bits";
        case SpecKind::INT_RANGE:
            return "int_range";
        case SpecKind::VARUINT:
            return "varuint";
        case SpecKind::SVARINT:
            return "svarint";
        case SpecKind::BOOL1:
            return "bool1";
        case SpecKind::BYTES_CAPPED:
            return "bytes_capped";
        case SpecKind::STRING:
            return "string";
    }
    return "unknown";
}

} // namespace netw::wire::detail
