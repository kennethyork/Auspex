#include "auspex/desktop.hpp"

#include <optional>

#include <algorithm>
#include <set>

#include <charconv>
#include <cstdio>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <sstream>

#include "auspex/display.hpp"
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

// A value plus how recently it was fetched. Deliberately tiny and local: the point
// is to stop two widgets on the same one-second timer asking the same expensive
// question twice, not to build a caching layer.
template <typename T>
struct TimedCache {
    std::mutex                            lock;
    T                                     value{};
    std::chrono::steady_clock::time_point fetched{};
    bool                                  valid = false;

    template <typename Fetch>
    T get(std::chrono::milliseconds ttl, Fetch&& fetch) {
        const std::scoped_lock guard(lock);
        const auto now = std::chrono::steady_clock::now();
        if (valid && now - fetched < ttl) return value;
        value   = fetch();
        fetched = now;
        valid   = true;
        return value;
    }

    void invalidate() {
        const std::scoped_lock guard(lock);
        valid = false;
    }
};

TimedCache<std::vector<MonitorInfo>>& monitor_cache() {
    static TimedCache<std::vector<MonitorInfo>> cache;
    return cache;
}

TimedCache<std::vector<std::string>>& visible_cache() {
    static TimedCache<std::vector<std::string>> cache;
    return cache;
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

std::vector<PlacedWindow> parse_placed_windows(const std::string& output) {
    std::vector<PlacedWindow> result;

    // `wmctrl -lG` is `wmctrl -l` with four geometry columns spliced in between the
    // workspace and the hostname:
    //
    //   0x02a0000c  0 10 90 950 472 hostname OllamaDev ADE — Linux-Mint-AI
    //
    // The title is everything after the hostname and may contain spaces, so it is
    // taken with getline rather than by tokenising.
    for (const auto& line : split_lines(output)) {
        std::istringstream fields(line);
        std::string id;
        std::string workspace_token;
        if (!(fields >> id >> workspace_token)) continue;

        const auto workspace = to_int(workspace_token);
        if (!workspace) continue;

        std::string x, y, width, height;
        if (!(fields >> x >> y >> width >> height)) continue;

        const auto px = to_int(x);
        const auto py = to_int(y);
        const auto pw = to_int(width);
        const auto ph = to_int(height);
        if (!px || !py || !pw || !ph) continue;

        std::string host;
        if (!(fields >> host)) continue;

        std::string rest;
        std::getline(fields, rest);
        rest = trim(std::move(rest));

        PlacedWindow placed;
        placed.window.id        = id;
        placed.window.workspace = *workspace;
        placed.window.title     = rest.empty() ? host : rest;
        placed.bounds = Rect{.x = *px, .y = *py, .width = *pw, .height = *ph};

        result.push_back(std::move(placed));
    }

    return result;
}

std::optional<double> window_opacity(std::string_view window_id) {
    const auto result = run({"xprop", "-id", std::string(window_id),
                             "_NET_WM_WINDOW_OPACITY"},
                            /*capture=*/true);
    if (!result.ok) return std::nullopt;

    // "_NET_WM_WINDOW_OPACITY(CARDINAL) = 3221225471", or "... not found."
    const auto equals = result.out.find('=');
    if (equals == std::string::npos) return std::nullopt;

    try {
        const auto raw = std::stoull(trim(result.out.substr(equals + 1)));
        // The property is a fraction of 0xFFFFFFFF, which is how a CARDINAL
        // carries a number between zero and one.
        return static_cast<double>(raw) / 4294967295.0;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool set_window_opacity(std::string_view window_id, double opacity) {
    const std::string id = canonical_window_id(window_id);
    if (id.empty()) return false;

    // 1.0 REMOVES the property rather than writing "fully opaque". A window we
    // stop managing should go back to whatever it was, not be pinned opaque by us
    // for the rest of its life -- including after Auspex has exited.
    if (opacity >= 1.0) {
        return run({"xprop", "-id", id, "-remove", "_NET_WM_WINDOW_OPACITY"},
                   /*capture=*/false)
            .ok;
    }

    const double clamped = std::clamp(opacity, 0.1, 1.0);
    // A CARDINAL scaled to 32 bits, which is what the spec says and what every
    // compositor reads. 0.1 is the floor because a window at 0 is one you cannot
    // find in order to undo this.
    const auto value = static_cast<unsigned long>(clamped * 0xFFFFFFFFul);

    return run({"xprop", "-id", id, "-f", "_NET_WM_WINDOW_OPACITY", "32c", "-set",
                "_NET_WM_WINDOW_OPACITY", std::to_string(value)},
               /*capture=*/false)
        .ok;
}

int apply_screen_opacity(const Rect& screen, double opacity,
                         const std::vector<PlacedWindow>& windows,
                         std::set<std::string>& dimmed) {
    int changed = 0;

    std::set<std::string> should_be_dim;
    for (const auto& placed : windows) {
        // The CENTRE decides which monitor a window is on. Overlap would dim a
        // window because one corner of it crossed the boundary.
        const int cx = placed.bounds.x + placed.bounds.width / 2;
        const int cy = placed.bounds.y + placed.bounds.height / 2;
        if (cx < screen.x || cx >= screen.x + screen.width) continue;
        if (cy < screen.y || cy >= screen.y + screen.height) continue;

        should_be_dim.insert(canonical_window_id(placed.window.id));
    }

    // Restore first. A window dragged to another monitor should be opaque there
    // BEFORE anything else is touched, because that is the change somebody is
    // watching for -- and a window that has closed is simply forgotten.
    for (auto it = dimmed.begin(); it != dimmed.end();) {
        if (should_be_dim.count(*it) != 0) { ++it; continue; }
        set_window_opacity(*it, 1.0);
        it = dimmed.erase(it);
        ++changed;
    }

    // Then dim what is newly here. Only what has CHANGED is touched, so this can
    // run on a timer without spawning an xprop per window per tick.
    for (const auto& id : should_be_dim) {
        if (dimmed.count(id) != 0) continue;
        if (set_window_opacity(id, opacity)) {
            dimmed.insert(id);
            ++changed;
        }
    }
    return changed;
}

std::vector<PlacedWindow> list_placed_windows() {
    auto placed = display().windows_with_geometry();

    // Converted to FRAME geometry, which is the coordinate space every other part of
    // this project speaks. move_window writes frame coordinates, the canvas lays out
    // in frame coordinates, and the layout compares the two -- so a read-back in any
    // other space is not a different convention, it is a permanent error.
    //
    // wmctrl -lG reports a position that is neither the frame nor the client. It
    // asks X for the client's offset INSIDE its frame and then translates that same
    // offset to root coordinates, so the decoration is counted twice:
    //
    //     reported = frame + 2 x (left, top)
    //
    // Measured across seven windows -- decorated, undecorated and maximised. An
    // undecorated window has zero extents and is untouched, which is why this went
    // unnoticed for so long: the panels and the desktop substrate were always right.
    //
    // The consequence was not cosmetic. A titlebar 29px tall made every window read
    // back 58px below where it had just been put, the layout took that for a drag by
    // the user, wrote it into the canvas, and every window walked steadily down the
    // screen -- until it no longer fitted and the canvas parked it off the edge.
    //
    // The size is converted too: wmctrl gives the client size, and what the layout
    // reasons about is the space a window occupies including its decoration.
    for (auto& entry : placed) {
        const auto frame = frame_extents(entry.window.id);
        if (!frame) continue;
        entry.bounds.x      -= 2 * frame->left;
        entry.bounds.y      -= 2 * frame->top;
        entry.bounds.width  += frame->left + frame->right;
        entry.bounds.height += frame->top  + frame->bottom;
    }

    return placed;
}

std::string canonical_window_id(std::string_view id) {
    const std::string text = trim(std::string(id));
    if (text.empty()) return {};

    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    const std::string digits = hex ? text.substr(2) : text;
    if (digits.empty()) return {};

    // Reject rather than let strtoul stop at the first bad character: a partly
    // parsed id is a different window, which is far worse than no id at all.
    for (const char c : digits) {
        const bool ok = hex ? std::isxdigit(static_cast<unsigned char>(c)) != 0
                            : std::isdigit(static_cast<unsigned char>(c)) != 0;
        if (!ok) return {};
    }

    unsigned long value = 0;
    try {
        value = std::stoul(digits, nullptr, hex ? 16 : 10);
    } catch (const std::exception&) {
        return {};
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", value);
    return buffer;
}

FrameExtents parse_frame_extents(const std::string& output) {
    // _NET_FRAME_EXTENTS(CARDINAL) = 5, 5, 29, 5
    const std::size_t eq = output.find('=');
    if (eq == std::string::npos) return {};

    std::vector<int> values;
    std::string token;
    for (std::size_t i = eq + 1; i <= output.size(); ++i) {
        const char c = i < output.size() ? output[i] : ',';
        if (c == ',' || c == '\n') {
            if (const auto value = to_int(trim(token))) values.push_back(*value);
            token.clear();
            if (c == '\n') break;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            token.push_back(c);
        }
    }
    if (values.size() < 4) return {};
    return FrameExtents{values[0], values[1], values[2], values[3]};
}

std::optional<FrameExtents> frame_extents(std::string_view window_id) {
    // Cached per window for ten seconds. apply_positions() needs these on every
    // move, which happens on every frame of a drag; an xprop per window per frame
    // would cost more than the panning does. Decoration changes only when a window
    // manager is reconfigured or a window toggles its own decorations, so a stale
    // answer for a few seconds is invisible.
    static std::mutex lock;
    static std::map<std::string, std::pair<FrameExtents,
                                           std::chrono::steady_clock::time_point>> cache;

    const std::string id(window_id);
    if (id.empty()) return std::nullopt;

    {
        const std::scoped_lock guard(lock);
        const auto it = cache.find(id);
        if (it != cache.end() &&
            std::chrono::steady_clock::now() - it->second.second <
                std::chrono::seconds(10)) {
            return it->second.first;
        }
    }

    const auto fresh = display().frame_extents(window_id);
    if (!fresh) return std::nullopt;

    const std::scoped_lock guard(lock);
    cache[id] = {*fresh, std::chrono::steady_clock::now()};
    return fresh;
}

std::vector<std::string> list_visible_windows() {
    // 400ms: shorter than the one-second poll, so the panel and the canvas each see
    // fresh data on their own tick, but the two of them landing in the same tick
    // share one 23ms call instead of making two.
    return visible_cache().get(std::chrono::milliseconds(400),
                               [] { return display().visible_windows(); });
}

bool minimize_window(std::string_view window_id) {
    return display().minimize_window(window_id);
}

bool restore_window(std::string_view window_id) {
    return display().restore_window(window_id);
}

bool maximize_window(std::string_view window_id) {
    return display().maximize_window(window_id);
}

bool close_window(std::string_view window_id) {
    return display().close_window(window_id);
}

std::optional<std::string> focused_window_id() { return display().focused_window_id(); }

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

// `xprop -root _NET_WORKAREA` output:
//   _NET_WORKAREA(CARDINAL) = 0, 32, 1920, 1016, 0, 32, 1920, 1016, ...
// One quadruple per workspace; they are almost always identical, and the current
// workspace's area is what matters, so the first is taken.
std::optional<Rect> parse_workarea(const std::string& output) {
    const auto eq = output.find('=');
    if (eq == std::string::npos) return std::nullopt;

    std::istringstream values(output.substr(eq + 1));
    std::vector<int> numbers;
    std::string token;
    while (std::getline(values, token, ',') && numbers.size() < 4) {
        const auto parsed = to_int(trim(token));
        if (!parsed) return std::nullopt;
        numbers.push_back(*parsed);
    }
    if (numbers.size() < 4 || numbers[2] <= 0 || numbers[3] <= 0) return std::nullopt;

    return Rect{.x = numbers[0], .y = numbers[1], .width = numbers[2], .height = numbers[3]};
}

// `xwininfo -id 0x...` output, the four lines that matter among ~20:
//   Absolute upper-left X:  100
//   Absolute upper-left Y:  132
//   Width: 640
//   Height: 480
// from_chars is deliberately not checked for trailing input: several xwininfo
// values carry a suffix, and only the leading integer is wanted.
std::optional<Rect> parse_window_geometry(const std::string& output) {
    Rect rect;
    bool have_x = false, have_y = false, have_w = false, have_h = false;

    for (const auto& line : split_lines(output)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key   = trim(line.substr(0, colon));
        const std::string value = trim(line.substr(colon + 1));

        int parsed = 0;
        const auto* begin = value.data();
        const auto* end   = value.data() + value.size();
        if (std::from_chars(begin, end, parsed).ec != std::errc{}) continue;

        if (key == "Absolute upper-left X")      { rect.x = parsed;      have_x = true; }
        else if (key == "Absolute upper-left Y") { rect.y = parsed;      have_y = true; }
        else if (key == "Width")                 { rect.width = parsed;  have_w = true; }
        else if (key == "Height")                { rect.height = parsed; have_h = true; }
    }

    if (!(have_x && have_y && have_w && have_h)) return std::nullopt;
    return rect;
}

void invalidate_desktop_caches() {
    monitor_cache().invalidate();
    visible_cache().invalidate();
}

std::vector<MonitorInfo> parse_monitors_current(const std::string& output) {
    std::vector<MonitorInfo> result;
    int index = 0;

    for (const auto& line : split_lines(output)) {
        std::istringstream fields(line);
        std::string connector, state;
        if (!(fields >> connector >> state)) continue;
        if (state != "connected") continue;   // also skips the "Screen 0:" header

        // "primary" is optional and sits between the state and the geometry.
        std::string token;
        if (!(fields >> token)) continue;

        MonitorInfo monitor;
        monitor.primary = (token == "primary");
        if (monitor.primary && !(fields >> token)) continue;

        // token is now WxH+X+Y. A connected-but-unmapped output has no geometry
        // here at all and lands on "(normal" instead, which parses to nothing.
        int width = 0, height = 0, x = 0, y = 0;
        if (std::sscanf(token.c_str(), "%dx%d+%d+%d", &width, &height, &x, &y) != 4) {
            continue;
        }
        if (width <= 0 || height <= 0) continue;

        monitor.index     = index++;
        monitor.connector = connector;
        monitor.bounds    = Rect{.x = x, .y = y, .width = width, .height = height};
        result.push_back(std::move(monitor));
    }

    return result;
}

std::vector<MonitorInfo> list_monitors() {
    // Five seconds. Hot-plugging a monitor is the only thing that changes this, and
    // waiting up to five seconds for the panel to notice is not something anyone
    // will perceive -- whereas 333ms twice a second very much is.
    return monitor_cache().get(std::chrono::seconds(5),
                               [] { return display().monitors(); });
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

std::optional<Rect> screen_bounds(const std::vector<MonitorInfo>& monitors) {
    bool first = true;
    int  left = 0, top = 0, right = 0, bottom = 0;

    for (const auto& monitor : monitors) {
        const Rect& bounds = monitor.bounds;
        // A zero-sized output is a disconnected one xrandr still lists. Including it
        // would drag the union to the origin and make everything left of the primary
        // monitor look like screen.
        if (bounds.width <= 0 || bounds.height <= 0) continue;

        if (first) {
            left = bounds.x;
            top  = bounds.y;
            right  = bounds.x + bounds.width;
            bottom = bounds.y + bounds.height;
            first = false;
            continue;
        }
        left   = std::min(left, bounds.x);
        top    = std::min(top, bounds.y);
        right  = std::max(right, bounds.x + bounds.width);
        bottom = std::max(bottom, bounds.y + bounds.height);
    }

    if (first) return std::nullopt;
    return Rect{.x = left, .y = top, .width = right - left, .height = bottom - top};
}

std::optional<Rect> screen_bounds() { return screen_bounds(list_monitors()); }

std::vector<Workspace> list_workspaces() {
    return display().workspaces();
}

std::optional<int> current_workspace() {
    for (const auto& workspace : list_workspaces()) {
        if (workspace.active) return workspace.index;
    }
    return std::nullopt;
}

bool switch_workspace(int index) {
    if (index < 0) return false;
    if (!display().set_workspace(index)) return false;

    // The switch is asynchronous: the request is accepted as soon as the window
    // manager has it, and the active workspace updates a beat later. Measured on
    // xfwm4, a read immediately after still reports the old desktop and only agrees
    // ~1s later, so a single check reported failure on a switch that had actually
    // worked. Poll instead of sleeping a fixed amount, so the common fast case
    // stays fast.
    //
    // This loop stays on this side of the seam: it is a policy about confirming a
    // request, expressed entirely in terms of current_workspace(), so a Wayland
    // backend inherits it rather than reimplementing it.
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (const auto now = current_workspace(); now && *now == index) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::optional<Rect> current_workarea() {
    return display().workarea();
}

std::vector<WindowEntry> list_windows() {
    return display().windows();
}

bool is_shell_window(const WindowEntry& window) {
    // The SURFACES, not everything Auspex owns.
    //
    // This used to match any title containing "Auspex", which swept up the
    // settings, the calendar, the board and the crew along with the panels -- so
    // Auspex's own windows were the only ones on the desktop the grid would not
    // arrange and the canvas would not manage. They are ordinary windows and should
    // behave like ordinary windows.
    //
    // What genuinely must never be adopted is short and specific: the two panels,
    // and the desktop substrate. Adopting the substrate would be the canvas trying
    // to lay out the canvas.
    const auto starts_with = [&window](std::string_view prefix) {
        return window.title.rfind(prefix, 0) == 0;
    };

    return starts_with("Auspex Panel") ||
           starts_with("Auspex Desktop") ||
           // A stale pre-rename panel left running from an older build.
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
    return display().activate_window(window_id);
}

std::optional<std::string> focused_window_title() {
    return display().focused_window_title();
}

std::optional<std::string> selected_text() {
    return display().primary_selection();
}

bool can_read_selection() {
    return display().can_read_selection();
}

std::optional<Point> parse_pointer_position(const std::string& shell_output) {
    std::optional<int> x;
    std::optional<int> y;

    for (const auto& line : split_lines(shell_output)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key   = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key == "X") x = to_int(value);
        if (key == "Y") y = to_int(value);
    }

    // Both, or neither. A half-parsed position would put a menu somewhere arbitrary
    // rather than admitting it does not know.
    if (!x || !y) return std::nullopt;
    return Point{.x = *x, .y = *y};
}

std::optional<Point> pointer_position() {
    return display().pointer_position();
}

bool type_text(std::string_view text) {
    return display().type_text(text);
}

}  // namespace auspex
