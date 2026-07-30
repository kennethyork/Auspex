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

#include "auspex/config.hpp"
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

    std::unique_ptr<auspex::gtk::ThemeManager>    theme;
    std::unique_ptr<auspex::gtk::VoiceController> voice;
    std::unique_ptr<auspex::gtk::Panel>           top;
    std::unique_ptr<auspex::gtk::Panel>           bottom;

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

        top = std::make_unique<auspex::gtk::Panel>(config, auspex::PanelPosition::Top, *voice);
        bottom =
            std::make_unique<auspex::gtk::Panel>(config, auspex::PanelPosition::Bottom, *voice);

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
    theme.reset();
    return status;
}
