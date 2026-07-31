// The three auxiliary windows: launcher, chat and settings.
//
// Ports core/launcher.py, llm_menu.py and settings.py. All three are plain
// Gtk::Window rather than Adw::ApplicationWindow: libadwaita has no packaged C++
// bindings (no libadwaitamm on Mint 22.2), so its widgets are unreachable from C++.
// The theme's libadwaita CSS selectors are retained regardless, since they cost
// nothing and keep the stylesheet a faithful port.
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

#include "auspex/config.hpp"
#include "auspex/desktop_entries.hpp"
#include "auspex/timekeeping.hpp"

namespace auspex::gtk {

class VoiceController;

// Search-as-you-type application launcher. Replaces launcher.py, and means the
// shell no longer depends on xfce4-appfinder / rofi being installed.
class LauncherWindow : public Gtk::Window {
public:
    explicit LauncherWindow(const Config& config);

private:
    void refilter();
    void launch_selected();

    const Config& config_;

    Gtk::Box            root_{Gtk::Orientation::VERTICAL, 6};
    Gtk::Entry          search_;
    Gtk::ScrolledWindow scroller_;
    Gtk::ListBox        list_;

    std::vector<DesktopEntry> entries_;
    // Parallel to the visible rows, so a selection maps back to an entry.
    std::vector<const DesktopEntry*> visible_;
    // ListBoxRow, not Label. Appending a bare widget to a GtkListBox makes GTK
    // wrap it in a row it creates itself, so the widget's parent becomes that row
    // and NOT the list -- at which point list.remove(widget) warns "Tried to
    // remove non-child" and does nothing. Owning the rows explicitly is what makes
    // removal work, and removal working is what makes the search filter.
    std::vector<std::unique_ptr<Gtk::ListBoxRow>> rows_;
};

// What the crew is holding, and the two decisions you can make about each.
//
// Deliberately NOT a mirror of `ollamadev board` text: the reason an Auditor held
// something is the part you actually read before deciding, so it gets equal weight
// to the summary rather than being a dim second line.
class BoardWindow : public Gtk::Window {
public:
    BoardWindow();

    // Re-reads the board. Called on open and after every decision, because
    // accepting one changeset can release or invalidate another.
    void refresh();

private:
    void decide(int n, bool accept);

    Gtk::Box            root_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label          heading_;
    Gtk::ScrolledWindow scroller_;
    Gtk::Box            list_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box            buttons_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button         refresh_{"Refresh"};
    Gtk::Button         close_{"Close"};

    std::vector<std::unique_ptr<Gtk::Widget>> rows_;
};

// Conversation window. Replaces llm_menu.py, including its per-message actions:
// copy, speak, and delete. The "run this code block" button llm_menu.py had is
// deliberately not ported -- see the note in windows.cpp.
class ChatWindow : public Gtk::Window {
public:
    ChatWindow(const Config& config, VoiceController& voice);

private:
    void send();
    void add_message(const std::string& text, bool from_user);
    void on_reply(const std::string& text);

    const Config&    config_;
    VoiceController& voice_;

    Gtk::Box            root_{Gtk::Orientation::VERTICAL, 0};
    Gtk::ScrolledWindow scroller_;
    Gtk::Box            log_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box            input_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Entry          entry_;
    Gtk::Button         send_{"Send"};
    Gtk::Button         talk_{"Talk"};
    Gtk::Label          status_;

    std::vector<std::unique_ptr<Gtk::Widget>> widgets_;
};

// Settings window. Replaces settings.py. Writes the same config.json the rest of
// Auspex reads, so a change is picked up by the panel's existing 1s config watcher
// without any extra plumbing.
class SettingsWindow : public Gtk::Window {
public:
    explicit SettingsWindow(Config config);

private:
    void save();
    void populate_microphones();

    Config config_;

    Gtk::Box  root_{Gtk::Orientation::VERTICAL, 10};
    Gtk::Box  buttons_{Gtk::Orientation::HORIZONTAL, 6};

    Gtk::DropDown theme_;
    Gtk::DropDown microphone_;
    Gtk::DropDown model_;
    Gtk::SpinButton panel_height_;
    Gtk::SpinButton workspaces_;
    Gtk::SpinButton memory_turns_;
    Gtk::SpinButton vad_threshold_;
    Gtk::CheckButton enable_ai_{"Enable AI features"};
    Gtk::CheckButton autostart_{"Start Auspex when I log in"};
    Gtk::CheckButton clock_24_{"Use a 24-hour clock"};

    // Date and time. Applied immediately on their own controls rather than on Save,
    // because they do not live in config.json -- they are system state behind a
    // polkit prompt, and batching a password prompt into "Save" would mean pressing
    // Save could ask for a password for a setting you did not touch.
    void refresh_time();
    void apply_timezone();
    void apply_manual_time();

    Gtk::Label       time_now_;
    Gtk::CheckButton time_automatic_{"Set the time automatically"};
    Gtk::DropDown    timezone_;
    Gtk::Entry       manual_time_;
    Gtk::Button      apply_time_{"Set"};
    Gtk::Box         manual_time_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label       time_status_;

    std::vector<std::string> timezones_;
    TimeSettings             time_state_;
    // Set while the controls are being filled in from the system, so their own
    // change handlers do not fire and try to apply what was just read.
    bool                     loading_time_ = false;
    Gtk::Entry terminal_;
    Gtk::Entry launcher_;
    Gtk::Label status_;
    Gtk::Button save_{"Save"};
    Gtk::Button close_{"Close"};

    std::vector<std::string> microphone_names_;
    std::vector<std::string> model_names_;
    std::vector<std::unique_ptr<Gtk::Widget>> widgets_;
};

}  // namespace auspex::gtk
