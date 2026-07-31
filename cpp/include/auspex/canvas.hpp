// An infinite canvas over real desktop windows.
//
// Auspex keeps a canvas coordinate for every managed window and pans by moving the
// real windows, so the screen becomes a viewport onto a space larger than itself.
// The windows stay ordinary top-levels: xfwm4 keeps drawing their decorations and
// handling drags, and Auspex only ever sets positions. Nothing is reparented.
//
// WHY NOT REPARENTING: XReparentWindow can pull a foreign window into an Auspex
// container, but GTK4 removed GtkSocket so there is no supported way to lay it out,
// reparented windows lose their decorations, and if Auspex dies they are orphaned
// off-screen. Position management achieves the same panning with none of that.
//
// WHAT THIS CANNOT DO YET: zoom. Scaling another application's contents requires
// owning the compositor. Panning is a position change and needs no such privilege;
// zooming is not, so it waits until Auspex is the compositor -- at which point the
// scene graph makes it a per-node transform and this file's position bookkeeping
// becomes the input to it rather than the mechanism.
//
// Everything here is expressed in canvas coordinates and applied through
// DisplayServer, so none of the layout logic is X11-specific. Only the final
// move_window() call crosses the seam.
#pragma once

#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include "auspex/desktop.hpp"
#include "auspex/panel_dock.hpp"

namespace auspex {

// Where a window sits in canvas space, independent of what is currently on screen.
struct CanvasPlacement {
    int x = 0;
    int y = 0;

    // The window's size at zoom 1.0, captured when it was adopted. Zoom scales
    // this; without it there is nothing to scale FROM, and zooming twice would
    // compound off whatever size the last zoom happened to leave behind.
    int width  = 0;
    int height = 0;

    bool operator==(const CanvasPlacement&) const = default;
};

// The rectangle of canvas space currently shown on the monitor.
struct Viewport {
    int x = 0;
    int y = 0;
    int width  = 1920;
    int height = 1080;

    bool operator==(const Viewport&) const = default;
};

// A window's on-screen position for the current viewport, plus whether it overlaps
// the view. Off-screen windows retain their true canvas-derived coordinates so
// panning never pins them to a monitor edge.
struct ScreenPosition {
    std::string id;
    int  x = 0;
    int  y = 0;
    bool visible = false;

    // Size to resize the window to. Zero means "leave it alone", which is the
    // normal case -- panning must never change how big anything is. Fit and
    // explicit zoom operations set these.
    int width  = 0;
    int height = 0;

    bool operator==(const ScreenPosition&) const = default;
};

// A grid that holds `count` windows inside a viewport.
struct FitLayout {
    int columns     = 1;
    int rows        = 1;
    int tile_width  = 0;
    int tile_height = 0;

    bool operator==(const FitLayout&) const = default;
};

// Chooses the grid that gives the largest possible tile for `count` windows in a
// `width` x `height` area.
//
// Not ceil(sqrt(n)): that ignores the aspect ratio of the screen, and on a 16:9
// monitor it produces tall narrow tiles that waste horizontal space. This tries
// every column count and keeps whichever yields the biggest tile area, which is
// both obviously correct and cheap -- `count` is the number of windows on a
// desktop, not a number that grows.
//
// Tiles stop shrinking at a readable minimum. Past that point the grid is larger
// than the viewport on purpose and the canvas scrolls, which is the honest
// outcome: forty windows cannot be legible on one screen, and pretending
// otherwise by shrinking them to slivers helps nobody.
FitLayout compute_fit_layout(int count, int width, int height);

class Canvas {
public:
    // Reserved screen space, so tiles are never placed under the panels. Pass the
    // strut heights the panels actually reserve.
    void set_insets(int top, int bottom);

    void set_viewport(const Viewport& viewport);
    const Viewport& viewport() const { return viewport_; }

    // Adopts a window at an explicit canvas position.
    void place(const std::string& window_id, CanvasPlacement placement);

    // Adopts a window into the next free slot in the visible viewport, laid out on a
    // grid so newly opened terminals do not stack exactly on top of each other.
    CanvasPlacement place_next(const std::string& window_id, int tile_width,
                               int tile_height);

    void forget(const std::string& window_id);
    bool manages(const std::string& window_id) const;

    // Every window on the canvas, in a stable order. What a reconcile pass diffs
    // the live window list against.
    std::vector<std::string> managed_ids() const;

    std::optional<CanvasPlacement> placement_of(const std::string& window_id) const;

    // Records where the user dragged a window to. The screen position is converted
    // back into canvas space, so a drag survives the next pan -- without this, any
    // manual move would be undone the moment the viewport changed.
    void note_screen_move(const std::string& window_id, int screen_x, int screen_y);

    void pan_by(int dx, int dy);

    // --- zoom ----------------------------------------------------------------
    //
    // On X11 this is zoom by RESIZE: the windows really become smaller and redraw
    // at that size, rather than their pixels being scaled. Two consequences worth
    // knowing -- text stays sharp instead of blurring, and an application with a
    // minimum size will simply refuse to go below it.
    //
    // Under Auspex's own compositor the same numbers drive
    // wlr_scene_buffer_set_dest_size() instead and both consequences disappear.
    // The arithmetic here does not change; only what consumes it does.
    double zoom() const { return zoom_; }

    // Multiplies the current zoom, keeping whatever is at the CENTRE of the
    // viewport fixed. Zooming around the origin instead would fling the thing you
    // were looking at off the edge, which is the difference between a zoom that
    // feels like a zoom and one that feels like a jump.
    void zoom_by(double factor);
    void set_zoom(double zoom);
    void reset_zoom();

    // Windows that are minimised. They stay MANAGED -- the canvas remembers where
    // they belong so restoring one puts it back rather than filing it somewhere new
    // -- but they are excluded from resolve() and from fit_all().
    //
    // Excluding them is not an optimisation. Moving an iconified window makes the
    // window manager de-iconify it, so a canvas that repositioned everything it
    // manages would silently un-minimise anything you minimised, within the second.
    void set_hidden(std::set<std::string> hidden);
    bool is_hidden(const std::string& window_id) const;

    // Re-lays every managed window onto a grid that fits inside the current
    // viewport, and returns the tile size they should all be resized to.
    //
    // This is the "show me everything" operation. Since Auspex cannot yet zoom --
    // that needs the compositor -- fitting means making the windows smaller rather
    // than making the view bigger. The two are indistinguishable on screen; they
    // differ only in that this one really does resize other people's windows, which
    // is why it is an explicit act and not something panning ever does.
    //
    // Returns a zero-tile layout when nothing is managed.
    FitLayout fit_all();

    // Centres the viewport on a managed window. Used by "go to the build terminal".
    bool focus_on(const std::string& window_id, int window_width, int window_height);

    // Where every managed window should be right now.
    //
    // `include_life_size` is for an explicit zoom operation. Ordinarily a resolve
    // at 1.0 leaves window sizes alone, so panning does not resize applications on
    // every frame. The transition back to 1.0 is different: it must issue one
    // resize to the stored natural dimensions or the real X11 window remains
    // stuck at the last zoomed-out size.
    std::vector<ScreenPosition> resolve(bool include_life_size = false) const;

    // Canvas-space bounding box of everything managed, which is what "fit
    // everything on screen" would need. Empty if nothing is managed.
    std::optional<Rect> content_bounds(int tile_width, int tile_height) const;

    std::size_t size() const { return placements_.size(); }

private:
    Viewport viewport_{};
    int inset_top_ = 0;
    int inset_bottom_ = 0;

    // Screen pixels per canvas unit. 1.0 is life size.
    double zoom_ = 1.0;

    std::map<std::string, CanvasPlacement> placements_;
    std::set<std::string>                  hidden_;
};

// Applies resolved positions to the real windows, through the DisplayServer.
// Off-viewport windows keep following their canvas coordinates rather than being
// clamped or parked at a screen edge.
//
// For a window's current geometry -- what note_screen_move needs -- call
// display().window_geometry() directly; there is nothing canvas-specific to add.
bool apply_positions(const std::vector<ScreenPosition>& positions, const Rect& monitor);

// --- adoption ---------------------------------------------------------------
//
// The canvas is the desktop, so it does not wait to be asked: every window that
// opens joins it and every window that closes leaves it. This is the difference
// between a canvas you have to put things on and one you are simply working in.

// What a reconcile pass should change. Two lists rather than a mutation so the
// decision can be tested with no windows, no canvas and no display server -- which
// is most of what can go wrong here.
struct CanvasSync {
    std::vector<std::string> adopt;   // live, not yet on the canvas
    std::vector<std::string> forget;  // on the canvas, no longer live

    bool empty() const { return adopt.empty() && forget.empty(); }
    bool operator==(const CanvasSync&) const = default;
};

// Diffs the live window list against what the canvas already manages.
//
// `live` should be list_user_windows() -- already filtered of Auspex's own panels
// and of the root desktop window. Adopting a panel would let a pan drag the panel
// off the edge of the screen; adopting the desktop window would make the canvas
// try to pan itself.
//
// Order is stable: `adopt` follows the order of `live`, so windows opened earlier
// take earlier tile slots and the layout does not reshuffle between passes.
CanvasSync plan_canvas_sync(const std::vector<WindowEntry>& live, const Canvas& canvas);

// Applies a sync to the canvas. Split from the planning above so that the part
// with a decision in it stays pure; this half is bookkeeping.
void apply_canvas_sync(Canvas& canvas, const CanvasSync& sync, int tile_width,
                       int tile_height);

}  // namespace auspex
