#include "netw/api/admission_config.hpp"

#include "godot/class_db.hpp"
#include "netw/fault_book.hpp"
#include "netw/log.hpp"

using namespace godot;

namespace netw {

Ref<NetwAdmissionConfig> NetwAdmissionConfig::accept(
    const Ref<NetwAccountProvider> &p_provider
) {
    if (p_provider.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_EMPTY_ARGUMENT,
            {"{NetwAdmissionConfig.accept}",
             String(),
             "{NetwAccountProvider}"}
        );
        return Ref<NetwAdmissionConfig>(this);
    }
    providers.push_back(p_provider);
    return Ref<NetwAdmissionConfig>(this);
}

Ref<NetwAdmissionConfig> NetwAdmissionConfig::timeout(double p_seconds) {
    if (p_seconds <= 0.0) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {"{NetwAdmissionConfig.timeout}",
             vformat("%s seconds", String::num(p_seconds)),
             "a number of seconds above zero"}
        );
        return Ref<NetwAdmissionConfig>(this);
    }
    timeout_seconds = p_seconds;
    return Ref<NetwAdmissionConfig>(this);
}

void NetwAdmissionConfig::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("accept", "provider"),
        &NetwAdmissionConfig::accept
    );
    ClassDB::bind_method(
        D_METHOD("timeout", "seconds"),
        &NetwAdmissionConfig::timeout
    );
}

} // namespace netw
