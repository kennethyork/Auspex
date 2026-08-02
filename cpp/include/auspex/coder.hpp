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

namespace auspex {

enum class CoderTool {
    List,      // what files are here
    Read,      // the contents of one
    Write,     // replace one, or create it
    Delete,    // remove one
    Run,       // run one of a fixed set of build/test programs, in the sandbox
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
struct CoderLimits {
    // Steps, not turns of conversation: a read is a step. Twenty-four is enough to
    // look at half a dozen files and write three, and small enough that a model
    // stuck in a loop costs seconds rather than an afternoon.
    int max_steps = 24;

    // A file read is truncated to this many bytes before going into the transcript.
    // Without it one large file fills the context window and every later turn is
    // answered with the subtask pushed out of view.
    std::size_t max_read_bytes = 24'000;

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
std::string coder_prompt(const PlannedSubtask& subtask,
                         const std::vector<std::string>& files,
                         const std::vector<CoderStep>& steps,
                         const CoderLimits& limits,
                         const std::string& steered = {});

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
                    const CoderLimits& limits);

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

CoderOutcome run_coder(const Config& config, const PlannedSubtask& subtask,
                       const std::filesystem::path& sandbox,
                       const CoderLimits& limits = {}, const std::string& model = {},
                       const std::filesystem::path& mailbox = {});

}  // namespace auspex
