// Where a coder works, and what it produces.
//
// A sandbox is a COPY of the project tree, not a git worktree. That is ollamadev's
// design and it is ported rather than improved on: a worktree needs the project to
// be a git repository and shares its object store, so a coder that runs `git` in
// its own sandbox can reach the user's history. A plain copy has neither problem,
// and the price -- copying the tree per coder -- is paid once against a run that
// will spend minutes in a language model.
//
// A changeset is the difference between a sandbox and the project it came from:
// the files whose contents differ, their new text, and a unified diff for reading.
// Nothing here talks to a model. Capture and apply are pure file operations, which
// is what makes the dangerous half of a crew testable without one.
//
// SAFETY. apply_changeset() writes into the user's project. Every path is checked
// to stay inside it before anything is opened -- a changeset carries paths that
// came, ultimately, from a language model's choice of filename, and "../.." is a
// filename. See safe_join().
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

// Directory and file names never copied into a sandbox, and never captured out of
// one.
//
// Beyond the obvious VCS and vendor directories, the caches are here because a
// coder that RUNS its own work -- a python import, a test suite -- leaves
// interpreter and test caches behind. Those are not the coder's changes, and
// without this they get captured into the changeset and land in the project when
// it is accepted.
const std::vector<std::string>& sandbox_excludes();

// True when a path component is one of the above.
bool is_excluded(const std::string& name);

// Every file under `root`, as project-relative path -> contents.
//
// Text only: a file containing a NUL byte is skipped, because everything
// downstream of this diffs and re-writes what it finds, and doing that to a
// binary produces a corrupt file rather than a bad patch.
std::map<std::string, std::string> list_files(const std::filesystem::path& root);

// `base / relative`, but only when the result stays inside `base`.
//
// nullopt for anything that escapes -- an absolute path, a leading "..", or a
// symlink component pointing out of the tree. This is the check between a
// model-chosen filename and an open() on the user's disk, so it is written to
// fail closed and is tested against the ways out that actually work.
std::optional<std::filesystem::path> safe_join(const std::filesystem::path& base,
                                               const std::string& relative);

// --- diffing -----------------------------------------------------------------

// A unified diff of one file, in the form `git apply` accepts.
//
// The `new file mode` / `deleted file mode` lines are not decoration: without them
// git apply strips /dev/null under -p1 and rejects the patch. 100644 is an
// assumption, because this function sees two strings and never the file's mode.
//
// A final line with no trailing newline is deliberately NOT the same line as one
// with it. Without that distinction a file whose only change is gaining or losing
// its last newline diffs to nothing at all.
std::string unified_diff(const std::string& path, const std::string& old_text,
                         const std::string& new_text, int context = 3);

// Splits into lines, reporting whether the last one lacked a trailing newline.
// Exposed for testing, because that flag is where the off-by-one lives.
std::vector<std::string> diff_lines(const std::string& text, bool* no_final_newline);

// --- sandboxes ---------------------------------------------------------------

// Copies `project` to `dest`, skipping the excludes. `dest` must not exist.
bool create_sandbox(const std::filesystem::path& project,
                    const std::filesystem::path& dest, std::string* error = nullptr);

// Removes a sandbox. Never touches anything outside `dest`.
bool destroy_sandbox(const std::filesystem::path& dest);

// --- changesets --------------------------------------------------------------

struct ChangedFile {
    std::string path;       // project-relative
    std::string contents;   // the new text; empty for a deletion
    bool        deleted = false;

    bool operator==(const ChangedFile&) const = default;
};

struct Changeset {
    std::vector<ChangedFile> files;
    std::string              diff;   // every file's unified diff, concatenated

    bool empty() const { return files.empty(); }

    bool operator==(const Changeset&) const = default;
};

// What `sandbox` changed relative to `project`.
//
// Ordered by path so two captures of the same work produce the same diff -- a
// changeset is read by a person and by an Auditor, and neither should see the
// files shuffle between runs.
Changeset capture_changeset(const std::filesystem::path& project,
                            const std::filesystem::path& sandbox);

// Writes a changeset into `project`. Reports which paths were written.
//
// Refuses the WHOLE changeset if any path in it escapes the project, rather than
// applying the safe ones and skipping the rest: a half-applied changeset is a
// state nobody asked for and nothing can undo.
bool apply_changeset(const Changeset& changeset, const std::filesystem::path& project,
                     std::vector<std::string>* wrote = nullptr,
                     std::string* error = nullptr);

}  // namespace auspex
