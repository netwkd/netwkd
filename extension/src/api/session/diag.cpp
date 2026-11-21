#include "netw/api/netw_multiplayer.hpp"

#if defined(DEBUG_ENABLED)

#include "godot/engine.hpp"
#include "netw/api/entity.hpp"
#include "netw/api/entity_record.hpp"
#include "netw/api/predict_slot_engine.hpp"
#include "netw/colors.hpp"
#include "netw/profile.hpp"

namespace netw {

using namespace godot;

void NetwMultiplayer::diag_poll() {
    if (!diag_views_decided) {
        diag_views_decided = true;
        if (diag::PredictionRings::wanted()) {
            prediction_rings = std::make_unique<diag::PredictionRings>();
        }
        if (diag::InterpolationView::wanted()) {
            interpolation_view = std::make_unique<diag::InterpolationView>();
            interpolation_view->rings_drawn = prediction_rings != nullptr;
        }
        if (prediction_rings != nullptr || interpolation_view != nullptr) {
            diag_sketch = std::make_unique<diag::Sketch>();
        }
    }
    if (diag_sketch == nullptr) {
        return;
    }
    const uint64_t frame = Engine::get_singleton()->get_process_frames();
    if (frame == diag_polled_frame) {
        return;
    }
    diag_polled_frame = frame;
    diag_drawing.clear();
    if (prediction_rings != nullptr) {
        NETW_ZONE_NC("Prediction rings sample", colors::PREDICTION);
        diag_gather(prediction_rings->sources);
        prediction_rings->sample(
            world.clock,
            store(),
            &prediction_engine,
            *this
        );
        NETW_ZONE_VALUE(prediction_rings->rows().size());
        diag::draw_ring_rows(prediction_rings->rows(), diag_drawing);
    }
    if (interpolation_view != nullptr) {
        {
            NETW_ZONE_NC("Interpolation view sample", colors::INTERP);
            diag_gather(interpolation_view->sources);
            interpolation_view->sample(
                world.clock,
                clock_reading(),
                store(),
                &prediction_engine
            );
            NETW_ZONE_VALUE(interpolation_view->rows().size());
        }
        NETW_ZONE_NC("Interpolation view draw", colors::INTERP);
        diag::draw_timelines(
            interpolation_view->rows(),
            interpolation_view->marks(),
            diag_drawing
        );
        for (const diag::Ghost &each : interpolation_view->ghosts()) {
            diag_drawing.ghosts.push_back(each);
        }
    }
    NETW_ZONE_NC("Diag sketch draw", colors::SESSION);
    diag_sketch->draw(diag_drawing);
}

void NetwMultiplayer::diag_gather(
    LocalVector<diag::InterpSource> &r_sources
) const {
    r_sources.clear();
    const LocalVector<display::Runtime *> &live = display_book.live_runtimes();
    r_sources.reserve(live.size());
    for (display::Runtime *runtime : live) {
        Node *owner = runtime != nullptr ? runtime->owner() : nullptr;
        if (owner == nullptr) {
            continue;
        }
        diag::InterpSource source;
        source.route = runtime->get_route();
        source.runtime = runtime;
        source.owner = owner;
        const Ref<NetwEntity> view = slot_pinned_entity(runtime->entity_rid());
        if (view.is_valid()) {
            source.record = view->get_record();
            source.controlled_here = view->get_is_controlled_locally();
        }
        r_sources.push_back(source);
    }
}

void NetwMultiplayer::diag_gather(
    LocalVector<diag::RingSource> &r_sources
) const {
    r_sources.clear();
    r_sources.reserve(predict_engines.size());
    for (const KeyValue<int64_t, NetwPredictSlotEngine *> &row :
         predict_engines) {
        NetwPredictSlotEngine *engine = row.value;
        Node *owner = engine != nullptr ? engine->seated_node() : nullptr;
        const int64_t slot = engine != nullptr ? engine->native_slot() : -1;
        if (owner == nullptr || slot < 0) {
            continue;
        }
        const NetwEntityRecord *record = engine->held_record();
        const RID entity = record->get_handle();
        const display::Decl *authored = record->find_display_declaration();
        diag::RingSource source;
        source.key = row.key;
        source.visual_root = entity_get_view(entity).is_valid()
            ? display_book.decl_of(entity).visual_root
            : authored != nullptr ? authored->visual_root
                                  : NodePath();
        source.slot = slot;
        source.owner = owner;
        r_sources.push_back(source);
    }
}

} // namespace netw

#endif
