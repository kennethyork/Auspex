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
#include "auspex/roles.hpp"
#include "auspex/router.hpp"
#include "auspex/symbols.hpp"
#include "auspex/gitflow.hpp"
#include "auspex/verify.hpp"
#include "auspex/json_util.hpp"

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
        // Persisted, or a held change reloaded after a restart would land with no
        // baseline to check and silently overwrite whatever had happened since.
        entry["base"]    = file.base_fingerprint;
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

    if (!write_atomically(dir / "manifest.json", safe_dump(manifest, 2))) {
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
        if (entry.contains("base") && entry["base"].is_number_unsigned()) {
            file.base_fingerprint = entry["base"].get<std::uint64_t>();
        }
        // A manifest written before "base" existed leaves it 0, which
        // apply_changeset reads as "no baseline" and lets through -- an upgrade
        // must not make work already on the board unlandable.
        changeset.files.push_back(std::move(file));
    }
    return changeset;
}

// ---------------------------------------------------------------------------
const std::vector<CrewRole>& configurable_roles() {
    static const std::vector<CrewRole> kRoles{
        {"researcher", "Researcher", "reads the project before anything is planned", ""},
        {"director",   "Director",   "decomposes the task into pieces", ""},
        {"coder",      "Coders",     "build in parallel sandboxes", ""},
        {"auditor",    "Auditor",    "reviews every changeset", ""},
        // The debate voices fall back to the Auditor, because that is the job they
        // are doing -- an unset advocate should review like the Auditor does, not
        // like whatever the chat model happens to be.
        {"advocate",   "Advocate",   "argues for landing a change", "auditor"},
        {"skeptic",    "Skeptic",    "argues against landing it", "auditor"},
        {"judge",      "Judge",      "rules on the two arguments", "auditor"},
        {"security",   "Security",   "hunts vulnerabilities, read-only", "auditor"},
    };
    return kRoles;
}

std::string RunOptions::backend_for(const std::string& role) const {
    // The role's own setting, then whatever it falls back to, then the run-wide
    // one. Walked rather than hard-coded so a new role needs no new branch.
    std::string key = role;
    for (int hop = 0; hop < 4 && !key.empty(); ++hop) {
        if (const auto found = role_backends.find(key);
            found != role_backends.end() && !found->second.empty()) {
            return found->second;
        }
        if (key == "director") return director_backend;
        if (key == "auditor")  return auditor_backend;
        if (key == "coder")    return coder_backend;
        if (key == "researcher") return researcher_backend;

        std::string next;
        for (const auto& entry : configurable_roles()) {
            if (entry.key == key) { next = entry.fallback; break; }
        }
        key = next;
    }
    return "ollama";
}

std::string RunOptions::backend_for_coder(int n) const {
    if (coder_backends.empty()) return coder_backend;
    // Round-robin, 1-based. A list shorter than the plan repeats rather than
    // running out -- two coders on claude is a sensible thing to ask for.
    const std::size_t index =
        static_cast<std::size_t>(n > 0 ? n - 1 : 0) % coder_backends.size();
    const std::string picked = coder_backends[index];
    return picked.empty() ? coder_backend : picked;
}

// The config's per-role models and backends, folded into whatever the caller
// asked for.
//
// ONE PLACE, on the way in, rather than at each call site. The GUI copied
// config.crew_role_models into RunOptions and every other caller did not, so a
// crew started any other way silently ignored the per-role models the user had
// set -- measured on a live run pinned to a different Auditor that never ran.
// resume_crew had grown its own inline lookup for the Auditor alone, which is the
// same knowledge in a second place and already disagreeing with the first.
//
// What the caller passed WINS. This fills gaps; it does not overrule an explicit
// choice, for the same reason the Router does not.
RunOptions with_config_roles(const Config& config, RunOptions options) {
    for (const auto& [role, model] : config.crew_role_models) {
        if (model.empty()) continue;
        if (options.role_models.find(role) == options.role_models.end()) {
            options.role_models[role] = model;
        }
    }
    for (const auto& [role, backend] : config.crew_role_backends) {
        if (backend.empty()) continue;
        if (options.role_backends.find(role) == options.role_backends.end()) {
            options.role_backends[role] = backend;
        }
    }
    return options;
}

int RunOptions::role_limit(const std::string& role) const {
    const auto found = role_limits.find(role);
    return found == role_limits.end() ? -1 : found->second;
}

bool RunOptions::role_allowed(const std::string& role) const {
    // An ABSENT limit is unlimited, not zero. A map nobody has filled in must not
    // silently mean the crew can do nothing -- that is the failure mode of every
    // allowlist that defaults to empty.
    return role_limit(role) != 0;
}

std::vector<std::string> RunOptions::offered_roles() const {
    std::vector<std::string> offered;
    for (const auto& persona : all_personas()) {
        if (role_allowed(persona.name)) offered.push_back(persona.name);
    }
    return offered;
}

std::string RunOptions::model_for(const std::string& role) const {
    // The role's own setting, then whatever it falls back to, then the run-wide
    // model, then (by returning empty) the config's. Walked rather than
    // hard-coded, so adding a role is a row in one table.
    std::string key = role;
    for (int hop = 0; hop < 4 && !key.empty(); ++hop) {
        if (const auto found = role_models.find(key);
            found != role_models.end() && !found->second.empty()) {
            return found->second;
        }
        if (key == "researcher" && !researcher_model.empty()) return researcher_model;
        if (key == "director"   && !director_model.empty())   return director_model;
        if (key == "auditor"    && !auditor_model.empty())    return auditor_model;
        if (key == "coder"      && !coder_model.empty())      return coder_model;

        std::string next;
        for (const auto& entry : configurable_roles()) {
            if (entry.key == key) { next = entry.fallback; break; }
        }
        key = next;
    }
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

RunResult scan_security(const Config& config, const RunOptions& requested,
                        const RunEvents& events, const std::atomic<bool>* cancel,
                        std::vector<Finding>* out_findings) {
    const RunOptions options = with_config_roles(config, requested);

    RunResult result;
    result.run_id = "scan_" + now_stamp();

    // Metered here rather than by run_crew, which returns this call's result
    // directly and so never reaches its own meter.
    const auto usage_before = usage_snapshot();
    const auto finish = [&usage_before](RunResult r) {
        r.usage = usage_since(usage_before);
        return r;
    };

    const auto note = [&events](const std::string& text) {
        if (events.log) events.log(text);
    };
    const auto stopped = [cancel] { return cancel && cancel->load(); };

    if (!is_project_dir(options.project)) {
        result.error = "no such project";
        return finish(result);
    }
    // A directory is not a project. See unsafe_project_reason(): pointed at $HOME
    // this would copy the tree once per coder and could land a change in it.
    if (const std::string why = unsafe_project_reason(options.project); !why.empty()) {
        result.error = why;
        return finish(result);
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

    const std::string model = options.model_for("security");
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
    return finish(result);
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

    // "tested" already existed here meaning "coders may run tests", and ollamadev
    // has one of the same name meaning "work test-first". They are the same
    // intention stated at two levels, so they are merged rather than shipped as
    // two packs a person has to choose between.
    CrewPack tested;
    tested.name = "tested";
    tested.options.coder.allow_run = true;
    tested.options.verify_attempts = 2;
    tested.options.focus =
        "build with test-first discipline -- every change covered by a test that "
        "runs green; do not finish with failing or missing tests";
    packs.push_back(tested);

    CrewPack audit;
    audit.name = "security";
    audit.options.security = true;
    packs.push_back(audit);

    CrewPack learning;
    learning.name = "learning";
    learning.options.learn = true;
    packs.push_back(learning);

    // --- what kind of thing is being built --------------------------------------
    //
    // A SECOND AXIS. The five above say how the crew should WORK -- debate it,
    // keep it to one coder, let it run tests. These say what it is WORKING ON,
    // which changes what a sensible plan looks like and what the coders should
    // already know. Ported from ollamadev-qt, where they are the packs people
    // actually reach for.
    //
    // A pack sets `focus`, and focus does two things: the Director plans against
    // it, and the project-type starters are matched on it.
    const auto project = [&packs](const char* name, const char* focus,
                                  int amplify = 0) {
        CrewPack pack;
        pack.name = name;
        pack.options.focus = focus;
        // Where an adversarial panel pays for itself. A bugfix and a refactor are
        // both "change as little as possible", which is exactly the judgement one
        // reviewer is worst at and several are best at.
        pack.options.amplify = amplify;
        packs.push_back(pack);
    };

    project("web-app",
            "a web application -- an HTML/CSS/JS frontend plus its backend; "
            "prioritise a working UI, sensible routing, and a clean separation of "
            "concerns");
    project("rest-api",
            "a REST API -- clear resource endpoints, input validation, consistent "
            "error responses, and a test for each route");
    project("cli-tool",
            "a command-line tool -- argument parsing, a helpful --help, clear error "
            "messages, and correct exit codes");
    project("data-pipeline",
            "a data-processing pipeline -- robust parsing, transformation, "
            "validation, and explicit handling of malformed or edge-case input");
    project("library",
            "a reusable library or package -- a small clear public API, "
            "documentation on it, no side effects on import, and unit tests");
    project("bugfix",
            "find and fix the bug with the smallest correct change, then add a "
            "regression test that fails before the fix and passes after",
            /*amplify=*/3);
    project("refactor",
            "refactor for clarity and structure WITHOUT changing behaviour; keep "
            "the public API stable and the diff reviewable",
            /*amplify=*/3);

    return packs;
}

std::vector<std::filesystem::path> crew_pack_dirs() {
    std::vector<std::filesystem::path> dirs;
    if (const auto home = data_home(); !home.empty()) dirs.push_back(home / "crew-packs");
    // ollamadev's. Somebody with packs saved there has already said what team they
    // want; not reading them would mean shipping fewer packs than the thing this
    // replaces.
    if (const char* home = std::getenv("HOME"); home && *home) {
        dirs.push_back(std::filesystem::path(home) / ".ollamadev" / "crew-packs");
    }
    return dirs;
}

std::optional<CrewPack> parse_pack(const std::string& name,
                                   const std::string& json_text) {
    const json doc = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
    if (trim(name).empty()) return std::nullopt;

    CrewPack pack;
    pack.name = name;

    // Both spellings. The files on disk are ollamadev's, so theirs is not a
    // fallback -- it is the common case.
    const auto text = [&doc](std::initializer_list<const char*> keys) -> std::string {
        for (const char* key : keys) {
            if (doc.contains(key) && doc[key].is_string()) {
                return trim(doc[key].get<std::string>());
            }
        }
        return {};
    };
    const auto number = [&doc](std::initializer_list<const char*> keys, int fallback) {
        for (const char* key : keys) {
            if (doc.contains(key) && doc[key].is_number_integer()) {
                return doc[key].get<int>();
            }
        }
        return fallback;
    };
    const auto flag = [&doc](std::initializer_list<const char*> keys, bool fallback) {
        for (const char* key : keys) {
            if (doc.contains(key) && doc[key].is_boolean()) {
                return doc[key].get<bool>();
            }
        }
        return fallback;
    };

    pack.options.focus        = text({"focus"});
    pack.options.max_subtasks = number({"max", "max_subtasks"}, pack.options.max_subtasks);
    pack.options.amplify      = number({"amplify"}, pack.options.amplify);
    pack.options.route        = flag({"route"}, pack.options.route);
    pack.options.debate       = flag({"debate"}, pack.options.debate);
    pack.options.learn        = flag({"learn"}, pack.options.learn);
    pack.options.research     = flag({"research"}, pack.options.research);
    pack.options.security     = flag({"security"}, pack.options.security);
    pack.options.verify_attempts =
        number({"verify", "verify_attempts"}, pack.options.verify_attempts);
    if (pack.options.verify_attempts > 0) pack.options.coder.allow_run = true;

    // Per-role models and backends, under either spelling.
    for (const auto& role : configurable_roles()) {
        const std::string camel = role.key;
        std::string Camel = camel;
        if (!Camel.empty()) Camel[0] = static_cast<char>(std::toupper(Camel[0]));

        if (const auto model =
                text({(camel + "Model").c_str(), (camel + "_model").c_str()});
            !model.empty()) {
            pack.options.role_models[role.key] = model;
        }
        if (const auto backend =
                text({(camel + "Backend").c_str(), (camel + "_backend").c_str()});
            !backend.empty()) {
            pack.options.role_backends[role.key] = backend;
        }
    }

    // Keys with no equivalent here -- skills, hosts, land -- are ignored rather
    // than refused: a pack that mentions something Auspex does not have should
    // still bring across the parts it does.
    return pack;
}

std::vector<CrewPack> user_packs() {
    std::vector<CrewPack> packs;
    std::error_code ec;

    for (const auto& dir : crew_pack_dirs()) {
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) continue;

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());   // same order twice

        for (const auto& file : files) {
            const std::string name = file.stem().string();
            // The FIRST directory wins, so an Auspex pack shadows an ollamadev one
            // of the same name rather than the other way round.
            if (std::any_of(packs.begin(), packs.end(),
                            [&](const CrewPack& p) { return p.name == name; })) {
                continue;
            }
            if (auto parsed = parse_pack(name, read_whole(file))) {
                packs.push_back(std::move(*parsed));
            }
        }
    }
    return packs;
}

std::vector<CrewPack> all_packs() {
    std::vector<CrewPack> packs = builtin_packs();
    for (auto& saved : user_packs()) {
        const auto at = std::find_if(packs.begin(), packs.end(),
                                     [&](const CrewPack& p) { return p.name == saved.name; });
        // A saved pack of the same name wins: the more specific one decides.
        if (at != packs.end()) *at = std::move(saved);
        else packs.push_back(std::move(saved));
    }
    return packs;
}

std::optional<CrewPack> find_pack(const std::string& name) {
    if (name.empty()) return std::nullopt;
    for (const auto& pack : all_packs()) {
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
        {"researcher", "Researcher",
         "reads the project read-only and reports what a team needs to know",
         FacultyState::Always},
        {"router", "Router", "picks a model per piece by how hard it looks",
         FacultyState::Optional},
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
        // Guards, not stages. Neither can be turned off and neither is a step the
        // run passes through -- they are refusals that sit across the landing
        // pass, which is why they are drawn apart from the rest.
        {"secret", "Secret gate", "never lands a leaked credential",
         FacultyState::Guard},
        {"overlap", "Overlap guard", "first writer wins on a shared file",
         FacultyState::Guard},
        {"landing", "Landing", "applies what passed, holds the rest",
         FacultyState::Always},
        {"debate", "Debate", "advocate vs skeptic vs judge, on every changeset",
         FacultyState::Optional},
        {"amplify", "Amplify", "N plans kept by agreement, N reviewers voting",
         FacultyState::Optional},
        {"memory", "Memory", "remembers why work was held, for the next run",
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
    return safe_dump(array, 2);
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

    // What this coder is doing right now, and what it has changed so far.
    //
    // Written from the coder's own thread as each step finishes, read on the GTK
    // thread. Guarded by the same state_mutex everything else in this file uses.
    std::string    activity;
    int            added   = 0;
    int            removed = 0;
};

std::string encode_state(const std::string& run_id, const std::string& task,
                         bool active, const std::vector<Attempt>& attempts,
                         const std::string& phase, const RunOptions* options) {
    json document;
    document["runId"]  = run_id;
    document["task"]   = task;
    document["active"] = active;
    document["ts"]     = now_stamp();
    // WHICH FACULTY IS WORKING, not just which coder.
    //
    // The run view could only ever show coders, because coders were the only thing
    // in this file. The Researcher, the Director and the Auditor all did work and
    // none of them appeared anywhere -- a crew of five that reads as a crew of one.
    document["phase"]  = phase;

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
        entry["activity"] = attempt.activity;
        entry["added"]    = attempt.added;
        entry["removed"]  = attempt.removed;
        subtasks.push_back(std::move(entry));
    }
    document["subtasks"] = std::move(subtasks);

    // WHO REVIEWS, and how many of them. A debate is three named voices and a
    // panel is N Auditors; both were invisible, so a run with either read as
    // having one reviewer -- and the switch you paid three model calls for looked
    // exactly like the switch you did not turn on.
    if (options) {
        document["debate"]  = options->debate;
        document["amplify"] = options->amplify;
        document["verify"]  = options->verify_attempts > 0;
    }

    return safe_dump(document, 2);
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

    // Resuming is cheap but not free: no coder is restarted, and every recovered
    // sandbox is still audited. That is a model call each, so it is metered like
    // any other.
    const auto usage_before = usage_snapshot();
    const auto finish = [&usage_before](RunResult r) {
        r.usage = usage_since(usage_before);
        return r;
    };

    const auto note = [&events](const std::string& text) {
        if (events.log) events.log(text);
    };

    if (!is_project_dir(project)) {
        result.error = "no such project";
        return finish(result);
    }

    result.run_id = run_id;
    if (result.run_id.empty()) {
        const auto runs = resumable_runs();
        if (runs.empty()) {
            result.error = "there is no interrupted run to resume";
            return finish(result);
        }
        result.run_id = runs.front();
    }

    const auto run_dir = auspex_crew_dir() / result.run_id;
    std::error_code ec;
    if (!std::filesystem::is_directory(run_dir, ec)) {
        result.error = "no such run: " + result.run_id;
        return finish(result);
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
        return finish(result);
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
                                        AuditLimits{},
                                        with_config_roles(config, {}).model_for("auditor"));

        land_or_hold(result.run_id, project, attempt, landed, board, next_number,
                     result, note);
        destroy_sandbox(sandbox);
    }

    write_board(board);
    note("done: " + std::to_string(result.applied) + " applied · " +
         std::to_string(result.held) + " held");
    return finish(result);
}

RunResult run_crew(const Config& config, const RunOptions& requested,
                   const RunEvents& events, const std::atomic<bool>* cancel) {
    // The user's per-role settings apply however this run was started.
    const RunOptions options = with_config_roles(config, requested);

    RunResult result;
    result.run_id = "crew_" + now_stamp();

    // The meter reading before anything is spent. Every exit below runs through
    // `finish`, so a run that fails early still reports what it cost getting
    // there -- which is usually a Director call, and is not nothing.
    const auto usage_before = usage_snapshot();
    const auto finish = [&usage_before](RunResult r) {
        r.usage = usage_since(usage_before);
        return r;
    };

    const auto note = [&events](const std::string& text) {
        if (events.log) events.log(text);
    };
    const auto stopped = [cancel] {
        return cancel && cancel->load();
    };

    if (!is_project_dir(options.project)) {
        result.error = "no such project";
        return finish(result);
    }
    // A directory is not a project. See unsafe_project_reason(): pointed at $HOME
    // this would copy the tree once per coder and could land a change in it.
    if (const std::string why = unsafe_project_reason(options.project); !why.empty()) {
        result.error = why;
        return finish(result);
    }

    // A different run entirely: it reads and reports, and never builds. Routed
    // here rather than being a flag inside the build path, so there is no way for
    // a scan to reach the code that writes files.
    if (options.security) return scan_security(config, options, events, cancel);

    if (trim(options.task).empty()) {
        result.error = "there is no task";
        return finish(result);
    }

    std::vector<Attempt> attempts;
    std::mutex           state_mutex;

    std::string phase;
    const auto publish = [&](bool active) {
        std::lock_guard lock(state_mutex);
        write_atomically(auspex_run_state_path(),
                         encode_state(result.run_id, options.task, active, attempts,
                                      phase, &options));
        if (events.changed) events.changed();
    };
    // Sets the phase and says it in one go, so the two can never disagree -- the
    // log line and the roster are the same fact told twice.
    const auto enter = [&](const std::string& name, const std::string& said) {
        phase = name;
        note(said);
        publish(/*active=*/true);
    };

    // ---- plan ----
    enter("research", "research: reading the project");
    const std::vector<std::string> files = list_file_names(options.project);

    publish(/*active=*/true);
    enter("plan", "plan: the Director is deciding what the pieces are");

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
    skills.skills = all_skills(options.project);

    // The shipped starters, matched against what this run was actually asked to
    // do. Skills used to help only if you had already written some, which meant
    // the feature did nothing on a fresh install -- and the coders that need
    // instruction most are the small local ones whose owners have not yet written
    // a house style document.
    //
    // Written into the PROJECT's .auspex/skills, so they persist and can be
    // edited: a starter you disagree with should be changeable, and the next run
    // will leave your version alone. Anything you already had wins outright.
    if (options.starter_skills) {
        // Two axes, matched against two different things. The capability starters
        // answer "what does this TASK need"; the project starters answer "what
        // does this KIND OF PROJECT need", which the task never says.
        auto matched = skills_for_focus(options.task);
        for (const auto& spec : project_starters_for(options.focus)) {
            matched.push_back(spec);
        }
        const auto written =
            materialize_skills(matched, options.project, skills.skills);
        if (!written.empty()) {
            // Re-read, so the ones just written are discovered exactly like any
            // other project skill rather than by a second code path.
            skills.skills = all_skills(options.project);
            note("research: matched " + std::to_string(written.size()) +
                 " starter skill(s) to this task");
        }
    }

    skills.catalog = skills_catalog(skills.skills);
    if (!skills.empty()) {
        note("research: " + std::to_string(skills.skills.size()) + " skill(s) available");
    }

    std::string hint = relevant_files_note(config, options.project, options.task);

    // Where the names in the task are actually DEFINED.
    //
    // relevant_files_note above is cosine distance over embedded line windows: it
    // answers "what reads like this", which is the right tool for "where is the
    // retry logic" and the wrong one for "where is parse_plan". This answers the
    // second question exactly, needs no embedding model and no index, and is the
    // difference between a coder opening the right file first and hunting.
    if (const std::string names = symbols_note(options.project, options.task);
        !names.empty()) {
        hint += names + "\n";
    }
    if (!hint.empty()) note("research: the index suggests where to look");

    // The Researcher. A read-only pass that reports what the index cannot: the
    // index says which files are RELATED, this says what they mean and how this
    // project does things.
    if (options.research) {
        note("research: investigating the codebase");
        std::string findings;
        if (is_cli_backend(options.researcher_backend)) {
            // An agent CLI in a SANDBOX, and the sandbox thrown away afterwards.
            // Its own read-only mode differs per tool and some have none; a
            // throwaway copy makes the question moot -- whatever it writes, only
            // its text comes back.
            const auto scratch = auspex_crew_dir() / result.run_id / "research";
            std::string error;
            if (create_sandbox(options.project, scratch, &error)) {
                findings = ask_cli(options.researcher_backend,
                                   options.model_for("researcher"),
                                   researcher_prompt(options.task,
                                                     list_file_names(scratch), {},
                                                     options.coder),
                                   scratch);
                destroy_sandbox(scratch);
            }
        } else {
            findings = run_researcher(config, options.task, options.project,
                                      options.coder, options.model_for("researcher"));
        }

        if (!findings.empty()) {
            // Shared with the Director AND every coder, through the channel each
            // already reads. One set of findings, not one per role.
            hint += "What the Researcher found:\n" + findings + "\n\n";
            note("research: reported " + std::to_string(findings.size()) + " chars");
        } else {
            note("research: nothing reported");
        }
    }

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
                    director_prompt(options.task, files, options.max_subtasks,
                                    hint, options.focus, options.offered_roles(),
                                    options.role_limits),
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
        return finish(result);
    }

    // Enforced on the way back as well as offered on the way out.
    //
    // A Director asked for at most two testers will sometimes plan three. The
    // extra becomes a `coder` rather than being dropped: the piece of work is
    // still worth doing, it just does not get the role that was full. If `coder`
    // itself is switched off the piece goes, because there is nobody to give it
    // to.
    {
        std::map<std::string, int> used;
        std::vector<PlannedSubtask> kept;
        for (auto& subtask : plan.subtasks) {
            const int limit = options.role_limit(subtask.role);
            if (limit == 0 || (limit > 0 && used[subtask.role] >= limit)) {
                subtask.role = "coder";
            }
            const int coder_limit = options.role_limit("coder");
            if (subtask.role == "coder" && coder_limit == 0) continue;
            if (subtask.role == "coder" && coder_limit > 0 &&
                used["coder"] >= coder_limit) {
                continue;
            }
            ++used[subtask.role];
            kept.push_back(std::move(subtask));
        }
        plan.subtasks = std::move(kept);
        for (std::size_t i = 0; i < plan.subtasks.size(); ++i) {
            plan.subtasks[i].n = static_cast<int>(i) + 1;
        }
    }

    for (const auto& subtask : plan.subtasks) {
        PlannedSubtask piece = subtask;
        // Carried in the detail rather than as another prompt argument. The coder
        // prompt already renders detail prominently, and threading a fifth string
        // through every signature to say the same thing would be worse.
        std::string extra = lessons;
        if (!hint.empty()) extra = hint + extra;
        if (!extra.empty()) {
            piece.detail = piece.detail.empty() ? extra
                                                : piece.detail + "\n\n" + extra;
        }
        attempts.push_back({piece, {}, {}, {}, "todo"});
    }
    publish(/*active=*/true);
    enter("build", "build: " + std::to_string(attempts.size()) + " coders");

    if (stopped()) {
        result.error = "cancelled";
        publish(false);
        return finish(result);
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

            // The coder's model, refined by how hard THIS piece looks. A plan with
            // a rename and a redesign in it should not run both on the same model,
            // and no per-role setting can say that.
            //
            // Declared out here because the verify loop below re-runs the same
            // coder on the same model when the tests come back red.
            std::string coder_model = options.model_for("coder");
            if (options.route) {
                const Difficulty how = classify_difficulty(
                    attempt.subtask.title + " " + attempt.subtask.detail);
                coder_model = route_model(config, coder_model, how.tier);
            }

            if (is_cli_backend(backend)) {
                attempt.outcome = run_cli_coder(config, attempt.subtask, sandbox,
                                                backend, options.model_for("coder"),
                                                /*timeout_seconds=*/900, lessons);
            } else {
                // A read-only role is REFUSED, not asked. run_tool() turns down
                // every writing verb, so a reviewer that decides to edit something
                // is told no rather than trusted -- which is what ollamadev cannot
                // do, because its permission mode is process-global and coders run
                // concurrently. Auspex's limits are per coder, so it can.
                CoderLimits limits = options.coder;
                if (role_is_read_only(attempt.subtask.role)) limits.read_only = true;

                // Told BEFORE it writes anything, not only when a retry happens.
                // The warning used to live in retry_note() alone, which meant a
                // coder that took the shortcut on its first attempt was never
                // warned -- and on the first run that could, it did exactly that.
                PlannedSubtask piece = attempt.subtask;
                if (options.verify_attempts > 0 && options.coder.allow_run) {
                    piece.detail = piece.detail + "\n\n" + no_cheating_note();
                }

                // What it is doing, published as it happens.
                //
                // The board used to say "doing" for minutes at a time while every
                // read and write sat in CoderOutcome::steps, unpublished until the
                // coder had finished -- which is exactly when it stops being
                // worth watching.
                // Named BEFORE it starts, not after.
                //
                // outcome.model is only set when run_coder returns, so for the
                // whole time a coder was actually working the board said "ollama"
                // -- the one stretch when you might want to know which model is
                // spending your money.
                {
                    std::lock_guard lock(state_mutex);
                    attempt.outcome.model =
                        coder_model.empty() ? config.ollama_model : coder_model;
                }

                const auto report = [&](const CoderStep& step) {
                    std::string doing;
                    switch (step.call.tool) {
                        case CoderTool::Read:    doing = "reading "; break;
                        case CoderTool::Write:   doing = "writing "; break;
                        case CoderTool::Replace: doing = "editing "; break;
                        case CoderTool::Delete:  doing = "deleting "; break;
                        case CoderTool::List:    doing = "looking around"; break;
                        case CoderTool::Run:     doing = "running tests"; break;
                        case CoderTool::Skill:   doing = "opening a skill"; break;
                        case CoderTool::Mcp:     doing = "calling a tool"; break;
                        case CoderTool::Finish:  doing = "finishing"; break;
                        case CoderTool::Unknown: doing = "thinking"; break;
                    }
                    if (!step.call.path.empty()) doing += step.call.path;

                    {
                        std::lock_guard lock(state_mutex);
                        attempt.activity = doing;
                        // Counted from the sandbox as it stands, so the numbers
                        // move while the coder works rather than appearing all at
                        // once when it stops.
                        if (step.call.tool == CoderTool::Write ||
                            step.call.tool == CoderTool::Replace ||
                            step.call.tool == CoderTool::Delete) {
                            const Changeset so_far =
                                capture_changeset(options.project, sandbox);
                            count_diff_lines(so_far.diff, &attempt.added,
                                             &attempt.removed);
                        }
                    }
                    publish(/*active=*/true);
                };

                attempt.outcome = run_coder(config, piece, sandbox,
                                            limits, coder_model,
                                            steer_mailbox(result.run_id,
                                                          attempt.subtask.n),
                                            skills, mcp, report);
            }

            // Do the tests still pass?
            //
            // In the SANDBOX, before anything is captured, so a coder that broke
            // the suite gets told while it can still act -- and so a suite it
            // cannot fix never reaches the project. Needs allow_run as well as
            // verify_attempts: running a suite executes code this coder just
            // wrote, and that is one decision, not two.
            if (options.verify_attempts > 0 && options.coder.allow_run &&
                !is_cli_backend(backend)) {
                if (const auto tests = detect_tests(sandbox)) {
                    for (int attempt_n = 1; attempt_n <= options.verify_attempts;
                         ++attempt_n) {
                        if (stopped()) break;
                        const TestRun tested = run_tests(*tests, sandbox,
                                                         /*timeout_seconds=*/300,
                                                         cancel);
                        if (tested.green()) {
                            note("verify: #" + std::to_string(attempt.subtask.n) +
                                 " " + tests->label + " green");
                            break;
                        }
                        note("verify: #" + std::to_string(attempt.subtask.n) + " " +
                             tests->label + " failed, attempt " +
                             std::to_string(attempt_n) + " of " +
                             std::to_string(options.verify_attempts));

                        if (attempt_n == options.verify_attempts) {
                            // Out of attempts. The changeset is still captured and
                            // still audited -- what the coder wrote may be right
                            // and the suite wrong -- but the board will say so.
                            attempt.outcome.error =
                                "the tests were still failing after " +
                                std::to_string(options.verify_attempts) +
                                " attempts (" + tests->label + ")";
                            break;
                        }

                        // Handed back as a steer, which is the mechanism that
                        // already exists for "a person said something mid-run".
                        // The coder reads it on its next turn.
                        PlannedSubtask again = attempt.subtask;
                        again.detail = attempt.subtask.detail + "\n\n" +
                                       retry_note(tested, attempt_n,
                                                  options.verify_attempts);
                        attempt.outcome = run_coder(config, again, sandbox,
                                                    options.coder, coder_model,
                                                    steer_mailbox(result.run_id,
                                                                  attempt.subtask.n),
                                                    skills, mcp);
                    }
                } else {
                    note("verify: #" + std::to_string(attempt.subtask.n) +
                         " no test command found, so nothing was run");
                }
            }

            attempt.changeset = capture_changeset(options.project, sandbox);
            {
                std::lock_guard lock(state_mutex);
                attempt.activity.clear();   // it is not doing anything now
                count_diff_lines(attempt.changeset.diff, &attempt.added,
                                 &attempt.removed);
            }

            // A green suite proves nothing if the suite was edited to be green.
            // Checked on the diff rather than on intent: adding tests only adds
            // lines, so a coder asked for more tests trips nothing, while one that
            // rewrote an assertion is visible. Held for a person either way -- a
            // real test refactor looks the same and is also worth a glance.
            const auto weakened = weakened_tests(attempt.changeset);
            if (!weakened.empty()) {
                note("verify: #" + std::to_string(attempt.subtask.n) +
                     " removed lines from " + weakened.front() +
                     (weakened.size() > 1
                          ? " and " + std::to_string(weakened.size() - 1) + " more"
                          : ""));
            }

            // Audited on the worker thread. It is another model call, and doing it
            // here means a slow audit of one piece does not hold up the coder on
            // the next.
            {
                // The Auditor is working. Set under the same lock everything else
                // in this file uses; several coders reach here at once and the
                // phase is one string.
                std::lock_guard lock(state_mutex);
                phase = "audit";
            }
            publish(true);

            // Which review, in order of cost. Debate is three calls; a panel is
            // `amplify` calls; the plain Auditor is one.
            if (is_cli_backend(options.auditor_backend)) {
                // Certain checks first, as always -- there is nothing an agent
                // could say that makes a leaked credential acceptable.
                attempt.audit = deterministic_audit(attempt.changeset, options.audit);
                if (!attempt.audit.held()) {
                    attempt.audit = syntax_audit(attempt.changeset);
                }
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
                // Three voices, three settings. A debate whose advocate and
                // skeptic are the same model is one model arguing with itself.
                DebateModels voices;
                voices.advocate = options.model_for("advocate");
                voices.skeptic  = options.model_for("skeptic");
                voices.judge    = options.model_for("judge");
                attempt.audit = debate_changeset(config, attempt.subtask,
                                                 attempt.changeset, options.audit,
                                                 voices);
            } else if (options.amplify > 1) {
                attempt.audit = audit_panel(config, attempt.subtask, attempt.changeset,
                                            options.amplify, options.audit,
                                            options.model_for("auditor"));
            } else {
                attempt.audit = audit_changeset(config, attempt.subtask,
                                                attempt.changeset, options.audit,
                                                options.model_for("auditor"));
            }

            // A weakened test suite holds, whatever the Auditor said. This is a
            // fact about the diff rather than an opinion about it, so it is not
            // the Auditor's to overrule -- the same standing the secret gate and
            // the parser have.
            if (!weakened.empty()) {
                Audit gate;
                gate.certain = true;
                gate.reason = "lines were removed from " + weakened.front() +
                              " -- a suite edited to pass is not a suite that passed";
                for (const auto& path : weakened) gate.notes.push_back(path);
                if (!attempt.audit.reason.empty()) {
                    gate.notes.push_back("the Auditor also said: " + attempt.audit.reason);
                }
                attempt.audit = gate;
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
        return finish(result);
    }

    // ---- land ----
    //
    // Serial, and deliberately so. Two coders that edited one file are both
    // "accepted" and only one can win; deciding that on a thread would make which
    // one depends on timing.
    enter("land", "land: applying what the Auditor passed");

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

    // A record of what this run did, if asked for. AFTER landing, so nothing the
    // Auditor held is ever committed -- and only the paths that actually landed,
    // never `git add -A`, so work you had in progress alongside is not swept in.
    if (options.commit && result.applied > 0 && !landed.files.empty()) {
        std::vector<std::string> paths;
        paths.reserve(landed.files.size());
        for (const auto& file : landed.files) paths.push_back(file.path);

        const CommitResult committed = commit_paths(
            options.project, paths,
            commit_message(options.task, result.run_id, paths));
        note(committed.ok
                 ? "commit: " + committed.commit + " (" +
                       std::to_string(committed.staged) + " file(s))"
                 : "commit: " + committed.error);
    }

    note("done: " + std::to_string(result.applied) + " applied · " +
         std::to_string(result.held) + " held");
    return finish(result);
}

}  // namespace auspex
