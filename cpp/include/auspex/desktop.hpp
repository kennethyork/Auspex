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

std::vector<MonitorInfo> list_monitors();

// The output xrandr marks primary; falls back to the monitor at the origin, then
// to the first listed. nullopt only if xrandr reports nothing.
std::optional<MonitorInfo> primary_monitor();

// Exposed for testing against captured `xrandr --listmonitors` output.
std::vector<MonitorInfo> parse_monitors(const std::string& listmonitors_output);

struct WindowEntry {
    std::string id;             // 0x........ as reported by wmctrl
    int         workspace = 0;  // -1 for sticky windows
    std::string title;

    bool operator==(const WindowEntry&) const = default;
};

// `wmctrl -d`. Empty if wmctrl is missing or no WM is running.
std::vector<Workspace> list_workspaces();

// Index of the workspace marked '*', or nullopt.
std::optional<int> current_workspace();

// `wmctrl -s` followed by `xdotool set_desktop`, as upstream did. Both are
// attempted; true only if the switch is observable afterwards.
bool switch_workspace(int index);

// `wmctrl -l`. Titles are truncated by the caller, not here.
std::vector<WindowEntry> list_windows();

// Upstream skipped any title containing "MAGI" or "Desktop" to keep the panel out
// of its own window list. "Desktop" is too broad -- it also hides a user window
// merely named e.g. "Desktop Notes" -- so only the shell's own windows and the
// root desktop window are excluded here.
bool is_shell_window(const WindowEntry& window);

std::vector<WindowEntry> list_user_windows();

// `wmctrl -ia`: activate and raise.
bool activate_window(std::string_view window_id);

// Parsers exposed for testing against captured wmctrl output.
std::vector<Workspace>   parse_workspaces(const std::string& wmctrl_d_output);
std::vector<WindowEntry> parse_windows(const std::string& wmctrl_l_output);

}  // namespace auspex
