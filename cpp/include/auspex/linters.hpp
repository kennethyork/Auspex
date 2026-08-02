// Does the code parse? Asked of a parser, not of a model.
//
// This is the answer to the one grade the crew genuinely deserved: everything
// upstream of here is a model's opinion about another model's output, and twice
// on this project an Auditor held correct code with a confident, self-
// contradicting reason. A syntax error is not a matter of opinion. `python3 -m
// py_compile` either accepts the file or names the line, and it is right both
// times.
//
// So the Auditor gets told, before it is asked anything: this changeset does not
// parse, at file X line N, according to Y. That is evidence, and unlike the
// Auditor's own quoted line it does not need quote_is_real() to check whether it
// was invented.
//
// ONLY PARSERS. Every tool driven here reads one file and answers about that file
// alone. gcc, g++ and rustc are deliberately ABSENT even though this is a C++
// project and they are installed: `g++ -fsyntax-only` on one file of a real
// project fails on the first #include it cannot resolve, and a linter that
// reports "no such file" for correct code would hold correct changesets. A false
// hold is worse than no check at all -- it teaches you to ignore the check. The
// tools kept here resolve nothing and depend on nothing: they answer "is this
// valid syntax", which is exactly the question being asked.
//
// NOTHING HERE RUNS THE CODE. py_compile compiles to bytecode without executing,
// `node --check` parses and exits, `ruby -c` and `php -l` are syntax-only, gofmt
// only formats. That is what makes this safe to leave ON while CoderLimits::
// allow_run is off: running a coder's tests is a risk the owner of the machine
// must accept, and asking whether its file parses is not.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "auspex/sandbox.hpp"

namespace auspex {

// LINES ARE 1-BASED, as every tool driven here reports them and as every editor
// and every human counts them. ollamadev converts to LSP's 0-based coordinates at
// the parse because it serves an LSP; Auspex has no LSP, and its consumers are a
// prompt and a person. Converting for nobody is how the classic off-by-one gets
// in, so there is no conversion anywhere in this file.
struct Diagnostic {
    std::string path;      // project-relative
    int         line = 0;  // 1-based; 0 when the tool named no line
    int         column = 0;
    std::string message;
    std::string source;    // which tool said so: "python3 -m py_compile", "node --check"

    // One line, the way it is shown to a person and put in a prompt.
    std::string format() const;

    bool operator==(const Diagnostic&) const = default;
};

// The tool that would check this path, as an argv template, or empty when the
// extension is not one we can check or the tool is not installed.
//
// The FILENAME is appended by the caller -- this returns the program and its
// flags only, so no path ever passes through a string that gets split later.
std::vector<std::string> linter_for(const std::string& path);

// Every extension this knows how to check, whether or not the tool is present.
// For reporting what a project could be checked with if the tools were there.
const std::vector<std::string>& lintable_extensions();

// True when the file has an extension we check AND the tool exists on this box.
bool can_lint(const std::string& path);

// Check one file that already exists on disk, at `root / path`.
//
// A missing tool yields NO diagnostics rather than an error: "we could not check"
// and "it is fine" must not be the same answer, so the caller is told which it
// got via can_lint() -- and the Auditor is only ever told about files that were
// actually checked.
std::vector<Diagnostic> lint_file(const std::filesystem::path& root,
                                  const std::string& path);

// Check every file in a changeset that can be checked, reading them from
// `sandbox` where the coder left them.
//
// Deletions are skipped -- there is nothing to parse -- and so is any file whose
// language we have no parser for, which on most changesets is most of them. That
// is the honest outcome: this finds a real class of error completely, and says
// nothing at all about the rest.
std::vector<Diagnostic> lint_changeset(const Changeset& changeset,
                                       const std::filesystem::path& sandbox);

// The same, with no sandbox to read from: the changeset already carries the new
// contents, so the lintable files are staged into a private temp directory and
// checked there. The directory is removed before this returns.
//
// This works only because of the restriction at the top of this file. Every tool
// kept here parses one file and resolves nothing -- no imports, no includes, no
// module graph -- so a file checked alone gives the same answer as the same file
// checked in its project. A checker that resolved dependencies would report
// garbage under these conditions, which is the second reason gcc and rustc are
// not here.
std::vector<Diagnostic> lint_changeset(const Changeset& changeset);

// How many files in this changeset could be checked at all. Reported alongside
// the diagnostics so "no errors" can be distinguished from "nothing was checked".
int lintable_count(const Changeset& changeset);

// Reads one tool's output into diagnostics.
//
// Exposed for testing: every one of these tools has its own idea of how to report
// a syntax error, and the parsing is where this would quietly break when a tool
// changes its format. The tests pin real captured output.
std::vector<Diagnostic> parse_diagnostics(const std::string& tool,
                                          const std::string& path,
                                          const std::string& output);

// The block handed to the Auditor. Empty when nothing was checked or nothing was
// wrong -- an empty string adds nothing to the prompt, which is what we want when
// there is nothing to say.
std::string diagnostics_block(const std::vector<Diagnostic>& diagnostics);

}  // namespace auspex
