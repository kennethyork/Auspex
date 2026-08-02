#include "auspex/code_index.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"
#include "auspex/sandbox.hpp"
#include "auspex/json_util.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

// How much of a chunk is not whitespace. A window of blank lines and closing
// braces embeds to something, and that something competes with real answers.
std::size_t content_chars(const std::string& text) {
    std::size_t n = 0;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) ++n;
    }
    return n;
}

// The embedding model's window, not ours. Past this the server truncates anyway,
// and sending more costs time to have it thrown away.
constexpr std::size_t kMaxEmbedChars = 8000;

}  // namespace

// ---------------------------------------------------------------------------
std::string embed_model(const Config& config) {
    // Not a Config field of its own: the embedding model is a detail of this
    // feature, and adding a top-level setting for it would put it beside
    // ollama_model where it would be mistaken for the model that answers.
    (void)config;
    return "nomic-embed-text";
}

std::filesystem::path index_path(const std::filesystem::path& project) {
    if (project.empty()) return {};
    return project / ".auspex" / "index.json";
}

// ---------------------------------------------------------------------------
std::vector<CodeChunk> chunk_text(const std::string& file, const std::string& text,
                                  const IndexOptions& options) {
    std::vector<CodeChunk> chunks;
    if (file.empty() || text.empty()) return chunks;

    const int per = std::max(1, options.lines_per_chunk);
    // Overlap must leave forward progress, or the loop never advances.
    const int overlap = std::clamp(options.overlap_lines, 0, per - 1);
    const int stride  = per - overlap;

    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    if (lines.empty()) return chunks;

    for (std::size_t start = 0; start < lines.size();
         start += static_cast<std::size_t>(stride)) {
        const std::size_t stop = std::min(lines.size(), start + static_cast<std::size_t>(per));

        std::string body;
        for (std::size_t i = start; i < stop; ++i) {
            body += lines[i];
            body += '\n';
        }

        if (content_chars(body) >= options.min_chunk_chars) {
            CodeChunk chunk;
            chunk.file  = file;
            chunk.start = static_cast<int>(start) + 1;   // 1-based, as an editor counts
            chunk.end   = static_cast<int>(stop);
            chunk.text  = body.size() > kMaxEmbedChars ? body.substr(0, kMaxEmbedChars)
                                                       : body;
            chunks.push_back(std::move(chunk));
        }

        if (stop == lines.size()) break;
    }

    return chunks;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n == 0) return 0.0;

    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        na  += static_cast<double>(a[i]) * static_cast<double>(a[i]);
        nb  += static_cast<double>(b[i]) * static_cast<double>(b[i]);
    }
    const double magnitude = std::sqrt(na) * std::sqrt(nb);
    // Zero rather than NaN for an all-zero vector, so an unembedded chunk sinks
    // instead of tying with everything at once.
    return magnitude > 0.0 ? dot / magnitude : 0.0;
}

std::vector<CodeHit> rank(const std::vector<CodeChunk>& chunks,
                          const std::vector<float>& query, int limit) {
    std::vector<CodeHit> hits;
    if (query.empty() || limit <= 0) return hits;

    hits.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        hits.push_back({chunk.file, chunk.start, chunk.end, chunk.text,
                        cosine(query, chunk.vector)});
    }

    // Stable, so two chunks with identical scores keep index order rather than
    // swapping between runs -- a search that reorders its own ties looks broken.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const CodeHit& a, const CodeHit& b) { return a.score > b.score; });

    if (hits.size() > static_cast<std::size_t>(limit)) {
        hits.resize(static_cast<std::size_t>(limit));
    }
    return hits;
}

// ---------------------------------------------------------------------------
std::string encode_index(const std::vector<CodeChunk>& chunks,
                         const std::string& model) {
    json document;
    document["model"] = model;
    document["chunks"] = json::array();

    for (const auto& chunk : chunks) {
        json entry;
        entry["file"]  = chunk.file;
        entry["start"] = chunk.start;
        entry["end"]   = chunk.end;
        entry["text"]  = chunk.text;
        entry["vec"]   = chunk.vector;
        document["chunks"].push_back(std::move(entry));
    }
    // Compact: an index of a real project is thousands of vectors of hundreds of
    // floats, and pretty-printing it would triple a file nobody reads by eye.
    return safe_dump(document);
}

std::vector<CodeChunk> decode_index(const std::string& json_text) {
    std::vector<CodeChunk> chunks;

    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return chunks;
    if (!document.contains("chunks") || !document["chunks"].is_array()) return chunks;

    for (const auto& entry : document["chunks"]) {
        if (!entry.is_object()) continue;

        CodeChunk chunk;
        if (entry.contains("file") && entry["file"].is_string()) {
            chunk.file = entry["file"].get<std::string>();
        }
        if (entry.contains("start") && entry["start"].is_number_integer()) {
            chunk.start = entry["start"].get<int>();
        }
        if (entry.contains("end") && entry["end"].is_number_integer()) {
            chunk.end = entry["end"].get<int>();
        }
        if (entry.contains("text") && entry["text"].is_string()) {
            chunk.text = entry["text"].get<std::string>();
        }
        if (entry.contains("vec") && entry["vec"].is_array()) {
            chunk.vector.reserve(entry["vec"].size());
            for (const auto& value : entry["vec"]) {
                if (value.is_number()) {
                    chunk.vector.push_back(static_cast<float>(value.get<double>()));
                }
            }
        }
        if (chunk.file.empty()) continue;
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

// ---------------------------------------------------------------------------
IndexReport build_index(const Config& config, const std::filesystem::path& project,
                        const IndexProgress& progress, const IndexOptions& options) {
    IndexReport report;

    if (!std::filesystem::is_directory(project)) {
        report.error = "no such project";
        return report;
    }

    OllamaClient ollama(config);
    const std::string model = embed_model(config);

    // Probed once, before any work. Without this a missing embedding model means
    // one failed request per chunk and a report of "0 chunks" that says nothing
    // about why.
    if (ollama.embed(model, "ping").empty()) {
        report.error = "the embedding model " + model +
                       " did not answer (try: ollama pull " + model + ")";
        return report;
    }

    // list_files() rather than a walk of our own: the excludes, the size cap and
    // the binary test are already there, and a second copy would drift from the
    // one the sandbox uses.
    std::vector<CodeChunk> chunks;
    for (const auto& [path, contents] : list_files(project)) {
        auto file_chunks = chunk_text(path, contents, options);
        if (file_chunks.empty()) continue;
        ++report.files;
        chunks.insert(chunks.end(), std::make_move_iterator(file_chunks.begin()),
                      std::make_move_iterator(file_chunks.end()));
    }

    const int total = static_cast<int>(chunks.size());
    int done = 0;
    for (auto& chunk : chunks) {
        chunk.vector = ollama.embed(model, chunk.text);
        // One retry. The model is live and local; a single failure is usually the
        // server being busy rather than the request being wrong.
        if (chunk.vector.empty()) chunk.vector = ollama.embed(model, chunk.text);
        if (progress) progress(++done, total);
    }

    // Chunks that never embedded are dropped rather than stored empty: kept, they
    // would score 0 against everything and pad the file for nothing.
    chunks.erase(std::remove_if(chunks.begin(), chunks.end(),
                                [](const CodeChunk& c) { return c.vector.empty(); }),
                 chunks.end());
    report.chunks = static_cast<int>(chunks.size());

    const auto path = index_path(project);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        report.error = "could not create " + path.parent_path().string();
        return report;
    }

    // Temp plus rename, so a search running against the old index never reads half
    // of a new one.
    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            report.error = "could not write the index";
            return report;
        }
        out << encode_index(chunks, model);
        if (!out) {
            report.error = "could not write the index";
            return report;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        report.error = "could not replace the index";
    }
    return report;
}

SearchReport search_index(const Config& config, const std::filesystem::path& project,
                          const std::string& query, int limit) {
    SearchReport report;

    const std::string trimmed = trim(query);
    if (trimmed.empty()) {
        report.error = "nothing to search for";
        return report;
    }

    std::ifstream in(index_path(project));
    if (!in) {
        report.error = "no index for this project yet";
        return report;
    }
    std::ostringstream contents;
    contents << in.rdbuf();

    const auto chunks = decode_index(contents.str());
    if (chunks.empty()) {
        report.error = "the index is empty or unreadable";
        return report;
    }

    OllamaClient ollama(config);
    const auto vector = ollama.embed(embed_model(config), trimmed);
    if (vector.empty()) {
        report.error = "could not embed the query";
        return report;
    }

    report.hits = rank(chunks, vector, limit);
    return report;
}

std::string relevant_files_note(const Config& config,
                                const std::filesystem::path& project,
                                const std::string& task, int limit) {
    const SearchReport found = search_index(config, project, task, limit);
    // Silent on failure. Planning must still work with no index, and an error
    // about embeddings in the middle of a Director's prompt would be noise it
    // cannot act on.
    if (!found.ok() || found.hits.empty()) return {};

    // Files, not chunks. The Director decides how to CUT UP a job; it needs to
    // know where the work lives, and three windows from one file say the same
    // thing three times.
    std::vector<std::string> seen;
    std::ostringstream out;
    out << "These files look most related to the task:\n";
    for (const auto& hit : found.hits) {
        if (std::find(seen.begin(), seen.end(), hit.file) != seen.end()) continue;
        seen.push_back(hit.file);
        out << "  " << hit.file << "\n";
    }
    out << "\n";
    return out.str();
}

}  // namespace auspex
