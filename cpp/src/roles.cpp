#include "auspex/roles.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/director.hpp"
#include "auspex/process.hpp"

namespace auspex {

namespace {

using nlohmann::json;

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

const std::vector<RolePersona>& builtin_personas() {
    static const std::vector<RolePersona> kPersonas{
        {"coder",
         "write or change code so the piece works",
         "Make the change. Call your tools to actually write the files -- describing "
         "what you would do is not doing it. Keep the diff to this piece only; when "
         "the files are written, finish.",
         false, false},

        {"tester",
         "add automated tests for the piece, without changing what is being tested",
         "Add or extend AUTOMATED TESTS only. Match the framework, layout and naming "
         "this project already uses -- look at an existing test before writing a new "
         "one, because a test in the wrong style is a test nobody runs. Do not change "
         "production code. When the tests are written, finish.",
         false, false},

        {"docs",
         "update documentation, comments or the changelog to match a change",
         "Update the documentation, README, comments or changelog so they match the "
         "change. Match the tone already in the file. Do NOT alter program logic -- if "
         "the code looks wrong, say so in your finishing note rather than fixing it "
         "here. When the writing is done, finish.",
         false, false},

        {"refactor",
         "restructure code without changing what it does",
         "Improve structure, naming and clarity WITHOUT changing observable behaviour "
         "and without adding features. Public interfaces stay as they are. If you "
         "cannot make a change without altering behaviour, leave it and say so in "
         "your finishing note. Keep the diff focused; when done, finish.",
         false, false},

        {"security",
         "fix concrete vulnerabilities the piece names",
         "Fix the concrete vulnerabilities this piece names -- injection, unsafe shell "
         "or eval, path traversal, weak validation, a hardcoded credential -- without "
         "breaking behaviour. Fix what is really there rather than hardening things "
         "at random. When the fixes are written, finish.",
         false, false},

        {"reviewer",
         "read and report; never edits",
         "Read the code and report what you find. You may not create, change or delete "
         "any file: the tools will refuse you, so do not spend turns trying. Put your "
         "findings in your finishing note -- that note is the whole product of this "
         "piece.",
         // Enforced in run_tool(), not merely stated above. The sentence exists so
         // the model does not waste its budget discovering the refusal one verb at
         // a time.
         true, false},
    };
    return kPersonas;
}

std::filesystem::path crew_roles_dir() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "auspex" / "crew-roles";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "auspex" / "crew-roles";
    }
    return {};
}

std::optional<RolePersona> parse_persona(const std::string& json_text,
                                         const std::string& fallback_name) {
    const json doc = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;

    RolePersona persona;
    persona.custom = true;
    persona.name = lowered(trim(doc.value("name", fallback_name)));
    if (persona.name.empty()) return std::nullopt;

    // Both spellings: ollamadev writes "desc", and the files on disk are theirs.
    persona.description = trim(doc.value("description", std::string{}));
    if (persona.description.empty()) {
        persona.description = trim(doc.value("desc", std::string{}));
    }
    persona.prompt = trim(doc.value("prompt", std::string{}));

    // "permission": "readonly" | "write". ABSENT LEAVES THE BASE VALUE ALONE, and
    // `permission_stated` is how merge_persona is told which happened. A plain
    // bool defaulting to false would mean a file setting only the prompt turns a
    // read-only reviewer into one that can write -- a permission changed by
    // omission.
    if (doc.contains("permission") && doc["permission"].is_string()) {
        persona.read_only =
            lowered(trim(doc["permission"].get<std::string>())) == "readonly";
        persona.permission_stated = true;
    }
    return persona;
}

RolePersona merge_persona(const RolePersona& base, const RolePersona& over) {
    RolePersona merged = base;
    merged.custom = true;
    if (!over.name.empty()) merged.name = over.name;
    if (!over.description.empty()) merged.description = over.description;
    if (!over.prompt.empty()) merged.prompt = over.prompt;
    // Only when the file actually said so. Silence keeps the base's permission --
    // see RolePersona::permission_stated for what happens without this check.
    if (over.permission_stated) {
        merged.read_only = over.read_only;
        merged.permission_stated = true;
    }
    return merged;
}

std::vector<std::filesystem::path> crew_roles_dirs() {
    std::vector<std::filesystem::path> dirs;
    if (const auto ours = crew_roles_dir(); !ours.empty()) dirs.push_back(ours);
    if (const char* home = std::getenv("HOME"); home && *home) {
        dirs.push_back(std::filesystem::path(home) / ".ollamadev" / "crew-roles");
    }
    return dirs;
}

std::vector<RolePersona> all_personas(const std::filesystem::path& dir) {
    std::vector<RolePersona> personas = builtin_personas();

    // An explicit directory means exactly that one -- the tests rely on it. Empty
    // means every place a role can live, ours before theirs.
    const std::vector<std::filesystem::path> dirs =
        dir.empty() ? crew_roles_dirs() : std::vector<std::filesystem::path>{dir};

    std::error_code ec;
    for (const auto& dir : dirs) {
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) continue;

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());   // same order twice

        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
            if (!in) continue;
            std::ostringstream buffer;
            buffer << in.rdbuf();

            const auto parsed = parse_persona(buffer.str(), file.stem().string());
            if (!parsed) continue;

            const auto at = std::find_if(
                personas.begin(), personas.end(),
                [&](const RolePersona& p) { return p.name == parsed->name; });
            if (at != personas.end()) {
                // An override of a built-in starts FROM the built-in, so a file
                // that sets only "prompt" keeps the description and the permission.
                // merge_persona is the single place that rule lives.
                *at = merge_persona(*at, *parsed);
            } else {
                personas.push_back(*parsed);
            }
        }
    }

    std::sort(personas.begin(), personas.end(),
              [](const RolePersona& a, const RolePersona& b) { return a.name < b.name; });
    return personas;
}

RolePersona persona_for(const std::string& role, const std::filesystem::path& dir) {
    const auto personas = all_personas(dir);
    const std::string wanted = lowered(trim(role));

    const auto at = std::find_if(personas.begin(), personas.end(),
                                 [&](const RolePersona& p) { return p.name == wanted; });
    if (at != personas.end()) return *at;

    // A Director that invented a role must not strand the subtask. parse_plan()
    // already collapses unknown roles to "coder", so this is the second net.
    const auto fallback =
        std::find_if(personas.begin(), personas.end(),
                     [](const RolePersona& p) { return p.name == "coder"; });
    if (fallback != personas.end()) return *fallback;
    return personas.empty() ? RolePersona{} : personas.front();
}

bool role_is_read_only(const std::string& role, const std::filesystem::path& dir) {
    return persona_for(role, dir).read_only;
}

std::string persona_block(const RolePersona& persona) {
    if (persona.prompt.empty()) return {};
    std::ostringstream out;
    out << "Your role is " << persona.name << ". " << persona.prompt << "\n";
    return out.str();
}

std::string role_catalog(const std::vector<RolePersona>& personas) {
    std::ostringstream out;
    for (const auto& persona : personas) {
        out << "- " << persona.name << ": "
            << (persona.description.empty() ? "no description" : persona.description)
            << (persona.read_only ? " (never edits)" : "") << "\n";
    }
    return out.str();
}

}  // namespace auspex
