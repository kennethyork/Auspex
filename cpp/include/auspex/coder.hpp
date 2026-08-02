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
// VERB, not a command. There is no verb here that runs a program, opens a shell,
// or reaches the network -- a coder reads and writes files inside its own sandbox
// and does nothing else. That is a real limit on what this crew can do (it cannot
// run the tests it writes) and it is the honest trade for a loop that cannot be
// talked into anything.
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
// `files` is the sandbox listing. `steps` is everything done so far, oldest first;
// it is summarised rather than replayed verbatim, because the contents the coder
// WROTE are already on disk and re-sending them would double the context for no
// gain.
std::string coder_prompt(const PlannedSubtask& subtask,
                         const std::vector<std::string>& files,
                         const std::vector<CoderStep>& steps,
                         const CoderLimits& limits);

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
CoderOutcome run_coder(const Config& config, const PlannedSubtask& subtask,
                       const std::filesystem::path& sandbox,
                       const CoderLimits& limits = {}, const std::string& model = {});

}  // namespace auspex
