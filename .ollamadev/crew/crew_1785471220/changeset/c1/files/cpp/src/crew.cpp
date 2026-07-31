#include "auspex/crew.hpp"

#include <algorithm>

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

}  // namespace auspex
