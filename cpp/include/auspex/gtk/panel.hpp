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
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/calendar.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/scale.h>
#include <gtkmm/togglebutton.h>

#include "auspex/config.hpp"
#include "auspex/desktop.hpp"
#include "auspex/panel_dock.hpp"
#include "auspex/gtk/tray.hpp"
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

    void set_restore_handler(sigc::slot<void(const std::string&)> handler) {
        on_restore_ = std::move(handler);
    }
    void set_full_handler(sigc::slot<void(const std::string&)> handler) {
        on_full_ = std::move(handler);
    }

private:
    void poll();

    // Clicking a task button does what xfce4-panel's does: it toggles. Clicking the
    // window you are already in minimises it; clicking any other one brings it up,
    // de-iconifying if it was minimised. One button, both directions -- which is the
    // only way a task list is usable without a separate minimise control.
    void toggle(const std::string& window_id);

    // Right-click a task button for the things a click cannot express: closing it,
    // and minimising one that is not currently focused. Without this there is no
    // way to close an application from inside Auspex at all -- you had to go to the
    // window's own titlebar, which the canvas may have moved off screen.
    void show_window_menu(const std::string& window_id, Gtk::Widget& anchor);

    struct Entry {
        std::unique_ptr<Gtk::Button> button;
        std::string title;
        // Tracked so the label and styling are only touched when they change:
        // rewriting them every second forces a relayout of the whole panel.
        bool minimized = false;
        bool active    = false;
    };
    std::map<std::string, Entry> entries_;

    // One popover reused for every button rather than one per window: the task list
    // churns as windows open and close, and a popover per entry would be built and
    // destroyed with them.
    Gtk::Popover menu_;
    Gtk::Box     menu_box_{Gtk::Orientation::VERTICAL, 4};
    std::string  menu_target_;
    sigc::slot<void(const std::string&)> on_restore_;
    sigc::slot<void(const std::string&)> on_full_;
};

// Volume, as a panel control rather than a tray icon.
//
// The sound icon on a stock Xfce desktop is libpulseaudio-plugin.so running inside
// xfce4-panel's own process -- a panel plugin, not a tray icon. It publishes
// nothing on StatusNotifierItem or on XApp, so no tray can adopt it and Auspex has
// to bring its own. It drives the same wpctl/pactl path the spoken "set the volume"
// command already uses.
// A MenuButton, not a Button with a popover bolted on. A plain button pops the
// popover up and then dismisses it again with its own release event -- the button
// highlights and nothing appears. MenuButton is the widget that owns this
// interaction, including parenting and placement.
class VolumeButton : public Gtk::MenuButton {
public:
    VolumeButton();

private:
    void poll();
    void apply(int percent, bool muted);

    Gtk::Popover     popover_;
    Gtk::Box         popover_box_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Scale       slider_;
    Gtk::Button      mute_{"Mute"};

    VolumeState state_;
    // Set while the slider is being dragged, so the one-second poll cannot yank the
    // handle out from under the pointer with a value it read mid-drag.
    bool        adjusting_ = false;
};

// Network, for the same reason as VolumeButton: it is not a tray icon on a stock
// desktop either, and having it here means the state is visible without expanding
// the tray.
class NetworkButton : public Gtk::MenuButton {
public:
    explicit NetworkButton(const Config& config);

private:
    void poll();

    const Config& config_;

    void rebuild_networks();

    Gtk::Popover popover_;
    Gtk::Box     popover_box_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label   status_;
    // Wifi list. Rebuilt each time the popover opens: a cached list is a list of
    // where you were, and the whole point of opening it is to see where you are.
    Gtk::Label          wifi_heading_;
    Gtk::ScrolledWindow wifi_scroller_;
    Gtk::Box            wifi_box_{Gtk::Orientation::VERTICAL, 2};
    Gtk::Button         radio_{"Turn Wi-Fi on"};
    Gtk::Button  settings_{"Network settings\u2026"};

    NetworkState state_;
    std::vector<std::unique_ptr<Gtk::Widget>> wifi_rows_;
};

// CPU/RAM/GPU/VRAM readout.
class SystemMonitorWidget : public Gtk::Box {
public:
    SystemMonitorWidget();

private:
    Gtk::Label     label_;
    SystemMonitor  monitor_;
};

// The clock, and the calendar behind it.
//
// A MenuButton rather than a Label, because a clock you cannot click to see the
// month is a clock that makes you open something else to answer "what day is the
// 14th" -- which is the one question a panel clock is asked.
class Clock : public Gtk::MenuButton {
public:
    explicit Clock(const Config& config);

private:
    void tick();

    const Config& config_;

    Gtk::Label    label_;
    Gtk::Popover  popover_;
    Gtk::Box      popover_box_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Calendar calendar_;
    Gtk::Button   today_{"Today"};
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
    // Resolved once at construction: whether reading the selection is even possible
    // here. The poller must not ask PATH this question on every tick.
    bool             selection_readable_ = false;
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
    void show_board();

public:
    // Wired by the shell to the DesktopWindow, which owns the canvas view. A
    // factor of 0 means "back to life size".
    void set_zoom_handler(sigc::slot<void(double)> handler) {
        on_zoom_ = std::move(handler);
    }
    void set_window_restore_handler(sigc::slot<void(const std::string&)> handler) {
        if (windows_) windows_->set_restore_handler(std::move(handler));
    }
    void set_window_full_handler(sigc::slot<void(const std::string&)> handler) {
        if (windows_) windows_->set_full_handler(std::move(handler));
    }
    void set_geometry_handler(sigc::slot<void(PanelPosition, int)> handler) {
        on_geometry_ = std::move(handler);
    }
    void set_pan_handler(sigc::slot<void(int, int)> handler) {
        on_pan_ = std::move(handler);
    }

private:
    sigc::slot<void(double)> on_zoom_;
    sigc::slot<void(PanelPosition, int)> on_geometry_;
    sigc::slot<void(int, int)> on_pan_;
    void install_status_handler();

    // Right-click menu. A panel with no way to close it is a panel you have to go
    // to a terminal to get rid of, which is not something a desktop should ask of
    // anyone -- and quitting must be a choice you make, never something a stray
    // click does, hence a menu rather than a close button.
    void show_panel_menu(double x, double y);
    void confirm_quit();
    void dock();
    void refresh_geometry();

    // Spawns a command string (split on whitespace) detached from the panel.
    static void launch(const std::string& command);

    const Config&    config_;
    PanelPosition    position_;
    VoiceController& voice_;
    std::string      title_;

    Gtk::Box box_{Gtk::Orientation::HORIZONTAL, 2};

    Gtk::Popover menu_;
    Gtk::Box     menu_box_{Gtk::Orientation::VERTICAL, 4};

    // Top
    Gtk::Button                          launcher_;

    // The one canvas control that is not a pan. It lives on the BOTTOM bar, in the
    // middle of the arrows -- see panel.cpp for why the zoom buttons that used to be
    // up here are gone.
    Gtk::Button                          zoom_reset_{"1:1"};
    std::unique_ptr<WorkspaceSwitcher>   workspaces_;
    std::unique_ptr<WindowList>          windows_;
    std::unique_ptr<SystemTray>          tray_;
    std::unique_ptr<VolumeButton>        volume_;
    std::unique_ptr<NetworkButton>       network_button_;
    std::unique_ptr<SystemMonitorWidget> sysmon_;
    Gtk::Button                          network_;
    Gtk::Image                           network_icon_;
    std::unique_ptr<Clock>               clock_;

    // Bottom
    Gtk::Box                          pan_box_{Gtk::Orientation::HORIZONTAL, 1};
    Gtk::Button                       pan_left_{"\u2190"};
    Gtk::Button                       pan_up_{"\u2191"};
    Gtk::Button                       pan_down_{"\u2193"};
    Gtk::Button                       pan_right_{"\u2192"};
    Gtk::Box                          button_box_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button                       settings_;
    Gtk::Image                        settings_icon_;
    std::unique_ptr<LlmContextButton> llm_;
    Gtk::Button                       speak_;
    Gtk::Image                        speak_icon_;
    Gtk::Button                       dictate_;
    Gtk::Image                        dictate_icon_;

    // Voice to text, as a TOGGLE rather than a press-and-hold.
    //
    // The mic beside it records only while held, which is right for a quick word
    // and wrong for a sentence -- holding a mouse button steady for thirty seconds
    // while composing is not something to ask of anyone. This one starts on a click
    // and stops on the next.
    Gtk::ToggleButton                 to_text_;
    Gtk::Box                          to_text_box_{Gtk::Orientation::HORIZONTAL, 4};
    Gtk::Image                        to_text_icon_;
    Gtk::Label                        to_text_label_;

    // Ask a question aloud and hear the answer. This path was implemented and
    // reachable from nothing at all -- every spoken utterance went through the
    // command parser first, which can read a plain question as an action.
    Gtk::Button                       ask_;
    Gtk::Image                        ask_icon_;
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
    std::unique_ptr<BoardWindow>    board_window_;

    bool                       watching_geometry_ = false;
    std::optional<std::string> window_id_;
    Rect                       last_monitor_{};
    PanelLayout                last_layout_{};
    int                        dock_attempts_ = 0;
};

}  // namespace auspex::gtk
