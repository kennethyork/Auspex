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

#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct AgentTool {
    std::string key;      // canonical id, and what the model is asked to return
    std::string label;    // how it is spoken back: "Claude Code"
    std::string binary;   // the executable, resolved in PATH at use time

    bool operator==(const AgentTool&) const = default;
};

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

// argv that opens `agent` inside `terminal`.
//
// Terminals disagree about the flag that means "then run this": most take -e,
// gnome-terminal deprecated it in favour of --, and kitty/foot want the command
// with no flag at all. Getting this wrong opens an empty terminal, which looks like
// the agent crashed instantly, so it is decided per terminal rather than hoped for.
//
// Empty if either argument is empty.
std::vector<std::string> agent_terminal_command(const std::string& terminal,
                                                const AgentTool& agent);

}  // namespace auspex
