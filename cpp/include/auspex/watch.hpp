// Do it again when something changes.
//
// A crew run is a thing you ask for. This is the other shape: a task that stands,
// and runs itself whenever the tree moves -- keep the tests green, keep the docs
// matching the code, fix the lint. It only makes sense because the compute can be
// local; a metered model billed on every save would be a standing order to spend
// money on nothing.
//
// DEBOUNCED, HARD. An editor writing a file produces several events, a formatter
// produces more, and `git checkout` produces hundreds. Reacting to each would
// start a crew per keystroke, so a change starts a clock rather than a run, and
// the clock restarts every time something else moves. Nothing runs until the tree
// has been still.
//
// AND NEVER TWO AT ONCE. A run takes minutes and the tree keeps changing while it
// goes -- including because the crew itself is writing to it. Without a guard
// that is a fork bomb with a language model in it.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "auspex/config.hpp"

namespace auspex {

// A snapshot of what the tree looked like: path -> (size, mtime).
//
// Sizes and times rather than contents. Reading every file to notice a change
// would cost more than the change, and this runs on a timer.
using TreeSnapshot = std::map<std::string, std::pair<std::uint64_t, std::int64_t>>;

// Snapshot `root`, honouring sandbox_excludes() -- a build directory changing is
// not the project changing, and on a compiled project it changes constantly.
TreeSnapshot snapshot_tree(const std::filesystem::path& root);

// What moved between two snapshots: added, removed or touched, sorted.
std::vector<std::string> changed_between(const TreeSnapshot& before,
                                         const TreeSnapshot& after);

struct WatchOptions {
    std::filesystem::path project;
    std::string           task;      // what to do when something changes
    // How still the tree must be before a run starts. Two seconds is enough to
    // cover an editor's write-and-rename and short enough to feel immediate.
    int quiet_seconds = 2;
    // How often the tree is looked at.
    int poll_seconds = 1;
    // Stop after this many runs. 0 is "keep going" -- and the reason a bound
    // exists at all is that this is the one part of Auspex that starts work
    // without being asked, so being able to say "three and then stop" matters.
    int max_runs = 0;
};

struct WatchEvents {
    // Something moved; a run is about to be considered.
    std::function<void(const std::vector<std::string>& changed)> on_change;
    // A run is starting, and which number it is.
    std::function<void(int run_number)> on_run;
    // A run finished: applied, held.
    std::function<void(int applied, int held)> on_done;
    std::function<void(const std::string& line)> log;
};

// Watch until `stop` is set, or `max_runs` runs have happened.
//
// Blocking; run it on your own thread. Returns how many runs it started.
//
// Changes made BY the run are not a reason to run again -- the snapshot is retaken
// after each one, so a crew that edits three files does not immediately trigger
// itself. That is the difference between a standing task and a loop.
int watch_project(const Config& config, const WatchOptions& options,
                  const WatchEvents& events = {},
                  const std::atomic<bool>* stop = nullptr);

}  // namespace auspex
