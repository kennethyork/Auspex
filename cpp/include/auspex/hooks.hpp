// Your own gate, in front of the crew's.
//
// Everything else that can stop a coder was written by me and lives in this
// repository: the verb table, the allowlist, safe_join, the Auditor. This is the
// one that is yours. A hook is a program you name; it is handed what is about to
// happen and its exit status decides whether it happens.
//
// FAILS CLOSED, AND THAT IS THE ENTIRE POINT. A pre-tool hook that cannot be
// spawned, or that hangs past its deadline, BLOCKS the tool. A gate that opens
// when it breaks is not a gate. People install these to stop a model doing a
// specific thing, and "the hook crashed so we let it through" is the failure that
// makes the whole mechanism worthless.
//
// WHERE THE CONFIG MAY COME FROM -- read this before changing anything here.
// A hook is a program Auspex executes. Hooks are therefore read from the HOME
// config only: ~/.config/auspex/hooks.json. NEVER from a file inside the project
// being worked on. If a project-local hooks file were honoured, then merely
// pointing the crew at a repository somebody else wrote would run that
// repository's chosen program on the first tool call -- a clone would be code
// execution. ollamadev makes the same split for the same reason and its header
// says not to simplify it; this one says so too.
//
// ARGV, NOT A SHELL COMMAND. ollamadev's hooks are shell command strings. These
// are argv arrays, because "no shell anywhere" is an invariant this codebase
// actually holds and a hook is not worth breaking it for. The cost is that a hook
// cannot be a pipeline; the gain is that the tool name and file path handed to it
// are arguments, not text that gets re-parsed. A coder that writes a file called
// `; rm -rf ~` is then a coder with a strangely named file.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace auspex {

// When a hook fires.
enum class HookEvent {
    PreTool,       // before a coder's tool runs. THE ONLY ONE THAT CAN BLOCK.
    PostTool,      // after it ran. Informational.
    RunStart,      // a crew run is beginning
    RunEnd,        // it finished
    ChangesetReady // a coder produced a changeset, before the Auditor sees it
};

std::string_view hook_event_name(HookEvent event);
// The event of that name, or nullopt. Case-insensitive.
bool parse_hook_event(const std::string& name, HookEvent* out);
const std::vector<std::string>& hook_event_names();

struct Hook {
    HookEvent event = HookEvent::PreTool;
    // The program and its arguments. What the hook is told about the event is
    // appended to this, and also handed to it as JSON on stdin.
    std::vector<std::string> command;
    // Fires only when this appears in the subject -- the tool name for a tool
    // event, the project path otherwise. Empty fires always.
    //
    // A SUBSTRING, not a regex. A regex in a config file is a second language to
    // get wrong, and the mistakes are silent: a matcher that accidentally matches
    // nothing turns a gate into an ornament. Substring matching is what people
    // expect "match" to mean when they cannot see the code.
    std::string matcher;

    bool operator==(const Hook&) const = default;
};

// ~/.config/auspex/hooks.json. Deliberately its own file rather than a key in
// config.json: config.json is written by the settings window, and a stray write
// there must never be able to drop or alter the gates.
std::filesystem::path hooks_path();

// Every configured hook. A missing file, unreadable JSON, or a malformed entry
// yields no hooks for that entry -- never a partially-understood command line.
std::vector<Hook> load_hooks(const std::filesystem::path& path = hooks_path());

// Parsing exposed for testing, because the fail-closed behaviour has to be tested
// against text and not against a file somebody has to create first.
std::vector<Hook> parse_hooks(const std::string& json_text);

// Whether `hook` fires for this subject.
bool hook_matches(const Hook& hook, const std::string& subject);

struct HookOutcome {
    bool        blocked = false;
    std::string reason;   // what the hook said, handed back to the model
    bool        ran = false;   // a hook actually fired
};

// The gate. Returns blocked=true when the tool must NOT run.
//
// `subject` is the verb ("write", "run"), `detail` the path or command it names.
// Both go to the hook as arguments and in the JSON on stdin.
//
// With no hooks configured this is a no-op that allows -- the absence of a gate
// is not a closed gate, or nobody could use the crew at all. What fails closed is
// a hook that EXISTS and does not answer.
HookOutcome run_pre_tool_hooks(const std::string& subject, const std::string& detail,
                               const std::vector<Hook>& hooks,
                               int timeout_seconds = 10);

// The non-blocking events. Failures are swallowed: an informational hook that
// breaks must not take the run with it.
void fire_hook_event(HookEvent event, const std::string& subject,
                     const std::string& detail, const std::vector<Hook>& hooks,
                     int timeout_seconds = 10);

// The JSON a hook receives on stdin. Exposed so a test can pin the shape -- this
// is an interface other people write programs against, so changing it silently
// breaks their hooks.
std::string hook_payload(HookEvent event, const std::string& subject,
                         const std::string& detail);

// Human-readable listing, for a settings window and for `--hooks`.
std::string render_hooks(const std::vector<Hook>& hooks);

}  // namespace auspex
