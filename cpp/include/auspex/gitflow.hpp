// A record of what the crew did to your repository.
//
// A run lands changes into your working tree and then it is over. What is left is
// a dirty tree, and nothing anywhere says which run made which edit -- so a
// mixture of your work and three coders' work sits in `git status` with no way to
// tell them apart an hour later.
//
// This commits what landed, with a message naming the run and the task. That is
// the whole feature: it is not a git wrapper, and it deliberately does not branch,
// merge, rebase, push or resolve anything.
//
// WHY SO LITTLE. ollamadev has a 354-line GitFlow with branches per coder and a PR
// flow. Auspex keeps git away from coders on purpose -- `git` is not on the
// runnable allowlist, and a sandbox is a tree copy rather than a worktree
// precisely so a coder cannot reach real history. Adding a rich git surface here
// would undo that from the other end. Committing what YOU already accepted is a
// different act from letting a model touch your history.
//
// OPT-IN, and it must stay opt-in. Committing is a change to your repository that
// you did not type, and a tool that quietly writes to your history is a tool you
// have to audit. Off by default; a flag turns it on for one run.
//
// NEVER `git add -A`. Only the paths the run actually landed are staged, so work
// you had in progress alongside the crew's is not swept into its commit. That is
// the failure this is most likely to cause and the one it is written to avoid.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "auspex/sandbox.hpp"

namespace auspex {

// Is this a git repository at all? False for a plain directory, which is a
// perfectly good project -- it just cannot be committed to.
bool is_git_repo(const std::filesystem::path& root);

// The current branch, or empty when there is none (a detached head, or no commits
// yet). Reported so a person can see where a commit would land BEFORE it does.
std::string git_branch(const std::filesystem::path& root);

// Paths git reports as changed, relative to the repository root.
std::vector<std::string> git_dirty_paths(const std::filesystem::path& root);

struct CommitResult {
    bool        ok = false;
    std::string commit;    // the short hash, when one was made
    std::string error;
    int         staged = 0;
};

// The message a run's commit carries.
//
// Its own function because this is the thing a person reads in six months, and
// the run id in it is what connects a line of code back to the board entry that
// explains why it is there.
std::string commit_message(const std::string& task, const std::string& run_id,
                           const std::vector<std::string>& paths);

// Stage exactly `paths` and commit them.
//
// Refuses rather than guesses: not a repo, nothing staged, or a path that escapes
// the repository all return an error and leave the index alone. A partial commit
// is worse than none, because it looks like a complete one.
CommitResult commit_paths(const std::filesystem::path& root,
                          const std::vector<std::string>& paths,
                          const std::string& message);

// --- a branch per coder --------------------------------------------------------
//
// One accepted changeset, on its own branch, so you can check it out, diff it,
// cherry-pick it or throw it away WITHOUT touching what the other coders did.
// That is the thing a shared working tree cannot give you: three coders landing
// into one tree leaves a pile you have to unpick by hand.
//
// THE CODER STILL NEVER TOUCHES GIT. `git` is not on the runnable allowlist and a
// sandbox is a tree copy rather than a worktree precisely so a coder cannot reach
// real history. Auspex makes the branch, after the Auditor has passed the work.
// Handing coders worktrees would give them the object store, which is the thing
// the copy exists to withhold.
//
// AND IT NEVER TOUCHES YOUR WORKING TREE. The commit is made in a throwaway
// worktree somewhere else, so your uncommitted work, your index and your current
// branch are all exactly where you left them. Several coders can land at once
// because each gets its own.

// A branch name for one coder's work: "crew/<run>/<n>-<slug>".
//
// Sanitised to what git will actually accept as a ref -- a title is a sentence a
// model wrote, and refs refuse spaces, "..", "~", "^", ":" and a trailing ".lock"
// among others. A name git rejects is a run that fails at the last step.
std::string branch_name(const std::string& run_id, int n, const std::string& title);

// Commit `changeset` onto a NEW branch cut from HEAD, leaving the working tree,
// the index and the current branch untouched.
//
// Fails rather than half-succeeds: a repository with no commits yet, a branch that
// already exists, a path outside the repository -- each returns an error having
// changed nothing.
CommitResult commit_to_branch(const std::filesystem::path& root,
                              const std::string& branch,
                              const Changeset& changeset,
                              const std::string& message);

}  // namespace auspex
