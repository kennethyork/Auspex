#include "auspex/display.hpp"

#include <cstdlib>

namespace auspex {

namespace {

// No display server. Every query is empty and every command fails, which is
// exactly what the callers already handle: before this class existed, a missing
// wmctrl produced the same empty vectors and false returns, so nothing downstream
// needs a new code path.
class NullDisplay final : public DisplayServer {
public:
    std::string_view name() const override { return "none"; }
    bool available() const override { return false; }

    std::vector<MonitorInfo> monitors() override { return {}; }

    std::vector<Workspace> workspaces() override { return {}; }
    bool set_workspace(int) override { return false; }
    std::optional<Rect> workarea() override { return std::nullopt; }

    std::vector<WindowEntry> windows() override { return {}; }
    std::vector<PlacedWindow> windows_with_geometry() override { return {}; }
    bool activate_window(std::string_view) override { return false; }
    std::optional<Rect> window_geometry(std::string_view) override { return std::nullopt; }
    bool move_window(std::string_view, int, int) override { return false; }
    bool resize_window(std::string_view, int, int) override { return false; }
    bool maximize_window(std::string_view) override { return false; }
    bool unmaximize_window(std::string_view) override { return false; }
    bool window_is_maximized(std::string_view) override { return false; }
    bool minimize_window(std::string_view) override { return false; }
    bool restore_window(std::string_view) override { return false; }
    bool close_window(std::string_view) override { return false; }
    std::optional<FrameExtents> frame_extents(std::string_view) override {
        return std::nullopt;
    }
    std::vector<std::string> visible_windows() override { return {}; }
    std::optional<std::string> focused_window_id() override { return std::nullopt; }
    std::optional<std::string> focused_window_title() override { return std::nullopt; }

    std::optional<std::string> find_own_window(std::string_view) override {
        return std::nullopt;
    }
    bool dock_panel(std::string_view, const PanelLayout&) override { return false; }
    bool place_desktop_window(std::string_view, const Rect&) override { return false; }
    std::optional<std::string> current_wallpaper() override { return std::nullopt; }

    std::optional<Point> pointer_position() override { return std::nullopt; }
    bool type_text(std::string_view) override { return false; }
    std::optional<std::string> primary_selection() override { return std::nullopt; }
    bool can_read_selection() override { return false; }
};

bool env_set(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

// The installed backend. A function-local static so construction is thread-safe
// without a mutex, and so there is no static-initialisation-order dependency on
// whatever else the process sets up first.
std::unique_ptr<DisplayServer>& slot() {
    static std::unique_ptr<DisplayServer> server;
    return server;
}

}  // namespace

std::unique_ptr<DisplayServer> make_null_display() {
    return std::make_unique<NullDisplay>();
}

std::unique_ptr<DisplayServer> detect_display_server() {
    // DISPLAY wins whenever it is set, even alongside WAYLAND_DISPLAY. That pair
    // means XWayland, where the X11 helpers genuinely work -- wmctrl and xdotool
    // drive XWayland clients, and while they cannot see native Wayland windows,
    // that is strictly more than a null backend can do.
    if (env_set("DISPLAY")) return make_x11_display();

    // WAYLAND_DISPLAY with no DISPLAY is a pure Wayland session. There is no
    // Wayland backend yet, so this is the honest answer rather than an X11 backend
    // whose every helper would fail with "cannot open display".
    return make_null_display();
}

DisplayServer& display() {
    auto& server = slot();
    if (!server) server = detect_display_server();
    return *server;
}

void set_display_server(std::unique_ptr<DisplayServer> server) {
    // Cached answers belong to the old backend; keeping them would let a test read
    // monitors from a backend it has just replaced.
    invalidate_desktop_caches();
    slot() = std::move(server);
}

}  // namespace auspex
