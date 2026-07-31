// The display-server seam.
//
// Every operation Auspex performs on windows, workspaces, outputs, keyboard input
// and the clipboard goes through DisplayServer. There is one real implementation
// today -- X11, driving wmctrl/xdotool/xprop/xwininfo/xclip as subprocesses, which
// is what the whole shell did inline before this interface existed. The interface
// is here so a Wayland backend can be added without touching a single caller.
//
// WHY AN INTERFACE AND NOT #ifdef: X11 and Wayland can both be live in the same
// session -- under XWayland the X11 helpers keep working while the compositor is
// Wayland -- so which one to use is a runtime question, not a compile-time one. One
// build has to be able to run on either.
//
// WHY THE FREE FUNCTIONS IN desktop.hpp STAYED: they are the vocabulary the panel,
// the voice controller and the command layer are written in, and they are covered
// by the selftests. They now forward here instead of forking helpers themselves, so
// the seam went in underneath the callers rather than through them.
//
// WHAT BELONGS HERE: anything that needs to ask the display server a question or
// tell it to do something. What does NOT belong here is policy -- where a window
// should go, how much of the screen a panel should reserve, whether a switch
// actually took effect. That arithmetic is server-independent, it is unit-tested
// without a display, and it stays in desktop.cpp / panel_dock.cpp / canvas.cpp.
//
// THREADING: display() is safe to call from any thread. The X11 backend holds no
// mutable state -- every call forks a helper -- and the accessor's storage is a
// function-local static, so initialisation is race-free. set_display_server() is
// NOT safe once threads exist: call it from main() before starting the voice
// worker, or leave it alone and let detection run.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "auspex/desktop.hpp"
#include "auspex/panel_dock.hpp"

namespace auspex {

class DisplayServer {
public:
    virtual ~DisplayServer() = default;

    // "x11", "wayland", "none". Reported by auspex-probe and used in log lines, so
    // a bug report says which backend was live.
    virtual std::string_view name() const = 0;

    // False when there is no usable display server at all. Callers do not need to
    // check it -- every operation below already fails softly -- but the shell can
    // refuse to start a panel it could never dock.
    virtual bool available() const = 0;

    // --- outputs ---------------------------------------------------------------

    virtual std::vector<MonitorInfo> monitors() = 0;

    // --- workspaces ------------------------------------------------------------

    virtual std::vector<Workspace> workspaces() = 0;

    // Requests a switch. Returns whether the request was accepted, NOT whether it
    // took effect -- EWMH makes this asynchronous, and the confirming poll lives in
    // switch_workspace() where it can be expressed against workspaces().
    virtual bool set_workspace(int index) = 0;

    // The usable screen rectangle after every panel's reserved space.
    virtual std::optional<Rect> workarea() = 0;

    // --- windows ---------------------------------------------------------------

    virtual std::vector<WindowEntry> windows() = 0;

    // Every window and its geometry in one round trip. The canvas reconciles once a
    // second and needs the position of everything it manages each time; asking per
    // window would be one fork each, which is why this exists alongside windows().
    virtual std::vector<PlacedWindow> windows_with_geometry() = 0;
    virtual bool activate_window(std::string_view id) = 0;

    // Outer geometry, including decorations, in screen coordinates.
    virtual std::optional<Rect> window_geometry(std::string_view id) = 0;

    virtual bool move_window(std::string_view id, int x, int y) = 0;
    virtual bool resize_window(std::string_view id, int width, int height) = 0;

    // A maximised window ignores position changes, so the canvas has to clear the
    // maximised state before it can place anything.
    virtual bool maximize_window(std::string_view id) = 0;
    virtual bool unmaximize_window(std::string_view id) = 0;
    virtual bool window_is_maximized(std::string_view id) = 0;

    // Iconify, and un-iconify. Separate from activate_window() because restoring is
    // activation but minimising is emphatically not: there is no "unactivate".
    virtual bool minimize_window(std::string_view id) = 0;
    virtual bool restore_window(std::string_view id) = 0;

    // Asks a window to close, the same way clicking its titlebar X does: the
    // application is told, and may prompt about unsaved work or refuse. This is
    // NOT killing the process -- a shell that terminated applications from a task
    // list would lose people's work, so the polite request is the only one offered.
    virtual bool close_window(std::string_view id) = 0;

    // Title bar and border thickness, so a caller sizing a window to fit a space
    // can account for what the window manager draws around it.
    virtual std::optional<FrameExtents> frame_extents(std::string_view id) = 0;

    // Ids of the windows that are actually on screen right now, in one call.
    //
    // This is how minimised windows are found: they stay in windows() but drop out
    // of here. The canvas needs to know, because moving an iconified window makes
    // the window manager restore it -- so without this, minimising anything the
    // canvas manages is undone within the second.
    virtual std::vector<std::string> visible_windows() = 0;

    virtual std::optional<std::string> focused_window_title() = 0;

    // Id of the focused window, for deciding whether a click on a panel button
    // means "come here" or "go away".
    virtual std::optional<std::string> focused_window_id() = 0;

    // --- the shell's own surfaces ----------------------------------------------

    // Finds a top-level belonging to THIS process by title. Used by the panel to
    // discover its own window id once GTK has realised it.
    virtual std::optional<std::string> find_own_window(std::string_view title) = 0;

    // Makes a window a panel: dock type, geometry, reserved space, stacking. One
    // call rather than four setters because the ordering between them is load
    // bearing and is the backend's business, not the caller's.
    virtual bool dock_panel(std::string_view id, const PanelLayout& layout) = 0;

    // Makes a window the canvas substrate: above the environment's existing
    // desktop input surface, below application windows, covering `bounds`, present
    // on every workspace and absent from the taskbar.
    //
    // Same reasoning as dock_panel: the type hint must be set before the window
    // manager decides the stacking, so the sequence belongs to the backend. It is
    // NOT dock_panel with different numbers -- the substrate reserves no space and
    // must never cover an application.
    virtual bool place_desktop_window(std::string_view id, const Rect& bounds) = 0;

    // The wallpaper image the session is already using, if it can be discovered.
    //
    // Auspex draws the desktop itself rather than leaving it to a wallpaper tool,
    // so it needs a picture. Asking the environment what it is already showing
    // means the canvas inherits the user's wallpaper on first run instead of
    // demanding they set the same path twice. Where that answer lives is entirely
    // environment-specific -- xfconf here, gsettings elsewhere, a compositor's own
    // state under Wayland -- which is exactly what puts it behind this seam.
    //
    // nullopt when nothing is set or the environment cannot be asked.
    virtual std::optional<std::string> current_wallpaper() = 0;

    // --- input and clipboard ---------------------------------------------------

    // Synthesises typing into whatever holds focus. This is how dictation delivers
    // its result; there is no "which window" argument because there is no way to
    // type into an unfocused window on either display server.
    virtual bool type_text(std::string_view text) = 0;

    // The primary selection -- the text the user has highlighted, not the clipboard
    // they explicitly copied. nullopt when nothing is selected.
    virtual std::optional<std::string> primary_selection() = 0;

    // Whether the selection can be read at all, which is NOT the same question as
    // whether anything is selected.
    //
    // The X11 backend reads the selection by forking a helper, and that helper is a
    // separate package almost nobody has by default. Without this, an absent helper
    // and an empty selection are the same nullopt -- so the shell tells you nothing
    // is selected while your text sits there highlighted, which is a lie, and the
    // context poller forks a doomed process several times a second forever.
    virtual bool can_read_selection() = 0;
};

// argv that prints the primary selection using `tool`, or empty for a tool this
// backend does not know how to drive. Separate from the class so the mapping can be
// tested without a display: the two helpers disagree about their flags, and getting
// one wrong fails silently as "nothing is selected".
std::vector<std::string> selection_command(std::string_view tool);

// The process-wide backend. Detects on first use if nothing was installed.
DisplayServer& display();

// Installs a backend, replacing whatever was there. Call before any thread starts.
// Passing nullptr restores detection on the next display() call, which is what the
// selftests use to put the real backend back.
void set_display_server(std::unique_ptr<DisplayServer> server);

// Chooses a backend from the environment. Exposed so a caller can see what would
// be picked -- and so the selftests can assert the choice -- without installing it.
//
// DISPLAY wins whenever it is set, including when WAYLAND_DISPLAY is also set:
// that combination means XWayland, where the X11 helpers do work and are the only
// thing that does until a Wayland backend exists.
std::unique_ptr<DisplayServer> detect_display_server();

// The X11 backend. Everything it does, it does by forking a helper from PATH.
std::unique_ptr<DisplayServer> make_x11_display();

// A backend for having no display server: every query is empty, every command
// fails, nothing crashes. What runs under `ssh` with no X forwarding, and what the
// selftests install so they never touch the developer's live session.
std::unique_ptr<DisplayServer> make_null_display();

}  // namespace auspex
