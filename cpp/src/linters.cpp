#include "auspex/linters.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"

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

// Leading digits as a number, 0 when there are none. Used on every tool's line
// field; a tool that changes its format yields 0 rather than a wrong line number,
// and a diagnostic with no line still names the file.
int leading_int(const std::string& s) {
    int value = 0;
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    bool any = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        value = value * 10 + (s[i] - '0');
        ++i;
        any = true;
        if (value > 10'000'000) break;   // a line number this large is a misparse
    }
    return any ? value : 0;
}

// The number after `marker` ("on line ", "line "), 0 if absent.
int int_after(const std::string& text, const std::string& marker) {
    const auto at = text.find(marker);
    if (at == std::string::npos) return 0;
    return leading_int(text.substr(at + marker.size()));
}

// A JSON syntax check done in-process. No subprocess for this one: we already
// link a JSON parser, and spawning python to ask whether a file is valid JSON
// would be slower and would depend on python being installed to check a file that
// has nothing to do with python.
std::vector<Diagnostic> lint_json(const std::string& path, const std::string& text) {
    std::vector<Diagnostic> found;
    nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_discarded()) return found;

    // Parse again for the message; the non-throwing form above gives none.
    Diagnostic d;
    d.path = path;
    d.source = "json";
    try {
        (void)nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        d.message = e.what();
        // nlohmann reports a byte offset, not a line. Convert here, once, rather
        // than handing a person a number they would have to convert themselves.
        std::size_t offset = e.byte;
        if (offset > text.size()) offset = text.size();
        d.line = 1 + static_cast<int>(std::count(text.begin(), text.begin() + offset, '\n'));
    }
    if (d.message.empty()) d.message = "invalid JSON";
    found.push_back(d);
    return found;
}

// True when node's complaint is about the module system rather than the syntax.
//
// Both spellings occur: the import form names the statement, the export form
// comes back as an unexpected token. Neither says anything is wrong with the code.
bool is_module_confusion(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) {
        if (d.message.find("Cannot use import statement outside a module") !=
            std::string::npos) {
            return true;
        }
        if (d.message.find("Unexpected token 'export'") != std::string::npos) return true;
    }
    return false;
}

// Ask node again with the file named .mjs, which it always treats as a module.
std::vector<Diagnostic> lint_as_module(const std::filesystem::path& source,
                                       const std::string& path) {
    std::vector<Diagnostic> found;
    std::error_code ec;
    const auto stage = std::filesystem::temp_directory_path(ec) /
                       ("auspex-mjs-" + std::to_string(::getpid()));
    if (ec) return found;
    std::filesystem::create_directories(stage, ec);

    const auto copy = stage / "module.mjs";
    std::filesystem::copy_file(source, copy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        std::filesystem::remove_all(stage, ec);
        return found;
    }

    const LimitedResult ran = run_limited({"node", "--check", "module.mjs"},
                                          stage.string(), /*timeout_seconds=*/10,
                                          /*max_output=*/8'000);
    if (!ran.ok && !ran.timed_out) {
        // A real syntax error, reported against the file the caller asked about
        // rather than against the temp copy nobody has ever heard of.
        found = parse_diagnostics("node", path, ran.output);
    }
    std::filesystem::remove_all(stage, ec);
    return found;
}

}  // namespace

std::string Diagnostic::format() const {
    std::ostringstream out;
    out << path;
    if (line > 0) {
        out << ":" << line;
        if (column > 0) out << ":" << column;
    }
    out << ": " << message;
    if (!source.empty()) out << "  [" << source << "]";
    return out.str();
}

const std::vector<std::string>& lintable_extensions() {
    static const std::vector<std::string> kExts{".py",  ".js",  ".mjs", ".cjs",
                                                ".json", ".php", ".go",  ".rb"};
    return kExts;
}

std::vector<std::string> linter_for(const std::string& path) {
    const std::string ext = extension_of(path);

    // .json is handled in-process; it has no argv and is not "not lintable".
    if (ext == ".json") return {};

    if (ext == ".py") {
        // Compiles to bytecode WITHOUT executing the module. The __pycache__ it
        // leaves behind is in sandbox_excludes(), so it never reaches a changeset.
        if (!in_path("python3")) return {};
        return {"python3", "-m", "py_compile"};
    }
    if (ext == ".js" || ext == ".mjs" || ext == ".cjs") {
        // Parses and exits. --check never runs the program.
        if (!in_path("node")) return {};
        return {"node", "--check"};
    }
    if (ext == ".php") {
        if (!in_path("php")) return {};
        return {"php", "-l"};
    }
    if (ext == ".go") {
        // gofmt, not `go vet`: vet takes packages and needs the module to resolve,
        // which is exactly the dependency this file refuses to take on. -e reports
        // all syntax errors rather than stopping at the first.
        if (!in_path("gofmt")) return {};
        return {"gofmt", "-e"};
    }
    if (ext == ".rb") {
        if (!in_path("ruby")) return {};
        return {"ruby", "-c"};
    }
    return {};
}

bool can_lint(const std::string& path) {
    if (extension_of(path) == ".json") return true;   // always, it is in-process
    return !linter_for(path).empty();
}

std::vector<Diagnostic> parse_diagnostics(const std::string& tool,
                                          const std::string& path,
                                          const std::string& output) {
    std::vector<Diagnostic> found;
    if (trim(output).empty()) return found;
    const auto lines = split_lines(output);

    if (tool == "python3") {
        // The shape:
        //     File "calc.py", line 3
        //       def f(:
        //             ^
        //   SyntaxError: invalid syntax
        // The line number and the message are on different lines, so both are
        // collected and joined at the end.
        Diagnostic d;
        d.path = path;
        d.source = "python3 -m py_compile";
        for (const auto& line : lines) {
            const auto at = line.find(", line ");
            if (at != std::string::npos && line.find("File \"") != std::string::npos) {
                d.line = int_after(line, ", line ");
            }
            const auto err = line.find("Error: ");
            if (err != std::string::npos && d.message.empty()) {
                d.message = trim(line.substr(line.find_first_not_of(" \t")));
            }
        }
        if (!d.message.empty()) found.push_back(d);
        return found;
    }

    if (tool == "node") {
        // The shape:
        //   /abs/or/rel/path.js:2
        //     return 1
        //            ^
        //   SyntaxError: Unexpected number
        //       at internalCompileFunction (node:internal/vm:73:18)
        // The trailing stack is node's own internals and says nothing about the
        // user's file, so it is dropped: "at node:internal/modules/cjs/loader"
        // in an Auditor's prompt is noise that reads like evidence.
        Diagnostic d;
        d.path = path;
        d.source = "node --check";
        for (const auto& line : lines) {
            if (d.line == 0) {
                const auto colon = line.find_last_of(':');
                if (colon != std::string::npos && colon + 1 < line.size() &&
                    line.find(".js") != std::string::npos) {
                    const int n = leading_int(line.substr(colon + 1));
                    if (n > 0) d.line = n;
                }
            }
            const auto err = line.find("Error: ");
            if (err != std::string::npos && d.message.empty() &&
                line.rfind("    at ", 0) != 0) {
                d.message = trim(line);
            }
            // The caret line gives the column the tool itself pointed at.
            const auto caret = line.find('^');
            if (caret != std::string::npos &&
                line.find_first_not_of(" \t^~") == std::string::npos && d.column == 0) {
                d.column = static_cast<int>(caret) + 1;
            }
        }
        if (!d.message.empty()) found.push_back(d);
        return found;
    }

    if (tool == "php") {
        // "PHP Parse error:  syntax error, unexpected token "{" in x.php on line 2"
        for (const auto& line : lines) {
            if (line.find("error") == std::string::npos &&
                line.find("Error") == std::string::npos) {
                continue;
            }
            if (line.rfind("Errors parsing", 0) == 0) continue;   // the summary line
            Diagnostic d;
            d.path = path;
            d.source = "php -l";
            d.line = int_after(line, " on line ");
            d.message = trim(line);
            found.push_back(d);
        }
        return found;
    }

    if (tool == "gofmt" || tool == "ruby") {
        // "file:line:col: message" (gofmt) and "file:line: message" (ruby).
        for (const auto& line : lines) {
            const auto first = line.find(':');
            if (first == std::string::npos) continue;
            Diagnostic d;
            d.path = path;
            d.source = tool == "gofmt" ? "gofmt -e" : "ruby -c";

            std::string rest = line.substr(first + 1);
            d.line = leading_int(rest);
            if (d.line == 0) continue;   // not a positioned diagnostic

            const auto second = rest.find(':');
            if (second != std::string::npos) {
                const std::string after = rest.substr(second + 1);
                const int col = leading_int(after);
                const auto third = after.find(':');
                if (col > 0 && third != std::string::npos) {
                    d.column = col;
                    d.message = trim(after.substr(third + 1));
                } else {
                    d.message = trim(after);
                }
            }
            if (d.message.empty()) continue;
            found.push_back(d);
        }
        return found;
    }

    return found;
}

std::vector<Diagnostic> lint_file(const std::filesystem::path& root,
                                  const std::string& path) {
    std::vector<Diagnostic> found;

    // The same check as everywhere else a model-chosen filename meets the disk.
    // A sandbox is a copy, not a jail -- see coder.hpp.
    const auto resolved = safe_join(root, path);
    if (!resolved || !std::filesystem::is_regular_file(*resolved)) return found;

    if (extension_of(path) == ".json") {
        std::ifstream in(*resolved, std::ios::binary);
        if (!in) return found;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return lint_json(path, buffer.str());
    }

    auto argv = linter_for(path);
    if (argv.empty()) return found;
    const std::string tool = argv[0];
    argv.push_back(path);

    // Short deadline: these are parsers, and a parser that has not answered in ten
    // seconds is not going to. Nothing here is allowed to become the slow part of
    // an audit.
    const LimitedResult ran = run_limited(argv, root.string(), /*timeout_seconds=*/10,
                                          /*max_output=*/8'000);
    // Exit zero is the file parsing. Anything else, we read the output -- and a
    // tool that failed for its OWN reasons yields no parsed diagnostics, which is
    // the right outcome: we could not check, so we say nothing.
    if (ran.ok) return found;
    if (ran.timed_out) return found;

    found = parse_diagnostics(tool, path, ran.output);

    // A .js file that is really an ES module.
    //
    // node decides CommonJS-or-module from the extension and the nearest
    // package.json, neither of which a single-file check can see. So a perfectly
    // valid module named .js comes back as "Cannot use import statement outside a
    // module" -- which is not a syntax error at all, and holding a changeset over
    // it would be exactly the false positive this file exists to avoid.
    //
    // Found by pointing --lint at this repository, which is what that mode is for.
    // The fix is to ask again the other way: the same bytes under a .mjs name,
    // which is the one extension node treats as a module unconditionally. If it
    // parses as a module, there was nothing wrong with it.
    if (tool == "node" && !found.empty() && is_module_confusion(found)) {
        found = lint_as_module(*resolved, path);
    }
    return found;
}

int lintable_count(const Changeset& changeset) {
    int n = 0;
    for (const auto& file : changeset.files) {
        if (!file.deleted && can_lint(file.path)) ++n;
    }
    return n;
}

std::vector<Diagnostic> lint_changeset(const Changeset& changeset,
                                       const std::filesystem::path& sandbox) {
    std::vector<Diagnostic> all;
    for (const auto& file : changeset.files) {
        if (file.deleted) continue;          // nothing left to parse
        if (!can_lint(file.path)) continue;  // no parser for this language
        const auto found = lint_file(sandbox, file.path);
        all.insert(all.end(), found.begin(), found.end());
    }
    return all;
}

std::vector<Diagnostic> lint_changeset(const Changeset& changeset) {
    std::vector<Diagnostic> all;
    if (lintable_count(changeset) == 0) return all;

    std::error_code ec;
    const auto stage = std::filesystem::temp_directory_path(ec) /
                       ("auspex-lint-" + std::to_string(::getpid()) + "-" +
                        std::to_string(std::filesystem::hash_value(
                            std::filesystem::path(changeset.files.front().path))));
    if (ec) return all;
    std::filesystem::remove_all(stage, ec);
    if (!std::filesystem::create_directories(stage, ec)) return all;

    for (const auto& file : changeset.files) {
        if (file.deleted || !can_lint(file.path)) continue;
        const auto target = safe_join(stage, file.path);
        if (!target) continue;
        std::filesystem::create_directories(target->parent_path(), ec);
        std::ofstream out(*target, std::ios::binary | std::ios::trunc);
        if (!out) continue;
        out << file.contents;
        out.close();
        const auto found = lint_file(stage, file.path);
        all.insert(all.end(), found.begin(), found.end());
    }

    std::filesystem::remove_all(stage, ec);
    return all;
}

std::string diagnostics_block(const std::vector<Diagnostic>& diagnostics) {
    if (diagnostics.empty()) return {};
    std::ostringstream out;
    out << "The following are facts from a compiler, not opinions. This code does "
           "not parse:\n";
    // Capped: a file that lost a brace on line 1 can produce hundreds of cascading
    // errors, and the first few are the only ones that mean anything.
    constexpr std::size_t kMax = 12;
    for (std::size_t i = 0; i < diagnostics.size() && i < kMax; ++i) {
        out << "  " << diagnostics[i].format() << "\n";
    }
    if (diagnostics.size() > kMax) {
        out << "  ... and " << (diagnostics.size() - kMax) << " more\n";
    }
    return out.str();
}

}  // namespace auspex
