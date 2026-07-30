#include "auspex/desktop.hpp"

#include <charconv>
#include <chrono>
#include <thread>
#include <sstream>

#include "auspex/process.hpp"

namespace auspex {

namespace {

std::optional<int> to_int(std::string_view s) {
    int value = 0;
    const auto* begin = s.data();
    const auto* end   = s.data() + s.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

}  // namespace

// `wmctrl -d` lines look like:
//   0  * DG: 1920x1080  VP: 0,0  WA: 0,32 1920x1016  Workspace 1
//   1  - DG: 1920x1080  VP: N/A  WA: 0,32 1920x1016  Workspace 2
// The name is the tail after the WA field, which itself contains spaces, so the
// name is taken as everything after the 9th whitespace-delimited token.
std::vector<Workspace> parse_workspaces(const std::string& output) {
    std::vector<Workspace> result;

    for (const auto& line : split_lines(output)) {
        std::istringstream fields(line);
        std::string index_token;
        std::string flag;
        if (!(fields >> index_token >> flag)) continue;

        const auto index = to_int(index_token);
        if (!index) continue;

        Workspace workspace;
        workspace.index  = *index;
        workspace.active = (flag == "*");

        // Skip DG:/VP:/WA: and their values, then take the remainder as the name.
        std::string token;
        int skipped = 0;
        while (skipped < 7 && (fields >> token)) ++skipped;
        std::string rest;
        std::getline(fields, rest);
        workspace.name = trim(std::move(rest));

        result.push_back(std::move(workspace));
    }

    return result;
}

// `wmctrl -l` lines look like:
//   0x03400007  0 hostname Some Window Title
// Upstream split with maxsplit=3 and read fields [0], [1], [3] -- which silently
// assumes the host column is present. wmctrl only emits it with -x/-p in some
// builds, so parse defensively: id, workspace, then the remainder as the title,
// dropping a leading hostname token only when a further token follows.
std::vector<WindowEntry> parse_windows(const std::string& output) {
    std::vector<WindowEntry> result;

    for (const auto& line : split_lines(output)) {
        std::istringstream fields(line);
        std::string id;
        std::string workspace_token;
        if (!(fields >> id >> workspace_token)) continue;

        const auto workspace = to_int(workspace_token);
        if (!workspace) continue;

        std::string host;
        if (!(fields >> host)) continue;

        std::string rest;
        std::getline(fields, rest);
        rest = trim(std::move(rest));

        WindowEntry window;
        window.id        = id;
        window.workspace = *workspace;
        // When nothing follows the host column, that token was the title.
        window.title     = rest.empty() ? host : rest;

        result.push_back(std::move(window));
    }

    return result;
}

// `xrandr --listmonitors` lines look like:
//    0: +*DP-2 1920/698x1080/393+0+0  DP-2
//       ^^     ^^^^     ^^^^  ^^ ^^
//       |'*' marks primary   width/mm x height/mm +x+y
std::vector<MonitorInfo> parse_monitors(const std::string& output) {
    std::vector<MonitorInfo> result;

    for (const auto& line : split_lines(output)) {
        std::istringstream fields(line);
        std::string index_token;
        std::string flags_and_name;
        std::string geometry;
        if (!(fields >> index_token >> flags_and_name >> geometry)) continue;

        // "0:" -> 0
        if (!index_token.empty() && index_token.back() == ':') index_token.pop_back();
        const auto index = to_int(index_token);
        if (!index) continue;

        MonitorInfo monitor;
        monitor.index = *index;

        std::size_t pos = 0;
        while (pos < flags_and_name.size() &&
               (flags_and_name[pos] == '+' || flags_and_name[pos] == '*')) {
            if (flags_and_name[pos] == '*') monitor.primary = true;
            ++pos;
        }
        monitor.connector = flags_and_name.substr(pos);

        // Strip the physical-size suffixes: 1920/698x1080/393+0+0 -> 1920 1080 0 0
        std::string cleaned;
        cleaned.reserve(geometry.size());
        bool in_physical = false;
        for (const char ch : geometry) {
            if (ch == '/') { in_physical = true; continue; }
            if (in_physical) {
                if (ch == 'x' || ch == '+' || ch == '-') in_physical = false;
                else continue;
            }
            cleaned.push_back(ch == 'x' ? ' ' : (ch == '+' ? ' ' : ch));
        }

        std::istringstream geo(cleaned);
        int w = 0, h = 0, x = 0, y = 0;
        if (!(geo >> w >> h >> x >> y)) continue;
        if (w <= 0 || h <= 0) continue;

        monitor.bounds = Rect{.x = x, .y = y, .width = w, .height = h};
        result.push_back(std::move(monitor));
    }

    return result;
}

std::vector<MonitorInfo> list_monitors() {
    const auto result = run({"xrandr", "--listmonitors"});
    if (!result.ok) return {};
    return parse_monitors(result.out);
}

std::optional<MonitorInfo> primary_monitor() {
    const auto monitors = list_monitors();
    if (monitors.empty()) return std::nullopt;

    for (const auto& monitor : monitors) {
        if (monitor.primary) return monitor;
    }
    // No output flagged primary (possible with some xrandr setups): prefer the one
    // at the origin, since that is where most desktops put the main screen.
    for (const auto& monitor : monitors) {
        if (monitor.bounds.x == 0 && monitor.bounds.y == 0) return monitor;
    }
    return monitors.front();
}

std::vector<Workspace> list_workspaces() {
    const auto result = run({"wmctrl", "-d"});
    if (!result.ok) return {};
    return parse_workspaces(result.out);
}

std::optional<int> current_workspace() {
    for (const auto& workspace : list_workspaces()) {
        if (workspace.active) return workspace.index;
    }
    return std::nullopt;
}

bool switch_workspace(int index) {
    if (index < 0) return false;
    const std::string target = std::to_string(index);

    // wmctrl is authoritative; xdotool is kept as a second attempt because some
    // window managers only honour one of the two.
    const bool via_wmctrl  = run({"wmctrl", "-s", target}, false).ok;
    const bool via_xdotool = run({"xdotool", "set_desktop", target}, false).ok;
    if (!via_wmctrl && !via_xdotool) return false;

    // The switch is asynchronous: wmctrl returns as soon as the WM has the request,
    // and _NET_CURRENT_DESKTOP updates a beat later. Measured on xfwm4, a read
    // immediately after still reports the old desktop and only agrees ~1s later, so
    // a single check reported failure on a switch that had actually worked. Poll
    // instead of sleeping a fixed amount, so the common fast case stays fast.
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (const auto now = current_workspace(); now && *now == index) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::vector<WindowEntry> list_windows() {
    const auto result = run({"wmctrl", "-l"});
    if (!result.ok) return {};
    return parse_windows(result.out);
}

bool is_shell_window(const WindowEntry& window) {
    // Matches the title Panel sets ("Auspex Panel (top)"). "MAGI" is still matched
    // so a stale pre-rename panel left running does not appear in the list.
    return window.title.find("Auspex") != std::string::npos ||
           window.title.find("MAGI") != std::string::npos ||
           window.title == "Desktop" ||
           window.title == "xfdesktop";
}

std::vector<WindowEntry> list_user_windows() {
    std::vector<WindowEntry> result;
    for (auto& window : list_windows()) {
        if (!is_shell_window(window)) result.push_back(std::move(window));
    }
    return result;
}

bool activate_window(std::string_view window_id) {
    if (window_id.empty()) return false;
    return run({"wmctrl", "-ia", std::string(window_id)}, false).ok;
}

}  // namespace auspex
