// Searching a project by meaning rather than by spelling.
//
// The Director gets a list of FILENAMES. That is enough to split "add rate
// limiting" into pieces when the project is small and honestly named, and useless
// the moment it is neither -- a name tells you nothing about which file already
// does the throttling. This gives it a way to ask.
//
// HOW: every text file is cut into overlapping windows of lines, each window is
// embedded by a small local model through Ollama, and the vectors are kept in one
// JSON file. A query is embedded the same way and compared by cosine similarity.
// No database, no service; the index is a file and the search is a loop.
//
// WHY THAT IS ENOUGH: a project is thousands of chunks, not billions. A linear
// scan over ten thousand vectors is milliseconds, and the alternative -- an
// approximate index -- would add a dependency and a failure mode to save time
// nobody was spending.
//
// EVERYTHING EXCEPT embed() AND THE FILE I/O IS PURE. Chunking, cosine, ranking
// and the on-disk format are text-in/values-out and tested without a model, which
// matters because a silently wrong ranking looks exactly like a working one.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "auspex/config.hpp"

namespace auspex {

// One indexed window of one file.
struct CodeChunk {
    std::string        file;    // project-relative
    int                start = 0;   // 1-based, inclusive
    int                end   = 0;   // 1-based, inclusive
    std::string        text;
    std::vector<float> vector;

    bool operator==(const CodeChunk&) const = default;
};

struct CodeHit {
    std::string file;
    int         start = 0;
    int         end   = 0;
    std::string snippet;
    double      score = 0.0;

    bool operator==(const CodeHit&) const = default;
};

// How the project is cut up.
struct IndexOptions {
    // Lines per chunk, and how many are repeated into the next one.
    //
    // The overlap is not waste. A function whose signature lands on the last line
    // of one chunk and whose body is in the next matches neither well; repeating a
    // few lines means every construct appears whole somewhere.
    int lines_per_chunk = 60;
    int overlap_lines   = 10;

    // Chunks with less than this much actual content are dropped. A window of
    // blank lines and closing braces embeds to something, and that something is
    // noise competing with real answers.
    std::size_t min_chunk_chars = 40;

    bool operator==(const IndexOptions&) const = default;
};

// The embedding model. `embed.model` in config, else nomic-embed-text.
std::string embed_model(const Config& config);

// <project>/.auspex/index.json.
//
// Inside the project so it moves with it, and under .auspex -- which is in
// sandbox_excludes() -- so a coder never sees it, never copies it into a sandbox,
// and can never land it as part of a changeset.
std::filesystem::path index_path(const std::filesystem::path& project);

// --- pure ---------------------------------------------------------------------

// Cuts one file's text into windows. Line numbers are 1-based and inclusive.
std::vector<CodeChunk> chunk_text(const std::string& file, const std::string& text,
                                  const IndexOptions& options = {});

// Cosine similarity. 0 when either side is empty or all zeroes, so an unembedded
// chunk sinks rather than tying with everything.
double cosine(const std::vector<float>& a, const std::vector<float>& b);

// Best `limit` chunks for an already-embedded query, ordered by score.
std::vector<CodeHit> rank(const std::vector<CodeChunk>& chunks,
                          const std::vector<float>& query, int limit);

std::string             encode_index(const std::vector<CodeChunk>& chunks,
                                     const std::string& model);
std::vector<CodeChunk>  decode_index(const std::string& json_text);

// --- with a model -------------------------------------------------------------

struct IndexReport {
    int         files  = 0;
    int         chunks = 0;
    std::string error;   // empty on success

    bool ok() const { return error.empty(); }
};

// Progress, for a UI. Called with (done, total) as chunks are embedded.
using IndexProgress = std::function<void(int, int)>;

// Builds the index and writes it. Blocking, and slow the first time.
//
// One failed probe before the work starts: if the embedding model does not answer
// at all, the alternative is one failed request per chunk and a report saying
// "0 chunks" that gives no clue why.
IndexReport build_index(const Config& config, const std::filesystem::path& project,
                        const IndexProgress& progress = {},
                        const IndexOptions& options = {});

// Searches an index built earlier. Empty with `error` set when there is none.
struct SearchReport {
    std::vector<CodeHit> hits;
    std::string          error;

    bool ok() const { return error.empty(); }
};

SearchReport search_index(const Config& config, const std::filesystem::path& project,
                          const std::string& query, int limit = 8);

// A few lines naming the files most relevant to a task, for the Director's prompt.
// Empty when there is no index -- planning must still work without one.
std::string relevant_files_note(const Config& config,
                                const std::filesystem::path& project,
                                const std::string& task, int limit = 8);

}  // namespace auspex
