#include "auspex/agents.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>

#include "auspex/process.hpp"
#include "auspex/projects.hpp"

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
        // The crew's own engine. Listed here so "open ollamadev in Auspex" and
        // "start the crew in Auspex" reach the same folder by the same route --
        // before this it was the one agent the panel could run but not open.
        {"ollamadev", "OllamaDev", "ollamadev",
         {"ollamadev", "ollama dev", "olama dev", "llama dev", "ollamadev cli"}},
        {"qwen", "Qwen Code", "qwen", {"qwen", "qwen code", "quinn", "qwen coder"}},
        {"aider", "Aider", "aider", {"aider", "eider", "aide"}},
        // Known to ollamadev and resolvable here, so a coder backend can name
        // them. Not installed on this machine, which available_agents() reports
        // rather than guessing at.
        {"goose", "Goose", "goose", {"goose", "block goose"}},
        {"amp", "Amp", "amp", {"amp", "sourcegraph amp"}},
        {"crush", "Crush", "crush", {"crush", "charm crush"}},
        {"droid", "Droid", "droid", {"droid", "factory droid"}},
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
            tools.push_back({.key = def.key, .label = def.label, .binary = def.binary, .path = {}});
        }
        return tools;
    }();
    return kTools;
}

const std::vector<std::filesystem::path>& agent_search_dirs() {
    static const std::vector<std::filesystem::path> kDirs = [] {
        std::vector<std::filesystem::path> dirs;
        const char* home_env = std::getenv("HOME");
        if (!home_env || !*home_env) return dirs;
        const std::filesystem::path home(home_env);

        // The fixed ones, in the order a person would expect them to win.
        for (const char* relative : {"bin", ".local/bin", ".bun/bin", ".deno/bin",
                                     ".cargo/bin", ".npm-global/bin", ".volta/bin",
                                     ".asdf/shims", ".yarn/bin"}) {
            dirs.push_back(home / relative);
        }
        dirs.emplace_back("/usr/local/bin");
        dirs.emplace_back("/opt/homebrew/bin");   // harmless on Linux, right on macOS

        // Version-manager trees, where the interesting directory is named after a
        // version and cannot be written down. Enumerated newest-first by name,
        // which is what these managers' own directory names sort as.
        const auto add_versioned = [&dirs](const std::filesystem::path& root,
                                           const std::filesystem::path& suffix) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root, ec)) return;
            std::vector<std::filesystem::path> versions;
            for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
                if (entry.is_directory(ec)) versions.push_back(entry.path());
            }
            std::sort(versions.begin(), versions.end(), std::greater<>());
            for (const auto& version : versions) dirs.push_back(version / suffix);
        };
        add_versioned(home / ".nvm" / "versions" / "node", "bin");
        add_versioned(home / ".local" / "share" / "fnm" / "node-versions",
                      std::filesystem::path("installation") / "bin");
        add_versioned(home / ".nodenv" / "versions", "bin");

        return dirs;
    }();
    return kDirs;
}

std::string resolve_agent_binary(const std::string& binary) {
    // $PATH first, so a deliberately-installed copy still wins over one that
    // happens to be lying in a version manager's tree.
    if (auto found = resolve_in_path(binary); !found.empty()) return found;

    for (const auto& dir : agent_search_dirs()) {
        std::error_code ec;
        const auto candidate = dir / binary;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return candidate.string();
        }
    }
    return {};
}

std::vector<AgentTool> available_agents() {
    std::vector<AgentTool> present;
    for (const auto& tool : known_agents()) {
        AgentTool resolved = tool;
        resolved.path = resolve_agent_binary(tool.binary);
        if (!resolved.path.empty()) present.push_back(std::move(resolved));
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
                    // Resolved here so EVERY caller gets an absolute path without
                    // having to know it needs one -- the voice verb reaches
                    // agent_terminal_command() through a different route than the
                    // panel does, and only one of them would have been fixed.
                    // Empty when it is not installed, which callers already check.
                    return AgentTool{.key    = def.key,
                                     .label  = def.label,
                                     .binary = def.binary,
                                     .path   = resolve_agent_binary(def.binary)};
                }
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
std::vector<std::string> agent_terminal_command(const std::string& terminal,
                                                const AgentTool&   agent,
                                                const std::filesystem::path& directory) {
    // The resolved absolute path when there is one; see AgentTool::path for why a
    // bare name is not good enough. Falling back to the name keeps the voice verb
    // and the tests working with a hand-built AgentTool.
    const std::string program = agent.path.empty() ? agent.binary : agent.path;
    return terminal_command_in(terminal, program, directory);
}

}  // namespace auspex
