// The X11 DisplayServer, implemented by forking command-line helpers.
//
// This is where every wmctrl / xdotool / xprop / xwininfo / xclip invocation in the
// project now lives. They were previously spread across desktop.cpp, panel_dock.cpp,
// canvas.cpp, gtk/panel.cpp, gtk/voice.cpp and commands.cpp; collecting them behind
// one class is what makes a second backend possible.
//
// WHY HELPERS AND NOT XLIB: nothing here needs a display connection of its own. The
// shell is a GTK application that already has one, and opening a second connection
// to duplicate what wmctrl does would add an event loop, an error handler and a
// reconnect path for no behavioural gain. The helpers also mean the CLI tools link
// zero X libraries, which is the property README's "GTK is optional" rests on.
//
// The cost is one fork per operation, which matters only for the panel's half-second
// context poll -- two forks a second, measured at well under a millisecond each.
//
// WHEN THIS BECOMES XLIB: Stage 2, when Auspex owns the window manager. A WM must
// hold a display connection anyway to take SubstructureRedirect, and at that point
// these operations become direct calls on state the WM already has, with no forks
// and no round-trips. The interface does not change; only this file does.
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "auspex/display.hpp"
#include "auspex/process.hpp"

namespace auspex {

namespace {

class X11Display final : public DisplayServer {
public:
    std::string_view name() const override { return "x11"; }
    bool available() const override { return true; }

    // --- outputs ---------------------------------------------------------------

    std::vector<MonitorInfo> monitors() override {
        // `-q --current` rather than `--listmonitors`: measured at 5ms against
        // 334ms on a 4-output NVIDIA box, because --current does not re-probe every
        // output's mode list. This is the most expensive call in the project and it
        // runs on a timer, so the difference is most of the shell's CPU use.
        if (const auto fast = run({"xrandr", "-q", "--current"}); fast.ok) {
            if (auto monitors = parse_monitors_current(fast.out); !monitors.empty()) {
                return monitors;
            }
        }

        // Fallback. Kept because --current is an xrandr option like any other and
        // an older or differently-built xrandr may not have it, in which case an
        // empty parse above is indistinguishable from "no monitors" -- and a shell
        // that believes it has no monitors cannot place a panel.
        const auto result = run({"xrandr", "--listmonitors"});
        if (!result.ok) return {};
        return parse_monitors(result.out);
    }

    // --- workspaces ------------------------------------------------------------

    std::vector<Workspace> workspaces() override {
        const auto result = run({"wmctrl", "-d"});
        if (!result.ok) return {};
        return parse_workspaces(result.out);
    }

    bool set_workspace(int index) override {
        const std::string target = std::to_string(index);

        // wmctrl is authoritative; xdotool is kept as a second attempt because some
        // window managers only honour one of the two. Either accepting is enough --
        // whether the switch actually happened is checked by the caller.
        const bool via_wmctrl  = run({"wmctrl", "-s", target}, false).ok;
        const bool via_xdotool = run({"xdotool", "set_desktop", target}, false).ok;
        return via_wmctrl || via_xdotool;
    }

    std::optional<Rect> workarea() override {
        const auto result = run({"xprop", "-root", "_NET_WORKAREA"});
        if (!result.ok) return std::nullopt;
        return parse_workarea(result.out);
    }

    // --- windows ---------------------------------------------------------------

    std::vector<WindowEntry> windows() override {
        const auto result = run({"wmctrl", "-l"});
        if (!result.ok) return {};
        return parse_windows(result.out);
    }

    std::vector<PlacedWindow> windows_with_geometry() override {
        const auto result = run({"wmctrl", "-lG"});
        if (!result.ok) return {};
        return parse_placed_windows(result.out);
    }

    bool activate_window(std::string_view id) override {
        if (id.empty()) return false;
        return run({"wmctrl", "-ia", std::string(id)}, false).ok;
    }

    std::optional<Rect> window_geometry(std::string_view id) override {
        if (id.empty()) return std::nullopt;
        const auto info = run({"xwininfo", "-id", std::string(id)});
        if (!info.ok) return std::nullopt;
        return parse_window_geometry(info.out);
    }

    bool move_window(std::string_view id, int x, int y) override {
        if (id.empty()) return false;
        // wmctrl's EWMH geometry addresses the decorated FRAME on xfwm4. xdotool
        // windowmove addresses a gravity-adjusted client coordinate instead; the
        // old caller then subtracted frame extents a second time, hiding the title
        // bar under the top panel and leaving wallpaper above the bottom panel.
        // -1 leaves the current client size unchanged.
        const std::string geometry =
            "0," + std::to_string(x) + "," + std::to_string(y) + ",-1,-1";
        return run({"wmctrl", "-i", "-r", std::string(id), "-e", geometry},
                   false)
            .ok;
    }

    bool resize_window(std::string_view id, int width, int height) override {
        if (id.empty()) return false;
        return run({"xdotool", "windowsize", std::string(id), std::to_string(width),
                    std::to_string(height)},
                   false)
            .ok;
    }

    bool maximize_window(std::string_view id) override {
        if (id.empty()) return false;
        return run({"wmctrl", "-i", "-r", std::string(id), "-b",
                    "add,maximized_vert,maximized_horz"},
                   false)
            .ok;
    }

    bool unmaximize_window(std::string_view id) override {
        if (id.empty()) return false;
        return run({"wmctrl", "-i", "-r", std::string(id), "-b",
                    "remove,maximized_vert,maximized_horz"},
                   false)
            .ok;
    }

    bool window_is_maximized(std::string_view id) override {
        if (id.empty()) return false;
        const auto result = run({"xprop", "-id", std::string(id), "_NET_WM_STATE"});
        if (!result.ok) return false;
        return result.out.find("_NET_WM_STATE_MAXIMIZED_HORZ") != std::string::npos &&
               result.out.find("_NET_WM_STATE_MAXIMIZED_VERT") != std::string::npos;
    }

    bool minimize_window(std::string_view id) override {
        if (id.empty()) return false;
        // XIconifyWindow, which is what "minimise" means in EWMH terms. There is no
        // wmctrl equivalent: -b add,hidden is refused, because _NET_WM_STATE_HIDDEN
        // is a state the window manager reports rather than one clients may set.
        return run({"xdotool", "windowminimize", std::string(id)}, false).ok;
    }

    bool restore_window(std::string_view id) override {
        if (id.empty()) return false;
        // Activation de-iconifies as a side effect and also raises and focuses,
        // which is exactly what clicking a minimised task button should do.
        return run({"wmctrl", "-i", "-a", std::string(id)}, false).ok;
    }

    bool close_window(std::string_view id) override {
        if (id.empty()) return false;
        // wmctrl -c sends WM_DELETE_WINDOW, which is a request the client handles.
        return run({"wmctrl", "-i", "-c", std::string(id)}, false).ok;
    }

    std::optional<FrameExtents> frame_extents(std::string_view id) override {
        if (id.empty()) return std::nullopt;
        const auto result = run({"xprop", "-id", std::string(id), "_NET_FRAME_EXTENTS"});
        if (!result.ok) return std::nullopt;
        return parse_frame_extents(result.out);
    }

    std::vector<std::string> visible_windows() override {
        // One call for the whole set. `--onlyvisible` is the part that matters: an
        // iconified window is still a window and still in wmctrl's list, but it is
        // not viewable, so it drops out here.
        //
        // xdotool answers in decimal and wmctrl in zero-padded hex, so the ids are
        // canonicalised before they can be compared to anything.
        const auto result = run({"xdotool", "search", "--onlyvisible", "--name", ".*"});
        if (!result.ok) return {};

        std::vector<std::string> ids;
        for (const auto& line : split_lines(result.out)) {
            const std::string id = canonical_window_id(trim(line));
            if (!id.empty()) ids.push_back(id);
        }
        return ids;
    }

    std::optional<std::string> focused_window_id() override {
        const auto active = run({"xdotool", "getactivewindow"});
        if (!active.ok) return std::nullopt;
        const std::string id = canonical_window_id(trim(active.out));
        if (id.empty()) return std::nullopt;
        return id;
    }

    std::optional<std::string> focused_window_title() override {
        const auto active = run({"xdotool", "getactivewindow", "getwindowname"});
        if (!active.ok) return std::nullopt;
        const std::string title = trim(active.out);
        if (title.empty()) return std::nullopt;
        return title;
    }

    // --- the shell's own surfaces ----------------------------------------------

    std::optional<std::string> find_own_window(std::string_view title) override {
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
        for (const auto& window : windows()) {
            if (window.title.find(wanted) != std::string::npos) return window.id;
        }

        return std::nullopt;
    }

    bool dock_panel(std::string_view id, const PanelLayout& layout) override {
        if (id.empty()) return false;
        const std::string wid(id);

        // Order matters: the type hint must be set before the window manager
        // computes the frame, and struts are only honoured once the window has its
        // final geometry.
        bool ok = run({"xprop", "-id", wid, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set",
                       "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK"},
                      false)
                      .ok;
        ok = move_window(wid, layout.bounds.x, layout.bounds.y) && ok;
        ok = resize_window(wid, layout.bounds.width, layout.bounds.height) && ok;
        ok = run({"xprop", "-id", wid, "-f", "_NET_WM_STRUT_PARTIAL", "32c", "-set",
                  "_NET_WM_STRUT_PARTIAL", layout.strut_partial},
                 false)
                 .ok &&
             ok;
        ok = run({"wmctrl", "-i", "-r", wid, "-b", "add,sticky,above"}, false).ok && ok;
        return ok;
    }

    bool place_desktop_window(std::string_view id, const Rect& bounds) override {
        if (id.empty()) return false;
        const std::string wid(id);

        // Xfce already owns a full-screen DESKTOP window. When Auspex uses the same
        // layer, xfdesktop sits above it and consumes every empty-space click. A
        // NORMAL window with the BELOW state occupies the layer above xfdesktop
        // and below every application, which is exactly the canvas input layer.
        bool ok = run({"xprop", "-id", wid, "-f", "_NET_WM_WINDOW_TYPE", "32a", "-set",
                       "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL"},
                      false)
                      .ok;
        ok = move_window(wid, bounds.x, bounds.y) && ok;
        ok = resize_window(wid, bounds.width, bounds.height) && ok;

        // sticky: the canvas is not per-workspace, it is what workspaces become.
        // below: applications stay above it, while NORMAL keeps it above Xfce's
        // desktop so the pointer can actually reach the canvas.
        // skip_taskbar/skip_pager: the desktop is not something you alt-tab to.
        //
        // No _NET_WM_STRUT_PARTIAL, deliberately. A desktop that reserved space
        // would shrink the work area to nothing and leave every other window with
        // nowhere to go.
        // wmctrl accepts at most two properties in one -b request. Supplying all
        // four silently applied STICKY and discarded BELOW, allowing this normal
        // window to rise over applications. Keep the two pairs explicit.
        ok = run({"wmctrl", "-i", "-r", wid, "-b", "add,sticky,below"}, false).ok && ok;
        ok = run({"wmctrl", "-i", "-r", wid, "-b",
                  "add,skip_taskbar,skip_pager"},
                 false).ok && ok;
        return ok;
    }

    std::optional<std::string> current_wallpaper() override {
        // Xfce first, since that is what Mint's Xfce edition and Auspex's own
        // session both run. `xfconf-query -l` lists the backdrop properties for
        // however many monitors exist; last-image is the one the settings dialog
        // writes. Any monitor's answer will do -- the canvas draws one picture.
        const auto properties = run({"xfconf-query", "-c", "xfce4-desktop", "-l"});
        if (properties.ok) {
            for (const auto& line : split_lines(properties.out)) {
                const std::string property = trim(line);
                if (!property.ends_with("/last-image")) continue;
                const auto value =
                    run({"xfconf-query", "-c", "xfce4-desktop", "-p", property});
                if (const std::string path = trim(value.out);
                    value.ok && !path.empty() && std::filesystem::exists(path)) {
                    return path;
                }
            }
        }

        // GNOME/Cinnamon/MATE all keep it in gsettings, under different schemas.
        // The value is a URI, so file:// has to come off before it is a path.
        for (const auto& [schema, key] :
             {std::pair{"org.gnome.desktop.background", "picture-uri"},
              std::pair{"org.cinnamon.desktop.background", "picture-uri"},
              std::pair{"org.mate.background", "picture-filename"}}) {
            const auto value = run({"gsettings", "get", schema, key});
            if (!value.ok) continue;

            std::string path = trim(value.out);
            // gsettings quotes its answer.
            if (path.size() >= 2 && path.front() == '\'' && path.back() == '\'') {
                path = path.substr(1, path.size() - 2);
            }
            if (path.starts_with("file://")) path = path.substr(7);
            if (!path.empty() && std::filesystem::exists(path)) return path;
        }

        return std::nullopt;
    }

    // --- input and clipboard ---------------------------------------------------

    bool type_text(std::string_view text) override {
        if (text.empty()) return true;
        // --clearmodifiers stops a still-held panel click modifier from mangling the
        // synthesised keystrokes. The delay is what upstream used; typing with no
        // delay drops characters in some toolkits.
        return run({"xdotool", "type", "--clearmodifiers", "--delay", "12",
                    std::string(text)},
                   false)
            .ok;
    }

    std::optional<std::string> primary_selection() override {
        // A helper rather than GTK's clipboard API: reading the X primary selection
        // from the GTK thread needs the window to have focus, and the panel is a
        // dock window that never holds it.
        const auto argv = selection_command(selection_tool());
        if (argv.empty()) return std::nullopt;
        const auto selection = run(argv);
        if (!selection.ok) return std::nullopt;
        const std::string text = trim(selection.out);
        if (text.empty()) return std::nullopt;
        return text;
    }

    bool can_read_selection() override { return !selection_tool().empty(); }

private:
    // Resolved once. PATH does not change under a running shell, and this is asked
    // on a timer -- a which-style lookup several times a second to reach the same
    // answer is exactly the sort of cost the panel cannot afford.
    //
    // xclip first only because it is the more common install; xsel is equally good
    // and is what several distributions ship in a default desktop, which is why
    // hardcoding xclip left this silently dead on machines that had neither.
    static const std::string& selection_tool() {
        static const std::string tool = first_in_path({"xclip", "xsel"});
        return tool;
    }
};

}  // namespace

std::vector<std::string> selection_command(std::string_view tool) {
    // The primary selection specifically, not the clipboard: this is the text under
    // the user's highlight, which is what "speak this" and "ask about this" mean.
    // Both helpers default to a DIFFERENT selection than we want, so neither flag is
    // optional -- xclip defaults to the clipboard, xsel to XA_PRIMARY only by
    // accident of history and not on every build.
    if (tool == "xclip") return {"xclip", "-o", "-selection", "primary"};
    if (tool == "xsel")  return {"xsel", "--primary", "--output"};
    return {};
}

std::unique_ptr<DisplayServer> make_x11_display() {
    return std::make_unique<X11Display>();
}

}  // namespace auspex
