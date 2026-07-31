#include "auspex/autostart.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "auspex/process.hpp"

namespace auspex {

namespace {

std::filesystem::path config_home() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config";
    }
    return std::filesystem::path(".config");
}

// Lowercased, with all whitespace removed, so "Hidden = TRUE" and "Hidden=true"
// are the same line. Desktop files are written by hand and by half a dozen tools.
std::string canonical(const std::string& line) {
    std::string out;
    for (const unsigned char c : line) {
        if (std::isspace(c)) continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

}  // namespace

std::filesystem::path autostart_path() {
    return config_home() / "autostart" / "auspex.desktop";
}

std::filesystem::path own_executable() {
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return self;
}

std::string autostart_entry(const std::filesystem::path& executable) {
    std::ostringstream entry;
    entry << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=Auspex\n"
          << "Comment=Local AI desktop shell\n"
          << "Exec=" << executable.string() << "\n"
          << "Icon=preferences-desktop\n"
          << "Terminal=false\n"
          // Written explicitly rather than left out. Absent means enabled, but a
          // session editor that turns Auspex off flips these rather than deleting
          // the file, and having the keys already present is what makes its edit
          // land somewhere we look for it.
          << "Hidden=false\n"
          << "X-GNOME-Autostart-enabled=true\n"
          // The panel docks with _NET_WM_STRUT_PARTIAL and needs a window manager
          // already running to honour it. Autostart entries fire early, and on a
          // cold login Auspex can otherwise dock against a screen no one is
          // managing yet, landing at the wrong size.
          << "X-GNOME-Autostart-Delay=3\n";
    return entry.str();
}

bool autostart_entry_is_hidden(const std::string& contents) {
    std::istringstream stream(contents);
    std::string        line;
    while (std::getline(stream, line)) {
        const std::string flat = canonical(line);
        if (flat == "hidden=true") return true;
        // GNOME's own key, honoured by several other sessions too.
        if (flat == "x-gnome-autostart-enabled=false") return true;
    }
    return false;
}

bool autostart_enabled(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::ostringstream contents;
    contents << in.rdbuf();
    return !autostart_entry_is_hidden(contents.str());
}

bool autostart_enabled() { return autostart_enabled(autostart_path()); }

bool set_autostart(bool enabled, const std::filesystem::path& path,
                   const std::filesystem::path& executable) {
    std::error_code ec;

    if (!enabled) {
        std::filesystem::remove(path, ec);
        // Not an error: the box being unticked and the file never having existed
        // are the same end state.
        return !ec;
    }

    // Refuse rather than write an entry that cannot launch anything. An Exec= line
    // pointing at nothing produces a login that silently lacks a panel, which is
    // far worse to diagnose than a checkbox that declines to tick.
    if (executable.empty()) return false;

    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << autostart_entry(executable);
    return out.good();
}

bool set_autostart(bool enabled) {
    return set_autostart(enabled, autostart_path(), own_executable());
}

}  // namespace auspex
