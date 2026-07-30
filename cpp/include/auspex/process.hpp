// Subprocess helper shared by the dock and desktop layers.
//
// fork/exec with an argv vector, never a shell. Window titles and workspace names
// are arbitrary user-controlled text and must not be word-split or interpreted.
#pragma once

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
ProcessResult run(const std::vector<std::string>& argv, bool capture = true);

// Splits on '\n', dropping empty lines.
std::vector<std::string> split_lines(const std::string& text);

// Fire-and-forget launch. run() waits for the child, which would freeze the GTK
// thread for as long as a launched application stays open, so anything the panel
// starts on a button press must go through this instead.
//
// Double-forks so the grandchild is reparented to init: the panel then has no
// child to reap and cannot accumulate zombies, and the launched app survives the
// panel exiting.
bool spawn_detached(const std::vector<std::string>& argv);

// True if `program` resolves to an executable in PATH. Rejects anything with a
// path separator or shell metacharacter, so it doubles as the validator for
// model-supplied application names.
bool in_path(std::string_view program);

// First entry of `candidates` that resolves in PATH, or "" if none do. Used to
// pick desktop tools at runtime instead of hardcoding one desktop's binaries.
std::string first_in_path(const std::vector<std::string>& candidates);

std::string trim(std::string s);

}  // namespace auspex
