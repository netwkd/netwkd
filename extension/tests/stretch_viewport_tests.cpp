#include "support/netw_call_log.h"
#include "support/netw_cells.h"
#include "support/netw_test.h"

#include "godot/gui.hpp"
#include "godot/input_event.hpp"
#include "godot/scene_tree.hpp"
#include "godot/spatial_node.hpp"
#include "godot/viewport.hpp"
#include "netw/api/nodes/view/stretch_viewport_container.hpp"

namespace TestNetwStretchViewport {

using namespace godot;
using netw::StretchViewportContainer;
using netw_test::law_broken;
using netw_test::law_held;
using netw_test::LawVerdict;

struct Stand {
    String label;
};

typedef netw_test::LawRowFor<Stand> Row;

template <typename T> struct Mounted {
    T *node = nullptr;

    explicit Mounted(T *p_node) : node(p_node) {
        netw::gd::scene_root()->add_child(node);
    }

    ~Mounted() {
        netw::gd::scene_root()->remove_child(node);
        memdelete(node);
    }

    T *operator->() const {
        return node;
    }
};

template <typename T> T *added(Node *p_parent, T *p_child) {
    p_parent->add_child(p_child);
    return p_child;
}

Window *canvas_items_window(const Vector2i &p_size, const Vector2i &p_design) {
    Window *window = memnew(Window);
    window->set_size(p_size);
    window->set_content_scale_mode(Window::CONTENT_SCALE_MODE_CANVAS_ITEMS);
    window->set_content_scale_aspect(Window::CONTENT_SCALE_ASPECT_KEEP);
    window->set_content_scale_size(p_design);
    return window;
}

SubViewport *viewport_sized(const Vector2i &p_size) {
    SubViewport *viewport = memnew(SubViewport);
    viewport->set_size(p_size);
    return viewport;
}

bool renders(SubViewport *p_target, int p_width, int p_height) {
    return p_target->get_size() == Vector2i(p_width, p_height);
}

bool draws_as(SubViewport *p_target, int p_width, int p_height) {
    return p_target->get_size_2d_override() == Vector2i(p_width, p_height);
}

LawVerdict fills_canvas_items_window(const Stand &) {
    Mounted<Window> window(
        canvas_items_window(Vector2i(1280, 720), Vector2i(640, 360))
    );
    StretchViewportContainer *view
        = added(window.node, memnew(StretchViewportContainer));
    SubViewport *target = added(view, memnew(SubViewport));

    if (!renders(target, 1280, 720) || !draws_as(target, 640, 360)
        || !target->is_size_2d_override_stretch_enabled()) {
        return law_broken(
            "the seat rendered %dx%d drawing %dx%d",
            target->get_size().x,
            target->get_size().y,
            target->get_size_2d_override().x,
            target->get_size_2d_override().y
        );
    }

    window->set_content_scale_size(Vector2i(480, 360));

    if (!renders(target, 960, 720) || !draws_as(target, 480, 360)) {
        return law_broken(
            "after a new design the seat rendered %dx%d drawing %dx%d",
            target->get_size().x,
            target->get_size().y,
            target->get_size_2d_override().x,
            target->get_size_2d_override().y
        );
    }
    return law_held();
}

LawVerdict follows_resized_viewport(const Stand &) {
    Mounted<SubViewport> host(viewport_sized(Vector2i(800, 600)));
    Node2D *level = added(host.node, memnew(Node2D));
    StretchViewportContainer *view
        = added(level, memnew(StretchViewportContainer));
    SubViewport *target = added(view, memnew(SubViewport));

    if (!renders(target, 800, 600)) {
        return law_broken(
            "the seat rendered %dx%d in an 800x600 viewport",
            target->get_size().x,
            target->get_size().y
        );
    }

    host->set_size(Vector2i(1024, 768));

    if (!renders(target, 1024, 768)) {
        return law_broken(
            "the seat rendered %dx%d after the viewport grew to 1024x768",
            target->get_size().x,
            target->get_size().y
        );
    }
    return law_held();
}

LawVerdict follows_anchoring_control(const Stand &) {
    Mounted<Window> window(
        canvas_items_window(Vector2i(1280, 720), Vector2i(320, 180))
    );
    Control *half = added(window.node, memnew(Control));
    half->set_size(Vector2(800, 600));
    StretchViewportContainer *view
        = added(half, memnew(StretchViewportContainer));
    SubViewport *target = added(view, memnew(SubViewport));
    view->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);

    if (!renders(target, 800, 600) || !draws_as(target, 320, 240)) {
        return law_broken(
            "an 800x600 seat rendered %dx%d drawing %dx%d",
            target->get_size().x,
            target->get_size().y,
            target->get_size_2d_override().x,
            target->get_size_2d_override().y
        );
    }

    half->set_size(Vector2(400, 300));

    if (!renders(target, 400, 300)) {
        return law_broken(
            "a 400x300 seat rendered %dx%d",
            target->get_size().x,
            target->get_size().y
        );
    }

    half->set_size(Vector2(1000, 500));

    if (!renders(target, 1000, 500) || !draws_as(target, 360, 180)) {
        return law_broken(
            "a 1000x500 seat rendered %dx%d drawing %dx%d",
            target->get_size().x,
            target->get_size().y,
            target->get_size_2d_override().x,
            target->get_size_2d_override().y
        );
    }
    return law_held();
}

LawVerdict click_lands_under_cursor(const Stand &) {
    Mounted<Window> window(
        canvas_items_window(Vector2i(1280, 720), Vector2i(320, 180))
    );
    Control *half = added(window.node, memnew(Control));
    StretchViewportContainer *view
        = added(half, memnew(StretchViewportContainer));
    view->set_size(Vector2(640, 480));
    SubViewport *target = added(view, memnew(SubViewport));
    Control *pad = added(target, memnew(Control));
    pad->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    netw_test::CallLog log;
    pad->connect("gui_input", log.callable("click"));

    Ref<InputEventMouseButton> click;
    click.instantiate();
    click->set_button_index(netw::gd::MOUSE_LEFT);
    click->set_pressed(true);
    click->set_position(Vector2(64, 96));
    view->NETW_GUI_INPUT(click);

    if (log.count("click") != 1) {
        return law_broken(
            "the click reached the seat %d times",
            log.count("click")
        );
    }
    const Ref<InputEventMouseButton> reached = log.args("click")[0];
    if (reached->get_position() != Vector2(32, 48)) {
        return law_broken(
            "a click at 64,96 landed at %d,%d",
            int(reached->get_position().x),
            int(reached->get_position().y)
        );
    }
    return law_held();
}

LawVerdict hidden_seat_rests(const Stand &) {
    Mounted<SubViewport> host(viewport_sized(Vector2i(640, 360)));
    StretchViewportContainer *view
        = added(host.node, memnew(StretchViewportContainer));
    SubViewport *target = added(view, memnew(SubViewport));

    view->hide();

    if (!renders(target, 2, 2) || !target->is_input_disabled()
        || target->is_audio_listener_2d() || target->is_audio_listener_3d()) {
        return law_broken(
            "a hidden seat rendered %dx%d or still took input or audio",
            target->get_size().x,
            target->get_size().y
        );
    }

    view->show();

    if (!renders(target, 640, 360) || target->is_input_disabled()
        || !target->is_audio_listener_2d() || !target->is_audio_listener_3d()) {
        return law_broken(
            "a shown seat rendered %dx%d or took no input or audio",
            target->get_size().x,
            target->get_size().y
        );
    }
    return law_held();
}

const Row ROWS[] = {
    {"a seat filling a canvas items window, then a new design size",
     "it renders at the window's pixels and draws the design, and follows "
     "the new design",
     &fills_canvas_items_window},
    {"a seat under a 2D node when the viewport resizes",
     "it renders at the viewport's size, before and after",
     &follows_resized_viewport},
    {"a split screen seat anchored to a Control that shrinks and grows",
     "it renders at the Control's size and draws a design of the same aspect",
     &follows_anchoring_control},
    {"a click inside a split screen seat",
     "it reaches the seat's Control at the design position under the cursor",
     &click_lands_under_cursor},
    {"a seat hidden, then shown",
     "a hidden seat renders two pixels and takes no input or audio, and "
     "showing it restores all three",
     &hidden_seat_rests},
};

TEST_CASE(
    "[Networked][View][SceneTree] a seat renders at its size and a click "
    "lands under the cursor"
) {
    const Stand stand = {String("a mounted tree")};
    for (const Row &row : ROWS) {
        NETW_CELL(row, stand);
        NETW_LAW_HOLDS(row, stand);
    }
}

} // namespace TestNetwStretchViewport
