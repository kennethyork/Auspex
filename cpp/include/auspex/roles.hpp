// What a role actually means.
//
// The Director picks a role per subtask from a fixed list -- coder, tester, docs,
// refactor, security, reviewer. Until now that choice changed exactly one word of
// the coder's prompt: "You are a tester". A Director carefully deciding that a
// piece is documentation work, and a coder then behaving identically, is a
// decision that costs a model call and buys nothing.
//
// A persona is the instructions that come with the role. A tester is told to
// match the project's existing test framework and not to touch production code; a
// refactorer is told not to change observable behaviour. That is the difference
// between a label and a role.
//
// READ-ONLY IS ENFORCED, NOT REQUESTED. ollamadev's reviewer role asks the model
// not to write files, because its permission mode is process-global and crew
// coders run concurrently, so it cannot do more. Auspex already has a per-coder
// CoderLimits::read_only checked inside run_tool(), so a read-only role here is a
// refusal rather than a request: the reviewer that decides to edit something gets
// told no, instead of being trusted not to ask.
//
// USER ROLES override built-ins by name, from ~/.local/share/auspex/crew-roles/
// <name>.json. Only fields present are replaced, so changing one line of a
// persona does not mean restating the rest of it.
//
// A role file is a PROMPT, never a command -- unlike a hook, nothing here is
// executed. It still may not turn a writing role into a read-only one silently,
// or the reverse: see RolePersona::permission_stated.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct RolePersona {
    std::string name;
    // What the Director sees when it is choosing. One line.
    std::string description;
    // The instructions injected into that coder's prompt.
    std::string prompt;
    // Refuses every writing verb, in run_tool() rather than in the prompt.
    bool        read_only = false;
    // Whether a role FILE said anything about permission at all.
    //
    // A tri-state pretending to be two bools, and it has to be. Without it a file
    // that sets only "prompt" arrives with read_only defaulting to false, and
    // merging that over the built-in reviewer silently turns a role that cannot
    // write into one that can. That is a security setting changed by omission,
    // which is the worst way to change one -- and it is what this code did until
    // a test caught it.
    bool        permission_stated = false;
    // Came from a file rather than from the table below.
    bool        custom = false;

    bool operator==(const RolePersona&) const = default;
};

// The built-in personas, one per entry in director_roles().
//
// Kept in the same order and the same names, because the Director's fixed list is
// what a plan is validated against -- a persona for a role the Director cannot
// name would never be used, and a role with no persona would silently be a label
// again.
const std::vector<RolePersona>& builtin_personas();

// ~/.local/share/auspex/crew-roles, or $XDG_DATA_HOME's equivalent.
std::filesystem::path crew_roles_dir();

// Every directory a role file may live in, most specific first: ours, then
// ollamadev's ~/.ollamadev/crew-roles.
//
// Reading theirs matters more than it looks. Four custom roles were sitting there
// -- architect, debugger, perf, reviewer -- written by somebody who had already
// decided what they wanted their crew to be, and Auspex could not see one of
// them. A replacement that silently has fewer roles than the thing it replaces is
// not a replacement.
std::vector<std::filesystem::path> crew_roles_dirs();

// Built-ins with any user file merged over them, sorted by name.
std::vector<RolePersona> all_personas(const std::filesystem::path& dir = {});

// Parsing separated from the disk, so the merge rules are testable without
// creating files in somebody's home directory.
std::optional<RolePersona> parse_persona(const std::string& json_text,
                                         const std::string& fallback_name);

// Merge one override onto a base persona. Absent fields keep the base's value.
RolePersona merge_persona(const RolePersona& base, const RolePersona& over);

// The persona for a role name. An unknown role falls back to `coder` -- a
// Director that invented a role must never strand a subtask without one.
RolePersona persona_for(const std::string& role,
                        const std::filesystem::path& dir = {});

// True when this role may not write. Read through the same override path as
// everything else, so a user role can add a read-only reviewer of its own.
bool role_is_read_only(const std::string& role,
                       const std::filesystem::path& dir = {});

// The block appended to a coder's prompt. Empty for a persona with no prompt,
// which adds nothing rather than an empty heading.
std::string persona_block(const RolePersona& persona);

// "- name: description" lines, for the Director's prompt. This is what makes the
// role choice informed rather than a guess at what the words mean.
std::string role_catalog(const std::vector<RolePersona>& personas);

}  // namespace auspex
