#pragma once

#include <cstdint>

#include "godot/callable.hpp"
#include "godot/node.hpp"
#include "godot/object.hpp"
#include "godot/ref_counted.hpp"
#include "godot/rid.hpp"
#include "godot/variant.hpp"
#include "netw/api/entity_options.hpp"
#include "netw/api/promise.hpp"
#include "netw/comp_table.hpp"
#include "netw/display/decl.hpp"
#include "netw/entity/control.hpp"
#include "netw/entity/stage.hpp"
#include "netw/interest/decl.hpp"
#include "netw/sim/resolve.hpp"

namespace netw {

namespace spawn {
class Record;
} // namespace spawn

namespace predict {
struct Decl;
} // namespace predict

namespace sim {
struct Decl;
} // namespace sim

class NetwEntityRecord {
public:
    enum Part {
        PART_SCENE = 0,
        PART_INTEREST = 1,
        PART_PREDICTION = 2,
        PART_DISPLAY = 3,
        PART_PERSISTENCE = 4,
        PART_SIMULATION = 5,
        PART_MAX = 6,
    };

    static constexpr int RECORD_SLOTS = 4;

    struct HeldBinding {
        uint64_t sequence = 0;
        uint64_t binding = 0;
    };

    struct ControlClaim {
        uint64_t op = 0;
        godot::Ref<NetwPromise> promise;
    };

    struct StructureOp {
        uint64_t base = 0;
        godot::Ref<NetwPromise> promise;
    };

    struct HeldClaimImage {
        int64_t sender = 0;
        uint64_t observed_revision = 0;
        int64_t received_tick = -1;
        godot::PackedByteArray state;
    };

    struct ControlBook {
        HeldClaimImage held_claim_image;
        int64_t claim_image_tick = -1;
        int64_t claim_image_until = -1;
        godot::LocalVector<ControlClaim> control_claims;
        godot::LocalVector<StructureOp> structure_ops;
    };

private:
    ControlBook control_book;
    godot::LocalVector<HeldBinding> held_bindings;
    uint64_t record_bindings[RECORD_SLOTS] = {};
    godot::RID handle;
    godot::StringName entity_id;
    int64_t peer_id = 0;
    int64_t player_id = 0;
    int64_t route = 0;
    bool declares_scene = false;
    int64_t stage = int(entity::Stage::UNBOUND);
    entity::Control control;
    double replication_priority = 1.0;
    sim::Mode announced_mode = sim::Mode::NONE;
    struct MintedPart {
        int64_t part = 0;
        godot::Variant value;
    };
    godot::LocalVector<MintedPart> parts;
    godot::Ref<NetwDespawnOpts> despawning_opts;
    godot::StringName despawn_reason;
    interest::Facet *interest_facet = nullptr;
    NetwCompTable comp_table;
    godot::LocalVector<godot::ObjectID> comp_registered;
    display::Decl *display_decl = nullptr;
    predict::Decl *predict_decl = nullptr;
    sim::Decl *sim_decl = nullptr;
    godot::StringName scene_label;
    int64_t scene_isolation = -1;
    int64_t projected_follower = 0;
    int64_t projected_steerer = 0;
    spawn::Record *readmission = nullptr;
    bool predicted_at_departure = false;

    struct Projection {
        int64_t follower = SERVER_PEER;
        int64_t prior_follower = SERVER_PEER;
    };

    Projection projection_to(int64_t p_follower) const;
    void project_node(
        godot::Node *p_owner,
        godot::Node *p_node,
        const Projection &p_projection,
        uint64_t p_watch
    );
    void write_authority(godot::Node *p_node, int64_t p_to, int64_t p_prior);

public:
    NetwEntityRecord();
    ~NetwEntityRecord();

    NetwEntityRecord(const NetwEntityRecord &) = delete;
    NetwEntityRecord &operator=(const NetwEntityRecord &) = delete;

    godot::RID get_handle() const {
        return handle;
    }

    bool adopt_handle(const godot::RID &p_handle);

    godot::StringName get_entity_id() const {
        return entity_id;
    }
    void set_entity_id(const godot::StringName &p_entity_id) {
        entity_id = p_entity_id;
    }

    int64_t get_peer_id() const {
        return peer_id;
    }
    void set_peer_id(int64_t p_peer_id) {
        peer_id = p_peer_id;
    }

    int64_t get_player_id() const {
        return player_id;
    }
    void set_player_id(int64_t p_player_id) {
        player_id = p_player_id;
    }

    int64_t get_route() const {
        return route;
    }
    void set_route(int64_t p_route) {
        route = p_route;
    }

    godot::StringName scene_label_of(godot::Node *p_owner);
    int64_t scene_isolation_of(godot::Node *p_owner);
    godot::StringName get_scene_label() const {
        return scene_label;
    }
    void set_scene_label(const godot::StringName &p_label) {
        scene_label = p_label;
    }
    int64_t get_scene_isolation() const {
        return scene_isolation;
    }
    void set_scene_isolation(int64_t p_isolation) {
        scene_isolation = p_isolation;
    }

    bool get_declares_scene() const {
        return declares_scene;
    }
    void set_declares_scene(bool p_declares) {
        declares_scene = p_declares;
    }

    int64_t get_stage() const {
        return stage;
    }

    bool advance(int64_t p_stage);

    entity::Control *get_control() {
        return &control;
    }
    const entity::Control *get_control() const {
        return &control;
    }

    ControlBook &get_control_book() {
        return control_book;
    }
    const ControlBook &get_control_book() const {
        return control_book;
    }

    double get_replication_priority() const {
        return replication_priority;
    }
    void set_replication_priority(double p_weight) {
        replication_priority = p_weight;
    }

    sim::Mode get_announced_mode() const {
        return announced_mode;
    }
    void set_announced_mode(sim::Mode p_mode) {
        announced_mode = p_mode;
    }

    bool transition(int64_t p_stage);

    void deactivate(godot::Node *p_owner);

    bool publish_stage(godot::Object *p_wrapper, int64_t p_from);

    void apply_control(
        godot::Object *p_wrapper,
        godot::Node *p_owner,
        uint64_t p_watch = 0
    );

    void project_entered(
        godot::Node *p_owner,
        godot::Node *p_node,
        uint64_t p_watch
    );

    bool projects_to_server() const {
        return projected_follower == SERVER_PEER;
    }

    static godot::StringName entity_meta();

    void hydrate_identity(godot::Node *p_owner);

    bool set_controller(godot::Object *p_wrapper, int64_t p_peer);

    int64_t admit_control_request(
        godot::Object *p_wrapper,
        int64_t p_requester,
        int64_t p_hold,
        godot::Error &r_code,
        godot::String &r_reason
    );

    bool begin_despawn(
        godot::Object *p_wrapper,
        godot::Node *p_owner,
        const godot::Ref<NetwDespawnOpts> &p_opts
    );

    godot::Ref<NetwDespawnOpts> get_active_despawn_opts() const {
        return despawning_opts;
    }

    const godot::StringName &get_despawn_reason() const {
        return despawn_reason;
    }

    void set_despawn_reason(const godot::StringName &p_reason) {
        despawn_reason = p_reason;
    }

    bool finish_teardown(godot::Object *p_wrapper);
    bool complete_departure(godot::Object *p_wrapper);
    void readmit();

    void keep_readmission(const spawn::Record &p_recipe);
    spawn::Record *take_readmission();

    bool get_predicted_at_departure() const {
        return predicted_at_departure;
    }
    void set_predicted_at_departure(bool p_predicted) {
        predicted_at_departure = p_predicted;
    }

    godot::Variant part(int64_t p_part, godot::Object *p_wrapper);
    godot::Variant minted_part(int64_t p_part) const;
    void keep_part(int64_t p_part, const godot::Variant &p_value);
    uint32_t minted_part_count() const {
        return parts.size();
    }

    interest::Facet &get_interest_facet();
    interest::Facet *find_interest_facet() {
        return interest_facet;
    }
    const interest::Facet *find_interest_facet() const {
        return interest_facet;
    }

    NetwCompTable &get_comp_table() {
        return comp_table;
    }
    const NetwCompTable &get_comp_table() const {
        return comp_table;
    }

    godot::LocalVector<godot::ObjectID> &get_comp_registered() {
        return comp_registered;
    }
    const godot::LocalVector<godot::ObjectID> &get_comp_registered() const {
        return comp_registered;
    }

    godot::LocalVector<HeldBinding> &get_held_bindings() {
        return held_bindings;
    }

    int64_t held_bytes() const {
        return int64_t(sizeof(NetwEntityRecord)) + held::vector(held_bindings)
            + held::vector(parts) + comp_table.held_bytes()
            + held::vector(comp_registered)
            + (interest_facet != nullptr ? int64_t(sizeof(interest::Facet)) : 0)
            + (display_decl != nullptr ? int64_t(sizeof(display::Decl)) : 0)
            + predict_declaration_bytes() + sim_declaration_bytes();
    }
    int64_t predict_declaration_bytes() const;
    int64_t sim_declaration_bytes() const;
    uint64_t get_record_binding(int64_t p_record) const {
        return p_record >= 0 && p_record < RECORD_SLOTS
            ? record_bindings[p_record]
            : 0;
    }
    void set_record_binding(int64_t p_record, uint64_t p_binding) {
        if (p_record >= 0 && p_record < RECORD_SLOTS) {
            record_bindings[p_record] = p_binding;
        }
    }
    void forget_bindings() {
        held_bindings.clear();
        for (uint64_t &held : record_bindings) {
            held = 0;
        }
    }

    display::Decl &display_declaration();
    const display::Decl *find_display_declaration() const {
        return display_decl;
    }

    predict::Decl &predict_declaration();
    const predict::Decl *find_predict_declaration() const {
        return predict_decl;
    }
    predict::Decl *find_predict_declaration() {
        return predict_decl;
    }

    sim::Decl &sim_declaration();
    const sim::Decl *find_sim_declaration() const {
        return sim_decl;
    }

    static void set_part_factory(
        int64_t p_part,
        const godot::Callable &p_factory
    );
    static bool has_part_factory(int64_t p_part);
    static godot::Callable part_factory(int64_t p_part);
    static void clear_part_factories();
};

} // namespace netw
