// Subprocess helper shared by the dock and desktop layers.
//
// fork/exec with an argv vector, never a shell. Window titles and workspace names
// are arbitrary user-controlled text and must not be word-split or interpreted.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace auspex {

struct ProcessResult {
    bool        ok = false;   // exited 0
    std::string out;          // stdout, empty unless capture was requested
};

// Runs argv[0] via execvp. stderr is always discarded: a missing window or an
// absent helper is an expected, recoverable state, not something to spray across
// the shell's log every second.
//
// `cwd` is the directory the child runs in; empty inherits this process's. It is
// applied in the CHILD, after the fork and before the exec, so the panel's own
// working directory is never disturbed -- chdir is process-wide, and a shell that
// moved itself to run one command would break every relative path it holds.
//
// A cwd that cannot be entered fails the command rather than running it somewhere
// else. For a tool whose whole job is editing the files in front of it, "the
// directory was wrong so it worked here instead" is the one outcome worth
// crashing to avoid.
ProcessResult run(const std::vector<std::string>& argv, bool capture = true,
                  const std::string& cwd = {});

// Runs argv with a deadline and an output cap, capturing stdout AND stderr.
//
// The plain run() above is for helpers that answer in milliseconds; this is for
// running somebody else's test suite, where all three of "it hangs", "it prints a
// gigabyte" and "it fails" are ordinary outcomes rather than surprises.
//
// A child that outlives `timeout` is sent SIGKILL and its process GROUP with it --
// a test runner that forks workers would otherwise leave them behind holding the
// pipe open, and the read would block long after the child it was waiting for had
// died.
//
// Output past `max_output` is discarded, not buffered: the cap exists to bound
// memory, so growing to the full size first would defeat it.
struct LimitedResult {
    bool        ok = false;        // exited 0, within the deadline
    int         exit_code = -1;
    bool        timed_out = false;
    bool        truncated = false;
    std::string output;            // stdout and stderr interleaved, as a terminal
                                   // would show them
};

// `stdin_text` is fed to the child and the pipe then closed. Empty means stdin is
// /dev/null, which is what stops a command that stops to ask a question from
// waiting out the whole timeout for an answer that cannot come.
LimitedResult run_limited(const std::vector<std::string>& argv,
                          const std::string& cwd,
                          int timeout_seconds = 60,
                          std::size_t max_output = 16'000,
                          const std::string& stdin_text = {});

// Splits on '\n', dropping empty lines.
std::vector<std::string> split_lines(const std::string& text);

// Fire-and-forget launch. run() waits for the child, which would freeze the GTK
// thread for as long as a launched application stays open, so anything the panel
// starts on a button press must go through this instead.
//
// Double-forks so the grandchild is reparented to init: the panel then has no
// child to reap and cannot accumulate zombies, and the launched app survives the
// panel exiting.
//
// `cwd` as in run(). Note what this returns: the double fork means the parent has
// already reaped the intermediate child by the time the grandchild reaches chdir,
// so `true` means "the fork succeeded", NOT "the program started in that
// directory". Anything that must be sure the directory was usable has to check it
// before calling -- see is_project_dir() in projects.hpp.
bool spawn_detached(const std::vector<std::string>& argv, const std::string& cwd = {});

// True if `program` resolves to an executable in PATH. Rejects anything with a
// path separator or shell metacharacter, so it doubles as the validator for
// model-supplied application names.
bool in_path(std::string_view program);

// The absolute path `program` resolves to in PATH, or "" if it does not.
//
// Same rejection rules as in_path() -- this is the same lookup, returning WHERE
// rather than WHETHER. It exists because "is it installed" and "run it" can be
// answered by two different processes with two different environments: a terminal
// that spawns its window from a long-lived server process executes the command
// line in THAT process's PATH, not in the panel's. Passing an absolute path takes
// the second environment out of the question entirely.
std::string resolve_in_path(std::string_view program);

// First entry of `candidates` that resolves in PATH, or "" if none do. Used to
// pick desktop tools at runtime instead of hardcoding one desktop's binaries.
std::string first_in_path(const std::vector<std::string>& candidates);

std::string trim(std::string s);

}  // namespace auspex
