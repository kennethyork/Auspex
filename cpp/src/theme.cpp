#include "auspex/theme.hpp"

#include <unordered_map>

namespace auspex {

namespace {

// clang-format off
constexpr Palette kPlain{
    .name = "Plain",
    .panel_bg = "#ffffff", .panel_fg = "#000000",
    .button_bg = "#f0f0f0", .button_hover = "#e0e0e0", .button_active = "#d0d0d0",
    .launcher_bg = "#2c71cc", .accent = "#2c71cc",
    .entry_bg = "#ffffff", .entry_fg = "#000000",
    .entry_border = "#cccccc", .entry_focus = "#2c71cc",
    .selection_bg = "#2c71cc", .selection_fg = "#ffffff",
    .link = "#0066cc", .error = "#cc0000", .subtitle_fg = "#666666",
};

constexpr Palette kTokyoNight{
    .name = "Tokyo Night",
    .panel_bg = "#1a1b26", .panel_fg = "#c0caf5",
    .button_bg = "#24283b", .button_hover = "#414868", .button_active = "#565f89",
    .launcher_bg = "#bb9af7", .accent = "#7aa2f7",
    .entry_bg = "#1f2335", .entry_fg = "#c0caf5",
    .entry_border = "#414868", .entry_focus = "#7aa2f7",
    .selection_bg = "#7aa2f7", .selection_fg = "#1a1b26",
    .link = "#73daca", .error = "#f7768e", .subtitle_fg = "#a9b1d6",
};

constexpr Palette kForest{
    .name = "Forest",
    .panel_bg = "#2b3328", .panel_fg = "#e4dfd2",
    .button_bg = "#3a4637", .button_hover = "#4f6146", .button_active = "#546c4d",
    .launcher_bg = "#a7c080", .accent = "#83c092",
    .entry_bg = "#323d2f", .entry_fg = "#e4dfd2",
    .entry_border = "#4f6146", .entry_focus = "#a7c080",
    .selection_bg = "#a7c080", .selection_fg = "#2b3328",
    .link = "#83c092", .error = "#e67e80", .subtitle_fg = "#d3c6aa",
};
// clang-format on

const std::array<Palette, 3> kThemes{kPlain, kTokyoNight, kForest};

// Placeholders are spelled $name rather than {name}: CSS is full of braces, and
// keeping them literal means this template stays byte-comparable with the CSS in
// theme.py instead of needing every brace doubled.
constexpr std::string_view kTemplate = R"CSS(
/* Override libadwaita default background colors */
.background {
    background-color: $panel_bg;
    color: $panel_fg;
}

window,
.window {
    background-color: $panel_bg;
    color: $panel_fg;
}

/* Panels are translucent, so the canvas and the wallpaper show through them.
 *
 * Scoped to .auspex-panel deliberately: the rules above paint every window,
 * and applying alpha there would also make the settings, chat and launcher
 * windows see-through -- which is unreadable over a photograph rather than
 * stylish. Only the two bars opt in.
 *
 * The inner `box` selectors are not redundant. The generic `box` rule below
 * paints an opaque background over the whole panel interior, so making only the
 * window translucent would change nothing visible; these override it at higher
 * specificity.
 *
 * Requires a compositor. Without one the alpha is ignored and the panels are
 * simply opaque, which is a fine thing to degrade to.
 *
 * 0.95 is not a taste call: it is xfce4-panel's own default background-alpha of
 * 95, so the bars sit at the same weight as the panel they replace instead of
 * announcing themselves as something new. */
window.auspex-panel,
window.auspex-panel > box,
window.auspex-panel box,
window.auspex-panel .background {
    background-color: alpha($panel_bg, 0.95);
}

box {
    background-color: $panel_bg;
}

.navigationview {
    background-color: $panel_bg;
}

preferencespage > scrolledwindow > viewport > box > clamp > box {
    background-color: $panel_bg;
}

preferencespage > box > box {
    background-color: $panel_bg;
}

preferencespage box.content {
    background-color: $panel_bg;
}

preferencespage > scrolledwindow > viewport {
    background-color: $panel_bg;
}

box.content {
    background-color: $panel_bg;
}

.preferences-page {
    background-color: $panel_bg;
}

row {
    background-color: $button_bg;
    color: $panel_fg;
    border-radius: 6px;
    margin: 2px 0;
}

row:hover {
    background-color: $button_hover;
}

row label {
    color: $panel_fg;
}

preferencesgroup {
    background-color: $button_bg;
    border-radius: 12px;
    padding: 6px;
    margin: 6px;
}

preferencesgroup > box > box {
    background-color: $button_bg;
}

/* Group headers */
preferencesgroup > box > box.header {
    color: $panel_fg;
}

preferencesgroup > box > box.header label {
    color: $panel_fg;
}

preferencesgroup > box > box.header label.subtitle {
    color: $subtitle_fg;
}

actionrow {
    background-color: $entry_bg;
    color: $panel_fg;
    border-radius: 6px;
}

actionrow:hover {
    background-color: $button_hover;
}

actionrow label {
    color: $panel_fg;
}

actionrow .subtitle {
    color: $subtitle_fg;
}

/* Button Styling */
button {
    background-color: $button_bg;
    color: $panel_fg;
    padding: 6px 10px;
    border-radius: 6px;
    border: 1px solid rgba(0, 0, 0, 0.1);
}

button:hover {
    background-color: $button_hover;
}

button:active {
    background-color: $button_active;
}

/* Panel buttons */
box button {
    background-color: $button_bg;
    color: $panel_fg;
    min-height: 24px;
    margin: 2px;
}

box button:hover {
    background-color: $button_hover;
}

box button:active {
    background-color: $button_active;
}

/* Workspace switcher buttons */
box.horizontal > button {
    background-color: $button_bg;
    color: $panel_fg;
}

box.horizontal > button:hover {
    background-color: $button_hover;
}

/* System button styles */
button.flat {
    background-color: $button_bg;
    color: $panel_fg;
}

button.flat:hover {
    background-color: $button_hover;
}

button.image-button {
    background-color: $button_bg;
    color: $panel_fg;
}

button.text-button {
    background-color: $button_bg;
    color: $panel_fg;
}

/* Message buttons */
.message-button {
    background-color: $button_bg;
    color: $panel_fg;
    padding: 4px;
    margin: 2px;
}

/* Active workspace button */
.active-workspace {
    background-color: $accent;
    color: $selection_fg;
}

/* Suggested action button */
button.suggested-action {
    background-color: $accent;
    color: $selection_fg;
}

button.suggested-action:hover {
    opacity: 0.9;
}

/* Launcher button */
.launcher-button {
    background-color: $launcher_bg;
    color: $selection_fg;
    font-weight: bold;
    padding: 0 12px;
}

.launcher-button:hover {
    opacity: 0.9;
}

/* Entry/TextField Styling */
entry {
    background-color: $entry_bg;
    color: $entry_fg;
    border: 1px solid $entry_border;
    border-radius: 6px;
    padding: 8px;
    caret-color: $entry_fg;
}

entry:focus {
    border-color: $entry_focus;
    box-shadow: 0 0 0 2px $entry_focus;
}

/* Header styling */
headerbar {
    background-color: $button_bg;
    color: $panel_fg;
}

headerbar label {
    color: $panel_fg;
}

headerbar title {
    color: $panel_fg;
}

/* Title and text styling */
.title {
    color: $panel_fg;
}

.subtitle {
    color: $subtitle_fg;
}

label {
    color: $panel_fg;
}

/* Navigation sidebar */
.navigation-sidebar {
    background-color: $button_bg;
}

.navigation-sidebar label {
    color: $panel_fg;
}

.navigation-sidebar row:selected {
    background-color: $accent;
    color: $selection_fg;
}

.navigation-sidebar row:hover:not(:selected) {
    background-color: $button_hover;
}

/* Spinbutton styling */
spinbutton {
    background-color: $entry_bg;
    color: $entry_fg;
}

spinbutton text {
    color: $entry_fg;
}

spinbutton button {
    background-color: $button_bg;
    color: $panel_fg;
}

/* Combobox styling */
combobox {
    background-color: $entry_bg;
    color: $entry_fg;
}

combobox button {
    background-color: $entry_bg;
    color: $entry_fg;
}

/* Menu styling */
menu {
    background-color: $button_bg;
    color: $panel_fg;
}

menuitem {
    color: $panel_fg;
}

menuitem:hover {
    background-color: $button_hover;
}

/* Link styling */
link {
    color: $link;
}

link:hover {
    text-decoration: underline;
}

/* Level bar styling */
levelbar block {
    min-height: 10px;
}

levelbar block.filled {
    background-color: $accent;
}

/* Selection styling */
*:selected {
    background-color: $selection_bg;
    color: $selection_fg;
}

/* Scrollbar styling */
scrollbar {
    background: transparent;
}

scrollbar slider {
    background-color: alpha($panel_fg, 0.2);
    border-radius: 9999px;
    min-width: 8px;
    min-height: 8px;
}

scrollbar slider:hover {
    background-color: alpha($panel_fg, 0.4);
}

scrollbar slider:active {
    background-color: alpha($panel_fg, 0.6);
}

/* Message styling for LLM Menu */
.user-message {
    background-color: $button_bg;
    color: $panel_fg;
    border-radius: 6px;
    padding: 8px;
}

.assistant-message {
    background-color: $entry_bg;
    color: $entry_fg;
    border-radius: 6px;
    padding: 8px;
}

.code-block {
    background-color: $button_bg;
    color: $panel_fg;
    font-family: monospace;
    padding: 8px;
    border-radius: 6px;
}

/* Monitor and Clock labels */
.monitor-label {
    color: $panel_fg;
}

.clock-label {
    color: $panel_fg;
}

/* Recording state */
.recording {
    color: $error;
}
)CSS";

}  // namespace

const std::array<Palette, 3>& themes() { return kThemes; }

const Palette& theme_by_name(std::string_view name) {
    for (const auto& palette : kThemes) {
        if (palette.name == name) return palette;
    }
    return kPlain;
}

std::string generate_css(const Palette& palette) {
    const std::unordered_map<std::string_view, std::string_view> vars{
        {"panel_bg", palette.panel_bg},
        {"panel_fg", palette.panel_fg},
        {"button_bg", palette.button_bg},
        {"button_hover", palette.button_hover},
        {"button_active", palette.button_active},
        {"launcher_bg", palette.launcher_bg},
        {"accent", palette.accent},
        {"entry_bg", palette.entry_bg},
        {"entry_fg", palette.entry_fg},
        {"entry_border", palette.entry_border},
        {"entry_focus", palette.entry_focus},
        {"selection_bg", palette.selection_bg},
        {"selection_fg", palette.selection_fg},
        {"link", palette.link},
        {"error", palette.error},
        {"subtitle_fg", palette.subtitle_fg},
    };

    std::string out;
    out.reserve(kTemplate.size() + 1024);

    for (std::size_t i = 0; i < kTemplate.size();) {
        if (kTemplate[i] != '$') {
            out.push_back(kTemplate[i++]);
            continue;
        }

        // Longest identifier wins, so $panel_bg is not matched as $panel.
        const std::size_t start = i + 1;
        std::size_t end = start;
        while (end < kTemplate.size() &&
               (std::isalnum(static_cast<unsigned char>(kTemplate[end])) ||
                kTemplate[end] == '_')) {
            ++end;
        }

        const std::string_view key = kTemplate.substr(start, end - start);
        const auto it = vars.find(key);
        if (it == vars.end()) {
            // Unknown placeholder: emit it literally rather than silently
            // dropping it, so a typo shows up in the stylesheet instead of
            // producing an invisible styling gap.
            out.push_back('$');
            i = start;
            continue;
        }

        out.append(it->second);
        i = end;
    }

    return out;
}

}  // namespace auspex
