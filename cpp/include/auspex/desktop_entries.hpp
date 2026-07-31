// Application discovery from XDG .desktop files.
//
// Backs the launcher window, replacing core/launcher.py's Gio.AppInfo use. Kept
// GTK-free so the parsing and the Exec-line cleanup are unit-testable -- those are
// where the real bugs live, not in the list widget.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct DesktopEntry {
    std::string name;
    std::string comment;
    std::string icon;
    std::string exec;          // field codes already stripped
    bool        terminal = false;
    std::string id;            // basename, e.g. "firefox.desktop"

    bool operator==(const DesktopEntry&) const = default;
};

// $XDG_DATA_HOME/applications plus each $XDG_DATA_DIRS/applications, in
// precedence order. Earlier directories win on duplicate ids, which is how a user
// override in ~/.local/share/applications is supposed to shadow /usr/share.
std::vector<std::filesystem::path> application_dirs();

// All visible entries, sorted by name, duplicates resolved by id.
// Skips NoDisplay=true and Hidden=true, and entries whose Exec is missing.
std::vector<DesktopEntry> load_desktop_entries();

// Parses one .desktop file. nullopt if it should not be shown.
std::optional<DesktopEntry> parse_desktop_entry(const std::string& contents,
                                                std::string id = {});

// Removes the Exec field codes (%f %F %u %U %i %c %k and friends). Leaving them in
// means passing a literal "%U" as an argv element, which some applications treat as
// a filename and then fail to open.
std::string strip_field_codes(std::string_view exec);

// --- pinned launchers --------------------------------------------------------

// Where xfce4-panel keeps the .desktop files for its launcher plugins: one
// directory per launcher, each holding one file per item.
//
// Read so that replacing that panel does not mean losing the row of applications
// someone has had one click away for years. Nothing is written back and nothing is
// changed; xfce4-panel is left exactly as it was.
std::filesystem::path xfce_panel_launcher_dir();

// The pinned applications xfce4-panel is configured with, in directory order.
//
// Only the FIRST .desktop file in each launcher directory is taken. The others are
// that launcher's right-click actions -- "Open a New Private Window" and the like --
// which are not separate buttons on the panel and must not become separate buttons
// here.
std::vector<DesktopEntry> import_xfce_launchers(
    const std::filesystem::path& dir = xfce_panel_launcher_dir());

// Resolves a saved pin back to something launchable.
//
// A pin is stored as a desktop entry id ("firefox.desktop") rather than a command,
// so the icon and name follow the application when it is updated, and so nothing in
// the config file is a command line waiting to be run.
std::optional<DesktopEntry> find_desktop_entry(const std::string& id);

// Case-insensitive match of `query` against name and comment. Empty query matches
// everything. Used by the launcher's search box.
bool entry_matches(const DesktopEntry& entry, std::string_view query);

}  // namespace auspex
