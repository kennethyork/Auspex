#include "auspex/agents.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

#include "auspex/process.hpp"

namespace auspex {

namespace {

// Aliases are matched after normalisation, so they are written here already
// normalised: lower case, no punctuation, single spaces, and without the filler
// words stripped below. Every alias is a literal -- nothing here is a pattern, so
// there is no way for an unexpected input to match by accident.
struct AgentDefinition {
    const char*              key;
    const char*              label;
    const char*              binary;
    std::vector<const char*> aliases;
};

const std::vector<AgentDefinition>& definitions() {
    static const std::vector<AgentDefinition> kAgents{
        {"claude", "Claude Code", "claude",
         {"claude", "claude code", "anthropic", "claude cli", "cloud", "clod"}},
        {"codex", "Codex", "codex",
         {"codex", "openai codex", "open ai codex", "open eye codex", "codecs"}},
        {"gemini", "Gemini", "gemini",
         {"gemini", "google gemini", "jiminy", "gemini cli"}},
        {"cursor", "Cursor Agent", "cursor-agent",
         {"cursor", "cursor agent", "cursoragent"}},
        {"opencode", "OpenCode", "opencode", {"opencode", "open code"}},
        {"qwen", "Qwen Code", "qwen", {"qwen", "qwen code", "quinn", "qwen coder"}},
        {"aider", "Aider", "aider", {"aider", "eider", "aide"}},
    };
    return kAgents;
}

// Words that arrive attached to every one of these names and mean nothing. Removed
// as whole words, so "opencode" is untouched while "open code agent" is not.
const std::vector<std::string>& filler_words() {
    static const std::vector<std::string> kFiller{"a",   "an",     "the",  "agent",
                                                  "cli", "tool",   "new",  "session",
                                                  "up",  "please", "start", "open"};
    return kFiller;
}

}  // namespace

// ---------------------------------------------------------------------------
namespace {

// Splits on anything that is not a letter or digit, lower-casing as it goes. This
// folds "claude-code", "Claude Code" and "claude_code." together, and it also means
// no character that survives can be a shell metacharacter or a path separator --
// the result is only ever compared against literals, never executed, but keeping it
// alphanumeric makes that obvious rather than something to be argued.
std::vector<std::string> words_of(std::string_view spoken) {
    std::vector<std::string> words;
    std::string              word;
    for (std::size_t i = 0; i <= spoken.size(); ++i) {
        const unsigned char c = i < spoken.size() ? spoken[i] : ' ';
        if (std::isalnum(c)) {
            word.push_back(static_cast<char>(std::tolower(c)));
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    return words;
}

std::string join(const std::vector<std::string>& words) {
    std::string result;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i) result.push_back(' ');
        result += words[i];
    }
    return result;
}

}  // namespace

std::string normalise_agent_name(std::string_view spoken) {
    std::vector<std::string> kept;
    for (const auto& word : words_of(spoken)) {
        const auto& filler = filler_words();
        if (std::find(filler.begin(), filler.end(), word) == filler.end()) {
            kept.push_back(word);
        }
    }
    return join(kept);
}

// ---------------------------------------------------------------------------
const std::vector<AgentTool>& known_agents() {
    static const std::vector<AgentTool> kTools = [] {
        std::vector<AgentTool> tools;
        for (const auto& def : definitions()) {
            tools.push_back({.key = def.key, .label = def.label, .binary = def.binary});
        }
        return tools;
    }();
    return kTools;
}

std::vector<AgentTool> available_agents() {
    std::vector<AgentTool> present;
    for (const auto& tool : known_agents()) {
        if (in_path(tool.binary)) present.push_back(tool);
    }
    return present;
}

std::optional<AgentTool> resolve_agent(std::string_view spoken) {
    // Two candidate forms, tried in order:
    //
    //   1. filler stripped -- "open a claude code agent" -> "claude code"
    //   2. filler kept     -- "open code" -> "open code"
    //
    // Both are needed because the filler list and the agent names overlap: "open"
    // is meaningless in "open a codex agent" and load-bearing in "open code". One
    // pass cannot have it both ways, and two passes over a table of literals costs
    // nothing.
    const std::string stripped = normalise_agent_name(spoken);
    const std::string verbatim = join(words_of(spoken));

    for (const std::string& name : {stripped, verbatim}) {
        if (name.empty()) continue;
        for (const auto& def : definitions()) {
            for (const char* alias : def.aliases) {
                if (name == alias) {
                    return AgentTool{
                        .key = def.key, .label = def.label, .binary = def.binary};
                }
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::vector<std::string> agent_terminal_command(const std::string& terminal,
                                                const AgentTool& agent) {
    if (terminal.empty() || agent.binary.empty()) return {};

    // Match on the basename: `terminal` may be an absolute path from PATH lookup.
    const std::string name = std::filesystem::path(terminal).filename().string();

    // gnome-terminal removed -e in 3.14 and prints a deprecation warning instead of
    // running anything useful; -- is the supported form. kitty, foot and alacritty
    // take the command as trailing arguments with no separator at all (alacritty
    // does have -e, but the bare form works on every version). Everything else in
    // config.cpp's candidate list understands -e.
    if (name == "gnome-terminal" || name == "mate-terminal") {
        return {terminal, "--", agent.binary};
    }
    if (name == "kitty" || name == "foot" || name == "alacritty" || name == "wezterm") {
        return {terminal, agent.binary};
    }
    return {terminal, "-e", agent.binary};
}

}  // namespace auspex
