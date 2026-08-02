#include "auspex/sandbox.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <unordered_map>

namespace auspex {

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

bool write_file(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

// A NUL byte is the practical test for "not text". Everything downstream diffs
// and rewrites what it is given, and doing that to a binary produces a corrupt
// file rather than a bad patch.
bool looks_binary(const std::string& contents) {
    return contents.find('\0') != std::string::npos;
}

// Files past this are not read at all.
//
// Not a judgement about what matters -- a judgement about what a coder can DO. A
// write replaces a whole file, so anything a coder could usefully change has to
// fit in a prompt, and nothing that fits in a prompt is a megabyte. Without this,
// one checked-in dump or minified bundle is read in full on every single turn.
constexpr std::uintmax_t kMaxFileBytes = 1024 * 1024;

// --- the diff ---------------------------------------------------------------
//
// Longest common subsequence, but only after the common prefix and suffix are
// trimmed off. That trim is what makes this usable: an edit of three lines in a
// four-thousand-line file leaves a handful of lines to compare, and the quadratic
// table is built over those rather than over the file.
//
// The cap below is a real limit, not a formality -- a full-table LCS over two
// large, wholly-different files would allocate gigabytes. Past it, the file is
// reported as replaced wholesale, which is true, readable, and applies correctly;
// it is only a worse-looking diff.
constexpr std::size_t kMaxTable = 4'000'000;   // ~4M cells

struct Op {
    char kind;   // ' ', '-', '+'
    int  a = -1;
    int  b = -1;
};

std::vector<Op> diff_ops(const std::vector<int>& a, const std::vector<int>& b) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());

    int prefix = 0;
    while (prefix < n && prefix < m && a[prefix] == b[prefix]) ++prefix;

    int suffix = 0;
    while (suffix < n - prefix && suffix < m - prefix &&
           a[n - 1 - suffix] == b[m - 1 - suffix]) {
        ++suffix;
    }

    const int an = n - prefix - suffix;
    const int bm = m - prefix - suffix;

    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(n + m));
    for (int i = 0; i < prefix; ++i) ops.push_back({' ', i, i});

    const bool too_big = static_cast<std::size_t>(an) * static_cast<std::size_t>(bm) >
                         kMaxTable;
    if (too_big) {
        // Everything in the middle, removed then added. Correct, just coarse.
        for (int i = 0; i < an; ++i) ops.push_back({'-', prefix + i, -1});
        for (int j = 0; j < bm; ++j) ops.push_back({'+', -1, prefix + j});
    } else if (an > 0 || bm > 0) {
        // lcs[i][j] = length of the LCS of a[prefix+i..] and b[prefix+j..]
        std::vector<std::vector<int>> lcs(
            static_cast<std::size_t>(an) + 1,
            std::vector<int>(static_cast<std::size_t>(bm) + 1, 0));
        for (int i = an - 1; i >= 0; --i) {
            for (int j = bm - 1; j >= 0; --j) {
                lcs[i][j] = a[prefix + i] == b[prefix + j]
                                ? lcs[i + 1][j + 1] + 1
                                : std::max(lcs[i + 1][j], lcs[i][j + 1]);
            }
        }

        int i = 0, j = 0;
        while (i < an && j < bm) {
            if (a[prefix + i] == b[prefix + j]) {
                ops.push_back({' ', prefix + i, prefix + j});
                ++i;
                ++j;
            } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
                ops.push_back({'-', prefix + i, -1});
                ++i;
            } else {
                ops.push_back({'+', -1, prefix + j});
                ++j;
            }
        }
        while (i < an) ops.push_back({'-', prefix + i++, -1});
        while (j < bm) ops.push_back({'+', -1, prefix + j++});
    }

    for (int k = 0; k < suffix; ++k) {
        ops.push_back({' ', n - suffix + k, m - suffix + k});
    }
    return ops;
}

}  // namespace

// ---------------------------------------------------------------------------
const char* const kBaselineFile = ".auspex-baseline";

const std::vector<std::string>& sandbox_excludes() {
    static const std::vector<std::string> kExcludes{
        // Version control and our own state.
        ".git", ".hg", ".svn", ".ollamadev", ".auspex", "auspex-crew",
        // The sandbox's own baseline manifest -- our bookkeeping, never a file the
        // coder wrote, so it must not be copied onward or captured back.
        kBaselineFile,
        // Dependencies somebody else installed.
        "node_modules", "vendor", "third_party", "Pods", ".bundle",
        // CMake FetchContent puts fetched projects here. It is somebody else's
        // source, it is large, and a symbol search that includes it answers "where
        // is update()" with a file in a vendored LLM runtime.
        "_deps",
        // BUILD OUTPUT. Measured on a real Qt project: build 47M, .build 146M,
        // dist 106M -- three hundred megabytes that would be copied into every
        // coder's sandbox and re-read on every turn, to find nothing a coder
        // should ever edit. Their absence made this module unusable on any
        // compiled project.
        "build", ".build", "cmake-build-debug", "cmake-build-release",
        "dist", "out", "target", "bin-int", "obj", ".gradle", ".dart_tool",
        ".next", ".nuxt", ".svelte-kit", ".parcel-cache", ".turbo",
        // Caches a coder that runs its own work leaves behind.
        "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".tox",
        ".eggs", ".cache", ".ccache", "coverage", ".nyc_output", ".venv", "venv",
        ".DS_Store",
    };
    return kExcludes;
}

bool is_excluded(const std::string& name) {
    const auto& list = sandbox_excludes();
    return std::find(list.begin(), list.end(), name) != list.end();
}

std::optional<std::filesystem::path> safe_join(const std::filesystem::path& base,
                                               const std::string& relative) {
    if (relative.empty()) return std::nullopt;

    const std::filesystem::path candidate(relative);
    // An absolute path is not relative to anything, and neither is one with a
    // root name. Both are rejected before any normalisation, because normalising
    // them would produce something that looks fine.
    if (candidate.is_absolute() || candidate.has_root_path()) return std::nullopt;

    // Walk the components rather than trusting lexically_normal(): ".." is
    // rejected outright, so there is no arithmetic to get wrong. A path that
    // climbs out and back in ("a/../../b") is refused too -- it has no legitimate
    // use here and allowing it would mean reasoning about symlinks mid-path.
    std::filesystem::path joined = base;
    for (const auto& part : candidate) {
        const std::string name = part.string();
        if (name.empty() || name == ".") continue;
        if (name == "..") return std::nullopt;
        if (is_excluded(name)) return std::nullopt;
        joined /= name;
    }
    if (joined == base) return std::nullopt;

    // Belt and braces against a symlink component: if the parent already exists,
    // its real location must still be inside the project.
    std::error_code ec;
    const auto parent = joined.parent_path();
    if (std::filesystem::exists(parent, ec)) {
        const auto real_parent = std::filesystem::weakly_canonical(parent, ec);
        const auto real_base   = std::filesystem::weakly_canonical(base, ec);
        if (!ec) {
            const std::string p = real_parent.string();
            const std::string b = real_base.string();
            if (p != b && p.rfind(b + "/", 0) != 0) return std::nullopt;
        }
    }
    return joined;
}

std::map<std::string, std::string> list_files(const std::filesystem::path& root) {
    std::map<std::string, std::string> files;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return files;

    // follow_directory_symlink is deliberately OFF. A symlinked directory inside
    // the project would otherwise be walked into, and its contents captured as if
    // they lived here -- which on accept would write them into the project for
    // real.
    auto it = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return files;

    for (auto end = std::filesystem::recursive_directory_iterator(); it != end;
         it.increment(ec)) {
        if (ec) break;

        const auto& entry = *it;
        const std::string name = entry.path().filename().string();

        if (entry.is_directory(ec)) {
            if (is_excluded(name)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;   // symlinks, sockets, devices
        if (is_excluded(name)) continue;

        // Size BEFORE contents. A stat is free; reading a 200MB artefact to
        // discover it is binary is what made this unusable on a compiled project.
        if (const auto size = entry.file_size(ec); ec || size > kMaxFileBytes) continue;

        const std::string contents = read_file(entry.path());
        if (looks_binary(contents)) continue;

        files.emplace(
            std::filesystem::relative(entry.path(), root, ec).generic_string(),
            contents);
    }

    return files;
}

// ---------------------------------------------------------------------------
std::vector<std::string> list_file_names(const std::filesystem::path& root) {
    std::vector<std::string> names;

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return names;

    auto it = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return names;

    for (auto end = std::filesystem::recursive_directory_iterator(); it != end;
         it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        const std::string name = entry.path().filename().string();

        if (entry.is_directory(ec)) {
            if (is_excluded(name)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(ec) || is_excluded(name)) continue;
        if (const auto size = entry.file_size(ec); ec || size > kMaxFileBytes) continue;

        // Deliberately does NOT read the file. A listing needs names, and reading
        // every file to produce one is the difference between a prompt that costs
        // milliseconds and one that costs a second of disk on every turn.
        names.push_back(
            std::filesystem::relative(entry.path(), root, ec).generic_string());
    }

    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> diff_lines(const std::string& text, bool* no_final_newline) {
    std::vector<std::string> lines;
    if (no_final_newline) *no_final_newline = false;
    if (text.empty()) return lines;

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            if (no_final_newline) *no_final_newline = true;
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
        if (start == text.size()) break;   // trailing newline: no empty last line
    }
    return lines;
}

std::string unified_diff(const std::string& path, const std::string& old_text,
                         const std::string& new_text, int context) {
    std::string out = "diff --git a/" + path + " b/" + path + "\n";
    if (old_text == new_text) return out;
    if (context < 0) context = 0;

    bool old_bare = false, new_bare = false;
    const auto old_lines = diff_lines(old_text, &old_bare);
    const auto new_lines = diff_lines(new_text, &new_bare);

    if (old_text.empty()) {
        out += "new file mode 100644\n--- /dev/null\n+++ b/" + path + "\n";
    } else if (new_text.empty()) {
        out += "deleted file mode 100644\n--- a/" + path + "\n+++ /dev/null\n";
    } else {
        out += "--- a/" + path + "\n+++ b/" + path + "\n";
    }

    // Lines interned to ints. The comparison runs many times over the same pairs,
    // and an int compare beats a string compare by a wide margin on a large file.
    //
    // A final line with no trailing newline gets a DISTINCT identity from the same
    // text with one, or a file whose only change is gaining its last newline would
    // diff to nothing.
    std::unordered_map<std::string, int> ids;
    const auto id_of = [&ids](const std::string& line, bool bare) {
        const std::string key = bare ? line + '\1' : line;
        const auto found = ids.find(key);
        if (found != ids.end()) return found->second;
        const int id = static_cast<int>(ids.size());
        ids.emplace(key, id);
        return id;
    };

    std::vector<int> a, b;
    a.reserve(old_lines.size());
    b.reserve(new_lines.size());
    for (std::size_t k = 0; k < old_lines.size(); ++k) {
        a.push_back(id_of(old_lines[k], old_bare && k + 1 == old_lines.size()));
    }
    for (std::size_t k = 0; k < new_lines.size(); ++k) {
        b.push_back(id_of(new_lines[k], new_bare && k + 1 == new_lines.size()));
    }

    const std::vector<Op> ops = diff_ops(a, b);

    // Group the ops into hunks: every run of changes, plus `context` unchanged
    // lines either side, merged when two runs are close enough to share.
    std::vector<bool> interesting(ops.size(), false);
    for (std::size_t k = 0; k < ops.size(); ++k) {
        if (ops[k].kind == ' ') continue;
        const std::size_t from = k > static_cast<std::size_t>(context)
                                     ? k - static_cast<std::size_t>(context)
                                     : 0;
        const std::size_t to = std::min(ops.size(), k + static_cast<std::size_t>(context) + 1);
        for (std::size_t j = from; j < to; ++j) interesting[j] = true;
    }

    std::size_t k = 0;
    while (k < ops.size()) {
        if (!interesting[k]) {
            ++k;
            continue;
        }
        const std::size_t start = k;
        while (k < ops.size() && interesting[k]) ++k;

        int a_start = 0, a_count = 0, b_start = 0, b_count = 0;
        bool have_a = false, have_b = false;
        for (std::size_t j = start; j < k; ++j) {
            if (ops[j].a >= 0) {
                if (!have_a) { a_start = ops[j].a; have_a = true; }
                ++a_count;
            }
            if (ops[j].b >= 0) {
                if (!have_b) { b_start = ops[j].b; have_b = true; }
                ++b_count;
            }
        }

        // Unified-diff line numbers are 1-based, and a zero-length side is written
        // as the line BEFORE the change rather than as line 1.
        out += "@@ -" + std::to_string(a_count ? a_start + 1 : a_start) + "," +
               std::to_string(a_count) + " +" +
               std::to_string(b_count ? b_start + 1 : b_start) + "," +
               std::to_string(b_count) + " @@\n";

        for (std::size_t j = start; j < k; ++j) {
            const Op& op = ops[j];
            const bool from_old = op.kind != '+';
            const std::string& line =
                from_old ? old_lines[static_cast<std::size_t>(op.a)]
                         : new_lines[static_cast<std::size_t>(op.b)];
            out += op.kind;
            out += line;
            out += "\n";

            const bool bare = from_old
                                  ? (old_bare && op.a + 1 == static_cast<int>(old_lines.size()))
                                  : (new_bare && op.b + 1 == static_cast<int>(new_lines.size()));
            if (bare) out += "\\ No newline at end of file\n";
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
bool create_sandbox(const std::filesystem::path& project,
                    const std::filesystem::path& dest, std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };

    std::error_code ec;
    if (!std::filesystem::is_directory(project, ec)) return fail("no such project");
    if (std::filesystem::exists(dest, ec)) return fail("sandbox already exists");

    std::filesystem::create_directories(dest, ec);
    if (ec) return fail("could not create the sandbox: " + ec.message());

    // Copied file by file through list_files() rather than with copy(), so the
    // excludes and the binary rule are applied ONCE, here and in capture. Two
    // copies of that logic is how a cache directory ends up in a changeset.
    // What the sandbox started with, recorded as it is copied.
    //
    // This is what makes "what did THIS coder change" answerable later. Without
    // it, capture can only ask "how do the sandbox and the project differ now",
    // which counts everything that happened to the project meanwhile as this
    // coder's work -- see capture_changeset().
    std::ostringstream manifest;
    for (const auto& [relative, contents] : list_files(project)) {
        const auto target = safe_join(dest, relative);
        if (!target) continue;
        if (!write_file(*target, contents)) {
            return fail("could not write " + relative + " into the sandbox");
        }
        // One line per file: fingerprint, a space, then the path. The path is last
        // because it is the field that can contain a space.
        manifest << fingerprint(contents) << ' ' << relative << '\n';
    }

    // On disk rather than in memory: the process that captures may not be the one
    // that created this -- a resume after a restart is exactly that case.
    if (!write_file(dest / kBaselineFile, manifest.str())) {
        return fail("could not record what the sandbox started with");
    }
    return true;
}

bool destroy_sandbox(const std::filesystem::path& dest) {
    if (dest.empty()) return false;
    std::error_code ec;
    // A relative path here would remove something under the process's working
    // directory, which is not what any caller means.
    if (!dest.is_absolute()) return false;
    std::filesystem::remove_all(dest, ec);
    return !ec;
}

// ---------------------------------------------------------------------------
std::uint64_t fingerprint(const std::string& text) {
    // FNV-1a, 64-bit. Spelled out rather than taken from std::hash because the
    // two ends of this comparison are always two different processes -- the run
    // that captured the change and the one that lands it, often after a restart --
    // and std::hash is not required to agree between them.
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    // 0 means "no baseline recorded", so a real hash must never be 0. One input in
    // 2^64 would collide with the sentinel; this costs nothing and removes it.
    return hash == 0 ? 1 : hash;
}

std::map<std::string, std::uint64_t> sandbox_baseline(
    const std::filesystem::path& sandbox) {
    std::map<std::string, std::uint64_t> baseline;
    std::ifstream in(sandbox / kBaselineFile, std::ios::binary);
    if (!in) return baseline;

    std::string line;
    while (std::getline(in, line)) {
        const auto space = line.find(' ');
        if (space == std::string::npos || space + 1 >= line.size()) continue;
        try {
            baseline[line.substr(space + 1)] =
                std::stoull(line.substr(0, space));
        } catch (const std::exception&) {
            // A corrupt line is skipped, not fatal. The consequence is that one
            // file falls back to the project comparison, which is the old
            // behaviour rather than a wrong answer.
        }
    }
    return baseline;
}

Changeset capture_changeset(const std::filesystem::path& project,
                            const std::filesystem::path& sandbox) {
    Changeset changeset;

    const auto before = list_files(project);
    const auto after  = list_files(sandbox);
    const auto baseline = sandbox_baseline(sandbox);

    // std::map iterates in key order, so the result is stable across runs. A
    // changeset is read by a person and by an Auditor, and neither should see the
    // files shuffle between two captures of the same work.
    for (const auto& [path, contents] : after) {
        const auto found = before.find(path);

        if (!baseline.empty()) {
            // Did the CODER change this file? Only the sandbox's own starting
            // state can answer that. A file it never opened still matches its
            // baseline no matter how far the project has moved on, and reporting
            // it would be reporting somebody else's work as this coder's -- as a
            // revert of it.
            const auto was = baseline.find(path);
            if (was != baseline.end() && was->second == fingerprint(contents)) {
                continue;
            }
        } else if (found != before.end() && found->second == contents) {
            continue;   // no manifest: the old project-relative comparison
        }

        if (found != before.end() && found->second == contents) continue;

        // What the PROJECT held when this coder started -- empty for a file the
        // coder created. Checked again before the change is ever written back.
        const std::string base = found == before.end() ? std::string{} : found->second;
        changeset.files.push_back(
            {path, contents, /*deleted=*/false, fingerprint(base)});
        changeset.diff +=
            unified_diff(path, found == before.end() ? std::string{} : found->second,
                         contents);
    }

    for (const auto& [path, contents] : before) {
        if (after.find(path) != after.end()) continue;
        // A file the coder never had cannot have been deleted by it. Without this,
        // a file the PROJECT gained after the sandbox was made looks like a
        // deletion by every coder still running.
        if (!baseline.empty() && baseline.find(path) == baseline.end()) continue;
        changeset.files.push_back({path, {}, /*deleted=*/true, fingerprint(contents)});
        changeset.diff += unified_diff(path, contents, {});
    }

    return changeset;
}

bool apply_changeset(const Changeset& changeset, const std::filesystem::path& project,
                     std::vector<std::string>* wrote, std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };

    std::error_code ec;
    if (!std::filesystem::is_directory(project, ec)) return fail("no such project");

    // EVERY path is resolved before ANY file is touched. A changeset that is
    // partly outside the project is refused whole, because a half-applied one is
    // a state nobody asked for and nothing can undo.
    std::vector<std::pair<std::filesystem::path, const ChangedFile*>> targets;
    targets.reserve(changeset.files.size());
    for (const auto& file : changeset.files) {
        const auto target = safe_join(project, file.path);
        if (!target) return fail("refusing to write outside the project: " + file.path);
        targets.emplace_back(*target, &file);
    }

    // AND every baseline is checked before ANY file is touched, for the same
    // reason the paths are: refuse the whole thing or none of it.
    //
    // A changeset holds whole file contents, so landing one is an overwrite. A
    // change that has sat on the board while you kept working would otherwise
    // replace your edits with no conflict and no warning -- and holding a change
    // is precisely what makes time pass before it lands, so this is the normal
    // case rather than an exotic one.
    for (const auto& [target, file] : targets) {
        if (file->base_fingerprint == 0) continue;   // captured before this existed

        std::string now;
        if (std::filesystem::is_regular_file(target, ec)) {
            std::ifstream in(target, std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            now = buffer.str();
        }
        // An absent file hashes as empty, which is exactly right: a coder that
        // created a file expects it not to be there, and finding one means
        // somebody else got there first.
        if (fingerprint(now) != file->base_fingerprint) {
            return fail(file->path +
                        " has changed since this work was done, so landing it would "
                        "overwrite those edits. Nothing was written.");
        }
    }

    for (const auto& [target, file] : targets) {
        if (file->deleted) {
            std::filesystem::remove(target, ec);
        } else if (!write_file(target, file->contents)) {
            return fail("could not write " + file->path);
        }
        if (wrote) wrote->push_back(file->path);
    }
    return true;
}

}  // namespace auspex
