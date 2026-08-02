#include "auspex/hooks.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

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

std::string_view hook_event_name(HookEvent event) {
    switch (event) {
        case HookEvent::PreTool:        return "pre_tool";
        case HookEvent::PostTool:       return "post_tool";
        case HookEvent::RunStart:       return "run_start";
        case HookEvent::RunEnd:         return "run_end";
        case HookEvent::ChangesetReady: return "changeset_ready";
    }
    return "pre_tool";
}

const std::vector<std::string>& hook_event_names() {
    static const std::vector<std::string> kNames{"pre_tool", "post_tool", "run_start",
                                                 "run_end", "changeset_ready"};
    return kNames;
}

bool parse_hook_event(const std::string& name, HookEvent* out) {
    const std::string n = lowered(trim(name));
    static const std::pair<const char*, HookEvent> kMap[] = {
        {"pre_tool", HookEvent::PreTool},
        {"post_tool", HookEvent::PostTool},
        {"run_start", HookEvent::RunStart},
        {"run_end", HookEvent::RunEnd},
        {"changeset_ready", HookEvent::ChangesetReady},
    };
    for (const auto& [text, event] : kMap) {
        if (n == text) {
            if (out) *out = event;
            return true;
        }
    }
    return false;
}

std::filesystem::path hooks_path() {
    // The HOME config, and only the home config. See the header: a project-local
    // hooks file would make cloning a repository equivalent to running it.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "auspex" / "hooks.json";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "auspex" / "hooks.json";
    }
    return {};
}

std::vector<Hook> parse_hooks(const std::string& json_text) {
    std::vector<Hook> hooks;
    const json doc = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) return hooks;

    // Either a bare array, or {"hooks": [...]}. Both spellings occur in the wild
    // and neither is worth an error message.
    const json* array = nullptr;
    if (doc.is_array()) {
        array = &doc;
    } else if (doc.is_object() && doc.contains("hooks") && doc["hooks"].is_array()) {
        array = &doc["hooks"];
    }
    if (!array) return hooks;

    for (const auto& entry : *array) {
        if (!entry.is_object()) continue;

        Hook hook;
        if (!entry.contains("event") || !entry["event"].is_string()) continue;
        if (!parse_hook_event(entry["event"].get<std::string>(), &hook.event)) continue;

        // The command must be an ARRAY. A string here is rejected rather than
        // split on spaces: splitting is what a shell does, and guessing where the
        // arguments are in `rm -rf "my files"` is how a gate becomes a hazard.
        if (!entry.contains("command") || !entry["command"].is_array()) continue;
        for (const auto& word : entry["command"]) {
            if (!word.is_string()) {
                hook.command.clear();
                break;
            }
            hook.command.push_back(word.get<std::string>());
        }
        if (hook.command.empty() || trim(hook.command[0]).empty()) continue;

        if (entry.contains("match") && entry["match"].is_string()) {
            hook.matcher = entry["match"].get<std::string>();
        }
        hooks.push_back(std::move(hook));
    }
    return hooks;
}

std::vector<Hook> load_hooks(const std::filesystem::path& path) {
    if (path.empty() || !std::filesystem::is_regular_file(path)) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_hooks(buffer.str());
}

bool hook_matches(const Hook& hook, const std::string& subject) {
    if (hook.matcher.empty()) return true;
    return lowered(subject).find(lowered(hook.matcher)) != std::string::npos;
}

std::string hook_payload(HookEvent event, const std::string& subject,
                         const std::string& detail) {
    json payload{
        {"event", std::string(hook_event_name(event))},
        {"subject", subject},
        {"detail", detail},
    };
    return payload.dump();
}

HookOutcome run_pre_tool_hooks(const std::string& subject, const std::string& detail,
                               const std::vector<Hook>& hooks, int timeout_seconds) {
    HookOutcome outcome;

    for (const auto& hook : hooks) {
        if (hook.event != HookEvent::PreTool) continue;
        if (!hook_matches(hook, subject)) continue;

        outcome.ran = true;

        std::vector<std::string> argv = hook.command;
        argv.push_back(subject);
        argv.push_back(detail);

        const LimitedResult ran =
            run_limited(argv, /*cwd=*/{}, timeout_seconds, /*max_output=*/4'000,
                        hook_payload(HookEvent::PreTool, subject, detail));

        if (ran.timed_out) {
            // Closed. A hook that never answers has not approved anything.
            outcome.blocked = true;
            outcome.reason = "blocked: the hook " + hook.command[0] + " did not answer in " +
                             std::to_string(timeout_seconds) + "s";
            return outcome;
        }

        // 127 with nothing said is our own child reporting that exec failed -- a
        // typo'd path, a file that is not executable. See process.cpp, which uses
        // 127 for exactly this. A real hook could of course choose to exit 127
        // itself; it would then have to also print nothing, and the only
        // difference either way is the wording, because both block.
        //
        // Worth distinguishing anyway: "could not be run" is the case a
        // "block only on non-zero" design gets wrong most often, because a hook
        // that is not there looks exactly like a hook that is not configured -- and
        // it is not. The user asked for a gate here.
        if (ran.exit_code < 0 || (ran.exit_code == 127 && trim(ran.output).empty())) {
            outcome.blocked = true;
            outcome.reason = "blocked: the hook " + hook.command[0] + " could not be run";
            return outcome;
        }

        if (!ran.ok) {
            outcome.blocked = true;
            const std::string said = trim(ran.output);
            outcome.reason = said.empty()
                                 ? "blocked by the hook " + hook.command[0] + " (exit " +
                                       std::to_string(ran.exit_code) + ")"
                                 : said;
            return outcome;
        }
    }

    return outcome;   // nothing blocked
}

void fire_hook_event(HookEvent event, const std::string& subject,
                     const std::string& detail, const std::vector<Hook>& hooks,
                     int timeout_seconds) {
    // PreTool goes through run_pre_tool_hooks, which is the only path that reads
    // the answer. Sending it here instead would run the gate and ignore it.
    if (event == HookEvent::PreTool) return;

    for (const auto& hook : hooks) {
        if (hook.event != event) continue;
        if (!hook_matches(hook, subject)) continue;

        std::vector<std::string> argv = hook.command;
        argv.push_back(subject);
        argv.push_back(detail);
        // The result is deliberately dropped. These events are notifications, and
        // a notification that can fail a run is not a notification.
        (void)run_limited(argv, /*cwd=*/{}, timeout_seconds, /*max_output=*/4'000,
                          hook_payload(event, subject, detail));
    }
}

std::string render_hooks(const std::vector<Hook>& hooks) {
    std::ostringstream out;
    if (hooks.empty()) {
        out << "no hooks configured (" << hooks_path().string() << ")\n";
        return out.str();
    }
    for (const auto& hook : hooks) {
        out << "  " << hook_event_name(hook.event);
        if (!hook.matcher.empty()) out << " [match: " << hook.matcher << "]";
        out << "\n      ";
        for (std::size_t i = 0; i < hook.command.size(); ++i) {
            out << (i ? " " : "") << hook.command[i];
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace auspex
