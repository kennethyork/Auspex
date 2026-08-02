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
    std::vector<std::string> file_names;

    // The project this changeset would land in, from the board's own record.
    //
    // Shown because the board is GLOBAL -- ~/.ollamadev/board, not the project's --
    // so a run in one folder and a run in another appear in the same numbered list.
    // "accept 2" applies to the repo the engine recorded at the time, which may not
    // be the one you are looking at, and a bare summary gives you no way to notice.
    std::string repo_root;

    // The directory the held changeset was saved into, so a decision made later --
    // after the run has finished and its process has gone -- can still apply it.
    // Empty for a board written by ollamadev, which stores its work elsewhere.
    std::string store;

    // The unified diff, straight from the board's "detail" field.
    //
    // Carried here rather than fetched on demand because there is nothing to fetch
    // it WITH: there is no `crew diff` verb, and asking for one starts a run with
    // "diff" as the prompt. The board already holds the whole patch.
    std::string diff;

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
// `project` is where the command RUNS, not an argument to it. There is no board
// flag for a directory -- ollamadev treats an unrecognised argument as a PROMPT,
// so a helpfully-added `-C /path` would not error, it would quietly send "/path"
// to a language model. Empty runs it in this process's working directory.
//
// The board itself is global (~/.ollamadev/board), so the directory does not
// filter what comes back; it decides which project's ./.ollamadev.json is in
// force while the command runs. See BoardItem::repo_root for how the entries are
// told apart.
std::vector<BoardItem> board_items(const std::filesystem::path& project = {});

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

    // The engine's own vocabulary, taken from ollamadev-qt's CoderPane rather than
    // guessed at: "todo", "doing", "done", "held", "flagged". Anything unrecognised
    // is treated as not-yet-started, which is the reading that overstates least.
    std::string state;

    // Which model is doing it, and why that one.
    //
    // Worth carrying because --route exists: its whole purpose is to give each
    // subtask a model chosen for its difficulty, and without this the switch is
    // one you turn on and cannot observe. `route` is the engine's own note about
    // the choice, empty when routing was off.
    std::string backend;
    std::string model;
    std::string route;

    bool operator==(const CrewSubtask&) const = default;
};

// Where a subtask belongs on the board.
//
// The mapping is ollamadev-qt's BoardPane, copied rather than reasoned out:
// held gets its OWN column, done is Done, todo is To do, and everything else --
// including `flagged` -- is Doing. Two front ends onto one engine should not
// disagree about what a state means, and the Qt one is the older reading.
//
// Held earns a column rather than a marker because it is the only state waiting on
// a PERSON. Folding it into Done, which is what Auspex did first, makes a run that
// needs a decision look like a run that is finished.
enum class CrewLane { Todo, Doing, Done, Held };

CrewLane crew_lane_of(const CrewSubtask& subtask);

// True when the Auditor is holding this one for a decision.
//
// `flagged` is deliberately NOT held: the Qt board treats it as still in flight.
bool crew_subtask_held(const CrewSubtask& subtask);

// "gpt-oss:20b-cloud · hard" -- the model and the routing note, for one line under
// a subtask's title. Empty when nothing is known about either.
std::string crew_subtask_model_line(const CrewSubtask& subtask);

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

// --- starting a run ----------------------------------------------------------
//
// The brain options are all opt-in and a plain run is unchanged by them, which is
// why they are a set of switches rather than a mode: each one buys something at a
// cost in time and tokens, and the honest default is none of them.

struct CrewOptions {
    // 0 leaves the engine's own default (4) alone rather than restating it here,
    // where it would go stale the moment ollamadev changes its mind.
    int  max_coders = 0;

    bool route    = false;   // pick each role's model by difficulty
    bool debate   = false;   // advocate/skeptic/judge vote per changeset
    bool dedupe   = false;   // hold coders whose work duplicates another's
    bool learn    = false;   // remember what this run teaches
    bool security = false;   // read-only vulnerability scan, producing a report

    // Raise the coder cap for a bigger fan-out. 0 leaves it alone.
    //
    // Distinct from max_coders, which BOUNDS the Director: --max caps how many
    // pieces it may plan, --swarm raises the ceiling on how many run at once. They
    // are not two spellings of one number, so both are here.
    int swarm = 0;

    // N Director plans, keep the modal one, then an N-reviewer audit panel. 0 off.
    //
    // The most expensive switch by a distance -- it multiplies both the planning
    // and the review -- which is why it is a number rather than a checkbox: the
    // cost is the value, so it should be the thing you set.
    int amplify = 0;

    // A saved team. Empty for none.
    std::string pack;

    bool operator==(const CrewOptions&) const = default;
};

// The argv for `ollamadev crew "<task>" [flags]`.
//
// The task is ONE argv element however many spaces, quotes or semicolons are in it,
// and it is the only free text here -- every other argument is a fixed flag or a
// number. Empty when the task is blank, because ollamadev treats a missing prompt
// as a request to start an interactive session rather than as an error.
std::vector<std::string> crew_run_command(const std::string& task,
                                          const CrewOptions& options);

// Parses `ollamadev crew pack` / `crew role`, which both print two columns:
// two leading spaces, a name, then a description. Only the name is wanted.
std::vector<std::string> parse_crew_names(const std::string& output);

// The saved teams and the personas the Director can assign. `project` is the
// directory the command runs in, because both lists include the ones defined by
// that project rather than only the built-ins.
std::vector<std::string> crew_packs(const std::filesystem::path& project = {});
std::vector<std::string> crew_roles(const std::filesystem::path& project = {});

// Whether `pack` is one the engine actually knows.
//
// Validated for the same reason a timezone is: it becomes an argument to a command
// that edits files, and "is it one the system itself listed" is the only check that
// cannot be argued with. An unknown pack is not a harmless typo -- ollamadev treats
// an unrecognised bare argument as a PROMPT.
bool is_known_pack(const std::string& pack, const std::vector<std::string>& known);

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

// --- the everyday flows ------------------------------------------------------
//
// The same set ollamadev-qt puts on its Start pane: the handful of engine commands
// you run against a project often enough to want one click away. Each opens a
// terminal in the chosen folder running the command, which is exactly what the Qt
// front end does with its tiles -- so the two behave the same way and neither is
// the odd one out.
//
// A TABLE rather than a switch, for the same reason agents.hpp is one: every argv
// here is a fixed literal, so there is no input -- from a model, a config file or
// a text box -- that can reach execvp through this. Adding a flow is adding a row.
//
// Deliberately NOT the whole of the Qt list. `setup` and `models` are things you do
// once or read once, and a panel popover is for what you do repeatedly; a menu that
// lists everything is one you have to read rather than aim at.
struct EngineAction {
    std::string label;
    std::string tooltip;
    std::vector<std::string> argv;

    bool operator==(const EngineAction&) const = default;
};

const std::vector<EngineAction>& engine_actions();

// --- the backends, and fanning one prompt across them -------------------------
//
// ollamadev-qt's AgentTeamPane: tick several providers, type one prompt, and each
// gets its own terminal running it. The nearest thing to what a "run an army of
// agents" product sells, and the engine already does all of it -- `ollamadev
// --backend <id> "<prompt>"` is the whole mechanism.

struct Backend {
    std::string id;         // "claude", "ollama" -- what --backend takes
    std::string label;      // "Claude Code" -- what `ollamadev backends` prints
    bool        installed = false;

    bool operator==(const Backend&) const = default;
};

// Every backend the engine knows, installed or not, with the id each label maps to.
//
// The ID IS NOT THE LABEL, and this is the whole reason this table exists: the
// command line wants "cursor-agent" while the listing prints "Cursor Agent".
// Deriving one from the other by lower-casing and hyphenating would work for six
// of the eleven and quietly produce "gemini-cli" for the seventh.
const std::vector<Backend>& known_backends();

// Parses `ollamadev backends`, whose first two lines are a header and a rule, and
// whose "Installed" column is "yes" or an em dash.
//
// Unknown labels are skipped rather than guessed at: a label with no id cannot be
// put on a command line, so listing it would offer a button that cannot work.
std::vector<Backend> parse_backends(const std::string& output);

std::vector<Backend> available_backends();

// The argv for one member of a team: `ollamadev --backend <id> "<prompt>"`.
//
// The prompt is ONE argv element however it is spelled. Empty when either the
// backend or the prompt is empty -- ollamadev with no prompt opens an interactive
// session, which is not what a Launch button means.
std::vector<std::string> backend_prompt_command(const std::string& backend_id,
                                                const std::string& prompt);

// --- the brain ----------------------------------------------------------------
//
// ollamadev-qt's BrainPane. --route picks each role's model by how hard its subtask
// is, and until now that was a switch you could turn on with no way to see what it
// decided, or to change what it would decide.

// The three difficulty tiers the router sorts work into. Fixed by the engine.
const std::vector<std::string>& router_tiers();

// `ollamadev config get router.<tier>` / `config set router.<tier> <model>`.
//
// Empty tier or model yields no command. The model is checked by the caller against
// the engine's own list before it gets here -- `config set` will write whatever it
// is given, and a typo becomes a model name that fails at the next run rather than
// at the moment it was entered.
std::vector<std::string> router_get_command(const std::string& tier);
std::vector<std::string> router_set_command(const std::string& tier,
                                            const std::string& model);

// What one request would be routed to, without running it.
//
// `ollamadev route "<text>"` prints one line:
//     → simple  ollama:gpt-oss:20b-cloud  (short lookup-style question)
struct RouteDecision {
    std::string tier;     // "simple", "moderate", "hard"
    std::string model;    // "ollama:gpt-oss:20b-cloud", backend prefix included
    std::string reason;   // the engine's own note, without its parentheses

    bool operator==(const RouteDecision&) const = default;
};

std::vector<std::string> route_command(const std::string& text);
RouteDecision            parse_route(const std::string& output);

// --- what it cost -------------------------------------------------------------
//
// ollamadev-qt's BrainPane token line. Read from the project's own
// .ollamadev/costs/usage.json rather than from `ollamadev stats`, because stats
// rounds for reading -- "57.9k" -- and a percentage split computed from rounded
// halves is a percentage that does not add up.

// True when a model tag names a hosted model rather than a local one.
//
// A suffix test, matching Models::isCloud in the engine: "-cloud" or ":cloud".
// Substring matching would call a local model named "cloudy-7b" hosted.
bool is_cloud_model(const std::string& tag);

// Token usage for one project, split by where the work ran.
//
// The split is the number worth showing on a local-first desktop: it answers "how
// much of this actually stayed on my machine", which is the claim the whole project
// makes. A bare total answers nothing.
struct TokenUsage {
    long long local = 0;
    long long cloud = 0;
    int       turns = 0;
    bool      known = false;   // false when nothing has been recorded here yet

    long long total() const { return local + cloud; }

    bool operator==(const TokenUsage&) const = default;
};

// <project>/.ollamadev/costs/usage.json.
std::filesystem::path usage_path(const std::filesystem::path& project);

TokenUsage parse_usage(const std::string& json_text);
TokenUsage project_usage(const std::filesystem::path& project);

// "60.4k tokens · 0% local · 100% cloud", or empty when nothing is recorded.
//
// Percentages are of the total and rounded to whole numbers; they are a sense of
// proportion, not an accounting.
std::string usage_summary(const TokenUsage& usage);

// `ollamadev models`, for the tier pickers. One name per line, and the engine
// marks the active one -- the marker is stripped so what comes back can be put
// straight onto a command line.
std::vector<std::string> parse_models(const std::string& output);
std::vector<std::string> available_models(const std::filesystem::path& project = {});

// --- reading a diff ----------------------------------------------------------

// What one line of a unified diff is, for colouring it.
enum class DiffLine { Added, Removed, Hunk, FileHeader, Context };

DiffLine classify_diff_line(std::string_view line);

// How many lines were added and removed, for a one-line summary above the patch.
struct DiffStat {
    int added   = 0;
    int removed = 0;

    bool operator==(const DiffStat&) const = default;
};

DiffStat diff_stat(const std::string& diff);

}  // namespace auspex
