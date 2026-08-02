#include "auspex/projects.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

std::filesystem::path home_dir() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home);
    }
    return {};
}

std::string string_field(const json& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto& value = object[key];
    return value.is_string() ? value.get<std::string>() : std::string{};
}

// The folder's own name, for a bookmark that never got one. Normalised first, or
// "/home/me/project/" yields an empty filename.
std::string name_for(const std::filesystem::path& path) {
    const auto trimmed = normal_project_path(path);
    const std::string name = trimmed.filename().string();
    return name.empty() ? trimmed.string() : name;
}

Project project_from(const json& entry, bool bookmarked) {
    Project project;
    project.path = string_field(entry, "path");
    project.name = trim(string_field(entry, "name"));
    if (project.name.empty()) project.name = name_for(project.path);
    project.last_opened = string_field(entry, "lastOpened");
    project.bookmarked  = bookmarked;
    return project;
}

// Most-recently-opened first. An entry with no timestamp sorts last rather than
// first: "never recorded" is not "just now", and putting it at the top would push
// the folder you were actually in off the front of the list.
void sort_by_recency(std::vector<Project>& projects) {
    std::stable_sort(projects.begin(), projects.end(),
                     [](const Project& a, const Project& b) {
                         if (a.last_opened.empty() != b.last_opened.empty()) {
                             return b.last_opened.empty();
                         }
                         return a.last_opened > b.last_opened;
                     });
}

std::string read_file(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

}  // namespace

// ---------------------------------------------------------------------------
bool is_project_dir(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

std::string unsafe_project_reason(const std::filesystem::path& path) {
    if (path.empty()) return "no project is chosen";

    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return "that is not a directory";

    const auto normal = normal_project_path(std::filesystem::absolute(path, ec));

    // The filesystem root, or a top-level directory. Nothing anybody wants a crew
    // copying, and the cost of being wrong is the whole disk.
    if (!normal.has_relative_path() || normal.parent_path() == normal) {
        return "that is the filesystem root";
    }

    if (const char* home = std::getenv("HOME"); home && *home) {
        const auto home_path = normal_project_path(home);
        if (normal == home_path) {
            return "that is your home directory, not a project -- a crew copies the "
                   "tree once per coder and can write changes back into it";
        }
        // The folders that HOLD projects rather than being one. A project inside
        // any of them is fine; the folder itself is not.
        for (const char* folder : {"Documents", "Downloads", "Desktop", "Music",
                                   "Pictures", "Videos", "Public", "Templates"}) {
            if (normal == normal_project_path(home_path / folder)) {
                return std::string("that is your ") + folder +
                       " folder, not a project -- pick the project inside it";
            }
        }
    }

    return {};
}

std::filesystem::path normal_project_path(const std::filesystem::path& path) {
    if (path.empty()) return path;

    std::filesystem::path normal = path.lexically_normal();

    // lexically_normal() keeps a trailing separator, which is what makes "/tmp"
    // and "/tmp/" compare unequal. Strip it -- but never past the root, or "/"
    // would normalise to nothing and match every relative path.
    while (normal.has_relative_path() && normal.filename().empty()) {
        normal = normal.parent_path();
    }
    return normal;
}

std::filesystem::path bookmarks_path() {
    const auto home = home_dir();
    if (home.empty()) return {};
    return home / ".ollamadev" / "workspaces.json";
}

std::vector<Project> parse_bookmarks(const std::string& json_text) {
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return {};
    if (!document.contains("workspaces") || !document["workspaces"].is_array()) return {};

    std::vector<Project> projects;
    for (const auto& entry : document["workspaces"]) {
        if (!entry.is_object()) continue;
        Project project = project_from(entry, /*bookmarked=*/true);
        // A bookmark whose folder has been deleted or moved is dropped here rather
        // than shown greyed out. The list is a set of buttons; every one of them
        // should do something.
        if (!is_project_dir(project.path)) continue;
        projects.push_back(std::move(project));
    }

    sort_by_recency(projects);
    return projects;
}

std::optional<std::string> parse_active_bookmark(const std::string& json_text) {
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return std::nullopt;

    const std::string active = string_field(document, "active");
    if (active.empty()) return std::nullopt;

    // Resolved to a PATH here, not returned as the opaque id ollamadev stores.
    // An id is only meaningful inside that file, and every caller wants a folder.
    if (!document.contains("workspaces") || !document["workspaces"].is_array()) {
        return std::nullopt;
    }
    for (const auto& entry : document["workspaces"]) {
        if (!entry.is_object()) continue;
        if (string_field(entry, "id") != active) continue;
        const std::string path = string_field(entry, "path");
        if (path.empty()) return std::nullopt;
        return path;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::filesystem::path recents_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "auspex" / "projects.json";
    }
    const auto home = home_dir();
    if (home.empty()) return {};
    return home / ".config" / "auspex" / "projects.json";
}

std::vector<Project> parse_recents(const std::string& json_text) {
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_array()) return {};

    std::vector<Project> projects;
    for (const auto& entry : document) {
        // Two accepted forms: a bare path string, and an object with a path. The
        // string form is what a person editing this file by hand would write, and
        // accepting it costs three lines.
        Project project;
        if (entry.is_string()) {
            project.path = entry.get<std::string>();
            project.name = name_for(project.path);
        } else if (entry.is_object()) {
            project = project_from(entry, /*bookmarked=*/false);
        } else {
            continue;
        }
        if (project.path.empty()) continue;
        projects.push_back(std::move(project));
    }
    // NOT sorted. A recents file is already in order -- that is the whole content
    // of it -- and re-sorting by a timestamp most entries do not carry would throw
    // that away.
    return projects;
}

std::string encode_recents(const std::vector<Project>& recents) {
    json array = json::array();
    for (const auto& project : recents) {
        array.push_back(json{{"path", project.path.string()}, {"name", project.name}});
    }
    return array.dump(2);
}

std::vector<Project> promote_recent(std::vector<Project> recents,
                                    const std::filesystem::path& path,
                                    std::size_t limit) {
    if (path.empty()) return recents;

    // Compared as lexically-normal paths, so ".../Auspex" and ".../Auspex/" are one
    // entry rather than two that look identical in the list.
    const auto normal = normal_project_path(path);
    recents.erase(std::remove_if(recents.begin(), recents.end(),
                                 [&normal](const Project& p) {
                                     return normal_project_path(p.path) == normal;
                                 }),
                  recents.end());

    Project fresh;
    fresh.path = normal;
    fresh.name = name_for(normal);
    recents.insert(recents.begin(), std::move(fresh));

    if (recents.size() > limit) recents.resize(limit);
    return recents;
}

std::vector<Project> load_recents() { return parse_recents(read_file(recents_path())); }

void remember_project(const std::filesystem::path& path) {
    const auto file = recents_path();
    if (file.empty() || !is_project_dir(path)) return;

    const auto updated = promote_recent(load_recents(), path);

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return;

    // Temp file plus rename, so a panel killed mid-write leaves the old list
    // rather than half of a new one.
    const auto temp = file.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return;
        out << encode_recents(updated) << "\n";
        if (!out) return;
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) std::filesystem::remove(temp, ec);
}

// ---------------------------------------------------------------------------
std::vector<Project> all_projects() {
    std::vector<Project> projects = parse_bookmarks(read_file(bookmarks_path()));

    // Recents that are not already bookmarked, appended in their own order. A
    // folder in both lists is shown once, as the bookmark -- that is the entry with
    // the name somebody chose.
    for (auto& recent : load_recents()) {
        if (!is_project_dir(recent.path)) continue;
        const auto normal = normal_project_path(recent.path);
        const bool duplicate =
            std::any_of(projects.begin(), projects.end(), [&normal](const Project& p) {
                return normal_project_path(p.path) == normal;
            });
        if (!duplicate) projects.push_back(std::move(recent));
    }

    return projects;
}

std::optional<Project> default_project() {
    const std::string workspaces = read_file(bookmarks_path());

    // The active workspace first: it is where ollamadev would put ITSELF, so
    // starting anywhere else would mean the panel and the engine disagree about
    // what "this project" means.
    if (const auto active = parse_active_bookmark(workspaces)) {
        for (auto& project : parse_bookmarks(workspaces)) {
            if (normal_project_path(project.path) == normal_project_path(*active)) {
                return project;
            }
        }
    }

    const auto projects = all_projects();
    if (!projects.empty()) return projects.front();
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::vector<std::string> terminal_command_in(const std::string& terminal,
                                             const std::string& program,
                                             const std::filesystem::path& directory) {
    if (program.empty()) return {};
    return terminal_command_argv(terminal, {program}, directory);
}

std::vector<std::string> terminal_command_argv(const std::string& terminal,
                                               const std::vector<std::string>& command,
                                               const std::filesystem::path& directory) {
    if (command.empty() || command.front().empty()) return {};

    std::vector<std::string> argv = terminal_here(terminal, directory);
    if (argv.empty()) return {};

    const std::string name = std::filesystem::path(terminal).filename().string();

    // "And then run this", whose spelling is the part terminals disagree about.
    // Getting it wrong opens an empty terminal, which reads as the agent having
    // crashed instantly.
    //
    // gnome-terminal removed -e in 3.14 and prints a deprecation warning instead
    // of running anything; -- is the supported form. kitty, foot, alacritty and
    // wezterm take the command as trailing arguments with no separator at all
    // (alacritty does have -e, but the bare form works on every version).
    //
    // xfce4-terminal is the trap. Its -e takes a SINGLE string and word-splits it
    // itself, so a command with arguments dies on `Unknown option "..."` and opens
    // nothing at all. -x takes the rest of the line, and works equally well for a
    // bare program, so it is used for both rather than switching on the argument
    // count -- one path, one thing to be wrong about.
    if (name == "gnome-terminal" || name == "mate-terminal") {
        argv.push_back("--");
    } else if (name == "xfce4-terminal") {
        argv.push_back("-x");
    } else if (name != "kitty" && name != "foot" && name != "alacritty" &&
               name != "wezterm") {
        argv.push_back("-e");
    }

    argv.insert(argv.end(), command.begin(), command.end());
    return argv;
}

std::vector<std::string> terminal_here(const std::string& terminal,
                                       const std::filesystem::path& directory) {
    if (terminal.empty()) return {};

    // Match on the basename: `terminal` may be an absolute path from PATH lookup.
    const std::string name = std::filesystem::path(terminal).filename().string();
    const std::string dir  = directory.string();

    std::vector<std::string> argv{terminal};

    // The working-directory flag, where the terminal has one. See the header for
    // why this is worth doing even though the child is chdir'd as well.
    if (!dir.empty()) {
        if (name == "xfce4-terminal" || name == "gnome-terminal" ||
            name == "mate-terminal" || name == "lxterminal" || name == "foot") {
            argv.push_back("--working-directory=" + dir);
        } else if (name == "konsole") {
            argv.push_back("--workdir");
            argv.push_back(dir);
        } else if (name == "kitty") {
            argv.push_back("--directory");
            argv.push_back(dir);
        } else if (name == "alacritty") {
            argv.push_back("--working-directory");
            argv.push_back(dir);
        }
        // xterm, urxvt and wezterm get it from the inherited cwd only. wezterm's
        // own form is `wezterm start --cwd DIR -- prog`, which changes the shape of
        // the whole command line rather than adding a flag to it, and wezterm does
        // inherit -- so it is not worth a special case that could only break.
    }

    return argv;
}

}  // namespace auspex
