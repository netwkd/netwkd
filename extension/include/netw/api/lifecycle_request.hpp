#pragma once

#include <cstdint>

#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/request.hpp"
#include "netw/lifecycle/rule.hpp"

namespace netw {

class NetwLifecycleRequest : public NetwRequest {
    GDCLASS(NetwLifecycleRequest, NetwRequest)

protected:
    static void _bind_methods();

public:
    enum Kind {
        KIND_SPAWN = int(lifecycle::Kind::SPAWN),
        KIND_DESPAWN = int(lifecycle::Kind::DESPAWN),
        KIND_REPARENT = int(lifecycle::Kind::REPARENT),
    };

    static godot::Ref<NetwLifecycleRequest> create(
        lifecycle::Kind p_kind,
        int64_t p_requester,
        const godot::Ref<NetwEntity> &p_entity,
        godot::Node *p_destination
    );

    Kind get_kind() const {
        return kind;
    }
    godot::Ref<NetwEntity> get_entity() const {
        return entity;
    }
    godot::Node *get_destination() const;

private:
    Kind kind = KIND_SPAWN;
    godot::Ref<NetwEntity> entity;
    godot::ObjectID destination;
};

} // namespace netw

VARIANT_ENUM_CAST(netw::NetwLifecycleRequest::Kind);
