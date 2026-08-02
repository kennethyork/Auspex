// The folder an agent works in.
//
// WHY THIS EXISTS AT ALL:
//
// Every coding CLI Auspex can open -- claude, codex, opencode, ollamadev -- reads
// and writes the tree it is started in. Nothing in the panel used to say which tree
// that was: spawn_detached() inherited the shell's own working directory, which is
// wherever the session manager happened to start it. On a real login that is $HOME,
// so every agent the panel opened was pointed at the home directory. For a terminal
// that is merely unhelpful; for `ollamadev crew "refactor the parser"`, which
// decomposes a task against the tree it is standing in and applies diffs back into
// it, it is the whole behaviour being wrong.
//
// There is a second, quieter failure on top of it. `ollamadev` with no subcommand
// -- the REPL, and the one-shot turn, which is what the OllamaDev button here
// launches -- calls syncCurrentProject() before doing anything: a cwd that is not
// inside a bookmarked folder makes it chdir into the last ACTIVE bookmark instead,
// announcing the move on stderr. spawn_detached() sends stderr to /dev/null, so
// that warning went nowhere. (`crew` does not do this -- it takes QDir::currentPath()
// as it finds it -- which is why the first problem was the one that mattered.)
//
// So a directory is now chosen explicitly, and every agent command carries it.
//
// WHERE THE LIST COMES FROM: ~/.ollamadev/workspaces.json, which ollamadev already
// maintains and its own desktop already follows. Reading it rather than keeping a
// second list means a folder bookmarked in either program shows up in both, and
// picking one here is the same "current project" ollamadev would have chosen for
// itself -- so the two can never disagree about where the work is happening.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct Project {
    std::string           name;      // "Auspex" -- the workspace's name, or the folder's
    std::filesystem::path path;
    // ISO-8601 as ollamadev writes it, or empty. Sorted on as text, which is what
    // that format is for; nothing here has to parse a date.
    std::string           last_opened;
    // True when it came from ollamadev's own bookmarks rather than Auspex's
    // recents, so the two can be shown apart.
    bool                  bookmarked = false;

    bool operator==(const Project&) const = default;
};

// A directory an agent can sensibly be started in: it exists, and it is a
// directory. Checked before every launch rather than trusted from the list,
// because a bookmark outlives the folder it points at.
bool is_project_dir(const std::filesystem::path& path);

// The form two paths are compared in, so "/home/me/x" and "/home/me/x/" are one
// project rather than two rows of identical-looking text.
//
// NOT just lexically_normal(): that PRESERVES a trailing directory separator by
// design ("/tmp/" normalises to "/tmp/"), so on its own it would let the same
// folder into a recents list twice -- once per way of typing it. The trailing
// separator is stripped here, except from a root path, which is nothing but one.
//
// Purely textual: no symlink resolution and no touching the disk, because this is
// also used to compare a path against one recorded by another program at another
// time, which may no longer exist.
std::filesystem::path normal_project_path(const std::filesystem::path& path);

// NAMING: ollamadev calls these "workspaces", and everything below reads the file
// it writes under that name. They are called BOOKMARKS here because "workspace"
// already means an X11 virtual desktop everywhere else in Auspex -- see
// desktop.hpp, which has its own parse_workspaces() over `wmctrl -d`. Two
// unrelated things sharing one word in one namespace is how you get a caller that
// compiles and does something else entirely.

// $HOME/.ollamadev/workspaces.json.
std::filesystem::path bookmarks_path();

// Parses that file. Entries without a usable path are dropped rather than listed
// as broken -- the list is a menu of things to launch, and an entry that cannot be
// launched is not a menu item.
//
// Ordered most-recently-opened first, which is the order you want a project list
// in: the one you were just in is the one you want again.
std::vector<Project> parse_bookmarks(const std::string& json_text);

// Which of those is marked active, as a PATH rather than the opaque id the file
// stores. This is the folder ollamadev would put itself in if it were started with
// no directory of its own, so it is the honest default for a window that is about
// to run ollamadev.
std::optional<std::string> parse_active_bookmark(const std::string& json_text);

// --- Auspex's own recents ----------------------------------------------------
//
// Kept separate from ollamadev's bookmarks and never written back into them: a
// folder opened once to look at something is not a project you have adopted, and
// silently editing another program's list from a file browser would be rude.

// $XDG_CONFIG_HOME/auspex/projects.json, else ~/.config/auspex/projects.json.
std::filesystem::path recents_path();

std::vector<Project> parse_recents(const std::string& json_text);
std::string          encode_recents(const std::vector<Project>& recents);

// Moves `path` to the front, dropping any earlier copy of it, and truncates to
// `limit`. Pure, so the ordering rule is testable without touching disk.
std::vector<Project> promote_recent(std::vector<Project> recents,
                                    const std::filesystem::path& path,
                                    std::size_t limit = 12);

std::vector<Project> load_recents();
void                 remember_project(const std::filesystem::path& path);

// Bookmarks first, then recents that are not already bookmarked, then anything
// still missing from disk removed. What a project picker should show, in one call.
std::vector<Project> all_projects();

// The folder to start on: the active workspace if there is one, else the most
// recent thing in the list, else nothing. Never the process's own cwd -- guessing
// that is exactly the behaviour this module replaces.
std::optional<Project> default_project();

// --- opening one in an agent --------------------------------------------------

// argv that opens `program` inside `terminal`, in `directory`.
//
// The directory is passed BOTH as the terminal's own flag and (by the caller,
// through spawn_detached's cwd) as the child's working directory. That is
// deliberate belt and braces: terminals that spawn their window from an existing
// server process -- gnome-terminal is the common one -- do not inherit the cwd of
// whoever asked, so for those only the flag works; terminals with no such flag,
// like xterm, only get it from the cwd. Neither mechanism covers everything, and
// together they cover what is installed on a Linux desktop.
//
// `program` is not free text. It comes from the agent table in agents.hpp or from
// Config::terminal, never from a model and never from something typed here.
//
// Empty if the terminal or the program is empty.
std::vector<std::string> terminal_command_in(const std::string& terminal,
                                             const std::string& program,
                                             const std::filesystem::path& directory);

// The same for a command that has ARGUMENTS, not just a name.
//
// A separate entry point because the flag differs, and only on the terminal most
// likely to be installed: xfce4-terminal's `-e` takes exactly ONE argument, which
// it word-splits itself, so `-e ollamadev index build` fails with `Unknown option
// "index"` and opens nothing. `-x` takes the rest of the line as argv, which is
// what every other terminal's `-e` (or `--`) already does.
//
// `terminal_command_in` is implemented in terms of this, so there is one copy of
// the per-terminal table rather than two that drift.
//
// `argv` is never free text -- it comes from a fixed table (agents.hpp,
// engine_actions() in crew.hpp) -- and there is no shell anywhere in the path, so
// its elements are data however they are spelled.
std::vector<std::string> terminal_command_argv(const std::string& terminal,
                                               const std::vector<std::string>& argv,
                                               const std::filesystem::path& directory);

// The same, with no program: just the terminal, standing in `directory`.
//
// A separate function rather than an empty `program`, because "run nothing" and
// "you forgot to say what to run" are different requests and only one of them
// should open a window.
std::vector<std::string> terminal_here(const std::string& terminal,
                                       const std::filesystem::path& directory);

}  // namespace auspex
