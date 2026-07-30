#include "auspex/panel_dock.hpp"

#include <sstream>
#include <string>

#include <unistd.h>

#include "auspex/process.hpp"

namespace auspex {

std::string_view to_string(PanelPosition position) {
    return position == PanelPosition::Top ? "top" : "bottom";
}

PanelLayout compute_panel_layout(const Rect& monitor, int scale_factor,
                                 int configured_height, PanelPosition position) {
    if (scale_factor < 1) scale_factor = 1;
    if (configured_height < 1) configured_height = 1;

    // Upstream padded the scaled height differently per edge (+4 top, +8 bottom).
    // Preserved rather than normalised: the bottom panel's taller controls rely
    // on it, and changing it silently reflows the shell.
    const int padding = position == PanelPosition::Bottom ? 8 : 4;
    return layout_for_height(monitor, configured_height * scale_factor + padding, position);
}

PanelLayout layout_for_height(const Rect& monitor, int height, PanelPosition position) {
    if (height < 1) height = 1;

    PanelLayout layout;
    layout.bounds.x      = monitor.x;
    layout.bounds.width  = monitor.width;
    layout.bounds.height = height;
    layout.bounds.y      = position == PanelPosition::Top
                               ? monitor.y
                               : monitor.y + monitor.height - height;

    // _NET_WM_STRUT_PARTIAL is 12 cardinals:
    //   left, right, top, bottom,
    //   left_start_y, left_end_y, right_start_y, right_end_y,
    //   top_start_x, top_end_x, bottom_start_x, bottom_end_x
    const int x0 = monitor.x;
    const int x1 = monitor.x + monitor.width;

    // std::to_string, NOT ostringstream. GTK calls setlocale() during init, and
    // under a locale with digit grouping (en_US.UTF-8 and most others) an
    // ostringstream renders 1920 as "1,920". xprop then reads that as two values
    // and the 12-element strut becomes 13, so the window manager silently
    // reserves nothing. std::to_string is locale-independent by definition.
    const std::string h  = std::to_string(height);
    const std::string a  = std::to_string(x0);
    const std::string b  = std::to_string(x1);

    if (position == PanelPosition::Top) {
        layout.strut_partial = "0, 0, " + h + ", 0, 0, 0, 0, 0, " + a + ", " + b + ", 0, 0";
    } else {
        layout.strut_partial = "0, 0, 0, " + h + ", 0, 0, 0, 0, 0, 0, " + a + ", " + b;
    }

    return layout;
}

namespace dock {

std::optional<std::string> find_window_id(std::string_view title) {
    const std::string wanted(title);

    // Preferred: our own PID's windows, confirmed by title. GTK4 windows report
    // class Gtk4Window under X11.
    const auto by_pid = run({"xdotool", "search", "--pid", std::to_string(::getpid()),
                             "--class", "Gtk4Window"},
                            true);
    if (by_pid.ok) {
        for (const auto& candidate : split_lines(by_pid.out)) {
            const std::string wid = trim(candidate);
            if (wid.empty()) continue;
            const auto info = run({"xwininfo", "-id", wid}, true);
            if (info.ok && info.out.find(wanted) != std::string::npos) return wid;
        }
    }

    // Fallback: scan the window list by title.
    const auto listing = run({"wmctrl", "-l"}, true);
    if (listing.ok) {
        for (const auto& line : split_lines(listing.out)) {
            if (line.find(wanted) == std::string::npos) continue;
            std::istringstream fields(line);
            std::string wid;
            if (fields >> wid && !wid.empty()) return wid;
        }
    }

    return std::nullopt;
}

bool set_dock_window_type(std::string_view window_id) {
    return run({"xprop", "-id", std::string(window_id),
                "-f", "_NET_WM_WINDOW_TYPE", "32a",
                "-set", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"},
               false)
        .ok;
}

bool set_strut_partial(std::string_view window_id, const std::string& strut) {
    return run({"xprop", "-id", std::string(window_id),
                "-f", "_NET_WM_STRUT_PARTIAL", "32c",
                "-set", "_NET_WM_STRUT_PARTIAL", strut},
               false)
        .ok;
}

bool move_resize(std::string_view window_id, const Rect& bounds) {
    const std::string wid(window_id);
    const bool moved = run({"xdotool", "windowmove", wid,
                            std::to_string(bounds.x), std::to_string(bounds.y)},
                           false)
                           .ok;
    const bool sized = run({"xdotool", "windowsize", wid,
                            std::to_string(bounds.width), std::to_string(bounds.height)},
                           false)
                           .ok;
    return moved && sized;
}

bool set_sticky_above(std::string_view window_id) {
    return run({"wmctrl", "-i", "-r", std::string(window_id), "-b", "add,sticky,above"},
               false)
        .ok;
}

bool apply(std::string_view window_id, const PanelLayout& layout) {
    // Order matters: the type hint must be set before the WM computes the frame,
    // and struts are only honoured once the window has its final geometry.
    bool ok = set_dock_window_type(window_id);
    ok = move_resize(window_id, layout.bounds) && ok;
    ok = set_strut_partial(window_id, layout.strut_partial) && ok;
    ok = set_sticky_above(window_id) && ok;
    return ok;
}

}  // namespace dock

}  // namespace auspex
