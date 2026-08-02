#include "auspex/gtk/panel.hpp"

#include <set>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <giomm/file.h>
#include <glibmm/main.h>

#include <glibmm/markup.h>

#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/stringlist.h>

#include "auspex/crew.hpp"
#include "auspex/display.hpp"
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

// A task button's caption. Minimised windows are bracketed, which is what
// xfce4-panel does and what makes the list readable at a glance. Elision happens
// before the brackets are added, so a long title cannot push them off the button.
std::string label_for(const std::string& title, bool minimized) {
    const std::string shown = elide(title, minimized ? 28 : 30);
    return minimized ? "[" + shown + "]" : shown;
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
        button->set_tooltip_text("Switch to workspace " + std::to_string(index + 1));
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

    menu_.set_parent(*this);
    menu_.set_child(menu_box_);
    menu_.set_has_arrow(false);
    menu_box_.set_margin(6);

    poll();
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        1000);
}

void WindowList::toggle(const std::string& window_id) {
    // Minimise only what is already focused. Anything else gets raised, which also
    // de-iconifies it -- so one click always does the thing you can see is missing.
    if (const auto focused = focused_window_id();
        focused && canonical_window_id(*focused) == canonical_window_id(window_id)) {
        minimize_window(window_id);
        return;
    }
    if (on_restore_) on_restore_(window_id);
    else             restore_window(window_id);
}

void WindowList::show_window_menu(const std::string& window_id, Gtk::Widget& anchor) {
    menu_target_ = window_id;

    while (Gtk::Widget* child = menu_box_.get_first_child()) menu_box_.remove(*child);

    const auto add = [this](const std::string& label, sigc::slot<void()> action) {
        auto* button = Gtk::make_managed<Gtk::Button>(label);
        button->set_has_frame(false);
        if (auto* text = dynamic_cast<Gtk::Label*>(button->get_child())) {
            text->set_xalign(0.0f);
        }
        button->signal_clicked().connect([this, action] {
            menu_.popdown();
            action();
        });
        menu_box_.append(*button);
    };

    const std::string id = window_id;
    add("Bring to front", [this, id] {
        if (on_restore_) on_restore_(id);
        else             restore_window(id);
    });
    add("Full window",    [this, id] {
        if (on_full_) on_full_(id);
        else {
            restore_window(id);
            maximize_window(id);
        }
    });
    add("Minimise",       [id] { minimize_window(id); });

    auto* separator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    menu_box_.append(*separator);

    // Close, not kill. wmctrl -c sends WM_DELETE_WINDOW, so the application gets to
    // prompt about unsaved work. A task list that force-killed processes would
    // eventually cost somebody a document.
    add("Close", [id] { close_window(id); });

    // Anchored to the button that was right-clicked, so the menu appears where the
    // pointer is rather than wherever the popover was last used.
    Gtk::Widget* parent = menu_.get_parent();
    if (parent != nullptr) {
        double x = 0, y = 0;
        if (anchor.translate_coordinates(*parent, 0, 0, x, y)) {
            menu_.set_pointing_to(Gdk::Rectangle(
                static_cast<int>(x), static_cast<int>(y),
                anchor.get_width(), anchor.get_height()));
        }
    }
    menu_.popup();
}

void WindowList::poll() {
    const auto windows = list_user_windows();

    // Two calls for the whole list rather than two per window.
    std::set<std::string> visible;
    for (const auto& id : list_visible_windows()) visible.insert(id);
    const auto focused = focused_window_id();
    const std::string focused_id = focused ? canonical_window_id(*focused) : std::string{};

    std::map<std::string, bool> seen;
    for (const auto& window : windows) {
        seen[window.id] = true;

        const std::string canonical = canonical_window_id(window.id);
        const bool minimized = visible.count(canonical) == 0;
        const bool active    = !focused_id.empty() && canonical == focused_id;

        auto it = entries_.find(window.id);
        if (it == entries_.end()) {
            Entry entry;
            entry.title     = window.title;
            entry.minimized = minimized;
            entry.active    = active;
            entry.button = std::make_unique<Gtk::Button>(label_for(window.title, minimized));
            // The full title. The button shows at most thirty characters, so for
            // anything longer the hover text is the only place it exists.
            entry.button->set_tooltip_text(window.title);
            const std::string id = window.id;
            entry.button->signal_clicked().connect([this, id] { toggle(id); });

            auto right_click = Gtk::GestureClick::create();
            right_click->set_button(GDK_BUTTON_SECONDARY);
            Gtk::Button* button_ptr = entry.button.get();
            right_click->signal_pressed().connect(
                [this, id, button_ptr](int, double, double) {
                    show_window_menu(id, *button_ptr);
                });
            entry.button->add_controller(right_click);
            if (active) entry.button->add_css_class("active-workspace");
            append(*entry.button);
            entries_.emplace(window.id, std::move(entry));
            continue;
        }

        Entry& entry = it->second;
        if (entry.title != window.title || entry.minimized != minimized) {
            entry.title     = window.title;
            entry.minimized = minimized;
            // Kept in step with the label, or the hover text goes on describing the
            // page a browser tab used to be on.
            entry.button->set_tooltip_text(window.title);
            // Minimised windows are shown in brackets, as xfce4-panel does. Without
            // some mark, a minimised window and an open one look identical and the
            // task list stops telling you anything.
            entry.button->set_label(label_for(window.title, minimized));
        }
        if (entry.active != active) {
            entry.active = active;
            if (active) entry.button->add_css_class("active-workspace");
            else        entry.button->remove_css_class("active-workspace");
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
// The readout is four numbers with no units and no explanation of what they are
// measured against, which is fine once you know and opaque until then.
SystemMonitorWidget::SystemMonitorWidget() : Gtk::Box(Gtk::Orientation::HORIZONTAL, 4) {
    label_.add_css_class("monitor-label");
    // Four numbers with no units. Obvious once you know, opaque until then.
    set_tooltip_text("Processor, memory, graphics and video memory in use");
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
Clock::Clock(const Config& config)
    : config_(config), clock_24_hour_(config.clock_24_hour) {
    add_css_class("clock-label");
    add_css_class("flat");
    set_has_frame(false);
    set_child(label_);

    note_heading_.set_xalign(0.0f);
    note_heading_.add_css_class("subtitle");

    note_scroller_.set_child(note_box_);
    note_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    note_scroller_.set_max_content_height(160);
    note_scroller_.set_propagate_natural_height(true);

    today_.set_tooltip_text("Jump back to today");
    open_calendar_.set_tooltip_text("Open the full calendar");
    open_calendar_.signal_clicked().connect([this] {
        popover_.popdown();
        if (on_open_) on_open_();
    });

    popover_box_.set_margin(10);
    popover_box_.append(calendar_);
    popover_box_.append(today_);
    popover_box_.append(note_rule_);
    popover_box_.append(note_heading_);
    popover_box_.append(note_scroller_);
    popover_box_.append(open_calendar_);
    popover_.set_child(popover_box_);

    // Selecting a day changes which notes are shown; changing month changes which
    // days are marked. Both are needed -- GTK reports them separately.
    calendar_.signal_day_selected().connect([this] { reload_notes(); });
    calendar_.signal_prev_month().connect([this] { refresh_marks(); });
    calendar_.signal_next_month().connect([this] { refresh_marks(); });
    calendar_.signal_prev_year().connect([this] { refresh_marks(); });
    calendar_.signal_next_year().connect([this] { refresh_marks(); });

    // Upward, like every other popover on the bottom bar -- except this one is on
    // the TOP bar, so it opens downwards. Stated rather than defaulted because the
    // two bars are built from the same class and the difference is easy to lose.
    set_direction(Gtk::ArrowType::DOWN);
    set_popover(popover_);

    // Opened on today, always. A calendar left on last March because that is where
    // it was when you closed it is a calendar you have to reset before reading.
    //
    // Re-read from disk on every open rather than trusting what is in memory: the
    // notes file is plain JSON in the user's own data directory and editing it by
    // hand is a perfectly reasonable thing to do.
    popover_.signal_show().connect([this] {
        notes_ = EventStore::load();
        calendar_.select_day(Glib::DateTime::create_now_local());
        reload_notes();
    });
    today_.signal_clicked().connect(
        [this] { calendar_.select_day(Glib::DateTime::create_now_local()); });

    tick();
    Glib::signal_timeout().connect(
        [this] {
            tick();
            return true;
        },
        1000);
}

void Clock::refresh_marks() {
    calendar_.clear_marks();

    const auto shown = calendar_.get_date();
    for (const auto& [day, count] : notes_.counts_in_month(shown.get_year(),
                                                          shown.get_month())) {
        (void)count;
        calendar_.mark_day(static_cast<guint>(day));
    }
}

void Clock::reload_notes() {
    const auto shown = calendar_.get_date();
    selected_date_ = format_date(shown.get_year(), shown.get_month(),
                                 shown.get_day_of_month());

    refresh_marks();

    while (Gtk::Widget* child = note_box_.get_first_child()) note_box_.remove(*child);
    note_rows_.clear();

    // Read-only here. Adding and removing live in the calendar window, where there
    // is room to show a time next to a title without the popover becoming a form.
    const auto day = notes_.on(selected_date_);
    note_heading_.set_text(day.empty() ? selected_date_ + "  \u2014  nothing on"
                                       : selected_date_);
    note_scroller_.set_visible(!day.empty());

    for (const auto& occurrence : day) {
        const auto& event = occurrence.event;
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

        auto* when = Gtk::make_managed<Gtk::Label>(
            event.start.empty() ? "All day" : event.start);
        when->set_xalign(0.0f);
        when->set_size_request(56, -1);
        when->add_css_class("subtitle");

        auto* what = Gtk::make_managed<Gtk::Label>(
            occurrence.repeating ? event.title + "  \u21bb" : event.title);
        what->set_xalign(0.0f);
        what->set_hexpand(true);
        what->set_wrap(true);
        what->set_max_width_chars(26);

        row->append(*when);
        row->append(*what);
        note_box_.append(*row);
        note_rows_.push_back(std::move(row));
    }
}

void Clock::refresh_format() {
    // Same shape as the theme watcher: compare the modification time and only read
    // the file when it has moved. Saving Settings rewrites config.json, so this is
    // what makes the checkbox take effect without restarting the shell.
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(Config::default_path(), ec);
    // Missing or half-written is expected while something else is saving it; try
    // again on the next tick rather than reporting anything.
    if (ec || mtime == config_mtime_) return;
    config_mtime_ = mtime;

    clock_24_hour_ = Config::load().clock_24_hour;
}

void Clock::tick() {
    refresh_format();

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return;

    // %I is the 12-hour hour and %p the AM/PM marker; both come from the C library
    // rather than being assembled here, so a locale that writes "p.m." gets its own
    // spelling instead of an English one.
    const char* format = clock_24_hour_ ? "%Y-%m-%d %H:%M:%S"
                                        : "%Y-%m-%d %I:%M:%S %p";

    char buffer[64];
    if (std::strftime(buffer, sizeof(buffer), format, &local) > 0) {
        label_.set_text(buffer);
    }

    // The long form, which is the thing the short form on the panel leaves out --
    // the day of the week is what people are usually squinting at a date for.
    char full[128];
    if (std::strftime(full, sizeof(full), "%A %e %B %Y", &local) > 0) {
        set_tooltip_text(std::string(full) + "\nClick for the calendar");
    }
}

// ---------------------------------------------------------------------------
// PinnedLaunchers
// ---------------------------------------------------------------------------
PinnedLaunchers::PinnedLaunchers(const Config& config)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, 1), config_(config) {
    reload();
}

void PinnedLaunchers::reload() {
    while (Gtk::Widget* child = get_first_child()) remove(*child);
    buttons_.clear();

    for (const auto& id : config_.pinned) {
        // Resolved against the installed applications rather than trusted. A pin
        // whose application has been uninstalled is skipped, not turned into a
        // button that fails silently when pressed.
        const auto entry = find_desktop_entry(id);
        if (!entry) continue;

        auto button = std::make_unique<Gtk::Button>();
        button->set_has_frame(false);
        button->add_css_class("flat");
        button->set_tooltip_text(entry->name);

        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name(entry->icon.empty() ? "application-x-executable"
                                                     : entry->icon);
        icon->set_pixel_size(std::max(16, config_.panel_height - 10));
        button->set_child(*icon);

        const std::string command = entry->exec;
        const bool in_terminal = entry->terminal;
        const std::string terminal = config_.terminal;
        button->signal_clicked().connect([command, in_terminal, terminal] {
            // Field codes are already stripped by the parser, so this is a plain
            // argv split and never a shell.
            if (in_terminal && !terminal.empty()) {
                auto argv = split_words(terminal);
                argv.push_back("-e");
                for (auto& word : split_words(command)) argv.push_back(word);
                spawn_detached(argv);
            } else {
                spawn_detached(split_words(command));
            }
        });

        append(*button);
        buttons_.push_back(std::move(button));
    }

    // Nothing pinned is not an empty gap in the panel.
    set_visible(!buttons_.empty());
}

// ---------------------------------------------------------------------------
// NotificationButton
// ---------------------------------------------------------------------------
NotificationButton::NotificationButton() {
    add_css_class("flat");
    set_has_frame(false);
    set_icon_name("preferences-system-notifications-symbolic");
    set_tooltip_text("Notifications");

    heading_.set_xalign(0.0f);
    heading_.add_css_class("subtitle");

    scroller_.set_child(list_);
    scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller_.set_max_content_height(320);
    scroller_.set_propagate_natural_height(true);
    scroller_.set_size_request(320, -1);

    // Only offered when a daemon whose switch we know how to reach is running.
    // A toggle that silently does nothing is worse than no toggle.
    dnd_.set_visible(dnd_supported());
    dnd_.set_active(dnd_supported() && dnd_enabled());
    dnd_.signal_toggled().connect([this] {
        if (!set_dnd(dnd_.get_active())) {
            // Put it back rather than showing a state the daemon does not have.
            dnd_.set_active(dnd_enabled());
        }
    });

    clear_.signal_clicked().connect([this] {
        log_.clear();
        rebuild();
    });

    actions_.append(dnd_);
    actions_.append(clear_);

    popover_box_.set_margin(10);
    popover_box_.append(heading_);
    popover_box_.append(scroller_);
    popover_box_.append(actions_);
    popover_.set_child(popover_box_);

    set_direction(Gtk::ArrowType::DOWN);
    set_popover(popover_);
    popover_.signal_show().connect([this] {
        if (dnd_supported()) dnd_.set_active(dnd_enabled());
        log_.mark_seen();
        rebuild();
        remove_css_class("recording");
    });

    start_monitor();
    rebuild();
}

NotificationButton::~NotificationButton() {
    // The filter is removed with the connection; closing it is what stops the
    // worker thread that would otherwise call back into a destroyed widget.
    if (monitor_ != nullptr) {
        g_dbus_connection_close_sync(monitor_, nullptr, nullptr);
        g_object_unref(monitor_);
    }
}

void NotificationButton::start_monitor() {
    // A private connection, not the shared session bus.
    //
    // BecomeMonitor puts the CONNECTION into monitor mode: it can then receive
    // everything and call nothing. Doing that to the bus the tray and the rest of
    // the shell are using would break all of them.
    GError* error = nullptr;
    gchar*  address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr,
                                                      &error);
    if (address == nullptr) {
        if (error != nullptr) g_error_free(error);
        return;
    }

    monitor_ = g_dbus_connection_new_for_address_sync(
        address,
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    g_free(address);

    if (monitor_ == nullptr) {
        if (error != nullptr) g_error_free(error);
        return;
    }

    // Watch the method CALL, not a signal: Notify is a call an application makes to
    // the daemon, and there is no signal announcing that a notification was shown.
    GVariantBuilder rules;
    g_variant_builder_init(&rules, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&rules, "s",
                          "type='method_call',interface='org.freedesktop.Notifications',"
                          "member='Notify'");

    GVariant* reply = g_dbus_connection_call_sync(
        monitor_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus.Monitoring", "BecomeMonitor",
        g_variant_new("(asu)", &rules, static_cast<guint32>(0)), nullptr,
        G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);

    if (reply == nullptr) {
        // Some bus configurations refuse monitoring. The log is then simply empty,
        // which is a missing feature rather than a broken panel.
        if (error != nullptr) g_error_free(error);
        g_object_unref(monitor_);
        monitor_ = nullptr;
        return;
    }
    g_variant_unref(reply);

    // A message FILTER, not a signal subscription.
    //
    // Notify is a method CALL an application makes to the daemon, and
    // g_dbus_connection_signal_subscribe only ever dispatches signals -- so a
    // subscription here sees the monitored traffic go past and never fires once.
    // A filter sees every message on the connection, which is what a monitor is
    // for. Measured: the bell stayed empty through every test notification until
    // this was changed.
    g_dbus_connection_add_filter(
        monitor_,
        [](GDBusConnection*, GDBusMessage* message, gboolean incoming,
           gpointer user_data) -> GDBusMessage* {
            auto* self = static_cast<NotificationButton*>(user_data);
            if (!incoming || message == nullptr) return message;

            if (g_dbus_message_get_message_type(message) !=
                G_DBUS_MESSAGE_TYPE_METHOD_CALL) {
                return message;
            }
            const gchar* interface = g_dbus_message_get_interface(message);
            const gchar* member    = g_dbus_message_get_member(message);
            if (interface == nullptr || member == nullptr) return message;
            if (g_strcmp0(interface, "org.freedesktop.Notifications") != 0) return message;
            if (g_strcmp0(member, "Notify") != 0) return message;

            GVariant* body = g_dbus_message_get_body(message);
            if (body == nullptr || g_variant_n_children(body) < 5) return message;

            const auto text_at = [body](gsize index) -> std::string {
                GVariant* child = g_variant_get_child_value(body, index);
                std::string value;
                if (g_variant_is_of_type(child, G_VARIANT_TYPE_STRING)) {
                    value = g_variant_get_string(child, nullptr);
                }
                g_variant_unref(child);
                return value;
            };

            Notification notification;
            notification.app     = text_at(0);
            notification.summary = text_at(3);
            notification.body    = text_at(4);

            const std::time_t now = std::time(nullptr);
            std::tm local{};
            char stamp[16] = {};
            if (localtime_r(&now, &local) &&
                std::strftime(stamp, sizeof(stamp), "%H:%M", &local) > 0) {
                notification.when = stamp;
            }

            // This runs on the connection's own worker thread, so nothing here may
            // touch a widget or the log directly. g_idle_add is thread-safe and is
            // what carries it back to the GTK thread.
            Glib::signal_idle().connect_once([self, notification] {
                self->log_.add(notification);
                if (self->log_.unseen() > 0) self->add_css_class("recording");
            });

            // A monitor must not consume what it sees; returning the message
            // unchanged is what lets the daemon still receive it.
            return message;
        },
        this, nullptr);
}

void NotificationButton::rebuild() {
    while (Gtk::Widget* child = list_.get_first_child()) list_.remove(*child);
    rows_.clear();

    const auto recent = log_.recent();
    heading_.set_text(recent.empty() ? "No notifications"
                                     : std::to_string(recent.size()) + " recent");
    scroller_.set_visible(!recent.empty());
    clear_.set_sensitive(!recent.empty());

    for (const auto& notification : recent) {
        auto row = std::make_unique<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

        auto* top = Gtk::make_managed<Gtk::Label>();
        top->set_markup("<b>" + Glib::Markup::escape_text(notification.summary) +
                        "</b>");
        top->set_xalign(0.0f);
        top->set_wrap(true);
        top->set_max_width_chars(34);

        auto* who = Gtk::make_managed<Gtk::Label>(
            notification.app + (notification.when.empty() ? ""
                                                          : "  " + notification.when));
        who->set_xalign(0.0f);
        who->add_css_class("subtitle");

        row->append(*who);
        row->append(*top);

        if (!notification.body.empty()) {
            auto* body = Gtk::make_managed<Gtk::Label>(notification.body);
            body->set_xalign(0.0f);
            body->set_wrap(true);
            body->set_max_width_chars(34);
            row->append(*body);
        }

        list_.append(*row);
        rows_.push_back(std::move(row));
    }
}

// ---------------------------------------------------------------------------
// TerminalButton
// ---------------------------------------------------------------------------
TerminalButton::TerminalButton(const Config& config) : config_(config) {
    icon_.set_from_icon_name("utilities-terminal-symbolic");
    set_child(icon_);
    set_tooltip_text("Open a terminal — hover to pick a folder and an agent");

    // The old behaviour, unchanged. Somebody who just wants a terminal should not
    // have to wait out a hover delay or read a menu to get one.
    signal_clicked().connect([this] { launch_plain(); });

    // ---- the folder ----
    folder_label_.set_text("Folder");
    folder_label_.set_xalign(0.0f);
    folder_label_.add_css_class("subtitle");

    folder_path_.set_xalign(0.0f);
    folder_path_.add_css_class("subtitle");
    folder_path_.set_ellipsize(Pango::EllipsizeMode::START);
    folder_path_.set_max_width_chars(34);

    folders_.property_selected().signal_changed().connect([this] {
        const auto path = chosen();
        folder_path_.set_text(path.string());
        status_.set_text({});
    });

    browse_.signal_clicked().connect([this] { browse(); });

    popover_box_.set_margin(10);
    popover_box_.append(folder_label_);
    popover_box_.append(folders_);
    popover_box_.append(folder_path_);
    popover_box_.append(browse_);
    popover_box_.append(rule_);
    popover_box_.append(agent_box_);

    status_.set_xalign(0.0f);
    status_.add_css_class("subtitle");
    status_.set_wrap(true);
    status_.set_max_width_chars(34);
    popover_box_.append(status_);

    popover_.set_child(popover_box_);
    popover_.set_parent(*this);
    popover_.set_position(Gtk::PositionType::TOP);   // a bottom-panel button

    // Autohide, and it has to be.
    //
    // A GTK4 popover is only its OWN window when it autohides. Without that it is
    // drawn inside its parent's surface -- and this parent is a dock 42 pixels
    // tall, so a menu six hundred pixels high is clipped to nothing and the button
    // looks broken. Turning autohide off to avoid the grab made the popover stop
    // appearing at all.
    //
    // The grab is therefore a fact to work around rather than avoid, and it costs
    // two things. It swallows the panel's own right-click, so a popover left open
    // makes the first right-click merely dismiss it; and it makes the BUTTON emit
    // crossing events the pointer never caused, so neither its `leave` nor its
    // contains_pointer() can be trusted while the popover is up.
    //
    // Both are answered by closing promptly and by asking the POPOVER where the
    // pointer is -- it owns the grab, so it is the one widget whose answer is
    // reliable. See schedule_close().
    popover_.set_autohide(true);

    // ---- hover ----
    on_button_ = Gtk::EventControllerMotion::create();
    on_button_->signal_enter().connect([this](double, double) {
        cancel_close();
        if (open_.connected() || popover_.get_visible()) return;
        open_ = Glib::signal_timeout().connect(
            [this] {
                // Rebuilt on every open rather than once: agents get installed and
                // folders get bookmarked while the panel is running, and a list
                // that only reflects login time is one you learn to distrust.
                rebuild();
                popover_.popup();
                // Armed as it opens. From here the pointer has about a second to
                // move up into the menu; if it does not, the menu was not wanted.
                schedule_close();
                return false;
            },
            300);
    });
    on_button_->signal_leave().connect([this] {
        // Only a PENDING open is cancelled here. Once the popover is up this same
        // signal fires because of the grab rather than because the pointer moved,
        // so it is not evidence of anything and must not close it.
        if (open_.connected()) open_.disconnect();
    });
    add_controller(on_button_);

    on_popover_ = Gtk::EventControllerMotion::create();
    popover_box_.add_controller(on_popover_);
}

void TerminalButton::cancel_close() {
    if (close_.connected()) close_.disconnect();
}

void TerminalButton::schedule_close() {
    cancel_close();

    // Polled rather than driven by a leave event, because the only widget whose
    // crossing state is trustworthy here is the popover itself -- it holds the
    // grab. Two consecutive ticks with the pointer outside it, so that skimming
    // along the popover's edge does not close it mid-reach.
    //
    // Closing promptly matters more than usual: while this is up its grab
    // swallows the panel's right-click, so a menu left open is a panel whose own
    // menu appears not to work.
    close_ = Glib::signal_timeout().connect(
        [this, missed = 0]() mutable {
            if (!popover_.get_visible()) return false;

            if (on_popover_ && on_popover_->contains_pointer()) {
                missed = 0;
                return true;
            }
            if (++missed < 2) return true;

            popover_.popdown();
            return false;
        },
        500);
}

TerminalButton::~TerminalButton() {
    // The popover is parented to this widget rather than owned by it, so it has to
    // be unparented explicitly or GTK warns about a finalized widget with children.
    if (open_.connected()) open_.disconnect();
    if (close_.connected()) close_.disconnect();
    popover_.unparent();
}

std::filesystem::path TerminalButton::chosen() const {
    const auto index = folders_.get_selected();
    if (index == GTK_INVALID_LIST_POSITION || index >= projects_.size()) return {};
    return projects_[index].path;
}

void TerminalButton::rebuild() {
    // Keep what was selected across a rebuild, or picking a folder and then
    // hovering again would silently move you back to the top of the list.
    const auto previous = chosen();

    projects_ = all_projects();
    {
        std::vector<Glib::ustring> labels;
        for (const auto& project : projects_) labels.emplace_back(project.name);
        if (labels.empty()) labels.emplace_back("No projects yet");
        folders_.set_model(Gtk::StringList::create(labels));
    }

    guint restore = 0;
    for (std::size_t i = 0; i < projects_.size(); ++i) {
        if (normal_project_path(projects_[i].path) == normal_project_path(previous)) {
            restore = static_cast<guint>(i);
            break;
        }
    }
    // Nothing remembered yet: start where ollamadev thinks the current project is,
    // so the panel and the engine agree before anything is clicked.
    if (previous.empty()) {
        if (const auto preferred = default_project()) {
            for (std::size_t i = 0; i < projects_.size(); ++i) {
                if (normal_project_path(projects_[i].path) ==
                    normal_project_path(preferred->path)) {
                    restore = static_cast<guint>(i);
                    break;
                }
            }
        }
    }
    folders_.set_selected(restore);
    folder_path_.set_text(chosen().string());

    const bool have_folder = !projects_.empty();
    folders_.set_sensitive(have_folder);

    // ---- the agents ----
    while (Gtk::Widget* child = agent_box_.get_first_child()) agent_box_.remove(*child);
    agent_rows_.clear();

    auto* plain = Gtk::make_managed<Gtk::Button>("Terminal here");
    plain->set_has_frame(false);
    if (auto* text = dynamic_cast<Gtk::Label*>(plain->get_child())) text->set_xalign(0.0f);
    // "Here" means the folder named just above it, with no dialog. It is the one
    // row whose label already answers the question the others have to ask.
    plain->signal_clicked().connect([this] {
        cancel_close();
        popover_.popdown();
        launch_plain();
    });
    agent_box_.append(*plain);

    // Only what is installed, and found by resolve_agent_binary() rather than by
    // $PATH alone -- the panel's PATH is the login one, which does not include the
    // trees nvm, bun, deno and cargo install into. See agents.hpp.
    agents_ = available_agents();
    for (const auto& agent : agents_) {
        auto* button = Gtk::make_managed<Gtk::Button>(agent.label);
        button->set_has_frame(false);
        if (auto* text = dynamic_cast<Gtk::Label*>(button->get_child())) {
            text->set_xalign(0.0f);
        }
        button->set_tooltip_text(agent.path);
        button->set_sensitive(have_folder);
        const AgentTool copy = agent;
        // No popdown here: launch_agent() asks for the folder first, and doing it
        // there keeps the dismissal and the dialog in one place.
        button->signal_clicked().connect([this, copy] { launch_agent(copy); });
        agent_box_.append(*button);
    }

    if (agents_.empty()) {
        auto* none = Gtk::make_managed<Gtk::Label>("No coding agents found");
        none->set_xalign(0.0f);
        none->add_css_class("subtitle");
        agent_box_.append(*none);
    }

    // The crew and the engine's everyday flows, set apart below a rule.
    //
    // Everything above opens ONE agent that then waits for you. These go and do
    // something to the folder -- Ship pushes, Crew plans and edits -- so they do
    // not belong in the same run of identical rows, one slip of the mouse away
    // from "open a terminal".
    //
    // The set is ollamadev-qt's Start pane, minus the once-only entries: the two
    // front ends offer the same flows so neither is the odd one out. See
    // engine_actions() in crew.hpp.
    if (crew_available()) {
        auto* rule = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        rule->set_margin_top(4);
        rule->set_margin_bottom(4);
        agent_box_.append(*rule);

        const auto add_row = [this, have_folder](const std::string& label,
                                                 const std::string& tooltip,
                                                 sigc::slot<void()> action) {
            auto* button = Gtk::make_managed<Gtk::Button>(label);
            button->set_has_frame(false);
            if (auto* text = dynamic_cast<Gtk::Label*>(button->get_child())) {
                text->set_xalign(0.0f);
            }
            button->set_tooltip_text(tooltip);
            button->set_sensitive(have_folder);
            button->signal_clicked().connect(action);
            agent_box_.append(*button);
            return button;
        };

        // The crew opens its WINDOW rather than starting a run. A crew needs a
        // task, and nothing that edits files should begin from a hover menu -- so
        // the folder is confirmed in that window's own row rather than here.
        add_row("Crew…",
                "Open the crew bench: a Director plans the task, coders build it in "
                "worktrees, an Auditor reads every diff before it lands",
                [this] {
                    const auto folder = chosen();
                    cancel_close();
                    popover_.popdown();
                    if (!is_project_dir(folder) || !on_crew_) return;
                    remember_project(folder);
                    on_crew_(folder);
                });

        // Team asks for a folder the same way Crew does -- by opening its window
        // with one already chosen -- because a fan-out across four providers is
        // four agents editing a tree, and that is not a hover-menu decision.
        add_row("Team…",
                "One prompt, several providers at once — a terminal each",
                [this] {
                    const auto folder = chosen();
                    cancel_close();
                    popover_.popdown();
                    if (!is_project_dir(folder) || !on_team_) return;
                    remember_project(folder);
                    on_team_(folder);
                });

        // Brain has no folder: the router's tiers are a machine-wide preference,
        // not a property of a project.
        {
            auto* button = add_row("Brain…",
                                   "Which model each difficulty routes to, and where "
                                   "a given request would go",
                                   [this] {
                                       cancel_close();
                                       popover_.popdown();
                                       if (on_brain_) on_brain_();
                                   });
            button->set_sensitive(true);
        }

        for (const auto& action : engine_actions()) {
            add_row(action.label, action.tooltip, [this, action] {
                ask_folder([this, action](std::filesystem::path folder) {
                    const auto argv =
                        terminal_command_argv(config_.terminal, action.argv, folder);
                    if (argv.empty()) return;
                    spawn_detached(argv, folder.string());
                });
            });
        }
    }

    status_.set_text({});
}

void TerminalButton::launch_plain() {
    // The chosen folder when the popover has one, otherwise wherever the panel is
    // -- a bare left-click on a machine with no projects must still open a
    // terminal, which is what this button has always been for.
    const auto folder = chosen();
    const bool usable = is_project_dir(folder);

    const auto argv = usable ? terminal_here(config_.terminal, folder)
                             : terminal_here(config_.terminal, {});
    if (argv.empty()) return;
    spawn_detached(argv, usable ? folder.string() : std::string{});
    if (usable) remember_project(folder);
}

void TerminalButton::ask_folder(sigc::slot<void(std::filesystem::path)> then) {
    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) return;

    cancel_close();
    popover_.popdown();

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Which folder should it open in?");
    dialog->set_accept_label("Open here");
    // Seeded from the dropdown, so the common case is one Enter away and the
    // uncommon one is possible at all.
    if (const auto current = chosen(); is_project_dir(current)) {
        dialog->set_initial_folder(Gio::File::create_for_path(current.string()));
    }

    dialog->select_folder(
        *root, [this, dialog, then](const Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                const auto folder = dialog->select_folder_finish(result);
                if (!folder) return;
                const std::filesystem::path path = folder->get_path();
                if (!is_project_dir(path)) return;
                remember_project(path);
                then(path);
            } catch (const Glib::Error&) {
                // Cancelled, which means launch nothing.
            }
        });
}

void TerminalButton::launch_agent(const AgentTool& agent) {
    // Re-resolved before the dialog rather than after: there is no point asking
    // where to open something that is no longer installed.
    AgentTool fresh = agent;
    fresh.path = resolve_agent_binary(agent.binary);
    if (fresh.path.empty()) return;

    ask_folder([this, fresh](std::filesystem::path folder) {
        const auto argv = agent_terminal_command(config_.terminal, fresh, folder);
        if (argv.empty()) return;
        spawn_detached(argv, folder.string());
    });
}

void TerminalButton::browse() {
    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) return;

    // Dismissed first: the popover holds an input grab, and a file dialog opened
    // underneath one cannot be clicked.
    popover_.popdown();

    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Open a folder");
    if (const auto current = chosen(); is_project_dir(current)) {
        dialog->set_initial_folder(Gio::File::create_for_path(current.string()));
    }
    dialog->select_folder(*root, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            const auto folder = dialog->select_folder_finish(result);
            if (!folder) return;
            const std::filesystem::path path = folder->get_path();
            if (!is_project_dir(path)) return;

            // Remembered before the rebuild, so it is in the list that rebuild
            // then selects from.
            remember_project(path);
            rebuild();
            for (std::size_t i = 0; i < projects_.size(); ++i) {
                if (normal_project_path(projects_[i].path) == normal_project_path(path)) {
                    folders_.set_selected(static_cast<guint>(i));
                    break;
                }
            }
            folder_path_.set_text(chosen().string());
            popover_.popup();
        } catch (const Glib::Error&) {
            // Cancelled.
        }
    });
}

// ---------------------------------------------------------------------------
// CrewButton
// ---------------------------------------------------------------------------
CrewButton::CrewButton() {
    add_css_class("flat");
    set_has_frame(false);

    icon_.set_from_icon_name("system-run-symbolic");
    box_.append(icon_);
    box_.append(label_);
    set_child(box_);

    // Straight to the board. While a run is live the board is where its results
    // will land, and after it finishes the board is the only reason to care that it
    // ran at all.
    signal_clicked().connect([this] { if (on_board_) on_board_(); });

    // Hidden until there is something to say. A control that is permanently
    // present and permanently blank is worse than no control.
    set_visible(false);

    poll();
    // Two seconds. A crew subtask takes tens of seconds at best, so this is already
    // far finer than the thing it is watching, and it costs one stat when nothing
    // has changed.
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        2000);
}

void CrewButton::poll() {
    const auto path = crew_state_path();
    if (path.empty()) {
        set_visible(false);
        return;
    }

    // The modification time first: a run that has not changed costs a stat rather
    // than opening and parsing the file every two seconds for the whole session.
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        // No file at all means no crew has ever run here, which is not a state
        // worth showing anything for.
        set_visible(false);
        return;
    }
    if (have_mtime_ && stamp == mtime_) return;
    mtime_      = stamp;
    have_mtime_ = true;

    const CrewRun run = current_crew_run(path);
    if (run == last_) return;
    last_ = run;

    const std::string label = crew_status_label(run);
    set_visible(!label.empty());
    if (label.empty()) return;

    label_.set_text(label);
    set_tooltip_text(crew_status_detail(run));

    // The same class the recording controls use, so "something of mine is running"
    // looks the same wherever it appears on the panel.
    add_css_class("recording");
}

// ---------------------------------------------------------------------------
// VolumeButton
// ---------------------------------------------------------------------------
VolumeButton::VolumeButton()
    : slider_(Gtk::Adjustment::create(0, 0, 100, 1, 5, 0), Gtk::Orientation::HORIZONTAL) {
    add_css_class("flat");
    set_has_frame(false);

    slider_.set_size_request(180, -1);
    slider_.set_draw_value(true);
    slider_.set_value_pos(Gtk::PositionType::RIGHT);
    slider_.signal_value_changed().connect([this] {
        if (!adjusting_) return;   // a poll wrote this value, not the user
        set_volume(static_cast<int>(slider_.get_value()));
    });
    // The drag guard. Without it the one-second poll writes the sink's value back
    // into the slider halfway through a drag and the handle jumps under the hand.
    {
        auto press = Gtk::GestureClick::create();
        press->signal_pressed().connect([this](int, double, double) { adjusting_ = true; });
        press->signal_released().connect([this](int, double, double) { adjusting_ = false; });
        slider_.add_controller(press);
    }

    mute_.signal_clicked().connect([this] {
        set_muted(!state_.muted);
        poll();
    });

    popover_box_.set_margin(10);
    popover_box_.append(slider_);
    popover_box_.append(mute_);
    popover_.set_child(popover_box_);

    // Upward: this lives on the bottom bar, and a popover opening downwards from
    // there is placed below the screen edge and simply never seen.
    set_direction(Gtk::ArrowType::UP);
    set_popover(popover_);

    // Read the real level as it opens rather than trusting the last poll, so the
    // slider is never a second out of date at the moment you look at it.
    popover_.signal_show().connect([this] { poll(); });

    // Scrolling the button itself, which is what people do without opening anything.
    {
        auto scroll = Gtk::EventControllerScroll::create();
        scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
        scroll->signal_scroll().connect(
            [this](double, double dy) {
                if (!state_.known) return true;
                const int step = dy < 0 ? 5 : -5;
                apply(std::clamp(state_.percent + step, 0, 100), state_.muted);
                set_volume(state_.percent);
                return true;
            },
            false);
        add_controller(scroll);
    }

    poll();
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        1000);
}

void VolumeButton::poll() {
    const VolumeState now = current_volume();
    // Neither tool present: hide rather than show a slider that cannot do anything.
    set_visible(now.known);
    if (!now.known || now == state_) return;
    apply(now.percent, now.muted);
}

void VolumeButton::apply(int percent, bool muted) {
    state_.percent = percent;
    state_.muted   = muted;
    state_.known   = true;

    set_icon_name(volume_icon_name(state_));
    if (!adjusting_) slider_.set_value(percent);
    mute_.set_label(muted ? "Unmute" : "Mute");
    set_tooltip_text(muted ? "Muted" : "Volume " + std::to_string(percent) + "%");
}

// ---------------------------------------------------------------------------
// NetworkButton
// ---------------------------------------------------------------------------
NetworkButton::NetworkButton(const Config& config) : config_(config) {
    add_css_class("flat");
    set_has_frame(false);

    status_.set_xalign(0.0f);
    status_.set_max_width_chars(28);
    status_.set_wrap(true);

    // Opening the system's own network editor rather than reimplementing it. Joining
    // a wifi network means passwords, certificates and captive portals, and a panel
    // that did that badly would be worse than one that hands over to the tool built
    // for it.
    settings_.signal_clicked().connect([this] {
        popover_.popdown();
        if (!config_.network_command.empty()) {
            spawn_detached(split_words(config_.network_command));
        }
    });
    settings_.set_visible(!config_.network_command.empty());

    wifi_heading_.set_xalign(0.0f);
    wifi_heading_.add_css_class("subtitle");

    wifi_scroller_.set_child(wifi_box_);
    wifi_scroller_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    // Tall enough for a handful of networks, capped so a busy street does not give
    // the popover the height of the screen.
    wifi_scroller_.set_max_content_height(260);
    wifi_scroller_.set_propagate_natural_height(true);

    radio_.signal_clicked().connect([this] {
        set_wifi_radio(!wifi_radio_enabled());
        rebuild_networks();
    });

    popover_box_.set_margin(10);
    popover_box_.append(status_);
    popover_box_.append(wifi_heading_);
    popover_box_.append(wifi_scroller_);
    popover_box_.append(radio_);
    popover_box_.append(settings_);
    popover_.set_child(popover_box_);

    set_direction(Gtk::ArrowType::UP);
    set_popover(popover_);
    popover_.signal_show().connect([this] {
        poll();
        rebuild_networks();
    });

    poll();
    // Five seconds, not one. Network state changes on the scale of plugging a cable
    // in, and each poll is up to three nmcli processes -- the cost the panel's other
    // pollers were already trimmed for.
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        5000);
}

void NetworkButton::poll() {
    const NetworkState now = current_network();
    // nmcli absent: hide rather than show a permanently offline icon on a machine
    // whose network is fine.
    set_visible(now.known);
    if (!now.known || now == state_) return;
    state_ = now;

    set_icon_name(network_icon_name(state_));

    std::string summary;
    switch (state_.kind) {
        case NetworkState::Kind::Wired:
            summary = state_.connection.empty() ? "Wired" : state_.connection;
            break;
        case NetworkState::Kind::Wireless:
            summary = state_.connection.empty() ? "Wireless" : state_.connection;
            if (state_.signal_percent > 0) {
                summary += "  " + std::to_string(state_.signal_percent) + "%";
            }
            break;
        case NetworkState::Kind::None:
            summary = "Not connected";
            break;
    }
    // Connected without a route is the state worth naming: it is the difference
    // between the cable being out and the internet being down.
    if (state_.kind != NetworkState::Kind::None && !state_.online) {
        summary += "\nNo internet access";
    }

    status_.set_text(summary);
    set_tooltip_text(summary);
}

void NetworkButton::rebuild_networks() {
    while (Gtk::Widget* child = wifi_box_.get_first_child()) wifi_box_.remove(*child);
    wifi_rows_.clear();

    // A desktop with no radio is not shown an empty list and a switch that does
    // nothing -- it is simply not shown wifi at all.
    if (!has_wifi_device()) {
        wifi_heading_.set_visible(false);
        wifi_scroller_.set_visible(false);
        radio_.set_visible(false);
        return;
    }

    const bool on = wifi_radio_enabled();
    radio_.set_visible(true);
    radio_.set_label(on ? "Turn Wi-Fi off" : "Turn Wi-Fi on");

    if (!on) {
        // The radio being off is the reason the list is empty, and saying so is the
        // difference between a menu that looks broken and one that tells you what
        // to press.
        wifi_heading_.set_text("Wi-Fi is off");
        wifi_heading_.set_visible(true);
        wifi_scroller_.set_visible(false);
        return;
    }

    const auto networks = scan_wifi();
    wifi_heading_.set_visible(true);
    if (networks.empty()) {
        wifi_heading_.set_text("No networks in range");
        wifi_scroller_.set_visible(false);
        return;
    }

    wifi_heading_.set_text("Wi-Fi networks");
    wifi_scroller_.set_visible(true);

    for (const auto& network : networks) {
        auto row = std::make_unique<Gtk::Button>();
        auto* label = Gtk::make_managed<Gtk::Label>();

        std::string text = network.ssid;
        if (network.secured) text += "  \U0001F512";
        text += "   " + std::to_string(network.signal_percent) + "%";
        label->set_text(text);
        label->set_xalign(0.0f);

        row->set_child(*label);
        row->set_has_frame(false);
        if (network.in_use) row->add_css_class("active-workspace");

        const std::string ssid = network.ssid;
        row->signal_clicked().connect([this, ssid] {
            popover_.popdown();
            // A saved network joins from here. A new secured one cannot -- it needs
            // a password, and this panel deliberately has nowhere to type one and
            // nowhere to keep it -- so the system's own dialog is opened instead.
            if (!connect_wifi(ssid) && !config_.network_command.empty()) {
                spawn_detached(split_words(config_.network_command));
            }
            poll();
        });

        wifi_box_.append(*row);
        wifi_rows_.push_back(std::move(row));
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

    selection_readable_ = can_read_selection();

    poll();
    // Every tick forks a helper to ask what has focus, so the interval is a running
    // cost rather than a one-off. Half a second is imperceptible on a label that
    // says which window you are pointing at, and costs half of what 250ms did.
    Glib::signal_timeout().connect(
        [this] {
            poll();
            return true;
        },
        500);
}

void LlmContextButton::poll() {
    const auto active = focused_window_title();
    if (!active) return;

    const std::string name = *active;
    // Skip our own windows, or the context would describe the panel itself.
    if (name.find("Auspex") != std::string::npos ||
        name.find("MAGI") != std::string::npos) {
        return;
    }

    // Guarded rather than called unconditionally. This runs on a timer, and where
    // no selection helper is installed the call is a fork and an exec that can only
    // ever fail -- several times a second, for as long as the shell is up.
    const std::string selected =
        selection_readable_ ? selected_text().value_or(std::string{}) : std::string{};

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

    // Opts this window into the translucency rules in theme.cpp. Only the panels
    // carry it -- the settings, chat and launcher windows keep the opaque
    // background, since text over a photograph is hard to read.
    add_css_class("auspex-panel");

    const auto monitor = primary_monitor();
    last_monitor_ = monitor ? monitor->bounds : Rect{0, 0, 1920, 1080};
    last_layout_  = compute_panel_layout(last_monitor_, 1, config_.panel_height, position_);

    set_default_size(last_layout_.bounds.width, last_layout_.bounds.height);

    box_.set_margin_start(2);
    box_.set_margin_end(2);
    set_child(box_);

    // Right-click anywhere on the panel opens the menu. Attached to the window
    // rather than to box_ so it fires on empty panel space too -- the gap between
    // the clock and the edge is where people actually right-click.
    //
    // Button 3 explicitly: the default GestureClick button is 1, and left-click has
    // to keep reaching the buttons underneath.
    menu_.set_parent(*this);
    menu_.set_child(menu_box_);
    menu_.set_has_arrow(false);
    menu_box_.set_margin(6);

    auto right_click = Gtk::GestureClick::create();
    right_click->set_button(GDK_BUTTON_SECONDARY);
    right_click->signal_pressed().connect(
        [this](int /*n_press*/, double x, double y) { show_panel_menu(x, y); });
    add_controller(right_click);

    if (position_ == PanelPosition::Top) build_top();
    else build_bottom();

    // After the row exists, so it catches every button however it was added.
    drop_focus(box_);

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
    launcher_.set_tooltip_text("Find and start an application");
    launcher_.signal_clicked().connect([this] { show_launcher(); });
    box_.append(launcher_);

    // The pinned row sits next to the launcher, which is where xfce4-panel puts it
    // and therefore where the hand already goes.
    pinned_ = std::make_unique<PinnedLaunchers>(config_);
    box_.append(*pinned_);

    // Only when there is a crew to report on. On a machine without ollamadev this
    // is not a feature that is switched off, it is one that does not exist.
    if (crew_available()) {
        crew_ = std::make_unique<CrewButton>();
        crew_->set_board_handler([this] { show_crew(); });
        box_.append(*crew_);
    }

    workspaces_ = std::make_unique<WorkspaceSwitcher>(config_.workspace_count);
    box_.append(*workspaces_);

    // No zoom buttons on the top bar, and no "Grid" button either.
    //
    // Both were controls that promised more than X11 can deliver. Zooming in scales
    // the layout but cannot magnify a window's CONTENTS -- only Auspex's own
    // compositor will do that -- so the buttons grew windows until applications
    // refused to be squeezed any further and hung over onto the next monitor. The
    // canvas is navigated by panning, which works exactly as it says, and reset to
    // its grid by one button. Both now live together on the bottom bar.
    //
    // Zoom itself is unchanged and still reachable by ctrl+scroll and by voice; it
    // simply no longer has a button implying it is the primary way around.

    windows_ = std::make_unique<WindowList>();
    box_.append(*windows_);

    sysmon_ = std::make_unique<SystemMonitorWidget>();
    box_.append(*sysmon_);

    // No network button up here. It moved to the bottom bar next to the volume and
    // the tray, where the rest of the status controls now live -- and one button in
    // one place is a row of pixels back for the window list.

    notifications_ = std::make_unique<NotificationButton>();
    box_.append(*notifications_);

    clock_ = std::make_unique<Clock>(config_);
    clock_->set_open_handler([this] { show_calendar(); });
    box_.append(*clock_);
}

void Panel::build_bottom() {
    button_box_.set_halign(Gtk::Align::CENTER);
    button_box_.set_hexpand(true);

    // A maximised/full window leaves no empty desktop to drag. These stay on the
    // bottom overlay, so the infinite canvas is always navigable regardless of how
    // completely application windows cover the viewport.
    pan_left_.set_tooltip_text("Pan canvas left");
    pan_up_.set_tooltip_text("Pan canvas up");
    pan_down_.set_tooltip_text("Pan canvas down");
    pan_right_.set_tooltip_text("Pan canvas right");
    pan_left_.signal_clicked().connect([this] { if (on_pan_) on_pan_(-1, 0); });
    pan_up_.signal_clicked().connect([this] { if (on_pan_) on_pan_(0, -1); });
    pan_down_.signal_clicked().connect([this] { if (on_pan_) on_pan_(0, 1); });
    pan_right_.signal_clicked().connect([this] { if (on_pan_) on_pan_(1, 0); });
    // "1:1" sits in the MIDDLE of the arrows, not beside them. The arrows take you
    // away from where you started in four directions; this is the one that brings
    // you back and tidies up, so it belongs at their centre where a home key would
    // be -- reachable without reading, from any of the four.
    zoom_reset_.set_tooltip_text("Back to the grid at life size");
    zoom_reset_.signal_clicked().connect([this] { if (on_zoom_) on_zoom_(0.0); });

    // The mode switch sits with the canvas controls, because it decides what they
    // do: in grid mode the layout is the desktop's, in canvas mode it is yours.
    grid_mode_.set_active(config_.grid_mode);
    grid_mode_icon_.set_from_icon_name("view-grid-symbolic");
    grid_mode_label_.set_text(config_.grid_mode ? "Grid" : "Canvas");
    grid_mode_box_.append(grid_mode_icon_);
    grid_mode_box_.append(grid_mode_label_);
    grid_mode_.set_child(grid_mode_box_);
    grid_mode_.set_tooltip_text(
        "Grid arranges your windows to fill the screen; canvas leaves them where "
        "you drag them and lets you pan around");
    grid_mode_.signal_toggled().connect([this] {
        const bool grid = grid_mode_.get_active();
        grid_mode_label_.set_text(grid ? "Grid" : "Canvas");
        grid_mode_icon_.set_from_icon_name(grid ? "view-grid-symbolic"
                                                : "view-paged-symbolic");
        if (on_mode_) on_mode_(grid);
    });
    pan_box_.append(grid_mode_);

    pan_box_.append(pan_left_);
    pan_box_.append(pan_up_);
    pan_box_.append(zoom_reset_);
    pan_box_.append(pan_down_);
    pan_box_.append(pan_right_);

    button_box_.append(pan_box_);

    // The tray lives on the bottom bar, and it is told so: XApp hands the panel's
    // edge to the application so it knows which way to open its menu, and an icon
    // on the bottom bar that claims to be on the top gets a menu opening downwards
    // off the screen.
    tray_ = std::make_unique<SystemTray>(std::max(16, config_.panel_height - 8),
                                         /*panel_at_top=*/false);
    button_box_.append(*tray_);

    // Sound sits next to the tray because that is where people look for it, even
    // though it is not and cannot be a tray icon.
    volume_ = std::make_unique<VolumeButton>();
    button_box_.append(*volume_);

    network_button_ = std::make_unique<NetworkButton>(config_);
    button_box_.append(*network_button_);

    // Upstream launched src/settings.py here. That window is not ported yet, so
    // this opens whichever settings app this desktop provides.
    // Our own settings window, replacing settings.py; the detected system settings
    // manager is no longer used as a stand-in.
    settings_icon_.set_from_icon_name("preferences-system-symbolic");
    settings_.set_child(settings_icon_);
    settings_.set_tooltip_text("Auspex settings");
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
    speak_label_.set_text("Speak");
    speak_box_.append(speak_icon_);
    speak_box_.append(speak_label_);
    speak_.set_child(speak_box_);
    speak_.set_tooltip_text("Read the highlighted text aloud");
    speak_.signal_clicked().connect(
        [this] { voice_.submit(VoiceController::Action::SpeakSelection); });
    button_box_.append(speak_);

    // Voice to text. Click to start, click again to stop, then whisper transcribes
    // what was said and it is typed into whatever window has focus.
    //
    // Deliberately NOT routed through the command parser. Dictation is the one voice
    // path where the words are the point -- "open my downloads" typed into a document
    // must land as those words and not open anything.
    // A microphone, not insert-text-symbolic. This button dictates: the sound goes
    // in and the words come out, and the icon should say the first half. The text
    // glyph rendered as a small "a" in a box, which reads as a font setting.
    to_text_icon_.set_from_icon_name("audio-input-microphone-symbolic");
    to_text_label_.set_text("Text");
    to_text_box_.append(to_text_icon_);
    to_text_box_.append(to_text_label_);
    to_text_.set_child(to_text_box_);
    to_text_.set_tooltip_text("Click to dictate into the focused window, click again to stop");
    to_text_.signal_toggled().connect([this] {
        if (to_text_.get_active()) {
            to_text_label_.set_text("Listening");
            to_text_.add_css_class("recording");
            voice_.press_hold(VoiceController::Action::Dictate);
        } else {
            to_text_label_.set_text("Text");
            to_text_.remove_css_class("recording");
            voice_.release_hold();
        }
    });
    button_box_.append(to_text_);

    // Ask aloud: speak a question, hear the answer. Separate from the command button
    // because they want opposite things from the same sentence -- one is trying to
    // find a verb in it, this one is not trying to interpret it at all.
    ask_icon_.set_from_icon_name("dialog-question-symbolic");
    ask_label_.set_text("Ask");
    ask_box_.append(ask_icon_);
    ask_box_.append(ask_label_);
    ask_.set_child(ask_box_);
    ask_.set_tooltip_text("Ask a question aloud and hear the answer");
    ask_.signal_clicked().connect(
        [this] { voice_.submit(VoiceController::Action::AskAloud); });
    button_box_.append(ask_);

    // No press-and-hold button. It dictated into the focused window, which is
    // exactly what the toggle beside it does -- two controls, one behaviour, and
    // the only difference was whether you held the mouse down for the length of a
    // sentence. The toggle is the one that survives; holding a button steady while
    // composing is not something to ask of anyone.
    //
    // press_hold/release_hold are untouched: the chat window's Talk button still
    // uses them, where holding IS the natural gesture because you are already
    // there and the recording is a moment long.

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

    // The terminal sits last, after the voice controls. It is the one button here
    // that is not part of talking to the machine, so it does not belong in the
    // middle of the ones that are.
    if (!config_.terminal.empty()) {
        terminal_ = std::make_unique<TerminalButton>(config_);
        terminal_->set_crew_handler(
            [this](std::filesystem::path project) { show_crew_in(project); });
        terminal_->set_team_handler(
            [this](std::filesystem::path project) { show_team(project); });
        terminal_->set_brain_handler([this] { show_brain(); });
        button_box_.append(*terminal_);
    }

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

void Panel::show_panel_menu(double x, double y) {
    // Rebuilt each time so the menu never holds stale buttons, and because it is
    // three widgets -- cheaper than reasoning about when to refresh it.
    while (Gtk::Widget* child = menu_box_.get_first_child()) menu_box_.remove(*child);

    const auto add = [this](const std::string& label, sigc::slot<void()> action) {
        auto* button = Gtk::make_managed<Gtk::Button>(label);
        button->set_has_frame(false);
        button->set_halign(Gtk::Align::FILL);
        // The child label, not the button, carries the alignment: a Button's own
        // halign positions the button in its parent, not its text inside itself.
        if (auto* text = dynamic_cast<Gtk::Label*>(button->get_child())) {
            text->set_xalign(0.0f);
        }
        button->signal_clicked().connect([this, action] {
            menu_.popdown();
            action();
        });
        menu_box_.append(*button);
    };

    add("Settings", [this] { show_settings(); });
    add("Calendar", [this] { show_calendar(); });
    // Above Crew, because it answers the question Crew asks first: which folder.
    // Offered whether or not an agent is installed -- it opens terminals and file
    // managers too, and it is where you find out which agents were found.
    add("Projects", [this] { show_projects(); });
    // Only when ollamadev is installed: a menu entry that always explains it
    // cannot work is worse than no entry.
    // Crew needs no ollamadev now -- Auspex has its own engine. Team and Brain
    // still drive the CLI, so they stay gated on it being installed.
    add("Crew", [this] { show_crew(); });
    if (crew_available()) {
        add("Team", [this] { show_team({}); });
        add("Brain", [this] { show_brain(); });
    }
    add("Open a terminal", [this] {
        if (!config_.terminal.empty()) launch(config_.terminal);
    });

    auto* separator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    menu_box_.append(*separator);

    add("Quit Auspex", [this] { confirm_quit(); });

    menu_.set_pointing_to(Gdk::Rectangle(static_cast<int>(x), static_cast<int>(y), 1, 1));
    menu_.popup();
}

void Panel::confirm_quit() {
    // A second, deliberate step. Quitting takes down both panels, the desktop and
    // the canvas at once, and there is no undo -- so it is worth one more click,
    // and worth saying plainly what goes away.
    auto dialog = Gtk::AlertDialog::create();
    dialog->set_message("Quit Auspex?");
    dialog->set_detail(
        "Both panels, the desktop canvas and voice control will close. Your open "
        "windows stay where they are.");
    dialog->set_buttons({"Cancel", "Quit"});
    dialog->set_cancel_button(0);
    dialog->set_default_button(0);

    dialog->choose(*this, [this, dialog](const Glib::RefPtr<Gio::AsyncResult>& result) {
        int choice = 0;
        try {
            choice = dialog->choose_finish(result);
        } catch (const Glib::Error&) {
            return;   // dismissed with Escape or the window manager
        }
        if (choice != 1) return;

        // quit() rather than close(): closing one panel would leave the other,
        // the desktop window and the voice worker running.
        if (auto app = get_application()) app->quit();
    });
}

void Panel::drop_focus(Gtk::Widget& root) {
    for (Gtk::Widget* child = root.get_first_child(); child;
         child = child->get_next_sibling()) {
        // A popover is a menu in its own window. Recursing into one would make its
        // rows unreachable by keyboard, which is a real loss -- unlike a panel
        // button, which nobody has ever tabbed to on purpose.
        if (dynamic_cast<Gtk::Popover*>(child)) continue;

        if (auto* button = dynamic_cast<Gtk::Button*>(child)) {
            button->set_can_focus(false);
            // Both are needed. can_focus stops it being reachable at all;
            // focus_on_click stops the click itself moving focus off whatever the
            // person was typing into.
            button->set_focus_on_click(false);
        } else if (auto* toggle = dynamic_cast<Gtk::ToggleButton*>(child)) {
            toggle->set_can_focus(false);
            toggle->set_focus_on_click(false);
        }
        drop_focus(*child);
    }
}

void Panel::show_crew() { show_crew_in({}); }

void Panel::show_crew_in(const std::filesystem::path& project) {
    if (!crew_window_) crew_window_ = std::make_unique<CrewWindow>();
    // Set BEFORE presenting, so the window is never briefly showing the previous
    // folder next to a task box you are about to type into.
    if (!project.empty()) crew_window_->set_project(project);
    crew_window_->present();
}

void Panel::show_team(const std::filesystem::path& project) {
    if (!team_window_) team_window_ = std::make_unique<TeamWindow>(config_);
    if (!project.empty()) team_window_->set_project(project);
    team_window_->present();
}

void Panel::show_brain() {
    if (!brain_window_) brain_window_ = std::make_unique<BrainWindow>();
    brain_window_->present();
}

void Panel::show_projects() {
    if (!projects_window_) {
        projects_window_ = std::make_unique<ProjectsWindow>(config_);
        projects_window_->set_crew_handler(
            [this](std::filesystem::path project) { show_crew_in(project); });
    }
    projects_window_->present();
}

void Panel::show_calendar() {
    if (calendar_window_) {
        calendar_window_->present();
        return;
    }
    calendar_window_ = std::make_unique<CalendarWindow>();
    calendar_window_->signal_close_request().connect(
        [this] {
            Glib::signal_idle().connect_once([this] { calendar_window_.reset(); });
            return false;
        },
        false);
    calendar_window_->present();
}

void Panel::show_launcher() {
    if (launcher_window_) {
        launcher_window_->present();
        return;
    }
    // No close handler, deliberately. LauncherWindow sets hide_on_close, so closing
    // it hides it and present() brings the same window back. The previous version
    // destroyed the window from an idle callback on every close request -- which
    // was fine when only the user closed it, and fatal once anything else sent a
    // close request, because the object went away underneath this pointer.
    launcher_window_ = std::make_unique<LauncherWindow>(config_);
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
        window_id_ = display().find_own_window(title_);
        if (!window_id_) return;
    }

    // Reserve exactly what is drawn. GTK's minimum height comes from the button
    // padding and min-height in the stylesheet, so the mapped window is taller
    // than config's panel_height; a strut computed from config alone would leave
    // other windows overlapping the panel by the difference.
    if (const int actual = get_height(); actual > 0) {
        last_layout_ = layout_for_height(last_monitor_, actual, position_);
    }

    display().dock_panel(*window_id_, overlay_panel_layout(last_layout_));
    if (on_geometry_) {
        // GTK reports the content allocation here (39px on the current theme),
        // while Xfwm's actual undecorated panel window is 42px. Full-window layout
        // needs the outer X11 edge or it starts three pixels under the top bar and
        // stops three pixels short of the bottom one.
        const auto outer = display().window_geometry(*window_id_);
        on_geometry_(position_, outer ? outer->height : last_layout_.bounds.height);
    }

    // Re-check the monitor periodically. Rect has value equality, so this only acts
    // on real changes -- panel.py compared Gdk.Rectangle objects, which never
    // compared equal, and so re-ran three subprocesses every second forever.
    // Per-instance, not static: a static flag here would let only the first of the
    // two panels ever install a watcher.
    //
    // Five seconds, not one. Measured on a 4-output NVIDIA box, the underlying
    // `xrandr --listmonitors` costs 333ms -- so two panels asking once a second
    // each spent two thirds of a core establishing that the monitors had not
    // changed. list_monitors() also caches for 5s now, so the two panels share one
    // call rather than making two. Between them that is ~666ms/s of work reduced to
    // ~66ms/10s. The cost is that hot-plugging a monitor takes up to five seconds
    // to be reflected, which nobody will notice.
    if (!watching_geometry_) {
        watching_geometry_ = true;
        Glib::signal_timeout().connect(
            [this] {
                refresh_geometry();
                return true;
            },
            5000);
    }
}

void Panel::refresh_geometry() {
    const auto monitor = primary_monitor();
    if (!monitor || monitor->bounds == last_monitor_) return;

    last_monitor_ = monitor->bounds;
    const int height = get_height() > 0 ? get_height() : config_.panel_height;
    last_layout_     = layout_for_height(last_monitor_, height, position_);
    set_default_size(last_layout_.bounds.width, last_layout_.bounds.height);

    if (window_id_) {
        display().dock_panel(*window_id_, overlay_panel_layout(last_layout_));
    }
    if (on_geometry_) {
        const auto outer = window_id_ ? display().window_geometry(*window_id_)
                                      : std::optional<Rect>{};
        on_geometry_(position_, outer ? outer->height : last_layout_.bounds.height);
    }
}

void Panel::launch(const std::string& command) {
    const auto words = split_words(command);
    if (words.empty()) return;
    // Detached: run() would block the GTK thread until the app exits.
    spawn_detached(words);
}

}  // namespace auspex::gtk
