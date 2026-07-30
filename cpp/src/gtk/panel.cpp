#include "auspex/gtk/panel.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <glibmm/main.h>

#include "auspex/gtk/voice.hpp"
#include "auspex/process.hpp"

namespace auspex::gtk {

namespace {

// Was /tmp/MAGI/current_context.txt, chmod 666, in shared /tmp -- any local user
// could rewrite what the assistant believed it was looking at. Now under
// XDG_RUNTIME_DIR, which is per-user and 0700.
std::filesystem::path context_file() {
    return Config::runtime_dir() / "context.txt";
}

std::vector<std::string> split_words(const std::string& command) {
    std::vector<std::string> words;
    std::istringstream in(command);
    std::string word;
    while (in >> word) words.push_back(word);
    return words;
}

// Titles on a panel button must not push the window list off screen.
std::string elide(const std::string& text, std::size_t limit = 30) {
    if (text.size() <= limit) return text;
    // Do not cut mid-UTF-8-sequence: back off to a lead byte.
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "...";
}

}  // namespace

// ---------------------------------------------------------------------------
// WorkspaceSwitcher
// ---------------------------------------------------------------------------
WorkspaceSwitcher::WorkspaceSwitcher(int workspace_count)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 1) {
    if (workspace_count < 1) workspace_count = 1;

    for (int index = 0; index < workspace_count; ++index) {
        auto button = std::make_unique<Gtk::Button>(std::to_string(index + 1));
        button->signal_clicked().connect([index] { switch_workspace(index); });
        append(*button);
        buttons_.push_back(std::move(button));
    }

    poll();
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        1000);
}

void WorkspaceSwitcher::poll() {
    const auto active = current_workspace();
    if (!active) return;
    // Unlike workspace.py, this keeps polling for the lifetime of the panel: its
    // cache guard returned early once populated, freezing the highlight forever.
    if (*active == last_active_) return;
    last_active_ = *active;

    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (static_cast<int>(i) == *active) {
            buttons_[i]->add_css_class("active-workspace");
        } else {
            buttons_[i]->remove_css_class("active-workspace");
        }
    }
}

// ---------------------------------------------------------------------------
// WindowList
// ---------------------------------------------------------------------------
WindowList::WindowList() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 1) {
    set_hexpand(true);

    poll();
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        1000);
}

void WindowList::poll() {
    const auto windows = list_user_windows();

    std::map<std::string, bool> seen;
    for (const auto& window : windows) {
        seen[window.id] = true;

        auto it = entries_.find(window.id);
        if (it == entries_.end()) {
            Entry entry;
            entry.title  = window.title;
            entry.button = std::make_unique<Gtk::Button>(elide(window.title));
            const std::string id = window.id;
            entry.button->signal_clicked().connect([id] { activate_window(id); });
            append(*entry.button);
            entries_.emplace(window.id, std::move(entry));
        } else if (it->second.title != window.title) {
            // Only touch the label when the title actually changed; setting it
            // every second forces a needless relayout of the whole panel.
            it->second.title = window.title;
            it->second.button->set_label(elide(window.title));
        }
    }

    for (auto it = entries_.begin(); it != entries_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        remove(*it->second.button);
        it = entries_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// SystemMonitorWidget
// ---------------------------------------------------------------------------
SystemMonitorWidget::SystemMonitorWidget() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 4) {
    label_.add_css_class("monitor-label");
    append(label_);

    label_.set_text(monitor_.format_label());
    Glib::signal_timeout().connect(
        [this] {
            label_.set_text(monitor_.format_label());
            return true;
        },
        3000);
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
Clock::Clock() {
    add_css_class("clock-label");
    tick();
    Glib::signal_timeout().connect(
        [this] {
            tick();
            return true;
        },
        1000);
}

void Clock::tick() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return;

    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local) > 0) {
        set_text(buffer);
    }
}

// ---------------------------------------------------------------------------
// LlmContextButton
// ---------------------------------------------------------------------------
LlmContextButton::LlmContextButton(const Config& config, VoiceController& voice)
    : Gtk::Button("Ask anything..."), config_(config), voice_(voice) {
    add_css_class("llm-button");
    set_hexpand(true);
    set_halign(Gtk::Align::CENTER);

    // Commands, not plain Q&A: "open my downloads" should act, and anything that
    // is not an action degrades to a spoken answer anyway.
    signal_clicked().connect(
        [this] { voice_.submit(VoiceController::Action::Command); });

    poll();
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        250);
}

void LlmContextButton::poll() {
    const auto active = run({"xdotool", "getactivewindow", "getwindowname"});
    if (!active.ok) return;

    const std::string name = trim(active.out);
    // Skip our own windows, or the context would describe the panel itself.
    if (name.empty() || name.find("Auspex") != std::string::npos ||
        name.find("MAGI") != std::string::npos) {
        return;
    }

    const auto selection = run({"xclip", "-o", "-selection", "primary"});
    const std::string selected = selection.ok ? trim(selection.out) : std::string{};

    if (name == window_name_ && selected == selection_) return;
    window_name_ = name;
    selection_   = selected;

    std::ostringstream context;
    if (!selected.empty()) {
        set_label("Ask about selection...");
        context << "Context: Selected text in " << name << ":\n" << selected;
    } else {
        set_label("Ask about " + elide(name, 40) + "...");
        context << "Context: Working with " << name;
    }

    std::error_code ec;
    std::filesystem::create_directories(Config::runtime_dir(), ec);
    if (std::ofstream out(context_file(), std::ios::trunc); out) out << context.str();
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------
Panel::Panel(const Config& config, PanelPosition position, VoiceController& voice)
    : config_(config), position_(position), voice_(voice) {
    title_ = std::string("Auspex Panel (") + std::string(to_string(position)) + ")";

    set_title(title_);
    set_decorated(false);
    set_resizable(false);

    const auto monitor = primary_monitor();
    last_monitor_ = monitor ? monitor->bounds : Rect{0, 0, 1920, 1080};
    last_layout_  = compute_panel_layout(last_monitor_, 1, config_.panel_height, position_);

    set_default_size(last_layout_.bounds.width, last_layout_.bounds.height);

    box_.set_margin_start(2);
    box_.set_margin_end(2);
    set_child(box_);

    if (position_ == PanelPosition::Top) build_top();
    else build_bottom();

    // The X window does not exist until realize, and wmctrl cannot see it until it
    // is mapped, so docking is retried from a timer rather than done inline.
    signal_realize().connect([this] {
        Glib::signal_timeout().connect(
            [this] {
                dock();
                return !window_id_.has_value() && ++dock_attempts_ < 40;
            },
            150);
    });
}

void Panel::build_top() {
    launcher_.set_label(" Auspex ");
    launcher_.add_css_class("launcher-button");
    // Our own launcher window now, so the shell no longer needs xfce4-appfinder or
    // rofi to be installed.
    launcher_.signal_clicked().connect([this] { show_launcher(); });
    box_.append(launcher_);

    workspaces_ = std::make_unique<WorkspaceSwitcher>(config_.workspace_count);
    box_.append(*workspaces_);

    windows_ = std::make_unique<WindowList>();
    box_.append(*windows_);

    sysmon_ = std::make_unique<SystemMonitorWidget>();
    box_.append(*sysmon_);

    // Only shown when something to launch was actually found on this system.
    if (!config_.network_command.empty()) {
        network_icon_.set_from_icon_name("network-wireless-symbolic");
        network_.set_child(network_icon_);
        const std::string command = config_.network_command;
        network_.signal_clicked().connect([command] { launch(command); });
        box_.append(network_);
    }

    clock_ = std::make_unique<Clock>();
    box_.append(*clock_);
}

void Panel::build_bottom() {
    button_box_.set_halign(Gtk::Align::CENTER);
    button_box_.set_hexpand(true);

    // Upstream launched src/settings.py here. That window is not ported yet, so
    // this opens whichever settings app this desktop provides.
    // Our own settings window, replacing settings.py; the detected system settings
    // manager is no longer used as a stand-in.
    settings_icon_.set_from_icon_name("preferences-system-symbolic");
    settings_.set_child(settings_icon_);
    settings_.signal_clicked().connect([this] { show_settings(); });
    button_box_.append(settings_);

    llm_ = std::make_unique<LlmContextButton>(config_, voice_);
    // Left click speaks a command; right click opens the chat window, so both the
    // voice and typed paths are reachable from the same control.
    {
        auto secondary = Gtk::GestureClick::create();
        secondary->set_button(GDK_BUTTON_SECONDARY);
        secondary->signal_pressed().connect(
            [this](int, double, double) { show_chat(); });
        llm_->add_controller(secondary);
        llm_->set_tooltip_text("Click to speak a command, right-click to chat");
    }
    button_box_.append(*llm_);

    speak_icon_.set_from_icon_name("audio-speakers-symbolic");
    speak_.set_child(speak_icon_);
    speak_.signal_clicked().connect(
        [this] { voice_.submit(VoiceController::Action::SpeakSelection); });
    button_box_.append(speak_);

    // Press-and-hold dictation: recording lasts exactly as long as the button is
    // down, restoring the gesture behaviour of widgets/voice.py. A GestureClick is
    // used rather than signal_clicked because we need press and release separately.
    dictate_icon_.set_from_icon_name("audio-input-microphone-symbolic");
    dictate_.set_child(dictate_icon_);
    dictate_gesture_ = Gtk::GestureClick::create();
    dictate_gesture_->signal_pressed().connect([this](int, double, double) {
        voice_.press_hold(VoiceController::Action::Dictate);
    });
    dictate_gesture_->signal_released().connect([this](int, double, double) {
        voice_.release_hold();
    });
    dictate_.add_controller(dictate_gesture_);
    dictate_.set_tooltip_text("Hold to dictate into the focused window");
    button_box_.append(dictate_);

    if (!config_.terminal.empty()) {
        terminal_icon_.set_from_icon_name("utilities-terminal-symbolic");
        terminal_.set_child(terminal_icon_);
        terminal_.signal_clicked().connect([this] { launch(config_.terminal); });
        button_box_.append(terminal_);
    }

    // Always-on listening. VAD decides utterance boundaries, so no button is held
    // and no fixed window applies.
    // media-record, not another microphone: the dictate button beside it already
    // uses a mic glyph, and two microphones in a row were indistinguishable. A
    // record dot also reads correctly for a latching always-on state, which is what
    // this is, unlike the momentary dictate button.
    ear_icon_.set_from_icon_name("media-record-symbolic");
    ear_box_.set_spacing(4);
    ear_box_.append(ear_icon_);
    ear_label_.set_text("Auto");
    ear_box_.append(ear_label_);
    ear_.set_child(ear_box_);
    ear_.set_tooltip_text("Listen continuously (hands-free)");
    ear_.signal_toggled().connect([this] {
        if (ear_.get_active()) {
            if (!voice_.start_continuous(VoiceController::Action::Command)) {
                // Untoggle rather than sit in a state that looks armed but is not.
                ear_.set_active(false);
                return;
            }
            ear_.add_css_class("recording");
        } else {
            voice_.stop_continuous();
            ear_.remove_css_class("recording");
        }
    });
    button_box_.append(ear_);

    box_.append(button_box_);

    install_status_handler();
}

void Panel::install_status_handler() {
    // Surface worker-thread progress on the context button so voice actions are
    // not silent. VoiceController marshals this onto the GTK thread.
    voice_.on_status = [this](const std::string& text) {
        if (!llm_) return;
        if (text.empty()) {
            llm_->set_label("Ask anything...");
            llm_->remove_css_class("recording");
        } else {
            llm_->set_label(text);
            if (text == "Listening...") llm_->add_css_class("recording");
            else llm_->remove_css_class("recording");
        }
    };
}

void Panel::show_launcher() {
    if (launcher_window_) {
        launcher_window_->present();
        return;
    }
    launcher_window_ = std::make_unique<LauncherWindow>(config_);
    launcher_window_->signal_close_request().connect(
        [this] {
            // Destroyed from an idle callback: deleting the window from inside its
            // own close handler would free the object still on the stack.
            Glib::signal_idle().connect_once([this] { launcher_window_.reset(); });
            return false;
        },
        false);
    launcher_window_->present();
}

void Panel::show_settings() {
    if (settings_window_) {
        settings_window_->present();
        return;
    }
    settings_window_ = std::make_unique<SettingsWindow>(config_);
    settings_window_->signal_close_request().connect(
        [this] {
            Glib::signal_idle().connect_once([this] { settings_window_.reset(); });
            return false;
        },
        false);
    settings_window_->present();
}

void Panel::show_chat() {
    if (chat_window_) {
        chat_window_->present();
        return;
    }
    chat_window_ = std::make_unique<ChatWindow>(config_, voice_);
    chat_window_->signal_close_request().connect(
        [this] {
            // The chat window installs its own on_reply/on_status handlers, so they
            // must be cleared or the controller keeps calling into freed widgets.
            voice_.on_reply = nullptr;
            voice_.on_transcript = nullptr;
            Glib::signal_idle().connect_once([this] {
                chat_window_.reset();
                install_status_handler();
            });
            return false;
        },
        false);
    chat_window_->present();
}

void Panel::dock() {
    if (!window_id_) {
        window_id_ = dock::find_window_id(title_);
        if (!window_id_) return;
    }

    // Reserve exactly what is drawn. GTK's minimum height comes from the button
    // padding and min-height in the stylesheet, so the mapped window is taller
    // than config's panel_height; a strut computed from config alone would leave
    // other windows overlapping the panel by the difference.
    if (const int actual = get_height(); actual > 0) {
        last_layout_ = layout_for_height(last_monitor_, actual, position_);
    }

    dock::apply(*window_id_, last_layout_);

    // Re-check the monitor once a second. Rect has value equality, so this only
    // acts on real changes -- panel.py compared Gdk.Rectangle objects, which never
    // compared equal, and so re-ran three subprocesses every second forever.
    // Per-instance, not static: a static flag here would let only the first of the
    // two panels ever install a watcher.
    if (!watching_geometry_) {
        watching_geometry_ = true;
        Glib::signal_timeout().connect(
            [this] {
                refresh_geometry();
                return true;
            },
            1000);
    }
}

void Panel::refresh_geometry() {
    const auto monitor = primary_monitor();
    if (!monitor || monitor->bounds == last_monitor_) return;

    last_monitor_ = monitor->bounds;
    const int height = get_height() > 0 ? get_height() : config_.panel_height;
    last_layout_     = layout_for_height(last_monitor_, height, position_);
    set_default_size(last_layout_.bounds.width, last_layout_.bounds.height);

    if (window_id_) dock::apply(*window_id_, last_layout_);
}

void Panel::launch(const std::string& command) {
    const auto words = split_words(command);
    if (words.empty()) return;
    // Detached: run() would block the GTK thread until the app exits.
    spawn_detached(words);
}

}  // namespace auspex::gtk
