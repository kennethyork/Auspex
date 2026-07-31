// Spoken-command interpretation and execution.
//
// This is the piece src/magi_shell/desktop_assistant.py was meant to be: turning
// an utterance into a desktop action rather than just an answer.
//
// SAFETY MODEL -- the important part of this file:
//
//   1. The model never produces a command line. It produces one JSON object whose
//      "action" must be one of a fixed set of verbs. Anything else degrades to
//      Answer (speak the text), never to execution.
//   2. Every action is executed via execvp with an argv vector. No shell is
//      involved anywhere, so shell metacharacters in a target are inert -- a
//      hallucinated `; rm -rf ~` is a literal filename argument to xdg-open, not
//      a command.
//   3. Targets are validated against reality before use: a path must exist on
//      disk, an app must resolve in PATH, a window must actually be open, a
//      workspace index must be in range, a volume must be 0-100. A hallucinated
//      target fails closed with a spoken explanation.
//   4. Window focus never trusts a model-supplied window id. The model names a
//      title; the id is looked up from the live window list.
#pragma once

#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "auspex/config.hpp"
#include "auspex/canvas.hpp"
#include "auspex/desktop.hpp"

namespace auspex {

enum class ActionKind {
    Answer,           // no desktop action; speak `target`
    OpenPath,         // xdg-open a file or directory
    LaunchApp,        // run an executable found in PATH, no arguments
    SwitchWorkspace,  // go to workspace `number`
    FocusWindow,      // raise the window whose title matches `target`
    SetVolume,        // set the default sink to `number` percent
    OpenUrl,          // open an http/https URL in the browser
    WebSearch,        // search the web for `target`
    OpenTerminal,     // spawn a terminal onto the canvas
    OpenAgent,        // open a coding agent CLI on the canvas; `target` names it
    RunCrew,          // hand a task to the ollamadev crew; `target` is the USER's words
    CrewAccept,       // apply held changeset `number`
    CrewDiscard,      // bin held changeset `number`
    CrewSteer,        // talk to coder `number`; `target` is the USER's words
    ShowBoard,        // show what the crew is holding
    CrewResume,       // finish an interrupted crew run
    PanCanvas,        // pan the canvas viewport; `target` is a direction
};

std::string_view to_string(ActionKind kind);

struct Action {
    ActionKind  kind   = ActionKind::Answer;
    std::string target;
    int         number = 0;

    bool operator==(const Action&) const = default;
};

// What the model is allowed to know about the desktop, so its targets can be
// grounded in things that actually exist.
struct CommandContext {
    int                      workspace_count = 4;
    std::vector<WindowEntry> windows;
    std::string              focused_window;
    std::string              selection;

    // Copied from Config so execute_action() needs no separate Config argument.
    std::string browser;
    std::string search_url;
    std::string terminal;

    // Canvas state lives in the host process (the shell or the server), not in the
    // command layer, so it is borrowed here. Null means no canvas is running and the
    // canvas verbs report that rather than silently doing nothing.
    Canvas* canvas = nullptr;
    Rect    monitor{};

    // Recent (question, answer) turns, oldest first. Replayed into the prompt so
    // follow-ups like "what about the second one?" resolve. voice_assistant.py had
    // this; Auspex was stateless until now.
    std::vector<std::pair<std::string, std::string>> history;

    // What the user actually said, verbatim from the transcript.
    //
    // Load-bearing for run_crew and for nothing else. The crew takes a free-text
    // task, and that text becomes an argument to a subprocess -- so it must be the
    // human's own words, NOT something the model wrote. The model's only job for
    // this verb is deciding that it IS a crew request; the payload never passes
    // through it. Every other verb takes either no argument or one validated
    // against reality, and this keeps run_crew in the same posture.
    std::string utterance;
};

CommandContext gather_context(const Config& config);

// The instruction given to the model. Kept in one place so it can be inspected
// and tested rather than being buried in a string literal at the call site.
std::string build_command_prompt(const std::string& utterance,
                                 const CommandContext& context);

struct ParseResult {
    std::optional<Action> action;
    std::string           error;   // set when the output could not be used
};

// Extracts the first balanced JSON object from `model_output` (a reasoning model
// may wrap it in prose), then validates it against the whitelist and `context`.
ParseResult parse_action(const std::string& model_output, const CommandContext& context);

// Strips the phrasing people wrap a crew request in, leaving the task itself.
//
// "have the crew add rate limiting to the api"  ->  "add rate limiting to the api"
// "ask the crew to write tests for canvas"      ->  "write tests for canvas"
//
// Exposed because it is the whole of the argument construction for run_crew, and
// the one place a bad result would send nonsense to a tool that edits files.
std::string crew_task_from_utterance(std::string_view utterance);

struct ExecResult {
    bool        ok = false;
    std::string message;   // spoken back to the user
};

ExecResult execute_action(const Action& action, const CommandContext& context);

// Resolves a spoken path: expands ~, and if that misses, retries the final
// component case-insensitively inside its parent, so "~/downloads" finds
// "~/Downloads". nullopt if nothing on disk matches.
std::optional<std::filesystem::path> resolve_path(std::string_view spoken);

}  // namespace auspex
