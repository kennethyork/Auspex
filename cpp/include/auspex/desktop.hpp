// Workspace and window enumeration.
//
// Port of the logic in src/magi_shell/widgets/workspace.py and window.py, with the
// GTK widget shells left out -- this is the data layer those buttons render. Kept
// GTK-free so it is testable and so auspex-core stays linkable without gtkmm.
//
// Both source files had bugs that are fixed here rather than carried over:
//
//   workspace.py::_switch_workspace called GLib.timeout_add, but the module only
//   imports Gtk -- clicking a workspace button raised NameError.
//
//   workspace.py::_update_current_workspace opened with
//       current = cache.get('current_workspace')
//       if current is not None: return
//   so once the cache was populated it returned early forever and the
//   active-workspace highlight froze after the first poll. There is no cache here;
//   the current workspace is read each time it is asked for.
//
// These functions are the shell's vocabulary for talking about the desktop, and
// they are display-server independent: each one either computes something from
// values it was handed, or forwards to the DisplayServer backend in display.hpp.
// Nothing in this file forks a helper or names an X11 utility any more. Adding a
// Wayland backend does not change a single declaration here.
#pragma once

#include <optional>

#include "auspex/panel_dock.hpp"
#include <string>
#include <vector>

namespace auspex {

struct Workspace {
    int         index = 0;
    bool        active = false;
    std::string name;

    bool operator==(const Workspace&) const = default;
};

// GTK4 removed the primary-monitor concept: Gdk::Display::get_monitors() returns
// an unordered list with no primary flag, and get_primary_monitor() (which
// panel.py called) does not exist. So which monitor the panel docks to has to be
// decided explicitly. xrandr is the authority X11 itself uses, and it marks the
// primary output with '*'.
struct MonitorInfo {
    int         index = 0;
    std::string connector;   // "DP-2", "HDMI-0", ...
    bool        primary = false;
    Rect        bounds;

    bool operator==(const MonitorInfo&) const = default;
};

// Cached briefly. Measured on a 4-output NVIDIA box, `xrandr --listmonitors` takes
// 333ms -- it is by an order of magnitude the most expensive question this file can
// ask, and the answer changes only when someone plugs in a monitor. Two panels
// polling it once a second each spent two thirds of a core on it.
std::vector<MonitorInfo> list_monitors();

// Drops every cache below. Called by set_display_server(), so swapping backends in
// a test never sees another backend's answers.
void invalidate_desktop_caches();

// The output xrandr marks primary; falls back to the monitor at the origin, then
// to the first listed. nullopt only if xrandr reports nothing.
std::optional<MonitorInfo> primary_monitor();

// The rectangle covering every monitor -- the X screen the outputs are laid out on.
//
// This is what "off screen" has to mean on a multi-head desk. The canvas owns one
// monitor and hides anything it cannot fit by moving it outside that monitor, but
// outside the primary monitor is where the user's OTHER monitors are: a window
// pushed off the left edge lands on the screen to the left, in full view. The union
// is the only rectangle a window can be moved beyond and genuinely disappear.
//
// nullopt when no monitor is known, which is the same condition primary_monitor()
// returns nullopt for.
std::optional<Rect> screen_bounds();

// Exposed for testing: the same union, over a list you supply.
std::optional<Rect> screen_bounds(const std::vector<MonitorInfo>& monitors);

// Exposed for testing against captured `xrandr --listmonitors` output.
std::vector<MonitorInfo> parse_monitors(const std::string& listmonitors_output);

// Parses `xrandr -q --current`, which is a different format from --listmonitors
// and is the one actually used at runtime.
//
// WHY: measured on a 4-output NVIDIA box, `xrandr --listmonitors` takes 334ms and
// `xrandr -q --current` takes 5ms -- 67x -- because --current skips re-probing
// every output's mode list. That one call was over half of the shell's entire CPU
// use. --listmonitors stays supported as a fallback; nothing else changes.
//
// Lines of interest look like:
//   DP-0 connected primary 1920x1080+1920+0 (normal left ...) 698mm x 393mm
//   DP-2 connected 1920x1080+0+0 (normal left ...) 698mm x 393mm
//   DP-1 disconnected (normal left ...)
std::vector<MonitorInfo> parse_monitors_current(const std::string& xrandr_q_output);

struct WindowEntry {
    std::string id;             // 0x........ as reported by wmctrl
    int         workspace = 0;  // -1 for sticky windows
    std::string title;

    bool operator==(const WindowEntry&) const = default;
};

// Every workspace the session has. Empty if no window manager is running.
std::vector<Workspace> list_workspaces();

// Index of the workspace currently active, or nullopt.
std::optional<int> current_workspace();

// Switches workspace and confirms it happened. True only if the switch is
// observable afterwards -- the request itself is asynchronous, so this polls.
bool switch_workspace(int index);

// The usable screen rectangle after panel struts. This is the authority on where
// the bars are NOT, so canvas tiles laid out inside it can never sit underneath a
// panel.
std::optional<Rect> current_workarea();

// A window together with where it currently is on screen.
//
// Exists because asking for geometry one window at a time does not scale to a
// per-second reconcile: the canvas needs the position of everything it manages on
// every tick, both to notice windows the user has dragged and to decide which
// monitor a new window is on. `wmctrl -lG` answers that for every window in a
// single call, where xwininfo needs one fork each.
struct PlacedWindow {
    WindowEntry window;
    Rect        bounds;

    bool operator==(const PlacedWindow&) const = default;
};

// One window id spelling, so ids from different helpers can be compared.
//
// This is not cosmetic. wmctrl prints `0x04400006`; xdotool prints `71303174`;
// they are the same window. Comparing them as strings silently never matches, and
// the symptom is not an error but a feature that quietly does nothing -- the
// minimised-window set comes back empty and every window looks visible.
//
// Accepts decimal or 0x-prefixed hex, returns `0x` + eight lower-case hex digits,
// which is wmctrl's spelling. Empty for anything unparseable.
std::string canonical_window_id(std::string_view id);

// Every top-level window. Titles are truncated by the caller, not here.
std::vector<WindowEntry> list_windows();

// Ids of windows currently on screen. A window that is open but minimised appears
// in list_windows() and NOT here; that difference is the definition of minimised.
std::vector<std::string> list_visible_windows();

// The decoration a window manager draws around a window: title bar and borders,
// in pixels, from _NET_FRAME_EXTENTS.
//
// Needed because move and resize address different rectangles. `xdotool
// windowsize` sets the CLIENT size, while the space the window actually occupies
// is the client plus these -- so a window told to be exactly as tall as the gap
// between the panels ends up a title bar taller than the gap, and overhangs the
// bottom bar. Measured on xfwm4: left 5, right 5, top 29, bottom 5.
struct FrameExtents {
    int left = 0, right = 0, top = 0, bottom = 0;

    bool operator==(const FrameExtents&) const = default;
};

// Parses `xprop -id <id> _NET_FRAME_EXTENTS`. All zero when the property is
// absent, which is correct: an undecorated window has no frame.
FrameExtents parse_frame_extents(const std::string& xprop_output);

// nullopt when the window is gone or the property cannot be read.
std::optional<FrameExtents> frame_extents(std::string_view window_id);

// Iconify / de-iconify.
bool minimize_window(std::string_view window_id);
bool restore_window(std::string_view window_id);
bool maximize_window(std::string_view window_id);

// Politely ask a window to close. The application decides what that means -- it
// may prompt about unsaved work, or ignore it. Never kills a process.
bool close_window(std::string_view window_id);

// Id of the focused window, or nullopt.
std::optional<std::string> focused_window_id();

// Every top-level window and its geometry, in one round trip.
std::vector<PlacedWindow> list_placed_windows();

// Upstream skipped any title containing "MAGI" or "Desktop" to keep the panel out
// of its own window list. "Desktop" is too broad -- it also hides a user window
// merely named e.g. "Desktop Notes" -- so only the shell's own windows and the
// root desktop window are excluded here.
bool is_shell_window(const WindowEntry& window);

std::vector<WindowEntry> list_user_windows();

// Activate and raise.
bool activate_window(std::string_view window_id);

// Title of whatever currently holds focus. Used to build command context and to
// describe "the focused window" to the model.
std::optional<std::string> focused_window_title();

// The text the user has highlighted (the primary selection), not the clipboard
// they explicitly copied. nullopt when nothing is selected.
std::optional<std::string> selected_text();

// Whether selected_text() can answer at all. False means the environment is missing
// the helper it needs, and every read will come back empty no matter what is
// highlighted -- which callers must say out loud rather than reporting as "nothing
// selected", and must not keep retrying on a timer.
bool can_read_selection();

// Synthesises typing into whatever holds focus. How dictation delivers its result.
bool type_text(std::string_view text);

// Parsers exposed for testing against captured helper output, and used by the X11
// backend in src/display_x11.cpp. They are pure text-in / values-out, which is why
// they live here rather than with the backend: they are the half of the X11 layer
// that can be tested without a display server, and 20 of the selftests do exactly
// that.
std::vector<Workspace>   parse_workspaces(const std::string& wmctrl_d_output);
std::vector<WindowEntry> parse_windows(const std::string& wmctrl_l_output);
std::vector<PlacedWindow> parse_placed_windows(const std::string& wmctrl_lG_output);
std::optional<Rect>      parse_workarea(const std::string& xprop_workarea_output);
std::optional<Rect>      parse_window_geometry(const std::string& xwininfo_output);

}  // namespace auspex
