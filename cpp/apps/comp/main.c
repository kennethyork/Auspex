// auspex-canvas -- a nested, compositor-backed infinite canvas.
//
// This is not the compositor. It is the one experiment that decides whether
// building the compositor is worth it, and it exists to answer exactly one
// question: can Auspex scale OTHER applications' windows, live, while they keep
// running and rendering?
//
// That question has never been answerable on X11. Panning works there because
// moving a window is just a position change, but scaling one is not -- there is no
// X11 mechanism to draw another client's window at a size it did not choose. So the
// canvas can pan and cannot zoom, which is the single biggest thing missing from it.
//
// The claim being tested: wlr_scene_buffer_set_dest_size() scales a client's
// committed buffer to an arbitrary destination rectangle. If that works, zoom is a
// scene-graph property rather than a feature to be implemented, and the existing
// Canvas class in canvas.hpp -- which already tracks positions in a space larger
// than the screen -- becomes the input to it unchanged.
//
// HOW TO RUN IT: it comes up nested, in a window, inside your current session.
// wlr_backend_autocreate() picks the Wayland or X11 backend when it finds an
// existing session rather than taking over your screen, which is why this can be
// iterated on without logging out -- the thing that made auspex-session so awkward
// to test.
//
//   ./cpp/build/auspex-canvas
//
// It spawns two terminals into itself. Then:
//
//   drag middle button   pan the infinite canvas
//   scroll               pan
//   Ctrl+scroll          zoom around the pointer
//   Escape         quit
//
// WHAT THIS DELIBERATELY DOES NOT DO: multiple outputs, layer-shell, XWayland,
// damage tracking, window placement, keyboard focus follow, popups positioned
// correctly, or anything Auspex-specific. All of that is known work. None of it is
// worth starting until the line marked THE EXPERIMENT below is proven.
//
// Written in C rather than C++ because wlroots is a C library that uses designated
// initialisers, anonymous unions and flexible array members throughout. Fighting
// that from C++ would be a second experiment layered on top of the one that
// matters.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/x11.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// Zoom limits. Below ~0.2 a window is a postage stamp; above ~3 you are looking at
// four pixels. Both ends are further than anything useful, which is the point --
// the experiment is about whether the mechanism holds, not about taste.
#define ZOOM_MIN  0.20
#define ZOOM_MAX  3.00
#define ZOOM_STEP 1.12

struct spike_server {
    struct wl_display        *display;
    struct wlr_backend       *backend;
    struct wlr_renderer      *renderer;
    struct wlr_allocator     *allocator;
    struct wlr_scene         *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wlr_xdg_shell     *xdg_shell;
    struct wlr_seat          *seat;
    struct wlr_cursor        *cursor;
    struct wlr_xcursor_manager *cursor_mgr;

    // Every toplevel hangs off this tree. Panning would be this node's position;
    // zoom is applied to the buffers underneath it.
    struct wlr_scene_tree *canvas;
    double                 zoom;
    double                 view_x;
    double                 view_y;
    int                    output_width;
    int                    output_height;
    int                    next_window;
    bool                   panning;
    double                 last_cursor_x;
    double                 last_cursor_y;

    struct wl_list toplevels;   // spike_toplevel::link
    struct wl_list outputs;     // spike_output::link
    struct wl_list keyboards;   // spike_keyboard::link

    struct wl_listener new_output;
    struct wl_listener new_xdg_surface;
    struct wl_listener new_input;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_button;
    struct wl_listener cursor_frame;

    struct wl_event_source *auto_zoom_timer;
    bool                    auto_zoom;
};

struct spike_output {
    struct wl_list        link;
    struct spike_server  *server;
    struct wlr_output    *output;
    struct wl_listener    frame;
    struct wl_listener    destroy;
};

struct spike_toplevel {
    struct wl_list          link;
    struct spike_server    *server;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_scene_tree  *tree;

    // The size the client last told us it wanted, before any scaling. Zoom is
    // applied on top of this, so zooming twice does not compound.
    int natural_width;
    int natural_height;
    int canvas_x;
    int canvas_y;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener commit;
};

struct spike_keyboard {
    struct wl_list        link;
    struct spike_server  *server;
    struct wlr_keyboard  *keyboard;
    struct wl_listener    modifiers;
    struct wl_listener    key;
    struct wl_listener    destroy;
};

// ---------------------------------------------------------------------------
// THE EXPERIMENT
// ---------------------------------------------------------------------------

// Called for every buffer beneath a toplevel -- the surface itself plus any
// subsurfaces. Each one is told what rectangle to paint into.
//
// dest_size is in layout coordinates, so a buffer the client rendered at 800x600
// asked to paint into 400x300 is drawn at half size, with the compositor doing the
// filtering. The client is not consulted and does not need to cooperate, which is
// the whole reason this can work on windows Auspex does not own.
static void scale_buffer(struct wlr_scene_buffer *buffer, int sx, int sy,
                         void *user_data) {
    (void)sx;
    (void)sy;
    const double zoom = *(double *)user_data;

    if (buffer->buffer == NULL) return;   // nothing committed yet

    const int width  = (int)(buffer->buffer->width  * zoom);
    const int height = (int)(buffer->buffer->height * zoom);
    if (width <= 0 || height <= 0) return;

    wlr_scene_buffer_set_dest_size(buffer, width, height);
}

static void apply_view(struct spike_server *server) {
    struct spike_toplevel *toplevel;

    // The canvas tree is the camera transform. Moving one node pans every window,
    // regardless of how many applications are open.
    wlr_scene_node_set_position(
        &server->canvas->node,
        (int)(-server->view_x * server->zoom),
        (int)(-server->view_y * server->zoom));

    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->tree == NULL) continue;

        const int x = (int)(toplevel->canvas_x * server->zoom);
        const int y = (int)(toplevel->canvas_y * server->zoom);
        wlr_scene_node_set_position(&toplevel->tree->node, x, y);

        wlr_scene_node_for_each_buffer(&toplevel->tree->node, scale_buffer,
                                       &server->zoom);
    }
}

static void set_zoom_at(struct spike_server *server, double zoom,
                        double anchor_x, double anchor_y) {
    if (zoom < ZOOM_MIN) zoom = ZOOM_MIN;
    if (zoom > ZOOM_MAX) zoom = ZOOM_MAX;
    if (zoom == server->zoom) return;

    // Keep the canvas point under the pointer stationary. Without this, zooming
    // always pulls toward 0,0 and the user loses the window they were inspecting.
    const double canvas_x = server->view_x + anchor_x / server->zoom;
    const double canvas_y = server->view_y + anchor_y / server->zoom;

    server->zoom = zoom;
    server->view_x = canvas_x - anchor_x / zoom;
    server->view_y = canvas_y - anchor_y / zoom;

    wlr_log(WLR_INFO, "view=(%.1f,%.1f) zoom=%.2f",
            server->view_x, server->view_y, zoom);
    apply_view(server);
}

static void pan_by(struct spike_server *server, double screen_dx, double screen_dy) {
    // Inputs are screen pixels. Converting through zoom makes the canvas track the
    // hand at every scale instead of feeling slower when zoomed out.
    server->view_x += screen_dx / server->zoom;
    server->view_y += screen_dy / server->zoom;
    apply_view(server);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_output *output = wl_container_of(listener, output, frame);

    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(output->server->scene, output->output);
    if (scene_output == NULL) return;

    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct spike_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output   *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // wlr_output_state is the 0.17 way: build the state, then commit it once.
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    wlr_log(WLR_INFO, "new_output: %s %dx%d", wlr_output->name,
            wlr_output->width, wlr_output->height);
    server->output_width = wlr_output->width;
    server->output_height = wlr_output->height;

    struct spike_output *output = calloc(1, sizeof(*output));
    output->server = server;
    output->output = wlr_output;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output =
        wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output,
                                       scene_output);
}

// ---------------------------------------------------------------------------
// Toplevels
// ---------------------------------------------------------------------------
static void toplevel_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->toplevel->base->initial_commit) {
        // 0,0 means "you choose". Left to the client on purpose: the spike is about
        // scaling whatever it produces, not about dictating a size to it.
        wlr_xdg_toplevel_set_size(toplevel->toplevel, 0, 0);
        return;
    }

    // Re-apply on every commit. A client that redraws would otherwise come back at
    // its natural size and pop back to 100% -- which, when it happened, would look
    // exactly like the zoom having silently failed.
    wlr_scene_node_for_each_buffer(&toplevel->tree->node, scale_buffer,
                                   &toplevel->server->zoom);
}

static void toplevel_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

    struct wlr_box geometry;
    wlr_xdg_surface_get_geometry(toplevel->toplevel->base, &geometry);
    toplevel->natural_width  = geometry.width;
    toplevel->natural_height = geometry.height;

    wlr_seat_keyboard_notify_enter(toplevel->server->seat,
                                   toplevel->toplevel->base->surface, NULL, 0, NULL);
    // New windows get stable canvas coordinates rather than screen coordinates.
    // The grid continues beyond the visible output, so opening more applications
    // grows the canvas instead of stacking them all in one corner.
    const int index = toplevel->server->next_window++;
    const int columns = 3;
    toplevel->canvas_x = (index % columns) * 700;
    toplevel->canvas_y = (index / columns) * 500;

    apply_view(toplevel->server);
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    wl_list_remove(&toplevel->link);
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->commit.link);
    free(toplevel);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct spike_server    *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *surface = data;

    if (surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        // Popups are parented to whatever spawned them and inherit its scaling for
        // free, which is a pleasant consequence of zoom living in the scene graph.
        struct wlr_xdg_surface *parent =
            wlr_xdg_surface_try_from_wlr_surface(surface->popup->parent);
        if (parent != NULL && parent->data != NULL) {
            struct wlr_scene_tree *parent_tree = parent->data;
            surface->data = wlr_scene_xdg_surface_create(parent_tree, surface);
        }
        return;
    }
    if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;

    struct spike_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->server   = server;
    toplevel->toplevel = surface->toplevel;
    toplevel->tree     = wlr_scene_xdg_surface_create(server->canvas, surface);
    toplevel->tree->node.data = toplevel;
    surface->data = toplevel->tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&surface->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&surface->surface->events.unmap, &toplevel->unmap);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&surface->events.destroy, &toplevel->destroy);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&surface->surface->events.commit, &toplevel->commit);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void keyboard_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                       &keyboard->keyboard->modifiers);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
    struct spike_keyboard        *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event   = data;
    struct spike_server          *server   = keyboard->server;

    const uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t sym =
        xkb_state_key_get_one_sym(keyboard->keyboard->xkb_state, keycode);

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (sym == XKB_KEY_Escape) {
            wl_display_terminate(server->display);
            return;
        }

        // Keyboard canvas controls require Ctrl+Alt so ordinary application
        // shortcuts and arrow keys keep working.
        const uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
        const uint32_t canvas_mods = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT;
        if ((modifiers & canvas_mods) == canvas_mods) {
            const double cx = server->output_width > 0
                                  ? server->output_width / 2.0 : server->cursor->x;
            const double cy = server->output_height > 0
                                  ? server->output_height / 2.0 : server->cursor->y;
            switch (sym) {
                case XKB_KEY_Left:  pan_by(server, -160, 0); return;
                case XKB_KEY_Right: pan_by(server, 160, 0);  return;
                case XKB_KEY_Up:    pan_by(server, 0, -120); return;
                case XKB_KEY_Down:  pan_by(server, 0, 120);  return;
                case XKB_KEY_plus:
                case XKB_KEY_equal:
                case XKB_KEY_KP_Add:
                    set_zoom_at(server, server->zoom * ZOOM_STEP, cx, cy);
                    return;
                case XKB_KEY_minus:
                case XKB_KEY_KP_Subtract:
                    set_zoom_at(server, server->zoom / ZOOM_STEP, cx, cy);
                    return;
                case XKB_KEY_0:
                case XKB_KEY_KP_0:
                    set_zoom_at(server, 1.0, cx, cy);
                    return;
                case XKB_KEY_Home:
                    server->view_x = 0;
                    server->view_y = 0;
                    apply_view(server);
                    return;
                default:
                    break;
            }
        }
    }

    wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode,
                                 event->state);
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct spike_server *server,
                                struct wlr_input_device *device) {
    struct wlr_keyboard   *wlr_keyboard = wlr_keyboard_from_input_device(device);
    struct spike_keyboard *keyboard     = calloc(1, sizeof(*keyboard));
    keyboard->server   = server;
    keyboard->keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap  *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct spike_server     *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    wlr_log(WLR_INFO, "new_input: type=%d name=%s", (int)device->type,
            device->name ? device->name : "(null)");

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        server_new_keyboard(server, device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(server->cursor, device);
    }

    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, capabilities);
}

static uint32_t current_modifiers(struct spike_server *server) {
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    return keyboard == NULL ? 0 : wlr_keyboard_get_modifiers(keyboard);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct spike_server            *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event  *event  = data;
    wlr_log(WLR_INFO, "AXIS delta=%.2f", event->delta);

    if ((current_modifiers(server) & WLR_MODIFIER_CTRL) != 0) {
        // Ctrl+scroll zooms around the pointer, matching the GTK/X11 canvas.
        const double factor = event->delta < 0 ? ZOOM_STEP : 1.0 / ZOOM_STEP;
        set_zoom_at(server, server->zoom * factor,
                    server->cursor->x, server->cursor->y);
        return;
    }

    // Plain wheel/trackpad input pans. Continuous deltas are already in useful
    // pixel-like units; discrete wheel notches need a larger step.
    double dx = event->orientation == WLR_AXIS_ORIENTATION_HORIZONTAL
                    ? event->delta : 0.0;
    double dy = event->orientation == WLR_AXIS_ORIENTATION_VERTICAL
                    ? event->delta : 0.0;
    if (event->delta_discrete != 0) {
        const double step = event->delta_discrete * 48.0;
        if (event->orientation == WLR_AXIS_ORIENTATION_HORIZONTAL) dx = step;
        else dy = step;
    }
    pan_by(server, dx, dy);
}

static void pointer_motion_common(struct spike_server *server, uint32_t time_msec) {
    if (server->panning) {
        const double dx = server->last_cursor_x - server->cursor->x;
        const double dy = server->last_cursor_y - server->cursor->y;
        pan_by(server, dx, dy);
        server->last_cursor_x = server->cursor->x;
        server->last_cursor_y = server->cursor->y;
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }

    double sx = 0.0;
    double sy = 0.0;
    struct wlr_surface *surface = NULL;
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, server->cursor->x, server->cursor->y, &sx, &sy);

    if (node != NULL && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer  *scene_buffer = wlr_scene_buffer_from_node(node);
        struct wlr_scene_surface *scene_surface =
            wlr_scene_surface_try_from_buffer(scene_buffer);
        if (scene_surface != NULL) surface = scene_surface->surface;
    }

    if (surface == NULL) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
        wlr_seat_pointer_clear_focus(server->seat);
        return;
    }
    // Scene hit-testing reports coordinates in the scaled destination. Clients
    // speak in their unscaled surface coordinates, so invert the canvas transform.
    sx /= server->zoom;
    sy /= server->zoom;
    wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct spike_server              *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event  *event  = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x,
                    event->delta_y);
    pointer_motion_common(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct spike_server                       *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event  *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    pointer_motion_common(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct spike_server             *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event  = data;
    if (event->button == BTN_MIDDLE) {
        server->panning = event->state == WLR_BUTTON_PRESSED;
        server->last_cursor_x = server->cursor->x;
        server->last_cursor_y = server->cursor->y;
        if (server->panning) {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "grabbing");
            wlr_seat_pointer_clear_focus(server->seat);
        } else {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
            pointer_motion_common(server, event->time_msec);
        }
        return;
    }

    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
                                   event->state);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct spike_server *server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

// Creates the window this compositor renders into, for whichever nested backend
// is present. Called per sub-backend, because autocreate wraps them in a multi.
// Drives the zoom from a timer instead of the scroll wheel.
//
// Added because proving the experiment through input turned into its own project:
// the nested backend's window is unmanaged, XSendEvent is ignored by its XInput2
// listener, and the window kept being closed by stray clicks mid-test. None of
// that is what is being tested. A timer removes every one of those variables --
// if the picture changes size on its own, dest_size works.
static int auto_zoom_tick(void *data) {
    struct spike_server *server = data;
    static const double steps[] = {1.0, 0.75, 0.5, 0.35, 0.5, 0.75};
    static int i = 0;

    const double cx = server->output_width / 2.0;
    const double cy = server->output_height / 2.0;
    set_zoom_at(server, steps[i % 6], cx, cy);
    i++;

    wl_event_source_timer_update(server->auto_zoom_timer, 2000);
    return 0;
}

// ---------------------------------------------------------------------------
static void spawn_client(const char *socket, const char *const argv[]) {
    if (fork() != 0) return;
    setsid();
    setenv("WAYLAND_DISPLAY", socket, 1);
    // GTK would otherwise reuse the outer X11 session and the window would appear
    // on the real desktop instead of inside the spike.
    setenv("GDK_BACKEND", "wayland", 1);
    unsetenv("DISPLAY");
    execvp(argv[0], (char *const *)argv);
    _exit(127);
}

int main(int argc, char **argv) {
    bool spawn = true;
    bool auto_zoom = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-clients") == 0) spawn = false;
        if (strcmp(argv[i], "--auto-zoom") == 0)  auto_zoom = true;
    }

    wlr_log_init(WLR_INFO, NULL);

    struct spike_server server = {0};
    server.zoom = 1.0;

    server.display = wl_display_create();
    server.backend = wlr_backend_autocreate(server.display, NULL);
    if (server.backend == NULL) {
        fprintf(stderr, "auspex-canvas: could not create a backend\n");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.display);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create();
    wl_list_init(&server.outputs);
    wl_list_init(&server.toplevels);
    wl_list_init(&server.keyboards);

    server.scene        = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene,
                                                         server.output_layout);
    // Everything the user's windows live in. Panning the canvas would be a single
    // set_position on THIS node -- one call for the whole desktop, where the X11
    // implementation needs one subprocess per window.
    server.canvas = wlr_scene_tree_create(&server.scene->tree);

    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_surface.notify = server_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
                  &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.display, "seat0");

    // The nested backends come up with NO outputs -- both headers say so explicitly
    // -- so the window this runs in has to be asked for. Called before
    // wlr_backend_start(), which is allowed: the output then arrives through the
    // new_output signal during initialisation like any other.
    //
    // Without this the compositor starts, opens its socket and accepts clients, but
    // never renders and never emits a frame event, because it has nowhere to render
    // to. It looks like a hang; it is actually a compositor with no screen.
    // wlr_backend_autocreate() hands back a MULTI backend wrapping the real one,
    // so wlr_backend_is_x11() on it is false and a naive check silently skips the
    // output creation -- leaving a compositor that starts, serves its socket and
    // accepts clients while never rendering anything. Walk the children.
    struct output_maker { int made; };
    static struct output_maker maker;
    maker.made = 0;

    // NOTHING to do here, and that is the finding.
    //
    // Both nested backends already create one output at startup on their own --
    // WLR_X11_OUTPUTS / WLR_WL_OUTPUTS, defaulting to 1 -- and they arrive through
    // the new_output signal like any other. Calling wlr_x11_output_create() as well
    // makes a SECOND window, and on this machine the second one fails to set a
    // property (BadAtom on X_ChangeProperty) and takes the first down with it. The
    // symptom is a compositor that runs, serves clients and renders nothing.
    //
    // The header's "created with no outputs" wording describes wlr_x11_backend_create(),
    // not wlr_backend_autocreate(), which is what misled me into adding the call.
    (void)maker;

    // A NAMED socket, not add_socket_auto().
    //
    // add_socket_auto() takes the first free name, which on a machine with no
    // Wayland session is "wayland-0" -- and GDK falls back to exactly that name
    // when WAYLAND_DISPLAY is unset. So an X11 GTK application started while this
    // spike was running would silently connect HERE instead of to X11, and its
    // windows would appear inside the experiment rather than on the desktop. That
    // happened: auspex-shell came up with no visible panels, because it was running
    // inside this compositor.
    //
    // A name nothing defaults to makes that impossible.
    const char *socket = "auspex-spike-0";
    if (wl_display_add_socket(server.display, socket) != 0) {
        fprintf(stderr, "auspex-canvas: could not create socket %s\n", socket);
        wlr_backend_destroy(server.backend);
        return 1;
    }
    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    printf("\n");
    printf("  auspex-canvas on WAYLAND_DISPLAY=%s\n", socket);
    printf("\n");
    printf("    middle-drag          pan the infinite canvas\n");
    printf("    scroll / trackpad    pan\n");
    printf("    Ctrl+scroll          zoom around the pointer\n");
    printf("    Ctrl+Alt+arrows      pan from the keyboard\n");
    printf("    Ctrl+Alt+plus/minus  zoom; Ctrl+Alt+0 resets\n");
    printf("    Ctrl+Alt+Home        return to the origin\n");
    printf("    Escape               quit\n");
    printf("\n");
    printf("  run more clients into it with:\n");
    printf("    WAYLAND_DISPLAY=%s GDK_BACKEND=wayland xfce4-terminal\n\n", socket);
    fflush(stdout);

    if (spawn) {
        static const char *const term[] = {"xfce4-terminal", NULL};
        spawn_client(socket, term);
        spawn_client(socket, term);
    }

    if (auto_zoom) {
        server.auto_zoom = true;
        struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
        server.auto_zoom_timer =
            wl_event_loop_add_timer(loop, auto_zoom_tick, &server);
        // 4s of grace so the clients have mapped and committed a buffer first;
        // scaling a surface that has never committed does nothing observable.
        wl_event_source_timer_update(server.auto_zoom_timer, 4000);
        printf("  --auto-zoom: cycling 1.0 -> 0.35 -> 1.0 every 2s\n\n");
        fflush(stdout);
    }

    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return 0;
}
