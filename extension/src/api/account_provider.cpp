#include "netw/api/account_provider.hpp"

#include "godot/class_db.hpp"
#include "netw/game_call.hpp"

using namespace godot;

namespace netw {

Variant NetwAccountProvider::prove(
    const String &p_challenge,
    const Ref<NetwSession> &p_session
) {
    Variant answer;
    if (GDVIRTUAL_CALL(_prove, p_challenge, p_session, answer)) {
        return answer;
    }
    return Variant();
}

Variant NetwAccountProvider::verify(
    const Ref<NetwJoinRequest> &p_request,
    const Variant &p_proof
) {
    const StringName method("_verify");
    if (!has_method(method)) {
        return Variant();
    }
    Array args;
    args.push_back(p_request);
    args.push_back(p_proof);
    return game_call(Callable(this, method), args, game_calls::ACCOUNT_VERIFY);
}

void NetwAccountProvider::_bind_methods() {
    GDVIRTUAL_BIND(_prove, "challenge", "session");
    GDVIRTUAL_BIND(_verify, "request", "proof");
}

} // namespace netw
