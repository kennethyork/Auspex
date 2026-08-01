// The crew bench, as seen from Auspex.
//
// The crew itself lives in ollamadev (github: ollamadev-qt): a Director decomposes
// a task, coders build each piece in its own git worktree, and an Auditor reads
// every diff before it lands. Auspex does NOT reimplement any of that. It drives
// the `ollamadev` CLI and renders the result.
//
// WHY SHELL OUT RATHER THAN PORT: the engine is ~2,100 lines of Qt across
// Crew/Board/Agent, and Auspex is GTK. Porting it would mean two copies of the
// same logic, which is how they drift -- and the crew is the half where drift
// costs you a wrong diff landing in your files. One engine, two front ends.
//
// WHAT THIS FILE IS: the parsing and validation layer. Everything here is either
// pure text-in/values-out or a single argv invocation, so the decisions are
// testable without a crew ever running.
//
// SAFETY: `accept` and `discard` apply or bin real changesets. A number that came
// from a language model is never passed straight through -- it is checked against
// the board actually returned by `ollamadev board --json`, so "accept 7" when only
// #1 and #2 are pending is refused rather than doing something arbitrary.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

// One held changeset awaiting a decision.
struct BoardItem {
    int         n = 0;          // the number the user says: "accept 2"
    std::string id;
    std::string kind;
    std::string summary;        // what the coder says it did
    std::string reason;         // why the Auditor held it
    int         files = 0;

    bool operator==(const BoardItem&) const = default;
};

// Parses `ollamadev board --json`.
//
// The JSON form is used rather than the human one because the text output is laid
// out for reading -- two lines per item, the number embedded in "  #2  summary" --
// and re-deriving structure from that would break the first time the formatting
// is tightened. `--json` is a contract; the text is a presentation.
//
// Malformed input yields an empty list rather than a partial one: half a board is
// worse than none, because the numbers are what accept/discard act on.
std::vector<BoardItem> parse_board(const std::string& json_output);

// True when ollamadev is installed and can be driven at all.
bool crew_available();

// Runs `ollamadev board --json` and parses it. Empty when nothing is pending,
// ollamadev is missing, or the command failed -- all of which mean the same thing
// to a caller: there is nothing to decide right now.
//
// NO directory argument, deliberately. `--cwd` belongs to `ollamadev terminal`,
// not to `board`, and ollamadev treats an unrecognised argument as a PROMPT -- so
// a helpfully-added `-C /path` would not error, it would quietly send "/path" to a
// language model. The board is read from this process's working directory.
std::vector<BoardItem> board_items();

// Finds an item by the number a person would say. nullopt when that number is not
// on the board, which is the check that keeps a hallucinated index from applying
// somebody else's changeset.
std::optional<BoardItem> board_item(const std::vector<BoardItem>& items, int n);

// --- run state ---------------------------------------------------------------
//
// ollamadev writes what the crew is doing to ~/.ollamadev/crew/current.json while
// it runs. Auspex reads that file rather than watching for a process, because a
// crew is started in a terminal on the canvas and detached -- there is no pid to
// hold on to, and pgrep for a command line is guesswork that breaks the moment
// somebody runs two.
//
// A file the engine already maintains is also the only source that can say WHICH
// subtask is being worked on. That is the difference between a panel that says
// something is happening and one that says what.

struct CrewSubtask {
    int         n = 0;
    std::string role;    // "coder", "researcher", ...
    std::string title;
    std::string state;   // "done" and whatever else the engine uses

    bool operator==(const CrewSubtask&) const = default;
};

struct CrewRun {
    bool                     active = false;
    std::string              run_id;
    std::string              task;
    std::vector<CrewSubtask> subtasks;
    // False when the file is missing or unreadable, which is different from a run
    // that has finished: one means "no crew has ever run here", the other means
    // "the crew is idle". A panel should show nothing for the first.
    bool                     known = false;

    bool operator==(const CrewRun&) const = default;
};

// $HOME/.ollamadev/crew/current.json.
std::filesystem::path crew_state_path();

// $HOME/.ollamadev/board/current.json -- the board's own file.
//
// Watched rather than polled with `ollamadev board --json`, which is a subprocess:
// asking it every couple of seconds for the whole of a session would cost more than
// the feature is worth. The file's modification time answers "has anything landed"
// for the price of a stat, and the command is only run when the answer is yes.
std::filesystem::path board_state_path();

// Parses that file. Anything malformed yields a run that is not `known`, rather
// than a half-populated one that would report a wrong count.
CrewRun parse_crew_run(const std::string& json_text);

CrewRun current_crew_run(const std::filesystem::path& path = crew_state_path());

// How far along. `total` is the number of subtasks the Director planned; `done` is
// how many have finished. Both zero when nothing is planned yet, which is a real
// state -- the Director runs before there are any subtasks to count.
struct CrewProgress {
    int done  = 0;
    int total = 0;

    bool operator==(const CrewProgress&) const = default;
};

CrewProgress crew_progress(const CrewRun& run);

// The subtask being worked on: the first that is not done. nullopt when the plan
// is empty or everything has finished.
std::optional<CrewSubtask> crew_current_subtask(const CrewRun& run);

// A short line for a panel button: "Crew 1/3" while running, empty when idle.
std::string crew_status_label(const CrewRun& run);

// The longer form for a tooltip: what the crew was asked to do, and what it is
// doing about it right now.
std::string crew_status_detail(const CrewRun& run);

// The argv for each decision. Returned rather than run so the command can be
// asserted in a test without a crew, and so the caller decides whether it runs on
// the canvas or in the background.
//
// Empty when `n` is not positive, or (for steer) the text is empty.
std::vector<std::string> crew_accept_command(int n);
std::vector<std::string> crew_discard_command(int n);
std::vector<std::string> crew_steer_command(int n, const std::string& instruction);

// Finishes an interrupted run: every coder that already completed has its work
// landed from disk with no model calls, and the Director re-plans only what is
// left. Cheap and safe to run when nothing was interrupted -- there is simply
// nothing to resume -- which is why no confirmation is required for it.
//
// Takes no run id. `crew resume` alone picks the most recent, which is the one a
// person means; naming a specific id is a thing to do at a terminal with the list
// in front of you, not by voice.
std::vector<std::string> crew_resume_command();

}  // namespace auspex
