#include "auspex/watch.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "auspex/crew_run.hpp"
#include "auspex/process.hpp"
#include "auspex/projects.hpp"
#include "auspex/sandbox.hpp"

namespace auspex {

TreeSnapshot snapshot_tree(const std::filesystem::path& root) {
    TreeSnapshot snapshot;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return snapshot;

    auto it = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return snapshot;

    for (auto end = std::filesystem::recursive_directory_iterator(); it != end;
         it.increment(ec)) {
        if (ec) break;

        const auto& entry = *it;
        const std::string name = entry.path().filename().string();

        // The same excludes the sandbox uses. A build directory changing is not
        // the project changing, and on a compiled project it changes constantly --
        // watching it would mean a run every time you built.
        if (entry.is_directory(ec)) {
            if (is_excluded(name)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (is_excluded(name)) continue;

        const auto size = entry.file_size(ec);
        if (ec) continue;
        const auto written = entry.last_write_time(ec);
        if (ec) continue;

        snapshot[std::filesystem::relative(entry.path(), root, ec).generic_string()] =
            {static_cast<std::uint64_t>(size),
             static_cast<std::int64_t>(written.time_since_epoch().count())};
    }
    return snapshot;
}

std::vector<std::string> changed_between(const TreeSnapshot& before,
                                         const TreeSnapshot& after) {
    std::vector<std::string> changed;

    for (const auto& [path, stat] : after) {
        const auto was = before.find(path);
        if (was == before.end() || was->second != stat) changed.push_back(path);
    }
    for (const auto& [path, stat] : before) {
        (void)stat;
        if (after.find(path) == after.end()) changed.push_back(path);
    }

    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    return changed;
}

int watch_project(const Config& config, const WatchOptions& options,
                  const WatchEvents& events, const std::atomic<bool>* stop) {
    const auto say = [&events](const std::string& line) {
        if (events.log) events.log(line);
    };

    if (const std::string why = unsafe_project_reason(options.project); !why.empty()) {
        say("watch: " + why);
        return 0;
    }
    if (trim(options.task).empty()) {
        say("watch: there is no task to run");
        return 0;
    }

    const int poll = std::max(1, options.poll_seconds);
    const int quiet = std::max(1, options.quiet_seconds);

    TreeSnapshot seen = snapshot_tree(options.project);
    int runs = 0;
    int still_for = 0;   // consecutive polls with nothing moving
    bool pending = false;

    say("watch: watching " + options.project.string() + " (" +
        std::to_string(seen.size()) + " files)");

    while (!stop || !stop->load()) {
        std::this_thread::sleep_for(std::chrono::seconds(poll));
        if (stop && stop->load()) break;

        const TreeSnapshot now = snapshot_tree(options.project);
        const auto changed = changed_between(seen, now);

        if (!changed.empty()) {
            seen = now;
            // A change RESTARTS the clock rather than starting a run. An editor's
            // write-and-rename is several events and `git checkout` is hundreds;
            // reacting to each would be a crew per keystroke.
            still_for = 0;
            if (!pending) {
                pending = true;
                if (events.on_change) events.on_change(changed);
                say("watch: " + std::to_string(changed.size()) +
                    " file(s) changed, waiting for the tree to settle");
            }
            continue;
        }

        if (!pending) continue;
        if (++still_for * poll < quiet) continue;

        // Settled. Run.
        pending = false;
        still_for = 0;
        ++runs;
        if (events.on_run) events.on_run(runs);

        RunOptions run;
        run.project = options.project;
        run.task = options.task;

        RunEvents forwarded;
        forwarded.log = events.log;
        const RunResult result = run_crew(config, run, forwarded, stop);

        if (events.on_done) events.on_done(result.applied, result.held);
        say("watch: run " + std::to_string(runs) + " -- " +
            std::to_string(result.applied) + " applied, " +
            std::to_string(result.held) + " held");

        // Retaken AFTER the run, so what the crew itself wrote is not a reason to
        // run again. Without this a crew that edits three files immediately
        // retriggers itself, which is a loop with a language model in it.
        seen = snapshot_tree(options.project);

        if (options.max_runs > 0 && runs >= options.max_runs) {
            say("watch: stopping after " + std::to_string(runs) + " run(s)");
            break;
        }
    }
    return runs;
}

}  // namespace auspex
