#include "auspex/router.hpp"

#include <algorithm>
#include <cctype>

#include "auspex/process.hpp"

namespace auspex {

namespace {

// Words that mark a genuinely hard software task -- the kind worth a strong model.
const std::vector<std::string>& hard_words() {
    static const std::vector<std::string> kWords{
        "design",     "architect",   "architecture", "refactor",  "implement",
        "algorithm",  "optimize",    "optimise",     "debug",     "diagnose",
        "prove",      "migrate",     "concurrency",  "thread",    "race condition",
        "security",   "vulnerab",    "port ",        "rewrite",   "profile",
        "benchmark",  "distributed", "recursive",    "parser",    "compiler",
        "state machine"};
    return kWords;
}

// Openers that mark a trivial ask -- a small model is plenty.
const std::vector<std::string>& simple_words() {
    static const std::vector<std::string> kWords{
        "what is", "what's",    "who is", "when ",    "where ",  "define",
        "rename",  "list ",     "spell",  "translate", "summarize", "summarise",
        "tldr",    "one line",  "briefly", "hello",   "hi ",     "thanks",
        "capital of"};
    return kWords;
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

const std::vector<std::string>& router_tiers_list() {
    static const std::vector<std::string> kTiers{"simple", "moderate", "hard"};
    return kTiers;
}

Difficulty classify_difficulty(const std::string& text) {
    const std::string p = lowered(trim(text));
    if (p.empty()) return {"moderate", "nothing to judge"};

    const auto contains = [&p](const std::string& needle) {
        return p.find(needle) != std::string::npos;
    };

    // Code, or an indented block, is hard whatever the words say: the model has to
    // reason over real code rather than answer a one-liner.
    if (contains("```") || contains("    ")) return {"hard", "contains code"};

    for (const auto& word : hard_words()) {
        if (contains(word)) return {"hard", "mentions \"" + trim(word) + "\""};
    }

    if (p.size() > 400) return {"hard", "long / detailed request"};

    // Short AND lookup-shaped. The length gate comes first on purpose: without it
    // "design a cache" would slip through as simple on brevity alone, which is the
    // one mistake here that actually costs something.
    if (p.size() <= 80) {
        for (const auto& word : simple_words()) {
            if (p.rfind(word, 0) == 0 || contains(word)) {
                return {"simple", "short lookup-style question"};
            }
        }
        if (p.size() <= 24) return {"simple", "very short"};
    }

    return {"moderate", "general request"};
}

std::string tier_for_role(const std::string& role) {
    // Fixed rather than classified, because these do not depend on the subtask.
    // The Director reasons over a whole codebase to cut a job up; the Researcher
    // reads and summarises. Reviewing is judgement, and the role that has been
    // wrong most often on this project is the one to spend on.
    if (role == "director" || role == "auditor" || role == "judge" ||
        role == "security") {
        return "hard";
    }
    if (role == "researcher" || role == "advocate" || role == "skeptic") {
        return "moderate";
    }
    // Coders are the exception: their difficulty is the subtask's, so a caller
    // classifies rather than asking here.
    return "moderate";
}

std::string tier_model(const Config& config, const std::string& tier) {
    const auto found = config.crew_role_models.find("tier_" + tier);
    return found == config.crew_role_models.end() ? std::string{} : found->second;
}

std::string route_model(const Config& config, const std::string& chosen,
                        const std::string& tier) {
    // An explicit choice wins outright. The router fills gaps; it does not
    // overrule a person -- a setting that silently does nothing is exactly the bug
    // this project has already shipped once.
    if (!chosen.empty()) return chosen;
    return tier_model(config, tier);
}

}  // namespace auspex
