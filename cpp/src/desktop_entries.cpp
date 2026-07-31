#include "auspex/desktop_entries.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

namespace auspex {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

}  // namespace

std::vector<fs::path> application_dirs() {
    std::vector<fs::path> dirs;

    if (const char* home = std::getenv("XDG_DATA_HOME"); home && *home) {
        dirs.emplace_back(fs::path(home) / "applications");
    } else if (const char* h = std::getenv("HOME"); h && *h) {
        dirs.emplace_back(fs::path(h) / ".local" / "share" / "applications");
    }

    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    const std::string list = (data_dirs && *data_dirs)
                                 ? data_dirs
                                 : "/usr/local/share:/usr/share";   // XDG default

    std::istringstream parts(list);
    std::string dir;
    while (std::getline(parts, dir, ':')) {
        if (!dir.empty()) dirs.emplace_back(fs::path(dir) / "applications");
    }
    return dirs;
}

std::string strip_field_codes(std::string_view exec) {
    std::string out;
    out.reserve(exec.size());

    for (std::size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] != '%') {
            out.push_back(exec[i]);
            continue;
        }
        if (i + 1 >= exec.size()) break;

        const char code = exec[++i];
        if (code == '%') {
            out.push_back('%');   // %% is a literal percent
            continue;
        }
        // Every other field code expands to files/urls/icons we are not supplying,
        // so it is dropped entirely.
    }

    // Field code removal leaves double spaces behind; collapse them so the argv
    // split does not produce empty elements.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    for (const char c : out) {
        const bool space = (c == ' ' || c == '\t');
        if (space && prev_space) continue;
        collapsed.push_back(space ? ' ' : c);
        prev_space = space;
    }
    return trim(std::move(collapsed));
}

std::optional<DesktopEntry> parse_desktop_entry(const std::string& contents,
                                                std::string id) {
    std::istringstream in(contents);
    std::string line;
    bool in_desktop_entry = false;

    DesktopEntry entry;
    entry.id = std::move(id);
    bool type_is_application = true;   // absent Type is tolerated in practice

    while (std::getline(in, line)) {
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') continue;

        if (line.front() == '[') {
            // Only the [Desktop Entry] group matters; action groups define
            // alternative Exec lines we must not pick up.
            in_desktop_entry = (line == "[Desktop Entry]");
            continue;
        }
        if (!in_desktop_entry) continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key   = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        // Localised keys look like Name[de]; the unlocalised one is what we want.
        if (key == "Name")            entry.name = value;
        else if (key == "Comment")    entry.comment = value;
        else if (key == "Icon")       entry.icon = value;
        else if (key == "Exec")       entry.exec = strip_field_codes(value);
        else if (key == "Terminal")   entry.terminal = (lower(value) == "true");
        else if (key == "Type")       type_is_application = (value == "Application");
        else if (key == "NoDisplay" && lower(value) == "true") return std::nullopt;
        else if (key == "Hidden"    && lower(value) == "true") return std::nullopt;
    }

    if (!type_is_application) return std::nullopt;
    if (entry.name.empty() || entry.exec.empty()) return std::nullopt;
    return entry;
}

std::vector<DesktopEntry> load_desktop_entries() {
    // Keyed by id so an earlier directory shadows a later one, per XDG precedence.
    std::map<std::string, DesktopEntry> by_id;

    std::error_code ec;
    for (const auto& dir : application_dirs()) {
        if (!fs::is_directory(dir, ec)) continue;

        for (const auto& item : fs::directory_iterator(dir, ec)) {
            if (!item.is_regular_file(ec)) continue;
            if (item.path().extension() != ".desktop") continue;

            const std::string id = item.path().filename().string();
            if (by_id.count(id)) continue;   // already provided at higher precedence

            std::ifstream file(item.path());
            if (!file) continue;
            std::ostringstream buffer;
            buffer << file.rdbuf();

            if (auto entry = parse_desktop_entry(buffer.str(), id)) {
                by_id.emplace(id, std::move(*entry));
            }
        }
    }

    std::vector<DesktopEntry> entries;
    entries.reserve(by_id.size());
    for (auto& [id, entry] : by_id) entries.push_back(std::move(entry));

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return lower(a.name) < lower(b.name);
    });
    return entries;
}

bool entry_matches(const DesktopEntry& entry, std::string_view query) {
    if (query.empty()) return true;
    const std::string needle = lower(query);
    return lower(entry.name).find(needle) != std::string::npos ||
           lower(entry.comment).find(needle) != std::string::npos;
}


std::filesystem::path xfce_panel_launcher_dir() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "xfce4" / "panel";
    }
    return {};
}

std::vector<DesktopEntry> import_xfce_launchers(const std::filesystem::path& dir) {
    std::vector<DesktopEntry> pinned;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return pinned;

    // Sorted, because the directory order is arbitrary and the panel's order is
    // launcher-1, launcher-2 ... Sorting by name reproduces it closely enough to be
    // recognisable, which is the whole point of importing.
    std::vector<std::filesystem::path> launchers;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (!item.is_directory()) continue;
        const std::string name = item.path().filename().string();
        if (name.rfind("launcher-", 0) != 0) continue;
        launchers.push_back(item.path());
    }
    std::sort(launchers.begin(), launchers.end());

    for (const auto& launcher : launchers) {
        std::vector<std::filesystem::path> files;
        for (const auto& item : std::filesystem::directory_iterator(launcher, ec)) {
            if (item.path().extension() == ".desktop") files.push_back(item.path());
        }
        if (files.empty()) continue;
        std::sort(files.begin(), files.end());

        // The first file only -- see the header. The rest are right-click actions.
        std::ifstream in(files.front());
        if (!in) continue;
        std::stringstream buffer;
        buffer << in.rdbuf();

        if (auto entry = parse_desktop_entry(buffer.str(),
                                             files.front().filename().string())) {
            pinned.push_back(std::move(*entry));
        }
    }

    return pinned;
}

std::optional<DesktopEntry> find_desktop_entry(const std::string& id) {
    if (id.empty()) return std::nullopt;
    for (auto& entry : load_desktop_entries()) {
        if (entry.id == id) return entry;
    }
    return std::nullopt;
}

}  // namespace auspex
