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

#include "auspex/audio.hpp"
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
    set_default_size(520, 560);
    set_modal(true);

    search_.set_placeholder_text("Search applications...");
    search_.signal_changed().connect([this] { refilter(); });

    // Enter launches the selected row, so typing then Enter is the whole flow.
    search_.signal_activate().connect([this] { launch_selected(); });

    list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
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
    search_.grab_focus();
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

        auto label = std::make_unique<Gtk::Label>();
        label->set_markup(markup);
        label->set_xalign(0.0f);
        label->set_margin(6);
        label->set_wrap(false);
        label->set_ellipsize(Pango::EllipsizeMode::END);

        list_.append(*label);
        visible_.push_back(&entry);
        rows_.push_back(std::move(label));

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
// ChatWindow
// ---------------------------------------------------------------------------
ChatWindow::ChatWindow(const Config& config, VoiceController& voice)
    : config_(config), voice_(voice) {
    set_title("Auspex");
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
        OllamaClient ollama(config_);
        model_names_ = ollama.list_models();
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

    // The panel watches config.json every second, so the theme and panel geometry
    // apply without a restart. Model and microphone are read when the relevant
    // subsystem next initialises.
    status_.set_text("Saved. Theme applies immediately; some options need a restart.");
}

}  // namespace auspex::gtk
