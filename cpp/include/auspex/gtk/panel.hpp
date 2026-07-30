// The MAGI panel window.
//
// Port of MAGIPanel from src/magi_shell/core/panel.py.
//
// The Python version never ran: _setup_window (called from __init__) invoked
// display.get_primary_monitor(), which GTK4 removed, so construction always threw
// AttributeError. The docking target is now resolved through auspex::primary_monitor(),
// which reads xrandr.
//
// Widgets are members rather than heap-allocated: gtkmm4 has no Gtk::manage for
// plain widgets, and Box::append does not take ownership.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/togglebutton.h>

#include "auspex/config.hpp"
#include "auspex/desktop.hpp"
#include "auspex/panel_dock.hpp"
#include "auspex/gtk/windows.hpp"
#include "auspex/sysmon.hpp"

namespace auspex::gtk {

class VoiceController;

// Buttons for each virtual desktop, highlighting the active one.
class WorkspaceSwitcher : public Gtk::Box {
public:
    explicit WorkspaceSwitcher(int workspace_count);

private:
    void poll();

    std::vector<std::unique_ptr<Gtk::Button>> buttons_;
    int last_active_ = -1;
};

// One button per user window, kept in sync with wmctrl -l.
class WindowList : public Gtk::Box {
public:
    WindowList();

private:
    void poll();

    struct Entry {
        std::unique_ptr<Gtk::Button> button;
        std::string title;
    };
    std::map<std::string, Entry> entries_;
};

// CPU/RAM/GPU/VRAM readout.
class SystemMonitorWidget : public Gtk::Box {
public:
    SystemMonitorWidget();

private:
    Gtk::Label     label_;
    SystemMonitor  monitor_;
};

class Clock : public Gtk::Label {
public:
    Clock();

private:
    void tick();
};

// "Ask about <window>..." button, tracking the focused window and primary
// selection. Writes the same /tmp/MAGI/current_context.txt the Python did.
class LlmContextButton : public Gtk::Button {
public:
    explicit LlmContextButton(const Config& config, VoiceController& voice);

private:
    void poll();

    const Config&    config_;
    VoiceController& voice_;
    std::string      window_name_;
    std::string      selection_;
};

class Panel : public Gtk::ApplicationWindow {
public:
    Panel(const Config& config, PanelPosition position, VoiceController& voice);

private:
    void build_top();
    void build_bottom();
    void show_launcher();
    void show_settings();
    void show_chat();
    void install_status_handler();
    void dock();
    void refresh_geometry();

    // Spawns a command string (split on whitespace) detached from the panel.
    static void launch(const std::string& command);

    const Config&    config_;
    PanelPosition    position_;
    VoiceController& voice_;
    std::string      title_;

    Gtk::Box box_{Gtk::Orientation::HORIZONTAL, 2};

    // Top
    Gtk::Button                          launcher_;
    std::unique_ptr<WorkspaceSwitcher>   workspaces_;
    std::unique_ptr<WindowList>          windows_;
    std::unique_ptr<SystemMonitorWidget> sysmon_;
    Gtk::Button                          network_;
    Gtk::Image                           network_icon_;
    std::unique_ptr<Clock>               clock_;

    // Bottom
    Gtk::Box                          button_box_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button                       settings_;
    Gtk::Image                        settings_icon_;
    std::unique_ptr<LlmContextButton> llm_;
    Gtk::Button                       speak_;
    Gtk::Image                        speak_icon_;
    Gtk::Button                       dictate_;
    Gtk::Image                        dictate_icon_;
    Gtk::Button                       terminal_;
    Gtk::Image                        terminal_icon_;
    // A toggle, not a button: this is the always-on ear, the closest equivalent to
    // widgets/voice.py's WhisperingEarButton.
    Gtk::ToggleButton                 ear_;
    Gtk::Box                          ear_box_{Gtk::Orientation::HORIZONTAL, 4};
    Gtk::Image                        ear_icon_;
    Gtk::Label                        ear_label_;
    Glib::RefPtr<Gtk::GestureClick>   dictate_gesture_;

    // Held so a second click re-presents the existing window rather than stacking
    // duplicates; reset when the window is closed.
    std::unique_ptr<LauncherWindow> launcher_window_;
    std::unique_ptr<SettingsWindow> settings_window_;
    std::unique_ptr<ChatWindow>     chat_window_;

    bool                       watching_geometry_ = false;
    std::optional<std::string> window_id_;
    Rect                       last_monitor_{};
    PanelLayout                last_layout_{};
    int                        dock_attempts_ = 0;
};

}  // namespace auspex::gtk
