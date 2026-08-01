// The canvas you can see and drag.
//
// Everything in canvas.hpp is bookkeeping: it knows where windows sit in a space
// larger than the screen and what that means for their on-screen positions. None of
// it is visible, and until now the only way to move the viewport was to say "pan
// left" out loud. This window is the other half -- the substrate itself.
//
// It is a sticky _NET_WM_STATE_BELOW window covering the monitor, one layer above
// xfdesktop. If both were desktop-type siblings, Mint's desktop wins the stacking
// race and swallows every empty-space pointer event. BELOW still keeps every
// application above Auspex, while dragging empty desktop reaches this surface and
// moves the canvas like a drawing program.
//
// WHY NOT A GLOBAL POINTER GRAB: it would work, and it would also swallow every
// click meant for an application. A below-layer window gets precisely the empty
// area left by applications, which is the correct set.
//
// WHAT THIS STILL CANNOT DO: zoom, and pan smoothly. Both need the compositor.
// Panning here repositions real X11 windows, so a drag costs one move per managed
// window per frame; that is fine for a dozen windows and visibly not fine for a
// hundred. The pan is therefore rate-limited to the frame clock and the grid
// underneath redraws freely, so the canvas always *feels* continuous even when the
// windows on it lag a frame behind. Under Auspex's own compositor both become a
// single scene-graph transform and the rate limit goes away.
#pragma once

#include <gdkmm/pixbuf.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/window.h>

#include <map>
#include <optional>
#include <set>

#include <string>
#include <vector>

#include "auspex/canvas.hpp"
#include "auspex/config.hpp"
#include "auspex/desktop.hpp"

namespace auspex::gtk {

class DesktopWindow : public Gtk::Window {
public:
    // The canvas is borrowed, not owned: the shell owns it, and the voice verbs
    // pan the same object this window draws. That shared reference is what makes
    // "pan left" and a mouse drag land in the same place.
    DesktopWindow(const Config& config, Canvas& canvas, const Rect& monitor);

    // Re-reads the live window list, adopts what is new, drops what closed, and
    // repositions everything. Safe to call at any time; called on a timer.
    void reconcile();

    // Moves the viewport and repositions the real windows to match.
    void pan_by(int dx, int dy);

    // One monitor-relative navigation step, used by persistent panel controls when
    // a full window leaves no bare desktop surface to drag.
    void pan_step(int x_direction, int y_direction);

    // Back to the origin of canvas space.
    void go_home();

    // Ctrl+scroll, or + / - / 0.
    void zoom_by(double factor);
    void reset_zoom();

    // Re-lays and resizes every managed window so they all fit on screen at once.
    // Bound to F, and run automatically whenever a window opens or closes.
    void fit_all();

    // True for grid, false for canvas. Switching TO grid lays out immediately --
    // asking for the grid and being told it will happen next time a window opens
    // would be a control that appears not to work.
    void set_grid_mode(bool grid);

    // Zoom keeping the canvas point under (px, py) fixed.
    //
    // This is what makes a wheel feel like a canvas rather than a slider: the thing
    // you are pointing at is the thing that stays still. The panel's buttons and
    // the keyboard deliberately do NOT use it -- an explicit control should do the
    // same thing wherever the mouse happens to be resting.
    void zoom_at(double factor, double px, double py);

    // Frames everything that is open WITHOUT moving any of it.
    //
    // Distinct from fit_all(), which rearranges windows into a grid. This only
    // changes where you are looking and how far away, which is the only kind of
    // "fit" that means anything in canvas mode.
    void fit_view();
    bool grid_mode() const { return auto_fit_; }

    // Restores a task-list window and makes sure it rejoins the visible layout
    // immediately rather than waiting off-canvas for the next reconciliation tick.
    void restore_managed_window(const std::string& window_id);

    // Fill the visible area between the sticky bars without using native
    // maximise (the bars intentionally reserve no X11 strut).
    void full_managed_window(const std::string& window_id);

    // Realised GTK panel heights include theme padding and can differ from the
    // configured height.
    void set_panel_height(PanelPosition position, int height);

    // Overview on/off.
    //
    // This is what "zoom" can mean on X11, and it is worth being exact about the
    // difference. There is no way to scale another application's pixels here -- that
    // needs the compositor. What this does instead is make every window smaller and
    // lay them all out, so you see everything at once; a terminal reflows to fewer
    // columns rather than its text shrinking.
    //
    // Turning it off puts every window back at the size and position it had before,
    // which is the half that makes it a toggle rather than a one-way rearrangement.
    void toggle_overview();
    bool in_overview() const { return overview_; }

    // Told when overview turns on or off, so a button elsewhere can show the state.
    void set_overview_changed_handler(sigc::slot<void(bool)> handler) {
        on_overview_changed_ = std::move(handler);
    }

    // Called after the canvas moves to a different monitor, so the voice verbs --
    // which hold their own copy of the monitor rectangle -- do not keep placing
    // windows on the old screen.
    void set_monitor_changed_handler(sigc::slot<void(const Rect&)> handler) {
        on_monitor_changed_ = std::move(handler);
    }

private:
    void on_draw(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height);
    void load_wallpaper();
    void draw_wallpaper(const Cairo::RefPtr<Cairo::Context>& cr, int width,
                        int height) const;
    void draw_grid(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) const;

    // Outlines for windows that are on the canvas but not on the screen.
    //
    // This is what makes the canvas feel infinite while still never putting a real
    // window on another monitor. A real window has to be parked the moment it would
    // spill, because X11 cannot clip a foreign window to a rectangle -- so panning
    // used to make windows vanish and reappear. Their ghosts have no such limit:
    // they are drawn by Auspex, on Auspex's own surface, and can slide off the edge
    // exactly as a window would on a canvas that really was unbounded.
    void draw_minimap(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) const;
    void draw_position(const Cairo::RefPtr<Cairo::Context>& cr, int width, int height) const;

    void install_gestures();
    void claim_desktop_layer();
    bool desktop_geometry_is_wrong() const;

    // Re-reads the primary monitor and adopts it if it changed. Returns true when
    // something moved.
    //
    // The panel already did this and the desktop did not, so unplugging a monitor
    // -- or just making a different one primary -- left the canvas on the old
    // screen while the bars moved to the new one. Worse than cosmetic: the geometry
    // self-heal below compares against this rectangle, so a stale value made every
    // reconcile tick drag the canvas back to the monitor it used to be on.
    bool adopt_monitor_change();

    // Applies the canvas to the real windows. Coalesced: repeated calls within one
    // frame collapse to one, so a fast drag does not queue hundreds of window moves.
    void schedule_apply();
    void apply_now();

    const Config& config_;
    Canvas&       canvas_;
    Rect          monitor_;

    // Every monitor's union. Windows the canvas cannot show are parked outside this,
    // not merely outside monitor_ -- see apply_positions. Empty until a monitor list
    // is available, and passed as null in that case so the single-screen behaviour
    // is what a missing answer degrades to.
    std::optional<Rect> screen_;
    const Rect*         screen() const { return screen_ ? &*screen_ : nullptr; }

    Gtk::DrawingArea area_;
    std::string      title_ = "Auspex Desktop";

    // Own-window discovery, same retry shape as Panel: the X window does not exist
    // until realize and wmctrl cannot see it until it is mapped.
    std::optional<std::string> window_id_;
    int                        claim_attempts_ = 0;

    // Viewport at the moment a drag started, so the drag is absolute rather than
    // an accumulation of deltas -- accumulating drops sub-pixel remainders and the
    // canvas slowly drifts away from the pointer.
    int  drag_origin_x_ = 0;
    int  drag_origin_y_ = 0;
    bool dragging_      = false;

    bool apply_queued_ = false;

    // Tile size used when adopting a window whose real geometry is not worth a
    // round trip to ask for. A quarter of the monitor, so four fit on a screen.
    int tile_width_  = 960;
    int tile_height_ = 540;
    int panel_top_    = 0;
    int panel_bottom_ = 0;

    // Cached for the minimap so drawing never touches the display server.
    std::vector<ScreenPosition> last_positions_;

    // Titles, so a ghost can be labelled with what it is. Without a name an
    // outline sliding in from the edge is a mystery rather than a window.
    std::map<std::string, std::string> titles_;

    // Keep every window fitted on screen as the set of them changes. On by
    // default: an unfitted canvas hides windows off-viewport with nothing to say
    // they are there, which is a bad first impression of an infinite space.
    // Grid mode, or canvas mode.
    //
    // Grid is what this has always done: opening or closing a window re-lays the
    // whole screen so the windows share it. Canvas leaves them exactly where they
    // are put and lets the plane be panned around them -- which is what an infinite
    // canvas is for, and is impossible while something keeps tidying up behind you.
    //
    // The same flag drives both, because they are the same question asked once:
    // does the desktop arrange windows, or do you.
    bool      auto_fit_ = true;

    // One remembered layout per mode.
    //
    // Each mode gives back what YOU left in it. Grid is not simply re-fitted on
    // return, because a fit throws away any window you moved or resized while you
    // were in grid mode -- the grid decides the starting arrangement, not the
    // permanent one. Canvas obviously has to be remembered, since its whole point
    // is placement chosen by hand.
    //
    // Empty means "never been in that mode": grid then fits for the first time, and
    // canvas leaves everything exactly where it is.
    std::map<std::string, CanvasPlacement> canvas_layout_;
    std::map<std::string, CanvasPlacement> grid_layout_;

    // Where the pointer last was over the canvas, for pointer-anchored zoom. A
    // scroll event does not carry a position, so it has to be remembered.
    double last_pointer_x_ = 0;
    double last_pointer_y_ = 0;

    // The minimap's on-screen rectangle and the canvas region it covers, recorded
    // while drawing so a click on it can be turned back into a place to go.
    //
    // mutable because draw_minimap is const: it is a drawing routine and should
    // stay one. What it records here is a note of where it put things, not a change
    // to what the desktop is.
    mutable double minimap_x_ = 0, minimap_y_ = 0, minimap_w_ = 0, minimap_h_ = 0;
    mutable double minimap_scale_ = 1.0;
    mutable int    minimap_min_x_ = 0, minimap_min_y_ = 0;
    FitLayout last_fit_{};

    sigc::slot<void(const Rect&)> on_monitor_changed_;
    sigc::slot<void(bool)>        on_overview_changed_;

    // Where every managed window was before overview was turned on, so leaving it
    // restores rather than approximates. Keyed by window id; entries for windows
    // that closed while in overview are simply never used.
    bool                        overview_ = false;
    std::map<std::string, Rect> before_overview_;

    // The wallpaper, drawn as the canvas substrate. Null when none was found, in
    // which case the themed background colour shows through instead.
    // Pre-scaled to the monitor ONCE, at load. Scaling on every draw meant running
    // a 1920x1080 resample through Cairo every time the desktop repainted, which is
    // most of what the shell's main thread was doing.
    Glib::RefPtr<Gdk::Pixbuf>  wallpaper_;
    std::string                wallpaper_path_;

    // Redraw only when something visible actually changed. The reconcile tick fires
    // once a second forever; repainting a full-screen window on each one costs a
    // 1920x1080 composite per second to display an identical picture.
    bool needs_redraw_ = true;
    std::set<std::string> last_hidden_;
};

}  // namespace auspex::gtk
