#include "netw/api/database_connection.hpp"

#include "godot/class_db.hpp"

using namespace godot;

namespace netw {

namespace {

Ref<NetwPromise> unimplemented(const char *p_verb) {
    return NetwPromise::rejected(
        ERR_UNAVAILABLE,
        String("this connection implements no ") + p_verb
    );
}

} // namespace

Ref<NetwPromise> NetwDatabaseConnection::read(const Dictionary &p_address) {
    Ref<NetwPromise> answer;
    if (GDVIRTUAL_CALL(_read, p_address, answer)) {
        return answer.is_valid() ? answer
                                 : NetwPromise::rejected(
                                       ERR_INVALID_DATA,
                                       "_read answered no promise"
                                   );
    }
    return unimplemented("_read");
}

Ref<NetwPromise> NetwDatabaseConnection::scan(const Dictionary &p_request) {
    Ref<NetwPromise> answer;
    if (GDVIRTUAL_CALL(_scan, p_request, answer)) {
        return answer.is_valid() ? answer
                                 : NetwPromise::rejected(
                                       ERR_INVALID_DATA,
                                       "_scan answered no promise"
                                   );
    }
    return unimplemented("_scan");
}

Ref<NetwPromise> NetwDatabaseConnection::write_batch(
    const Array &p_operations
) {
    Ref<NetwPromise> answer;
    if (GDVIRTUAL_CALL(_write_batch, p_operations, answer)) {
        return answer.is_valid() ? answer
                                 : NetwPromise::rejected(
                                       ERR_INVALID_DATA,
                                       "_write_batch answered no promise"
                                   );
    }
    return unimplemented("_write_batch");
}

Ref<NetwPromise> NetwDatabaseConnection::close() {
    Ref<NetwPromise> answer;
    if (GDVIRTUAL_CALL(_close, answer)) {
        return answer.is_valid() ? answer : NetwPromise::resolved(OK);
    }
    return NetwPromise::resolved(OK);
}

void NetwDatabaseConnection::_bind_methods() {
    GDVIRTUAL_BIND(_read, "address");
    GDVIRTUAL_BIND(_scan, "request");
    GDVIRTUAL_BIND(_write_batch, "operations");
    GDVIRTUAL_BIND(_close);
}

} // namespace netw
