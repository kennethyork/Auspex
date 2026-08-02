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

#include <cstdint>
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

// Just the names, without reading a byte of any file.
//
// For the listings in prompts, which is most of what asks. list_files() reads
// every file to compare contents, and the coder loop rebuilds its listing on
// every turn -- doing that by reading a whole project each time is what made a
// compiled repository unusable.
std::vector<std::string> list_file_names(const std::filesystem::path& root);

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

    // A fingerprint of what this file held in the PROJECT when the coder started.
    //
    // A changeset carries whole file contents, not a patch, so applying one is an
    // overwrite. Without this there is nothing to notice that the file has moved
    // on since -- and a held change accepted an hour later would silently overwrite
    // an hour of your own edits, with no conflict and no warning. That is the
    // failure a held change is most likely to meet, because holding one is exactly
    // what makes time pass before it lands.
    //
    // 0 means "no baseline recorded" -- a changeset from before this existed --
    // and is deliberately NOT treated as a mismatch, or every stored change would
    // become unlandable on upgrade. See apply_changeset().
    std::uint64_t base_fingerprint = 0;

    bool operator==(const ChangedFile&) const = default;
};

// FNV-1a over the file's bytes. Not a security hash and not trying to be: this
// catches "somebody edited this while the change sat on the board", which is an
// accident, never an attack. Chosen over std::hash because that is not required
// to give the same answer in two different processes, and the two ends of this
// check are always two different processes.
std::uint64_t fingerprint(const std::string& text);

struct Changeset {
    std::vector<ChangedFile> files;
    std::string              diff;   // every file's unified diff, concatenated

    bool empty() const { return files.empty(); }

    bool operator==(const Changeset&) const = default;
};

// The name of the baseline manifest written into a sandbox at creation.
//
// In sandbox_excludes(), so it is never copied into a nested sandbox and never
// captured out of one.
extern const char* const kBaselineFile;

// What THIS CODER changed -- not what the sandbox and the project now differ by.
//
// Those are different questions and the difference cost real work. capture used
// to diff the sandbox against the project as it stands at capture time, so any
// file the PROJECT changed after the sandbox was made showed up as a change by
// this coder -- specifically, as a revert of it.
//
// Watched end to end: three coders, a run interrupted, then resumed. Coder 2's
// docstring landed. Coder 3's sandbox predated that, so its capture reported
// "f2.py loses its docstring" as coder 3's work, and accepting it silently undid
// coder 2. The Auditor actually caught it and said so; the changeset was wrong
// underneath the Auditor, which is not somewhere a review can save you.
//
// So a file is the coder's change only when it differs from what the SANDBOX
// started with, recorded at creation. A file the coder never opened is identical
// to its baseline and is not reported, however far the project has moved.
//
// Without a baseline manifest -- a sandbox from before this existed -- it falls
// back to the old project-relative comparison, because a resume of work already
// on disk must keep working.
Changeset capture_changeset(const std::filesystem::path& project,
                            const std::filesystem::path& sandbox);

// path -> fingerprint of every file the sandbox was created with. Empty when the
// sandbox has no manifest.
std::map<std::string, std::uint64_t> sandbox_baseline(
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
