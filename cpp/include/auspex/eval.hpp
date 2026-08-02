// Does the crew actually work? A number, not an impression.
//
// WHY THIS EXISTS. Everything else in this repository is tested: 1800 checks, and
// they pass. Not one of them measures the thing the crew is for. They prove that
// quote_is_real() handles a leading '+', that a tie holds, that safe_join refuses
// "..". They say nothing about whether a coder, handed a task, produces code that
// works -- and on this project that question has been answered twice by reading
// the output and being wrong about it both times. An impression of quality formed
// by looking at a diff is worth about what it cost.
//
// So: a fixed suite of small tasks with checkable answers. Run the coder on each,
// in a directory of its own, and check the result with something that cannot be
// talked round.
//
// THE VERDICT IS NEVER A MODEL'S. A check is a file's contents, or a command's
// exit status. An LLM asked to grade another LLM's work measures agreeableness,
// and the two models in this project's crew have already demonstrated they will
// confidently agree with a wrong thing. There is no "judge" here and there should
// not be one.
//
// A MISSING INTERPRETER IS NOT A FAILURE. A task whose check needs python3 on a
// box without python3 is SKIPPED and left out of the denominator. Counting it as
// a failure would make the pass rate a measurement of the machine rather than of
// the model, and the number would drop for a reason nobody could act on.
//
// EVERY TASK RUNS IN ITS OWN TEMP DIRECTORY, never in the user's project. This is
// the one part of the crew that deliberately runs unreviewed model output, so it
// runs it nowhere near anything real.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "auspex/config.hpp"

namespace auspex {

// How a task is marked. Exactly one of these is set.
struct EvalCheck {
    // The file that must exist afterwards, relative to the task directory.
    std::string file;
    // ... and contain this text. Whitespace-insensitive at the ends of lines, so a
    // trailing space does not fail an otherwise correct answer.
    std::string contains;
    // ... or NOT contain this. For "did it stop doing the thing we told it not to".
    std::string absent;

    // Or: run this and require exit 0. argv, never a command line -- the same rule
    // as everywhere else. The program must be installed or the task SKIPS.
    std::vector<std::string> command;
    // When set, the command's output must also contain this.
    std::string output_contains;

    bool operator==(const EvalCheck&) const = default;
};

struct EvalTask {
    std::string name;
    std::string prompt;   // what the coder is asked to do
    // Files the directory is seeded with before the coder starts.
    std::map<std::string, std::string> files;
    std::vector<EvalCheck> checks;   // ALL must pass

    bool operator==(const EvalTask&) const = default;
};

struct EvalResult {
    std::string task;
    std::string model;
    bool        passed = false;
    // Excluded from the denominator. See the header: a task we could not check is
    // not a task the model failed.
    bool        skipped = false;
    std::string detail;      // why it failed, or what was skipped
    int         milliseconds = 0;
    int         steps = 0;   // how many tool calls the coder used
    int         writes = 0;

    bool operator==(const EvalResult&) const = default;
};

// The built-in suite.
//
// Small on purpose. Each task is answerable in two or three tool calls by a model
// that understands it, so a failure means the model did not understand rather
// than that it ran out of budget. They also cover the specific things this crew
// has been observed getting wrong: creating a new file instead of editing the one
// named, and ignoring an instruction about style.
const std::vector<EvalTask>& builtin_evals();

// User-authored tasks from `<project>/.auspex/evals/*.json`. Absent directory
// yields none.
//
// Unlike a hook, a task here is never executed as a program -- it is a prompt and
// a set of expectations -- so reading these from the project is not the risk that
// reading hooks from the project would be. The only command an eval task can name
// is its check command, which is why load_evals() refuses one that is not on the
// runnable allowlist.
std::vector<EvalTask> load_evals(const std::filesystem::path& directory);
std::vector<EvalTask> parse_evals(const std::string& json_text);

// builtin + user, optionally filtered to one name.
std::vector<EvalTask> eval_suite(const std::filesystem::path& project,
                                 const std::string& only = {});

// --- checking -----------------------------------------------------------------

// Whether this check can be evaluated on this machine at all. False means the
// command it names is not installed, and the task will be skipped rather than
// failed.
bool check_runnable(const EvalCheck& check);

// Apply one check to a finished directory. `detail` says why on a failure.
bool apply_check(const EvalCheck& check, const std::filesystem::path& directory,
                 std::string* detail);

// --- running ------------------------------------------------------------------

struct EvalOptions {
    std::string model;     // empty falls back to the config's
    std::string backend;   // "ollama" (or empty) is Auspex's own loop; else a CLI
    int         max_steps = 16;
    int         timeout_seconds = 300;
    // Leave a failed task's directory behind for inspection. The passing ones are
    // always removed -- there is nothing to look at.
    bool        keep_failures = false;
};

// One task, start to finish, in its own temp directory. Blocking.
EvalResult run_eval(const Config& config, const EvalTask& task,
                    const EvalOptions& options = {});

// The suite. `on_result` is called as each finishes, so a caller can print
// progress rather than waiting for the whole run.
std::vector<EvalResult> run_evals(const Config& config,
                                  const std::vector<EvalTask>& tasks,
                                  const EvalOptions& options = {},
                                  const std::function<void(const EvalResult&)>& on_result = {});

// --- reporting ----------------------------------------------------------------

struct EvalSummary {
    int passed = 0;
    int failed = 0;
    int skipped = 0;

    // Skipped tasks are NOT in the denominator. `scored()` is what the rate is
    // over, and it can be zero -- on a machine with no interpreters at all, the
    // honest pass rate is "no tasks could be scored", not 0% and not 100%.
    int scored() const { return passed + failed; }
    double rate() const {
        return scored() == 0 ? 0.0 : (100.0 * passed) / static_cast<double>(scored());
    }
};

EvalSummary summarize_evals(const std::vector<EvalResult>& results);
std::string render_evals(const std::vector<EvalResult>& results);

}  // namespace auspex
