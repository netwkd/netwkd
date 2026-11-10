#pragma once

#include "godot/gdvirtual.hpp"
#include "godot/ref_counted.hpp"
#include "godot/variant.hpp"
#include "netw/api/promise.hpp"

namespace netw {

class NetwDatabaseConnection : public godot::RefCounted {
    GDCLASS(NetwDatabaseConnection, godot::RefCounted)

protected:
    static void _bind_methods();

public:
    virtual godot::Ref<NetwPromise> read(const godot::Dictionary &p_address);
    virtual godot::Ref<NetwPromise> scan(const godot::Dictionary &p_request);
    virtual godot::Ref<NetwPromise> write_batch(
        const godot::Array &p_operations
    );
    virtual godot::Ref<NetwPromise> close();

    GDVIRTUAL1R(godot::Ref<NetwPromise>, _read, const godot::Dictionary &)
    GDVIRTUAL1R(godot::Ref<NetwPromise>, _scan, const godot::Dictionary &)
    GDVIRTUAL1R(godot::Ref<NetwPromise>, _write_batch, const godot::Array &)
    GDVIRTUAL0R(godot::Ref<NetwPromise>, _close)
};

} // namespace netw
