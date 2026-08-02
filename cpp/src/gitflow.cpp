#include "auspex/gitflow.hpp"

#include <algorithm>
#include <sstream>

#include "auspex/process.hpp"
#include "auspex/sandbox.hpp"

namespace auspex {

bool is_git_repo(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return false;
    // Asking git rather than looking for a .git directory: a worktree's .git is a
    // file, and a subdirectory of a repository has no .git of its own but is
    // still in one.
    const auto result = run({"git", "rev-parse", "--is-inside-work-tree"},
                            /*capture=*/true, root.string());
    return result.ok && trim(result.out) == "true";
}

std::string git_branch(const std::filesystem::path& root) {
    const auto result =
        run({"git", "rev-parse", "--abbrev-ref", "HEAD"}, true, root.string());
    if (!result.ok) return {};
    const std::string branch = trim(result.out);
    return branch == "HEAD" ? std::string{} : branch;   // detached
}

std::vector<std::string> git_dirty_paths(const std::filesystem::path& root) {
    std::vector<std::string> paths;

    // --porcelain is the stable, machine-readable form; the human output is
    // explicitly not promised to stay the same between versions.
    const auto result =
        run({"git", "status", "--porcelain", "--untracked-files=all"}, true,
            root.string());
    if (!result.ok) return paths;

    for (const auto& line : split_lines(result.out)) {
        // "XY path", where XY is two status characters and one space.
        if (line.size() < 4) continue;
        std::string path = line.substr(3);

        // A rename is "old -> new"; the new name is the one that exists.
        if (const auto arrow = path.find(" -> "); arrow != std::string::npos) {
            path = path.substr(arrow + 4);
        }
        // git quotes paths containing unusual characters. Left as-is rather than
        // half-unquoted: a wrong path here would stage the wrong file.
        if (!path.empty() && path.front() == '"') continue;
        paths.push_back(std::move(path));
    }
    return paths;
}

std::string commit_message(const std::string& task, const std::string& run_id,
                           const std::vector<std::string>& paths) {
    std::ostringstream out;

    // A subject somebody can read in a log. The task is what was asked for, which
    // is more useful than any summary of the diff.
    std::string subject = trim(task);
    if (subject.empty()) subject = "crew changes";
    // Kept to a readable width the way git wants, and cut on a word.
    constexpr std::size_t kSubject = 68;
    if (subject.size() > kSubject) {
        const auto space = subject.rfind(' ', kSubject);
        subject = subject.substr(0, space == std::string::npos ? kSubject : space) + "…";
    }
    out << subject << "\n\n";

    out << "Landed by an Auspex crew";
    if (!run_id.empty()) out << " (" << run_id << ")";
    out << ".\n";
    // The run id is what connects a line of code back to the board entry that
    // explains why it is there -- in six months that is the only thread back.

    if (!paths.empty()) {
        out << "\nFiles this run changed:\n";
        for (const auto& path : paths) out << "  " << path << "\n";
    }
    return out.str();
}

CommitResult commit_paths(const std::filesystem::path& root,
                          const std::vector<std::string>& paths,
                          const std::string& message) {
    CommitResult result;

    if (!is_git_repo(root)) {
        result.error = "that project is not a git repository";
        return result;
    }
    if (paths.empty()) {
        result.error = "there is nothing to commit";
        return result;
    }
    if (trim(message).empty()) {
        result.error = "a commit needs a message";
        return result;
    }

    // Every path checked BEFORE any is staged. These came from a changeset, which
    // came ultimately from a model's choice of filename.
    std::vector<std::string> safe;
    for (const auto& path : paths) {
        if (!safe_join(root, path)) {
            result.error = "refusing to commit a path outside the repository: " + path;
            return result;
        }
        std::error_code ec;
        // A file the run deleted is a legitimate thing to stage, so existence is
        // not required -- but a path that never existed at all is a mistake.
        if (std::filesystem::exists(root / path, ec) ||
            std::find(safe.begin(), safe.end(), path) == safe.end()) {
            safe.push_back(path);
        }
    }
    if (safe.empty()) {
        result.error = "none of those paths are in the repository";
        return result;
    }

    // NEVER `git add -A`. Only what this run landed, so work you had in progress
    // beside the crew's is not swept into its commit -- the failure this is most
    // likely to cause and the one it exists to avoid.
    std::vector<std::string> add{"git", "add", "--"};
    add.insert(add.end(), safe.begin(), safe.end());
    if (const auto staged = run(add, true, root.string()); !staged.ok) {
        result.error = "could not stage the files";
        return result;
    }
    result.staged = static_cast<int>(safe.size());

    // Nothing actually staged means the files were already committed. Not an
    // error, but not a commit either -- and `git commit` would fail confusingly.
    if (const auto diff = run({"git", "diff", "--cached", "--name-only"}, true,
                              root.string());
        trim(diff.out).empty()) {
        result.error = "those files were already committed";
        return result;
    }

    // --only WITH the paths, which is the whole point of it: it commits exactly
    // these files and ignores anything else that happened to be staged, so a
    // crew commit cannot pick up something you had queued yourself.
    //
    // `--only` with no paths after `--` is refused by git outright ("No paths
    // with --include/--only does not make sense") -- caught by the test, not by
    // reading the manual.
    //
    // The message is an argument, never a shell string. It contains the task,
    // which is arbitrary text somebody typed.
    std::vector<std::string> commit{"git", "commit", "--only", "--message", message,
                                    "--"};
    commit.insert(commit.end(), safe.begin(), safe.end());
    const auto committed = run(commit, true, root.string());
    if (!committed.ok) {
        result.error = "git refused the commit";
        return result;
    }

    if (const auto hash = run({"git", "rev-parse", "--short", "HEAD"}, true,
                              root.string());
        hash.ok) {
        result.commit = trim(hash.out);
    }
    result.ok = true;
    return result;
}

}  // namespace auspex
