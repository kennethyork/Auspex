// MAGI theme palettes and CSS generation.
//
// Port of src/magi_shell/core/theme.py. Deliberately GTK-free: this header
// produces a CSS *string* and knows nothing about Gtk::CssProvider. The provider
// glue lives in the shell target. That split means the palettes and the
// stylesheet can be built and diffed against the Python original without the
// GTK4 dev packages installed, and it keeps auspex-core linkable by CLI tools.
//
// The three palettes are carried over verbatim -- they are the shell's identity,
// not incidental values.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace auspex {

struct Palette {
    std::string_view name;
    std::string_view panel_bg;
    std::string_view panel_fg;
    std::string_view button_bg;
    std::string_view button_hover;
    std::string_view button_active;
    std::string_view launcher_bg;
    std::string_view accent;
    std::string_view entry_bg;
    std::string_view entry_fg;
    std::string_view entry_border;
    std::string_view entry_focus;
    std::string_view selection_bg;
    std::string_view selection_fg;
    std::string_view link;
    std::string_view error;
    std::string_view subtitle_fg;
};

// In the same order as MAGI_THEMES in theme.py; "Plain" is the fallback.
const std::array<Palette, 3>& themes();

// Case-sensitive lookup by display name, matching the config value. Falls back
// to "Plain" for an unknown name, as the Python did.
const Palette& theme_by_name(std::string_view name);

// The full stylesheet for a palette. Selectors are a faithful port of
// _generate_css(), including the libadwaita-specific ones (preferencespage,
// preferencesgroup, actionrow, .navigationview) that have no C++ bindings and so
// can only be reached through CSS.
std::string generate_css(const Palette& palette);

}  // namespace auspex
