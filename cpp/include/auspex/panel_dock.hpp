// Panel geometry: where a panel sits and how much screen it reserves.
//
// Port of the window-management half of src/magi_shell/core/panel.py. Kept free of
// GTK so the strut arithmetic is unit-testable and auspex-core stays linkable
// without the GTK4 dev packages. The caller supplies the monitor rectangle it got
// from GDK; nothing here talks to a display server.
//
// This file is pure arithmetic. Actually applying a PanelLayout to a window is
// DisplayServer::dock_panel() in display.hpp -- under X11 that means
// _NET_WM_WINDOW_TYPE_DOCK plus _NET_WM_STRUT_PARTIAL, which is what makes the
// panel desktop-agnostic across xfwm4, marco and the rest; under Wayland it will
// mean wlr-layer-shell. The strut_partial string below is X11's encoding of the
// reserved space and is computed here because it is 12 integers of arithmetic that
// deserve a test, not because this layer knows about X11.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace auspex {

struct Point {
    int x = 0;
    int y = 0;

    bool operator==(const Point&) const = default;
};

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

// Keeps the panel at the same edge and size but publishes a zero strut. Infinite
// canvas mode uses this so the bars float above the plane instead of cutting
// permanent top and bottom boundaries into it.
PanelLayout overlay_panel_layout(PanelLayout layout);

}  // namespace auspex
