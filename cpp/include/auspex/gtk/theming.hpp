// GTK glue for the theme layer.
//
// Port of ThemeManager from src/magi_shell/core/theme.py. The palettes and the
// stylesheet itself live in magi/theme.hpp with no GTK dependency; this is only
// the part that feeds the CSS to a display and reloads it when config.json
// changes.
//
// Differences from the Python:
//   * ThemeManager kept a list of weak refs to every window and re-added a fresh
//     provider to each window's display on every change. Providers are per-display,
//     not per-window, so N windows on one display meant N stacked providers that
//     were never removed. Here one provider is installed per display and its
//     contents are replaced in place.
//   * The Python polled os.path.getmtime every second forever. Same approach kept
//     (it is cheap and needs no inotify plumbing) but a failed stat no longer
//     spams stderr each tick.
#pragma once

#include <filesystem>
#include <string>

#include <gdkmm/display.h>
#include <gtkmm/cssprovider.h>

#include "auspex/theme.hpp"

namespace auspex::gtk {

class ThemeManager {
public:
    // Installs a provider on `display` and applies the theme named in config.json
    // (key "auspex_theme", defaulting to "Plain").
    explicit ThemeManager(const Glib::RefPtr<Gdk::Display>& display,
                          std::filesystem::path config_path = {});

    // Re-reads the config and swaps the stylesheet if the theme name changed.
    void reload();

    // Polls the config file's mtime every `interval_ms` and reloads on change.
    void start_watching(unsigned interval_ms = 1000);

    const std::string& current_theme_name() const { return theme_name_; }

private:
    void apply(const Palette& palette);
    std::string read_theme_name() const;

    Glib::RefPtr<Gtk::CssProvider> provider_;
    std::filesystem::path          config_path_;
    std::string                    theme_name_;
    // Watched alongside the theme name. Without it, reload() returned early
    // whenever the name was unchanged and an opacity edit did nothing at all --
    // the setting only took effect on the next theme change or restart, which is
    // a setting that appears not to work.
    double                         window_opacity_ = -1.0;
    std::filesystem::file_time_type last_mtime_{};
};

}  // namespace auspex::gtk
