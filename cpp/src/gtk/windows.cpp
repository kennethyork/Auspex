#include "auspex/gtk/windows.hpp"

#include <algorithm>
#include <thread>
#include <fstream>
#include <sstream>

#include <giomm/file.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>

#include <gdkmm/clipboard.h>

#include <nlohmann/json.hpp>

#include <glibmm/datetime.h>

#include <gtkmm/expression.h>
#include <gtkmm/stringobject.h>

#include "auspex/audio.hpp"
#include "auspex/autostart.hpp"
#include "auspex/crew.hpp"
#include "auspex/cli_coder.hpp"
#include "auspex/crew_run.hpp"
#include "auspex/eval.hpp"
#include "auspex/hooks.hpp"
#include "auspex/watch.hpp"
#include "auspex/roles.hpp"
#include "auspex/usage.hpp"
#include "auspex/gtk/voice.hpp"
#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"
#include "auspex/router.hpp"
#include "auspex/theme.hpp"
#include "auspex/tts.hpp"

using json = nlohmann::json;

namespace auspex::gtk {

namespace {

std::vector<std::string> split_words(const std::string& command) {
    std::vector<std::string> words;
    std::istringstream in(command);
    std::string word;
    while (in >> word) words.push_back(word);
    return words;
}

}  // namespace

// ---------------------------------------------------------------------------
// LauncherWindow
// ---------------------------------------------------------------------------
LauncherWindow::LauncherWindow(const Config& config) : config_(config) {
    set_title("Auspex Launcher");
    // Opts this window into the shell's translucency. See the .auspex-window rule
    // in theme.cpp: the glass is the window and its layout boxes, never the rows.
    add_css_class("auspex-window");
    set_default_size(520, 560);

    // NOT modal. A modal window needs a transient parent to mean anything, and the
    // only parent available is the panel -- which is a _NET_WM_WINDOW_TYPE_DOCK
    // window that never takes focus. Declaring modality against a parent that
    // cannot hold focus left the launcher in a state where the window manager gave
    // it focus, immediately took it back, and the window closed itself about two
    // seconds later without the user touching anything.
    set_modal(false);

    // Hidden rather than destroyed on close, and re-shown on the next open. The
    // 148 desktop files are then parsed once per session instead of once per
    // launcher open, and a stray close request cannot take the object out from
    // under the panel's unique_ptr.
    set_hide_on_close(true);

    search_.set_placeholder_text("Search applications...");
    search_.signal_changed().connect([this] { refilter(); });

    // Enter launches the selected row, so typing then Enter is the whole flow.
    search_.signal_activate().connect([this] { launch_selected(); });

    list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    // One click launches. Stated explicitly rather than relying on the default,
    // because "click the app and nothing happens" is indistinguishable from the
    // launcher being broken.
    list_.set_activate_on_single_click(true);
    list_.signal_row_activated().connect(
        [this](Gtk::ListBoxRow*) { launch_selected(); });

    scroller_.set_child(list_);
    scroller_.set_vexpand(true);
    scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);

    root_.set_margin(10);
    root_.append(search_);
    root_.append(scroller_);
    set_child(root_);

    // Escape closes, matching launcher.py's dismiss-on-focus-loss intent without
    // the focus-follows-mouse surprises that behaviour causes on some WMs.
    auto escape = Gtk::EventControllerKey::create();
    escape->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) {
                close();
                return true;
            }
            return false;
        },
        false);
    add_controller(escape);

    entries_ = load_desktop_entries();
    refilter();

    // Focus the search box on MAP, not here.
    //
    // grab_focus() on a widget whose window has not been realised does nothing at
    // all -- it does not queue, it does not warn, it silently fails. Called from
    // the constructor, as it was, the entry never held focus, so typing went to
    // whatever the window manager thought was focused instead and the list never
    // filtered. The window looked completely normal; the search simply did not
    // respond.
    //
    // signal_map fires once the window is on screen, which is the first moment
    // focus can actually be taken.
    signal_map().connect([this] {
        search_.grab_focus();
        // Re-focus and clear on every re-open, since the window is now hidden
        // rather than destroyed and would otherwise come back with the last
        // search still in it.
        search_.set_text("");
    });
}

void LauncherWindow::refilter() {
    // Rows are rebuilt rather than shown/hidden: the list is a few hundred entries
    // at most and rebuilding keeps the visible_ index trivially correct.
    for (auto& row : rows_) list_.remove(*row);
    rows_.clear();
    visible_.clear();

    const std::string query = search_.get_text();
    for (const auto& entry : entries_) {
        if (!entry_matches(entry, query)) continue;

        // Escaped: an application's Name or Comment can legitimately contain & or
        // <, which would otherwise be parsed as Pango markup and drop the row.
        Glib::ustring markup =
            "<b>" + Glib::Markup::escape_text(entry.name) + "</b>";
        if (!entry.comment.empty()) {
            markup += "\n<small>" + Glib::Markup::escape_text(entry.comment) + "</small>";
        }

        // Managed: the row owns the label, so destroying the row destroys both and
        // there is no second lifetime to get wrong.
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_markup(markup);
        label->set_xalign(0.0f);
        label->set_margin(6);
        label->set_wrap(false);
        label->set_ellipsize(Pango::EllipsizeMode::END);

        auto row = std::make_unique<Gtk::ListBoxRow>();
        row->set_child(*label);

        list_.append(*row);
        visible_.push_back(&entry);
        rows_.push_back(std::move(row));

        if (visible_.size() >= 300) break;   // keep the list responsive
    }

    if (auto* first = list_.get_row_at_index(0)) list_.select_row(*first);
}

void LauncherWindow::launch_selected() {
    auto* row = list_.get_selected_row();
    if (!row) return;

    const int index = row->get_index();
    if (index < 0 || static_cast<std::size_t>(index) >= visible_.size()) return;

    const DesktopEntry& entry = *visible_[index];
    auto argv = split_words(entry.exec);
    if (argv.empty()) return;

    if (entry.terminal && !config_.terminal.empty()) {
        // Terminal=true entries are command-line programs and need a terminal to
        // be visible at all; -e is the portable-enough spelling across the
        // terminals Config::resolve_commands() picks from.
        auto wrapped = split_words(config_.terminal);
        wrapped.push_back("-e");
        wrapped.insert(wrapped.end(), argv.begin(), argv.end());
        argv = std::move(wrapped);
    }

    spawn_detached(argv);
    close();
}

// ---------------------------------------------------------------------------
// CalendarWindow
// ---------------------------------------------------------------------------
CalendarWindow::CalendarWindow() {
    set_title("Auspex Calendar");
    add_css_class("auspex-window");
    set_default_size(980, 660);

    events_ = EventStore::load();

    previous_.set_icon_name("pan-start-symbolic");
    previous_.set_has_frame(false);
    next_.set_icon_name("pan-end-symbolic");
    next_.set_has_frame(false);

    heading_.add_css_class("title-2");
    heading_.set_xalign(0.0f);
    heading_.set_hexpand(true);

    previous_.signal_clicked().connect([this] {
        int year = year_, month = month_ - 1;
        if (month < 1) { month = 12; --year; }
        show_month(year, month);
    });
    next_.signal_clicked().connect([this] {
        int year = year_, month = month_ + 1;
        if (month > 12) { month = 1; ++year; }
        show_month(year, month);
    });
    today_.signal_clicked().connect([this] { go_today(); });

    header_.set_margin(12);
    header_.append(previous_);
    header_.append(next_);
    header_.append(today_);
    header_.append(heading_);
    root_.append(header_);

    build_grid();

    grid_.set_hexpand(true);
    grid_.set_vexpand(true);
    body_.append(grid_);

    // ---- the day panel ----
    day_heading_.add_css_class("subtitle");
    day_heading_.set_xalign(0.0f);

    day_scroller_.set_child(day_box_);
    day_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    day_scroller_.set_vexpand(true);

    // Time first and narrow, title second and wide, which is the order they are
    // spoken in and the order they are read in.
    time_entry_.set_placeholder_text("09:00");
    time_entry_.set_max_width_chars(6);
    time_entry_.set_width_chars(6);
    title_entry_.set_placeholder_text("What is happening\u2026");
    title_entry_.set_hexpand(true);
    // Enter from either field adds it. Reaching for the mouse is most of the reason
    // people do not bother writing something down.
    time_entry_.signal_activate().connect([this] { add_event(); });
    title_entry_.signal_activate().connect([this] { add_event(); });
    add_.signal_clicked().connect([this] { add_event(); });

    entry_row_.append(time_entry_);
    entry_row_.append(title_entry_);
    entry_row_.append(add_);

    {
        std::vector<Glib::ustring> labels;
        for (const auto& [value, label] : repeat_choices()) {
            repeat_values_.push_back(value);
            labels.emplace_back(label);
        }
        repeat_.set_model(Gtk::StringList::create(labels));
        repeat_.set_selected(0);
        repeat_.set_hexpand(true);
    }
    until_entry_.set_placeholder_text("until (optional)");
    until_entry_.set_max_width_chars(14);
    until_entry_.signal_activate().connect([this] { add_event(); });
    // The end date is meaningless without a rule, so it only appears once one is
    // chosen rather than sitting there inert.
    repeat_.property_selected().signal_changed().connect([this] {
        const auto index = repeat_.get_selected();
        const bool repeating = index < repeat_values_.size() &&
                               repeat_values_[index] != Repeat::None;
        until_entry_.set_visible(repeating);
    });
    until_entry_.set_visible(false);

    repeat_row_.append(repeat_);
    repeat_row_.append(until_entry_);

    day_panel_.set_margin(12);
    day_panel_.set_size_request(300, -1);
    day_panel_.append(day_heading_);
    day_panel_.append(day_scroller_);
    day_panel_.append(entry_row_);
    day_panel_.append(repeat_row_);
    body_.append(day_panel_);

    root_.append(body_);
    set_child(root_);

    go_today();
}

void CalendarWindow::build_grid() {
    grid_.set_row_homogeneous(true);
    grid_.set_column_homogeneous(true);

    static const char* names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (int column = 0; column < 7; ++column) {
        auto* label = Gtk::make_managed<Gtk::Label>(names[column]);
        label->add_css_class("subtitle");
        label->set_margin_bottom(4);
        grid_.attach(*label, column, 0, 1, 1);
    }
    // The weekday row must not stretch with the day rows, or it takes a sixth of
    // the height for one word.
    grid_.set_row_homogeneous(false);
    grid_.set_vexpand(true);

    cells_.resize(42);
    for (int i = 0; i < 42; ++i) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        auto* number = Gtk::make_managed<Gtk::Label>();
        auto* detail = Gtk::make_managed<Gtk::Label>();

        number->set_xalign(0.0f);
        detail->set_xalign(0.0f);
        detail->set_wrap(true);
        detail->set_max_width_chars(16);
        detail->add_css_class("subtitle");
        detail->set_vexpand(true);
        detail->set_valign(Gtk::Align::START);

        box->set_margin(4);
        box->append(*number);
        box->append(*detail);

        auto* button = Gtk::make_managed<Gtk::Button>();
        button->set_child(*box);
        button->set_has_frame(false);
        button->set_hexpand(true);
        button->set_vexpand(true);
        const int index = i;
        button->signal_clicked().connect([this, index] {
            if (!cells_[index].date.empty()) select_day(cells_[index].date);
        });

        grid_.attach(*button, i % 7, 1 + i / 7, 1, 1);
        cells_[i] = {button, box, number, detail, ""};
    }
}

void CalendarWindow::show_month(int year, int month) {
    year_  = year;
    month_ = month;
    heading_.set_text(month_name(month) + " " + std::to_string(year));

    const auto grid   = month_grid(year, month);
    const auto counts = events_.counts_in_month(year, month);

    for (std::size_t i = 0; i < cells_.size() && i < grid.size(); ++i) {
        auto& cell = cells_[i];
        cell.date = grid[i].date;

        // The day number alone; the date is already in the heading.
        cell.number->set_text(std::to_string(std::stoi(grid[i].date.substr(8, 2))));

        // Days from the months either side are shown but dimmed. Hiding them would
        // leave holes in the first and last weeks; showing them at full weight makes
        // the month's own edges impossible to find.
        if (grid[i].in_month) {
            cell.button->remove_css_class("subtitle");
            cell.button->set_opacity(1.0);
        } else {
            cell.button->set_opacity(0.45);
        }

        // What is on the day, in the cell. Two entries then a count, because a cell
        // that lists eight things is a cell you cannot read at a glance.
        const auto day = events_.on(grid[i].date);
        std::string detail;
        for (std::size_t n = 0; n < day.size() && n < 2; ++n) {
            if (!detail.empty()) detail += "\n";
            const auto& event = day[n].event;
            detail += event.start.empty() ? event.title
                                          : event.start + "  " + event.title;
        }
        if (day.size() > 2) {
            detail += "\n+" + std::to_string(day.size() - 2) + " more";
        }
        cell.detail->set_text(detail);

        if (grid[i].date == selected_) {
            cell.button->add_css_class("active-workspace");
        } else {
            cell.button->remove_css_class("active-workspace");
        }
    }
    (void)counts;
}

void CalendarWindow::select_day(const std::string& date) {
    selected_ = date;

    // Clicking a day in the trailing or leading week moves to that month, which is
    // what clicking it plainly means.
    const int year  = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    if (year != year_ || month != month_) {
        show_month(year, month);
    } else {
        show_month(year_, month_);   // repaint, to move the selection highlight
    }

    reload_day();
}

void CalendarWindow::reload_day() {
    while (Gtk::Widget* child = day_box_.get_first_child()) day_box_.remove(*child);
    day_rows_.clear();

    day_heading_.set_text(selected_);

    const auto day = events_.on(selected_);
    if (day.empty()) {
        auto* empty = Gtk::make_managed<Gtk::Label>("Nothing on this day");
        empty->add_css_class("subtitle");
        empty->set_xalign(0.0f);
        day_box_.append(*empty);
        return;
    }

    for (std::size_t i = 0; i < day.size(); ++i) {
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

        auto* when = Gtk::make_managed<Gtk::Label>(
            day[i].event.start.empty() ? "All day" : day[i].event.start);
        when->set_xalign(0.0f);
        when->set_size_request(64, -1);
        when->add_css_class("subtitle");

        // A repeat is marked, because deleting one deletes the whole series and
        // that is not something to discover afterwards.
        auto* what = Gtk::make_managed<Gtk::Label>(
            day[i].repeating ? day[i].event.title + "  \u21bb" : day[i].event.title);
        what->set_xalign(0.0f);
        what->set_hexpand(true);
        what->set_wrap(true);
        what->set_max_width_chars(24);

        // Removal is per event and immediate. A one-line entry is not worth a
        // confirmation dialog, and the file behind it is plain JSON.
        auto* remove = Gtk::make_managed<Gtk::Button>();
        remove->set_icon_name("edit-delete-symbolic");
        remove->set_has_frame(false);
        remove->set_tooltip_text(day[i].repeating ? "Remove every occurrence"
                                                  : "Remove");
        // Removal goes through the occurrence's ORIGIN, not the day being viewed: a
        // repeat shown on the 14th is stored on the 1st, and removing "the 14th"
        // would find nothing there.
        const std::string origin = day[i].origin;
        const std::size_t index  = day[i].index;
        remove->signal_clicked().connect([this, origin, index] {
            if (events_.remove(origin, index)) {
                events_.save();
                show_month(year_, month_);
                reload_day();
            }
        });

        row->append(*when);
        row->append(*what);
        row->append(*remove);
        day_box_.append(*row);
        day_rows_.push_back(std::move(row));
    }
}

void CalendarWindow::add_event() {
    CalendarEvent event;
    event.start = std::string(time_entry_.get_text());
    event.title = std::string(title_entry_.get_text());

    const auto choice = repeat_.get_selected();
    if (choice < repeat_values_.size()) event.repeat = repeat_values_[choice];
    if (event.repeat != Repeat::None) {
        event.until = std::string(until_entry_.get_text());
    }

    if (!events_.add(selected_, event)) {
        // The two ways this fails are worth telling apart: a blank title is a slip,
        // a malformed time is a typo with a specific fix.
        day_heading_.set_text(is_valid_time(event.start)
                                  ? selected_ + "  \u2014  needs a title"
                                  : selected_ + "  \u2014  time must be HH:MM");
        return;
    }

    // Saved as it is entered. An event only in memory is an event lost to the next
    // restart, and nothing here is worth a Save button.
    events_.save();
    time_entry_.set_text("");
    title_entry_.set_text("");
    until_entry_.set_text("");
    // The rule is left as it was: entering three weekly things in a row should not
    // mean choosing "every week" three times.

    show_month(year_, month_);
    reload_day();
}

void CalendarWindow::go_today() {
    const auto now = Glib::DateTime::create_now_local();
    const std::string today = format_date(now.get_year(), now.get_month(),
                                          now.get_day_of_month());
    selected_ = today;
    show_month(now.get_year(), now.get_month());
    reload_day();
}

// ---------------------------------------------------------------------------
// CrewWindow
// ---------------------------------------------------------------------------
CrewWindow::CrewWindow() {
    set_title("Auspex Crew");
    add_css_class("auspex-window");
    // 960, not 720. --check-windows measures this window's content at 888px with
    // the tuning open, so the old default clipped out of the box -- three switches
    // and part of the role grid unreachable on first run, which nobody would read
    // as anything but broken.
    set_default_size(960, 780);
    set_hide_on_close(true);

    // ---- where ----
    //
    // First, because it is the question the rest of the window depends on and the
    // one that used to have no answer. See windows.hpp for what happened when a
    // run was started without it.
    if (const auto initial = default_project()) project_ = initial->path;
    project_label_.set_xalign(0.0f);
    project_label_.set_hexpand(true);
    project_label_.set_ellipsize(Pango::EllipsizeMode::START);   // keep the leaf visible
    project_pick_.signal_clicked().connect([this] { choose_project(); });
    project_row_.append(project_label_);
    project_row_.append(project_pick_);

    // ---- the task ----
    task_label_.set_text("What should the crew build?");
    task_label_.set_xalign(0.0f);
    task_.set_placeholder_text("add rate limiting to the api…");
    task_.set_hexpand(true);
    // Enter starts it. A task is one line and reaching for the mouse to send it is
    // friction on the thing this window exists to do.
    task_.signal_activate().connect([this] { start(); });

    start_.add_css_class("suggested-action");
    start_.signal_clicked().connect([this] { start(); });

    resume_.set_tooltip_text(
        "Recover an interrupted run: capture what its coders wrote, audit it, and "
        "land or hold it. No coder is restarted and no plan is remade.");
    resume_.signal_clicked().connect([this] {
        if (running_.load()) {
            status_.set_text("A run is going. Stop it first.");
            return;
        }
        if (!is_project_dir(project_)) {
            status_.set_text("Choose a folder first");
            return;
        }
        if (resumable_runs().empty()) {
            status_.set_text("There is no interrupted run to resume");
            return;
        }

        if (runner_.joinable()) runner_.join();
        running_.store(true);
        start_.set_sensitive(false);
        resume_.set_sensitive(false);
        status_.set_text("Recovering the last interrupted run\u2026");

        const Config config = Config::load();
        const auto   project = project_;
        runner_ = std::thread([this, config, project] {
            RunEvents events;
            events.log = [this](const std::string& line) {
                {
                    std::lock_guard lock(log_mutex_);
                    last_log_ = line;
                }
                run_changed_.emit();
            };
            const RunResult result = resume_crew(config, project, {}, events);
            {
                std::lock_guard lock(log_mutex_);
                last_log_ = result.error.empty()
                                ? std::to_string(result.applied) + " applied \u00b7 " +
                                      std::to_string(result.held) + " held"
                                : result.error;
            
                // What it cost, per model. The engine has metered this all along
                // and the window never showed it, so a run on a metered model
                // looked exactly like a run on a local one.
                last_cost_ = usage_report(result.usage);
            }
            running_.store(false);
            run_changed_.emit();
        });
    });
    resume_.signal_clicked().connect([this] {
        if (!is_project_dir(project_)) {
            status_.set_text("Choose a folder for the crew to work in first");
            return;
        }
        if (!spawn_detached(crew_resume_command(), project_.string())) {
            status_.set_text("Could not reach the crew");
            return;
        }
        status_.set_text("Resuming the most recent run…");
    });

    stop_.set_sensitive(false);
    stop_.set_tooltip_text("Stop after the current step, keeping what is done");
    stop_.signal_clicked().connect([this] { stop(); });

    start_row_.append(start_);
    start_row_.append(stop_);
    start_row_.append(resume_);

    // The runner signals; the UI re-reads. Both watchers are forced because the
    // state file changed for a reason we already know about, so waiting on a
    // modification time would only add latency.
    run_changed_.connect([this] {
        {
            std::lock_guard lock(log_mutex_);
            if (!last_log_.empty()) status_.set_text(last_log_);
            // Shown only once there is something to show. A run that spent
            // nothing -- one that failed before planning -- leaves this hidden
            // rather than printing an empty table.
            if (!last_cost_.empty()) {
                cost_.set_text(last_cost_);
                cost_.set_visible(true);
            }
        }
        if (!running_.load()) {
            start_.set_sensitive(true);
            stop_.set_sensitive(false);
            resume_.set_sensitive(!resumable_runs().empty());
        }
        have_run_mtime_   = false;
        have_board_mtime_ = false;
        refresh_run();
        refresh_board();
    });

    // ---- the options ----
    // Per-role models are a setting rather than a switch now: crew_director_model,
    // crew_coder_model and crew_auditor_model in config.json. The measured
    // difference is large enough that it belongs in config, not behind a tickbox.
    route_.set_tooltip_text(
        "Pick each coder's model by how hard its piece looks, filling in only where "
        "a role was left on Default. Set the tiers in the Brain window.");
    debate_.set_tooltip_text(
        "An advocate, a skeptic and a judge argue over every changeset. Three model "
        "calls per piece instead of one.");
    // Dedupe is not a switch here: overlapping work is ALWAYS held, because
    // silently letting one coder overwrite another's approved change is not a mode
    // worth offering. Ticked and locked, so the behaviour is visible.
    dedupe_.set_active(true);
    dedupe_.set_sensitive(false);
    dedupe_.set_tooltip_text("Always on: overlapping work is held, never overwritten");
    learn_.set_tooltip_text(
        "Remember why work was held, and put it in front of the next run's coders");
    security_.set_tooltip_text(
        "A read-only vulnerability hunt instead of building anything. Nothing is "
        "written; the result is a report.");
    verify_.set_tooltip_text(
        "After each coder, run this project's tests in its sandbox and hand the "
        "failures back. This also lets coders run commands, because a test suite "
        "IS code -- and a coder that broke the suite is told while it can still "
        "fix it.");
    starters_.set_active(true);
    starters_.set_tooltip_text(
        "Write the shipped skills that match this task into .auspex/skills. "
        "Anything you have written yourself is never overwritten.");

    options_.set_selection_mode(Gtk::SelectionMode::NONE);
    options_.set_max_children_per_line(5);
    options_.set_row_spacing(2);
    options_.set_column_spacing(14);

    options_.insert(route_, -1);
    options_.insert(debate_, -1);
    options_.insert(dedupe_, -1);
    options_.insert(learn_, -1);
    options_.insert(security_, -1);
    branch_.set_tooltip_text(
        "Land each coder on its own branch, crew/<run>/<n>-<title>, instead of in "
        "your working tree. One coder's work can then be reviewed, cherry-picked "
        "or thrown away without touching the others. Your working tree, index and "
        "current branch are left exactly as they are.");
    commit_.set_tooltip_text(
        "Commit what lands, with a message naming the run and the task. Only the "
        "paths this run changed are staged, so work you have in progress is not "
        "swept in.");
    watch_.set_tooltip_text(
        "After the run, keep watching: run the task again whenever the tree "
        "settles. Changes the crew itself makes do not count as a reason to run "
        "again.");

    // Branch already commits, so Commit alongside it is a switch that does
    // nothing. Disabled rather than ignored -- a control that silently has no
    // effect is worse than one that is plainly unavailable.
    const auto sync_commit = [this] {
        commit_.set_sensitive(!branch_.get_active());
        if (branch_.get_active()) commit_.set_active(false);
    };
    branch_.signal_toggled().connect(sync_commit);
    sync_commit();

    options_.insert(verify_, -1);
    options_.insert(starters_, -1);
    options_.insert(branch_, -1);
    options_.insert(commit_, -1);
    options_.insert(watch_, -1);

    // The gates you have installed, if any. A hook you have forgotten is worse
    // than no hook, and nothing else in the interface mentions them.
    if (const auto hooks = load_hooks(); !hooks.empty()) {
        hooks_note_.set_text(std::to_string(hooks.size()) + " hook" +
                             (hooks.size() == 1 ? "" : "s") + " will gate this run");
        hooks_note_.set_tooltip_text(render_hooks(hooks));
    } else {
        hooks_note_.set_text("No hooks configured");
        hooks_note_.set_tooltip_text("Gates of your own live in " +
                                     hooks_path().string());
    }
    hooks_note_.set_xalign(0.0f);
    hooks_note_.add_css_class("subtitle");

    // The roles this crew may use, one box each, all ticked.
    //
    // Built from all_personas(), so a role you write into your own crew-roles
    // directory appears here without a line of GUI code -- the same way the pack
    // picker gained seven packs for free.
    // The label says what the widget SHOWS. It used to say "blank is no limit"
    // while the spinner read -1, which is a label contradicting the thing beside
    // it -- small, and the sort of wrongness that makes a person distrust the
    // rest of the window.
    roles_label_.set_text("How many of each role  (0 leaves it to the Director)");
    roles_label_.set_xalign(0.0f);
    roles_label_.add_css_class("subtitle");

    // Built from all_personas(), so a role you write into your own crew-roles
    // directory appears here without a line of GUI code -- the same way the pack
    // picker gained seven packs for free.
    // Three pairs per row, so nine roles are three tidy rows at the width this
    // window is used at.
    constexpr int kRolesPerRow = 3;
    int role_index = 0;

    for (const auto& persona : all_personas()) {
        const int column = (role_index % kRolesPerRow) * 2;
        const int row    = role_index / kRolesPerRow;
        ++role_index;

        auto* name = Gtk::make_managed<Gtk::Label>(persona.name);
        name->add_css_class("caption");
        // LEFT, like "Coders" and "Swarm" in the row below. The first column of
        // this grid starts at the same edge those labels do, so left-aligning
        // puts every role name on the one vertical line the rest of the window
        // already uses. Right-aligned looked tidy on its own and was out of step
        // with everything under it.
        name->set_xalign(0.0f);

        auto* count = Gtk::make_managed<Gtk::SpinButton>();
        // Starts at 0, like every other number in this window, and means the same
        // thing there: leave it alone.
        count->set_range(0, 12);
        count->set_increments(1, 1);
        count->set_value(0);
        count->set_width_chars(3);

        std::string tip = persona.description;
        if (persona.read_only) tip += " (never edits)";
        if (persona.custom) tip += "  — your own role";
        tip += "\n0 leaves it to the Director · N means at most N pieces";
        count->set_tooltip_text(tip);
        name->set_tooltip_text(tip);

        roles_row_.attach(*name, column, row);
        roles_row_.attach(*count, column + 1, row);

        role_counts_.push_back(count);
        role_names_.push_back(persona.name);
    }

    coders_label_.set_text("Coders");
    coders_.set_range(0, 12);
    coders_.set_increments(1, 2);
    // Zero shows as "default" rather than as a cap of no coders, and is what leaves
    // the engine's own number alone.
    coders_.set_value(0);
    coders_.set_tooltip_text("--max: how many pieces the Director may plan. "
                             "0 leaves ollamadev's own default alone");

    swarm_label_.set_text("Swarm");
    swarm_.set_range(0, 24);
    swarm_.set_increments(1, 4);
    swarm_.set_value(0);
    swarm_.set_tooltip_text("--swarm: raise the cap on coders running at once. 0 off");

    amplify_label_.set_text("Amplify");
    amplify_.set_range(0, 7);
    amplify_.set_increments(1, 2);
    amplify_.set_value(0);
    amplify_.set_tooltip_text(
        "N Director plans, keeping the shape most agree on, and N reviewers voting "
        "on every changeset. The most expensive option here: it multiplies both "
        "ends of the run. 0 is off.");

    pack_label_.set_text("Pack");
    pack_.set_tooltip_text("Start from a saved set of options; the switches above "
                           "still win");
    packs_.clear();
    for (const auto& pack : builtin_packs()) packs_.push_back(pack.name);
    {
        std::vector<Glib::ustring> labels;
        labels.emplace_back("None");
        for (const auto& pack : packs_) labels.emplace_back(pack);
        pack_.set_model(Gtk::StringList::create(labels));
        pack_.set_selected(0);
    }
    pack_.set_tooltip_text("Start from a saved team; the switches above still win");

    second_row_.append(coders_label_);
    second_row_.append(coders_);
    second_row_.append(swarm_label_);
    second_row_.append(swarm_);
    second_row_.append(amplify_label_);
    second_row_.append(amplify_);
    second_row_.append(pack_label_);
    second_row_.append(pack_);

    // ---- what it is doing ----
    run_heading_.set_xalign(0.0f);
    run_heading_.add_css_class("subtitle");

    {
        const auto build_lane = [](Lane& lane, const char* title) {
            lane.title.set_text(title);
            lane.title.set_xalign(0.0f);
            lane.title.add_css_class("subtitle");
            lane.scroller.set_child(lane.body);
            lane.scroller.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
            lane.scroller.set_min_content_height(90);
            lane.scroller.set_max_content_height(180);
            lane.scroller.set_propagate_natural_height(true);
            lane.column.append(lane.title);
            lane.column.append(lane.scroller);
            lane.column.set_hexpand(true);
        };
        build_lane(todo_,  "To do");
        build_lane(doing_, "Doing");
        build_lane(done_,  "Done");
        build_lane(held_,  "Held");

        lanes_.append(todo_.column);
        lanes_.append(doing_.column);
        lanes_.append(done_.column);
        lanes_.append(held_.column);
        // Shown only when something is held; see windows.hpp.
        held_.column.set_visible(false);
    }

    // Steering a coder while it works. The instruction is your words, sent as one
    // argument -- the model never composes what goes to a running coder.
    steer_.set_placeholder_text("Tell the coder that is running something\u2026");
    steer_.set_hexpand(true);
    steer_.signal_activate().connect([this] { steer_send_.activate(); });
    steer_send_.signal_clicked().connect([this] {
        const std::string text = trim(std::string(steer_.get_text()));
        if (text.empty()) {
            status_.set_text("Say what to tell the coder first");
            return;
        }

        // Aimed at the coder that is actually running. A held one has stopped and
        // has nowhere to receive a message.
        const CrewRun run = current_crew_run(auspex_run_state_path());
        const auto target = crew_current_subtask(run);
        if (!target) {
            status_.set_text("\u26a0 nothing is running to steer");
            return;
        }

        std::string error;
        if (!steer_coder(target->n, text, &error)) {
            status_.set_text("\u26a0 " + (error.empty() ? std::string("could not steer")
                                                         : error));
            return;
        }
        status_.set_text("\u2713 told coder " + std::to_string(target->n) +
                         " \u2014 it will see this at its next step");
        steer_.set_text("");
    });

    steer_row_.append(steer_);
    steer_row_.append(steer_send_);

    // ---- what it is holding ----
    board_heading_.set_xalign(0.0f);
    board_heading_.add_css_class("subtitle");
    board_scroller_.set_child(board_box_);
    board_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    board_scroller_.set_vexpand(true);

    status_.set_xalign(0.0f);
    status_.add_css_class("subtitle");
    status_.set_wrap(true);

    root_.set_margin(14);
    // Breathing room. The window was edge-to-edge, which is what made a long
    // stack of controls read as one undifferentiated block.
    root_.set_margin(14);

    root_.append(project_row_);
    root_.append(task_label_);
    root_.append(task_);

    // Everything below here is tuning, and it goes behind one disclosure.
    tuning_box_.append(options_);
    roles_row_.set_row_spacing(6);
    roles_row_.set_column_spacing(8);
    roles_row_.set_column_homogeneous(true);
    tuning_box_.append(roles_label_);
    tuning_box_.append(roles_row_);
    tuning_box_.append(second_row_);
    tuning_box_.append(hooks_note_);
    tuning_box_.set_margin_top(6);
    tuning_box_.set_margin_start(4);
    tuning_.set_child(tuning_box_);
    tuning_.set_expanded(false);   // shut, or the disclosure discloses nothing
    tuning_.set_margin_top(4);
    root_.append(tuning_);

    start_row_.set_margin_top(4);
    root_.append(start_row_);
    run_heading_.set_margin_top(10);
    root_.append(run_heading_);
    crew_row_.set_visible(false);
    root_.append(crew_row_);
    root_.append(lanes_);
    root_.append(steer_row_);
    board_heading_.set_margin_top(10);
    root_.append(board_heading_);
    root_.append(board_scroller_);
    root_.append(status_);

    cost_.set_xalign(0.0f);
    cost_.add_css_class("dim-label");
    cost_.set_visible(false);   // nothing to say until a run has finished
    root_.append(cost_);

    // Vertical only. Horizontal scrolling would let the lanes run off the side
    // rather than narrowing, which turns a readable three-column board into
    // something you have to drag around.
    root_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    root_scroller_.set_child(root_);
    root_scroller_.set_vexpand(true);
    set_child(root_scroller_);

    show_project();
    refresh_run();
    refresh_board();

    // Two seconds, and two stats when nothing has moved.
    Glib::signal_timeout().connect(
        [this] {
            refresh_run();
            refresh_board();
            return true;
        },
        2000);
}

// ---------------------------------------------------------------------------
void CrewWindow::show_project() {
    if (project_.empty()) {
        project_label_.set_markup("<b>No folder chosen</b> — the crew has nowhere to work");
        project_label_.set_tooltip_text({});
        return;
    }

    // The name in bold and the full path beside it. The name is what you recognise;
    // the path is what tells you it is the copy you meant, and for something that
    // edits files both belong on screen at once.
    const std::string leaf = project_.filename().empty()
                                 ? project_.string()
                                 : project_.filename().string();
    project_label_.set_markup("Working in <b>" + Glib::Markup::escape_text(leaf) +
                              "</b>  <span alpha='60%'>" +
                              Glib::Markup::escape_text(project_.string()) + "</span>");
    project_label_.set_tooltip_text(project_.string());

    if (!is_project_dir(project_)) {
        project_label_.set_markup("<b>" + Glib::Markup::escape_text(project_.string()) +
                                  "</b> is gone");
    }
}

void CrewWindow::set_project(const std::filesystem::path& path) {
    if (!is_project_dir(path)) return;
    project_ = normal_project_path(path);
    remember_project(project_);
    show_project();

    // Both watchers forced, so the run and the board are re-read against the folder
    // just chosen rather than left showing the previous one until a file happens to
    // change.
    have_run_mtime_   = false;
    have_board_mtime_ = false;
    refresh_run();
    refresh_board();
}

void CrewWindow::choose_project() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Which folder should the crew work in?");
    if (is_project_dir(project_)) {
        dialog->set_initial_folder(Gio::File::create_for_path(project_.string()));
    }
    dialog->select_folder(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            if (const auto folder = dialog->select_folder_finish(result)) {
                set_project(folder->get_path());
                status_.set_text({});
            }
        } catch (const Glib::Error&) {
            // Cancelled, which is not an error worth reporting.
        }
    });
}

void CrewWindow::start() {
    // WHERE first. Everything below is about what to build; with no answer to
    // where, none of it should run.
    if (!is_project_dir(project_)) {
        status_.set_text(project_.empty()
                             ? "Choose a folder for the crew to work in first"
                             : project_.string() + " is not a folder any more");
        return;
    }
    if (running_.load()) {
        status_.set_text("A run is already going. Stop it first.");
        return;
    }

    const std::string task = trim(std::string(task_.get_text()));
    if (task.empty()) {
        status_.set_text("Give the crew something to do first");
        return;
    }

    const Config config = Config::load();

    RunOptions options;
    options.project      = project_;
    options.task         = task;
    options.max_subtasks = coders_.get_value_as_int() > 0
                               ? coders_.get_value_as_int()
                               : 4;
    // Swarm raises how many run AT ONCE; the coder cap bounds how many are
    // planned. Two numbers, as in the engine.
    options.parallel = swarm_.get_value_as_int() > 0 ? swarm_.get_value_as_int()
                                                     : options.max_subtasks;
    // A pack first, so the switches below override it rather than the other way
    // round -- a pack is a starting point, not a lock.
    if (const auto index = pack_.get_selected();
        index != GTK_INVALID_LIST_POSITION && index > 0 && index <= packs_.size()) {
        if (const auto pack = find_pack(packs_[index - 1])) {
            const auto project = options.project;
            const auto task    = options.task;
            options = pack->options;
            options.project = project;
            options.task    = task;
        }
    }

    options.route    = route_.get_active();
    options.debate   = debate_.get_active();
    options.learn    = learn_.get_active();
    options.security = security_.get_active();
    options.starter_skills = starters_.get_active();

    // Only the numbers you actually set. 0 is "leave it alone", which is the same
    // as saying nothing and one fewer thing for the engine to carry.
    for (std::size_t i = 0; i < role_counts_.size(); ++i) {
        if (const int value = role_counts_[i]->get_value_as_int(); value > 0) {
            options.role_limits[role_names_[i]] = value;
        }
    }

    // ONE switch, not two. verify_attempts without allow_run runs nothing, so a
    // person ticking "Run the tests" and getting silence would be right to think
    // the setting was broken. Ticking it here turns on the ability it needs.
    if (verify_.get_active()) {
        options.verify_attempts = 2;
        options.coder.allow_run = true;
    }
    options.branch_per_coder = branch_.get_active();
    options.commit           = commit_.get_active();
    // Read on the GTK thread and handed to the worker, rather than the worker
    // reaching back into a widget it must not touch.
    watch_now_ = watch_.get_active();
    if (amplify_.get_value_as_int() > 0) options.amplify = amplify_.get_value_as_int();

    // Straight across, whatever roles exist. The engine's fallback chain does the
    // rest, so a role left unset borrows from whatever it falls back to.
    options.role_models   = config.crew_role_models;
    options.role_backends = config.crew_role_backends;

    const auto backend_of = [&config](const char* role) {
        const auto found = config.crew_role_backends.find(role);
        return (found == config.crew_role_backends.end() || found->second.empty())
                   ? std::string("ollama")
                   : found->second;
    };
    options.researcher_backend = backend_of("researcher");
    options.director_backend   = backend_of("director");
    options.coder_backend      = backend_of("coder");
    options.auditor_backend    = backend_of("auditor");

    // A previous thread must be reaped before another starts, or two runs write
    // one state file.
    if (runner_.joinable()) runner_.join();

    cancel_.store(false);
    running_.store(true);
    start_.set_sensitive(false);
    stop_.set_sensitive(true);
    task_.set_text("");
    status_.set_text("Started in " + project_.string() + ".");

    runner_ = std::thread([this, config, options] {
        RunEvents events;
        events.changed = [this] { run_changed_.emit(); };
        events.log = [this](const std::string& line) {
            {
                std::lock_guard lock(log_mutex_);
                last_log_ = line;
            }
            run_changed_.emit();
        };

        // Keep watching, or run once.
        //
        // watch_project runs the crew itself and blocks until told to stop, so it
        // takes the same cancel token the Stop button already sets -- the button
        // does not need to know which of the two it is stopping.
        RunResult result;
        if (watch_now_) {
            WatchOptions watching;
            watching.project = options.project;
            watching.task    = options.task;

            WatchEvents watch_events;
            watch_events.log = events.log;
            const int runs = watch_project(config, watching, watch_events, &cancel_);
            result.applied = 0;
            result.error = runs == 0 ? "watching stopped without running"
                                     : std::to_string(runs) + " run(s) while watching";
        } else {
            result = run_crew(config, options, events, &cancel_);
        }
        {
            std::lock_guard lock(log_mutex_);
            last_log_ = result.error.empty()
                            ? std::to_string(result.applied) + " applied \u00b7 " +
                                  std::to_string(result.held) + " held"
                            : result.error;
        
            // What it cost, per model. The engine has metered this all along
            // and the window never showed it, so a run on a metered model
            // looked exactly like a run on a local one.
            last_cost_ = usage_report(result.usage);
        }
        running_.store(false);
        run_changed_.emit();
    });
}

CrewWindow::~CrewWindow() {
    // Ask, then wait. The runner polls `cancel` between steps, so this blocks for
    // at most one model call -- unpleasant on quit, and far better than detaching
    // a thread that would go on writing a state file and emitting into a
    // Dispatcher owned by a window that no longer exists.
    cancel_.store(true);
    if (runner_.joinable()) runner_.join();
}

void CrewWindow::stop() {
    if (!running_.load()) return;
    // Polled between steps rather than killing anything: a coder mid-call finishes
    // its turn, and whatever was already written stays in its sandbox. Stopping a
    // crew should not throw away the work it has done.
    cancel_.store(true);
    status_.set_text("Stopping after the current step\u2026");
    stop_.set_sensitive(false);
}

void CrewWindow::refresh_run() {
    // Auspex's own state file now, not ollamadev's. See crew_run.hpp for why the
    // two engines must not share one.
    const auto path = auspex_run_state_path();
    if (path.empty()) return;

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        run_heading_.set_text("No crew has run here yet.");
        lanes_.set_visible(false);
        steer_row_.set_visible(false);
        return;
    }
    if (have_run_mtime_ && stamp == run_mtime_) return;
    run_mtime_      = stamp;
    have_run_mtime_ = true;

    for (Lane* lane : {&todo_, &doing_, &done_, &held_}) {
        while (Gtk::Widget* child = lane->body.get_first_child()) lane->body.remove(*child);
    }
    run_rows_.clear();

    const CrewRun run = current_crew_run(path);
    const std::string label = crew_status_label(run);

    run_heading_.set_text(label.empty()
                              ? (run.task.empty() ? "The crew is idle."
                                                  : "Idle. Last task: " + run.task)
                              : label + " \u2014 " + run.task);
    // The crew, as members. Rebuilt rather than updated: four chips is nothing to
    // redraw, and keeping widgets in step with a changing roster is how a row ends
    // up showing a member that finished ten seconds ago.
    while (Gtk::Widget* child = crew_row_.get_first_child()) crew_row_.remove(*child);
    crew_chips_.clear();

    const auto members = crew_members(run);
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];

        auto chip = std::make_unique<Gtk::Label>();
        std::string text = member.name;
        if (!member.detail.empty()) text += "  " + member.detail;
        chip->set_text(text);
        chip->set_xalign(0.0f);
        // Working is the one that matters; done is dimmed; not yet reached is
        // ordinary. Three states rather than two, because "has not started" and
        // "has finished" look identical otherwise and mean opposite things.
        if (member.working)   chip->add_css_class("accent");
        else if (member.done) chip->add_css_class("dim-label");
        chip->add_css_class(member.working ? "heading" : "caption");
        crew_row_.append(*chip);
        crew_chips_.push_back(std::move(chip));

        if (i + 1 < members.size()) {
            auto arrow = std::make_unique<Gtk::Label>();
            arrow->set_text("\u2192");
            arrow->add_css_class("dim-label");
            crew_row_.append(*arrow);
            crew_chips_.push_back(std::move(arrow));
        }
    }
    crew_row_.set_visible(!members.empty());

    // Visible whenever there is a crew at all, not only once the Director has cut
    // the job up: the Researcher and the Director are working before any subtask
    // exists, and hiding the lanes until then is hiding the first half of a run.
    lanes_.set_visible(!run.subtasks.empty() || !members.empty());
    // Only offered while something is actually running. A steer box on an idle
    // crew is a control that can only report that there is nothing to steer.
    steer_row_.set_visible(run.active);

    int counts[4] = {0, 0, 0, 0};

    for (const auto& subtask : run.subtasks) {
        // The engine's own state words, via crew_lane_of(). This used to look for
        // "running", "active" and "working" -- none of which ollamadev writes -- so
        // a coder in flight sat in To do and the lanes never moved until it was
        // finished.
        Lane* lane  = &todo_;
        int   which = 0;
        switch (crew_lane_of(subtask)) {
            case CrewLane::Doing: lane = &doing_; which = 1; break;
            case CrewLane::Done:  lane = &done_;  which = 2; break;
            case CrewLane::Held:  lane = &held_;  which = 3; break;
            case CrewLane::Todo:  break;
        }
        ++counts[which];

        auto card = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        card->add_css_class("crew-card");
        switch (crew_lane_of(subtask)) {
            case CrewLane::Doing: card->add_css_class("crew-card-working"); break;
            case CrewLane::Done:  card->add_css_class("crew-card-done"); break;
            case CrewLane::Held:  card->add_css_class("crew-card-held"); break;
            case CrewLane::Todo:  break;
        }

        auto* title = Gtk::make_managed<Gtk::Label>(subtask.title);
        title->set_xalign(0.0f);
        title->set_wrap(true);
        title->set_max_width_chars(24);

        auto* who = Gtk::make_managed<Gtk::Label>(
            "#" + std::to_string(subtask.n) +
            (subtask.role.empty() ? "" : "  " + subtask.role));
        who->set_xalign(0.0f);
        who->add_css_class("subtitle");

        card->append(*who);
        card->append(*title);

        // Which model, and why that one. Without this --route is a switch you can
        // turn on and never observe: its entire purpose is giving each subtask a
        // model picked for its difficulty, and the choice was going straight into
        // the state file and no further.
        if (const std::string engine = crew_subtask_model_line(subtask); !engine.empty()) {
            auto* which_model = Gtk::make_managed<Gtk::Label>(engine);
            which_model->set_xalign(0.0f);
            which_model->set_wrap(true);
            which_model->add_css_class("subtitle");
            card->append(*which_model);
        }

        // What it is doing right now, and what it has changed so far.
        //
        // This is the line that turns "doing" into something worth watching:
        // "reading cpp/src/gtk/panel.cpp · +12 −3". Every step was already
        // recorded and none of it was published until the coder finished, which
        // is exactly when it stops being interesting.
        if (const std::string doing = crew_subtask_activity_line(subtask);
            !doing.empty()) {
            auto* activity = Gtk::make_managed<Gtk::Label>(doing);
            activity->set_xalign(0.0f);
            activity->set_wrap(true);
            activity->add_css_class("subtitle");
            // Marked while it is actually working, so a glance separates the
            // coder in flight from the three that have stopped.
            if (!subtask.activity.empty()) activity->add_css_class("accent");
            card->append(*activity);
        }

        // A steer box on the coder itself, as ollamadev-qt's CoderPane has. The
        // window already had one, but it auto-targeted the earliest running coder
        // -- fine with one in flight, guesswork with four. This says which.
        //
        // NOTE: a rebuild clears what is half-typed here, and the lanes rebuild
        // whenever the engine rewrites its state file. Steering is a sentence, not
        // an essay, so this is a real edge and a small one.
        if (lane == &doing_) {
            auto* row  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
            auto* say  = Gtk::make_managed<Gtk::Entry>();
            auto* send = Gtk::make_managed<Gtk::Button>("Steer");
            say->set_placeholder_text("say something to this coder…");
            say->set_hexpand(true);

            const int target = subtask.n;
            const auto steer_this = [this, target, say] {
                const std::string words = trim(std::string(say->get_text()));
                if (words.empty()) return;
                const auto argv = crew_steer_command(target, words);
                if (argv.empty() || !spawn_detached(argv, project_.string())) {
                    status_.set_text("⚠ could not reach coder " + std::to_string(target));
                    return;
                }
                status_.set_text("✓ steered coder " + std::to_string(target));
                say->set_text("");
            };
            say->signal_activate().connect(steer_this);
            send->signal_clicked().connect(steer_this);

            row->append(*say);
            row->append(*send);
            card->append(*row);
        } else if (lane == &done_) {
            auto* finished = Gtk::make_managed<Gtk::Label>("this coder has finished");
            finished->set_xalign(0.0f);
            finished->add_css_class("subtitle");
            card->append(*finished);
        }

        // The one in flight is marked, so the Doing lane still reads at a glance
        // when it holds several.
        if (lane == &doing_) card->add_css_class("recording");

        lane->body.append(*card);
        run_rows_.push_back(std::move(card));
    }

    // THE REST OF THE CREW, as cards in the lanes.
    //
    // The lanes held coder subtasks and nothing else, so a run of five faculties
    // read as a run of coders with a thin grey line above it. The Researcher, the
    // Director and the Auditor do real work and took no space at all.
    //
    // They are cards rather than another row because that is what makes them look
    // like members of the same crew: the same lane, the same shape, moving left to
    // right as the run goes.
    //
    // AFTER the coders, not before. A lane is a column and the fold is real:
    // putting three faculty cards first pushed the actual work out of sight, which
    // was worse than not showing them at all.
    for (const auto& member : members) {
        if (member.name == "Coders") continue;   // the coders below ARE those

        Lane* lane  = &todo_;
        int   which = 0;
        if (member.working)   { lane = &doing_; which = 1; }
        else if (member.done) { lane = &done_;  which = 2; }
        ++counts[which];

        auto card = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        // The left border says the state, so a column is read by scanning down
        // the edge rather than by reading every card.
        card->add_css_class("crew-card");
        if (member.working)   card->add_css_class("crew-card-working");
        else if (member.done) card->add_css_class("crew-card-done");

        auto* who = Gtk::make_managed<Gtk::Label>(member.name);
        who->set_xalign(0.0f);
        who->add_css_class(member.working ? "heading" : "subtitle");
        card->append(*who);

        // What it is for, so a card is not just a noun. The Director's "3 pieces"
        // and the Auditor's "1 held" land here.
        std::string detail = member.detail;
        if (detail.empty()) {
            detail = member.working ? "working" : (member.done ? "finished" : "waiting");
        }
        auto* what = Gtk::make_managed<Gtk::Label>(detail);
        what->set_xalign(0.0f);
        what->set_wrap(true);
        what->add_css_class("subtitle");
        if (member.working) what->add_css_class("accent");
        card->append(*what);

        lane->body.append(*card);
        run_rows_.push_back(std::move(card));
    }

    todo_.title.set_text("To do (" + std::to_string(counts[0]) + ")");
    doing_.title.set_text("Doing (" + std::to_string(counts[1]) + ")");
    done_.title.set_text("Done (" + std::to_string(counts[2]) + ")");
    held_.title.set_text("Held (" + std::to_string(counts[3]) + ")");
    held_.column.set_visible(counts[3] > 0);
}

void CrewWindow::refresh_board() {
    const auto path = auspex_board_path();
    if (!path.empty()) {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            if (have_board_mtime_ && stamp == board_mtime_) return;
            board_mtime_      = stamp;
            have_board_mtime_ = true;
        }
    }

    while (Gtk::Widget* child = board_box_.get_first_child()) board_box_.remove(*child);
    board_rows_.clear();

    const auto items = read_board();

    // ollamadev keeps its OWN board, and anything held there before Auspex grew an
    // engine is still sitting in it. Auspex will not touch it -- accepting through
    // this window would apply a changeset stored in a format it does not own -- but
    // saying nothing would let that work quietly rot. So it is counted and named.
    std::string elsewhere;
    if (const auto theirs = board_items(project_); !theirs.empty()) {
        elsewhere = "  (" + std::to_string(theirs.size()) +
                    " more held by ollamadev itself \u2014 decide those with "
                    "`ollamadev crew accept`)";
    }

    if (items.empty()) {
        board_heading_.set_text(elsewhere.empty()
                                    ? "Nothing is being held for review."
                                    : "Nothing held here." + elsewhere);
        board_scroller_.set_visible(false);
        return;
    }

    board_heading_.set_text(std::to_string(items.size()) +
                            (items.size() == 1 ? " change held for review"
                                               : " changes held for review") +
                            elsewhere);
    board_scroller_.set_visible(true);

    for (const auto& item : items) {
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        row->add_css_class("code-block");

        auto* summary = Gtk::make_managed<Gtk::Label>(
            "#" + std::to_string(item.n) + "  " + item.summary);
        summary->set_xalign(0.0f);
        summary->set_wrap(true);

        // The Auditor's reason gets equal weight to the summary. It is the sentence
        // that decides whether you accept, and burying it makes the buttons a coin
        // flip.
        auto* reason = Gtk::make_managed<Gtk::Label>(item.reason);
        reason->set_xalign(0.0f);
        reason->set_wrap(true);

        // The files by name, and what the patch does to them. A count alone does
        // not tell you whether this touches one test or the whole module.
        const DiffStat stat = diff_stat(item.diff);
        std::string subtitle = std::to_string(item.files) +
                               (item.files == 1 ? " file" : " files");
        if (stat.added || stat.removed) {
            subtitle += "   +" + std::to_string(stat.added) +
                        " \u2212" + std::to_string(stat.removed);
        }
        if (!item.file_names.empty()) {
            subtitle += "   ";
            for (std::size_t i = 0; i < item.file_names.size() && i < 3; ++i) {
                if (i) subtitle += ", ";
                subtitle += item.file_names[i];
            }
            if (item.file_names.size() > 3) {
                subtitle += ", +" + std::to_string(item.file_names.size() - 3) + " more";
            }
        }

        auto* files = Gtk::make_managed<Gtk::Label>(subtitle);
        files->set_xalign(0.0f);
        files->set_wrap(true);
        files->add_css_class("subtitle");

        // Which tree this would land in, but ONLY when it is not the one being
        // looked at.
        //
        // The board is global -- ~/.ollamadev/board, shared by every project -- so
        // a run left holding changes in one folder is listed beside a run in
        // another, under one set of numbers. Accept applies to the repo the engine
        // recorded, not to the folder above, and without this line there is nothing
        // on screen to tell you the two are different. Silent when they match,
        // because a warning shown every time is a warning nobody reads.
        Gtk::Label* elsewhere = nullptr;
        if (!item.repo_root.empty() &&
            normal_project_path(item.repo_root) != normal_project_path(project_)) {
            elsewhere = Gtk::make_managed<Gtk::Label>("⚠  lands in " + item.repo_root);
            elsewhere->set_xalign(0.0f);
            elsewhere->set_wrap(true);
            elsewhere->add_css_class("error");
        }

        const int n = item.n;

        // ---- the diff, folded away ----
        auto* diff_view = Gtk::make_managed<Gtk::TextView>();
        diff_view->set_editable(false);
        diff_view->set_monospace(true);
        diff_view->set_cursor_visible(false);
        auto* diff_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
        diff_scroller->set_child(*diff_view);
        // Horizontal scrolling, NOT wrapping: a wrapped diff line stops lining up
        // with the ones above it and the patch becomes unreadable.
        diff_scroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
        diff_scroller->set_min_content_height(180);
        diff_scroller->add_css_class("code-block");

        {
            auto buffer = diff_view->get_buffer();
            auto added   = buffer->create_tag();
            auto removed = buffer->create_tag();
            auto hunk    = buffer->create_tag();
            auto header  = buffer->create_tag();
            // The theme's own colours, so a patch reads the same as the rest of
            // the desktop rather than in whatever green a diff tool would pick.
            const Palette& palette = theme_by_name(Config::load().theme);
            added->property_foreground()   = Glib::ustring(std::string(palette.link));
            removed->property_foreground() = Glib::ustring(std::string(palette.error));
            hunk->property_foreground()    = Glib::ustring(std::string(palette.accent));
            header->property_foreground()  = Glib::ustring(std::string(palette.subtitle_fg));

            for (const auto& line : split_lines(item.diff)) {
                auto end = buffer->end();
                switch (classify_diff_line(line)) {
                    case DiffLine::Added:
                        buffer->insert_with_tag(end, line + "\n", added); break;
                    case DiffLine::Removed:
                        buffer->insert_with_tag(end, line + "\n", removed); break;
                    case DiffLine::Hunk:
                        buffer->insert_with_tag(end, line + "\n", hunk); break;
                    case DiffLine::FileHeader:
                        buffer->insert_with_tag(end, line + "\n", header); break;
                    case DiffLine::Context:
                        buffer->insert(end, line + "\n"); break;
                }
            }
        }

        diff_scroller->set_visible(expanded_.count(n) != 0);

        auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* toggle = Gtk::make_managed<Gtk::Button>(
            expanded_.count(n) ? "Hide diff" : "Show diff");
        toggle->set_sensitive(!item.diff.empty());
        toggle->signal_clicked().connect([this, n, diff_scroller, toggle] {
            const bool open = expanded_.count(n) != 0;
            if (open) expanded_.erase(n); else expanded_.insert(n);
            diff_scroller->set_visible(!open);
            toggle->set_label(open ? "Show diff" : "Hide diff");
        });

        auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        spacer->set_hexpand(true);

        // The file count ON the button, as ollamadev-qt does it. Accept is the
        // irreversible-ish half of this pair, and "how much am I taking" belongs
        // where the hand is going rather than two lines above it.
        auto* accept = Gtk::make_managed<Gtk::Button>(
            item.files > 0 ? "Accept (" + std::to_string(item.files) +
                                 (item.files == 1 ? " file)" : " files)")
                           : std::string("Accept"));
        accept->add_css_class("suggested-action");
        auto* discard = Gtk::make_managed<Gtk::Button>("Discard");
        accept->signal_clicked().connect([this, n] { decide(n, true); });

        // Discard asks first. Accepting puts files into your folder and git can
        // undo it; discarding deletes the changeset and nothing can.
        const std::string summary_text = item.summary;
        discard->signal_clicked().connect([this, n, summary_text] {
            auto dialog = Gtk::AlertDialog::create();
            dialog->set_message("Discard change " + std::to_string(n) + "?");
            dialog->set_detail(summary_text +
                               "\n\nIts changeset is deleted and cannot be recovered.");
            dialog->set_buttons({"Cancel", "Discard"});
            dialog->set_cancel_button(0);
            dialog->set_default_button(0);
            dialog->choose(*this, [this, n](const Glib::RefPtr<Gio::AsyncResult>& result) {
                try {
                    if (auto d = Gtk::AlertDialog::create(); d->choose_finish(result) == 1) {
                        decide(n, false);
                    }
                } catch (const Glib::Error&) {
                    // Dismissed with Escape, which is a Cancel.
                }
            });
        });

        buttons->append(*toggle);
        buttons->append(*spacer);
        buttons->append(*discard);
        buttons->append(*accept);

        row->append(*summary);
        if (!item.reason.empty()) row->append(*reason);
        row->append(*files);
        if (elsewhere) row->append(*elsewhere);
        row->append(*buttons);
        row->append(*diff_scroller);

        board_box_.append(*row);
        board_rows_.push_back(std::move(row));
    }
}

void CrewWindow::decide(int n, bool accept) {
    // Checked against the board that actually exists, not trusted from the button.
    // The buttons are built from a real board, but it can change between drawing a
    // row and pressing it, and applying the wrong changeset is not something to
    // leave to timing.
    if (!board_item(read_board(), n)) {
        status_.set_text("Change " + std::to_string(n) + " is no longer on the board");
        have_board_mtime_ = false;
        refresh_board();
        return;
    }

    std::string error;
    const bool ok = accept ? accept_held(n, &error) : discard_held(n, &error);
    if (!ok) {
        status_.set_text("\u26a0 " + (error.empty() ? std::string("could not do that")
                                                     : error));
    } else {
        status_.set_text((accept ? "\u2713 accepted change " : "\u2713 discarded change ") +
                         std::to_string(n));
    }

    // Forced: accepting one changeset can release or invalidate another, and the
    // file may not have been rewritten yet.
    have_board_mtime_ = false;
    refresh_board();
}

// ---------------------------------------------------------------------------
// TeamWindow
// ---------------------------------------------------------------------------
TeamWindow::TeamWindow(const Config& config) : config_(config) {
    set_title("Auspex Team");
    add_css_class("auspex-window");
    set_default_size(620, 620);
    set_hide_on_close(true);

    if (const auto initial = default_project()) project_ = initial->path;
    project_label_.set_xalign(0.0f);
    project_label_.set_hexpand(true);
    project_label_.set_ellipsize(Pango::EllipsizeMode::START);
    project_pick_.signal_clicked().connect([this] { choose_project(); });
    project_row_.append(project_label_);
    project_row_.append(project_pick_);

    providers_heading_.set_text("Providers — one terminal per pick");
    providers_heading_.set_xalign(0.0f);
    providers_heading_.add_css_class("subtitle");

    // Every backend the engine knows, with the uninstalled ones present but
    // disabled. A list that hid them would answer "why is Goose not here?" with
    // silence; this answers it with a greyed row and a tooltip.
    backends_ = available_backends();
    if (backends_.empty()) backends_ = known_backends();

    for (const auto& backend : backends_) {
        auto box = std::make_unique<Gtk::CheckButton>(backend.label);
        box->set_sensitive(backend.installed);
        box->set_tooltip_text(backend.installed
                                  ? "ollamadev --backend " + backend.id
                                  : "not installed on this machine");
        // Ollama is ticked by default when it is there: it is the one backend that
        // is local and free, so it is the honest default for a fan-out whose cost
        // scales with how many boxes are ticked.
        if (backend.installed && backend.id == "ollama") box->set_active(true);
        providers_.append(*box);
        boxes_.push_back(std::move(box));
    }

    providers_scroller_.set_child(providers_);
    providers_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    providers_scroller_.set_min_content_height(180);

    prompt_label_.set_text("Prompt for the whole team");
    prompt_label_.set_xalign(0.0f);
    prompt_.set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    prompt_.set_monospace(false);
    prompt_scroller_.set_child(prompt_);
    prompt_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    prompt_scroller_.set_min_content_height(120);
    prompt_scroller_.set_vexpand(true);
    prompt_scroller_.add_css_class("code-block");

    launch_.add_css_class("suggested-action");
    launch_.signal_clicked().connect([this] { launch(); });
    buttons_.append(launch_);

    status_.set_xalign(0.0f);
    status_.add_css_class("subtitle");
    status_.set_wrap(true);

    root_.set_margin(14);
    root_.append(project_row_);
    root_.append(providers_heading_);
    root_.append(providers_scroller_);
    root_.append(prompt_label_);
    root_.append(prompt_scroller_);
    root_.append(buttons_);
    root_.append(status_);
    set_child(root_);

    show_project();
}

void TeamWindow::show_project() {
    if (!is_project_dir(project_)) {
        project_label_.set_markup("<b>No folder chosen</b> — the team has nowhere to work");
        return;
    }
    const std::string leaf = project_.filename().empty() ? project_.string()
                                                         : project_.filename().string();
    project_label_.set_markup("Working in <b>" + Glib::Markup::escape_text(leaf) +
                              "</b>  <span alpha='60%'>" +
                              Glib::Markup::escape_text(project_.string()) + "</span>");
    project_label_.set_tooltip_text(project_.string());
}

void TeamWindow::set_project(const std::filesystem::path& path) {
    if (!is_project_dir(path)) return;
    project_ = normal_project_path(path);
    remember_project(project_);
    show_project();
}

void TeamWindow::choose_project() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Which folder should the team work in?");
    if (is_project_dir(project_)) {
        dialog->set_initial_folder(Gio::File::create_for_path(project_.string()));
    }
    dialog->select_folder(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            if (const auto folder = dialog->select_folder_finish(result)) {
                set_project(folder->get_path());
                status_.set_text({});
            }
        } catch (const Glib::Error&) {
        }
    });
}

void TeamWindow::launch() {
    if (!is_project_dir(project_)) {
        status_.set_text("Choose a folder for the team to work in first");
        return;
    }
    if (config_.terminal.empty()) {
        status_.set_text("No terminal is configured");
        return;
    }

    const auto buffer = prompt_.get_buffer();
    const std::string prompt = trim(std::string(buffer->get_text()));
    if (prompt.empty()) {
        status_.set_text("Type a prompt first");
        return;
    }

    std::vector<std::string> launched;
    for (std::size_t i = 0; i < boxes_.size() && i < backends_.size(); ++i) {
        if (!boxes_[i]->get_active()) continue;

        const auto command = backend_prompt_command(backends_[i].id, prompt);
        if (command.empty()) continue;

        const auto argv = terminal_command_argv(config_.terminal, command, project_);
        if (argv.empty() || !spawn_detached(argv, project_.string())) continue;
        launched.push_back(backends_[i].label);
    }

    if (launched.empty()) {
        status_.set_text("Select at least one installed provider");
        return;
    }

    remember_project(project_);

    std::string names;
    for (std::size_t i = 0; i < launched.size(); ++i) {
        if (i) names += ", ";
        names += launched[i];
    }
    status_.set_text("✓ " + std::to_string(launched.size()) +
                     (launched.size() == 1 ? " terminal: " : " terminals: ") + names);
}

// ---------------------------------------------------------------------------
// BrainWindow
// ---------------------------------------------------------------------------
BrainWindow::~BrainWindow() {
    // Ask, then wait. The loop checks between cases, so this blocks for at most
    // one model call.
    measure_cancel_.store(true);
    if (measure_thread_.joinable()) measure_thread_.join();
}

void BrainWindow::measure(bool auditor) {
    if (measuring_.exchange(true)) return;   // already going
    measure_cancel_.store(false);
    measure_go_.set_sensitive(false);
    measure_coder_.set_sensitive(false);
    measure_stop_.set_sensitive(true);
    measure_result_.set_text("measuring…");

    if (measure_thread_.joinable()) measure_thread_.join();
    measure_thread_ = std::thread([this, auditor] {
        const Config config = Config::load();

        if (!auditor) {
            // The coder suite. Every task runs in a directory of its own, never
            // in a project of yours -- this is the one part of the crew that
            // deliberately runs unreviewed model output.
            EvalOptions options;
            options.model = with_config_roles(config, {}).model_for("coder");

            std::vector<EvalResult> results;
            for (const auto& task : builtin_evals()) {
                if (measure_cancel_.load()) break;
                results.push_back(run_eval(config, task, options));
            }

            const EvalSummary summary = summarize_evals(results);
            std::ostringstream coder_out;
            if (summary.scored() == 0) {
                // Not 0% and not 100%: neither would be true.
                coder_out << "no tasks could be scored on this machine";
            } else {
                coder_out << summary.passed << "/" << summary.scored() << " passed";
                if (measure_cancel_.load()) coder_out << " (stopped early)";
                if (summary.skipped > 0) {
                    coder_out << "  ·  " << summary.skipped
                              << " skipped for a missing interpreter";
                }
            }
            {
                std::lock_guard lock(measure_mutex_);
                measure_text_ = coder_out.str();
            }
            measuring_.store(false);
            measure_done_.emit();
            return;
        }

        // The model this window has the Auditor set to -- the whole point is to
        // measure the choice you just made, not some default.
        AuditEvalOptions options;
        options.model = with_config_roles(config, {}).model_for("auditor");

        std::vector<AuditEvalResult> results;
        for (const auto& item : builtin_audit_cases()) {
            if (measure_cancel_.load()) break;
            results.push_back(run_audit_case(config, item, options));
        }

        const AuditEvalSummary summary = summarize_audit(results);
        std::ostringstream out;
        if (results.empty()) {
            out << "stopped before anything was measured";
        } else {
            out << summary.correct << "/" << summary.total() << " correct";
            if (measure_cancel_.load()) out << " (stopped early)";
            // Never one averaged number: an Auditor that holds everything and one
            // that accepts everything both score 50%, and only one can hurt you.
            out << "  ·  " << summary.false_holds << " false hold"
                << (summary.false_holds == 1 ? "" : "s") << " (work thrown away)"
                << "  ·  " << summary.false_accepts << " false accept"
                << (summary.false_accepts == 1 ? "" : "s")
                << " (broken code landed)";
            if (summary.invented_quotes > 0) {
                out << "  ·  " << summary.invented_quotes
                    << " held on evidence not in the diff";
            }
        }

        {
            std::lock_guard lock(measure_mutex_);
            measure_text_ = out.str();
        }
        measuring_.store(false);
        measure_done_.emit();
    });
}

BrainWindow::BrainWindow() {
    set_title("Auspex Brain");
    add_css_class("auspex-window");
    set_default_size(700, 860);
    set_hide_on_close(true);

    heading_.set_markup("<b>The crew's brain</b> — what it is made of, and which "
                        "model does each part");
    heading_.set_xalign(0.0f);
    heading_.set_wrap(true);

    // ---- per-role models ----
    roles_.set_row_spacing(8);
    roles_.set_column_spacing(12);

    // From the engine's own table, so a role added there appears here with no edit.
    // Order is the order of the run.
    int row = 0;
    for (const auto& role : configurable_roles()) {
        auto entry = std::make_unique<RoleRow>();
        entry->key = role.key;
        entry->label.set_text(role.label);
        entry->label.set_tooltip_text(
            role.hint + (role.fallback.empty()
                             ? ""
                             : "  ·  unset follows the " + role.fallback));
        entry->label.set_xalign(0.0f);
        entry->models.set_hexpand(true);

        // The Auditor is the one worth setting, and the tooltip says why rather
        // than leaving it to be discovered the hard way.
        if (role.key == "auditor") {
            entry->models.set_tooltip_text(
                "The one worth changing. A small model here holds correct work with "
                "confident wrong reasons, which makes the whole crew pointless.");
        }

        entry->backends.set_tooltip_text(
            "Which agent does this role. \"ollama\" is Auspex's own loop; the rest "
            "hand the role to that CLI, running whatever model it is configured "
            "with.");

        entry->models.property_selected().signal_changed().connect([this] {
            if (loading_) return;
            save_models();
        });
        entry->backends.property_selected().signal_changed().connect([this] {
            if (loading_) return;
            save_models();
        });

        roles_.attach(entry->label, 0, row);
        roles_.attach(entry->backends, 1, row);
        roles_.attach(entry->models, 2, row);
        rows_.push_back(std::move(entry));
        ++row;
    }

    // ---- the tiers ----
    tiers_heading_.set_markup(
        "<b>Router</b> — a model per difficulty, used only where a role above is "
        "left on Default");
    tiers_heading_.set_xalign(0.0f);
    tiers_heading_.set_wrap(true);
    tiers_.set_row_spacing(6);
    tiers_.set_column_spacing(12);

    static const std::vector<std::pair<const char*, const char*>> kTiers{
        {"simple",   "Simple — trivia, renames"},
        {"moderate", "Moderate — ordinary work"},
        {"hard",     "Hard — design, debugging"},
    };

    int tier_row = 0;
    for (const auto& [tier, caption] : kTiers) {
        auto entry = std::make_unique<TierRow>();
        entry->tier = tier;
        entry->label.set_text(caption);
        entry->label.set_xalign(0.0f);
        entry->models.set_hexpand(true);
        entry->models.property_selected().signal_changed().connect([this] {
            if (loading_) return;
            save_models();
        });
        tiers_.attach(entry->label, 0, tier_row);
        tiers_.attach(entry->models, 1, tier_row);
        tier_rows_.push_back(std::move(entry));
        ++tier_row;
    }

    probe_entry_.set_placeholder_text(
        "Try it — e.g. \"rename a variable\" or \"design a cache layer\"…");
    probe_entry_.set_hexpand(true);
    probe_entry_.signal_activate().connect([this] { probe(); });
    probe_go_.signal_clicked().connect([this] { probe(); });
    probe_row_.append(probe_entry_);
    probe_row_.append(probe_go_);
    probe_result_.set_xalign(0.0f);
    probe_result_.set_wrap(true);

    // ---- the pipeline ----
    map_heading_.set_text("The pipeline");
    map_heading_.set_xalign(0.0f);
    map_heading_.add_css_class("subtitle");
    map_scroller_.set_child(map_);
    map_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    map_scroller_.set_vexpand(true);

    tokens_.set_xalign(0.0f);
    tokens_.set_wrap(true);
    tokens_.set_margin_top(6);

    status_.set_xalign(0.0f);
    status_.add_css_class("subtitle");
    status_.set_wrap(true);

    root_.set_margin(14);
    root_.append(heading_);
    root_.append(roles_);
    root_.append(tiers_heading_);
    root_.append(tiers_);
    root_.append(probe_row_);
    root_.append(probe_result_);
    root_.append(map_heading_);
    root_.append(map_scroller_);

    // Measure the model you just picked.
    measure_heading_.set_text("Is the Auditor any good?");
    measure_heading_.set_xalign(0.0f);
    measure_heading_.add_css_class("title");
    measure_go_.set_tooltip_text(
        "Run the Auditor corpus against the model above: thirteen changesets whose "
        "right answer is beyond argument. Reports the two errors apart -- a false "
        "HOLD wastes a coder's work, a false ACCEPT puts broken code in your "
        "project, and only the split tells them apart.");
    measure_stop_.set_sensitive(false);
    measure_result_.set_xalign(0.0f);
    measure_result_.set_wrap(true);
    measure_result_.add_css_class("subtitle");

    measure_coder_.set_tooltip_text(
        "Run the coder suite against the model above: eight small tasks, each "
        "scored by RUNNING the code rather than by asking a model. Each runs in a "
        "directory of its own, never in your project.");
    measure_row_.append(measure_go_);
    measure_row_.append(measure_coder_);
    measure_row_.append(measure_stop_);
    root_.append(measure_heading_);
    root_.append(measure_row_);
    root_.append(measure_result_);

    measure_go_.signal_clicked().connect([this] { measure(/*auditor=*/true); });
    measure_coder_.signal_clicked().connect([this] { measure(/*auditor=*/false); });
    measure_stop_.signal_clicked().connect([this] { measure_cancel_.store(true); });
    measure_done_.connect([this] {
        {
            std::lock_guard lock(measure_mutex_);
            measure_result_.set_text(measure_text_);
        }
        measure_go_.set_sensitive(true);
        measure_coder_.set_sensitive(true);
        measure_stop_.set_sensitive(false);
        if (measure_thread_.joinable()) measure_thread_.join();
    });

    root_.append(tokens_);
    root_.append(status_);
    set_child(root_);

    reload();
    refresh_map();

    // The stage a run is in changes as it works. Two seconds, and a rebuild only
    // when the stage actually changed -- redrawing thirteen rows every tick would
    // flicker for nothing.
    Glib::signal_timeout().connect(
        [this] {
            refresh_map();
            return true;
        },
        2000);
}

void BrainWindow::reload() {
    models_ = available_models();
    const Config config = Config::load();

    // Ollama plus every agent CLI actually installed. Offering one that is not
    // there would be a role that fails at the moment it is needed rather than at
    // the moment it is chosen.
    backends_.clear();
    backends_.push_back("ollama");
    for (const auto& id : coder_backends()) {
        if (id == "ollama") continue;
        if (!resolve_agent_binary(id == "cursor-agent" ? "cursor-agent" : id).empty()) {
            backends_.push_back(id);
        }
    }

    loading_ = true;
    for (auto& entry : rows_) {
        {
            std::vector<Glib::ustring> labels;
            for (const auto& id : backends_) labels.emplace_back(id);
            entry->backends.set_model(Gtk::StringList::create(labels));
        }

        const auto found_backend = config.crew_role_backends.find(entry->key);
        const std::string chosen_backend =
            found_backend == config.crew_role_backends.end() ? std::string{}
                                                             : found_backend->second;
        guint backend_index = 0;
        for (std::size_t i = 0; i < backends_.size(); ++i) {
            if (backends_[i] == chosen_backend) {
                backend_index = static_cast<guint>(i);
                break;
            }
        }
        entry->backends.set_selected(backend_index);
    }
    for (auto& entry : rows_) {
        // "Default" first, so a role can be put back to following ollama_model
        // without having to know what that is.
        std::string follows = config.ollama_model;
        for (const auto& role : configurable_roles()) {
            if (role.key == entry->key && !role.fallback.empty()) {
                follows = "the " + role.fallback;
                break;
            }
        }
        std::vector<Glib::ustring> labels;
        labels.emplace_back("Default (" + follows + ")");
        for (const auto& model : models_) labels.emplace_back(model);
        entry->models.set_model(Gtk::StringList::create(labels));

        const auto found_model = config.crew_role_models.find(entry->key);
        const std::string current =
            found_model == config.crew_role_models.end() ? std::string{}
                                                         : found_model->second;

        guint selected = 0;
        for (std::size_t i = 0; i < models_.size(); ++i) {
            if (models_[i] == current) {
                selected = static_cast<guint>(i) + 1;   // +1 for "Default"
                break;
            }
        }
        entry->models.set_selected(selected);
    }
    for (auto& entry : tier_rows_) {
        std::vector<Glib::ustring> labels;
        labels.emplace_back("Not set");
        for (const auto& model : models_) labels.emplace_back(model);
        entry->models.set_model(Gtk::StringList::create(labels));

        const auto found = config.crew_role_models.find("tier_" + entry->tier);
        const std::string current =
            found == config.crew_role_models.end() ? std::string{} : found->second;

        guint selected = 0;
        for (std::size_t i = 0; i < models_.size(); ++i) {
            if (models_[i] == current) {
                selected = static_cast<guint>(i) + 1;
                break;
            }
        }
        entry->models.set_selected(selected);
    }

    loading_ = false;

    if (models_.empty()) status_.set_text("No models listed — is Ollama running?");
    show_usage();
}

void BrainWindow::save_models() {
    const auto path = Config::default_path();

    // Read-modify-write, so keys this window does not expose are preserved rather
    // than dropped -- the same rule SettingsWindow follows.
    json document = json::object();
    if (std::ifstream in(path); in) {
        document = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (document.is_discarded() || !document.is_object()) document = json::object();
    }

    for (const auto& entry : rows_) {
        const auto index = entry->models.get_selected();
        // Index 0 is "Default": an empty string, which is what the engine reads as
        // "fall back to ollama_model".
        const std::string value =
            (index == GTK_INVALID_LIST_POSITION || index == 0 ||
             index > models_.size())
                ? std::string{}
                : models_[index - 1];
        document["crew_" + entry->key + "_model"] = value;

        const auto backend_index = entry->backends.get_selected();
        const std::string backend =
            (backend_index == GTK_INVALID_LIST_POSITION ||
             backend_index >= backends_.size())
                ? std::string{}
                : backends_[backend_index];
        // "ollama" is stored as empty, which is what the engine reads as its own
        // loop -- one spelling of the default rather than two.
        document["crew_" + entry->key + "_backend"] =
            backend == "ollama" ? std::string{} : backend;
    }

    for (const auto& entry : tier_rows_) {
        const auto index = entry->models.get_selected();
        const std::string value =
            (index == GTK_INVALID_LIST_POSITION || index == 0 || index > models_.size())
                ? std::string{}
                : models_[index - 1];
        document["crew_tier_" + entry->tier + "_model"] = value;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            status_.set_text("⚠ could not write the config");
            return;
        }
        out << document.dump(2) << "\n";
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        status_.set_text("⚠ could not replace the config");
        return;
    }
    status_.set_text("✓ saved — the next run uses these");
}

void BrainWindow::refresh_map() {
    const CrewRun run = current_crew_run(auspex_run_state_path());
    const std::string active = active_faculty(run);

    // Only when it changed. Redrawing thirteen rows every two seconds would
    // flicker for nothing.
    if (have_map_ && active == last_active_) return;
    last_active_ = active;
    have_map_    = true;

    while (Gtk::Widget* child = map_.get_first_child()) map_.remove(*child);
    map_rows_.clear();

    const Config config = Config::load();
    const bool have_mcp = !load_mcp_servers().empty();

    for (const auto& part : crew_faculties()) {
        auto card = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
        card->add_css_class("faculty");

        // The category decides the colour of the stripe down the left, so the
        // shape of the pipeline reads before any of the words do: what always
        // runs, what you asked for, and what simply refuses.
        switch (part.state) {
            case FacultyState::Guard:    card->add_css_class("faculty-guard"); break;
            case FacultyState::Optional: card->add_css_class("faculty-optin"); break;
            case FacultyState::Missing:  card->add_css_class("faculty-missing"); break;
            case FacultyState::Always:   card->add_css_class("faculty-always"); break;
        }

        std::string note = part.role;
        if (part.state == FacultyState::Optional) {
            // Whether the optional thing is actually ON, rather than leaving
            // "opt-in" to mean either.
            if (part.key == "mcp") {
                note += have_mcp ? "  ·  configured" : "  ·  none configured";
            } else if (part.key == "run") {
                note += "  ·  off by default";
            } else {
                note += "  ·  opt-in";
            }
        } else if (part.state == FacultyState::Missing) {
            note += "  ·  not in Auspex yet";
        }

        auto* name = Gtk::make_managed<Gtk::Label>(part.label);
        name->set_xalign(0.0f);
        name->add_css_class("faculty-name");

        auto* what = Gtk::make_managed<Gtk::Label>(note);
        what->set_xalign(0.0f);
        what->set_wrap(true);
        what->add_css_class("faculty-role");

        card->append(*name);
        card->append(*what);

        if (part.key == active) card->add_css_class("faculty-active");
        if (part.state == FacultyState::Missing) {
            name->set_opacity(0.4);
            what->set_opacity(0.4);
        }

        map_.append(*card);
        map_rows_.push_back(std::move(card));
    }

    (void)config;
    show_usage();
}

void BrainWindow::probe() {
    const std::string text = trim(std::string(probe_entry_.get_text()));
    if (text.empty()) {
        probe_result_.set_text({});
        return;
    }

    // No model call. Classifying is word lists and lengths, so the answer is
    // instant and free -- asking a model how hard something is would spend a call
    // to decide how to spend a call.
    const Difficulty how = classify_difficulty(text);
    const Config config = Config::load();

    const std::string model = tier_model(config, how.tier);
    probe_result_.set_markup(
        "<b>" + Glib::Markup::escape_text(how.tier) + "</b>  →  " +
        Glib::Markup::escape_text(model.empty()
                                      ? "(no model set for that tier — the role's "
                                        "own choice is used)"
                                      : model) +
        "\n<span alpha='70%'>" + Glib::Markup::escape_text(how.reason) + "</span>");
}

void BrainWindow::show_usage() {
    const auto project = default_project();
    if (!project) {
        tokens_.set_text({});
        return;
    }

    const TokenUsage usage = project_usage(project->path);
    const std::string summary = usage_summary(usage);
    if (summary.empty()) {
        tokens_.set_markup("<span alpha='70%'>No tokens recorded in " +
                           Glib::Markup::escape_text(project->name) + " yet</span>");
        return;
    }
    tokens_.set_markup("<b>" + Glib::Markup::escape_text(project->name) + ":</b> " +
                       Glib::Markup::escape_text(summary));
    tokens_.set_tooltip_text(usage_path(project->path).string());
}

// ---------------------------------------------------------------------------
// ProjectsWindow
// ---------------------------------------------------------------------------
ProjectsWindow::ProjectsWindow(const Config& config) : config_(config) {
    set_title("Auspex Projects");
    add_css_class("auspex-window");
    set_default_size(820, 560);
    set_hide_on_close(true);

    // ---- the folders ----
    left_heading_.set_text("Projects");
    left_heading_.set_xalign(0.0f);
    left_heading_.add_css_class("subtitle");

    list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    // Selection, not activation, drives the right-hand side. A single click should
    // show you what you would be opening; only the agent buttons launch anything.
    list_.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        const auto index = static_cast<std::size_t>(row->get_index());
        if (index < projects_.size()) select(projects_[index].path);
    });

    list_scroller_.set_child(list_);
    list_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    list_scroller_.set_vexpand(true);

    browse_.signal_clicked().connect([this] { browse(); });

    left_.set_margin(14);
    left_.set_size_request(280, -1);
    left_.append(left_heading_);
    left_.append(list_scroller_);
    left_.append(browse_);

    // ---- what to do with one ----
    chosen_name_.set_xalign(0.0f);
    chosen_name_.add_css_class("title");
    chosen_path_.set_xalign(0.0f);
    chosen_path_.add_css_class("subtitle");
    chosen_path_.set_wrap(true);
    chosen_path_.set_selectable(true);   // so it can be copied into a terminal

    agents_heading_.set_text("Open it in");
    agents_heading_.set_xalign(0.0f);
    agents_heading_.add_css_class("subtitle");

    // Installed only. Offering an agent that is not there turns a button into a
    // way of finding out it is missing, which is a thing to be told before you
    // press rather than after.
    agents_ = available_agents();
    if (agents_.empty()) {
        auto* none = Gtk::make_managed<Gtk::Label>(
            "No coding agents found in PATH.\n"
            "Auspex looks for claude, codex, gemini, cursor-agent, opencode, "
            "ollamadev and aider.");
        none->set_xalign(0.0f);
        none->set_wrap(true);
        none->add_css_class("subtitle");
        agent_box_.append(*none);
    }
    for (const auto& agent : agents_) {
        auto* button = Gtk::make_managed<Gtk::Button>();
        auto* row    = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto* label  = Gtk::make_managed<Gtk::Label>(agent.label);
        label->set_xalign(0.0f);
        label->set_hexpand(true);
        auto* binary = Gtk::make_managed<Gtk::Label>(agent.binary);
        binary->add_css_class("subtitle");
        row->append(*label);
        row->append(*binary);
        button->set_child(*row);
        button->set_tooltip_text("Run " + agent.binary + " in a terminal, in this folder");
        // Captured by value: the button outlives this loop iteration, and agents_
        // could be reallocated by anything that reloads the list later.
        const AgentTool copy = agent;
        button->signal_clicked().connect([this, copy] { open_in(copy); });
        agent_box_.append(*button);
    }

    terminal_.set_tooltip_text("A plain terminal, in this folder");
    terminal_.signal_clicked().connect([this] { open_terminal(); });
    files_.set_tooltip_text("Open this folder in the file manager");
    files_.signal_clicked().connect([this] { open_files(); });
    crew_.set_tooltip_text("Hand this folder to the crew bench");
    crew_.signal_clicked().connect([this] {
        if (!is_project_dir(selected_)) return;
        remember_project(selected_);
        if (on_crew_) on_crew_(selected_);
    });
    // Only when the engine is installed; otherwise it is a button to a window whose
    // every control would report the same absence.
    crew_.set_visible(crew_available());

    extras_.append(terminal_);
    extras_.append(files_);
    extras_.append(crew_);

    status_.set_xalign(0.0f);
    status_.add_css_class("subtitle");
    status_.set_wrap(true);

    right_.set_margin(14);
    right_.set_hexpand(true);
    right_.append(chosen_name_);
    right_.append(chosen_path_);
    right_.append(agents_heading_);
    right_.append(agent_box_);
    right_.append(extras_);
    right_.append(status_);

    root_.append(left_);
    root_.append(right_);
    set_child(root_);

    reload();
}

void ProjectsWindow::reload() {
    while (Gtk::Widget* child = list_.get_first_child()) list_.remove(*child);
    rows_.clear();

    projects_ = all_projects();

    for (const auto& project : projects_) {
        auto row = std::make_unique<Gtk::ListBoxRow>();
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        box->set_margin(6);

        auto* name = Gtk::make_managed<Gtk::Label>(project.name);
        name->set_xalign(0.0f);

        // The path under the name, ellipsised from the LEFT: two checkouts of the
        // same repo differ in their first components, not their last, so trimming
        // the front is what hides the difference.
        auto* path = Gtk::make_managed<Gtk::Label>(project.path.string());
        path->set_xalign(0.0f);
        path->add_css_class("subtitle");
        path->set_ellipsize(Pango::EllipsizeMode::START);

        box->append(*name);
        box->append(*path);
        row->set_child(*box);
        row->set_tooltip_text(project.path.string());

        list_.append(*row);
        rows_.push_back(std::move(row));
    }

    if (projects_.empty()) {
        select({});
        status_.set_text("No projects yet. Use “Open another folder” to pick one.");
        return;
    }

    // Start on the one the engine calls current, so the panel and ollamadev agree
    // about "this project" before anything is clicked.
    std::size_t start = 0;
    if (const auto preferred = default_project()) {
        for (std::size_t i = 0; i < projects_.size(); ++i) {
            if (normal_project_path(projects_[i].path) ==
                normal_project_path(preferred->path)) {
                start = i;
                break;
            }
        }
    }
    list_.select_row(*rows_[start]);
    select(projects_[start].path);
}

void ProjectsWindow::select(const std::filesystem::path& path) {
    selected_ = normal_project_path(path);

    const bool usable = is_project_dir(selected_);
    // One gate for everything that launches. There is no meaningful "open an agent
    // in no directory" -- that is precisely the behaviour this window replaces.
    agent_box_.set_sensitive(usable);
    extras_.set_sensitive(usable);

    if (!usable) {
        chosen_name_.set_text(selected_.empty() ? "No folder chosen" : "Folder is gone");
        chosen_path_.set_text(selected_.string());
        return;
    }

    const std::string leaf = selected_.filename().empty()
                                 ? selected_.string()
                                 : selected_.filename().string();
    chosen_name_.set_text(leaf);
    chosen_path_.set_text(selected_.string());
    status_.set_text({});
}

void ProjectsWindow::browse() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open a folder");
    if (is_project_dir(selected_)) {
        dialog->set_initial_folder(Gio::File::create_for_path(selected_.string()));
    }
    dialog->select_folder(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            const auto folder = dialog->select_folder_finish(result);
            if (!folder) return;
            const std::filesystem::path path = folder->get_path();
            if (!is_project_dir(path)) return;

            // Remembered on browse rather than on launch, so a folder you opened to
            // look at is in the list next time even if you did not start anything.
            remember_project(path);
            reload();

            for (std::size_t i = 0; i < projects_.size(); ++i) {
                if (normal_project_path(projects_[i].path) == normal_project_path(path)) {
                    list_.select_row(*rows_[i]);
                    break;
                }
            }
            select(path);
        } catch (const Glib::Error&) {
            // Cancelled.
        }
    });
}

void ProjectsWindow::open_in(const AgentTool& agent) {
    // Re-checked here rather than trusted from the list. A bookmark outlives the
    // folder it names, and the gap between drawing this row and pressing it is
    // however long the window has been open.
    if (!is_project_dir(selected_)) {
        status_.set_text("⚠ " + selected_.string() + " is not a folder");
        return;
    }
    if (config_.terminal.empty()) {
        status_.set_text("⚠ no terminal is configured");
        return;
    }
    // Re-resolved rather than trusting the path found when the window was built:
    // the list can be minutes old, and an agent updated by its own installer moves
    // between version directories.
    AgentTool fresh = agent;
    fresh.path = resolve_agent_binary(agent.binary);
    if (fresh.path.empty()) {
        status_.set_text("⚠ " + agent.binary + " is not installed any more");
        return;
    }

    const auto argv = agent_terminal_command(config_.terminal, fresh, selected_);
    // Both mechanisms, for the reason in projects.hpp: the flag for terminals that
    // spawn from a server process, the cwd for terminals with no flag.
    if (argv.empty() || !spawn_detached(argv, selected_.string())) {
        status_.set_text("⚠ could not start " + agent.label);
        return;
    }

    remember_project(selected_);
    status_.set_text("✓ " + agent.label + " in " + selected_.string());
}

void ProjectsWindow::open_terminal() {
    if (!is_project_dir(selected_) || config_.terminal.empty()) return;

    const auto argv = terminal_here(config_.terminal, selected_);
    if (argv.empty() || !spawn_detached(argv, selected_.string())) {
        status_.set_text("⚠ could not start the terminal");
        return;
    }
    remember_project(selected_);
    status_.set_text("✓ terminal in " + selected_.string());
}

void ProjectsWindow::open_files() {
    if (!is_project_dir(selected_)) return;
    if (!spawn_detached({"xdg-open", selected_.string()})) {
        status_.set_text("⚠ could not open the file manager");
        return;
    }
    status_.set_text("✓ opened " + selected_.string());
}

// ---------------------------------------------------------------------------
// ChatWindow
// ---------------------------------------------------------------------------
ChatWindow::ChatWindow(const Config& config, VoiceController& voice)
    : config_(config), voice_(voice) {
    set_title("Auspex");
    add_css_class("auspex-window");
    set_default_size(680, 620);

    log_.set_margin(12);
    scroller_.set_child(log_);
    scroller_.set_vexpand(true);
    scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);

    entry_.set_placeholder_text("Ask anything...");
    entry_.set_hexpand(true);
    entry_.signal_activate().connect([this] { send(); });
    send_.signal_clicked().connect([this] { send(); });

    // Hold to dictate straight into the entry box.
    auto gesture = Gtk::GestureClick::create();
    gesture->signal_pressed().connect(
        [this](int, double, double) { voice_.dictate_to_callback(); });
    gesture->signal_released().connect(
        [this](int, double, double) { voice_.release_hold(); });
    talk_.add_controller(gesture);
    talk_.set_tooltip_text("Hold to dictate");

    input_row_.set_margin(10);
    input_row_.append(entry_);
    input_row_.append(send_);
    input_row_.append(talk_);

    status_.add_css_class("subtitle");
    status_.set_xalign(0.0f);
    status_.set_margin_start(12);
    status_.set_margin_bottom(6);

    root_.append(scroller_);
    root_.append(status_);
    root_.append(input_row_);
    set_child(root_);

    // Replies and dictation arrive from the worker thread via Glib::Dispatcher, so
    // these callbacks are already on the GTK thread.
    voice_.on_reply = [this](const std::string& text) { on_reply(text); };
    voice_.on_transcript = [this](const std::string& text) {
        entry_.set_text(text);
        entry_.grab_focus();
        entry_.set_position(-1);
    };
    voice_.on_status = [this](const std::string& text) { status_.set_text(text); };

    entry_.grab_focus();
}

void ChatWindow::send() {
    const std::string text = entry_.get_text();
    if (text.empty()) return;
    entry_.set_text("");
    add_message(text, /*from_user=*/true);
    voice_.ask_text(text);
}

void ChatWindow::on_reply(const std::string& text) { add_message(text, false); }

void ChatWindow::add_message(const std::string& text, bool from_user) {
    auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    row->set_halign(from_user ? Gtk::Align::END : Gtk::Align::START);

    auto label = std::make_unique<Gtk::Label>(text);
    label->set_wrap(true);
    label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    label->set_xalign(0.0f);
    label->set_selectable(true);
    label->set_max_width_chars(72);
    // The stylesheet already defines these; they were ported for llm_menu.py and
    // until now styled nothing.
    label->add_css_class(from_user ? "user-message" : "assistant-message");

    row->append(*label);

    // Per-message actions, as llm_menu.py had. Its "run this code block" button is
    // deliberately not ported: it executed model output, which is exactly the thing
    // the command whitelist exists to prevent.
    if (!from_user) {
        auto actions = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);

        auto copy = std::make_unique<Gtk::Button>("Copy");
        copy->add_css_class("message-button");
        const std::string payload = text;
        copy->signal_clicked().connect([this, payload] {
            get_clipboard()->set_text(payload);
            status_.set_text("Copied");
        });

        auto speak = std::make_unique<Gtk::Button>("Speak");
        speak->add_css_class("message-button");
        speak->signal_clicked().connect([this, payload] {
            // Spoken on a detached thread: Tts::speak blocks until playback ends,
            // which would freeze the window for the length of the utterance.
            std::thread([cmd = config_.tts_command, payload] {
                Tts(cmd).speak(payload);
            }).detach();
        });

        actions->append(*copy);
        actions->append(*speak);
        row->append(*actions);
        widgets_.push_back(std::move(copy));
        widgets_.push_back(std::move(speak));
        widgets_.push_back(std::move(actions));
    }

    log_.append(*row);
    widgets_.push_back(std::move(label));
    widgets_.push_back(std::move(row));

    // Scroll after the new row has been allocated, or the adjustment still has the
    // old upper bound and the view stays put.
    Glib::signal_idle().connect_once([this] {
        auto adjustment = scroller_.get_vadjustment();
        adjustment->set_value(adjustment->get_upper());
    });
}

// ---------------------------------------------------------------------------
// SettingsWindow
// ---------------------------------------------------------------------------
SettingsWindow::SettingsWindow(Config config) : config_(std::move(config)) {
    set_title("Auspex Settings");
    add_css_class("auspex-window");
    set_default_size(520, 620);

    root_.set_margin(14);

    const auto add_row = [this](const std::string& caption, Gtk::Widget& widget) {
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto label = std::make_unique<Gtk::Label>(caption);
        label->set_xalign(0.0f);
        label->set_size_request(170, -1);
        widget.set_hexpand(true);
        row->append(*label);
        row->append(widget);
        root_.append(*row);
        widgets_.push_back(std::move(label));
        widgets_.push_back(std::move(row));
    };

    // ---- theme ----
    {
        std::vector<Glib::ustring> names;
        int selected = 0;
        for (std::size_t i = 0; i < themes().size(); ++i) {
            names.emplace_back(std::string(themes()[i].name));
            if (themes()[i].name == config_.theme) selected = static_cast<int>(i);
        }
        theme_.set_model(Gtk::StringList::create(names));
        theme_.set_selected(static_cast<guint>(selected));
        add_row("Theme", theme_);
    }

    populate_microphones();
    add_row("Microphone", microphone_);

    // ---- ollama model ----
    {
        // Two seconds, not the default ten: this runs on the GTK thread while the
        // window is being built, so the timeout is exactly how long the whole shell
        // freezes for when ollama is not running. Two is long enough for a loopback
        // request and short enough not to read as a hang.
        OllamaClient ollama(config_);
        model_names_ = ollama.list_models(std::chrono::seconds(2));

        // The configured model always appears, even if ollama is unreachable or the
        // model was deleted. Without this the dropdown would land on whatever
        // happened to be first and saving would silently switch models -- the user
        // opened this window to change the theme and left with a different LLM.
        if (!config_.ollama_model.empty() &&
            std::find(model_names_.begin(), model_names_.end(), config_.ollama_model) ==
                model_names_.end()) {
            model_names_.insert(model_names_.begin(), config_.ollama_model);
        }
        if (model_names_.empty()) model_names_.push_back(config_.ollama_model);

        std::vector<Glib::ustring> names;
        int selected = 0;
        for (std::size_t i = 0; i < model_names_.size(); ++i) {
            names.emplace_back(model_names_[i]);
            if (model_names_[i] == config_.ollama_model) selected = static_cast<int>(i);
        }
        model_.set_model(Gtk::StringList::create(names));
        model_.set_selected(static_cast<guint>(selected));
        add_row("Language model", model_);
    }

    const auto spin = [](Gtk::SpinButton& button, double low, double high, double step,
                         double value, int digits = 0) {
        button.set_range(low, high);
        button.set_increments(step, step * 10);
        button.set_digits(digits);
        button.set_value(value);
    };

    spin(panel_height_, 16, 96, 1, config_.panel_height);
    add_row("Panel height", panel_height_);

    spin(workspaces_, 1, 16, 1, config_.workspace_count);
    add_row("Workspaces", workspaces_);

    spin(memory_turns_, 0, 40, 1, config_.memory_turns);
    add_row("Conversation memory (turns)", memory_turns_);

    spin(vad_threshold_, 0.10, 0.95, 0.05, config_.vad_threshold, 2);
    add_row("Listening sensitivity", vad_threshold_);

    terminal_.set_text(config_.terminal);
    terminal_.set_placeholder_text("detected automatically");
    add_row("Terminal", terminal_);

    launcher_.set_text(config_.launcher);
    launcher_.set_placeholder_text("detected automatically");
    add_row("Launcher", launcher_);

    enable_ai_.set_active(config_.enable_ai);
    root_.append(enable_ai_);

    // Autostart is NOT part of config.json. It lives in the XDG autostart directory
    // because that is the file the session reads at login -- Auspex is not running
    // then, so a flag in its own config could not possibly start it.
    autostart_.set_active(autostart_enabled());
    root_.append(autostart_);

    clock_24_.set_active(config_.clock_24_hour);
    root_.append(clock_24_);

    // ---- pinned applications ----
    pinned_ = config_.pinned;
    pins_summary_.set_xalign(0.0f);
    pins_summary_.set_hexpand(true);
    pins_summary_.add_css_class("subtitle");
    import_pins_.signal_clicked().connect([this] { import_pins(); });
    pins_row_.append(pins_summary_);
    pins_row_.append(import_pins_);
    add_row("Pinned applications", pins_row_);
    {
        // Written here rather than in a helper so the empty case reads as an
        // invitation rather than as a fault.
        const std::size_t count = pinned_.size();
        pins_summary_.set_text(count == 0 ? "None pinned"
                                          : std::to_string(count) + " pinned");
    }

    // ---- date and time ----
    //
    // System state, not Auspex's, so these apply on their own controls rather than
    // waiting for Save -- each one is a polkit prompt, and Save should never ask for
    // a password on behalf of a setting the user did not touch.
    {
        time_now_.set_xalign(0.0f);
        time_now_.add_css_class("subtitle");
        root_.append(time_now_);

        timezones_ = list_timezones();
        std::vector<Glib::ustring> names;
        names.reserve(timezones_.size());
        for (const auto& zone : timezones_) names.emplace_back(zone);
        timezone_.set_model(Gtk::StringList::create(names));
        // 598 zones is a scroll, not a list. Search is what makes it usable.
        timezone_.set_enable_search(true);
        timezone_.set_expression(
            Gtk::ClosureExpression<Glib::ustring>::create([](const Glib::RefPtr<Glib::ObjectBase>& item) {
                const auto text = std::dynamic_pointer_cast<Gtk::StringObject>(item);
                return text ? text->get_string() : Glib::ustring{};
            }));
        add_row("Time zone", timezone_);
        timezone_.property_selected().signal_changed().connect(
            [this] { apply_timezone(); });

        root_.append(time_automatic_);
        time_automatic_.signal_toggled().connect([this] {
            if (loading_time_) return;
            if (!set_automatic_time(time_automatic_.get_active())) {
                time_status_.set_text("Could not change automatic time");
            }
            refresh_time();
        });

        manual_time_.set_placeholder_text("YYYY-MM-DD HH:MM:SS");
        manual_time_.set_hexpand(true);
        apply_time_.signal_clicked().connect([this] { apply_manual_time(); });
        manual_time_row_.append(manual_time_);
        manual_time_row_.append(apply_time_);
        add_row("Set the time", manual_time_row_);

        time_status_.add_css_class("subtitle");
        time_status_.set_xalign(0.0f);
        time_status_.set_wrap(true);
        root_.append(time_status_);

        refresh_time();
    }

    status_.add_css_class("subtitle");
    status_.set_xalign(0.0f);
    root_.append(status_);

    save_.add_css_class("suggested-action");
    save_.signal_clicked().connect([this] { save(); });
    close_.signal_clicked().connect([this] { close(); });

    buttons_.set_halign(Gtk::Align::END);
    buttons_.append(close_);
    buttons_.append(save_);
    root_.append(buttons_);

    set_child(root_);
}

void SettingsWindow::refresh_time() {
    time_state_ = current_time_settings();

    // timedatectl absent: hide the whole section rather than offer controls that
    // cannot do anything.
    const bool have = time_state_.known;
    time_now_.set_visible(have);
    time_automatic_.set_visible(have);
    timezone_.set_visible(have);
    manual_time_row_.set_visible(have);
    if (!have) return;

    loading_time_ = true;

    time_now_.set_text(time_state_.local_time);
    time_automatic_.set_active(time_state_.ntp_active);

    const auto at = std::find(timezones_.begin(), timezones_.end(), time_state_.timezone);
    if (at != timezones_.end()) {
        timezone_.set_selected(static_cast<guint>(std::distance(timezones_.begin(), at)));
    }

    // The part that makes this honest. With NTP running, timedatectl refuses a
    // manual time outright -- so rather than offering a field that silently fails,
    // it is disabled and the reason is written next to it.
    const bool manual = !time_state_.ntp_active;
    manual_time_.set_sensitive(manual);
    apply_time_.set_sensitive(manual);
    if (!manual) {
        time_status_.set_text(
            "The clock is set automatically. Turn that off to set it by hand.");
        manual_time_.set_text("");
    } else if (time_status_.get_text().empty()) {
        time_status_.set_text("");
    }

    loading_time_ = false;
}

void SettingsWindow::apply_timezone() {
    if (loading_time_) return;

    const auto index = timezone_.get_selected();
    if (index >= timezones_.size()) return;
    const std::string zone = timezones_[index];
    if (zone == time_state_.timezone) return;

    if (!set_system_timezone(zone)) {
        // Cancelling the password prompt lands here too, which is a refusal rather
        // than a fault -- so it is reported the same way and the controls are put
        // back to whatever the system actually says.
        time_status_.set_text("Time zone unchanged");
    } else {
        time_status_.set_text("Time zone set to " + zone);
    }
    refresh_time();
}

void SettingsWindow::apply_manual_time() {
    const std::string text = std::string(manual_time_.get_text());

    // Checked before anything privileged is run, so a typo costs a message rather
    // than a password prompt for a command that was always going to be refused.
    if (!is_valid_datetime(text)) {
        time_status_.set_text("Enter the time as YYYY-MM-DD HH:MM:SS");
        return;
    }

    if (!set_system_time(text)) {
        time_status_.set_text("Could not set the time");
    } else {
        time_status_.set_text("Time set");
    }
    refresh_time();
}

void SettingsWindow::import_pins() {
    const auto found = import_xfce_launchers();
    if (found.empty()) {
        pins_summary_.set_text("Nothing found on xfce4-panel");
        return;
    }

    // Ids, not commands. Stored that way so the icon and name follow the
    // application when it is updated, and so nothing in config.json is a command
    // line waiting for something to run it.
    pinned_.clear();
    for (const auto& entry : found) pinned_.push_back(entry.id);

    pins_summary_.set_text(std::to_string(pinned_.size()) + " pinned \u2014 press Save");
}

void SettingsWindow::populate_microphones() {
    microphone_names_.clear();
    // Index 0 is the system default, so there is always a way back to it.
    microphone_names_.emplace_back("");

    std::vector<Glib::ustring> labels;
    labels.emplace_back("(system default)");

    int selected = 0;
    for (const auto& device : audio::list_input_devices()) {
        // "Monitor of ..." devices are loopbacks: they capture system output, not a
        // voice. Listed but flagged, since picking one silently records the wrong
        // thing and that is the failure this setting exists to prevent.
        const bool monitor = device.name.rfind("Monitor of", 0) == 0;
        labels.emplace_back(device.name + (monitor ? "  (loopback)" : ""));
        microphone_names_.push_back(device.name);

        if (!config_.default_microphone.empty() &&
            device.name.find(config_.default_microphone) != std::string::npos) {
            selected = static_cast<int>(microphone_names_.size()) - 1;
        }
    }

    microphone_.set_model(Gtk::StringList::create(labels));
    microphone_.set_selected(static_cast<guint>(selected));
}

void SettingsWindow::save() {
    const auto path = Config::default_path();

    // Read-modify-write the existing file rather than serialising Config: that
    // preserves keys this window does not expose (model paths, tts_command,
    // search_url) instead of silently dropping them.
    json document = json::object();
    if (std::ifstream in(path); in) {
        document = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (document.is_discarded() || !document.is_object()) document = json::object();
    }

    const auto theme_index = theme_.get_selected();
    if (theme_index < themes().size()) {
        document["auspex_theme"] = std::string(themes()[theme_index].name);
    }

    const auto mic_index = microphone_.get_selected();
    if (mic_index < microphone_names_.size()) {
        document["default_microphone"] = microphone_names_[mic_index];
    }

    const auto model_index = model_.get_selected();
    if (model_index < model_names_.size()) {
        document["ollama_model"] = model_names_[model_index];
    }

    document["panel_height"]    = panel_height_.get_value_as_int();
    document["workspace_count"] = workspaces_.get_value_as_int();
    document["memory_turns"]    = memory_turns_.get_value_as_int();
    document["vad_threshold"]   = vad_threshold_.get_value();
    document["enable_ai"]       = enable_ai_.get_active();
    document["clock_24_hour"]   = clock_24_.get_active();
    document["pinned"]          = pinned_;
    document["terminal"]        = std::string(terminal_.get_text());
    document["launcher"]        = std::string(launcher_.get_text());

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Write to a temporary file and rename: a crash mid-write would otherwise leave
    // a truncated config, and the shell would fall back to defaults on next start.
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) {
            status_.set_text("Could not write " + path.string());
            return;
        }
        out << document.dump(4) << "\n";
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        status_.set_text("Could not replace " + path.string());
        return;
    }

    // Applied after config.json is safely on disk, and reported separately: it is a
    // different file in a different place, and a failure here does not mean the rest
    // of the settings were lost.
    const bool want_autostart = autostart_.get_active();
    if (want_autostart != autostart_enabled() && !set_autostart(want_autostart)) {
        status_.set_text("Settings saved, but could not write " +
                         autostart_path().string());
        return;
    }

    // The panel watches config.json every second, so the theme and panel geometry
    // apply without a restart. Model and microphone are read when the relevant
    // subsystem next initialises.
    status_.set_text("Saved. Theme applies immediately; some options need a restart.");
}

}  // namespace auspex::gtk
