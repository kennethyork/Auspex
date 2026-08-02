// Does it still work?
//
// The crew could write code and review code and never once run it. The Auditor
// reads a diff and forms an opinion; the parsers say whether it parses. Neither
// answers the question the tests already answer, in a project that has them.
//
// So: find how this project runs its tests, run them in the SANDBOX after a coder
// finishes, and if they are red, hand the failures back and let the coder try
// again. That is the difference between "makes edits" and "makes edits that pass".
//
// OFF UNLESS ASKED FOR, and this is not caution for its own sake. Running a
// project's test suite executes code the coder just wrote -- it is exactly the
// risk CoderLimits::allow_run exists to gate, and the same decision belongs to
// the person whose machine it is. The `tested` pack turns both on together,
// because a test-first crew that cannot run tests is proofreading.
//
// GUESSING IS REFUSED. detect_tests() returns nothing when it cannot tell, and
// nothing means the verify step is skipped rather than a wrong command being run.
// A project whose suite is `make check` and which gets `pytest` run at it does not
// fail informatively -- it fails in a way that looks like the coder's fault.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "auspex/sandbox.hpp"

namespace auspex {

struct TestCommand {
    // argv, never a command line. The same rule as everywhere else here: a
    // model never names this, but a config file can, and word-splitting a
    // config string is how a path with a space becomes two arguments.
    std::vector<std::string> argv;
    // What we matched, so a person can see what was guessed and correct it.
    std::string label;

    bool operator==(const TestCommand&) const = default;
};

struct TestRun {
    TestCommand command;
    int         exit_code = -1;
    bool        timed_out = false;
    std::string output;      // stdout and stderr, interleaved as a terminal shows them

    bool green() const { return exit_code == 0 && !timed_out; }

    bool operator==(const TestRun&) const = default;
};

// How this project runs its tests, or nothing when we cannot tell.
//
// Sniffed from what is on disk: a pytest layout, a package.json with a test
// script, a Cargo.toml, a CMake build directory with tests, a Makefile with a
// test target. The FIRST match wins and the order is deliberate -- a Python
// project with a Makefile is usually still a pytest project.
//
// Only ever returns a program on the coder's allowlist. That is not a security
// boundary here (this argv comes from our own table, not a model) but it keeps
// one list of things this program is willing to execute.
std::optional<TestCommand> detect_tests(const std::filesystem::path& root);

// The detection rules, exposed so they can be tested against a directory layout
// without running anything.
std::optional<TestCommand> detect_tests_from(
    const std::vector<std::string>& file_names);

// Run the suite in `root`.
//
// Cancellation and the deadline both kill OUR child and its process group -- a
// test runner that forks workers would otherwise leave them holding the pipe.
// Never a kill by name, which would take out an unrelated pytest the user is
// running in another window.
TestRun run_tests(const TestCommand& command, const std::filesystem::path& root,
                  int timeout_seconds = 300,
                  const std::atomic<bool>* cancel = nullptr);

// The part of a red run worth showing a model.
//
// A failing suite prints thousands of lines, most of them setup and summary. This
// keeps the head and the tail and drops the middle, because the failure is
// usually announced at the top and counted at the bottom, and a model given the
// whole thing spends its context on the parts that say nothing.
std::string failure_digest(const std::string& output, std::size_t budget = 3'000);

// What the coder is told when its work leaves the suite red.
std::string retry_note(const TestRun& run, int attempt, int max_attempts);

// Did the coder make the suite green by weakening the suite?
//
// The oldest shortcut there is, and watched happening on the very first run that
// could: given two tests that contradict each other, the coder edited one of them
// and verify reported green, because it was. A check that can be satisfied by
// deleting the check is not a check.
//
// The rule is REMOVED LINES, not changed files. Adding tests only adds lines;
// changing an assertion removes one. So a coder asked to add tests can add all it
// likes and trips nothing, while one that rewrites an existing assertion is
// visible -- and no reading of intent is required to tell them apart.
//
// It returns names, never a verdict. A legitimate test refactor removes lines too,
// which is exactly why this HOLDS for a person rather than rejecting: the caller
// is being told what happened, not what it means.
std::vector<std::string> weakened_tests(const Changeset& changeset);

// True when the path looks like a test file. Deliberately generous -- a false
// positive here costs a person a glance, and a false negative costs the whole
// check.
bool looks_like_tests(const std::string& path);

// The line added to a coder's prompt when the suite will be run. Empty when it
// will not be, so nothing is said about tests that are never executed.
//
// Told UP FRONT, not only on a retry. The first version of this warning lived in
// retry_note(), which meant a coder that cheated on its first attempt was never
// warned at all -- and that is precisely what happened.
std::string no_cheating_note();

}  // namespace auspex
