#pragma once

#include "godot/resource.hpp"
#include "godot/variant.hpp"
#include "netw/api/database_backend.hpp"

namespace netw {

class NetwDatabaseConfig : public godot::Resource {
    GDCLASS(NetwDatabaseConfig, godot::Resource)

    godot::Ref<NetwDatabaseBackend> store;
    bool refused = false;

protected:
    static void _bind_methods();

public:
    void refuse() {
        refused = true;
    }
    bool is_refused() const {
        return refused;
    }

    godot::Ref<NetwDatabaseConfig> backend(
        const godot::Ref<NetwDatabaseBackend> &p_backend
    );
    godot::Ref<NetwDatabaseBackend> get_backend() const {
        return store;
    }
};

} // namespace netw
