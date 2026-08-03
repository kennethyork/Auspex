#include "auspex/gitflow.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <unistd.h>

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



std::string branch_name(const std::string& run_id, int n, const std::string& title) {
    // Sanitised to what git will actually accept. A title is a sentence a model
    // wrote, and a ref refuses spaces, "..", "~", "^", ":", "?", "*", "[", "\\",
    // a leading or trailing "/", a trailing "." and a trailing ".lock". A name git
    // rejects is a run that fails at its very last step.
    std::string slug;
    for (const char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(c)));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
        if (slug.size() >= 40) break;
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();

    std::string run = run_id.empty() ? "run" : run_id;
    for (char& c : run) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '-';
    }

    std::string name = "crew/" + run + "/" + std::to_string(n);
    if (!slug.empty()) name += "-" + slug;
    return name;
}

CommitResult commit_to_branch(const std::filesystem::path& root,
                              const std::string& branch, const Changeset& changeset,
                              const std::string& message) {
    CommitResult result;

    if (!is_git_repo(root)) {
        result.error = "that project is not a git repository";
        return result;
    }
    if (changeset.empty()) {
        result.error = "there is nothing to commit";
        return result;
    }
    if (trim(branch).empty() || trim(message).empty()) {
        result.error = "a branch needs a name and a message";
        return result;
    }

    // A repository with no commits has no HEAD to cut a branch from. Said plainly
    // rather than letting git say it: "ambiguous argument HEAD" is not an error
    // message about this program.
    if (!run({"git", "rev-parse", "--verify", "HEAD"}, true, root.string()).ok) {
        result.error = "this repository has no commits yet to branch from";
        return result;
    }
    if (run({"git", "rev-parse", "--verify", "--quiet", "refs/heads/" + branch}, true,
            root.string())
            .ok) {
        result.error = "the branch " + branch + " already exists";
        return result;
    }

    // Every path checked BEFORE anything is created. These came from a changeset,
    // which came ultimately from a model's choice of filename.
    for (const auto& file : changeset.files) {
        if (!safe_join(root, file.path)) {
            result.error = "refusing to commit a path outside the repository: " +
                           file.path;
            return result;
        }
    }

    // A THROWAWAY WORKTREE, somewhere else entirely.
    //
    // This is what leaves your working tree, your index and your current branch
    // exactly where you left them -- and what lets several coders land at once,
    // because each gets its own. Doing it in the real tree would mean stashing
    // your work, and a tool that stashes your work is a tool you cannot trust.
    std::error_code ec;
    const auto shed = std::filesystem::temp_directory_path(ec) /
                      ("auspex-land-" + std::to_string(::getpid()) + "-" + branch.substr(branch.rfind('/') + 1));
    std::filesystem::remove_all(shed, ec);

    if (!run({"git", "worktree", "add", "--detach", "--quiet", shed.string(), "HEAD"},
             true, root.string())
             .ok) {
        result.error = "could not make a place to build the branch";
        return result;
    }

    // Whatever happens below, the worktree goes. A shed left behind is a stale
    // entry in `git worktree list` that the user has to prune by hand.
    const auto tidy = [&root, &shed] {
        std::error_code inner;
        run({"git", "worktree", "remove", "--force", shed.string()}, true,
            root.string());
        std::filesystem::remove_all(shed, inner);
        run({"git", "worktree", "prune"}, true, root.string());
    };

    if (!run({"git", "switch", "--quiet", "-c", branch}, true, shed.string()).ok) {
        tidy();
        result.error = "git would not take the branch name " + branch;
        return result;
    }

    std::vector<std::string> paths;
    for (const auto& file : changeset.files) {
        const auto target = safe_join(shed, file.path);
        if (!target) continue;

        if (file.deleted) {
            std::filesystem::remove(*target, ec);
        } else {
            std::filesystem::create_directories(target->parent_path(), ec);
            std::ofstream out(*target, std::ios::binary | std::ios::trunc);
            if (!out) {
                tidy();
                result.error = "could not write " + file.path;
                return result;
            }
            out << file.contents;
        }
        paths.push_back(file.path);
    }

    // --all so a deletion is staged as one. Scoped to these paths, so nothing
    // else in the checkout can be swept in.
    std::vector<std::string> add{"git", "add", "--all", "--"};
    add.insert(add.end(), paths.begin(), paths.end());
    if (!run(add, true, shed.string()).ok) {
        tidy();
        result.error = "could not stage the change";
        return result;
    }
    result.staged = static_cast<int>(paths.size());

    if (!run({"git", "commit", "--quiet", "--message", message}, true, shed.string())
             .ok) {
        tidy();
        result.error = "git refused the commit";
        return result;
    }

    if (const auto hash = run({"git", "rev-parse", "--short", "HEAD"}, true,
                              shed.string());
        hash.ok) {
        result.commit = trim(hash.out);
    }

    // The worktree goes; the BRANCH stays. That is the whole point.
    tidy();
    result.ok = true;
    return result;
}

}  // namespace auspex
