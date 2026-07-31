#include "auspex/gtk/desktop_window.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>

#include <gdkmm/general.h>
#include <glibmm/main.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>

#include "auspex/display.hpp"
#include "auspex/theme.hpp"

namespace auspex::gtk {

namespace {

// Canvas-space spacing of the grid. Large enough not to shimmer when panning fast,
// small enough that a slow drag still visibly moves something.
constexpr double kGridStep  = 64.0;
constexpr double kMajorEvery = 8;   // every 8th line is drawn brighter

constexpr int kMinimapWidth  = 240;
constexpr int kMinimapHeight = 150;
constexpr int kMinimapMargin = 24;

// How far one scroll notch or one arrow key press moves the viewport. A fraction
// of the screen rather than a fixed pixel count, so it feels the same on a 4K
// monitor as on a laptop.
constexpr double kStepFraction = 0.25;

// How far a window may sit from where the canvas placed it before that counts as
// the user having dragged it.
//
// 24, measured rather than guessed: xfwm4 lands a window asked for x=960 at x=970,
// because move requests address the frame and the reported geometry is the client
// area, and the difference is the decoration. At 8 that offset read as a drag and
// the canvas recorded a position ten pixels from the one it had just chosen -- so
// every fit left every window slightly askew from where it belonged.
//
// The cost is that a deliberate nudge smaller than 24px is ignored. That is the
// right way round: an unnoticed 10px drift on every window on every tick is worse
// than needing to mean it when you move something.
constexpr int kMoveTolerance = 24;

struct Rgb {
    double r = 0, g = 0, b = 0;
};

// "#rrggbb" only -- that is the whole format used by theme.cpp, and anything else
// is a typo there rather than an input to be tolerated here.
Rgb parse_hex(std::string_view hex) {
    if (hex.size() != 7 || hex[0] != '#') return {};

    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    const auto byte = [&](std::size_t i) {
        return (nibble(hex[i]) * 16 + nibble(hex[i + 1])) / 255.0;
    };
    return {byte(1), byte(3), byte(5)};
}

void set_rgb(const Cairo::RefPtr<Cairo::Context>& cr, const Rgb& c, double alpha = 1.0) {
    cr->set_source_rgba(c.r, c.g, c.b, alpha);
}

// The grid line nearest to, and not after, `value`. Used to find where to start
// drawing so lines stay locked to canvas coordinates rather than to the screen --
// without this the grid would slide under itself and panning would look static.
double floor_to_step(double value, double step) {
    return std::floor(value / step) * step;
}

}  // namespace

// ---------------------------------------------------------------------------
DesktopWindow::DesktopWindow(const Config& config, Canvas& canvas, const Rect& monitor)
    : config_(config), canvas_(canvas), monitor_(monitor) {
    panel_top_ = compute_panel_layout(
        monitor_, 1, config_.panel_height, PanelPosition::Top).bounds.height;
    panel_bottom_ = compute_panel_layout(
        monitor_, 1, config_.panel_height, PanelPosition::Bottom).bounds.height;
    // The title is how find_own_window locates this window, and it also contains
    // "Auspex", which is what keeps is_shell_window() from letting the canvas adopt
    // itself. Both of those depend on the string; do not make it prettier.
    set_title(title_);
    set_decorated(false);
    set_default_size(monitor_.width, monitor_.height);

    // Needed for the arrow keys. A desktop-type window rarely receives focus under
    // a stock window manager, so the keyboard path is a bonus rather than the
    // primary one -- the drag and the scroll wheel are what this really runs on.
    set_can_focus(true);

    tile_width_  = std::max(320, monitor_.width / 2);
    tile_height_ = std::max(240, monitor_.height / 2);

    area_.set_expand(true);
    area_.set_draw_func(sigc::mem_fun(*this, &DesktopWindow::on_draw));
    set_child(area_);

    load_wallpaper();
    install_gestures();

    signal_realize().connect([this] {
        Glib::signal_timeout().connect(
            [this] {
                claim_desktop_layer();
                return !window_id_.has_value() && ++claim_attempts_ < 40;
            },
            150);

        // Adoption runs on its own timer rather than on window-manager events,
        // because there is no way to subscribe to those without owning the WM. One
        // second is under the threshold where a newly opened window feels like it
        // was ignored, and cheap: one wmctrl call.
        Glib::signal_timeout().connect(
            [this] {
                reconcile();
                return true;
            },
            1000);
    });
}

// ---------------------------------------------------------------------------
void DesktopWindow::claim_desktop_layer() {
    if (!window_id_) {
        window_id_ = display().find_own_window(title_);
        if (!window_id_) return;
    }
    display().place_desktop_window(*window_id_, monitor_);
}

bool DesktopWindow::desktop_geometry_is_wrong() const {
    if (!window_id_) return false;
    const auto geometry = display().window_geometry(*window_id_);
    if (!geometry) return false;   // cannot tell; do not thrash
    return geometry->width != monitor_.width || geometry->height != monitor_.height ||
           geometry->x != monitor_.x || geometry->y != monitor_.y;
}

// ---------------------------------------------------------------------------
void DesktopWindow::install_gestures() {
    // Drag with the primary button. Absolute, not incremental: every update
    // recomputes the viewport from where the drag started, so rounding cannot
    // accumulate and the canvas stays glued to the pointer for the whole gesture.
    auto drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double, double) {
        drag_origin_x_ = canvas_.viewport().x;
        drag_origin_y_ = canvas_.viewport().y;
        dragging_      = true;
    });
    drag->signal_drag_update().connect([this](double offset_x, double offset_y) {
        if (!dragging_) return;
        // Minus, because the content follows the hand: pulling right must bring
        // what is off the left edge into view, which is a viewport moving left.
        Viewport v = canvas_.viewport();
        v.x = drag_origin_x_ - static_cast<int>(offset_x);
        v.y = drag_origin_y_ - static_cast<int>(offset_y);
        canvas_.set_viewport(v);
        area_.queue_draw();
        schedule_apply();
    });
    drag->signal_drag_end().connect([this](double, double) {
        dragging_ = false;
        apply_now();   // one final exact placement, whatever the frame clock did
    });
    area_.add_controller(drag);

    auto scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    scroll->signal_scroll().connect(
        [this, scroll](double dx, double dy) {
            // Ctrl+scroll zooms, plain scroll pans -- the convention every map and
            // drawing program uses, so it needs no explaining.
            if ((scroll->get_current_event_state() & Gdk::ModifierType::CONTROL_MASK) ==
                Gdk::ModifierType::CONTROL_MASK) {
                zoom_by(dy < 0 ? 1.15 : 1.0 / 1.15);
                return true;
            }
            const int step_x = static_cast<int>(monitor_.width * kStepFraction);
            const int step_y = static_cast<int>(monitor_.height * kStepFraction);
            pan_by(static_cast<int>(dx * step_x), static_cast<int>(dy * step_y));
            return true;
        },
        /*after=*/false);
    area_.add_controller(scroll);

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            const int step_x = static_cast<int>(monitor_.width * kStepFraction);
            const int step_y = static_cast<int>(monitor_.height * kStepFraction);
            switch (keyval) {
                case GDK_KEY_Left:  pan_by(-step_x, 0); return true;
                case GDK_KEY_Right: pan_by(step_x, 0);  return true;
                case GDK_KEY_Up:    pan_by(0, -step_y); return true;
                case GDK_KEY_Down:  pan_by(0, step_y);  return true;
                case GDK_KEY_Home:  go_home();          return true;
                case GDK_KEY_f:
                case GDK_KEY_F:     fit_all();          return true;
                case GDK_KEY_plus:
                case GDK_KEY_equal:
                case GDK_KEY_KP_Add:      zoom_by(1.15);  return true;
                case GDK_KEY_minus:
                case GDK_KEY_KP_Subtract: zoom_by(1.0 / 1.15); return true;
                case GDK_KEY_0:
                case GDK_KEY_KP_0:        reset_zoom();   return true;
                default: return false;
            }
        },
        /*after=*/false);
    add_controller(keys);
}

// ---------------------------------------------------------------------------
void DesktopWindow::pan_by(int dx, int dy) {
    canvas_.pan_by(dx, dy);
    area_.queue_draw();
    schedule_apply();
}

void DesktopWindow::fit_all() {
    // Sticky overlay bars must not limit the infinite plane, but a deliberate
    // layout must not put title bars or controls underneath them. Apply their real
    // heights only while computing this layout, then return the canvas to its
    // unbounded edge behaviour before any later pan.
    canvas_.set_insets(panel_top_, panel_bottom_);
    last_fit_ = canvas_.fit_all();
    if (last_fit_.tile_width <= 0) {
        canvas_.set_insets(0, 0);
        return;
    }

    // resolve() knows positions; the tile size comes from the fit. Stamped on here
    // rather than stored in the Canvas because a size is a one-off instruction to
    // the window manager, not a property of where something lives on the canvas.
    auto positions = canvas_.resolve();
    for (auto& position : positions) {
        if (!position.visible) continue;   // parked windows are not worth resizing
        position.width  = last_fit_.tile_width;
        position.height = last_fit_.tile_height;
    }
    canvas_.set_insets(0, 0);
    apply_positions(positions, monitor_);
    last_positions_ = std::move(positions);
    area_.queue_draw();
}

void DesktopWindow::set_panel_height(PanelPosition position, int height) {
    height = std::max(0, height);
    int& current = position == PanelPosition::Top ? panel_top_ : panel_bottom_;
    if (current == height) return;
    current = height;

    // The first realised geometry arrives just after the initial adoption pass.
    // Refit then so a theme making a 28px configured panel 42px tall cannot leave
    // the windows using the stale, smaller gap.
    if (auto_fit_ && !canvas_.managed_ids().empty()) fit_all();
}

void DesktopWindow::full_managed_window(const std::string& window_id) {
    auspex::restore_window(window_id);

    if (canvas_.is_hidden(window_id)) {
        last_hidden_.erase(window_id);
        canvas_.set_hidden(last_hidden_);
    }

    if (!canvas_.manages(window_id)) return;

    const int top = std::clamp(panel_top_, 0, monitor_.height - 1);
    const int bottom = std::clamp(panel_bottom_, 0, monitor_.height - top - 1);
    const int safe_height = std::max(1, monitor_.height - top - bottom);
    const double zoom = canvas_.zoom() > 0.0 ? canvas_.zoom() : 1.0;
    const Viewport viewport = canvas_.viewport();

    // Store canvas units so the very next pan continues naturally from this exact
    // position. The requested screen rectangle itself is the bar-safe monitor.
    canvas_.place(window_id, {
        .x = viewport.x,
        .y = viewport.y + static_cast<int>(top / zoom),
        .width = static_cast<int>(monitor_.width / zoom),
        .height = static_cast<int>(safe_height / zoom),
    });

    std::vector<ScreenPosition> target{{
        .id = window_id,
        .x = 0,
        .y = top,
        .visible = true,
        .width = monitor_.width,
        .height = safe_height,
    }};
    apply_positions(target, monitor_);
    last_positions_ = canvas_.resolve();
    activate_window(window_id);
    needs_redraw_ = true;
    area_.queue_draw();
}

void DesktopWindow::restore_managed_window(const std::string& window_id) {
    auspex::restore_window(window_id);

    // A minimised window remains managed, but resolve() deliberately excludes it
    // while hidden so moving the canvas cannot accidentally de-iconify it. Remove
    // it from that set synchronously: otherwise clicking its task button can raise
    // it at an old off-canvas coordinate until the one-second poll notices.
    const bool was_hidden = canvas_.is_hidden(window_id);
    if (was_hidden) {
        last_hidden_.erase(window_id);
        canvas_.set_hidden(last_hidden_);
        if (auto_fit_) fit_all();
    } else {
        // A visible window may still be beyond the current viewport on the infinite
        // plane. Bringing it to front should also bring its part of the canvas into
        // view, without disturbing the rest of the grid.
        const auto position = std::find_if(
            last_positions_.begin(), last_positions_.end(),
            [&](const ScreenPosition& p) { return p.id == window_id; });
        if (position != last_positions_.end() && !position->visible) {
            const auto placement = canvas_.placement_of(window_id);
            const int width  = placement && placement->width  > 0
                                   ? placement->width : tile_width_;
            const int height = placement && placement->height > 0
                                   ? placement->height : tile_height_;
            if (canvas_.focus_on(window_id, width, height)) apply_now();
        }
    }

    needs_redraw_ = true;
    area_.queue_draw();
}

void DesktopWindow::zoom_by(double factor) {
    canvas_.zoom_by(factor);
    needs_redraw_ = true;
    area_.queue_draw();

    // An explicit zoom always carries sizes, including the final step to 1.0.
    // A plain resolve deliberately omits sizes at 1.0 so panning is cheap, but
    // using it here left the real windows stuck at the previous smaller size.
    last_positions_ = canvas_.resolve(/*include_life_size=*/true);
    apply_positions(last_positions_, monitor_);
}

void DesktopWindow::reset_zoom() {
    // Reset means life size, not "fit everything". Re-fitting here overwrote the
    // user's natural window sizes and made the 1:1 control behave like a second
    // layout command.
    canvas_.reset_zoom();
    last_positions_ = canvas_.resolve(/*include_life_size=*/true);
    apply_positions(last_positions_, monitor_);
    needs_redraw_ = true;
    area_.queue_draw();
}

void DesktopWindow::go_home() {
    Viewport v = canvas_.viewport();
    v.x = 0;
    v.y = 0;
    canvas_.set_viewport(v);
    area_.queue_draw();
    apply_now();
}

// ---------------------------------------------------------------------------
void DesktopWindow::schedule_apply() {
    // Coalescing matters more than it looks. A drag emits updates as fast as the
    // pointer reports, and each apply is one subprocess per managed window; without
    // this, a one-second drag across the screen would queue thousands of them and
    // the shell would still be catching up long after the hand stopped.
    if (apply_queued_) return;
    apply_queued_ = true;
    Glib::signal_idle().connect_once([this] {
        apply_queued_ = false;
        apply_now();
    });
}

void DesktopWindow::apply_now() {
    last_positions_ = canvas_.resolve();
    apply_positions(last_positions_, monitor_);
}

bool DesktopWindow::adopt_monitor_change() {
    const auto monitor = primary_monitor();
    if (!monitor || monitor->bounds == monitor_) return false;

    const Rect previous = monitor_;
    monitor_ = monitor->bounds;

    tile_width_  = std::max(320, monitor_.width / 2);
    tile_height_ = std::max(240, monitor_.height / 2);

    // The viewport keeps its position in canvas space but takes the new screen's
    // size, so whatever you were looking at stays under the cursor rather than
    // jumping because the window got wider.
    Viewport viewport = canvas_.viewport();
    viewport.width  = monitor_.width;
    viewport.height = monitor_.height;
    canvas_.set_viewport(viewport);

    set_default_size(monitor_.width, monitor_.height);

    // The wallpaper was cover-scaled to the OLD monitor at load, so a new size
    // needs a new scale -- otherwise it is letterboxed or cropped to the wrong
    // aspect on the new screen.
    if (previous.width != monitor_.width || previous.height != monitor_.height) {
        load_wallpaper();
    }

    claim_desktop_layer();
    apply_now();
    needs_redraw_ = true;

    if (on_monitor_changed_) on_monitor_changed_(monitor_);
    return true;
}

// ---------------------------------------------------------------------------
void DesktopWindow::reconcile() {
    // The bars are overlays in canvas mode, not boundaries. They remain sticky and
    // above every application, while the plane and its windows can pass behind
    // them and reappear on the other side.
    canvas_.set_insets(0, 0);

    // Before anything else: has the screen itself changed? Everything below --
    // the viewport, monitor filter and geometry self-heal -- is expressed
    // against monitor_, so a stale value makes all of them wrong together.
    adopt_monitor_change();

    // Re-assert the desktop geometry if something shrank it. This is not paranoia:
    // the window is sized once at startup, and the panels publish their struts a
    // moment later -- at which point the window manager clamps every managed window
    // to the new work area, including this one. The result is a strip of bare root
    // window showing where the desktop stops short, so it is fixed rather than
    // claimed once and hoped for. Also covers a monitor being resized or unplugged.
    if (desktop_geometry_is_wrong()) claim_desktop_layer();

    // One call for every window and its position. Everything below reads from this
    // snapshot rather than asking the display server again per window.
    const auto placed = list_placed_windows();

    std::vector<WindowEntry> live;
    std::map<std::string, Rect> geometry_of;
    for (const auto& entry : placed) {
        if (is_shell_window(entry.window)) continue;
        geometry_of[entry.window.id] = entry.bounds;
        titles_[entry.window.id]     = entry.window.title;
        live.push_back(entry.window);
    }

    // Native X11 maximise follows _NET_WORKAREA. Our overlay bars deliberately
    // publish zero struts so they cannot bound the infinite canvas, which means a
    // title-bar maximise fills the whole monitor, goes behind the bottom panel and
    // becomes immovable. Only query state for geometries that have the unmistakable
    // oversized shape; this avoids an xprop fork per ordinary window per second.
    std::set<std::string> native_maximized;
    const int safe_height = std::max(1, monitor_.height - panel_top_ - panel_bottom_);
    for (const auto& entry : placed) {
        if (is_shell_window(entry.window)) continue;
        const Rect& bounds = entry.bounds;
        const int centre_x = bounds.x + bounds.width / 2;
        const int centre_y = bounds.y + bounds.height / 2;
        const bool on_this_monitor =
            centre_x >= monitor_.x && centre_x < monitor_.x + monitor_.width &&
            centre_y >= monitor_.y && centre_y < monitor_.y + monitor_.height;
        if (on_this_monitor && bounds.width >= monitor_.width &&
            bounds.height > safe_height &&
            display().window_is_maximized(entry.window.id)) {
            native_maximized.insert(entry.window.id);
        }
    }

    // Which windows are minimised. Canonical ids on both sides, because wmctrl and
    // xdotool spell the same window differently and a mismatch here would report
    // every window as minimised.
    std::set<std::string> hidden;
    {
        std::set<std::string> visible;
        for (const auto& id : list_visible_windows()) visible.insert(id);
        for (const auto& entry : placed) {
            const std::string id = canonical_window_id(entry.window.id);
            if (visible.count(id) == 0) hidden.insert(entry.window.id);
        }
    }
    canvas_.set_hidden(hidden);

    // Learn where the user dragged things to, BEFORE deciding anything else.
    //
    // Without this the canvas silently fights the user: they drag a window with the
    // titlebar, the window manager moves it, and the next apply_positions() puts it
    // straight back where the canvas thinks it belongs. note_screen_move converts
    // the new on-screen position back into canvas space, so the drag is adopted as
    // intent rather than treated as drift.
    std::vector<std::string> released;

    if (!dragging_) {   // mid-pan the windows are meant to be moving; ignore them
        for (const auto& id : canvas_.managed_ids()) {
            if (canvas_.is_hidden(id)) continue;   // its geometry is stale, not a drag
            if (native_maximized.count(id) != 0) continue; // converted below
            const auto found = geometry_of.find(id);
            if (found == geometry_of.end()) continue;

            // Skip anything the canvas itself parked.
            //
            // Parking puts a window deliberately outside the monitor, which the
            // test below cannot distinguish from the user having dragged it to
            // another screen -- so the canvas released windows it had just parked,
            // they became unmanaged, and no amount of zooming or fitting would ever
            // move them again. A window frozen at the bottom edge, ignoring every
            // control, was this.
            const auto parked = std::find_if(
                last_positions_.begin(), last_positions_.end(),
                [&](const ScreenPosition& p) { return p.id == id && !p.visible; });
            if (parked != last_positions_.end()) continue;

            // Dragged off this monitor? Then it is no longer the canvas's business.
            //
            // The canvas owns one screen and never pans anything onto another. But
            // moving a window to another monitor BY HAND is a normal thing to want,
            // and it only works if the canvas lets go -- otherwise the next apply
            // pulls it straight back, and the window fights the hand. Releasing it
            // leaves it exactly where it was put, unmanaged, until it is dragged
            // back onto this screen and adopted again.
            const int centre_x = found->second.x + found->second.width / 2;
            const int centre_y = found->second.y + found->second.height / 2;
            const bool on_this_monitor =
                centre_x >= monitor_.x && centre_x < monitor_.x + monitor_.width &&
                centre_y >= monitor_.y && centre_y < monitor_.y + monitor_.height;
            if (!on_this_monitor) {
                released.push_back(id);
                continue;
            }

            const int screen_x = found->second.x - monitor_.x;
            const int screen_y = found->second.y - monitor_.y;

            // Only react to a move the canvas did not make. A tolerance is needed
            // because window managers adjust placement by a decoration's width and
            // that must not be mistaken for a drag.
            const auto expected = canvas_.placement_of(id);
            if (!expected) continue;
            // Compare in SCREEN coordinates. At a zoom other than 1.0 the old
            // comparison used unscaled canvas coordinates, so every resize looked
            // like a manual drag on the next reconcile tick. note_screen_move()
            // then wrote that false drag back into canvas space and successive
            // zooms walked the window away from where it belonged.
            const double zoom = canvas_.zoom();
            const int expected_x = static_cast<int>(
                (expected->x - canvas_.viewport().x) * zoom);
            const int expected_y = static_cast<int>(
                (expected->y - canvas_.viewport().y) * zoom);
            if (std::abs(screen_x - expected_x) <= kMoveTolerance &&
                std::abs(screen_y - expected_y) <= kMoveTolerance) {
                continue;
            }
            canvas_.note_screen_move(id, screen_x, screen_y);
        }
    }

    for (const auto& id : released) {
        canvas_.forget(id);
        needs_redraw_ = true;
    }

    auto sync = plan_canvas_sync(live, canvas_);

    // The canvas owns ONE monitor -- the primary. Windows living on another screen
    // are left exactly where they are.
    //
    // Without this, adoption is actively destructive on a multi-head desk: every
    // window on every monitor gets pulled onto the canvas and repositioned onto the
    // primary, so plugging in a second screen and starting the shell would sweep
    // everything on it into one pile. Only candidates for adoption are checked, so
    // this costs one geometry query per newly-seen window rather than one per window
    // per tick.
    if (!sync.adopt.empty()) {
        std::vector<std::string> on_this_monitor;
        for (const auto& id : sync.adopt) {
            const auto found = geometry_of.find(id);
            // Unknown geometry means the window went away between the list and now;
            // skip it rather than guess. It is offered again next tick if it is real.
            if (found == geometry_of.end()) continue;
            const Rect& geometry = found->second;

            // Centre point, not the origin: a window straddling the seam belongs to
            // whichever monitor holds most of it, and a window positioned at a
            // negative offset would otherwise be misfiled.
            const int centre_x = geometry.x + geometry.width / 2;
            const int centre_y = geometry.y + geometry.height / 2;
            if (centre_x >= monitor_.x && centre_x < monitor_.x + monitor_.width &&
                centre_y >= monitor_.y && centre_y < monitor_.y + monitor_.height) {
                on_this_monitor.push_back(id);
            }
        }
        sync.adopt = std::move(on_this_monitor);
    }

    const bool visibility_changed = hidden != last_hidden_;
    if (visibility_changed) {
        last_hidden_  = hidden;
        needs_redraw_ = true;
    }

    if (!sync.empty()) {
        needs_redraw_ = true;
        apply_canvas_sync(canvas_, sync, tile_width_, tile_height_);

        // Record each newly adopted window's real size as its zoom-1.0 baseline.
        // Without it there is nothing to scale from, and a window adopted while
        // already zoomed out would be stuck at whatever size it happened to have.
        for (const auto& id : sync.adopt) {
            const auto found = geometry_of.find(id);
            if (found == geometry_of.end()) continue;
            if (auto placement = canvas_.placement_of(id)) {
                // Divided by the zoom, because placement sizes are CANVAS units and
                // the geometry just read is screen pixels. Storing the observed size
                // directly meant a window adopted while zoomed out recorded its
                // shrunken size as its true size -- and "1:1" then restored it to
                // that, so zoom stopped being reversible.
                const double z = canvas_.zoom() > 0 ? canvas_.zoom() : 1.0;
                placement->width  = static_cast<int>(found->second.width  / z);
                placement->height = static_cast<int>(found->second.height / z);
                canvas_.place(id, *placement);
            }
        }
        // Auto-fit: the set of windows changed, so re-lay everything out to fit the
        // screen. This is what makes the canvas feel managed rather than merely
        // large -- open a fourth window and all four resize to share the space,
        // instead of the new one landing wherever the next free slot happened to be.
        if (auto_fit_) fit_all();
        apply_now();
    } else if (visibility_changed && auto_fit_) {
        // Minimise and restore change the set that should occupy the grid just as
        // opening and closing do. This was the missing trigger that left a hole
        // after minimising and restored a window at an old, apparently lost
        // position.
        fit_all();
    } else {
        // Still refresh the cached positions so the minimap does not go stale when
        // nothing was adopted but a window moved.
        last_positions_ = canvas_.resolve();
    }

    // Do this after adoption/layout so a newly opened window maximised by its own
    // application is already managed. The conversion clears native maximise,
    // sizes it to the realised bar gap, and records that geometry in canvas units;
    // subsequent pans therefore move it normally instead of leaving it pinned.
    for (const auto& id : native_maximized) {
        if (canvas_.manages(id)) full_managed_window(id);
    }

    // Nothing changed on most ticks, and repainting anyway is a full-screen
    // composite per second for an identical picture.
    if (needs_redraw_) {
        needs_redraw_ = false;
        area_.queue_draw();
    }
}

// ---------------------------------------------------------------------------
void DesktopWindow::load_wallpaper() {
    // The configured picture wins; otherwise inherit whatever the session is
    // already showing, so the canvas looks like the user's desktop on first run
    // rather than a flat colour they have to go and fix.
    wallpaper_path_ = config_.background;
    if (wallpaper_path_.empty()) {
        if (const auto found = display().current_wallpaper()) wallpaper_path_ = *found;
    }
    if (wallpaper_path_.empty()) return;

    try {
        const auto source = Gdk::Pixbuf::create_from_file(wallpaper_path_);

        // Cover-scale here, once, rather than in the draw handler. The draw handler
        // then paints it 1:1, which is a straight blit.
        const double scale = std::max(
            static_cast<double>(monitor_.width)  / source->get_width(),
            static_cast<double>(monitor_.height) / source->get_height());
        const int scaled_w = std::max(1, static_cast<int>(source->get_width()  * scale));
        const int scaled_h = std::max(1, static_cast<int>(source->get_height() * scale));
        wallpaper_ = source->scale_simple(scaled_w, scaled_h, Gdk::InterpType::BILINEAR);
    } catch (const Glib::Error& e) {
        // A missing or corrupt image is not worth failing the desktop over -- the
        // themed background is a perfectly good fallback.
        std::cerr << "auspex-shell: could not load wallpaper " << wallpaper_path_ << ": "
                  << e.what() << "\n";
        wallpaper_.reset();
    }
}

void DesktopWindow::draw_wallpaper(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                                   int height) const {
    if (!wallpaper_) return;

    // Already cover-scaled at load, so this is a blit with an offset -- no resample.
    // Centred, which is where the cropping happens: the pre-scaled image is at least
    // as large as the monitor in both axes, and the overflow hangs off both edges.
    const double offset_x = (width  - wallpaper_->get_width())  / 2.0;
    const double offset_y = (height - wallpaper_->get_height()) / 2.0;

    Gdk::Cairo::set_source_pixbuf(cr, wallpaper_, offset_x, offset_y);
    cr->paint();
}

void DesktopWindow::on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                            int height) {
    const Palette& palette = theme_by_name(config_.theme);
    const Rgb      bg      = parse_hex(palette.panel_bg);

    // Always fill first: the wallpaper may fail to load, and a transparent desktop
    // window shows whatever stale pixels the X server had there.
    set_rgb(cr, bg);
    cr->rectangle(0, 0, width, height);
    cr->fill();

    // The wallpaper does NOT pan with the canvas. It is the backdrop of the whole
    // space rather than an object in it, so it stays put while the grid and the
    // windows move over it -- which is also what makes panning legible, since a
    // sliding photograph gives the eye nothing fixed to measure against.
    draw_wallpaper(cr, width, height);

    // Darken slightly so the grid, minimap and window edges stay readable over a
    // bright photograph. Skipped entirely when there is no wallpaper, where the
    // themed background is already dark.
    if (wallpaper_) {
        cr->set_source_rgba(bg.r, bg.g, bg.b, 0.35);
        cr->rectangle(0, 0, width, height);
        cr->fill();
    }

    draw_grid(cr, width, height);
    draw_ghosts(cr, width, height);
    draw_minimap(cr, width, height);
    draw_position(cr, width, height);
}

void DesktopWindow::draw_grid(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                              int height) const {
    const Palette& palette = theme_by_name(config_.theme);
    const Rgb      line    = parse_hex(palette.button_hover);
    const Viewport v       = canvas_.viewport();

    cr->set_line_width(1.0);

    // Start at the first grid line at or before the left edge of the viewport, then
    // walk in canvas space and convert to screen. Doing it this way -- rather than
    // drawing a fixed grid and offsetting it -- is what makes the lines belong to
    // the canvas: pan far enough and the major lines still land on multiples of
    // 512, so the space reads as one continuous sheet rather than a repeating tile.
    const double first_x = floor_to_step(v.x, kGridStep);
    for (double cx = first_x; cx < v.x + width; cx += kGridStep) {
        const double sx    = cx - v.x + 0.5;   // +0.5 so a 1px line is not blurred
        const bool   major = std::fmod(std::abs(cx / kGridStep), kMajorEvery) < 0.5;
        set_rgb(cr, line, major ? 0.55 : 0.22);
        cr->move_to(sx, 0);
        cr->line_to(sx, height);
        cr->stroke();
    }

    const double first_y = floor_to_step(v.y, kGridStep);
    for (double cy = first_y; cy < v.y + height; cy += kGridStep) {
        const double sy    = cy - v.y + 0.5;
        const bool   major = std::fmod(std::abs(cy / kGridStep), kMajorEvery) < 0.5;
        set_rgb(cr, line, major ? 0.55 : 0.22);
        cr->move_to(0, sy);
        cr->line_to(width, sy);
        cr->stroke();
    }

    // The origin. Without a landmark an infinite grid gives no sense of place, and
    // "Home" would feel like it went nowhere in particular.
    const double ox = -v.x;
    const double oy = -v.y;
    if (ox > -200 && ox < width + 200 && oy > -200 && oy < height + 200) {
        set_rgb(cr, parse_hex(palette.accent), 0.8);
        cr->set_line_width(2.0);
        cr->move_to(ox - 18, oy);
        cr->line_to(ox + 18, oy);
        cr->move_to(ox, oy - 18);
        cr->line_to(ox, oy + 18);
        cr->stroke();
    }
}

void DesktopWindow::draw_ghosts(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                                int height) const {
    const Palette& palette = theme_by_name(config_.theme);
    const Viewport v       = canvas_.viewport();
    const double   zoom    = canvas_.zoom();

    cr->select_font_face("sans", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(13);

    // Clipped to the same band the windows live in, so a ghost cannot be drawn
    // underneath a panel where it would be invisible anyway.
    const auto area = current_workarea();
    const int  top    = area ? std::max(0, area->y - monitor_.y) : 0;
    const int  bottom = area ? std::min(height, (area->y + area->height) - monitor_.y)
                             : height;
    cr->save();
    cr->rectangle(0, top, width, std::max(0, bottom - top));
    cr->clip();

    for (const auto& id : canvas_.managed_ids()) {
        if (canvas_.is_hidden(id)) continue;

        // Only windows the canvas had to park. A window that is on screen is drawn
        // by itself and does not want an outline over the top of it.
        const auto shown = std::find_if(
            last_positions_.begin(), last_positions_.end(),
            [&](const ScreenPosition& p) { return p.id == id && p.visible; });
        if (shown != last_positions_.end()) continue;

        const auto placement = canvas_.placement_of(id);
        if (!placement || placement->width <= 0 || placement->height <= 0) continue;

        const double x = (placement->x - v.x) * zoom;
        const double y = (placement->y - v.y) * zoom;
        const double w = placement->width  * zoom;
        const double h = placement->height * zoom;

        // Off the far side of the canvas in either direction: nothing to suggest.
        if (x + w < -40 || y + h < -40 || x > width + 40 || y > height + 40) continue;

        set_rgb(cr, parse_hex(palette.accent), 0.14);
        cr->rectangle(x, y, w, h);
        cr->fill();

        // Dashed, so it reads as "this is not here yet" rather than as a window
        // that failed to draw.
        set_rgb(cr, parse_hex(palette.accent), 0.55);
        cr->set_line_width(2.0);
        cr->set_dash(std::vector<double>{8.0, 6.0}, 0.0);
        cr->rectangle(x + 1, y + 1, w - 2, h - 2);
        cr->stroke();
        cr->unset_dash();

        if (const auto title = titles_.find(id);
            title != titles_.end() && !title->second.empty() && w > 80) {
            set_rgb(cr, parse_hex(palette.panel_fg), 0.75);
            cr->save();
            cr->rectangle(x, y, w, h);
            cr->clip();                     // a long title cannot escape its ghost
            cr->move_to(x + 10, y + 24);
            cr->show_text(title->second);
            cr->restore();
        }
    }

    cr->restore();
}

void DesktopWindow::draw_minimap(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                                 int height) const {
    const Palette& palette = theme_by_name(config_.theme);
    const Viewport v       = canvas_.viewport();

    const auto content = canvas_.content_bounds(tile_width_, tile_height_);

    // The region the minimap covers: everything that exists, plus wherever you have
    // wandered to. Including the viewport means the indicator never leaves the box,
    // so panning into empty space still shows you moving.
    int min_x = std::min(v.x, content ? content->x : v.x);
    int min_y = std::min(v.y, content ? content->y : v.y);
    int max_x = std::max(v.x + v.width, content ? content->x + content->width : v.x + v.width);
    int max_y = std::max(v.y + v.height,
                         content ? content->y + content->height : v.y + v.height);

    const double span_x = std::max(1, max_x - min_x);
    const double span_y = std::max(1, max_y - min_y);

    const double box_x = width - kMinimapWidth - kMinimapMargin;
    const double box_y = height - kMinimapHeight - kMinimapMargin - 40;

    set_rgb(cr, parse_hex(palette.entry_bg), 0.85);
    cr->rectangle(box_x, box_y, kMinimapWidth, kMinimapHeight);
    cr->fill();
    set_rgb(cr, parse_hex(palette.entry_border), 0.9);
    cr->set_line_width(1.0);
    cr->rectangle(box_x + 0.5, box_y + 0.5, kMinimapWidth - 1, kMinimapHeight - 1);
    cr->stroke();

    const double scale = std::min((kMinimapWidth - 16) / span_x,
                                  (kMinimapHeight - 16) / span_y);
    const auto to_map = [&](double cx, double cy) {
        return std::pair{box_x + 8 + (cx - min_x) * scale,
                         box_y + 8 + (cy - min_y) * scale};
    };

    // Every managed window, whether or not it is currently on screen -- that is the
    // point of a minimap on an infinite canvas.
    set_rgb(cr, parse_hex(palette.accent), 0.65);
    for (const auto& id : canvas_.managed_ids()) {
        const auto placement = canvas_.placement_of(id);
        if (!placement) continue;
        const auto [mx, my] = to_map(placement->x, placement->y);
        cr->rectangle(mx, my, std::max(2.0, tile_width_ * scale),
                      std::max(2.0, tile_height_ * scale));
        cr->fill();
    }

    // Where you are.
    const auto [vx, vy] = to_map(v.x, v.y);
    set_rgb(cr, parse_hex(palette.launcher_bg), 0.95);
    cr->set_line_width(2.0);
    cr->rectangle(vx, vy, v.width * scale, v.height * scale);
    cr->stroke();
}

void DesktopWindow::draw_position(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                                  int height) const {
    const Palette& palette = theme_by_name(config_.theme);
    const Viewport v       = canvas_.viewport();

    const std::string label = std::to_string(v.x) + ", " + std::to_string(v.y) +
                              "    " + std::to_string(canvas_.size()) +
                              (canvas_.size() == 1 ? " window" : " windows");

    set_rgb(cr, parse_hex(palette.subtitle_fg), 0.75);
    cr->select_font_face("monospace", Cairo::ToyFontFace::Slant::NORMAL,
                         Cairo::ToyFontFace::Weight::NORMAL);
    cr->set_font_size(13);
    cr->move_to(width - kMinimapWidth - kMinimapMargin, height - kMinimapMargin - 16);
    cr->show_text(label);
}

}  // namespace auspex::gtk
