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

// Case-insensitive match of `query` against name and comment. Empty query matches
// everything. Used by the launcher's search box.
bool entry_matches(const DesktopEntry& entry, std::string_view query);

}  // namespace auspex
