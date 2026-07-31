// The system tray, as protocol rather than as plumbing.
//
// Everything here is pure: no D-Bus, no GTK, no display. The connection work lives
// in gtk/tray.cpp, and it is deliberately thin -- the decisions that are easy to get
// subtly wrong (which icon a status implies, how a pixmap's bytes are ordered, what
// a registered item's name actually is) are here where they can be tested against
// captured values from real applications.
//
// WHICH PROTOCOL: StatusNotifierItem over D-Bus, not the older XEmbed tray.
//
// XEmbed requires owning an X selection and REPARENTING other applications' windows
// into the panel, which means an Xlib connection, an error handler and an event
// loop -- the one thing this project has consistently refused to take on, and the
// reason every CLI tool here links zero X libraries. StatusNotifierItem is a D-Bus
// protocol, so it needs none of that, and it is what applications on this desktop
// actually use: Steam, blueman, Discord, Telegram, Nextcloud and anything built on
// libayatana-appindicator all speak it.
//
// The honest cost is that an application which only ever supported XEmbed will not
// appear. That is a shrinking set, and the alternative is an X11-only tray that
// Auspex would have to throw away the moment it owns a Wayland compositor.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace auspex {

// Where an item lives on the bus.
//
// The watcher hands out registered items as a single string, and there are two
// spellings in the wild: a bare bus name, and a bus name with the object path
// glued straight onto it -- ":1.77/org/blueman/sni". Both are seen on one desktop,
// so both are parsed.
struct TrayItemAddress {
    std::string service;   // ":1.77" or "org.example.App"
    std::string path;      // "/org/blueman/sni", defaulted when absent

    bool operator==(const TrayItemAddress&) const = default;
};

// Splits a registered item into service and object path.
//
// The split is at the FIRST '/', because a bus name can never contain one and an
// object path must begin with one. An empty or malformed value yields an empty
// service, which the caller must treat as "not an item" rather than as an item at
// the default path -- registering the whole desktop's default path by accident is
// how a tray ends up talking to the wrong process.
TrayItemAddress parse_tray_item_address(std::string_view registered);

// The item's own view of how interesting it is.
enum class TrayStatus { Passive, Active, NeedsAttention };

TrayStatus parse_tray_status(std::string_view status);

// Which icon name to draw.
//
// NeedsAttention is the only status that changes the picture, and only if the
// application actually supplied an attention icon -- several set the status and
// leave the icon empty, and honouring that literally would blank the icon at the
// exact moment it is trying to get your attention.
std::string tray_icon_name(TrayStatus status, std::string_view icon_name,
                           std::string_view attention_icon_name);

// Whether an item is shown at all.
//
// Everything is shown, including Passive. The specification says Passive items
// "can be hidden", and hiding them is defensible reading -- but applications set
// Passive far more loosely than the spec imagines (as "idle", not as "hide me"),
// and the result of believing them is an icon that silently disappears. A tray that
// hides icons is a tray people report as broken. Status still selects the icon.
bool tray_item_visible(TrayStatus status);

// Tooltip text.
//
// The SNI ToolTip is a struct of (icon name, icon pixmap, title, description) and
// applications fill it in inconsistently: blueman puts its message in description
// and leaves title empty. Falling back through title, description and finally the
// item's own Title/Id is what stops a tooltip reading as blank.
//
// Description may contain Qt-flavoured markup, which is not Pango markup and would
// be drawn literally; it is stripped rather than rendered.
std::string tray_tooltip(std::string_view title, std::string_view description,
                         std::string_view fallback);

// Removes markup tags from a tooltip description, keeping the text between them.
std::string strip_tray_markup(std::string_view text);

// --- icon pixmaps -----------------------------------------------------------

// SNI ships pixmaps as ARGB32 in NETWORK byte order; GdkPixbuf wants RGBA in memory
// order. On a little-endian machine that is not a swap of two channels but a full
// reordering, and getting it wrong produces an icon that is recognisably the right
// shape in entirely the wrong colours -- which reads as a theming bug rather than a
// byte-order one, so it is worth a test rather than an eyeball.
//
// Returns empty if the data is not exactly width*height*4 bytes, rather than
// reading past the end of a buffer another process supplied.
std::vector<std::uint8_t> tray_argb_to_rgba(const std::uint8_t* data, std::size_t length,
                                            int width, int height);

// The largest pixmap in a set, by area. Applications offer several sizes and the
// panel scales down, so the biggest is the one that survives scaling best.
//
// Returns -1 when there is nothing to choose from.
struct TrayPixmapSize {
    int width  = 0;
    int height = 0;
};
int best_tray_pixmap(const std::vector<TrayPixmapSize>& sizes);

// --- XApp status icons -------------------------------------------------------
//
// A SECOND tray protocol, and on Linux Mint not an optional one.
//
// Mint's own applications -- mintupdate, mintreport, blueberry, the process monitor
// -- do not use StatusNotifierItem. They use org.x.StatusIcon, which Mint wrote for
// XApp and which xfce4-panel displays through a separate plugin. A tray that speaks
// only SNI is missing exactly the icons a Mint user most expects to see, which is
// how this was found: two icons showed where the system panel had four.
//
// It is a much smaller protocol than SNI. There is no menu interface at all: the
// panel reports the click and its position, and the application puts up its own
// menu wherever it likes.
bool is_xapp_status_service(std::string_view bus_name);

// XApp lets an application name a themed icon OR give an absolute path to a file,
// and the two need different loaders. A leading '/' is the whole distinction.
bool tray_icon_is_path(std::string_view icon);

// GTK's position type for the edge a panel is docked to, which XApp's ButtonPress
// takes so the application knows which way to open its menu. Values are GtkPositionType:
// LEFT 0, RIGHT 1, TOP 2, BOTTOM 3.
int xapp_panel_position(bool panel_at_top);

// --- menus ------------------------------------------------------------------

// One entry of a com.canonical.dbusmenu layout.
//
// The tray is not much use without this. Both items on this desktop -- Steam and
// blueman -- expose their entire interface through a menu and do nothing at all on
// a left click, so a tray without menus would draw two icons that ignore you.
struct TrayMenuNode {
    std::int32_t id = 0;
    std::string  label;
    bool         enabled   = true;
    bool         visible   = true;
    bool         separator = false;
    // Checkmarks and radio buttons. "toggle-type" plus "toggle-state" in the
    // protocol; -1 there means indeterminate, which is drawn as unchecked.
    bool         checkable = false;
    bool         checked   = false;
    std::string  icon_name;

    std::vector<TrayMenuNode> children;

    bool operator==(const TrayMenuNode&) const = default;
};

// Underscores are mnemonics in a dbusmenu label, and GTK would take them as its own
// mnemonics only if the label were set as one -- so they are stripped instead of
// being drawn literally as "_Quit". A doubled underscore is a literal one.
std::string strip_menu_mnemonics(std::string_view label);

// Whether a node is worth putting in a menu at all: invisible entries, and
// separators that would land at an edge or next to another separator, are dropped.
// Applications emit those freely because most menu widgets tidy them up.
std::vector<TrayMenuNode> tidy_tray_menu(std::vector<TrayMenuNode> nodes);

}  // namespace auspex
