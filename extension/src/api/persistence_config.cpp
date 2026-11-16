#include "netw/api/persistence_config.hpp"

#include "godot/class_db.hpp"
#include "netw/fault_book.hpp"

using namespace godot;

namespace netw {

Ref<NetwPersistenceConfig> NetwPersistenceConfig::database(
    const StringName &p_name
) {
    database_name = p_name;
    return Ref<NetwPersistenceConfig>(this);
}

Ref<NetwPersistenceConfig> NetwPersistenceConfig::table(
    const StringName &p_name
) {
    table_name = p_name;
    return Ref<NetwPersistenceConfig>(this);
}

Ref<NetwPersistenceConfig> NetwPersistenceConfig::storage_version(
    int p_version
) {
    if (p_version < 1) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwPersistenceConfig.storage_version}"),
             String::num_int64(p_version),
             String("a version from 1")},
            uint64_t(get_instance_id())
        );
        return Ref<NetwPersistenceConfig>(this);
    }
    version = p_version;
    return Ref<NetwPersistenceConfig>(this);
}

Ref<NetwPersistenceConfig> NetwPersistenceConfig::migrate(
    int p_from_version,
    const Callable &p_step
) {
    if (p_from_version < 1) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_VALUE,
            {String("{NetwPersistenceConfig.migrate}"),
             String::num_int64(p_from_version),
             String("a version from 1")},
            uint64_t(get_instance_id())
        );
        return Ref<NetwPersistenceConfig>(this);
    }
    if (p_step.is_null()) {
        fault::raise(
            nullptr,
            NETW_FAULT_BAD_CALLABLE,
            {String("{NetwPersistenceConfig.migrate}"),
             String(),
             String("a Callable that takes the record")},
            uint64_t(get_instance_id())
        );
        return Ref<NetwPersistenceConfig>(this);
    }
    migrations[p_from_version] = p_step;
    return Ref<NetwPersistenceConfig>(this);
}

Ref<NetwPersistenceConfig> NetwPersistenceConfig::record_id(
    const Callable &p_provider
) {
    id_provider = p_provider;
    return Ref<NetwPersistenceConfig>(this);
}

Ref<NetwPersistenceConfig> NetwPersistenceConfig::interval(double p_seconds) {
    save_interval = p_seconds;
    return Ref<NetwPersistenceConfig>(this);
}

Ref<NetwPersistenceConfig> NetwPersistenceConfig::load_on_spawn(
    bool p_enabled
) {
    load_at_spawn = p_enabled;
    return Ref<NetwPersistenceConfig>(this);
}

void NetwPersistenceConfig::_bind_methods() {
    ClassDB::bind_method(
        D_METHOD("database", "name"),
        &NetwPersistenceConfig::database
    );
    ClassDB::bind_method(
        D_METHOD("table", "name"),
        &NetwPersistenceConfig::table
    );
    ClassDB::bind_method(
        D_METHOD("storage_version", "version"),
        &NetwPersistenceConfig::storage_version
    );
    ClassDB::bind_method(
        D_METHOD("migrate", "from_version", "handler"),
        &NetwPersistenceConfig::migrate
    );
    ClassDB::bind_method(
        D_METHOD("record_id", "handler"),
        &NetwPersistenceConfig::record_id
    );
    ClassDB::bind_method(
        D_METHOD("interval", "seconds"),
        &NetwPersistenceConfig::interval
    );
    ClassDB::bind_method(
        D_METHOD("load_on_spawn", "enabled"),
        &NetwPersistenceConfig::load_on_spawn,
        DEFVAL(true)
    );
}

} // namespace netw
