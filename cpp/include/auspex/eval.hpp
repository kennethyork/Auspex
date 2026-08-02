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

#include "auspex/auditor.hpp"
#include "auspex/config.hpp"
#include "auspex/director.hpp"
#include "auspex/sandbox.hpp"

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

// The held-out pair. Same behaviour as a task in the suite above, in a different
// shape -- see the note in eval.cpp. These exist to tell a general fix apart from
// a prompt fitted to one test, so they are NOT part of the default suite: adding
// them to it would make them the thing being fitted to next.
const std::vector<EvalTask>& holdout_evals();

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

// Per-task pass rates across repeated runs of the same suite.
//
// A SINGLE RUN IS AN ANECDOTE. A local model at a non-zero temperature does not
// give the same answer twice, so one 8/8 and one 6/8 can be the same build --
// and a change that looks like a regression may be noise, while a real regression
// can hide inside it. That distinction is the whole reason this exists: without
// it there is no honest way to say a change made the crew better.
//
// Reports each task's pass count out of N, so a task that is right half the time
// is visibly different from one that is always right. An average over everything
// would hide exactly that.
struct EvalTrend {
    std::string task;
    int runs = 0;
    int passed = 0;
    int skipped = 0;
    // Milliseconds, averaged over the runs that were scored.
    int mean_ms = 0;

    bool always() const { return runs > 0 && passed == runs; }
    bool never() const { return runs > 0 && passed == 0 && skipped < runs; }
};

std::vector<EvalTrend> eval_trends(const std::vector<std::vector<EvalResult>>& runs);
std::string render_eval_trends(const std::vector<std::vector<EvalResult>>& runs);

// --- measuring the Auditor ----------------------------------------------------
//
// The suite above measures the coder. This measures the reviewer, which on this
// project is the part that has been wrong most often and the part I have been
// wrong ABOUT most often -- I called the Auditor "working well" once while it was
// holding correct Python with a self-contradicting reason.
//
// THE GROUND TRUTH IS AUTHORED, NOT ASKED FOR. Each case is a changeset where the
// right verdict is beyond argument: the code does what the subtask asked, or it
// demonstrably does not. If a case is arguable it does not belong here, because
// then a disagreement measures the case rather than the Auditor.
//
// THE TWO ERRORS ARE NOT THE SAME ERROR, so they are never averaged into one
// number. A false HOLD wastes a coder's work and trains you to click through
// holds. A false ACCEPT lands broken code in your project. An Auditor that holds
// everything scores 50% and is useless; one that accepts everything scores 50%
// and is dangerous. Only the split tells them apart.

struct AuditCase {
    std::string    name;
    PlannedSubtask subtask;
    Changeset      changeset;
    Verdict        expected = Verdict::Hold;
    // Why that is the right answer. Printed when a case fails, so the person
    // reading knows whether to doubt the Auditor or the case.
    std::string    rationale;
};

// The corpus. Some cases are decidable without a model -- a secret, a syntax
// error, an empty changeset -- and those are kept deliberately: they are the
// checks most likely to regress silently, and they cost nothing to run.
const std::vector<AuditCase>& builtin_audit_cases();

struct AuditEvalResult {
    std::string name;
    Verdict     expected = Verdict::Hold;
    Verdict     got      = Verdict::Hold;
    bool        correct  = false;
    // True when the verdict came from a deterministic check rather than a model.
    bool        certain  = false;
    std::string reason;      // what the Auditor said
    std::string rationale;   // what the case says is true
    // The Auditor quoted a line that is not in the diff it reviewed. Counted
    // separately: a hold on invented evidence is a different failure from a hold
    // on a real disagreement, and only one of them is fixable by a better prompt.
    bool        invented_quote = false;
    int         milliseconds = 0;
};

struct AuditEvalSummary {
    int correct = 0;
    // Held something that should have landed. Wastes work; makes holds noise.
    int false_holds = 0;
    // Landed something that should have been held. The expensive one.
    int false_accepts = 0;
    int decided_without_a_model = 0;
    int invented_quotes = 0;

    int total() const { return correct + false_holds + false_accepts; }
    double rate() const {
        return total() == 0 ? 0.0 : (100.0 * correct) / static_cast<double>(total());
    }
};

struct AuditEvalOptions {
    std::string model;     // empty falls back to the config's
    std::string backend;   // an agent CLI, or empty for Auspex's own loop
    bool        debate = false;   // three calls per case instead of one
    int         voters = 1;       // a panel, when > 1
};

AuditEvalResult run_audit_case(const Config& config, const AuditCase& item,
                               const AuditEvalOptions& options = {});

std::vector<AuditEvalResult> run_audit_cases(
    const Config& config, const std::vector<AuditCase>& cases,
    const AuditEvalOptions& options = {},
    const std::function<void(const AuditEvalResult&)>& on_result = {});

AuditEvalSummary summarize_audit(const std::vector<AuditEvalResult>& results);
std::string render_audit_eval(const std::vector<AuditEvalResult>& results);

}  // namespace auspex
