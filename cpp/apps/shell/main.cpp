// auspex-shell — the panel shell.
//
// Replaces `python3 -m magi_shell` (src/magi_shell/__main__.py + core/application.py).
//
// Upstream's start.sh wrapped the shell in a crash-restart loop and replaced the
// whole session (marco, xcompmgr, mate-settings-daemon, mate-power-manager). This
// binary does none of that: it runs as an ordinary application inside your existing
// Xfce session, so xfwm4 keeps compositing and a crash costs you a panel rather
// than a desktop.
#include <iostream>
#include <memory>

#include <adwaita.h>
#include <gtkmm/application.h>

#include "auspex/canvas.hpp"
#include "auspex/config.hpp"
#include "auspex/desktop.hpp"
#include "auspex/gtk/desktop_window.hpp"
#include "auspex/gtk/panel.hpp"
#include "auspex/gtk/theming.hpp"
#include "auspex/gtk/voice.hpp"
#include "auspex/panel_dock.hpp"

int main(int argc, char** argv) {
    // Panels are dock windows that reserve screen edges via _NET_WM_STRUT_PARTIAL.
    // None of that exists under Wayland, so fail loudly rather than drawing a
    // floating window that silently does not dock.
    if (const char* session = std::getenv("XDG_SESSION_TYPE");
        session && std::string(session) != "x11") {
        std::cerr << "auspex-shell: requires an X11 session (found " << session << ")\n";
        return 1;
    }

    auto app = Gtk::Application::create("one.auspex.Shell");

    app->signal_startup().connect(
        [] {
            // libadwaita has no C++ bindings packaged on Mint 22.2, so it is
            // initialised through its C API. Called from startup, after GTK is up.
            adw_init();
        },
        /*after=*/true);

    // Held for the process lifetime: the config is passed by reference to the
    // panels and the voice worker.
    const auspex::Config config = auspex::Config::load();

    std::unique_ptr<auspex::Canvas>                canvas;
    std::unique_ptr<auspex::gtk::ThemeManager>     theme;
    std::unique_ptr<auspex::gtk::VoiceController>  voice;
    std::unique_ptr<auspex::gtk::DesktopWindow>    desktop;
    std::unique_ptr<auspex::gtk::Panel>            top;
    std::unique_ptr<auspex::gtk::Panel>            bottom;

    app->signal_activate().connect([&] {
        // activate can fire more than once (a second launch of a non-unique app
        // re-activates the primary instance); building twice would stack panels.
        if (top) {
            top->present();
            return;
        }

        theme = std::make_unique<auspex::gtk::ThemeManager>(Gdk::Display::get_default());
        theme->start_watching();
        std::cout << "auspex-shell: theme '" << theme->current_theme_name() << "'\n";

        voice = std::make_unique<auspex::gtk::VoiceController>(config);

        const auto monitor = auspex::primary_monitor();
        const auspex::Rect bounds =
            monitor ? monitor->bounds : auspex::Rect{0, 0, 1920, 1080};

        // The infinite canvas over real windows. The viewport is the primary
        // monitor; the panels and desktop substrate are never adopted, so the
        // wallpaper and both bars stay put no matter how far it pans.
        canvas = std::make_unique<auspex::Canvas>();
        canvas->set_viewport({.x = 0, .y = 0, .width = bounds.width,
                              .height = bounds.height});
        voice->set_canvas(canvas.get(), bounds);

        desktop =
            std::make_unique<auspex::gtk::DesktopWindow>(config, *canvas, bounds);
        desktop->set_monitor_changed_handler(
            [&](const auspex::Rect& changed) {
                voice->set_canvas(canvas.get(), changed);
            });

        app->add_window(*desktop);
        desktop->present();

        top = std::make_unique<auspex::gtk::Panel>(config, auspex::PanelPosition::Top, *voice);
        bottom =
            std::make_unique<auspex::gtk::Panel>(config, auspex::PanelPosition::Bottom, *voice);

        top->set_window_restore_handler(
            [&](const std::string& id) { desktop->restore_managed_window(id); });
        top->set_window_full_handler(
            [&](const std::string& id) { desktop->full_managed_window(id); });

        const auto panel_geometry = [&](auspex::PanelPosition position, int height) {
            desktop->set_panel_height(position, height);
        };
        top->set_geometry_handler(panel_geometry);
        bottom->set_geometry_handler(panel_geometry);
        bottom->set_pan_handler(
            [&](int x_direction, int y_direction) {
                desktop->pan_step(x_direction, y_direction);
            });

        // The canvas controls are all on the bottom bar now: four arrows and, in the
        // middle of them, 1:1. A factor of 0 means "back to the grid at life size",
        // and it drives the same canvas the desktop surface does, so the panel and
        // the wallpaper can never disagree about where things are.
        bottom->set_zoom_handler([&](double factor) {
            if (factor <= 0.0) desktop->reset_zoom();
            else               desktop->zoom_by(factor);
        });

        app->add_window(*top);
        app->add_window(*bottom);
        top->present();
        bottom->present();
    });

    const int status = app->run(argc, argv);

    // Tear the panels down before the voice worker: their callbacks reference it.
    top.reset();
    bottom.reset();
    voice.reset();
    // The desktop window borrows the canvas, so it goes first.
    desktop.reset();
    canvas.reset();
    theme.reset();
    return status;
}
