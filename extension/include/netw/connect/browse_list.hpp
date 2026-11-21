#pragma once

#include <cstdint>

#include "godot/local_vector.hpp"
#include "godot/object.hpp"
#include "godot/rid.hpp"
#include "godot/variant.hpp"
#include "netw/api/server_info.hpp"

namespace netw::connect {

godot::TypedArray<godot::Dictionary> targets_of(
    const godot::PackedStringArray &p_addresses,
    const godot::PackedStringArray &p_names,
    const godot::Array &p_infos
);

struct TargetRow {
    godot::RID transport;
    godot::String address;
    godot::String display_name;
    godot::Dictionary metadata;
    godot::Ref<NetwServerInfo> advertised;
    godot::StringName directory;
    godot::ObjectID source;
    int64_t status = 0;
    godot::Ref<NetwServerInfo> info;
    bool observed = false;
    bool invited = false;
    bool invite_pending = false;
    godot::String invited_by;
    double next_probe = 0.0;
    bool probing = false;
};

class BrowseList {
public:
    static const int MAX_IN_FLIGHT_PROBES = 6;

    BrowseList();
    ~BrowseList();

    void set_local_app_id(const godot::StringName &p_app_id);

    godot::RID add(
        const godot::RID &p_transport,
        const godot::String &p_address,
        const godot::String &p_display_name
    );
    bool remove(const godot::RID &p_target);
    godot::RID find(
        const godot::RID &p_transport,
        const godot::String &p_address
    ) const;
    bool is_removable(const godot::RID &p_target) const;
    bool holds(const godot::RID &p_target) const;
    godot::RID hold_current(
        const godot::RID &p_transport,
        const godot::String &p_address,
        godot::RID &r_dropped
    );
    godot::RID drop_current();

    TargetRow *row(const godot::RID &p_target);
    const TargetRow *row(const godot::RID &p_target) const;

    godot::TypedArray<godot::RID> list() const;
    int size() const;

    void publish_directory(
        const godot::StringName &p_directory,
        godot::ObjectID p_source,
        const godot::RID &p_transport,
        const godot::TypedArray<godot::Dictionary> &p_targets,
        godot::LocalVector<godot::RID> &r_dropped,
        godot::LocalVector<godot::RID> &r_added,
        godot::LocalVector<godot::RID> &r_updated
    );
    godot::RID invite(
        const godot::StringName &p_directory,
        godot::ObjectID p_source,
        const godot::RID &p_transport,
        const godot::String &p_address,
        bool &r_minted
    );
    void forget_source(
        godot::ObjectID p_source,
        godot::LocalVector<godot::RID> &r_dropped
    );

    void enqueue(const godot::RID &p_target);
    void arm_caller_rows();
    void enqueue_due(double p_elapsed, double p_interval);
    void drop_probes();
    bool next_to_probe(godot::RID &r_target);
    bool begin_probe(const godot::RID &p_target);
    void probe_settled(
        const godot::RID &p_target,
        int64_t p_error,
        const godot::Ref<NetwServerInfo> &p_info
    );
    int in_flight() const {
        return probes_in_flight;
    }

    int64_t classify(
        const godot::Ref<NetwServerInfo> &p_info,
        int64_t p_error
    ) const;

private:
    mutable godot::RID_Owner<TargetRow> targets;
    godot::StringName local_app_id;
    godot::LocalVector<godot::RID> caller_rows;
    godot::LocalVector<godot::RID> directory_rows;
    godot::LocalVector<godot::RID> queue;
    godot::RID current;
    int probes_in_flight = 0;

    godot::RID mint(const TargetRow &p_row);
    void release(const godot::RID &p_target);
};

} // namespace netw::connect
