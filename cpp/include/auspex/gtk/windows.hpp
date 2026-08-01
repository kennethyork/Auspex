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
#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

#include "auspex/config.hpp"
#include "auspex/calendar.hpp"
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

    // Refreshes itself while a crew is working.
    //
    // Changesets land while you are looking at the window, and a board that only
    // updates when reopened is a board you have to remember to distrust. Driven by
    // the modification times of the two files the engine already maintains, so a
    // quiet minute costs two stats rather than two subprocesses.
    void watch();

    std::filesystem::file_time_type board_mtime_{};
    std::filesystem::file_time_type crew_mtime_{};
    bool have_board_mtime_ = false;
    bool have_crew_mtime_  = false;
    Gtk::Label   running_;

    Gtk::Box            root_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label          heading_;
    Gtk::ScrolledWindow scroller_;
    Gtk::Box            list_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box            buttons_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button         refresh_{"Refresh"};
    Gtk::Button         close_{"Close"};

    std::vector<std::unique_ptr<Gtk::Widget>> rows_;
};

// A month view, laid out the way a calendar is expected to look: a heading with
// the month and year, a row of weekday names, and a six-by-seven grid of days each
// showing what is on it.
//
// A window rather than a panel popover. A popover big enough to read a month in is
// a window that has to be dismissed by clicking away from it, and one small enough
// to be a popover cannot show a month.
class CalendarWindow : public Gtk::Window {
public:
    CalendarWindow();

private:
    void build_grid();
    void show_month(int year, int month);
    void select_day(const std::string& date);
    void reload_day();
    void add_event();
    void go_today();

    EventStore  events_;
    int         year_  = 0;
    int         month_ = 0;
    std::string selected_;

    Gtk::Box    root_{Gtk::Orientation::VERTICAL, 0};

    Gtk::Box    header_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Button previous_;
    Gtk::Button next_;
    Gtk::Button today_{"Today"};
    Gtk::Label  heading_;

    Gtk::Box    body_{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Grid   grid_;

    // The day panel down the side, which is where events are actually read and
    // entered -- a grid cell has room to say that something is there, not what.
    Gtk::Box            day_panel_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label          day_heading_;
    Gtk::ScrolledWindow day_scroller_;
    Gtk::Box            day_box_{Gtk::Orientation::VERTICAL, 4};
    Gtk::Box            entry_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Entry          time_entry_;
    Gtk::Entry          title_entry_;
    Gtk::Button         add_{"Add"};

    // Repetition, on its own row: it applies to what is being added, and putting it
    // beside the title would make a one-line entry look like a form.
    Gtk::Box            repeat_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::DropDown       repeat_;
    Gtk::Entry          until_entry_;
    std::vector<Repeat> repeat_values_;

    // One button per grid cell, kept so the month can be redrawn without rebuilding
    // the widgets -- 42 cells rebuilt on every page turn is a visible flicker.
    struct Cell {
        Gtk::Button* button = nullptr;
        Gtk::Box*    box    = nullptr;
        Gtk::Label*  number = nullptr;
        Gtk::Label*  detail = nullptr;
        std::string  date;
    };
    std::vector<Cell> cells_;
    std::vector<std::unique_ptr<Gtk::Widget>> day_rows_;
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

    // Pinned applications. Imported rather than hand-listed: the point is to stop
    // replacing xfce4-panel from costing someone the row of icons they have had one
    // click away for years.
    void import_pins();
    Gtk::Box    pins_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Label  pins_summary_;
    Gtk::Button import_pins_{"Import from xfce4-panel"};
    std::vector<std::string> pinned_;

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
