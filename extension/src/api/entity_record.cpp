#include "netw/api/entity_record.hpp"

#include "netw/entity/ids.hpp"
#include "netw/entity/stage.hpp"
#include "netw/log.hpp"
#include "netw/predict/decl.hpp"
#include "netw/sim/row.hpp"
#include "netw/spawn/record.hpp"

#if defined(NETW_TESTS)
#include <unordered_set>
#endif

using namespace godot;

namespace netw {

#if defined(NETW_TESTS)
namespace {

std::unordered_set<const NetwEntityRecord *> &live_records() {
    static std::unordered_set<const NetwEntityRecord *> live;
    return live;
}

} // namespace

bool NetwEntityRecord::is_live(const NetwEntityRecord *p_record) {
    return live_records().count(p_record) > 0;
}
#endif

NetwEntityRecord::NetwEntityRecord() {
#if defined(NETW_TESTS)
    live_records().insert(this);
#endif
}

NetwEntityRecord::~NetwEntityRecord() {
#if defined(NETW_TESTS)
    live_records().erase(this);
#endif
    if (interest_facet != nullptr) {
        memdelete(interest_facet);
    }
    if (display_decl != nullptr) {
        memdelete(display_decl);
    }
    if (predict_decl != nullptr) {
        memdelete(predict_decl);
    }
    if (sim_decl != nullptr) {
        memdelete(sim_decl);
    }
    if (readmission != nullptr) {
        memdelete(readmission);
    }
    entity::release(handle);
}

void NetwEntityRecord::keep_readmission(const spawn::Record &p_recipe) {
    if (readmission == nullptr) {
        readmission = memnew(spawn::Record(p_recipe));
        return;
    }
    *readmission = p_recipe;
}

spawn::Record *NetwEntityRecord::take_readmission() {
    spawn::Record *taken = readmission;
    readmission = nullptr;
    return taken;
}

interest::Facet &NetwEntityRecord::get_interest_facet() {
    if (interest_facet == nullptr) {
        interest_facet = memnew(interest::Facet);
    }
    return *interest_facet;
}

display::Decl &NetwEntityRecord::display_declaration() {
    if (display_decl == nullptr) {
        display_decl = memnew(display::Decl);
    }
    return *display_decl;
}

predict::Decl &NetwEntityRecord::predict_declaration() {
    if (predict_decl == nullptr) {
        predict_decl = memnew(predict::Decl);
    }
    return *predict_decl;
}

int64_t NetwEntityRecord::predict_declaration_bytes() const {
    return predict_decl != nullptr ? int64_t(sizeof(predict::Decl)) : 0;
}

sim::Decl &NetwEntityRecord::sim_declaration() {
    if (sim_decl == nullptr) {
        sim_decl = memnew(sim::Decl);
    }
    return *sim_decl;
}

int64_t NetwEntityRecord::sim_declaration_bytes() const {
    return sim_decl != nullptr ? int64_t(sizeof(sim::Decl)) : 0;
}

bool NetwEntityRecord::adopt_handle(const RID &p_handle) {
    if (p_handle == handle) {
        return true;
    }
    if (!entity::retain(p_handle)) {
        return false;
    }
    entity::release(handle);
    handle = p_handle;
    return true;
}

bool NetwEntityRecord::advance(int64_t p_stage) {
    if (!entity::stage_edge_is_legal(stage, p_stage)) {
        return false;
    }
    stage = p_stage;
    return true;
}

bool NetwEntityRecord::transition(int64_t p_stage) {
    if (advance(p_stage)) {
        return true;
    }
    NETW_BUG_V(false);
}

StringName NetwEntityRecord::entity_meta() {
    return StringName("netw_entity");
}

void NetwEntityRecord::readmit() {
    stage = int64_t(entity::Stage::UNBOUND);
    route = 0;
    entity::release(handle);
    handle = RID();
    despawn_reason = StringName();
    projected_follower = 0;
}

} // namespace netw
