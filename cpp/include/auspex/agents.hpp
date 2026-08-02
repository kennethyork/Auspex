// Coding agents you can open by voice.
//
// "open a claude code agent" / "open a codex agent" / "start gemini".
//
// WHY A FIXED TABLE, AND NOT A NAME THE MODEL CHOOSES:
//
// The whole security posture of the command layer is that the model never names a
// program to run. `launch_app` already bends that as far as it goes -- it takes a
// model-supplied name, but only after rejecting anything with a path separator or a
// shell metacharacter, and only ever with no arguments. An agent CLI cannot use
// that path: it has to be started inside a terminal, which means arguments, and
// arguments plus a model-chosen binary is exactly the shape of hole this project
// exists to not have.
//
// So the model does not choose a binary here. It returns a *key*, that key is
// looked up in the table below, and the argv comes from the table. An unknown key
// resolves to nothing and the verb fails. The worst a compromised or confused model
// can do with `open_agent` is open one of a handful of agent CLIs the user already
// installed -- there is no input that reaches execvp except by matching a literal
// in this file.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct AgentTool {
    std::string key;      // canonical id, and what the model is asked to return
    std::string label;    // how it is spoken back: "Claude Code"
    std::string binary;   // the executable's name

    // Where it actually is, absolute. Empty when it is not installed.
    //
    // Filled by available_agents() and used instead of `binary` on every command
    // line, for a reason that cost an afternoon to find: a terminal that spawns
    // its window from a long-lived server process -- which xfce4-terminal does by
    // default, and gnome-terminal always -- executes the command in THAT process's
    // environment. Its PATH is whatever it inherited whenever it started, not the
    // panel's. A bare name resolved fine in Auspex and then failed to resolve in
    // the terminal, which opened an empty window and looked like the agent had
    // crashed on startup.
    std::string path;

    bool operator==(const AgentTool&) const = default;
};

// Directories searched for an agent BEYOND $PATH.
//
// WHY THIS IS NEEDED: the panel is started by the display manager, so its PATH is
// the login one -- roughly ~/.local/bin plus the system directories. It never runs
// a login shell, so it never sources the lines that nvm, bun, deno, cargo and
// friends append to a profile. Every agent installed by one of those was therefore
// invisible to the panel while being on the PATH of every terminal its owner ever
// opened, which is as confusing a failure as this can produce: the tool is plainly
// installed, and the shell that is meant to launch it says it is not.
//
// These are fixed literals under $HOME. Nothing here comes from a model, a config
// file or a command line, so widening the search does not widen what can be run --
// the NAME is still only ever one from the table above.
const std::vector<std::filesystem::path>& agent_search_dirs();

// The absolute path `binary` resolves to, searching $PATH first and then the
// directories above. Empty when it is not installed anywhere Auspex looks.
std::string resolve_agent_binary(const std::string& binary);

// Every agent Auspex knows how to open, installed or not. Order is the order they
// are offered to the model, so the better-known ones come first.
const std::vector<AgentTool>& known_agents();

// Only those actually present in PATH. What the panel should offer and what the
// model is told exists -- there is no point letting it pick something that is not
// installed, and the failure message is better when it never gets the chance.
std::vector<AgentTool> available_agents();

// Maps something a person said to an agent. Tolerant of the ways a name arrives
// from speech recognition: case, punctuation, and the filler words that always
// attach to these ("a claude code agent", "the codex cli", "open ai codex").
//
// Returns nothing for an unrecognised name. That is the only gate between model
// output and a process, so it fails closed.
std::optional<AgentTool> resolve_agent(std::string_view spoken);

// Exposed for testing: the normalisation resolve_agent() applies before matching.
std::string normalise_agent_name(std::string_view spoken);

// argv that opens `agent` inside `terminal`, optionally in `directory`.
//
// Terminals disagree about the flag that means "then run this", and about the one
// that means "starting here". Both tables live in projects.cpp, which this
// delegates to: one copy, because the failure mode of a second is an empty
// terminal that reads as the agent having crashed instantly.
//
// An empty directory means "wherever the caller is", which is what the voice verb
// wants -- speech has no way to name a folder. The picker passes a real one.
//
// Empty if either the terminal or the agent is empty.
std::vector<std::string> agent_terminal_command(const std::string& terminal,
                                                const AgentTool&   agent,
                                                const std::filesystem::path& directory = {});

}  // namespace auspex
