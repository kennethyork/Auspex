#include "auspex/tray.hpp"

#include <algorithm>
#include <cctype>

namespace auspex {

namespace {

std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

}  // namespace

TrayItemAddress parse_tray_item_address(std::string_view registered) {
    const std::string value = trim_copy(registered);
    if (value.empty()) return {};

    const auto slash = value.find('/');
    if (slash == std::string::npos) {
        // A bare bus name. The specification's default object path is the one every
        // implementation falls back to.
        return TrayItemAddress{.service = value, .path = "/StatusNotifierItem"};
    }

    // A leading slash means we were handed a path with no service. That is not an
    // item we can talk to, and guessing a service would mean calling into whichever
    // process happens to answer.
    if (slash == 0) return {};

    return TrayItemAddress{.service = value.substr(0, slash), .path = value.substr(slash)};
}

TrayStatus parse_tray_status(std::string_view status) {
    if (status == "NeedsAttention") return TrayStatus::NeedsAttention;
    if (status == "Passive")        return TrayStatus::Passive;
    // Active, and anything unrecognised. An unknown status must not hide an icon.
    return TrayStatus::Active;
}

std::string tray_icon_name(TrayStatus status, std::string_view icon_name,
                           std::string_view attention_icon_name) {
    if (status == TrayStatus::NeedsAttention && !attention_icon_name.empty()) {
        return std::string(attention_icon_name);
    }
    return std::string(icon_name);
}

bool tray_item_visible(TrayStatus) { return true; }

std::string strip_tray_markup(std::string_view text) {
    std::string out;
    out.reserve(text.size());

    bool inside_tag = false;
    for (const char c : text) {
        if (c == '<') {
            inside_tag = true;
            continue;
        }
        if (c == '>') {
            inside_tag = false;
            continue;
        }
        if (!inside_tag) out.push_back(c);
    }
    return trim_copy(out);
}

std::string tray_tooltip(std::string_view title, std::string_view description,
                         std::string_view fallback) {
    const std::string head = trim_copy(title);
    const std::string body = strip_tray_markup(description);

    if (!head.empty() && !body.empty()) return head + "\n" + body;
    if (!head.empty()) return head;
    if (!body.empty()) return body;
    return trim_copy(fallback);
}

std::vector<std::uint8_t> tray_argb_to_rgba(const std::uint8_t* data, std::size_t length,
                                            int width, int height) {
    if (data == nullptr || width <= 0 || height <= 0) return {};

    const std::size_t pixels = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height);
    // Exactly, not at least. A short buffer would be read past the end, and a long
    // one means the dimensions and the data disagree -- in which case trusting the
    // dimensions would slice the image at the wrong stride and draw noise.
    if (length != pixels * 4) return {};

    std::vector<std::uint8_t> rgba(length);
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t at = i * 4;
        // Source is ARGB in network (big-endian) order, so the bytes arrive in the
        // order A, R, G, B regardless of the machine's endianness. Destination is
        // R, G, B, A in memory order, which is what GdkPixbuf reads.
        const std::uint8_t a = data[at + 0];
        const std::uint8_t r = data[at + 1];
        const std::uint8_t g = data[at + 2];
        const std::uint8_t b = data[at + 3];

        rgba[at + 0] = r;
        rgba[at + 1] = g;
        rgba[at + 2] = b;
        rgba[at + 3] = a;
    }
    return rgba;
}

int best_tray_pixmap(const std::vector<TrayPixmapSize>& sizes) {
    int best  = -1;
    long area = -1;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const auto& size = sizes[i];
        if (size.width <= 0 || size.height <= 0) continue;
        const long candidate = static_cast<long>(size.width) *
                               static_cast<long>(size.height);
        if (candidate > area) {
            area = candidate;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool is_xapp_status_service(std::string_view bus_name) {
    // The prefix, and something after it. "org.x.StatusIcon" on its own is the
    // interface name, not a service offering an icon.
    constexpr std::string_view prefix = "org.x.StatusIcon.";
    return bus_name.size() > prefix.size() && bus_name.substr(0, prefix.size()) == prefix;
}

bool tray_icon_is_path(std::string_view icon) {
    return !icon.empty() && icon.front() == '/';
}

int xapp_panel_position(bool panel_at_top) {
    // GtkPositionType: TOP is 2, BOTTOM is 3. The application opens its menu away
    // from the edge the panel is on, so telling it the wrong one puts the menu off
    // the screen.
    return panel_at_top ? 2 : 3;
}

std::string strip_menu_mnemonics(std::string_view label) {
    std::string out;
    out.reserve(label.size());

    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] != '_') {
            out.push_back(label[i]);
            continue;
        }
        // A doubled underscore is an escaped literal one.
        if (i + 1 < label.size() && label[i + 1] == '_') {
            out.push_back('_');
            ++i;
            continue;
        }
        // A single underscore marks the next character as the mnemonic; the marker
        // is dropped and the character kept.
    }
    return out;
}

std::vector<TrayMenuNode> tidy_tray_menu(std::vector<TrayMenuNode> nodes) {
    std::vector<TrayMenuNode> kept;
    kept.reserve(nodes.size());

    for (auto& node : nodes) {
        if (!node.visible) continue;

        if (node.separator) {
            // Never first, and never doubled. Applications emit separators around
            // groups without knowing which groups are empty, so a menu that hides
            // one section arrives with two rules in a row and a rule at the top.
            if (kept.empty() || kept.back().separator) continue;
            kept.push_back(std::move(node));
            continue;
        }

        node.children = tidy_tray_menu(std::move(node.children));
        kept.push_back(std::move(node));
    }

    // And never last.
    while (!kept.empty() && kept.back().separator) kept.pop_back();
    return kept;
}

}  // namespace auspex
