// Skills: instructions a coder opens only when it needs them.
//
// A skill is a folder with a SKILL.md in it. The frontmatter carries a name and a
// one-line description; the body is however many pages of instruction the job
// needs -- a house style, a release checklist, how this project's migrations work.
//
// PROGRESSIVE DISCLOSURE IS THE WHOLE IDEA. Every coder prompt carries the
// CATALOGUE -- one line per skill -- which costs a few dozen tokens. A coder that
// decides a skill is relevant opens it with a verb, and only then does the body
// arrive. Putting every skill's full text in every prompt would spend the context
// window on instructions that do not apply, which is exactly the budget the
// coder needs for the code.
//
// WHERE THEY COME FROM: <project>/.auspex/skills, then ~/.local/share/auspex/skills.
// First writer wins, so a project can override a personal skill of the same name
// and never the other way round -- the more specific location is the one that
// should decide.
//
// PARSING IS PURE. Reading a SKILL.md is text-in/values-out and tested; only
// discovery touches the disk.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct Skill {
    std::string name;          // slug, as the coder names it
    std::string description;   // one line, for the catalogue
    std::string body;          // the instructions, without the frontmatter
    std::filesystem::path dir; // where it lives

    bool operator==(const Skill&) const = default;
};

// Parses a SKILL.md.
//
// Frontmatter is a `---` fenced block of `key: value` at the very top. A file
// without one is still a skill: its name falls back to the folder's and its
// description to the first non-empty line, because a useful skill written by
// somebody who did not read the format is better than an error.
Skill parse_skill(const std::string& text, const std::string& folder_name);

// Where skills are looked for, most specific first.
std::vector<std::filesystem::path> skill_dirs(const std::filesystem::path& project);

// Every skill visible from `project`, sorted by name, first writer winning.
std::vector<Skill> all_skills(const std::filesystem::path& project);

// One skill by name, or nothing. Case-insensitive, because the coder is quoting a
// catalogue line back and its case is not the point.
std::optional<Skill> find_skill(const std::filesystem::path& project,
                                const std::string& name);

// The catalogue: one line per skill, for a prompt. Empty when there are none, so
// nothing is added to the prompt at all rather than an empty heading.
std::string skills_catalog(const std::vector<Skill>& skills);

}  // namespace auspex
