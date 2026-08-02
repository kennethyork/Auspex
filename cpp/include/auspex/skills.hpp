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

// --- the starter library ------------------------------------------------------
//
// Skills only helped if you had already written some, which meant the feature did
// nothing at all on a fresh install -- and the coders that need instruction most
// are exactly the small local models whose owners have not yet written a house
// style document. These ship in the binary.
//
// MATCHED TO THE TASK, not loaded wholesale. A crew fixing a SQL query has no use
// for the accessibility skill, and putting all of them in the catalogue would
// spend the context this feature exists to save. Each starter carries trigger
// words; the ones whose triggers appear in the task text get written into the
// sandbox, where the coder discovers them exactly like a project skill.
//
// A USER SKILL OF THE SAME NAME ALWAYS WINS, and is never overwritten. Somebody
// who has written their own `testing` skill has said what they want; shipping one
// over the top of it would be the library deciding it knows better.
struct SkillSpec {
    std::string              name;
    // Lowercase. Matched as substrings of the task text.
    std::vector<std::string> triggers;
    std::string              description;
    std::string              body;

    bool operator==(const SkillSpec&) const = default;
};

const std::vector<SkillSpec>& starter_skills();

// The starters whose triggers appear in `focus`, most specific first, capped.
//
// Specificity is the LENGTH of the longest matching trigger: "database migration"
// occurring is a far stronger signal than "api", so when the cap bites it is the
// vague matches that are dropped. A cap of 0, or an empty focus, matches nothing
// -- silence rather than everything, because the failure mode of a wrong guess
// here is a coder reading five irrelevant documents.
std::vector<SkillSpec> skills_for_focus(const std::string& focus, int cap = 4);

// Write each spec to `base/.auspex/skills/<name>/SKILL.md`. Returns the names now
// available there.
//
// Skips any name the user already defines (in `home_skills`, and at the target),
// so a personal skill is never clobbered by a shipped one.
std::vector<std::string> materialize_skills(const std::vector<SkillSpec>& specs,
                                            const std::filesystem::path& base,
                                            const std::vector<Skill>& user_skills = {});

}  // namespace auspex
