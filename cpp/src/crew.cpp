#include "auspex/crew.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

// ollamadev emits `data` as a nested object; the fields Auspex renders live there
// rather than at the top level. Read defensively -- a board is produced by a run
// that may have been interrupted, so a missing key is expected, not exceptional.
int int_field(const json& object, const char* key, int fallback = 0) {
    if (!object.contains(key)) return fallback;
    const auto& value = object[key];
    if (value.is_number_integer()) return value.get<int>();
    if (value.is_number_float())   return static_cast<int>(value.get<double>());
    return fallback;
}

std::string string_field(const json& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto& value = object[key];
    return value.is_string() ? value.get<std::string>() : std::string{};
}

}  // namespace

std::vector<BoardItem> parse_board(const std::string& output) {
    const json parsed = json::parse(output, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_array()) return {};

    std::vector<BoardItem> items;
    for (const auto& entry : parsed) {
        if (!entry.is_object()) continue;

        BoardItem item;
        item.id      = string_field(entry, "id");
        item.kind    = string_field(entry, "kind");
        item.summary = string_field(entry, "summary");
        item.diff    = string_field(entry, "detail");

        if (entry.contains("data") && entry["data"].is_object()) {
            const auto& data = entry["data"];
            item.n         = int_field(data, "n");
            item.reason    = string_field(data, "reason");
            item.repo_root = string_field(data, "repoRoot");
            item.store     = string_field(data, "store");
            if (data.contains("files") && data["files"].is_array()) {
                item.files = static_cast<int>(data["files"].size());
                for (const auto& name : data["files"]) {
                    if (name.is_string()) item.file_names.push_back(name.get<std::string>());
                }
            }
        }

        // An item with no number cannot be accepted or discarded -- there is no way
        // to name it -- so it is dropped rather than listed as undecidable.
        if (item.n <= 0) continue;
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(),
              [](const BoardItem& a, const BoardItem& b) { return a.n < b.n; });
    return items;
}

// Checks if the 'ollamadev' crew is available in the current path
bool crew_available() { return in_path("ollamadev"); }

std::vector<BoardItem> board_items(const std::filesystem::path& project) {
    if (!crew_available()) return {};

    const auto result = run({"ollamadev", "board", "--json"}, /*capture=*/true,
                            project.string());
    if (!result.ok) return {};
    return parse_board(result.out);
}

std::optional<BoardItem> board_item(const std::vector<BoardItem>& items, int n) {
    const auto it = std::find_if(items.begin(), items.end(),
                                 [n](const BoardItem& item) { return item.n == n; });
    if (it == items.end()) return std::nullopt;
    return *it;
}

std::vector<std::string> crew_run_command(const std::string& task,
                                          const CrewOptions& options) {
    const std::string trimmed = trim(task);
    // A blank task is not an empty run: ollamadev with no prompt starts an
    // interactive session, which is not what a Start button means.
    if (trimmed.empty()) return {};

    std::vector<std::string> argv{"ollamadev", "crew", trimmed};

    // Flags after the task. Each is a fixed string; the only value that varies is a
    // number, formatted rather than passed through.
    if (options.route)    argv.push_back("--route");
    if (options.debate)   argv.push_back("--debate");
    if (options.dedupe)   argv.push_back("--dedupe");
    if (options.learn)    argv.push_back("--learn");
    if (options.security) argv.push_back("--security");

    if (options.max_coders > 0) {
        argv.push_back("--max");
        argv.push_back(std::to_string(options.max_coders));
    }
    if (options.swarm > 0) {
        argv.push_back("--swarm");
        argv.push_back(std::to_string(options.swarm));
    }
    if (options.amplify > 0) {
        argv.push_back("--amplify");
        argv.push_back(std::to_string(options.amplify));
    }

    // The pack is checked by the caller against the engine's own list before it
    // gets here; an empty one is simply left off.
    if (!options.pack.empty()) {
        argv.push_back("--pack");
        argv.push_back(options.pack);
    }

    return argv;
}

std::vector<std::string> parse_crew_names(const std::string& output) {
    std::vector<std::string> names;

    for (const auto& line : split_lines(output)) {
        // Two columns separated by runs of spaces, and the name is the first token.
        // A line that does not begin with whitespace is a heading rather than an
        // entry, which is how the engine separates built-ins from custom ones.
        if (line.empty() || !std::isspace(static_cast<unsigned char>(line[0]))) continue;

        const std::string entry = trim(line);
        if (entry.empty()) continue;

        std::string name;
        for (const char c : entry) {
            if (std::isspace(static_cast<unsigned char>(c))) break;
            name.push_back(c);
        }
        if (!name.empty()) names.push_back(std::move(name));
    }

    return names;
}

std::vector<std::string> crew_packs(const std::filesystem::path& project) {
    if (!crew_available()) return {};
    const auto result = run({"ollamadev", "crew", "pack"}, /*capture=*/true,
                            project.string());
    if (!result.ok) return {};
    return parse_crew_names(result.out);
}

std::vector<std::string> crew_roles(const std::filesystem::path& project) {
    if (!crew_available()) return {};
    const auto result = run({"ollamadev", "crew", "role"}, /*capture=*/true,
                            project.string());
    if (!result.ok) return {};
    return parse_crew_names(result.out);
}

bool is_known_pack(const std::string& pack, const std::vector<std::string>& known) {
    if (pack.empty()) return false;
    return std::find(known.begin(), known.end(), pack) != known.end();
}

std::vector<std::string> crew_accept_command(int n) {
    if (n <= 0) return {};
    return {"ollamadev", "crew", "accept", std::to_string(n)};
}

std::vector<std::string> crew_discard_command(int n) {
    if (n <= 0) return {};
    return {"ollamadev", "crew", "discard", std::to_string(n)};
}

std::vector<std::string> crew_steer_command(int n, const std::string& instruction) {
    if (n <= 0 || instruction.empty()) return {};
    // The instruction is ONE argv element. It is free text from the user and may
    // contain anything; as an argument it is data, and there is no shell to make it
    // otherwise.
    return {"ollamadev", "crew", "steer", std::to_string(n), instruction};
}

DiffLine classify_diff_line(std::string_view line) {
    // Order matters. "+++ b/file" starts with '+' and is a header, not an added
    // line -- colouring it green would put two bright lines at the top of every
    // file in the patch.
    if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0 ||
        line.rfind("diff --git", 0) == 0 || line.rfind("index ", 0) == 0 ||
        line.rfind("new file", 0) == 0 || line.rfind("deleted file", 0) == 0 ||
        line.rfind("similarity index", 0) == 0 || line.rfind("rename ", 0) == 0) {
        return DiffLine::FileHeader;
    }
    if (line.rfind("@@", 0) == 0) return DiffLine::Hunk;
    if (!line.empty() && line[0] == '+') return DiffLine::Added;
    if (!line.empty() && line[0] == '-') return DiffLine::Removed;
    return DiffLine::Context;
}

DiffStat diff_stat(const std::string& diff) {
    DiffStat stat;
    for (const auto& line : split_lines(diff)) {
        switch (classify_diff_line(line)) {
            case DiffLine::Added:   ++stat.added;   break;
            case DiffLine::Removed: ++stat.removed; break;
            default: break;
        }
    }
    return stat;
}

std::vector<std::string> crew_resume_command() {
    return {"ollamadev", "crew", "resume"};
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------
const std::vector<Backend>& known_backends() {
    // Copied from CliBackend::ids()/labelFor() in ollamadev-qt rather than derived,
    // because the mapping is not derivable: "Cursor Agent" -> "cursor-agent" but
    // "Gemini CLI" -> "gemini", and lower-case-and-hyphenate would give the wrong
    // answer for the second one while looking right for the first.
    static const std::vector<Backend> kBackends{
        {"ollama", "Ollama", false},
        {"claude", "Claude Code", false},
        {"codex", "Codex", false},
        {"gemini", "Gemini CLI", false},
        {"cursor-agent", "Cursor Agent", false},
        {"opencode", "OpenCode", false},
        {"qwen", "Qwen Code", false},
        {"aider", "Aider", false},
        {"goose", "Goose", false},
        {"amp", "Amp", false},
        {"crush", "Crush", false},
        {"droid", "Droid", false},
    };
    return kBackends;
}

std::vector<Backend> parse_backends(const std::string& output) {
    std::vector<Backend> backends;

    for (const auto& line : split_lines(output)) {
        const std::string row = trim(line);
        if (row.empty()) continue;

        // The label is a prefix of the row, and rows are columns padded with runs
        // of spaces. Matching against the known labels LONGEST FIRST matters:
        // "Claude Code" starts with nothing else, but a shorter label that is a
        // prefix of a longer one would otherwise win.
        const Backend* match = nullptr;
        for (const auto& candidate : known_backends()) {
            if (row.rfind(candidate.label, 0) != 0) continue;
            if (!match || candidate.label.size() > match->label.size()) match = &candidate;
        }
        if (!match) continue;   // a heading, the rule, or the trailing prose

        // What follows the label is the Installed column: "yes", or an em dash for
        // one that is not there.
        const std::string rest = trim(row.substr(match->label.size()));
        Backend backend  = *match;
        backend.installed = rest.rfind("yes", 0) == 0;
        backends.push_back(std::move(backend));
    }

    return backends;
}

std::vector<Backend> available_backends() {
    if (!crew_available()) return {};
    const auto result = run({"ollamadev", "backends"});
    if (!result.ok) return {};
    return parse_backends(result.out);
}

std::vector<std::string> backend_prompt_command(const std::string& backend_id,
                                                const std::string& prompt) {
    const std::string text = trim(prompt);
    if (backend_id.empty() || text.empty()) return {};
    // The prompt LAST and whole: ollamadev takes it as a positional, and it is the
    // only free text on this command line.
    return {"ollamadev", "--backend", backend_id, text};
}

// ---------------------------------------------------------------------------
// The brain
// ---------------------------------------------------------------------------
const std::vector<std::string>& router_tiers() {
    static const std::vector<std::string> kTiers{"simple", "moderate", "hard"};
    return kTiers;
}

std::vector<std::string> router_get_command(const std::string& tier) {
    if (tier.empty()) return {};
    return {"ollamadev", "config", "get", "router." + tier};
}

std::vector<std::string> router_set_command(const std::string& tier,
                                            const std::string& model) {
    if (tier.empty() || model.empty()) return {};
    return {"ollamadev", "config", "set", "router." + tier, model};
}

std::vector<std::string> route_command(const std::string& text) {
    const std::string trimmed = trim(text);
    if (trimmed.empty()) return {};
    // WITHOUT --run. This is the "where would this go" probe; adding --run would
    // make a question that was meant to be answered into work that gets done.
    return {"ollamadev", "route", trimmed};
}

RouteDecision parse_route(const std::string& output) {
    RouteDecision decision;

    for (const auto& line : split_lines(output)) {
        std::string row = trim(line);
        // "→ simple  ollama:gpt-oss:20b-cloud  (short lookup-style question)"
        constexpr std::string_view kArrow = "→";
        if (row.rfind(kArrow, 0) != 0) continue;
        row = trim(row.substr(kArrow.size()));

        // The reason is parenthesised and last; take it off before splitting the
        // rest on spaces, or a reason containing a space would become a field.
        if (const auto open = row.find('('); open != std::string::npos) {
            const auto close = row.rfind(')');
            if (close != std::string::npos && close > open) {
                decision.reason = row.substr(open + 1, close - open - 1);
            }
            row = trim(row.substr(0, open));
        }

        std::istringstream fields(row);
        fields >> decision.tier >> decision.model;
        return decision;
    }

    return decision;
}

bool is_cloud_model(const std::string& tag) {
    std::string lower = trim(tag);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.empty()) return false;

    const auto ends_with = [&lower](std::string_view suffix) {
        return lower.size() >= suffix.size() &&
               lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return ends_with("-cloud") || ends_with(":cloud");
}

std::filesystem::path usage_path(const std::filesystem::path& project) {
    if (project.empty()) return {};
    return project / ".ollamadev" / "costs" / "usage.json";
}

TokenUsage parse_usage(const std::string& json_text) {
    TokenUsage usage;
    if (json_text.empty()) return usage;

    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return usage;

    usage.known = true;
    usage.turns = int_field(document, "turns");

    if (!document.contains("models") || !document["models"].is_object()) return usage;

    for (const auto& [tag, entry] : document["models"].items()) {
        if (!entry.is_object()) continue;
        // Prompt AND generated: both are tokens the model processed, and counting
        // only what came back would understate a long context by an order of
        // magnitude -- which is exactly where the local/cloud question matters.
        const long long tokens =
            static_cast<long long>(int_field(entry, "prompt")) +
            static_cast<long long>(int_field(entry, "eval"));
        (is_cloud_model(tag) ? usage.cloud : usage.local) += tokens;
    }

    return usage;
}

TokenUsage project_usage(const std::filesystem::path& project) {
    const auto path = usage_path(project);
    if (path.empty()) return {};

    std::ifstream in(path);
    if (!in) return {};   // nothing has run here yet

    std::ostringstream contents;
    contents << in.rdbuf();
    return parse_usage(contents.str());
}

std::string usage_summary(const TokenUsage& usage) {
    const long long total = usage.total();
    if (!usage.known || total <= 0) return {};

    // Thousands, one decimal, the way the engine's own stats prints them -- so a
    // number read here and a number read there look like the same number.
    std::ostringstream out;
    if (total >= 1000) {
        out.setf(std::ios::fixed);
        out.precision(1);
        out << (static_cast<double>(total) / 1000.0) << "k tokens";
    } else {
        out << total << " tokens";
    }

    const long long local_pct = usage.local * 100 / total;
    out << " · " << local_pct << "% local · " << (100 - local_pct) << "% cloud";
    return out.str();
}

std::vector<std::string> parse_models(const std::string& output) {
    std::vector<std::string> models;
    for (const auto& line : split_lines(output)) {
        const std::string name = trim(line);
        if (name.empty()) continue;
        // A model name has no spaces. Anything with one is a heading or a note,
        // and a heading pushed onto a command line would be a model that is not.
        if (name.find(' ') != std::string::npos) continue;
        models.push_back(name);
    }
    return models;
}

std::vector<std::string> available_models(const std::filesystem::path& project) {
    if (!crew_available()) return {};
    const auto result = run({"ollamadev", "models"}, /*capture=*/true, project.string());
    if (!result.ok) return {};
    return parse_models(result.out);
}

const std::vector<EngineAction>& engine_actions() {
    static const std::vector<EngineAction> kActions{
        {"Chat", "An interactive agent turn in this folder", {"ollamadev"}},
        {"Ship", "Stage, scan for secrets, AI commit, then push", {"ollamadev", "ship"}},
        {"Verify", "Run this project's tests and auto-fix what fails",
         {"ollamadev", "verify"}},
        {"Index", "Build the semantic code index for this folder",
         {"ollamadev", "index", "build"}},
        {"Doctor", "Check Ollama, the models and the CLIs are healthy",
         {"ollamadev", "doctor"}},
    };
    return kActions;
}


// ---------------------------------------------------------------------------
// Run state
// ---------------------------------------------------------------------------
std::filesystem::path crew_state_path() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".ollamadev" / "crew" / "current.json";
    }
    return {};
}

std::filesystem::path board_state_path() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".ollamadev" / "board" / "current.json";
    }
    return {};
}

CrewRun parse_crew_run(const std::string& json_text) {
    CrewRun run;
    if (json_text.empty()) return run;

    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return run;

    // A file that parses but describes no run is still an answer -- it is how the
    // engine says the crew is idle -- so `known` is set here rather than only when
    // there is something to show.
    run.known = true;

    if (document.contains("active") && document["active"].is_boolean()) {
        run.active = document["active"].get<bool>();
    }
    if (document.contains("runId") && document["runId"].is_string()) {
        run.run_id = document["runId"].get<std::string>();
    }
    if (document.contains("phase") && document["phase"].is_string()) {
        run.phase = trim(document["phase"].get<std::string>());
    }
    if (document.contains("debate") && document["debate"].is_boolean()) {
        run.debate = document["debate"].get<bool>();
    }
    if (document.contains("amplify") && document["amplify"].is_number_integer()) {
        run.amplify = document["amplify"].get<int>();
    }
    if (document.contains("verify") && document["verify"].is_boolean()) {
        run.verify = document["verify"].get<bool>();
    }
    if (document.contains("task") && document["task"].is_string()) {
        run.task = trim(document["task"].get<std::string>());
    }

    if (document.contains("subtasks") && document["subtasks"].is_array()) {
        for (const auto& entry : document["subtasks"]) {
            if (!entry.is_object()) continue;
            CrewSubtask subtask;
            if (entry.contains("n") && entry["n"].is_number_integer()) {
                subtask.n = entry["n"].get<int>();
            }
            if (entry.contains("role") && entry["role"].is_string()) {
                subtask.role = entry["role"].get<std::string>();
            }
            if (entry.contains("title") && entry["title"].is_string()) {
                subtask.title = trim(entry["title"].get<std::string>());
            }
            if (entry.contains("state") && entry["state"].is_string()) {
                subtask.state = entry["state"].get<std::string>();
            }
            subtask.backend = string_field(entry, "backend");
            subtask.model   = string_field(entry, "model");
            subtask.route   = string_field(entry, "route");
            run.subtasks.push_back(std::move(subtask));
        }
    }

    return run;
}

CrewRun current_crew_run(const std::filesystem::path& path) {
    if (path.empty()) return {};

    std::ifstream in(path);
    if (!in) return {};   // no crew has ever run here

    std::ostringstream contents;
    contents << in.rdbuf();
    return parse_crew_run(contents.str());
}

bool crew_subtask_held(const CrewSubtask& subtask) { return subtask.state == "held"; }

CrewLane crew_lane_of(const CrewSubtask& subtask) {
    // ollamadev-qt's BoardPane mapping, verbatim: held gets its own column, done is
    // Done, todo is To do, and EVERYTHING ELSE -- flagged included -- is Doing.
    //
    // Note the default is Doing, not To do. That is the opposite of what Auspex
    // guessed at first, and the Qt reading is the right one: an unrecognised state
    // came from an engine that is doing something with the subtask, so calling it
    // "not started" understates a run that is live.
    if (crew_subtask_held(subtask))   return CrewLane::Held;
    if (subtask.state == "done")      return CrewLane::Done;
    if (subtask.state == "todo")      return CrewLane::Todo;
    if (subtask.state.empty())        return CrewLane::Todo;   // nothing recorded yet
    return CrewLane::Doing;
}

std::string crew_subtask_model_line(const CrewSubtask& subtask) {
    std::string line = subtask.model;
    // The backend only when it is not implied by the model name, which already
    // carries "-cloud" when it is one. A line reading "ollama · gpt-oss:20b" on
    // every card is noise that pushes the useful half off the end.
    if (line.empty()) line = subtask.backend;
    if (!subtask.route.empty()) {
        if (!line.empty()) line += " · ";
        line += subtask.route;
    }
    return line;
}

std::vector<CrewMember> crew_members(const CrewRun& run) {
    std::vector<CrewMember> members;
    if (!run.known) return members;

    // Counting the coders first, because three of the four rows below describe
    // themselves in terms of them.
    int running = 0, finished = 0, held = 0;
    for (const auto& subtask : run.subtasks) {
        switch (crew_lane_of(subtask)) {
            case CrewLane::Held:  ++held; break;
            case CrewLane::Done:  ++finished; break;
            case CrewLane::Doing: ++running; break;
            case CrewLane::Todo:  break;
        }
    }
    const int planned = static_cast<int>(run.subtasks.size());

    // The order the work happens in, which is the order it should be read in.
    // "Coders" is one row rather than one per coder: the lanes below already show
    // them individually, and repeating that here would be the same list twice.
    const std::string phase = run.phase;
    const auto reached = [&phase](std::initializer_list<const char*> earlier) {
        // A phase is "done" once a later one has started. With no phase recorded
        // -- an older state file -- nothing is marked, which is honest: we do not
        // know rather than guessing that everything finished.
        if (phase.empty()) return false;
        for (const char* name : earlier) {
            if (phase == name) return true;
        }
        return false;
    };

    CrewMember researcher{"Researcher", {}, phase == "research", false};
    researcher.done = reached({"plan", "build", "audit", "land"});
    if (!run.active && planned > 0) researcher.done = true;
    members.push_back(std::move(researcher));

    CrewMember director{"Director",
                        planned > 0 ? std::to_string(planned) + " piece" +
                                          (planned == 1 ? "" : "s")
                                    : std::string{},
                        phase == "plan", planned > 0};
    members.push_back(std::move(director));

    std::string coders;
    if (running > 0) coders = std::to_string(running) + " working";
    else if (planned > 0) coders = std::to_string(finished + held) + "/" +
                                   std::to_string(planned) + " finished";
    // Working when a coder IS working, not only when the phase says so.
    //
    // The phase is one string and the crew is not one thing: the audit of coder 1
    // runs on its worker thread while coders 2 and 3 are still writing. Watched on
    // a live run -- the roster showed the Auditor lit and the coders dark, with
    // three of them visibly in the Doing lane underneath. Two members working at
    // once is the truth, not a display bug to be tidied away.
    members.push_back({"Coders", coders, running > 0 || phase == "build",
                       planned > 0 && running == 0});

    // WHO reviews, by name and by number.
    //
    // "Auditor" was one row whatever the run was doing, so a debate -- an
    // Advocate, a Skeptic and a Judge, three model calls -- looked identical to a
    // single reviewer, and so did a panel of five. The switch you paid for should
    // be visible in the crew it produced.
    const bool reviewing = phase == "audit";
    const bool review_done = reached({"land"}) || (!run.active && planned > 0);

    if (run.debate) {
        for (const char* voice : {"Advocate", "Skeptic", "Judge"}) {
            members.push_back({voice, {}, reviewing, review_done});
        }
        if (held > 0) members.back().detail = std::to_string(held) + " held";
    } else {
        CrewMember auditor{"Auditor", {}, reviewing, review_done};
        if (run.amplify > 1) {
            auditor.name = "Auditors";
            auditor.detail = std::to_string(run.amplify) + " voting";
        }
        if (held > 0) {
            auditor.detail += (auditor.detail.empty() ? "" : ", ") +
                              std::to_string(held) + " held";
        }
        members.push_back(std::move(auditor));
    }

    // The tests are a member of the crew when they run: something is executing
    // this project's suite and deciding whether the work stands.
    if (run.verify) {
        members.push_back({"Tests", {}, phase == "build" && running > 0, review_done});
    }

    return members;
}

CrewProgress crew_progress(const CrewRun& run) {
    CrewProgress progress;
    progress.total = static_cast<int>(run.subtasks.size());
    for (const auto& subtask : run.subtasks) {
        if (subtask.state == "done") ++progress.done;
    }
    return progress;
}

std::optional<CrewSubtask> crew_current_subtask(const CrewRun& run) {
    // A coder that is actually RUNNING, preferred over one merely outstanding.
    //
    // This drives the steer box, and steering is talking to a live coder. A held
    // changeset has stopped working -- it is waiting on accept or discard -- so
    // aiming an instruction at it would send words to nobody. Taking the first
    // "doing" rather than the first not-done is what makes the target real.
    for (const auto& subtask : run.subtasks) {
        if (subtask.state == "doing") return subtask;
    }
    // Nothing in flight: fall back to the earliest that has neither finished nor
    // been held, which is what will start next. Better than nothing for the panel
    // line that names what the crew is up to.
    for (const auto& subtask : run.subtasks) {
        if (subtask.state != "done" && !crew_subtask_held(subtask)) return subtask;
    }
    return std::nullopt;
}

std::string crew_status_label(const CrewRun& run) {
    if (!run.known || !run.active) return {};

    const CrewProgress progress = crew_progress(run);
    // No plan yet means the Director is still deciding what the pieces are. Saying
    // "0/0" would look like a stalled run rather than the first stage of a live one.
    if (progress.total == 0) return "Crew planning";

    return "Crew " + std::to_string(progress.done) + "/" +
           std::to_string(progress.total);
}

std::string crew_status_detail(const CrewRun& run) {
    if (!run.known) return {};

    if (!run.active) {
        return run.task.empty() ? "The crew is idle"
                                : "The crew is idle. Last task: " + run.task;
    }

    std::string detail = run.task.empty() ? "The crew is working" : run.task;
    if (const auto subtask = crew_current_subtask(run); subtask && !subtask->title.empty()) {
        detail += "\n";
        if (!subtask->role.empty()) detail += subtask->role + ": ";
        detail += subtask->title;
    }
    return detail;
}

}  // namespace auspex
