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

std::filesystem::path stable_executable_path() {
    if (const char* xdg = std::getenv("XDG_BIN_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "auspex-shell";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "bin" / "auspex-shell";
    }
    return {};
}

std::filesystem::path install_stable_executable(const std::filesystem::path& link,
                                                const std::filesystem::path& target) {
    if (link.empty() || target.empty()) return {};

    std::error_code ec;
    if (!std::filesystem::exists(target, ec)) return {};

    std::filesystem::create_directories(link.parent_path(), ec);
    if (ec) return {};

    // Already pointing where it should: leave it alone rather than churning the
    // link every time the setting is saved.
    if (std::filesystem::is_symlink(link, ec)) {
        const auto current = std::filesystem::read_symlink(link, ec);
        if (!ec && current == target) return link;
    }

    // Removed first: create_symlink will not overwrite, and a stale link here is
    // exactly the thing being fixed.
    std::filesystem::remove(link, ec);
    std::filesystem::create_symlink(target, link, ec);
    if (ec) return {};
    return link;
}

std::filesystem::path own_executable() {
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return self;
}

std::filesystem::path supervisor_beside(const std::filesystem::path& shell) {
    if (shell.empty()) return {};
    std::error_code ec;
    const auto sibling = shell.parent_path() / "auspex-session";
    if (std::filesystem::exists(sibling, ec)) return sibling;
    return {};
}

std::string autostart_entry(const std::filesystem::path& executable, bool supervise) {
    std::ostringstream entry;
    entry << "[Desktop Entry]\n"
          << "Type=Application\n"
          << "Name=Auspex\n"
          << "Comment=Local AI desktop shell\n"
          << "Exec=" << executable.string() << (supervise ? " --supervise" : "")
          << "\n"
          << "Icon=preferences-desktop\n"
          << "Terminal=false\n"
          // Written explicitly rather than left out. Absent means enabled, but a
          // session editor that turns Auspex off flips these rather than deleting
          // the file, and having the keys already present is what makes its edit
          // land somewhere we look for it.
          << "Hidden=false\n"
          << "X-GNOME-Autostart-enabled=true\n"
          // Honoured by GNOME and by nothing else. Checked: xfce4-session does not
          // contain this string at all, so on an Xfce desktop it does nothing.
          //
          // It is kept because it is correct where it IS read, but it must not be
          // mistaken for the thing that stops Auspex docking before the window
          // manager is up. What actually protects against that is the docking
          // retry in Panel::dock(), which runs everywhere.
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
    // The supervisor when there is one. Its absence is not an error -- a build
    // without it still autostarts, just without anything to restart the panel.
    const bool supervise = executable.filename() == "auspex-session";
    out << autostart_entry(executable, supervise);
    return out.good();
}

bool set_autostart(bool enabled) {
    // Prefer the stable path, and create it as part of enabling: ticking the box
    // after moving the source tree is then also what repairs the link, rather than
    // leaving a login that quietly starts nothing.
    const std::filesystem::path shell = own_executable();

    // Start the SUPERVISOR at login, not the shell: a panel that dies with nothing
    // watching it is a panel you have to notice is gone and start by hand.
    std::filesystem::path target = shell;
    std::filesystem::path link   = stable_executable_path();
    if (const auto supervisor = supervisor_beside(shell); !supervisor.empty()) {
        target = supervisor;
        link   = link.parent_path() / "auspex-session";
    }

    if (enabled) {
        if (const auto stable = install_stable_executable(link, target); !stable.empty()) {
            target = stable;
        }
    }
    return set_autostart(enabled, autostart_path(), target);
}

}  // namespace auspex
