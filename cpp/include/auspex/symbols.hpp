// Where things are actually defined.
//
// code_index.hpp answers "what reads like this" -- it embeds line windows and
// ranks them by cosine distance. That is genuinely useful for "where is the retry
// logic", and it is the wrong tool for "where is parse_plan defined", which has
// an exact answer that no amount of similarity will pin down. Worse, it needs an
// embedding model, a built index, and a model call per question, to answer
// something a parser answers instantly and correctly.
//
// So: definitions, by parsing. Nothing here talks to a model, nothing here needs
// an index built in advance, and the answer is a file and a line rather than a
// ranking.
//
// LINES ARE 1-BASED, as in linters.hpp and for the same reason: the consumers are
// a prompt and a person, and converting for nobody is how the off-by-one gets in.
//
// WHAT THIS IS NOT. It is not a language server and does not resolve types,
// imports, overloads or scope. It finds where a name is DECLARED, textually, in
// the languages whose declaration syntax is unambiguous enough to recognise
// without building a parse tree. That covers the question the crew actually asks
// -- "which file do I open to change X" -- and stops well short of pretending to
// understand the program. A wrong answer here would send a coder to edit the
// wrong function, so the rules are deliberately conservative: a construct that
// cannot be recognised confidently is not reported at all.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace auspex {

enum class SymbolKind {
    Function,
    Class,     // class, struct, interface, enum, trait
    Constant,  // a module-level assignment in the languages that have no keyword
};

std::string_view symbol_kind_name(SymbolKind kind);

struct Symbol {
    std::string name;
    SymbolKind  kind = SymbolKind::Function;
    std::string path;      // project-relative
    int         line = 0;  // 1-based
    // The declaration line itself, trimmed. What a person recognises the thing by,
    // and cheaper to show than to re-read the file for.
    std::string signature;

    bool operator==(const Symbol&) const = default;
};

// Every definition in one file's text.
//
// PURE -- text in, symbols out -- which is what makes the language rules testable
// without a project on disk. That matters more here than usual: these rules are
// regex-shaped, and regex-shaped rules rot silently.
std::vector<Symbol> symbols_in(const std::string& path, const std::string& text);

// True when this file's language has declaration rules we can recognise.
bool has_symbol_rules(const std::string& path);

// Every definition in the project, sorted by name then path.
//
// Reads the files it can parse and skips the rest. No index is stored: on a
// 36k-line project this takes long enough not to be free and short enough not to
// be worth the staleness a cache would bring.
std::vector<Symbol> project_symbols(const std::filesystem::path& root);

// Definitions of `name`, exactly. Empty when there are none.
//
// Exact rather than fuzzy on purpose: the question this answers is "where is X",
// and a near-miss answer to that sends a coder to edit the wrong function. Use
// find_symbols_like() when you want the forgiving version and know you do.
std::vector<Symbol> find_symbol(const std::filesystem::path& root,
                                const std::string& name);

// Definitions whose name CONTAINS `text`, case-insensitively. For a person
// searching, where a near miss is a hint rather than a wrong turn.
std::vector<Symbol> find_symbols_like(const std::filesystem::path& root,
                                      const std::string& text, int limit = 25);

// Where a name is used, as file:line, excluding the definitions themselves.
//
// Textual, and says so: a comment mentioning the name counts, and a different
// symbol with the same name in another scope counts. It answers "what would I
// have to look at if I changed this", which is the question worth asking before
// a rename, and it is honest about being a starting point rather than a set.
struct Reference {
    std::string path;
    int         line = 0;
    std::string text;   // the line, trimmed
};

std::vector<Reference> find_references(const std::filesystem::path& root,
                                       const std::string& name, int limit = 50);

// The block handed to a coder or the Researcher: where the names in a task are
// defined. Empty when none of them are found, so nothing is added rather than a
// heading with nothing under it.
std::string symbols_note(const std::filesystem::path& root, const std::string& task,
                         int limit = 8);

// The identifier-shaped words in a piece of text, longest first.
//
// Exposed because it decides what symbols_note goes looking for, and getting it
// wrong shows up as a note full of matches for "the".
std::vector<std::string> candidate_names(const std::string& text);

}  // namespace auspex
