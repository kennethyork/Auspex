// A coder: one subtask, one sandbox, and a loop that ends.
//
// This is the agentic half of the crew, and the piece Auspex had no foundation
// for -- commands.cpp is a fixed eight-verb whitelist for DESKTOP actions with no
// file access at all, and OllamaClient offers `generate` and nothing else. So the
// tool loop is written here from nothing rather than adapted.
//
// THE SHAPE. Each turn the model gets the subtask, a listing of the sandbox, and a
// transcript of what it has done so far; it answers with ONE JSON object naming
// one tool. The tool runs, its result joins the transcript, and the loop goes
// round. It ends when the model says finish, when the step budget runs out, or
// when it stops making progress.
//
// WHY A FIXED VERB TABLE, AGAIN. Same reason as agents.hpp: the model names a
// VERB, not a command line.
//
// `run` is the exception that proves it, and the one place a model's output turns
// into a process. A tester that cannot run tests is doing proofreading, so it had
// to exist -- but it is bounded on every side:
//
//   * THE PROGRAM COMES FROM A FIXED ALLOWLIST. `pytest`, `cargo`, `make` and the
//     like. Not `sh`, not `bash`, not `env`, not `sudo`, not `ssh`, not `curl` --
//     anything that would run something else, or reach the network, or leave the
//     machine. A program that is not on the list is refused by name.
//   * THERE IS NO SHELL. execvp with an argv vector, so `;`, `|`, backticks and
//     `$(...)` are ordinary characters in an argument, not syntax.
//   * IT RUNS IN THE SANDBOX. A throwaway copy, never the real project.
//   * IT IS KILLED ON A DEADLINE, with its whole process group, and its output is
//     capped.
//
// What this does NOT do is contain a program that is on the list. `make` runs a
// Makefile the coder may have just written; `pytest` imports code it wrote. That
// is the actual risk, it is the same risk as running an unfamiliar repository's
// tests on your own machine, and no allowlist can remove it. It is off by default
// for exactly that reason -- see CoderLimits::allow_run.
//
// WHY PATHS ARE STILL CHECKED. Every path goes through safe_join() against the
// sandbox even though the sandbox is a throwaway copy. A copy is not a jail: a
// path with ".." in it walks out of the temp directory and into the real
// filesystem exactly as it would anywhere else. The copy limits what a CORRECT
// coder can damage; safe_join is what limits an incorrect one.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "auspex/config.hpp"
#include "auspex/director.hpp"
#include <functional>
#include <utility>

#include "auspex/hooks.hpp"
#include "auspex/mcp.hpp"
#include "auspex/skills.hpp"

namespace auspex {

enum class CoderTool {
    List,      // what files are here
    Read,      // the contents of one
    Write,     // replace one, or create it
    Replace,   // swap one exact piece of text in a file for another
    Delete,    // remove one
    Run,       // run one of a fixed set of build/test programs, in the sandbox
    Skill,     // open a skill listed in the catalogue
    Mcp,       // call a tool offered by a configured MCP server
    Finish,    // done, with a note
    Unknown,   // the model asked for something that is not a verb
};

// The verb names as the model must spell them, for the prompt and for parsing.
std::string_view tool_name(CoderTool tool);
CoderTool        tool_from_name(const std::string& name);

struct ToolCall {
    CoderTool   tool = CoderTool::Unknown;
    std::string path;
    std::string contents;
    std::string note;
    // For Skill: which one. Reuses `path` would be confusing, so it is its own
    // field -- a skill name is a slug, not a filename.
    std::string skill;
    // For Replace: the exact text to find, and what to put there.
    //
    // WHY THIS VERB EXISTS. `write` replaces a whole file, and a read is capped at
    // 24KB. So on any file bigger than the cap a coder could not make a safe
    // change AT ALL: to alter one line it would have to rewrite the 90% it had
    // never seen. Watched on this project -- asked to change one line of a
    // 2118-line file, the coder read the right place, understood the change, and
    // correctly refused to write, because writing would have destroyed the rest.
    // Every eval task was a forty-byte file, so nothing showed it.
    std::string find;
    std::string replace_with;

    // For Read: the line to start at, 1-based. 0 means the beginning.
    //
    // Without this a file bigger than max_read_bytes was UNREACHABLE past the cap.
    // The truncation was stated -- "this file is longer than shown" -- and there
    // was no way to act on it, which is a warning with no remedy. Watched on this
    // project: asked to change a function at line 1165 of a 2118-line file, the
    // coder read the first 24KB, did not find it, and read the same 24KB again
    // until its budget ran out.
    int from_line = 0;
    // For Mcp: which tool ("server.tool") and its arguments as JSON text.
    std::string mcp_tool;
    std::string mcp_arguments;
    // For Run: the program and its arguments, already split. Never a string to be
    // word-split later -- that is where a shell would sneak back in.
    std::vector<std::string> command;
    // Why the reply could not be turned into a call. Fed back to the model, which
    // is usually enough for it to correct itself on the next turn.
    std::string error;

    bool operator==(const ToolCall&) const = default;
};

struct ToolResult {
    bool        ok = false;
    // Succeeded but changed nothing -- a write whose contents the file already
    // had. Distinct from a failure, because the coder did nothing WRONG; it just
    // did nothing. The loop counts these as not-progress, which a bare `ok` cannot
    // express.
    bool        no_op = false;
    std::string output;   // what the model is told happened

    bool operator==(const ToolResult&) const = default;
};

struct CoderStep {
    ToolCall   call;
    ToolResult result;

    bool operator==(const CoderStep&) const = default;
};

struct CoderOutcome {
    // True only when the model said finish. A coder that ran out of budget may
    // still have written useful files -- the changeset is captured either way --
    // but the difference matters to the Auditor and to anyone reading the board.
    bool                   finished = false;
    std::string            note;
    std::vector<CoderStep> steps;
    // Set when the loop stopped for a reason that is not "the coder finished".
    std::string            error;
    // Which model did it. Carried so the run's state file can name it per subtask,
    // which is what makes routing observable on the board.
    std::string            model;

    int writes() const;
};

// How hard a coder may try.
// The MCP tools on offer, and how to call them.
//
// Passed in rather than discovered inside the loop: starting every configured
// server on every turn would be absurd, and the set does not change while a crew
// runs. `call` is a function so the loop needs no knowledge of transports.
struct McpAccess {
    std::vector<McpTool> tools;
    // (qualified name, arguments JSON) -> (ok, text). Empty means MCP is off.
    std::function<std::pair<bool, std::string>(const std::string&, const std::string&)>
        call;

    bool empty() const { return tools.empty() || !call; }
};

// The skill catalogue and the skills themselves.
//
// Passed in rather than looked up inside the loop, so run_coder() stays a function
// of its arguments and the prompt builder can be tested without a filesystem.
struct SkillSet {
    std::string              catalog;   // one line each, for the prompt
    std::vector<::auspex::Skill> skills;

    bool empty() const { return skills.empty(); }
};

struct CoderLimits {
    // Steps, not turns of conversation: a read is a step. Twenty-four is enough to
    // look at half a dozen files and write three, and small enough that a model
    // stuck in a loop costs seconds rather than an afternoon.
    int max_steps = 24;

    // A file read is truncated to this many bytes before going into the transcript.
    // Without it one large file fills the context window and every later turn is
    // answered with the subtask pushed out of view.
    // Smaller than it was, deliberately: a read is a WINDOW, and several small
    // windows the coder can hold at once beat one large one it cannot.
    std::size_t max_read_bytes = 12'000;

    // How much of the transcript's READ output is replayed each turn, newest
    // first. Older reads become a one-line note naming the file.
    //
    // This is a per-turn cost multiplied by the step budget, so getting it wrong
    // is expensive in a way that is invisible on a small project. Measured on
    // Auspex itself: a coder that had read five files was sending 125KB on every
    // subsequent turn -- one run cost 1.6 MILLION input tokens across 33 calls,
    // for a task whose answer was a one-line change. A 40-byte calc.py never
    // showed it.
    //
    // The newest read is the one the coder is working from; an older one it still
    // needs it can read again, and the repeat guard nudges rather than kills.
    // Four windows, not one.
    //
    // This started equal to max_read_bytes, which meant exactly ONE read survived
    // and every new one evicted the last -- so a coder could never hold the top of
    // a file and the middle at the same time. Watched it thrash between line 1120
    // and line 1 because of it: a fix for runaway cost that created a new failure,
    // found only by tracing the loop again after making it.
    std::size_t max_replayed_reads = 48'000;

    // Read-only: write, delete and run are refused whatever the model asks for.
    //
    // This is what makes a Researcher safe to point at the real project rather
    // than a copy. It is enforced in run_tool(), not by leaving the verbs out of
    // the prompt -- a model that asks for `write` anyway gets a refusal, not a
    // written file.
    bool read_only = false;

    // Whether `run` exists at all.
    //
    // OFF BY DEFAULT. Turning it on lets a coder execute a test suite it just
    // wrote, inside its sandbox -- which is the point, and is also the same risk
    // as running an unfamiliar repository's tests on your own machine. That is a
    // decision for the person whose machine it is, not a default.
    bool allow_run = false;

    // How long one command may take, and how much of its output is kept.
    int         run_timeout_seconds = 60;
    std::size_t max_run_output      = 12'000;

    // Consecutive calls at the same target before the coder is called stuck.
    //
    // Covers three things that all look like working and are not: repeating a
    // failing read, rewriting a file with the contents it already has, and
    // rewriting the same file over and over without reading it in between. The
    // last one is the common one -- an observed run spent six of its nine steps
    // writing one file repeatedly, and every repetition costs a full turn of
    // context as well as the tokens.
    int max_repeats = 3;

    // The user's own gates, loaded from the home config by run_coder().
    //
    // Carried here rather than looked up inside run_tool() so the loop stays a
    // function of its arguments -- and so a test can hand it a hook without
    // creating a file in the tester's home directory.
    //
    // Empty means no gates, which ALLOWS. That is not the same as a hook failing
    // closed: see hooks.hpp. Nobody configuring nothing must not mean nobody can
    // run a coder.
    std::vector<Hook> hooks;

    bool operator==(const CoderLimits&) const = default;
};

// The prompt for one turn.
//
// `steered` is a message from a person, shown once and prominently. Empty usually.
//
// `files` is the sandbox listing. `steps` is everything done so far, oldest first;
// it is summarised rather than replayed verbatim, because the contents the coder
// WROTE are already on disk and re-sending them would double the context for no
// gain.
// `hint` is what is known about the project before the coder starts -- chiefly
// where the names in its subtask are DEFINED, from symbols.hpp.
//
// It is the difference between jumping to a line and scanning for it. Watched on
// this project: asked to change a function at line 1165 of a 2118-line file, a
// coder with no hint spent its whole budget reading 24KB windows and never
// reached it.
std::string coder_prompt(const PlannedSubtask& subtask,
                         const std::vector<std::string>& files,
                         const std::vector<CoderStep>& steps,
                         const CoderLimits& limits,
                         const std::string& steered = {},
                         const SkillSet& skills = {}, const McpAccess& mcp = {},
                         const std::string& hint = {});

// Reads one reply into a call.
//
// A reply that names no known verb yields Unknown with an `error` saying so,
// rather than being guessed at. Guessing here means writing a file the model did
// not ask to write.
ToolCall parse_tool_call(const std::string& reply);

// Runs one call against `sandbox`. Every path is resolved through safe_join first;
// anything that escapes fails the call rather than the run, so the model is told
// and can correct itself.
ToolResult run_tool(const ToolCall& call, const std::filesystem::path& sandbox,
                    const CoderLimits& limits, const SkillSet& skills = {},
                    const McpAccess& mcp = {});

// The loop. Blocking -- run it off the GTK thread.
//
// Never throws and always returns: a coder that cannot reach the model, cannot
// parse a reply, or will not stop is a coder that produced nothing, which is a
// result the crew can carry on from.
// Programs a coder may run, when running is allowed at all.
//
// Build and test drivers only. Deliberately absent: every shell, `env`, `xargs`,
// `find`, `sudo`, `ssh`, `curl`, `wget`, `nc`, `git` -- anything whose job is to
// run something else, reach the network, or change the machine outside the
// sandbox.
const std::vector<std::string>& runnable_programs();
bool is_runnable(const std::string& program);

// A file a running coder reads between turns, so a person can say something to it.
//
// A FILE rather than a queue or a socket, because the two ends are not in the same
// place: the window may be in one process and the run in another, and a run
// outlives the window that started it. A path both ends can name is the whole
// mechanism.
//
// Read-and-truncate: the message is consumed on the turn it is seen, so it is
// injected once rather than repeated into every prompt for the rest of the run.
std::string take_steer(const std::filesystem::path& mailbox);
bool        leave_steer(const std::filesystem::path& mailbox, const std::string& message);

// The Researcher: read the project, report what a team needs to know.
//
// Runs BEFORE the Director and hands its findings to everyone downstream -- where
// things live, the conventions in use, the files the work will touch. A filename
// list and a semantic index say what EXISTS; this says what it means.
//
// Read-only by construction: the limits it runs under refuse every writing verb,
// so it can be pointed at the real project rather than a sandbox.
std::string run_researcher(const Config& config, const std::string& task,
                           const std::filesystem::path& project,
                           const CoderLimits& limits = {},
                           const std::string& model = {});

std::string researcher_prompt(const std::string& task,
                              const std::vector<std::string>& files,
                              const std::vector<CoderStep>& steps,
                              const CoderLimits& limits);

CoderOutcome run_coder(const Config& config, const PlannedSubtask& subtask,
                       const std::filesystem::path& sandbox,
                       const CoderLimits& limits = {}, const std::string& model = {},
                       const std::filesystem::path& mailbox = {},
                       const SkillSet& skills = {}, const McpAccess& mcp = {});

}  // namespace auspex
