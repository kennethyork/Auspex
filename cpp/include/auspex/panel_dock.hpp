// Panel geometry and X11 dock integration.
//
// Port of the window-management half of src/magi_shell/core/panel.py. Kept free
// of GTK so the strut arithmetic is unit-testable and auspex-core stays linkable
// without the GTK4 dev packages. The caller supplies the monitor rectangle it got
// from GDK; nothing here talks to a display server directly.
//
// Docking is done exactly as upstream did -- _NET_WM_WINDOW_TYPE_DOCK plus
// _NET_WM_STRUT_PARTIAL via xprop, position via xdotool, stacking via wmctrl.
// That is what makes the panel desktop-agnostic: xfwm4 honours these the same way
// marco did, which is why this port needs no Xfce-specific code.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace auspex {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    // Value equality. panel.py compared Gdk.Rectangle objects with `!=`, which
    // has no value semantics in PyGObject, so the "has the monitor changed?"
    // guard was always true and the panel re-ran three subprocesses every second
    // forever. Comparing by value is the fix.
    bool operator==(const Rect&) const = default;
};

enum class PanelPosition { Top, Bottom };

std::string_view to_string(PanelPosition position);

struct PanelLayout {
    Rect bounds;                 // where the panel window should sit
    std::string strut_partial;   // 12 comma-separated ints for _NET_WM_STRUT_PARTIAL

    bool operator==(const PanelLayout&) const = default;
};

// configured_height is Config::panel_height; it is multiplied by the monitor
// scale factor and then padded, matching upstream (+8 bottom, +4 top).
PanelLayout compute_panel_layout(const Rect& monitor, int scale_factor,
                                 int configured_height, PanelPosition position);

// Same, but with the panel's real on-screen height in pixels. GTK enforces a
// minimum height from the widget content (button padding + min-height in the
// stylesheet), so the window is routinely taller than config's panel_height. The
// strut MUST match the drawn height or the window manager reserves the wrong
// amount and other windows overlap the panel or leave a gap beside it.
PanelLayout layout_for_height(const Rect& monitor, int actual_height,
                             PanelPosition position);

// Thin wrappers over the X11 utilities. Each returns false if the helper is
// missing or exits non-zero.
namespace dock {

// Locates the panel's X window. Tries `xdotool search --pid` filtered by title
// via xwininfo, then falls back to scanning `wmctrl -l`, as upstream did.
std::optional<std::string> find_window_id(std::string_view title);

bool set_dock_window_type(std::string_view window_id);
bool set_strut_partial(std::string_view window_id, const std::string& strut);
bool move_resize(std::string_view window_id, const Rect& bounds);
bool set_sticky_above(std::string_view window_id);

// Applies the whole sequence in the order the window manager expects: type,
// then geometry, then struts, then stacking hints.
bool apply(std::string_view window_id, const PanelLayout& layout);

}  // namespace dock

}  // namespace auspex
