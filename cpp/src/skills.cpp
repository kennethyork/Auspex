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

// --- the starter library ------------------------------------------------------

const std::vector<SkillSpec>& starter_skills() {
    static const std::vector<SkillSpec> kLibrary{
        {"python-style",
         {"python", "py file", "django", "flask", "fastapi", "pytest", "pandas",
          "numpy"},
         "write Python the way this ecosystem expects",
         "# python-style\n\n"
         "- Follow PEP 8: four spaces, snake_case for functions and variables,\n"
         "  CapWords for classes.\n"
         "- Type-hint public functions. `def f(a: int, b: int) -> int:` costs one\n"
         "  line and documents more than a paragraph would.\n"
         "- Prefer a specific exception to a bare `except:`. Catching everything\n"
         "  catches the KeyboardInterrupt you wanted to work.\n"
         "- Use pathlib over string paths, and a context manager over an explicit\n"
         "  close().\n"
         "- No mutable default arguments. `def f(x=[])` shares that list between\n"
         "  every call, and the bug shows up weeks later.\n"},

        {"testing",
         {"test", "tests", "unit test", "coverage", "pytest", "jest", "vitest",
          "junit", "rspec", "regression"},
         "write tests that fail for the right reason",
         "# testing\n\n"
         "- Read an existing test before writing a new one. A test in the wrong\n"
         "  style is a test nobody runs.\n"
         "- One behaviour per test, named for the behaviour: `test_empty_list_\n"
         "  returns_zero`, not `test_2`.\n"
         "- Assert on the value, not on the type or on `is not None`. An assertion\n"
         "  that cannot fail is documentation pretending to be a test.\n"
         "- Cover the edge: empty, one element, the boundary, the negative case.\n"
         "  The happy path is the one already known to work.\n"
         "- A test that needs the network, the clock or the filesystem is a test\n"
         "  that fails on somebody else's machine. Inject those.\n"},

        {"error-handling",
         {"error", "exception", "retry", "timeout", "robust", "failure", "crash",
          "validation"},
         "fail in a way somebody can act on",
         "# error-handling\n\n"
         "- Fail closed on anything you cannot verify. \"I could not tell\" and\n"
         "  \"it is fine\" are the same answer only if you treat them the same way.\n"
         "- An error message names what was being done, what was expected, and\n"
         "  what was found. \"Invalid input\" names none of the three.\n"
         "- Do not swallow an exception to keep going unless you can say, in a\n"
         "  comment, why continuing is correct.\n"
         "- Validate at the boundary -- where data enters -- not at every layer\n"
         "  after it.\n"
         "- Anything with a timeout needs a decision about what happens when it\n"
         "  fires. A timeout with no handler is a hang with extra steps.\n"},

        {"sql-and-data",
         {"sql", "database", "query", "postgres", "mysql", "sqlite", "migration",
          "schema", "orm", "index"},
         "queries and schemas that survive contact with real data",
         "# sql-and-data\n\n"
         "- Parameterise every query. String concatenation with user input is the\n"
         "  injection, whatever the ORM around it looks like.\n"
         "- Name the columns in a SELECT. `SELECT *` breaks the day somebody adds\n"
         "  a column, and it is never obvious which day that was.\n"
         "- A migration must be reversible, or must say in a comment why it is not.\n"
         "- Index what you filter and join on -- and know that every index costs\n"
         "  write time.\n"
         "- Decide what NULL means in a column before you allow it. \"Unknown\" and\n"
         "  \"none\" are different, and one column cannot be both.\n"},

        {"web-frontend",
         {"html", "css", "frontend", "website", "web app", "landing page", "react",
          "vue", "svelte", "responsive", "ui", "page"},
         "layouts that work on a phone and on a desktop",
         "# web-frontend\n\n"
         "- Design mobile-first and add at larger breakpoints. The other direction\n"
         "  means removing things under pressure.\n"
         "- Fluid units -- %, rem, clamp(), min/max -- and grid or flex, over fixed\n"
         "  pixel widths.\n"
         "- Nothing may scroll the page horizontally. Wide content scrolls inside\n"
         "  its own container.\n"
         "- Real <button> and <a> for actions and links, never a clickable <div>:\n"
         "  the real ones are focusable and announce themselves.\n"
         "- Tap targets at least 44px, and nothing essential behind a hover.\n"},

        {"accessibility",
         {"accessib", "a11y", "wcag", "screen reader", "aria", "contrast", "form",
          "website", "web app"},
         "so the thing can actually be used",
         "# accessibility\n\n"
         "- Every form control has a <label for>. A placeholder is not a label; it\n"
         "  disappears exactly when it is needed.\n"
         "- One <h1> per page, and do not skip heading levels -- headings are how\n"
         "  a screen reader user navigates.\n"
         "- Alt text on meaningful images, alt=\"\" on decorative ones. Both are\n"
         "  decisions; a missing alt is neither.\n"
         "- Contrast at least 4.5:1 for body text. Check it rather than judging by\n"
         "  eye on a good monitor.\n"
         "- Everything reachable by mouse must be reachable by keyboard, with a\n"
         "  visible focus ring. Do not remove the outline without replacing it.\n"},

        {"api-design",
         {"api", "endpoint", "rest", "http", "json", "route", "graphql", "client",
          "server"},
         "interfaces somebody else can use without asking you",
         "# api-design\n\n"
         "- Status codes mean things: 400 the caller is wrong, 404 it is not here,\n"
         "  409 it conflicts, 500 we are wrong. A 200 with an error in the body is\n"
         "  a lie every client has to special-case.\n"
         "- Name resources as nouns and use the verb the method already gives you.\n"
         "- Never break a response shape in place. Add a field, or add a version.\n"
         "- Paginate anything that can grow. A list endpoint with no limit is an\n"
         "  outage waiting for the data to arrive.\n"
         "- An error body should carry a stable code as well as a human sentence.\n"
         "  Clients match on the code; people read the sentence.\n"},

        {"security-basics",
         {"security", "auth", "password", "token", "secret", "credential", "crypto",
          "encrypt", "session", "login", "permission"},
         "the mistakes that actually get exploited",
         "# security-basics\n\n"
         "- Never hardcode a credential. Read it from the environment or from a\n"
         "  secret store, and make sure it cannot land in a log line.\n"
         "- Build commands as an argument array, never by pasting strings into a\n"
         "  shell. Then a filename containing `;` is a filename.\n"
         "- Resolve and check every path that came from outside before opening it.\n"
         "  \"..\" is a valid filename until you decide it is not.\n"
         "- Hash passwords with bcrypt, scrypt or argon2. Not SHA-256, which is\n"
         "  fast, which is the problem.\n"
         "- Do not invent crypto, and do not roll your own token format. Use the\n"
         "  library the platform already ships.\n"},

        {"concurrency",
         {"thread", "concurren", "async", "parallel", "race", "lock", "mutex",
          "deadlock", "atomic", "worker"},
         "shared state, and how it goes wrong",
         "# concurrency\n\n"
         "- Say what each piece of shared state is protected by, in a comment next\n"
         "  to it. State with no stated owner is state with a race in its future.\n"
         "- Hold a lock for the shortest possible time, and never call out to\n"
         "  unknown code while holding one.\n"
         "- Take locks in one fixed order everywhere. Two orders is a deadlock.\n"
         "- Prefer handing ownership over sharing it: a queue is easier to reason\n"
         "  about than a mutex.\n"
         "- A test that passes once proves nothing here. Race conditions are the\n"
         "  bugs that pass in CI and fail on the user's machine.\n"},

        {"refactoring",
         {"refactor", "clean up", "restructure", "rename", "tidy", "simplify",
          "extract", "dead code", "duplicate"},
         "change the shape without changing the behaviour",
         "# refactoring\n\n"
         "- Behaviour does not change. If you find a bug while restructuring, say\n"
         "  so and leave it -- a fix hidden inside a refactor is a fix nobody\n"
         "  reviewed.\n"
         "- One kind of change per pass. Renaming and moving at the same time makes\n"
         "  a diff nobody can read.\n"
         "- Keep the public interface unless the piece asked you to change it.\n"
         "- Delete dead code rather than commenting it out. It is in the history.\n"
         "- If there is no test covering what you are about to move, the honest\n"
         "  order is: test first, then move.\n"},

        {"shell-and-cli",
         {"cli", "command line", "shell", "bash", "script", "argv", "terminal",
          "flag", "subcommand"},
         "programs run by people and by other programs",
         "# shell-and-cli\n\n"
         "- Exit zero on success and non-zero on failure, always. Everything that\n"
         "  automates your tool reads that number and nothing else.\n"
         "- Errors go to stderr, results to stdout, so the output can be piped\n"
         "  without the diagnostics landing in it.\n"
         "- Quote every variable expansion in a shell script. An unquoted $VAR\n"
         "  containing a space is two arguments.\n"
         "- Set the failure modes explicitly at the top -- `set -euo pipefail` in\n"
         "  bash -- rather than hoping each command is checked.\n"
         "- A destructive action gets a confirmation or a --yes flag. Both, if it\n"
         "  cannot be undone.\n"},

        {"documentation",
         {"readme", "docs", "documentation", "changelog", "comment", "docstring",
          "guide", "tutorial"},
         "write the part the code cannot say",
         "# documentation\n\n"
         "- Say WHY, not what. The code already says what it does; it cannot say\n"
         "  which alternative was rejected and for what reason.\n"
         "- A README opens with what the thing is and how to run it. Not with a\n"
         "  badge wall.\n"
         "- Every example must actually run. An example that has drifted is worse\n"
         "  than none, because it is trusted.\n"
         "- Document the surprising, not the obvious. A comment on an obvious line\n"
         "  trains the reader to skip comments.\n"
         "- Match the tone already in the file. A document in two voices reads as\n"
         "  unmaintained.\n"},
    };
    return kLibrary;
}

namespace {

// Which of `library` matches `text`, most specific first.
//
// Shared by both libraries because the rule is the same one: score is the LENGTH
// of the longest trigger that occurs, so a long trigger ("progressive web app")
// beats a short one ("api") and the vague matches are what the cap drops.
std::vector<SkillSpec> matching(const std::vector<SkillSpec>& library,
                                const std::string& text, int cap) {
    std::vector<SkillSpec> chosen;
    if (text.empty() || cap <= 0) return chosen;

    std::vector<std::pair<std::size_t, const SkillSpec*>> hits;
    for (const auto& spec : library) {
        std::size_t best = 0;
        for (const auto& trigger : spec.triggers) {
            if (text.find(trigger) != std::string::npos) {
                best = std::max(best, trigger.size());
            }
        }
        if (best > 0) hits.emplace_back(best, &spec);
    }

    // stable_sort, so two equally specific matches keep the library's order and
    // the same text picks the same skills twice.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& [score, spec] : hits) {
        if (static_cast<int>(chosen.size()) >= cap) break;
        (void)score;
        chosen.push_back(*spec);
    }
    return chosen;
}

}  // namespace

std::vector<SkillSpec> project_starters_for(const std::string& focus, int cap) {
    return matching(project_starters(), lowered(trim(focus)), cap);
}

std::vector<SkillSpec> skills_for_focus(const std::string& focus, int cap) {
    return matching(starter_skills(), lowered(trim(focus)), cap);
}

std::vector<std::string> materialize_skills(const std::vector<SkillSpec>& specs,
                                            const std::filesystem::path& base,
                                            const std::vector<Skill>& user_skills) {
    std::vector<std::string> present;
    if (base.empty()) return present;

    for (const auto& spec : specs) {
        if (spec.name.empty()) continue;

        // A skill the user has written wins outright, and is not touched. They
        // have already said what they want a skill of that name to say.
        const bool user_has = std::any_of(
            user_skills.begin(), user_skills.end(),
            [&](const Skill& s) { return lowered(s.name) == lowered(spec.name); });
        if (user_has) {
            present.push_back(spec.name);
            continue;
        }

        const auto target = base / ".auspex" / "skills" / spec.name / "SKILL.md";
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            present.push_back(spec.name);   // already there; leave it alone
            continue;
        }
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) continue;

        std::ofstream out(target, std::ios::trunc);
        if (!out) continue;
        out << "---\nname: " << spec.name << "\ndescription: " << spec.description
            << "\n---\n\n"
            << spec.body;
        if (out) present.push_back(spec.name);
    }
    return present;
}

}  // namespace auspex
