// auspex-shell — the panel shell.
//
// Replaces `python3 -m magi_shell` (src/magi_shell/__main__.py + core/application.py).
//
// Upstream's start.sh wrapped the shell in a crash-restart loop and replaced the
// whole session (marco, xcompmgr, mate-settings-daemon, mate-power-manager). This
// binary does none of that: it runs as an ordinary application inside your existing
// Xfce session, so xfwm4 keeps compositing and a crash costs you a panel rather
// than a desktop.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <memory>

#include <adwaita.h>
#include <gtkmm/application.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/expander.h>
#include <gtkmm/viewport.h>

#include "auspex/canvas.hpp"
#include "auspex/config.hpp"
#include "auspex/desktop.hpp"
#include "auspex/gtk/desktop_window.hpp"
#include "auspex/gtk/panel.hpp"
#include "auspex/gtk/theming.hpp"
#include "auspex/gtk/voice.hpp"
#include <gtkmm/switch.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/button.h>
#include "auspex/smoke.hpp"
#include <gtkmm/listboxrow.h>
#include <gtkmm/entry.h>
#include <thread>
#include <chrono>
#include "auspex/gtk/windows.hpp"
#include "auspex/panel_dock.hpp"

namespace {

// Every window, by name, for the checks below and for --window.
const std::vector<std::string>& window_names() {
    // ALL of them, not the four I happened to be working on. The check covered
    // crew/brain/projects/team and reported "every window fits" while four other
    // windows had never been measured once -- a green line about a subset reads
    // exactly like a green line about everything.
    static const std::vector<std::string> kNames{"launcher", "calendar", "crew",
                                                 "brain",    "projects", "team",
                                                 "chat",     "settings"};
    return kNames;
}

// Does this window FIT at `width`, or does its content force it wider?
//
// GTK will not shrink a widget below its minimum, so a minimum wider than the
// window is content you cannot reach -- not scrolled off, GONE. That is exactly
// how a row of ten switches lost its last three, and how nine role cells lost
// four, both in one day, both found by eye afterwards.
//
// measure() needs no display interaction and no clicking, which is the point: it
// turns "I would have to look at it" into something a test can assert.
struct Fit {
    std::string name;
    int minimum_width = 0;   // natural width, despite the name
    int target_width  = 0;

    bool fits() const { return minimum_width <= target_width; }
};

// Open every disclosure in the tree.
//
// A COLLAPSED Gtk::Expander does not measure its child, so everything folded away
// contributes nothing to the window's width -- and the check reported a tidy
// 432px for a window whose switch row clipped the moment you opened Tuning. The
// worst case is what a person can actually reach, which is with it open.
void expand_all(Gtk::Widget* widget) {
    if (!widget) return;
    if (auto* expander = dynamic_cast<Gtk::Expander*>(widget)) {
        expander->set_expanded(true);
    }
    for (Gtk::Widget* child = widget->get_first_child(); child;
         child = child->get_next_sibling()) {
        expand_all(child);
    }
}

Fit measure_window(Gtk::Window& window, const std::string& name, int width) {
    expand_all(window.get_child());

    Fit fit;
    fit.name = name;
    fit.target_width = width;

    // PAST the scroller.
    //
    // A ScrolledWindow reports a tiny minimum because it can scroll, so measuring
    // the window's own child hides what the content actually needs. The first
    // version of this check did exactly that and passed the very bug it was
    // written for -- a ten-switch row in a horizontal Box, reported as "fits,
    // needs 432px" while three of the switches were unreachable.
    //
    // Only VERTICAL scrollers are unwrapped, and only horizontally. A window that
    // genuinely scrolls sideways is allowed to be wider than its frame; these do
    // not, by policy, so their content's width is a real constraint.
    Gtk::Widget* content = window.get_child();
    for (int hop = 0; hop < 4 && content; ++hop) {
        if (auto* scroller = dynamic_cast<Gtk::ScrolledWindow*>(content)) {
            if (scroller->property_hscrollbar_policy().get_value() !=
                Gtk::PolicyType::NEVER) {
                break;
            }
            content = scroller->get_child();
            continue;
        }
        // AND the viewport inside it. GTK wraps a non-scrollable child in a
        // Gtk::Viewport, and a viewport reports any width as acceptable -- so
        // stopping at the scroller's child measured the wrapper rather than the
        // content, and the crew window's number never moved no matter what was
        // inside it.
        if (auto* viewport = dynamic_cast<Gtk::Viewport*>(content)) {
            content = viewport->get_child();
            continue;
        }
        break;
    }

    int minimum = 0, natural = 0, ignore_a = 0, ignore_b = 0;
    if (content) {
        content->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural,
                         ignore_a, ignore_b);
    }
    // NATURAL, not minimum.
    //
    // A label's MINIMUM width is its longest unbreakable word, because it can
    // wrap -- so ten checkboxes in a row "fit" a 950px window by squashing each
    // to about 45px, and the check reported 432px while three switches were
    // genuinely unreachable. Natural width is what the content wants without
    // being squeezed, which is the question actually being asked.
    fit.minimum_width = natural;
    (void)minimum;
    return fit;
}

// Put something in every text field.
//
// Handlers validate before they act, so an empty form means every press lands in
// a guard clause and the test measures nothing but the guard clauses.
void fill_entries(Gtk::Widget* widget) {
    if (!widget) return;
    if (auto* entry = dynamic_cast<Gtk::Entry*>(widget)) {
        if (entry->get_text().empty()) entry->set_text("smoke");
    }
    for (Gtk::Widget* child = widget->get_first_child(); child;
         child = child->get_next_sibling()) {
        fill_entries(child);
    }
}

// Every control in a window, deepest last.
//
// Collected BEFORE anything is pressed, never during. A handler is free to add a
// row, hide a box or rebuild a list, and walking the tree while that happens is
// walking a tree that is being edited underneath you.
void collect_controls(Gtk::Widget* widget, std::vector<Gtk::Widget*>& found) {
    if (!widget) return;
    if (dynamic_cast<Gtk::Button*>(widget) || dynamic_cast<Gtk::Switch*>(widget) ||
        dynamic_cast<Gtk::CheckButton*>(widget) ||
        dynamic_cast<Gtk::ListBoxRow*>(widget)) {
        found.push_back(widget);
    }
    for (Gtk::Widget* child = widget->get_first_child(); child;
         child = child->get_next_sibling()) {
        collect_controls(child, found);
    }
}

// Press one control, whatever kind it is.
//
// Insensitive controls are SKIPPED rather than forced. A greyed-out Stop button
// is greyed out because there is nothing to stop, and activating it anyway would
// test a state the program is written to prevent -- and would report a crash
// that no user could ever reach.
bool press(Gtk::Widget* widget) {
    if (!widget->get_sensitive() || !widget->get_mapped()) return false;
    if (auto* toggle = dynamic_cast<Gtk::Switch*>(widget)) {
        toggle->set_active(!toggle->get_active());
        return true;
    }
    if (auto* check = dynamic_cast<Gtk::CheckButton*>(widget)) {
        check->set_active(!check->get_active());
        return true;
    }
    if (auto* row = dynamic_cast<Gtk::ListBoxRow*>(widget)) {
        // The launcher is a list, not a row of buttons, so without this it
        // reported "pressed 0/0" and looked like a window with nothing in it.
        row->activate();
        return true;
    }
    if (auto* button = dynamic_cast<Gtk::Button*>(widget)) {
        // activate(), not a synthesised click. This is GTK's own API for "do what
        // this widget does", called in this process on a widget this process
        // built. Nothing is sent to the X server, no pointer moves, and no window
        // but ours can possibly receive it.
        button->activate();
        return true;
    }
    return false;
}

// Build one window by name, or null when the name is unknown.
//
// ONE list, because there were three: the single-window mode, the width check and
// a dead copy left behind by an earlier version. They had already drifted -- the
// check knew about four windows and the shell about eight -- and a list that has
// to be updated in three places is a list that will be updated in one.
std::unique_ptr<Gtk::Window> make_window(const std::string& name,
                                         const auspex::Config& config) {
    if (name == "launcher") return std::make_unique<auspex::gtk::LauncherWindow>(config);
    if (name == "calendar") return std::make_unique<auspex::gtk::CalendarWindow>();
    if (name == "crew")     return std::make_unique<auspex::gtk::CrewWindow>();
    if (name == "brain")    return std::make_unique<auspex::gtk::BrainWindow>();
    if (name == "projects") return std::make_unique<auspex::gtk::ProjectsWindow>(config);
    if (name == "team")     return std::make_unique<auspex::gtk::TeamWindow>(config);
    if (name == "settings") return std::make_unique<auspex::gtk::SettingsWindow>(config);
    if (name == "chat") {
        // Chat needs a voice controller, and it must outlive the window. The
        // constructor only picks an input device -- it does not open the
        // microphone -- so building one to measure a window records nothing.
        static std::unique_ptr<auspex::gtk::VoiceController> voice;
        if (!voice) voice = std::make_unique<auspex::gtk::VoiceController>(config);
        return std::make_unique<auspex::gtk::ChatWindow>(config, *voice);
    }
    return nullptr;
}

// Opens one named window, for looking at. False when the name is unknown.
bool open_single_window(Gtk::Application& app, const std::string& name,
                        const auspex::Config& config) {
    // HOLD the application for as long as the window lives.
    //
    // add_window() alone was not enough: with HANDLES_COMMAND_LINE the app exits
    // when the command-line handler returns unless something is holding it, and
    // the result was a mode that started, printed its theme line and vanished --
    // intermittently, which is worse than never. Released when the window closes,
    // so quitting still quits.
    const auto keep_open = [&app](Gtk::Window* window) {
        app.hold();
        window->signal_hide().connect([&app] { app.release(); });
        app.add_window(*window);
        window->present();
    };

    std::unique_ptr<Gtk::Window> window = make_window(name, config);
    if (!window) return false;
    keep_open(window.release());   // keep_open owns it from here
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // ARMED HERE, before anything else in the program runs.
    //
    // The guards read AUSPEX_SMOKE once and cache it, so setting it later than
    // the first thing that consults it would leave them off -- and --smoke with
    // the guards off is a program that presses "Talk" and opens the microphone
    // for real. Set from argv directly rather than from the parsed flag, because
    // the parsed flag arrives inside the command-line handler, which is already
    // too late to be sure.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--smoke") ::setenv("AUSPEX_SMOKE", "1", 1);
    }

    // Panels are dock windows that reserve screen edges via _NET_WM_STRUT_PARTIAL.
    // None of that exists under Wayland, so fail loudly rather than drawing a
    // floating window that silently does not dock.
    if (const char* session = std::getenv("XDG_SESSION_TYPE");
        session && std::string(session) != "x11") {
        std::cerr << "auspex-shell: requires an X11 session (found " << session << ")\n";
        return 1;
    }

    // --window <name> opens ONE window and nothing else: no panels, no docking,
    // no voice.
    //
    // This exists because the GUI is the part of Auspex that cannot be driven from
    // here. Synthesising input is off-limits, so a change to a window could be
    // read in the source and never looked at -- which is exactly how the engine
    // came to be several features ahead of anything you can click. A window that
    // can be opened on its own can at least be SEEN.
    std::string only_window;
    bool smoke = false;
    bool check_glass = false;
    int check_width = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--window" && i + 1 < argc) only_window = argv[i + 1];
        // --check-windows [width]: does every window FIT, or does its content
        // force it wider than a person's window actually is?
        if (arg == "--check-glass") {
            check_glass = true;
            continue;
        }
        if (arg == "--smoke") {
            smoke = true;
            continue;
        }
        if (arg == "--check-windows") {
            // -1 means "each window against its OWN default size", which is the
            // question that matters: a window is not usually opened at some width
            // I chose, it is opened at the size it opens at. Checking everything
            // against one shared width said "every window fits" while the crew
            // window needed 888px and opened at 720 -- fine at the width I tested,
            // broken on every first run.
            check_width = (i + 1 < argc && argv[i + 1][0] != '-')
                              ? std::atoi(argv[i + 1])
                              : -1;
            if (check_width == 0) check_width = -1;
        }
    }

    // A SEPARATE IDENTITY, and NON_UNIQUE, for the inspection modes.
    //
    // A running panel owns "one.auspex.Shell", so a second process with that id
    // hands off to it and exits. That is what --check-windows did on its first
    // run: no output, exit 0, looking exactly like a check that had passed. A
    // check that silently does nothing is worse than no check at all.
    const bool inspecting = !only_window.empty() || check_width != 0 || smoke || check_glass;
    auto app = Gtk::Application::create(
        inspecting ? "one.auspex.ShellInspect" : "one.auspex.Shell",
        Gio::Application::Flags::HANDLES_COMMAND_LINE |
            (inspecting ? Gio::Application::Flags::NON_UNIQUE
                        : Gio::Application::Flags::NONE));
    // Our own flag would otherwise be rejected as unknown before activate runs.
    app->signal_command_line().connect(
        [&app](const Glib::RefPtr<Gio::ApplicationCommandLine>&) {
            app->activate();
            return 0;
        },
        /*after=*/false);

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

        // Every window measured, then stop. No display interaction, no clicking:
        // a minimum wider than the target is content GTK will not shrink to fit,
        // which is content you cannot reach.
        if (check_glass) {
            // The check I should have written before claiming the windows were
            // transparent. I measured ONE window, got a number I liked and said
            // it held for eight; the Team window was letting through nothing at
            // all. This asks the same question of every window, of the property
            // the compositor actually reads.
            app->hold();
            static std::vector<std::unique_ptr<Gtk::Window>> shown;
            for (const auto& name : window_names()) {
                std::unique_ptr<Gtk::Window> window = make_window(name, config);
                if (!window) continue;
                window->present();
                shown.push_back(std::move(window));
            }

            // RETRIED until it settles, not sampled once.
            //
            // The dimming is another process's timer, and eight windows appearing
            // at once are not all dimmed in the same tick. Sampling once at six
            // seconds reported three windows solid that were merely not dimmed
            // YET -- a check that races the thing it is checking, which is worse
            // than no check because it fails on a healthy system.
            static int attempt = 0;
            Glib::signal_timeout().connect(
                [&app, config]() -> bool {
                    ++attempt;
                    constexpr int kLastAttempt = 12;
                    // The dimming is done by the RUNNING SHELL, on a timer. With
                    // no shell running nothing dims anything, and reporting eight
                    // failures would be blaming the windows for the absence of
                    // the thing that dims them.
                    bool shell_running = false;
                    for (const auto& placed : auspex::list_placed_windows()) {
                        if (placed.window.title.find("Auspex Panel") !=
                            std::string::npos) {
                            shell_running = true;
                        }
                    }
                    if (!shell_running) {
                        std::cout << "no panel is running, so nothing is dimming "
                                     "anything -- start the shell first\n";
                        for (auto& window : shown) window->hide();
                        shown.clear();
                        std::exit(2);
                    }

                    std::ostringstream report;
                    const double want = config.screen_opacity;
                    int bad = 0, checked = 0;
                    for (const auto& placed : auspex::list_placed_windows()) {
                        const std::string& title = placed.window.title;
                        if (title.find("Auspex") == std::string::npos) continue;

                        const bool is_chrome =
                            title.find("Auspex Panel") != std::string::npos ||
                            title.find("Auspex Desktop") != std::string::npos;
                        const auto opacity =
                            auspex::window_opacity(placed.window.id);

                        std::string name = title.substr(title.find("Auspex"));
                        if (is_chrome) {
                            // The bars and the wallpaper are supposed to carry
                            // nothing: they dim themselves, and dimming them from
                            // outside as well would fade the clock and grey out
                            // the wallpaper.
                            report << (opacity ? "  WRONG  " : "  ok     ") << name
                                   << "  chrome, should carry no opacity"
                                   << (opacity ? " but carries one" : "") << "\n";
                            if (opacity) ++bad;
                            continue;
                        }

                        // IS IT EVEN ON THE PANEL'S MONITOR?
                        //
                        // Eight windows opening at once get cascaded by the
                        // window manager, and some land mostly on another screen.
                        // Those are supposed NOT to be dimmed -- so demanding it
                        // of them made this check fail at random on a shell that
                        // was behaving perfectly, which is the worst kind of
                        // check to have. Same rule the dimming pass uses: the
                        // centre decides.
                        //
                        // It also means this now tests the OTHER half for free.
                        // A window off the panel's monitor must carry no opacity,
                        // which is the drag-to-another-screen behaviour I could
                        // not test by dragging.
                        const auto panel_screen = auspex::primary_monitor();
                        const int cx = placed.bounds.x + placed.bounds.width / 2;
                        const int cy = placed.bounds.y + placed.bounds.height / 2;
                        const bool here =
                            panel_screen &&
                            cx >= panel_screen->bounds.x &&
                            cx < panel_screen->bounds.x + panel_screen->bounds.width &&
                            cy >= panel_screen->bounds.y &&
                            cy < panel_screen->bounds.y + panel_screen->bounds.height;

                        if (!here) {
                            report << (opacity ? "  WRONG  " : "  off    ") << name
                                   << "  not on the panel's monitor, so it should "
                                   << "carry no opacity"
                                   << (opacity ? " but carries one" : "") << "\n";
                            if (opacity) ++bad;
                            continue;
                        }

                        ++checked;
                        const bool dimmed =
                            opacity && std::abs(*opacity - want) < 0.02;
                        report << (dimmed ? "  glass  " : "  SOLID  ") << name
                               << "  ";
                        if (opacity) report << "opacity " << *opacity;
                        else report << "no opacity property";
                        report << ", wanted " << want << "\n";
                        if (!dimmed) ++bad;
                    }

                    if (checked == 0) {
                        std::cout << "no windows were checked\n";
                        ++bad;
                    }

                    // Still settling: say nothing and look again. Only the last
                    // attempt is allowed to report, so the output is one verdict
                    // rather than a dozen contradictory ones.
                    if (bad != 0 && attempt < kLastAttempt) {
                        report.str("");
                        return true;
                    }
                    std::cout << report.str();
                    std::cout << (bad == 0
                                      ? "every window is dimmed like the desktop\n"
                                      : std::to_string(bad) + " window(s) wrong\n");
                    for (auto& window : shown) window->hide();
                    shown.clear();
                    std::exit(bad == 0 ? 0 : 1);
                    return false;
                },
                // Every 1.5s, up to twelve times: long enough for another
                // process's poll to come round several times over.
                1500);
            return;
        }

        if (smoke) {
            // Belt and braces. If this is ever false the guards are not on, and
            // the next thing this mode would do is press every button in the
            // shell with nothing stopping any of them.
            if (!auspex::smoke_mode()) {
                std::cerr << "auspex-shell: --smoke could not arm its guards; "
                             "refusing to press anything\n";
                std::exit(2);
            }

            // Same reason --check-windows holds: with HANDLES_COMMAND_LINE the
            // application exits the moment this handler returns.
            app->hold();
            static std::vector<std::unique_ptr<Gtk::Window>> alive;
            for (const auto& name : window_names()) {
                std::unique_ptr<Gtk::Window> window = make_window(name, config);
                if (!window) continue;
                window->present();
                alive.push_back(std::move(window));
            }

            Glib::signal_timeout().connect_once(
                [&app] {
                    int total = 0, refused_total = 0, dead = 0;
                    for (std::size_t i = 0; i < alive.size(); ++i) {
                        auspex::smoke_reset();

                        // FILLED FIRST. Nearly every handler validates before it
                        // does anything -- no task, no project, no prompt -- so
                        // pressing Start on an empty form exercises the guard
                        // clause and nothing else. The first run of this pressed
                        // 104 controls and reached an external sink exactly zero
                        // times, which looked like a pass and was really a
                        // measurement of how far the buttons had NOT got.
                        fill_entries(alive[i]->get_child());

                        std::vector<Gtk::Widget*> controls;
                        collect_controls(alive[i]->get_child(), controls);

                        int pressed = 0;
                        for (Gtk::Widget* control : controls) {
                            if (press(control)) ++pressed;
                        }
                        // Let anything the handlers posted to the main loop run,
                        // so a crash lands inside this check rather than after it.
                        // Several of these hand their work to a thread, and a
                        // refusal recorded on that thread arrives after the click
                        // returns -- so this pumps for a while rather than just
                        // draining what is already queued.
                        for (int spin = 0; spin < 400; ++spin) {
                            while (Glib::MainContext::get_default()->pending()) {
                                Glib::MainContext::get_default()->iteration(false);
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }

                        const auto refused = auspex::smoke_refusals();
                        total += pressed;
                        refused_total += static_cast<int>(refused.size());

                        std::cout << "  " << window_names()[i] << ": pressed "
                                  << pressed << "/" << controls.size()
                                  << ", refused " << refused.size();
                        if (!refused.empty()) {
                            std::cout << " (";
                            for (std::size_t r = 0; r < refused.size() && r < 4; ++r) {
                                std::cout << (r ? ", " : "") << refused[r];
                            }
                            if (refused.size() > 4) std::cout << ", …";
                            std::cout << ")";
                        }
                        std::cout << "\n";
                        if (pressed == 0 && !controls.empty()) ++dead;
                    }

                    // A window whose controls all did nothing is the failure this
                    // is really looking for: it is what a window looks like when
                    // its buttons were never connected to anything.
                    std::cout << "pressed " << total << " control(s), "
                              << refused_total << " reached something that touches "
                              << "the outside world and was refused\n";
                    std::cout << (dead == 0 ? "every window responded\n"
                                            : std::to_string(dead) +
                                                  " window(s) did nothing at all\n");
                    for (auto& window : alive) window->hide();
                    alive.clear();
                    std::exit(dead == 0 ? 0 : 1);
                },
                800);
            return;
        }

        if (check_width != 0) {
            // The windows are PRESENTED before being measured.
            //
            // An unrealised widget has no display connection and cannot size its
            // own text, so measure() on one returns a number that looks plausible
            // and is not real -- the first version of this reported 432px for a
            // window whose switch row alone needed over 900, and passed the exact
            // bug it was written to catch. They flash on screen for a moment;
            // that is the price of a number that means something.
            // Held, or the application exits the moment the command-line
            // handler returns and the timer below never fires -- which looked
            // exactly like a check that had run and found nothing.
            app->hold();
            static std::vector<std::unique_ptr<Gtk::Window>> built;
            static std::vector<int> widths;   // what each opens at, read before it does
            for (const auto& name : window_names()) {
                std::unique_ptr<Gtk::Window> window = make_window(name, config);
                if (!window) continue;
                // Only overridden when a width was asked for. Otherwise the
                // window keeps the size it really opens at, which is the size
                // being judged.
                if (check_width > 0) window->set_default_size(check_width, 400);

                // Read BEFORE present(). Once a window is realised, GTK writes the
                // window's actual size back into default-width, so asking
                // afterwards returns whatever the window manager settled on --
                // launcher reported 950 when it opens at 520. The size it opens
                // at is only knowable before it opens.
                int opens_at = 0, ignore_height = 0;
                window->get_default_size(opens_at, ignore_height);
                widths.push_back(opens_at);
                window->present();
                built.push_back(std::move(window));
            }

            Glib::signal_timeout().connect_once(
                [&app, check_width] {
                    int bad = 0;
                    for (std::size_t i = 0; i < built.size(); ++i) {
                        const int width =
                            check_width > 0 ? check_width : widths[i];
                        const Fit fit = measure_window(*built[i], window_names()[i],
                                                       width);
                        std::cout << (fit.fits() ? "  fits   " : "  CLIPS  ")
                                  << fit.name << "  needs " << fit.minimum_width
                                  << "px, window is " << fit.target_width << "px\n";
                        if (!fit.fits()) ++bad;
                    }
                    std::cout << (bad == 0
                                      ? "every window fits\n"
                                      : std::to_string(bad) + " window(s) would clip\n");
                    for (auto& window : built) window->hide();
                    built.clear();
                    std::exit(bad == 0 ? 0 : 1);
                },
                600);
            return;
        }

        // One window, then stop. The stylesheet is installed first: a harness that
        // skips it renders unstyled GTK, and every judgement made from the result
        // is about the wrong program.
        if (!only_window.empty()) {
            if (!open_single_window(*app, only_window, config)) {
                std::cerr << "auspex-shell: no window called '" << only_window
                          << "'\n";
                app->quit();
            }
            return;
        }

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
        bottom->set_mode_handler([&](bool grid) { desktop->set_grid_mode(grid); });

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
