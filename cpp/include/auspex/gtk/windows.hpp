// The three auxiliary windows: launcher, chat and settings.
//
// Ports core/launcher.py, llm_menu.py and settings.py. All three are plain
// Gtk::Window rather than Adw::ApplicationWindow: libadwaita has no packaged C++
// bindings (no libadwaitamm on Mint 22.2), so its widgets are unreachable from C++.
// The theme's libadwaita CSS selectors are retained regardless, since they cost
// nothing and keep the stylesheet a faithful port.
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glibmm/dispatcher.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/textview.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/textview.h>
#include <gtkmm/window.h>

#include "auspex/agents.hpp"
#include "auspex/config.hpp"
#include "auspex/crew.hpp"
#include "auspex/calendar.hpp"
#include "auspex/desktop_entries.hpp"
#include "auspex/projects.hpp"
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

// The crew, run from inside Auspex.
//
// Before this, starting a crew meant opening a terminal on the canvas and typing at
// ollamadev -- which works, and is not integration. This is the front end: a task,
// the options that are worth choosing, what the crew is doing right now, and the
// changesets it is holding, in one window.
//
// The ENGINE is still ollamadev. Nothing here reimplements the Director, the
// worktrees or the Auditor; it builds an argv, starts it, and reads the state files
// the engine already writes. See crew.hpp for why that division is deliberate.
class CrewWindow : public Gtk::Window {
public:
    CrewWindow();

    // Cancels a run in flight and waits for it. A std::thread member that is
    // still joinable when it is destroyed calls std::terminate, so this is not
    // tidiness -- without it, quitting during a run takes the shell down.
    ~CrewWindow() override;

    // Points the window at a folder. Called by the project picker, so choosing a
    // project there and pressing Crew do not have to be told the same thing twice.
    void set_project(const std::filesystem::path& path);

private:
    void start();
    void stop();
    void refresh_run();
    void refresh_board();
    void decide(int n, bool accept);
    void choose_project();
    void show_project();

    // The run happens on its own thread and can take minutes. GTK may only be
    // touched from the main thread, so the worker signals through a Dispatcher and
    // the UI re-reads the state file the runner writes -- the same file the panel
    // already watches, so there is one source of truth rather than two.
    std::thread              runner_;
    std::atomic<bool>        cancel_{false};
    std::atomic<bool>        running_{false};
    Glib::Dispatcher         run_changed_;
    // Written by the worker, read on the GTK thread after a dispatch. Guarded
    // because both threads touch it.
    std::mutex               log_mutex_;
    std::string              last_log_;

    Gtk::Box root_{Gtk::Orientation::VERTICAL, 12};

    // WHERE. First, above the task, and never blank.
    //
    // This is the whole point of the window. `ollamadev crew` plans against, and
    // applies diffs into, the tree it is started in -- Crew.cpp takes
    // QDir::currentPath() and never revisits it. Until this existed that tree was
    // the panel's own working directory, which on a real login is $HOME. So the
    // Director was decomposing every task against the home directory, and an
    // accepted changeset would have landed there.
    std::filesystem::path project_;
    Gtk::Box    project_row_{Gtk::Orientation::HORIZONTAL, 8};
    // The whole window scrolls.
    //
    // root_ used to be the window's child directly, so anything past the bottom
    // edge was not merely off-screen, it was UNREACHABLE -- and the window grew
    // this session by two switches, a crew roster and a card per faculty. A
    // screenshot of it showed the option row sliced in half with no way to reach
    // the rest.
    Gtk::ScrolledWindow root_scroller_;

    Gtk::Label  project_label_;
    Gtk::Button project_pick_{"Change…"};

    // What to do.
    Gtk::Label  task_label_;
    Gtk::Entry  task_;
    Gtk::Box    start_row_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button start_{"Start the crew"};
    Gtk::Button stop_{"Stop"};
    Gtk::Button resume_{"Resume"};

    // The brain options. All opt-in, and a plain run is unchanged by them -- which
    // is why they are switches rather than a mode: each buys something at a cost in
    // time and tokens, and the honest default is none of them.
    Gtk::Box         options_{Gtk::Orientation::HORIZONTAL, 14};
    Gtk::CheckButton route_{"Route models"};
    Gtk::CheckButton debate_{"Debate"};
    Gtk::CheckButton dedupe_{"Dedupe"};
    Gtk::CheckButton learn_{"Learn"};
    Gtk::CheckButton security_{"Security scan"};

    // Run the project's tests after each coder, and hand back the failures.
    //
    // Turning this on also turns on the coders' ability to run commands, because
    // it IS that ability -- a suite is code, and a switch that quietly needed a
    // second switch to do anything would be the kind of setting that silently does
    // nothing. See on_start().
    Gtk::CheckButton verify_{"Run the tests"};

    // Write the shipped starter skills that match this task into the project.
    // On by default, matching RunOptions.
    Gtk::CheckButton starters_{"Starter skills"};
    Gtk::Box         second_row_{Gtk::Orientation::HORIZONTAL, 14};
    Gtk::Label       coders_label_;
    Gtk::SpinButton  coders_;
    // --swarm and --amplify. Numbers rather than switches because in both cases the
    // number IS the cost: swarm is how many coders may run at once, amplify is how
    // many times the planning and the review are repeated.
    Gtk::Label       swarm_label_;
    Gtk::SpinButton  swarm_;
    Gtk::Label       amplify_label_;
    Gtk::SpinButton  amplify_;
    Gtk::Label       pack_label_;
    Gtk::DropDown    pack_;
    std::vector<std::string> packs_;

    // What it is doing, as lanes rather than a list.
    //
    // To-do / Doing / Done, which is how ollamadev-qt shows it and how the shape of
    // a run reads at a glance: a list of eight rows with a word beside each one
    // makes you count, three columns makes you look.
    Gtk::Label run_heading_;

    // WHO IS IN THE CREW, above the lanes.
    //
    // The lanes are coders and nothing else, so a crew of five read as a crew of
    // one -- the Researcher, the Director and the Auditor all worked and none of
    // them ever appeared. This is the row that says they exist and which of them
    // is working now.
    Gtk::Box   crew_row_{Gtk::Orientation::HORIZONTAL, 4};
    std::vector<std::unique_ptr<Gtk::Widget>> crew_chips_;

    Gtk::Box   lanes_{Gtk::Orientation::HORIZONTAL, 10};
    struct Lane {
        Gtk::Box            column{Gtk::Orientation::VERTICAL, 4};
        Gtk::Label          title;
        Gtk::ScrolledWindow scroller;
        Gtk::Box            body{Gtk::Orientation::VERTICAL, 4};
    };
    // Held last, and hidden when empty -- ollamadev-qt only adds that column when
    // something is actually held, so an ordinary run still reads as three lanes.
    Lane todo_, doing_, done_, held_;
    std::vector<std::unique_ptr<Gtk::Widget>> run_rows_;

    // Steering a coder that is already running.
    Gtk::Box    steer_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Entry  steer_;
    Gtk::Button steer_send_{"Steer"};

    // Which held changesets have their diff open. Kept across a refresh, or the
    // board updating while you are reading a patch would close it under you.
    std::set<int> expanded_;

    // What the last run cost, per model.
    //
    // The engine has metered this since usage.hpp existed and nothing in the
    // window ever showed it, so a run on a metered model looked exactly like a run
    // on a local one. Blank until a run finishes.
    Gtk::Label cost_;
    // Written by the worker under log_mutex_, read on the GTK thread, like
    // last_log_ beside it -- a std::string touched by two threads otherwise.
    std::string last_cost_;

    // What it is holding.
    Gtk::Label          board_heading_;
    Gtk::ScrolledWindow board_scroller_;
    Gtk::Box            board_box_{Gtk::Orientation::VERTICAL, 8};
    std::vector<std::unique_ptr<Gtk::Widget>> board_rows_;

    Gtk::Label status_;

    // Both watched by modification time. They answer different questions: the
    // board file says whether anything has landed, the crew file whether anything
    // is still working -- so a board that watched only one would either miss
    // arrivals or be unable to say why it is empty.
    std::filesystem::file_time_type run_mtime_{};
    std::filesystem::file_time_type board_mtime_{};
    bool have_run_mtime_   = false;
    bool have_board_mtime_ = false;
};

// Pick a folder, pick an agent, open it there.
//
// The gap this fills: Auspex could already start claude, codex, opencode and the
// rest -- by voice, into a terminal on the canvas -- but never with a directory.
// They all read and write the tree they are launched in, so "open a claude code
// agent" meant "open one wherever the panel happens to be", which is the login
// directory. That is not a usable way to work on anything.
//
// The folder list is ollamadev's own bookmarks (~/.ollamadev/workspaces.json) plus
// Auspex's recents, so a project adopted in either program appears in both. See
// projects.hpp for why that file rather than a list of our own.
//
// The agent list is only what is INSTALLED. An entry that cannot start is a button
// that reports a failure you could have been told about before pressing it.
class ProjectsWindow : public Gtk::Window {
public:
    // The crew handler is wired by the Panel, which owns the crew window: choosing
    // a folder here and starting a crew there should be one gesture, not two
    // separate places to say the same thing.
    explicit ProjectsWindow(const Config& config);

    void set_crew_handler(sigc::slot<void(std::filesystem::path)> handler) {
        on_crew_ = std::move(handler);
    }

private:
    void reload();
    void select(const std::filesystem::path& path);
    void browse();
    void open_in(const AgentTool& agent);
    void open_terminal();
    void open_files();

    const Config& config_;

    // The chosen folder. Empty only when there are no projects and nothing has been
    // browsed to, which is the one state where the agent buttons are insensitive --
    // launching into no directory is exactly the bug this window exists to fix.
    std::filesystem::path selected_;
    std::vector<Project>  projects_;

    Gtk::Box            root_{Gtk::Orientation::HORIZONTAL, 0};

    // Folders down the left, because the folder is chosen first and then acted on.
    Gtk::Box            left_{Gtk::Orientation::VERTICAL, 6};
    Gtk::Label          left_heading_;
    Gtk::ScrolledWindow list_scroller_;
    Gtk::ListBox        list_;
    Gtk::Button         browse_{"Open another folder…"};
    std::vector<std::unique_ptr<Gtk::ListBoxRow>> rows_;

    // What to do with it, down the right.
    Gtk::Box            right_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Label          chosen_name_;
    Gtk::Label          chosen_path_;
    Gtk::Label          agents_heading_;
    Gtk::Box            agent_box_{Gtk::Orientation::VERTICAL, 6};
    Gtk::Box            extras_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Button         terminal_{"Terminal"};
    Gtk::Button         files_{"Files"};
    Gtk::Button         crew_{"Crew…"};
    Gtk::Label          status_;

    std::vector<AgentTool>                    agents_;
    std::vector<std::unique_ptr<Gtk::Widget>> agent_widgets_;

    sigc::slot<void(std::filesystem::path)> on_crew_;
};

// One prompt, several agents, at once.
//
// Ports ollamadev-qt's AgentTeamPane. Tick the providers, type one thing, press
// Launch, and each gets its own terminal running `ollamadev --backend <id>
// "<prompt>"` in the chosen folder.
//
// This is the piece that was missing when Auspex is compared to the products that
// sell "an army of agents": the panel could already open agents, but only one at a
// time and only with an empty prompt, so fanning a question across four of them
// meant four terminals and four paste operations.
//
// The ENGINE does the work, as everywhere else here. Auspex ticks boxes and builds
// argv; ollamadev knows what a backend is.
class TeamWindow : public Gtk::Window {
public:
    explicit TeamWindow(const Config& config);

    void set_project(const std::filesystem::path& path);

private:
    void launch();
    void choose_project();
    void show_project();

    const Config& config_;

    // Same rule as the crew: every one of these edits the tree it lands in, so the
    // folder is named on screen and never inferred.
    std::filesystem::path project_;

    Gtk::Box    root_{Gtk::Orientation::VERTICAL, 10};
    Gtk::Box    project_row_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Label  project_label_;
    Gtk::Button project_pick_{"Change…"};

    Gtk::Label          providers_heading_;
    Gtk::ScrolledWindow providers_scroller_;
    Gtk::Box            providers_{Gtk::Orientation::VERTICAL, 2};
    // Parallel to `backends_`. Boxes for providers that are not installed exist but
    // are insensitive, so the list says what the machine COULD do as well as what
    // it can -- which is the difference between a short list and a broken one.
    std::vector<std::unique_ptr<Gtk::CheckButton>> boxes_;
    std::vector<Backend> backends_;

    Gtk::Label          prompt_label_;
    Gtk::ScrolledWindow prompt_scroller_;
    Gtk::TextView       prompt_;

    Gtk::Box    buttons_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button launch_{"Launch team"};
    Gtk::Label  status_;
};

// The crew's brain: what it is made of, and which model does each part.
//
// This window used to edit ollamadev's `router.*` preferences, which Auspex's own
// engine never reads -- so it was a settings panel that changed nothing about the
// crew it claimed to configure. It now writes Auspex's own per-role models, which
// are what actually run.
//
// It also draws the pipeline, as ollamadev-qt's Brain pane does, with the stage a
// live run is in lit up. The faculty list is Auspex's OWN (see crew_faculties()):
// a part this engine does not have is drawn as missing rather than as working.
class BrainWindow : public Gtk::Window {
public:
    BrainWindow();

private:
    void reload();
    void save_models();
    void refresh_map();
    void show_usage();

    Gtk::Box   root_{Gtk::Orientation::VERTICAL, 10};
    Gtk::Label heading_;

    // One picker per ROLE, not per difficulty tier. Auspex's engine chooses by
    // role -- planning, coding and reviewing are different jobs -- and offering
    // difficulty tiers it does not use would be the same lie as before.
    Gtk::Grid  roles_;
    struct RoleRow {
        Gtk::Label    label;
        // WHICH AGENT, then which model. Backend first because it is the bigger
        // lever: a role on claude runs whatever frontier model its owner
        // configured, while a role on ollama is capped at what Ollama serves.
        Gtk::DropDown backends;
        Gtk::DropDown models;
        std::string   key;     // "director" | "coder" | "auditor"
    };
    std::vector<std::unique_ptr<RoleRow>> rows_;
    std::vector<std::string> models_;
    std::vector<std::string> backends_;
    bool loading_ = false;

    // The tiers the Router fills gaps from, and a probe that answers "where would
    // this go?" without running anything.
    Gtk::Label  tiers_heading_;
    Gtk::Grid   tiers_;
    struct TierRow {
        Gtk::Label    label;
        Gtk::DropDown models;
        std::string   tier;
    };
    std::vector<std::unique_ptr<TierRow>> tier_rows_;

    Gtk::Box    probe_row_{Gtk::Orientation::HORIZONTAL, 6};
    Gtk::Entry  probe_entry_;
    Gtk::Button probe_go_{"Route it"};
    Gtk::Label  probe_result_;
    void probe();

    // The pipeline, with the live stage marked.
    Gtk::Label          map_heading_;
    Gtk::ScrolledWindow map_scroller_;
    Gtk::Box            map_{Gtk::Orientation::VERTICAL, 2};
    std::vector<std::unique_ptr<Gtk::Widget>> map_rows_;
    std::string         last_active_;
    bool                have_map_ = false;

    Gtk::Label  tokens_;
    Gtk::Label  status_;
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
