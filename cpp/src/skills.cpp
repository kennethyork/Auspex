#include "auspex/skills.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "auspex/process.hpp"

namespace auspex {

namespace {

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// A name a coder can quote back without ambiguity: lower case, and anything that
// is not a letter, digit or dash becomes a dash. Runs collapse, edges trim.
std::string slugify(const std::string& name) {
    std::string slug;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            slug.push_back(static_cast<char>(std::tolower(c)));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

// ---------------------------------------------------------------------------
Skill parse_skill(const std::string& text, const std::string& folder_name) {
    Skill skill;
    skill.name = slugify(folder_name);

    const auto lines = [&text] {
        std::vector<std::string> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(line);
        }
        return out;
    }();

    std::size_t body_from = 0;

    // Frontmatter, but only when the file OPENS with it. A "---" further down is
    // a horizontal rule in somebody's markdown, and treating it as a fence would
    // swallow the first half of the instructions.
    if (!lines.empty() && trim(lines[0]) == "---") {
        std::size_t close = 0;
        for (std::size_t i = 1; i < lines.size(); ++i) {
            if (trim(lines[i]) == "---") {
                close = i;
                break;
            }
        }
        if (close > 0) {
            for (std::size_t i = 1; i < close; ++i) {
                const auto colon = lines[i].find(':');
                if (colon == std::string::npos) continue;
                const std::string key   = lowered(trim(lines[i].substr(0, colon)));
                std::string       value = trim(lines[i].substr(colon + 1));

                // Quotes are presentation in YAML; strip a matched pair so a
                // description does not arrive wearing them.
                if (value.size() >= 2 &&
                    ((value.front() == '"' && value.back() == '"') ||
                     (value.front() == '\'' && value.back() == '\''))) {
                    value = value.substr(1, value.size() - 2);
                }

                if (key == "name" && !value.empty())        skill.name = slugify(value);
                else if (key == "description")              skill.description = value;
            }
            body_from = close + 1;
        }
    }

    // The body, with leading blank lines dropped so it does not begin with the gap
    // the frontmatter left behind.
    std::string body;
    bool started = false;
    for (std::size_t i = body_from; i < lines.size(); ++i) {
        if (!started && trim(lines[i]).empty()) continue;
        started = true;
        body += lines[i];
        body += '\n';
    }
    skill.body = body;

    // No description in the frontmatter: the first line of the body, minus any
    // markdown heading marks. A skill with no description is invisible in a
    // catalogue, which is the same as not being installed.
    if (skill.description.empty()) {
        for (std::size_t i = body_from; i < lines.size(); ++i) {
            std::string candidate = trim(lines[i]);
            if (candidate.empty()) continue;
            while (!candidate.empty() && candidate.front() == '#') candidate.erase(0, 1);
            skill.description = trim(candidate);
            break;
        }
    }

    return skill;
}

std::vector<std::filesystem::path> skill_dirs(const std::filesystem::path& project) {
    std::vector<std::filesystem::path> dirs;

    // Most specific first. all_skills() takes the first writer, so a project skill
    // shadows a personal one of the same name and never the reverse -- the more
    // specific location is the one that should decide.
    if (!project.empty()) dirs.push_back(project / ".auspex" / "skills");

    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        dirs.push_back(std::filesystem::path(xdg) / "auspex" / "skills");
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        dirs.push_back(std::filesystem::path(home) / ".local" / "share" / "auspex" /
                       "skills");
    }
    return dirs;
}

std::vector<Skill> all_skills(const std::filesystem::path& project) {
    std::vector<Skill> skills;
    std::vector<std::string> seen;

    for (const auto& base : skill_dirs(project)) {
        std::error_code ec;
        if (!std::filesystem::is_directory(base, ec)) continue;

        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            if (!entry.is_directory(ec)) continue;

            const auto markdown = entry.path() / "SKILL.md";
            if (!std::filesystem::is_regular_file(markdown, ec)) continue;

            Skill skill = parse_skill(read_file(markdown),
                                      entry.path().filename().string());
            if (skill.name.empty()) continue;

            const std::string key = lowered(skill.name);
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);

            skill.dir = entry.path();
            skills.push_back(std::move(skill));
        }
    }

    std::sort(skills.begin(), skills.end(), [](const Skill& a, const Skill& b) {
        return lowered(a.name) < lowered(b.name);
    });
    return skills;
}

std::optional<Skill> find_skill(const std::filesystem::path& project,
                                const std::string& name) {
    const std::string wanted = lowered(slugify(name));
    if (wanted.empty()) return std::nullopt;

    for (const auto& skill : all_skills(project)) {
        if (lowered(skill.name) == wanted) return skill;
    }
    return std::nullopt;
}

std::string skills_catalog(const std::vector<Skill>& skills) {
    if (skills.empty()) return {};

    std::ostringstream out;
    for (const auto& skill : skills) {
        out << "  " << skill.name;
        if (!skill.description.empty()) out << " — " << skill.description;
        out << "\n";
    }
    return out.str();
}

}  // namespace auspex
