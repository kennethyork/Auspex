#include "auspex/crew_run.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"
#include "auspex/cli_coder.hpp"
#include "auspex/code_index.hpp"
#include "auspex/mcp.hpp"
#include "auspex/ollama_client.hpp"
#include "auspex/skills.hpp"
#include "auspex/projects.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

std::filesystem::path data_home() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "auspex";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "auspex";
    }
    return {};
}

// Temp file plus rename. A reader that stats and parses this file while it is
// being written must never see half of it -- the panel polls it every two seconds
// for the life of a run.
bool write_atomically(const std::filesystem::path& path, const std::string& text) {
    if (path.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    const auto temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::string read_whole(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string now_stamp() {
    return std::to_string(static_cast<long long>(std::time(nullptr)));
}

}  // namespace

// ---------------------------------------------------------------------------
std::filesystem::path auspex_crew_dir() {
    const auto home = data_home();
    return home.empty() ? std::filesystem::path{} : home / "crew";
}

std::filesystem::path auspex_run_state_path() {
    const auto dir = auspex_crew_dir();
    return dir.empty() ? std::filesystem::path{} : dir / "current.json";
}

std::filesystem::path auspex_board_path() {
    const auto home = data_home();
    return home.empty() ? std::filesystem::path{} : home / "board" / "current.json";
}

std::filesystem::path changeset_store(const std::string& run_id, int n) {
    const auto dir = auspex_crew_dir();
    if (dir.empty() || run_id.empty()) return {};
    return dir / run_id / "changeset" / ("c" + std::to_string(n));
}

// ---------------------------------------------------------------------------
bool save_changeset(const std::filesystem::path& dir, const Changeset& changeset,
                    std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    if (dir.empty()) return fail("no store directory");

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return fail("could not create the store: " + ec.message());

    // The manifest carries the diff and the file list; the contents go in their
    // own files. Keeping contents out of the JSON means a changeset holding a
    // 200KB file does not become a 300KB escaped string that has to be parsed
    // before anything at all can be shown about it.
    json manifest;
    manifest["diff"] = changeset.diff;
    manifest["files"] = json::array();

    int index = 0;
    for (const auto& file : changeset.files) {
        json entry;
        entry["path"]    = file.path;
        entry["deleted"] = file.deleted;
        entry["blob"]    = index;
        manifest["files"].push_back(entry);

        if (!file.deleted) {
            const auto blob = dir / ("blob-" + std::to_string(index));
            std::ofstream out(blob, std::ios::binary | std::ios::trunc);
            if (!out) return fail("could not write " + blob.string());
            out << file.contents;
            if (!out) return fail("could not write " + blob.string());
        }
        ++index;
    }

    if (!write_atomically(dir / "manifest.json", manifest.dump(2))) {
        return fail("could not write the manifest");
    }
    return true;
}

Changeset load_changeset(const std::filesystem::path& dir) {
    Changeset changeset;
    if (dir.empty()) return changeset;

    const auto document =
        json::parse(read_whole(dir / "manifest.json"), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return changeset;

    if (document.contains("diff") && document["diff"].is_string()) {
        changeset.diff = document["diff"].get<std::string>();
    }
    if (!document.contains("files") || !document["files"].is_array()) return changeset;

    for (const auto& entry : document["files"]) {
        if (!entry.is_object()) continue;
        ChangedFile file;
        if (entry.contains("path") && entry["path"].is_string()) {
            file.path = entry["path"].get<std::string>();
        }
        if (entry.contains("deleted") && entry["deleted"].is_boolean()) {
            file.deleted = entry["deleted"].get<bool>();
        }
        if (file.path.empty()) continue;

        if (!file.deleted && entry.contains("blob") && entry["blob"].is_number_integer()) {
            file.contents =
                read_whole(dir / ("blob-" + std::to_string(entry["blob"].get<int>())));
        }
        changeset.files.push_back(std::move(file));
    }
    return changeset;
}

// ---------------------------------------------------------------------------
std::string RunOptions::backend_for_coder(int n) const {
    if (coder_backends.empty()) return coder_backend;
    // Round-robin, 1-based. A list shorter than the plan repeats rather than
    // running out -- two coders on claude is a sensible thing to ask for.
    const std::size_t index =
        static_cast<std::size_t>(n > 0 ? n - 1 : 0) % coder_backends.size();
    const std::string picked = coder_backends[index];
    return picked.empty() ? coder_backend : picked;
}

std::string RunOptions::model_for(const std::string& role) const {
    // Role first, then the run-wide model, then (by returning empty) the config's.
    if (role == "director" && !director_model.empty()) return director_model;
    if (role == "auditor"  && !auditor_model.empty())  return auditor_model;
    if (role == "coder"    && !coder_model.empty())    return coder_model;
    return model;
}

std::vector<std::string> overlapping_files(const Changeset& a, const Changeset& b) {
    std::vector<std::string> shared;
    for (const auto& left : a.files) {
        for (const auto& right : b.files) {
            if (left.path == right.path) {
                shared.push_back(left.path);
                break;
            }
        }
    }
    std::sort(shared.begin(), shared.end());
    return shared;
}

// ---------------------------------------------------------------------------
std::filesystem::path steer_mailbox(const std::string& run_id, int n) {
    const auto dir = auspex_crew_dir();
    if (dir.empty() || run_id.empty() || n <= 0) return {};
    return dir / run_id / ("steer-" + std::to_string(n));
}

std::string current_run_id() {
    return current_crew_run(auspex_run_state_path()).run_id;
}

bool steer_coder(int n, const std::string& message, std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };

    const CrewRun run = current_crew_run(auspex_run_state_path());
    if (run.run_id.empty()) return fail("no crew has run here yet");
    if (!run.active)        return fail("the crew is not running");

    const bool exists = std::any_of(
        run.subtasks.begin(), run.subtasks.end(),
        [n](const CrewSubtask& s) { return s.n == n; });
    if (!exists) return fail("there is no coder #" + std::to_string(n));

    if (!leave_steer(steer_mailbox(run.run_id, n), message)) {
        return fail("could not leave the message");
    }
    return true;
}

// ---------------------------------------------------------------------------
std::vector<std::string> resumable_runs() {
    std::vector<std::string> runs;
    const auto dir = auspex_crew_dir();
    if (dir.empty()) return runs;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return runs;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) continue;
        // A run is resumable only if a sandbox survived. One that finished cleanly
        // destroyed its own, so there is nothing left to recover and offering it
        // would be offering to redo work that already landed.
        bool has_sandbox = false;
        for (const auto& child : std::filesystem::directory_iterator(entry.path(), ec)) {
            if (child.is_directory(ec) &&
                child.path().filename().string().rfind("sandbox-", 0) == 0) {
                has_sandbox = true;
                break;
            }
        }
        if (has_sandbox) runs.push_back(entry.path().filename().string());
    }

    // Newest first. Run ids carry a unix timestamp, so they sort correctly as text
    // for any run made this millennium.
    std::sort(runs.begin(), runs.end(), std::greater<>());
    return runs;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
std::string security_prompt(const std::string& file, const std::string& contents) {
    std::ostringstream out;
    out << "You are a security reviewer. Read the file below and report only real, "
           "exploitable problems.\n\n"
           "Report:\n"
           "- injection: a shell, SQL or path built from input that is not checked\n"
           "- a credential, key or password written into the source\n"
           "- authentication or authorisation that can be skipped\n"
           "- unsafe deserialisation, or a path that escapes its directory\n\n"
           "Do NOT report: style, missing tests, missing comments, or "
           "\"could be improved\". If the file is fine, say so with an empty list. "
           "An invented finding wastes more of a person's time than a missed one.\n\n";
    out << "File: " << file << "\n```\n" << contents << "\n```\n\n";
    out << "Answer with JSON only:\n"
           "{\"findings\": [{\"severity\": \"high\"|\"medium\"|\"low\", "
           "\"detail\": \"one sentence, naming the line\"}]}\n";
    return out.str();
}

std::vector<Finding> parse_findings(const std::string& reply, const std::string& file) {
    std::vector<Finding> findings;

    const std::string body = extract_json(reply);
    if (body.empty()) return findings;

    const auto document = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) return findings;

    const json* list = nullptr;
    if (document.is_array()) list = &document;
    else if (document.is_object() && document.contains("findings") &&
             document["findings"].is_array()) {
        list = &document["findings"];
    }
    if (!list) return findings;

    for (const auto& entry : *list) {
        if (!entry.is_object()) continue;

        Finding finding;
        finding.file = file;
        if (entry.contains("detail") && entry["detail"].is_string()) {
            finding.detail = trim(entry["detail"].get<std::string>());
        }
        if (finding.detail.empty() && entry.contains("description") &&
            entry["description"].is_string()) {
            finding.detail = trim(entry["description"].get<std::string>());
        }
        if (entry.contains("severity") && entry["severity"].is_string()) {
            finding.severity = trim(entry["severity"].get<std::string>());
        }

        // Unknown severity becomes "low" rather than being dropped or promoted.
        // A finding with no severity is still a finding; calling it high because
        // the model forgot to say would be the report crying wolf.
        std::string level = finding.severity;
        std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        finding.severity = (level == "high" || level == "medium" || level == "low")
                               ? level
                               : "low";

        if (finding.detail.empty()) continue;   // nothing to report is not a report
        findings.push_back(std::move(finding));
    }
    return findings;
}

void sort_findings(std::vector<Finding>& findings) {
    const auto rank = [](const std::string& severity) {
        if (severity == "high")   return 0;
        if (severity == "medium") return 1;
        return 2;
    };
    // Stable, so two findings of one severity keep the order they were found in --
    // which is file order, and therefore reproducible.
    std::stable_sort(findings.begin(), findings.end(),
                     [&rank](const Finding& a, const Finding& b) {
                         return rank(a.severity) < rank(b.severity);
                     });
}

std::string security_report(const std::vector<Finding>& findings) {
    if (findings.empty()) return "No exploitable problems found.\n";

    std::ostringstream out;
    out << findings.size() << (findings.size() == 1 ? " finding\n\n" : " findings\n\n");
    for (const auto& finding : findings) {
        out << "[" << finding.severity << "] " << finding.file << "\n    "
            << finding.detail << "\n";
    }
    return out.str();
}

RunResult scan_security(const Config& config, const RunOptions& options,
                        const RunEvents& events, const std::atomic<bool>* cancel,
                        std::vector<Finding>* out_findings) {
    RunResult result;
    result.run_id = "scan_" + now_stamp();

    const auto note = [&events](const std::string& text) {
        if (events.log) events.log(text);
    };
    const auto stopped = [cancel] { return cancel && cancel->load(); };

    if (!is_project_dir(options.project)) {
        result.error = "no such project";
        return result;
    }

    // NO SANDBOX, and no coder. Nothing here can write: the scan reads the project
    // and produces text. A vulnerability hunt that could also edit files would need
    // reviewing as carefully as any other change, which defeats the point.
    const auto files = list_files(options.project);
    note("security: reading " + std::to_string(files.size()) + " files");

    OllamaClient ollama(config);
    GenerateOptions generate;
    generate.json = true;
    generate.disable_thinking = true;
    generate.temperature = 0.1;

    const std::string model = options.model_for("auditor");
    std::vector<Finding> findings;
    int done = 0;

    for (const auto& [path, contents] : files) {
        if (stopped()) {
            result.error = "cancelled";
            break;
        }
        ++done;
        if (done % 10 == 0) {
            note("security: " + std::to_string(done) + "/" +
                 std::to_string(files.size()));
        }

        // A file too large to send whole is skipped rather than truncated: half a
        // file reviewed is a review that can miss the half it did not see and say
        // nothing about it.
        if (contents.size() > options.audit.max_diff_bytes) continue;

        const auto reply = ollama.generate(
            model.empty() ? config.ollama_model : model,
            security_prompt(path, contents), generate);
        if (!reply) continue;

        for (auto& finding : parse_findings(
                 reply->response.empty() ? reply->thinking : reply->response, path)) {
            findings.push_back(std::move(finding));
        }
    }

    sort_findings(findings);
    if (out_findings) *out_findings = findings;

    result.held = static_cast<int>(findings.size());
    note(security_report(findings));
    return result;
}

std::filesystem::path lessons_path(const std::filesystem::path& project) {
    if (project.empty()) return {};
    // Under .auspex, so a coder can never read its own lessons file and rewrite
    // what it is about to be told.
    return project / ".auspex" / "lessons.txt";
}

std::vector<std::string> read_lessons(const std::filesystem::path& project) {
    std::vector<std::string> lessons;
    std::ifstream in(lessons_path(project));
    if (!in) return lessons;

    std::string line;
    while (std::getline(in, line)) {
        if (const std::string trimmed = trim(line); !trimmed.empty()) {
            lessons.push_back(trimmed);
        }
    }
    return lessons;
}

bool write_lessons(const std::filesystem::path& project,
                   const std::vector<std::string>& lessons, std::size_t limit) {
    const auto path = lessons_path(project);
    if (path.empty()) return false;

    auto merged = read_lessons(project);
    for (const auto& lesson : lessons) {
        const std::string text = trim(lesson);
        if (text.empty()) continue;
        // A lesson learned twice is not two lessons, and a file of repeats crowds
        // out everything else.
        if (std::find(merged.begin(), merged.end(), text) != merged.end()) continue;
        merged.push_back(text);
    }

    // Newest kept. An old lesson about code that no longer exists is worse than
    // no lesson: it is confidently wrong.
    if (merged.size() > limit) {
        merged.erase(merged.begin(),
                     merged.begin() + static_cast<long>(merged.size() - limit));
    }

    std::ostringstream out;
    for (const auto& lesson : merged) out << lesson << "\n";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return write_atomically(path, out.str());
}

std::vector<std::string> lessons_from(const std::vector<BoardItem>& held) {
    std::vector<std::string> lessons;
    for (const auto& item : held) {
        if (item.reason.empty()) continue;
        // No model call. The Auditor already said why it held this, and that
        // sentence IS the lesson -- asking a model to summarise it would add a
        // failure mode to a step that has none.
        lessons.push_back("A previous change was held: " + item.reason);
    }
    return lessons;
}

std::string lessons_note(const std::filesystem::path& project) {
    const auto lessons = read_lessons(project);
    if (lessons.empty()) return {};

    std::ostringstream out;
    out << "What previous runs on this project got wrong:\n";
    for (const auto& lesson : lessons) out << "  - " << lesson << "\n";
    out << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------
std::vector<CrewPack> builtin_packs() {
    std::vector<CrewPack> packs;

    CrewPack careful;
    careful.name = "careful";
    careful.options.debate  = true;
    careful.options.amplify = 3;
    packs.push_back(careful);

    CrewPack quick;
    quick.name = "quick";
    quick.options.max_subtasks = 1;
    quick.options.parallel     = 1;
    packs.push_back(quick);

    CrewPack tested;
    tested.name = "tested";
    tested.options.coder.allow_run = true;
    packs.push_back(tested);

    CrewPack audit;
    audit.name = "security";
    audit.options.security = true;
    packs.push_back(audit);

    CrewPack learning;
    learning.name = "learning";
    learning.options.learn = true;
    packs.push_back(learning);

    return packs;
}

std::optional<CrewPack> find_pack(const std::string& name) {
    if (name.empty()) return std::nullopt;
    for (const auto& pack : builtin_packs()) {
        if (pack.name == name) return pack;
    }
    return std::nullopt;
}

const std::vector<Faculty>& crew_faculties() {
    // Honest about what this engine has. Debate and the security scan exist in
    // ollamadev and not here; drawing them as faculties would be describing a
    // pipeline that does not run.
    //
    // Dedupe and the secret gate are Always rather than Optional on purpose:
    // overlapping work is held whatever you do, and a leaked credential is never
    // landed. Neither is a switch, so neither is drawn as one.
    static const std::vector<Faculty> kParts{
        {"researcher", "Researcher", "reads the project and indexes it by meaning",
         FacultyState::Always},
        {"director", "Director", "decomposes the task into independent pieces",
         FacultyState::Always},
        {"roles", "Roles", "assigns a persona to each piece", FacultyState::Always},
        {"skills", "Skills", "offers know-how a coder can open on demand",
         FacultyState::Always},
        {"mcp", "External tools", "servers you configured, if any",
         FacultyState::Optional},
        {"coders", "Coders", "build in parallel sandboxes", FacultyState::Always},
        {"run", "Test running", "coders may run tests; off unless allowed",
         FacultyState::Optional},
        {"auditor", "Auditor", "reviews every changeset before it lands",
         FacultyState::Always},
        {"secret", "Secret gate", "never lands a leaked credential",
         FacultyState::Always},
        {"overlap", "Overlap guard", "holds work that would overwrite another's",
         FacultyState::Always},
        {"landing", "Landing", "applies what passed, holds the rest",
         FacultyState::Always},
        {"debate", "Debate", "advocate vs skeptic vs judge, on every changeset",
         FacultyState::Optional},
        {"amplify", "Amplify", "N plans kept by agreement, N reviewers voting",
         FacultyState::Optional},
        {"learn", "Learn", "remembers why work was held, for the next run",
         FacultyState::Optional},
        {"security", "Security scan", "read-only vulnerability hunt; builds nothing",
         FacultyState::Optional},
    };
    return kParts;
}

std::string active_faculty(const CrewRun& run) {
    if (!run.known || !run.active) return {};
    // No plan yet: the Director is still deciding what the pieces are.
    if (run.subtasks.empty()) return "director";

    bool any_doing = false, any_todo = false, all_done = true;
    for (const auto& subtask : run.subtasks) {
        if (subtask.state == "doing") any_doing = true;
        if (subtask.state == "todo")  any_todo  = true;
        if (subtask.state != "done")  all_done  = false;
    }

    if (any_doing) return "coders";
    if (all_done)  return "landing";
    if (any_todo)  return "director";
    // Nothing running, nothing waiting to start, not all finished: what is left is
    // held or being reviewed.
    return "auditor";
}

std::string encode_board(const std::vector<BoardItem>& items) {
    json array = json::array();
    for (const auto& item : items) {
        json entry;
        entry["id"]      = item.id;
        entry["kind"]    = item.kind.empty() ? "crew_branch" : item.kind;
        entry["summary"] = item.summary;
        entry["detail"]  = item.diff;

        json data;
        data["n"]        = item.n;
        data["reason"]   = item.reason;
        data["repoRoot"] = item.repo_root;
        data["files"]    = item.file_names;
        data["store"]    = item.store;
        entry["data"]    = data;

        array.push_back(std::move(entry));
    }
    return array.dump(2);
}

std::vector<BoardItem> read_board() {
    return parse_board(read_whole(auspex_board_path()));
}

bool write_board(const std::vector<BoardItem>& items) {
    return write_atomically(auspex_board_path(), encode_board(items));
}

bool accept_held(int n, std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };

    auto items = read_board();
    const auto held = board_item(items, n);
    if (!held) return fail("change " + std::to_string(n) + " is not on the board");
    if (held->store.empty()) return fail("that change has no saved work");
    if (held->repo_root.empty()) return fail("that change does not say where it belongs");

    const Changeset changeset = load_changeset(held->store);
    if (changeset.empty()) return fail("the saved work is missing or unreadable");

    std::string apply_error;
    if (!apply_changeset(changeset, held->repo_root, nullptr, &apply_error)) {
        return fail(apply_error.empty() ? "could not apply the change" : apply_error);
    }

    // Removed only AFTER it landed. A board entry dropped before the write means a
    // failed apply loses the work with nothing left to retry from.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [n](const BoardItem& i) { return i.n == n; }),
                items.end());
    write_board(items);
    return true;
}

bool discard_held(int n, std::string* error) {
    auto items = read_board();
    if (!board_item(items, n)) {
        if (error) *error = "change " + std::to_string(n) + " is not on the board";
        return false;
    }

    const auto store = board_item(items, n)->store;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [n](const BoardItem& i) { return i.n == n; }),
                items.end());
    write_board(items);

    // The stored work goes too. Discard is the one decision with no undo, and
    // leaving the blobs behind would quietly fill the data directory with
    // changesets nobody can reach.
    if (!store.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(store, ec);
    }
    return true;
}

// ---------------------------------------------------------------------------
namespace {

// What one coder produced, carried from its thread back to the main one.
struct Attempt {
    PlannedSubtask subtask;
    CoderOutcome   outcome;
    Changeset      changeset;
    Audit          audit;
    std::string    state;   // the word written into the state file
};

std::string encode_state(const std::string& run_id, const std::string& task,
                         bool active, const std::vector<Attempt>& attempts) {
    json document;
    document["runId"]  = run_id;
    document["task"]   = task;
    document["active"] = active;
    document["ts"]     = now_stamp();

    json subtasks = json::array();
    for (const auto& attempt : attempts) {
        json entry;
        entry["n"]     = attempt.subtask.n;
        entry["role"]  = attempt.subtask.role;
        entry["title"] = attempt.subtask.title;
        entry["state"] = attempt.state;
        entry["model"] = attempt.outcome.model;
        entry["backend"] = attempt.outcome.model.empty() ? "ollama"
                                                       : attempt.outcome.model;
        entry["route"] = "";
        subtasks.push_back(std::move(entry));
    }
    document["subtasks"] = std::move(subtasks);

    return document.dump(2);
}

}  // namespace

namespace {

// Land or hold one attempt. Shared by run and resume, because "what happens to a
// changeset" is the one decision that must not differ between them.
void land_or_hold(const std::string& run_id, const std::filesystem::path& project,
                  Attempt& attempt, Changeset& landed, std::vector<BoardItem>& board,
                  int& next_number, RunResult& result,
                  const std::function<void(const std::string&)>& note) {
    if (attempt.changeset.empty()) return;

    std::string reason = attempt.audit.reason;
    bool        hold   = attempt.audit.held();

    if (!hold) {
        if (const auto shared = overlapping_files(landed, attempt.changeset);
            !shared.empty()) {
            hold   = true;
            reason = "overlaps another coder on " + shared.front();
            if (shared.size() > 1) {
                reason += " (and " + std::to_string(shared.size() - 1) + " more)";
            }
            attempt.state = "held";
        }
    }

    if (!hold) {
        std::string error;
        if (apply_changeset(attempt.changeset, project, nullptr, &error)) {
            landed.files.insert(landed.files.end(), attempt.changeset.files.begin(),
                                attempt.changeset.files.end());
            ++result.applied;
            if (note) note("applied #" + std::to_string(attempt.subtask.n));
            return;
        }
        hold   = true;
        reason = error.empty() ? "could not be applied" : error;
        attempt.state = "held";
    }

    const auto store = changeset_store(run_id, attempt.subtask.n);
    save_changeset(store, attempt.changeset);

    BoardItem item;
    item.n         = ++next_number;
    item.id        = run_id + "_" + std::to_string(attempt.subtask.n);
    item.kind      = "crew_branch";
    item.summary   = "coder #" + std::to_string(attempt.subtask.n) + " — " +
                     attempt.subtask.title;
    item.reason    = reason;
    item.diff      = attempt.changeset.diff;
    item.repo_root = project.string();
    item.store     = store.string();
    item.files     = static_cast<int>(attempt.changeset.files.size());
    for (const auto& file : attempt.changeset.files) {
        item.file_names.push_back(file.path);
    }
    board.push_back(std::move(item));
    ++result.held;
}

}  // namespace

RunResult resume_crew(const Config& config, const std::filesystem::path& project,
                      const std::string& run_id, const RunEvents& events) {
    RunResult result;

    const auto note = [&events](const std::string& text) {
        if (events.log) events.log(text);
    };

    if (!is_project_dir(project)) {
        result.error = "no such project";
        return result;
    }

    result.run_id = run_id;
    if (result.run_id.empty()) {
        const auto runs = resumable_runs();
        if (runs.empty()) {
            result.error = "there is no interrupted run to resume";
            return result;
        }
        result.run_id = runs.front();
    }

    const auto run_dir = auspex_crew_dir() / result.run_id;
    std::error_code ec;
    if (!std::filesystem::is_directory(run_dir, ec)) {
        result.error = "no such run: " + result.run_id;
        return result;
    }

    // The plan, from the state file, so the pieces keep their titles and roles --
    // the Auditor is asked whether the work matches what was ASKED for, and
    // without the original subtask it has nothing to compare against.
    const CrewRun previous = current_crew_run(auspex_run_state_path());

    std::vector<std::filesystem::path> sandboxes;
    for (const auto& child : std::filesystem::directory_iterator(run_dir, ec)) {
        if (child.is_directory(ec) &&
            child.path().filename().string().rfind("sandbox-", 0) == 0) {
            sandboxes.push_back(child.path());
        }
    }
    std::sort(sandboxes.begin(), sandboxes.end());

    if (sandboxes.empty()) {
        result.error = "that run left nothing to recover";
        return result;
    }
    note("resume: recovering " + std::to_string(sandboxes.size()) +
         " interrupted coder(s)");

    auto board = read_board();
    int  next_number = 0;
    for (const auto& item : board) next_number = std::max(next_number, item.n);

    Changeset landed;

    for (const auto& sandbox : sandboxes) {
        const std::string name = sandbox.filename().string();
        const int n = std::atoi(name.substr(std::string("sandbox-").size()).c_str());

        Attempt attempt;
        attempt.subtask.n     = n;
        attempt.subtask.role  = "coder";
        attempt.subtask.title = "recovered work";
        for (const auto& s : previous.subtasks) {
            if (s.n != n) continue;
            attempt.subtask.role  = s.role;
            attempt.subtask.title = s.title;
            break;
        }

        attempt.changeset = capture_changeset(project, sandbox);
        if (attempt.changeset.empty()) {
            note("#" + std::to_string(n) + " had written nothing");
            destroy_sandbox(sandbox);
            continue;
        }

        // Still audited. Work interrupted mid-thought is MORE likely to be
        // half-finished, not less, so skipping the review here would be exactly
        // backwards.
        attempt.audit = audit_changeset(config, attempt.subtask, attempt.changeset,
                                        AuditLimits{}, config.crew_auditor_model);

        land_or_hold(result.run_id, project, attempt, landed, board, next_number,
                     result, note);
        destroy_sandbox(sandbox);
    }

    write_board(board);
    note("done: " + std::to_string(result.applied) + " applied · " +
         std::to_string(result.held) + " held");
    return result;
}

RunResult run_crew(const Config& config, const RunOptions& options,
                   const RunEvents& events, const std::atomic<bool>* cancel) {
    RunResult result;
    result.run_id = "crew_" + now_stamp();

    const auto note = [&events](const std::string& text) {
        if (events.log) events.log(text);
    };
    const auto stopped = [cancel] {
        return cancel && cancel->load();
    };

    if (!is_project_dir(options.project)) {
        result.error = "no such project";
        return result;
    }

    // A different run entirely: it reads and reports, and never builds. Routed
    // here rather than being a flag inside the build path, so there is no way for
    // a scan to reach the code that writes files.
    if (options.security) return scan_security(config, options, events, cancel);

    if (trim(options.task).empty()) {
        result.error = "there is no task";
        return result;
    }

    std::vector<Attempt> attempts;
    std::mutex           state_mutex;

    const auto publish = [&](bool active) {
        std::lock_guard lock(state_mutex);
        write_atomically(auspex_run_state_path(),
                         encode_state(result.run_id, options.task, active, attempts));
        if (events.changed) events.changed();
    };

    // ---- plan ----
    note("research: reading the project");
    const std::vector<std::string> files = list_file_names(options.project);

    publish(/*active=*/true);
    note("plan: the Director is deciding what the pieces are");

    // What the index thinks is relevant, when there is one. Silent when there is
    // not: an unindexed project must still be plannable.
    // MCP servers are started ONCE for the run and shared. Starting every
    // configured server per coder, or per turn, would be absurd -- and the tools
    // do not change while a crew works.
    //
    // Serialised on a mutex: coders run on several threads and each client owns
    // one pipe, which is not safe to interleave. MCP calls are rare next to model
    // calls, so the contention is nothing.
    std::vector<std::unique_ptr<McpClient>> mcp_clients;
    std::vector<std::string> mcp_problems;
    McpAccess mcp;
    {
        for (const auto& server : load_mcp_servers()) {
            auto client = std::make_unique<McpClient>(server);
            std::string error;
            if (!client->start(&error)) {
                mcp_problems.push_back(server.name + ": " + error);
                continue;
            }
            for (auto& tool : client->tools()) mcp.tools.push_back(std::move(tool));
            mcp_clients.push_back(std::move(client));
        }
        for (const auto& problem : mcp_problems) note("mcp: " + problem);

        if (!mcp.tools.empty()) {
            auto* clients = &mcp_clients;
            auto  guard   = std::make_shared<std::mutex>();
            mcp.call = [clients, guard](const std::string& qualified,
                                        const std::string& arguments)
                -> std::pair<bool, std::string> {
                const auto dot = qualified.find('.');
                if (dot == std::string::npos) return {false, "not a server.tool name"};
                const std::string server = qualified.substr(0, dot);
                const std::string tool   = qualified.substr(dot + 1);

                std::lock_guard lock(*guard);
                for (auto& client : *clients) {
                    if (client->config().name != server) continue;
                    bool ok = false;
                    const std::string text = client->call(tool, arguments, &ok);
                    return {ok, text};
                }
                return {false, "no such server: " + server};
            };
            note("mcp: " + std::to_string(mcp.tools.size()) + " tool(s) available");
        }
    }

    // What earlier runs got wrong, put in front of every coder. Read whether or
    // not `learn` is on: learning is about WRITING them, and refusing to read
    // what is already there would make the switch retroactive.
    const std::string lessons = lessons_note(options.project);
    if (!lessons.empty()) note("research: applying lessons from earlier runs");

    // Discovered once for the run, not per coder: the set does not change while a
    // crew works, and re-walking two directories on every turn would be waste.
    SkillSet skills;
    skills.skills  = all_skills(options.project);
    skills.catalog = skills_catalog(skills.skills);
    if (!skills.empty()) {
        note("research: " + std::to_string(skills.skills.size()) + " skill(s) available");
    }

    const std::string hint =
        relevant_files_note(config, options.project, options.task);
    if (!hint.empty()) note("research: the index suggests where to look");

    if (options.amplify > 1) {
        note("plan: " + std::to_string(options.amplify) +
             " plans, keeping the shape most of them agree on");
    }
    Plan plan;
    if (is_cli_backend(options.director_backend)) {
        // The same prompt, answered by an agent CLI instead of Ollama. Run in the
        // project so it can look around if it wants to; it is planning, not
        // editing, and the Auditor still reads whatever the coders produce.
        note("plan: asking " + options.director_backend);
        const std::string reply =
            ask_cli(options.director_backend, options.model_for("director"),
                    director_prompt(options.task, files, options.max_subtasks, hint),
                    options.project);
        plan = parse_plan(reply, options.max_subtasks);
    } else if (options.amplify > 1) {
        plan = plan_amplified(config, options.task, files, options.max_subtasks,
                              options.amplify, options.model_for("director"), hint);
    } else {
        plan = plan_task(config, options.task, files, options.max_subtasks,
                         options.model_for("director"), hint);
    }
    if (!plan.ok()) {
        result.error = plan.error;
        publish(/*active=*/false);
        return result;
    }

    for (const auto& subtask : plan.subtasks) {
        PlannedSubtask piece = subtask;
        // Carried in the detail rather than as another prompt argument. The coder
        // prompt already renders detail prominently, and threading a fifth string
        // through every signature to say the same thing would be worse.
        if (!lessons.empty()) {
            piece.detail = piece.detail.empty() ? lessons
                                                : piece.detail + "\n\n" + lessons;
        }
        attempts.push_back({piece, {}, {}, {}, "todo"});
    }
    publish(/*active=*/true);
    note("build: " + std::to_string(attempts.size()) + " coders");

    if (stopped()) {
        result.error = "cancelled";
        publish(false);
        return result;
    }

    // ---- code, in parallel ----
    //
    // A slot per running coder rather than a thread per subtask: a plan of eight
    // on a four-core machine would otherwise put eight models in flight at once,
    // and Ollama serialises them anyway while the memory pressure is real.
    const int parallel = std::max(1, std::min(options.parallel,
                                              static_cast<int>(attempts.size())));
    std::atomic<std::size_t> next{0};

    const auto worker = [&] {
        for (;;) {
            const std::size_t index = next.fetch_add(1);
            if (index >= attempts.size()) return;
            if (stopped()) return;

            Attempt& attempt = attempts[index];
            {
                std::lock_guard lock(state_mutex);
                attempt.state = "doing";
            }
            publish(true);

            const auto sandbox = auspex_crew_dir() / result.run_id /
                                 ("sandbox-" + std::to_string(attempt.subtask.n));
            std::string error;
            if (!create_sandbox(options.project, sandbox, &error)) {
                attempt.outcome.error = "could not make a sandbox: " + error;
                attempt.state = "done";
                publish(true);
                continue;
            }

            // Somebody else's agent, or ours. Everything AFTER this line is the
            // same either way -- the changeset is captured from the sandbox, the
            // Auditor reviews it, the overlap guard and secret gate apply. Handing
            // the work to a stronger agent does not hand it the project.
            const std::string backend =
                options.backend_for_coder(attempt.subtask.n);
            if (is_cli_backend(backend)) {
                attempt.outcome = run_cli_coder(config, attempt.subtask, sandbox,
                                                backend, options.model_for("coder"),
                                                /*timeout_seconds=*/900, lessons);
            } else {
                attempt.outcome = run_coder(config, attempt.subtask, sandbox,
                                            options.coder, options.model_for("coder"),
                                            steer_mailbox(result.run_id,
                                                          attempt.subtask.n),
                                            skills, mcp);
            }
            attempt.changeset = capture_changeset(options.project, sandbox);

            // Audited on the worker thread. It is another model call, and doing it
            // here means a slow audit of one piece does not hold up the coder on
            // the next.
            // Which review, in order of cost. Debate is three calls; a panel is
            // `amplify` calls; the plain Auditor is one.
            if (is_cli_backend(options.auditor_backend)) {
                // Certain checks first, as always -- there is nothing an agent
                // could say that makes a leaked credential acceptable.
                attempt.audit = deterministic_audit(attempt.changeset, options.audit);
                if (!attempt.audit.held()) {
                    const std::string reply = ask_cli(
                        options.auditor_backend, options.model_for("auditor"),
                        auditor_prompt(attempt.subtask, attempt.changeset,
                                       options.audit),
                        options.project);
                    attempt.audit = parse_audit(reply);
                    if (attempt.audit.held() && !attempt.audit.quote.empty() &&
                        !quote_is_real(attempt.audit.quote, attempt.changeset.diff)) {
                        attempt.audit.notes.push_back(
                            "the Auditor quoted a line that is not in this diff, so "
                            "its reason may be invented: \"" + attempt.audit.quote +
                            "\"");
                    }
                }
            } else if (options.debate) {
                attempt.audit = debate_changeset(config, attempt.subtask,
                                                 attempt.changeset, options.audit,
                                                 options.model_for("auditor"));
            } else if (options.amplify > 1) {
                attempt.audit = audit_panel(config, attempt.subtask, attempt.changeset,
                                            options.amplify, options.audit,
                                            options.model_for("auditor"));
            } else {
                attempt.audit = audit_changeset(config, attempt.subtask,
                                                attempt.changeset, options.audit,
                                                options.model_for("auditor"));
            }

            {
                std::lock_guard lock(state_mutex);
                attempt.state = attempt.audit.held() ? "held" : "done";
            }
            publish(true);

            destroy_sandbox(sandbox);
        }
    };

    {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(parallel));
        for (int i = 0; i < parallel; ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }

    if (stopped()) {
        result.error = "cancelled";
        publish(false);
        return result;
    }

    // ---- land ----
    //
    // Serial, and deliberately so. Two coders that edited one file are both
    // "accepted" and only one can win; deciding that on a thread would make which
    // one depends on timing.
    note("land: applying what the Auditor passed");

    auto board = read_board();
    int  next_number = 0;
    for (const auto& item : board) next_number = std::max(next_number, item.n);

    Changeset landed;   // everything applied so far, for overlap detection

    for (auto& attempt : attempts) {
        land_or_hold(result.run_id, options.project, attempt, landed, board,
                     next_number, result, events.log);
    }

    write_board(board);

    if (options.learn) {
        // Only what THIS run held. The Auditor's reason is the lesson; nothing is
        // asked of a model, so this step cannot itself be wrong in a new way.
        std::vector<BoardItem> mine;
        for (const auto& item : board) {
            if (item.id.rfind(result.run_id, 0) == 0) mine.push_back(item);
        }
        if (const auto learned = lessons_from(mine); !learned.empty()) {
            write_lessons(options.project, learned);
            note("learn: remembered " + std::to_string(learned.size()) + " lesson(s)");
        }
    }

    publish(/*active=*/false);

    note("done: " + std::to_string(result.applied) + " applied · " +
         std::to_string(result.held) + " held");
    return result;
}

}  // namespace auspex
