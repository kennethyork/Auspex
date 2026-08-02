#include "auspex/verify.hpp"

#include <algorithm>
#include <sstream>

#include "auspex/coder.hpp"
#include "auspex/process.hpp"
#include "auspex/sandbox.hpp"

namespace auspex {

namespace {

bool has(const std::vector<std::string>& names, const std::string& wanted) {
    return std::find(names.begin(), names.end(), wanted) != names.end();
}

// Any file under a directory of this name, or any path starting with it.
bool has_under(const std::vector<std::string>& names, const std::string& prefix) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) {
        return n.rfind(prefix, 0) == 0;
    });
}

bool any_matching(const std::vector<std::string>& names, const std::string& prefix,
                  const std::string& suffix) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& n) {
        const auto slash = n.find_last_of('/');
        const std::string base = slash == std::string::npos ? n : n.substr(slash + 1);
        return base.rfind(prefix, 0) == 0 && base.size() >= suffix.size() &&
               base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0;
    });
}

}  // namespace

std::optional<TestCommand> detect_tests_from(const std::vector<std::string>& names) {
    // ORDER IS THE DESIGN. A Python project frequently also has a Makefile, and a
    // Node project frequently also has a Makefile; the language-specific runner is
    // the better answer in both cases. Make comes last precisely because it is the
    // thing everything has.
    const auto python_tests =
        any_matching(names, "test_", ".py") || any_matching(names, "", "_test.py") ||
        has_under(names, "tests/") || has_under(names, "test/");

    if (python_tests && (has(names, "pyproject.toml") || has(names, "setup.py") ||
                         has(names, "setup.cfg") || has(names, "pytest.ini") ||
                         has(names, "tox.ini") || python_tests)) {
        if (in_path("pytest")) return TestCommand{{"pytest", "-q"}, "pytest"};
        // pytest not installed but the layout is python: unittest discovery is in
        // the standard library, so it is there whenever python is.
        if (in_path("python3")) {
            return TestCommand{{"python3", "-m", "unittest", "discover", "-q"},
                               "python -m unittest"};
        }
    }

    if (has(names, "Cargo.toml") && in_path("cargo")) {
        return TestCommand{{"cargo", "test", "--quiet"}, "cargo test"};
    }

    if (has(names, "go.mod") && in_path("go")) {
        return TestCommand{{"go", "test", "./..."}, "go test"};
    }

    if (has(names, "package.json") && in_path("npm")) {
        // `npm test` and nothing cleverer: reading package.json to find out which
        // runner it wraps would be guessing at a script the project has already
        // written down.
        return TestCommand{{"npm", "test", "--silent"}, "npm test"};
    }

    if (has(names, "Gemfile") && in_path("bundle")) {
        return TestCommand{{"bundle", "exec", "rspec"}, "rspec"};
    }

    if (has(names, "CMakeLists.txt") && in_path("ctest")) {
        // Only with a build directory. ctest in a tree that has never been
        // configured fails for a reason that has nothing to do with the code.
        if (has_under(names, "build/")) {
            return TestCommand{{"ctest", "--test-dir", "build", "--output-on-failure"},
                               "ctest"};
        }
    }

    if (has(names, "Makefile") && in_path("make")) {
        return TestCommand{{"make", "test"}, "make test"};
    }

    // Nothing recognisable. Deliberately not a guess: running the wrong suite
    // fails in a way that looks like the coder's fault.
    return std::nullopt;
}

std::optional<TestCommand> detect_tests(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return std::nullopt;

    // Names only -- list_file_names() reads no contents, and detection needs only
    // the layout. On a large project reading every file to decide how to run the
    // tests would cost more than the tests.
    auto command = detect_tests_from(list_file_names(root));
    if (!command) return std::nullopt;

    // One list of things this program will execute. Not a security boundary here
    // -- this argv comes from the table above, never from a model -- but keeping
    // it true means there is exactly one answer to "what can Auspex run".
    if (!is_runnable(command->argv.front())) return std::nullopt;
    return command;
}

TestRun run_tests(const TestCommand& command, const std::filesystem::path& root,
                  int timeout_seconds, const std::atomic<bool>* cancel) {
    TestRun run;
    run.command = command;
    if (command.argv.empty()) return run;
    if (cancel && cancel->load()) return run;

    const LimitedResult result =
        run_limited(command.argv, root.string(), timeout_seconds,
                    /*max_output=*/40'000);

    run.exit_code = result.exit_code;
    run.timed_out = result.timed_out;
    run.output = result.output;
    return run;
}

std::string failure_digest(const std::string& output, std::size_t budget) {
    const std::string text = trim(output);
    if (text.size() <= budget) return text;

    // Head and tail, with the middle stated as dropped. A failing suite announces
    // the failure near the top and counts it at the bottom; the middle is other
    // tests passing, which is not what anybody is reading this for.
    const std::size_t half = budget / 2;
    const std::size_t dropped = text.size() - budget;

    std::ostringstream out;
    out << text.substr(0, half) << "\n\n... (" << dropped
        << " bytes of output not shown) ...\n\n"
        << text.substr(text.size() - half);
    return out.str();
}

std::string retry_note(const TestRun& run, int attempt, int max_attempts) {
    std::ostringstream out;

    if (run.timed_out) {
        out << "The test suite (" << run.command.label
            << ") did not finish in time. Something you changed is probably "
               "hanging -- an infinite loop, or something waiting on input.\n";
    } else {
        out << "The test suite (" << run.command.label << ") FAILED after your "
            << "changes, exit " << run.exit_code << ".\n";
    }

    out << "\n" << failure_digest(run.output) << "\n\n";

    out << "Fix the cause. This is attempt " << attempt << " of " << max_attempts
        << ".\n"
        // Named because it is the shortcut a model reaches for, and it makes the
        // suite green while making the project worse.
        << "Do NOT change or delete the tests to make them pass unless the piece "
           "you were given was about the tests themselves. Fix the code they are "
           "testing.\n";
    return out.str();
}



bool looks_like_tests(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const auto slash = lower.find_last_of('/');
    const std::string base = slash == std::string::npos ? lower : lower.substr(slash + 1);

    // A directory called tests, or a filename that says so in any of the spellings
    // the common runners use.
    if (lower.find("test/") != std::string::npos ||
        lower.find("tests/") != std::string::npos ||
        lower.find("spec/") != std::string::npos ||
        lower.find("__tests__/") != std::string::npos) {
        return true;
    }
    return base.rfind("test_", 0) == 0 || base.rfind("test-", 0) == 0 ||
           base.find("_test.") != std::string::npos ||
           base.find(".test.") != std::string::npos ||
           base.find("_spec.") != std::string::npos ||
           base.find(".spec.") != std::string::npos ||
           base.rfind("test", 0) == 0;
}

std::vector<std::string> weakened_tests(const Changeset& changeset) {
    std::vector<std::string> weakened;

    // Walk the unified diff rather than the file contents: the diff already says
    // which lines went and which arrived, which is the whole question.
    std::string current;
    for (const auto& line : split_lines(changeset.diff)) {
        if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0 ||
            line.rfind("diff --git", 0) == 0) {
            // "+++ b/path" names the file the hunks belong to -- EXCEPT when the
            // file was deleted, where it is /dev/null and the name is only on the
            // "--- a/path" side. Deleting the tests is the most complete way to
            // weaken them, so losing that case would miss the worst one.
            if (line.rfind("+++ ", 0) == 0 || line.rfind("--- ", 0) == 0) {
                std::string named = trim(line.substr(4));
                if (named == "/dev/null") continue;   // keep whichever side had a name
                if (named.rfind("a/", 0) == 0 || named.rfind("b/", 0) == 0) {
                    named = named.substr(2);
                }
                current = named;
            }
            continue;
        }
        if (line.rfind("@@", 0) == 0) continue;

        // A removed line, and not the /dev/null marker of a whole-file add.
        if (line.rfind("-", 0) == 0 && line != "---") {
            if (current.empty() || !looks_like_tests(current)) continue;
            if (std::find(weakened.begin(), weakened.end(), current) == weakened.end()) {
                weakened.push_back(current);
            }
        }
    }
    return weakened;
}

std::string no_cheating_note() {
    return "The project's tests will be RUN after you finish, and your piece is "
           "not done until they pass.\n"
           "Do NOT edit or delete an existing test to make them pass. Changing an "
           "assertion so it agrees with your code is not a fix, and it is checked "
           "for -- a change that removes lines from a test file is held for a "
           "person whatever the suite says. Adding new tests is fine.\n";
}

}  // namespace auspex
