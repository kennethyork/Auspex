#include "auspex/crew.hpp"

#include <algorithm>
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

        if (entry.contains("data") && entry["data"].is_object()) {
            const auto& data = entry["data"];
            item.n      = int_field(data, "n");
            item.reason = string_field(data, "reason");
            if (data.contains("files") && data["files"].is_array()) {
                item.files = static_cast<int>(data["files"].size());
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

std::vector<BoardItem> board_items() {
    if (!crew_available()) return {};

    const auto result = run({"ollamadev", "board", "--json"});
    if (!result.ok) return {};
    return parse_board(result.out);
}

std::optional<BoardItem> board_item(const std::vector<BoardItem>& items, int n) {
    const auto it = std::find_if(items.begin(), items.end(),
                                 [n](const BoardItem& item) { return item.n == n; });
    if (it == items.end()) return std::nullopt;
    return *it;
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

std::vector<std::string> crew_resume_command() {
    return {"ollamadev", "crew", "resume"};
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

CrewProgress crew_progress(const CrewRun& run) {
    CrewProgress progress;
    progress.total = static_cast<int>(run.subtasks.size());
    for (const auto& subtask : run.subtasks) {
        if (subtask.state == "done") ++progress.done;
    }
    return progress;
}

std::optional<CrewSubtask> crew_current_subtask(const CrewRun& run) {
    // The first that is not done. The engine runs coders in parallel, so this is
    // "the earliest thing still outstanding" rather than literally the only one
    // being worked on -- which is the right thing to name in one line of panel.
    for (const auto& subtask : run.subtasks) {
        if (subtask.state != "done") return subtask;
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
