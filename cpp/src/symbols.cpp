#include "auspex/symbols.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#include "auspex/process.hpp"
#include "auspex/sandbox.hpp"

namespace auspex {

namespace {

std::string extension_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    std::string ext = name.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// One declaration rule: a pattern whose first capture is the name.
//
// The regex is COMPILED ONCE, when the rule table is first built. Constructing it
// per line instead made project_symbols take longer than five minutes on a 36k
// line repository -- std::regex construction is expensive and it was happening
// lines x rules times.
struct Rule {
    SymbolKind kind;
    std::regex re;

    Rule(SymbolKind k, const char* pattern) : kind(k), re(pattern) {}
};

enum class Language { None, Python, CFamily, JavaScript, Go, Rust, Ruby, Php };

Language language_of(const std::string& path) {
    const std::string ext = extension_of(path);
    if (ext == ".py") return Language::Python;
    if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".cc" || ext == ".hpp" ||
        ext == ".hh" || ext == ".cxx") {
        return Language::CFamily;
    }
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs" || ext == ".ts" ||
        ext == ".tsx" || ext == ".jsx") {
        return Language::JavaScript;
    }
    if (ext == ".go") return Language::Go;
    if (ext == ".rs") return Language::Rust;
    if (ext == ".rb") return Language::Ruby;
    if (ext == ".php") return Language::Php;
    return Language::None;
}

const std::vector<Rule>& rules_for(Language language) {
    static const std::vector<Rule> kNone{};

    static const std::vector<Rule> kPython{
        Rule{SymbolKind::Function, R"(^\s*(?:async\s+)?def\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"},
        Rule{SymbolKind::Class,    R"(^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)\s*[\(:])"},
        // Module-level constants only -- the leading anchor has no indentation, so
        // a local variable inside a function is not a definition anyone can go to.
        Rule{SymbolKind::Constant, R"(^([A-Z][A-Z0-9_]{2,})\s*=)"},
    };

    static const std::vector<Rule> kCFamily{
        Rule{SymbolKind::Class,    R"(^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|union|enum(?:\s+class)?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:final)?\s*[:{])"},
        // A definition, not a declaration: it must be followed by an opening brace
        // on the same line. Prototypes in a header would otherwise double every
        // function and send a coder to the header to change behaviour.
        Rule{SymbolKind::Function, R"(^[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{\s*$)"},
    };

    static const std::vector<Rule> kJavaScript{
        Rule{SymbolKind::Function, R"(^\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?function\s*\*?\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*\()"},
        Rule{SymbolKind::Class,    R"(^\s*(?:export\s+)?(?:default\s+)?class\s+([A-Za-z_$][A-Za-z0-9_$]*)\b)"},
        // const f = (…) => and const f = function(…)
        Rule{SymbolKind::Function, R"(^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*(?:async\s*)?(?:function\b|\([^)]*\)\s*=>|[A-Za-z_$][A-Za-z0-9_$]*\s*=>))"},
    };

    static const std::vector<Rule> kGo{
        Rule{SymbolKind::Function, R"(^func\s+(?:\([^)]*\)\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*\()"},
        Rule{SymbolKind::Class,    R"(^type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(?:struct|interface)\b)"},
    };

    static const std::vector<Rule> kRust{
        Rule{SymbolKind::Function, R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+)?(?:unsafe\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*[<(])"},
        Rule{SymbolKind::Class,    R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:struct|enum|trait|union)\s+([A-Za-z_][A-Za-z0-9_]*)\b)"},
    };

    static const std::vector<Rule> kRuby{
        Rule{SymbolKind::Function, R"(^\s*def\s+(?:self\.)?([A-Za-z_][A-Za-z0-9_?!]*))"},
        Rule{SymbolKind::Class,    R"(^\s*(?:class|module)\s+([A-Za-z_][A-Za-z0-9_:]*))"},
    };

    static const std::vector<Rule> kPhp{
        Rule{SymbolKind::Function, R"(^\s*(?:(?:public|private|protected|static|final|abstract)\s+)*function\s+&?([A-Za-z_][A-Za-z0-9_]*)\s*\()"},
        Rule{SymbolKind::Class,    R"(^\s*(?:abstract\s+|final\s+)?(?:class|interface|trait|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\b)"},
    };

    switch (language) {
        case Language::Python:     return kPython;
        case Language::CFamily:    return kCFamily;
        case Language::JavaScript: return kJavaScript;
        case Language::Go:         return kGo;
        case Language::Rust:       return kRust;
        case Language::Ruby:       return kRuby;
        case Language::Php:        return kPhp;
        case Language::None:       break;
    }
    return kNone;
}

// Words that are keywords rather than names, in any of the languages above.
// Without this, `if (…) {` in C parses as a function called `if`.
bool is_keyword(const std::string& word) {
    static const std::vector<std::string> kWords{
        "if", "for", "while", "switch", "catch", "return", "else", "do", "try",
        "sizeof", "new", "delete", "case", "default", "throw", "using", "namespace",
        "typedef", "template", "operator", "static_assert", "constexpr", "decltype",
        "and", "or", "not", "in", "is", "with", "match", "loop", "unsafe", "impl",
    };
    return std::find(kWords.begin(), kWords.end(), word) != kWords.end();
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// How many '(' are still open at the end of the text. Negative is impossible in
// well-formed code and is treated as balanced.
int unbalanced_parens(const std::string& text) {
    int depth = 0;
    for (const char c : text) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
    }
    return depth;
}

// A whole-word occurrence of `name`, so `total` does not match `subtotal`.
bool mentions_word(const std::string& line, const std::string& name) {
    std::size_t from = 0;
    while ((from = line.find(name, from)) != std::string::npos) {
        const bool left_ok =
            from == 0 || (!std::isalnum(static_cast<unsigned char>(line[from - 1])) &&
                          line[from - 1] != '_');
        const std::size_t after = from + name.size();
        const bool right_ok =
            after >= line.size() ||
            (!std::isalnum(static_cast<unsigned char>(line[after])) &&
             line[after] != '_');
        if (left_ok && right_ok) return true;
        from = after;
    }
    return false;
}

}  // namespace

std::string_view symbol_kind_name(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Function: return "function";
        case SymbolKind::Class:    return "type";
        case SymbolKind::Constant: return "constant";
    }
    return "symbol";
}

bool has_symbol_rules(const std::string& path) {
    return language_of(path) != Language::None;
}

std::vector<Symbol> symbols_in(const std::string& path, const std::string& text) {
    std::vector<Symbol> found;
    const Language language = language_of(path);
    if (language == Language::None) return found;

    const auto& rules = rules_for(language);
    const auto lines = lines_of(text);

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty()) continue;

        // A line that is only a comment cannot declare anything. Cheap, and it
        // removes the commonest false positive: commented-out code.
        const std::string trimmed = trim(line);
        if (trimmed.rfind("//", 0) == 0 || trimmed.rfind("#", 0) == 0 ||
            trimmed.rfind("*", 0) == 0 || trimmed.rfind("/*", 0) == 0) {
            continue;
        }

        // A declaration is not two thousand characters long, and a minified or
        // generated line can make a backtracking regex take effectively forever.
        // Skipping them costs nothing real and removes the whole class.
        if (line.size() > 400) continue;

        // A signature that wraps is still one declaration.
        //
        // Found on this repository: safe_join() came back "not found" because its
        // parameters run onto a second line, which is the house style here and in
        // most C++. Matching only single lines would have missed a large share of
        // the project's own functions -- and a symbol index that silently misses
        // things is worse than none, because you stop checking.
        //
        // Joined only until the parentheses balance, and only a few lines, so an
        // unbalanced file cannot swallow the rest of itself.
        std::string logical = line;
        if (unbalanced_parens(line) > 0) {
            for (std::size_t j = i + 1; j < lines.size() && j <= i + 4; ++j) {
                logical += " " + trim(lines[j]);
                if (logical.size() > 400) break;
                if (unbalanced_parens(logical) <= 0) break;
            }
        }
        if (logical.size() > 400) continue;

        for (const auto& rule : rules) {
            std::smatch match;
            // The joined form, so a wrapped signature matches; the line number
            // reported is still the first one, which is where a person looks.
            if (!std::regex_search(logical, match, rule.re)) continue;
            if (match.size() < 2) continue;

            const std::string name = match[1].str();
            if (name.empty() || is_keyword(name)) continue;

            found.push_back(
                {name, rule.kind, path, static_cast<int>(i) + 1, trimmed});
            break;   // one declaration per line is enough
        }
    }
    return found;
}

std::vector<Symbol> project_symbols(const std::filesystem::path& root) {
    std::vector<Symbol> all;
    for (const auto& [path, contents] : list_files(root)) {
        if (!has_symbol_rules(path)) continue;
        auto found = symbols_in(path, contents);
        all.insert(all.end(), found.begin(), found.end());
    }

    std::sort(all.begin(), all.end(), [](const Symbol& a, const Symbol& b) {
        if (a.name != b.name) return a.name < b.name;
        if (a.path != b.path) return a.path < b.path;
        return a.line < b.line;
    });
    return all;
}

std::vector<Symbol> find_symbol(const std::filesystem::path& root,
                                const std::string& name) {
    std::vector<Symbol> found;
    if (trim(name).empty()) return found;
    for (auto& symbol : project_symbols(root)) {
        if (symbol.name == name) found.push_back(std::move(symbol));
    }
    return found;
}

std::vector<Symbol> find_symbols_like(const std::filesystem::path& root,
                                      const std::string& text, int limit) {
    std::vector<Symbol> found;
    const std::string wanted = lowered(trim(text));
    if (wanted.empty() || limit <= 0) return found;

    for (auto& symbol : project_symbols(root)) {
        if (lowered(symbol.name).find(wanted) == std::string::npos) continue;
        found.push_back(std::move(symbol));
        if (static_cast<int>(found.size()) >= limit) break;
    }
    return found;
}

std::vector<Reference> find_references(const std::filesystem::path& root,
                                       const std::string& name, int limit) {
    std::vector<Reference> found;
    if (trim(name).empty() || limit <= 0) return found;

    // Where it is DEFINED, so those lines can be left out -- the caller already
    // has those from find_symbol and is asking a different question.
    std::vector<std::pair<std::string, int>> definitions;
    for (const auto& symbol : find_symbol(root, name)) {
        definitions.emplace_back(symbol.path, symbol.line);
    }

    for (const auto& [path, contents] : list_files(root)) {
        const auto lines = lines_of(contents);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (!mentions_word(lines[i], name)) continue;

            const int line_number = static_cast<int>(i) + 1;
            const bool is_definition =
                std::find(definitions.begin(), definitions.end(),
                          std::make_pair(path, line_number)) != definitions.end();
            if (is_definition) continue;

            found.push_back({path, line_number, trim(lines[i])});
            if (static_cast<int>(found.size()) >= limit) return found;
        }
    }
    return found;
}

std::vector<std::string> candidate_names(const std::string& text) {
    std::vector<std::string> names;

    // Identifier-shaped and long enough to mean something. Three is the floor
    // because two-letter words are almost all English, and a note full of matches
    // for "id" is worse than no note.
    const std::regex word(R"([A-Za-z_][A-Za-z0-9_]{2,})");
    for (std::sregex_iterator it(text.begin(), text.end(), word), end; it != end; ++it) {
        std::string candidate = it->str();

        // Ordinary English that happens to be identifier-shaped. Without this the
        // note is dominated by "the", "and", "function", "should".
        static const std::vector<std::string> kNoise{
            "the",  "and",  "for",  "that",  "this",  "with",  "from",  "into",
            "make", "add",  "fix",  "should", "must", "not",   "use",   "using",
            "when", "then", "does", "return", "returns", "file", "files", "code",
            "test", "tests", "function", "class", "method", "value", "values",
            "new",  "old",  "one",  "two",   "all",  "any",   "its",   "have",
            // Words that are verbs in a task sentence and function names
            // everywhere. "and update parse_plan" is an instruction, not a
            // reference to a function called update -- and offering one is a
            // wrong file opened, where offering nothing costs the coder a `list`.
            // The asymmetry is why this list leans toward dropping.
            "update", "change", "remove", "replace", "ensure", "allow", "support",
            "handle", "improve", "rename", "move", "extend", "treat", "mention",
            "call", "check", "print", "show", "give", "take", "keep", "stop",
        };
        if (std::find(kNoise.begin(), kNoise.end(), lowered(candidate)) != kNoise.end()) {
            continue;
        }
        if (std::find(names.begin(), names.end(), candidate) != names.end()) continue;
        names.push_back(std::move(candidate));
    }

    // Longest first: a longer identifier is a more specific one, and when the
    // caller caps the list it is the vague names that should go.
    std::stable_sort(names.begin(), names.end(),
                     [](const std::string& a, const std::string& b) {
                         return a.size() > b.size();
                     });
    return names;
}

std::string symbols_note(const std::filesystem::path& root, const std::string& task,
                         int limit) {
    if (limit <= 0) return {};

    // The project is read ONCE and the candidates matched against it, rather than
    // find_symbol() per candidate -- which would re-read and re-parse the whole
    // project for every word in the task.
    const auto all = project_symbols(root);
    if (all.empty()) return {};

    std::vector<Symbol> hits;
    for (const auto& candidate : candidate_names(task)) {
        std::vector<Symbol> matches;
        for (const auto& symbol : all) {
            if (symbol.name == candidate) matches.push_back(symbol);
        }

        // A name defined in many places points nowhere.
        //
        // "update" is a word people write in tasks and also the name of a function
        // in half the projects on disk; offering four candidate definitions of it
        // is not a lead, it is four chances to open the wrong file. A name worth
        // reporting is one with few enough definitions to actually mean something.
        constexpr std::size_t kTooCommon = 3;
        if (matches.empty() || matches.size() > kTooCommon) continue;

        for (auto& symbol : matches) {
            hits.push_back(std::move(symbol));
            if (static_cast<int>(hits.size()) >= limit) break;
        }
        if (static_cast<int>(hits.size()) >= limit) break;
    }
    if (hits.empty()) return {};

    std::ostringstream out;
    out << "Names from the task, and where they are defined:\n";
    for (const auto& symbol : hits) {
        out << "  " << symbol.name << " (" << symbol_kind_name(symbol.kind) << ") "
            << symbol.path << ":" << symbol.line << "\n";
    }
    return out.str();
}

}  // namespace auspex
