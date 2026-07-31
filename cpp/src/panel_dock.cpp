#include "auspex/panel_dock.hpp"

#include <string>

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

PanelLayout overlay_panel_layout(PanelLayout layout) {
    layout.strut_partial = "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0";
    return layout;
}

}  // namespace auspex
