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
    std::vector<std::unique_ptr<Gtk::Label>> rows_;
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
