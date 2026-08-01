#include "auspex/gtk/windows.hpp"

#include <algorithm>
#include <thread>
#include <fstream>
#include <sstream>

#include <glibmm/main.h>
#include <glibmm/markup.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/stringlist.h>

#include <gdkmm/clipboard.h>

#include <nlohmann/json.hpp>

#include <glibmm/datetime.h>

#include <gtkmm/expression.h>
#include <gtkmm/stringobject.h>

#include "auspex/audio.hpp"
#include "auspex/autostart.hpp"
#include "auspex/crew.hpp"
#include "auspex/gtk/voice.hpp"
#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"
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
// BoardWindow
// ---------------------------------------------------------------------------
BoardWindow::BoardWindow() {
    set_title("Auspex Crew Board");
    add_css_class("auspex-window");
    set_default_size(680, 560);
    set_hide_on_close(true);

    heading_.set_xalign(0.0f);
    running_.add_css_class("subtitle");
    running_.set_xalign(0.0f);
    running_.set_wrap(true);
    running_.set_visible(false);

    heading_.add_css_class("subtitle");

    scroller_.set_child(list_);
    scroller_.set_vexpand(true);
    scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);

    refresh_.signal_clicked().connect([this] { refresh(); });
    close_.signal_clicked().connect([this] { close(); });
    buttons_.set_halign(Gtk::Align::END);
    buttons_.append(refresh_);
    buttons_.append(close_);

    root_.set_margin(12);
    root_.append(running_);
    root_.append(heading_);
    root_.append(scroller_);
    root_.append(buttons_);
    set_child(root_);

    signal_map().connect([this] { refresh(); });
    refresh();

    // Two seconds, and only two stats when nothing has moved. A changeset takes
    // tens of seconds to arrive, so this is already finer than what it watches.
    Glib::signal_timeout().connect(
        [this] {
            watch();
            return true;
        },
        2000);
}

void BoardWindow::watch() {
    std::error_code ec;
    bool changed = false;

    // The board itself: something landed, or a decision took effect.
    if (const auto path = board_state_path(); !path.empty()) {
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (!ec && (!have_board_mtime_ || stamp != board_mtime_)) {
            board_mtime_      = stamp;
            have_board_mtime_ = true;
            changed = true;
        }
    }

    // The run: worth watching separately so the line at the top of the window can
    // say a crew is still working even when nothing has landed yet. Those are very
    // different things to be looking at an empty board for.
    if (const auto path = crew_state_path(); !path.empty()) {
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (!ec && (!have_crew_mtime_ || stamp != crew_mtime_)) {
            crew_mtime_      = stamp;
            have_crew_mtime_ = true;
            changed = true;
        }
    }

    if (!changed) return;

    const CrewRun run = current_crew_run();
    const std::string status = crew_status_label(run);
    running_.set_visible(!status.empty());
    if (!status.empty()) {
        running_.set_text(status + " \u2014 " + crew_status_detail(run));
    }

    refresh();
}

void BoardWindow::refresh() {
    for (auto& row : rows_) list_.remove(*row);
    rows_.clear();

    const auto items = board_items();
    if (items.empty()) {
        heading_.set_text(crew_available()
                              ? "The crew is not holding anything."
                              : "ollamadev is not installed, so there is no crew.");
        return;
    }
    heading_.set_text(std::to_string(items.size()) +
                      (items.size() == 1 ? " change held for review"
                                         : " changes held for review"));

    for (const auto& item : items) {
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        row->add_css_class("code-block");

        auto* title = Gtk::make_managed<Gtk::Label>();
        title->set_markup("<b>#" + std::to_string(item.n) + "  " +
                          Glib::Markup::escape_text(item.summary) + "</b>");
        title->set_xalign(0.0f);
        title->set_wrap(true);
        row->append(*title);

        // The hold reason, given equal weight. This is the sentence that decides
        // whether you accept, and burying it would make the buttons a coin flip.
        if (!item.reason.empty()) {
            auto* reason = Gtk::make_managed<Gtk::Label>();
            reason->set_markup("<i>" + Glib::Markup::escape_text(item.reason) + "</i>");
            reason->set_xalign(0.0f);
            reason->set_wrap(true);
            row->append(*reason);
        }

        auto* files = Gtk::make_managed<Gtk::Label>(
            std::to_string(item.files) + (item.files == 1 ? " file" : " files"));
        files->set_xalign(0.0f);
        files->add_css_class("subtitle");
        row->append(*files);

        auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        actions->set_halign(Gtk::Align::END);

        auto* accept = Gtk::make_managed<Gtk::Button>("Accept");
        accept->add_css_class("suggested-action");
        const int n = item.n;
        accept->signal_clicked().connect([this, n] { decide(n, true); });

        auto* discard = Gtk::make_managed<Gtk::Button>("Discard");
        discard->signal_clicked().connect([this, n] { decide(n, false); });

        actions->append(*discard);
        actions->append(*accept);
        row->append(*actions);

        list_.append(*row);
        rows_.push_back(std::move(row));
    }
}

void BoardWindow::decide(int n, bool accept) {
    const auto argv = accept ? crew_accept_command(n) : crew_discard_command(n);
    if (argv.empty()) return;
    spawn_detached(argv);

    // Re-read shortly after rather than immediately: the decision is applied by a
    // separate process, and reading the board before it has written would show the
    // change still pending and invite a second click on something already actioned.
    Glib::signal_timeout().connect_once([this] { refresh(); }, 1200);
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
    set_default_size(720, 720);
    set_hide_on_close(true);

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
        "Finish an interrupted run: keep what is done, re-plan what is left");
    resume_.signal_clicked().connect([this] {
        if (!spawn_detached(crew_resume_command())) {
            status_.set_text("Could not reach the crew");
            return;
        }
        status_.set_text("Resuming the most recent run…");
    });

    start_row_.append(start_);
    start_row_.append(resume_);

    // ---- the options ----
    route_.set_tooltip_text("Pick each role's model by how hard its subtask is");
    debate_.set_tooltip_text("An advocate, a skeptic and a judge vote on every changeset");
    dedupe_.set_tooltip_text("Hold a coder whose work duplicates another's");
    learn_.set_tooltip_text("Remember what this run teaches, for the next one");
    options_.append(route_);
    options_.append(debate_);
    options_.append(dedupe_);
    options_.append(learn_);

    coders_label_.set_text("Coders");
    coders_.set_range(0, 12);
    coders_.set_increments(1, 2);
    // Zero shows as "default" rather than as a cap of no coders, and is what leaves
    // the engine's own number alone.
    coders_.set_value(0);
    coders_.set_tooltip_text("0 leaves ollamadev's own default alone");

    pack_label_.set_text("Pack");
    packs_ = crew_packs();
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
    second_row_.append(pack_label_);
    second_row_.append(pack_);

    // ---- what it is doing ----
    run_heading_.set_xalign(0.0f);
    run_heading_.add_css_class("subtitle");
    run_scroller_.set_child(run_box_);
    run_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    run_scroller_.set_max_content_height(200);
    run_scroller_.set_propagate_natural_height(true);

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
    root_.append(task_label_);
    root_.append(task_);
    root_.append(options_);
    root_.append(second_row_);
    root_.append(start_row_);
    root_.append(run_heading_);
    root_.append(run_scroller_);
    root_.append(board_heading_);
    root_.append(board_scroller_);
    root_.append(status_);
    set_child(root_);

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

void CrewWindow::start() {
    CrewOptions options;
    options.route      = route_.get_active();
    options.debate     = debate_.get_active();
    options.dedupe     = dedupe_.get_active();
    options.learn      = learn_.get_active();
    options.max_coders = coders_.get_value_as_int();

    // The pack is taken by INDEX into the list the engine gave us, never from typed
    // text, so an unknown name cannot reach the command line -- ollamadev would
    // treat it as a prompt rather than refusing it.
    if (const auto index = pack_.get_selected(); index > 0 && index <= packs_.size()) {
        options.pack = packs_[index - 1];
    }

    const auto argv = crew_run_command(std::string(task_.get_text()), options);
    if (argv.empty()) {
        status_.set_text("Give the crew something to do first");
        return;
    }

    if (!spawn_detached(argv)) {
        status_.set_text("Could not start ollamadev");
        return;
    }

    // Detached rather than held: a crew run outlasts this window, and a run that
    // died because its window was closed would be the worst possible behaviour for
    // something that edits files.
    status_.set_text("Started. Progress appears below as the Director plans.");
    task_.set_text("");
}

void CrewWindow::refresh_run() {
    const auto path = crew_state_path();
    if (path.empty()) return;

    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        run_heading_.set_text("No crew has run here yet.");
        run_scroller_.set_visible(false);
        return;
    }
    if (have_run_mtime_ && stamp == run_mtime_) return;
    run_mtime_      = stamp;
    have_run_mtime_ = true;

    while (Gtk::Widget* child = run_box_.get_first_child()) run_box_.remove(*child);
    run_rows_.clear();

    const CrewRun run = current_crew_run(path);
    const std::string label = crew_status_label(run);

    run_heading_.set_text(label.empty()
                              ? (run.task.empty() ? "The crew is idle."
                                                  : "Idle. Last task: " + run.task)
                              : label + " — " + run.task);
    run_scroller_.set_visible(!run.subtasks.empty());

    for (const auto& subtask : run.subtasks) {
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

        // The state in words rather than a colour alone: "done" and "running" are
        // the two things being looked for, and a dot cannot be read at a glance
        // without a legend.
        auto* state = Gtk::make_managed<Gtk::Label>(
            subtask.state.empty() ? "?" : subtask.state);
        state->set_xalign(0.0f);
        state->set_size_request(80, -1);
        state->add_css_class(subtask.state == "done" ? "subtitle" : "recording");

        auto* who = Gtk::make_managed<Gtk::Label>(subtask.role);
        who->set_xalign(0.0f);
        who->set_size_request(90, -1);
        who->add_css_class("subtitle");

        auto* what = Gtk::make_managed<Gtk::Label>(subtask.title);
        what->set_xalign(0.0f);
        what->set_hexpand(true);
        what->set_wrap(true);

        row->append(*state);
        row->append(*who);
        row->append(*what);
        run_box_.append(*row);
        run_rows_.push_back(std::move(row));
    }
}

void CrewWindow::refresh_board() {
    const auto path = board_state_path();
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

    const auto items = board_items();
    if (items.empty()) {
        board_heading_.set_text("Nothing is being held for review.");
        board_scroller_.set_visible(false);
        return;
    }

    board_heading_.set_text(std::to_string(items.size()) +
                            (items.size() == 1 ? " change held for review"
                                               : " changes held for review"));
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

        auto* files = Gtk::make_managed<Gtk::Label>(
            std::to_string(item.files) + (item.files == 1 ? " file" : " files"));
        files->set_xalign(0.0f);
        files->add_css_class("subtitle");

        auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_halign(Gtk::Align::END);
        auto* accept = Gtk::make_managed<Gtk::Button>("Accept");
        accept->add_css_class("suggested-action");
        auto* discard = Gtk::make_managed<Gtk::Button>("Discard");
        const int n = item.n;
        accept->signal_clicked().connect([this, n] { decide(n, true); });
        discard->signal_clicked().connect([this, n] { decide(n, false); });
        buttons->append(*discard);
        buttons->append(*accept);

        row->append(*summary);
        if (!item.reason.empty()) row->append(*reason);
        row->append(*files);
        row->append(*buttons);

        board_box_.append(*row);
        board_rows_.push_back(std::move(row));
    }
}

void CrewWindow::decide(int n, bool accept) {
    // Checked against the board that actually exists, not trusted from the button.
    // The buttons are built from a real board so this cannot normally fail -- but
    // the board can change between drawing a row and pressing it, and accepting the
    // wrong changeset is not something to leave to timing.
    const auto items = board_items();
    if (!board_item(items, n)) {
        status_.set_text("Change " + std::to_string(n) + " is no longer on the board");
        have_board_mtime_ = false;
        refresh_board();
        return;
    }

    const auto argv = accept ? crew_accept_command(n) : crew_discard_command(n);
    if (argv.empty() || !spawn_detached(argv)) {
        status_.set_text("Could not reach the crew");
        return;
    }

    status_.set_text((accept ? "Accepting change " : "Discarding change ") +
                     std::to_string(n));
    // Forced, because accepting one changeset can release or invalidate another and
    // the file may not have been rewritten yet.
    have_board_mtime_ = false;
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
