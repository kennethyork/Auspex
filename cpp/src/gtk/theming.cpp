#include "auspex/gtk/theming.hpp"

#include <fstream>
#include <iostream>

#include <glibmm/main.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/styleprovider.h>

#include <nlohmann/json.hpp>

#include "auspex/config.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace auspex::gtk {

ThemeManager::ThemeManager(const Glib::RefPtr<Gdk::Display>& display,
                           fs::path config_path)
    : provider_(Gtk::CssProvider::create()),
      config_path_(config_path.empty() ? Config::default_path() : std::move(config_path)) {
    // One provider per display, installed once. Its contents are swapped on
    // reload rather than stacking a new provider each time.
    Gtk::StyleProvider::add_provider_for_display(display, provider_,
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    std::error_code ec;
    last_mtime_ = fs::last_write_time(config_path_, ec);

    theme_name_ = read_theme_name();
    apply(theme_by_name(theme_name_));
}

std::string ThemeManager::read_theme_name() const {
    std::ifstream in(config_path_);
    if (!in) return "Plain";

    const json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return "Plain";

    // theme.py read this key; it is intentionally separate from the rest of
    // Config so a theme switch does not require the shell to reparse everything.
    return parsed.value("auspex_theme", std::string{"Plain"});
}

void ThemeManager::apply(const Palette& palette) {
    // Read here as well as in reload(), because the constructor calls apply()
    // directly and would otherwise start at the compiled default.
    window_opacity_ = Config::load(config_path_).window_opacity;
    const std::string css = generate_css(palette, window_opacity_);
    try {
        provider_->load_from_data(css);
    } catch (const Glib::Error& e) {
        // A malformed stylesheet must not take the panel down; the previous
        // provider contents stay in effect.
        std::cerr << "auspex: stylesheet rejected by GTK: " << e.what() << "\n";
    }
}

void ThemeManager::reload() {
    const std::string name = read_theme_name();
    const double opacity = Config::load(config_path_).window_opacity;

    // EITHER can have changed. This used to compare the theme name alone, so
    // editing window_opacity did nothing until the next theme change -- and a
    // setting that takes effect at some unrelated later moment is one you conclude
    // is broken.
    if (name == theme_name_ && opacity == window_opacity_) return;

    theme_name_ = name;
    window_opacity_ = opacity;
    apply(theme_by_name(theme_name_));
}

void ThemeManager::start_watching(unsigned interval_ms) {
    Glib::signal_timeout().connect(
        [this]() {
            std::error_code ec;
            const auto mtime = fs::last_write_time(config_path_, ec);
            // A missing or unreadable config is expected while an editor writes
            // it; stay quiet and try again next tick.
            if (!ec && mtime != last_mtime_) {
                last_mtime_ = mtime;
                reload();
            }
            return true;
        },
        interval_ms);
}

}  // namespace auspex::gtk
