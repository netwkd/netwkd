#pragma once

#include <cstdint>

#include "godot/hash_map.hpp"
#include "godot/local_vector.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/rid.hpp"
#include "godot/string_name.hpp"

namespace netw {
class NetwClock;
class NetwConnection;
class NetwDatabase;
class NetwEndpoint;
class NetwInterestLayer;
class NetwPlayer;
class NetwSession;
class NetwTable;
} // namespace netw

namespace netw::session {

struct EndpointView {
    godot::Ref<NetwEndpoint> view;
    uint32_t pins = 0;
    bool dropped = false;
};

struct ServiceRow {
    godot::ObjectID type;
    godot::ObjectID service;
    godot::StringName class_key;
};

struct Views {
    godot::Ref<NetwConnection> connection_handle;
    godot::Ref<NetwSession> session_handle;
    godot::Ref<NetwClock> clock_handle;
    godot::HashMap<godot::RID, godot::Ref<NetwInterestLayer>> layer_views;
    godot::HashMap<godot::RID, EndpointView> endpoint_views;
    godot::HashMap<godot::RID, godot::Ref<NetwDatabase>> database_handles;
    godot::HashMap<godot::RID, godot::Ref<NetwTable>> table_handles;
    godot::LocalVector<ServiceRow> services;
    mutable godot::HashMap<uint64_t, godot::StringName> native_class_names;
    mutable godot::HashMap<int64_t, godot::Ref<NetwPlayer>> player_views;
};

} // namespace netw::session
