#include "auspex/commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace auspex {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

fs::path home_dir() {
    if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home);
    return fs::path("/");
}

// Extract the first balanced {...} run. Ollama's format:"json" normally returns a
// bare object, but a reasoning model can still wrap it, and depending on the model
// the braces may be preceded by prose. Scanning for balance is more robust than
// find('{')/rfind('}'), which would swallow trailing garbage.
std::optional<std::string> first_json_object(const std::string& text) {
    int depth = 0;
    std::size_t start = std::string::npos;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (in_string) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  in_string = false;
            continue;
        }

        if (c == '"') { in_string = true; continue; }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0 && --depth == 0 && start != std::string::npos) {
                return text.substr(start, i - start + 1);
            }
        }
    }
    return std::nullopt;
}

// open_path hands the target to xdg-open, which delegates to whatever handler is
// registered for it. For most files that means a viewer, but a .desktop entry or
// an executable script can be *launched* rather than opened -- the widest edge in
// the whitelist. Directories and ordinary documents are allowed; anything that
// could execute is refused, so open_path can never become a launch primitive with
// a model-chosen argument.
//
// Returns a reason to refuse, or nullopt to allow.
std::optional<std::string> refuse_to_open(const fs::path& path) {
    std::error_code ec;

    if (fs::is_directory(path, ec)) return std::nullopt;   // always fine

    if (!fs::is_regular_file(path, ec)) {
        return "that is not a file or folder I can open";
    }

    // .desktop entries are launched by the file manager / xdg-open.
    const std::string ext = lower(path.extension().string());
    if (ext == ".desktop") {
        return "I will not open desktop entries, since that launches them";
    }

    // Interpreted-script extensions, which some handlers run rather than display.
    for (const char* risky : {".sh", ".bash", ".zsh", ".py", ".pl", ".rb", ".php",
                              ".lua", ".js", ".appimage", ".run", ".bin", ".jar"}) {
        if (ext == risky) return "I will not open " + ext + " files, since they can run";
    }

    // Any executable bit set: an ELF binary or a script with a shebang.
    const auto perms = fs::status(path, ec).permissions();
    if (!ec && (perms & (fs::perms::owner_exec | fs::perms::group_exec |
                         fs::perms::others_exec)) != fs::perms::none) {
        return "I will not open executable files";
    }

    return std::nullopt;
}

const WindowEntry* match_window(const std::vector<WindowEntry>& windows,
                                std::string_view wanted) {
    const std::string needle = lower(wanted);
    if (needle.empty()) return nullptr;

    // Exact (case-insensitive) beats substring, so "Files" does not lose to a
    // window called "Files and Folders Help".
    for (const auto& window : windows) {
        if (lower(window.title) == needle) return &window;
    }
    for (const auto& window : windows) {
        if (lower(window.title).find(needle) != std::string::npos) return &window;
    }
    return nullptr;
}

}  // namespace

std::string_view to_string(ActionKind kind) {
    switch (kind) {
        case ActionKind::Answer:          return "answer";
        case ActionKind::OpenPath:        return "open_path";
        case ActionKind::LaunchApp:       return "launch_app";
        case ActionKind::SwitchWorkspace: return "switch_workspace";
        case ActionKind::FocusWindow:     return "focus_window";
        case ActionKind::SetVolume:       return "set_volume";
        case ActionKind::OpenUrl:         return "open_url";
        case ActionKind::WebSearch:       return "web_search";
    }
    return "answer";
}

std::optional<fs::path> resolve_path(std::string_view spoken) {
    if (spoken.empty()) return std::nullopt;

    std::string text(spoken);
    if (text == "~" || text.starts_with("~/")) {
        text = (home_dir() / text.substr(text.size() > 1 ? 2 : 1)).string();
    } else if (!text.starts_with("/")) {
        // A bare name is interpreted relative to home, which is what someone means
        // by "open downloads".
        text = (home_dir() / text).string();
    }

    std::error_code ec;
    fs::path candidate(text);
    if (fs::exists(candidate, ec)) return fs::weakly_canonical(candidate, ec);

    // Retry the last component case-insensitively: models say "downloads", the
    // directory is "Downloads".
    const fs::path parent = candidate.parent_path();
    const std::string wanted = lower(candidate.filename().string());
    if (wanted.empty() || !fs::is_directory(parent, ec)) return std::nullopt;

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (lower(entry.path().filename().string()) == wanted) {
            return fs::weakly_canonical(entry.path(), ec);
        }
    }
    return std::nullopt;
}

CommandContext gather_context(const Config& config) {
    CommandContext context;
    context.workspace_count = config.workspace_count > 0 ? config.workspace_count : 4;
    context.windows         = list_user_windows();
    context.browser         = config.browser;
    context.search_url      = config.search_url;

    if (const auto focused = run({"xdotool", "getactivewindow", "getwindowname"});
        focused.ok) {
        context.focused_window = trim(focused.out);
    }
    if (const auto selected = run({"xclip", "-o", "-selection", "primary"}); selected.ok) {
        context.selection = trim(selected.out);
    }
    return context;
}

std::string build_command_prompt(const std::string& utterance,
                                 const CommandContext& context) {
    std::ostringstream prompt;

    prompt << "You control a Linux desktop. Convert the user's request into ONE JSON "
              "object and output nothing else.\n\n"
              "Allowed actions:\n"
              "  {\"action\":\"open_path\",\"target\":\"<file or folder path>\"}\n"
              "  {\"action\":\"launch_app\",\"target\":\"<executable name>\"}\n"
              "  {\"action\":\"switch_workspace\",\"number\":<int>}\n"
              "  {\"action\":\"focus_window\",\"target\":\"<window title>\"}\n"
              "  {\"action\":\"set_volume\",\"number\":<0-100>}\n"
              "  {\"action\":\"open_url\",\"target\":\"<http(s) URL>\"}\n"
              "  {\"action\":\"web_search\",\"target\":\"<search terms>\"}\n"
              "  {\"action\":\"answer\",\"target\":\"<spoken reply>\"}\n\n"
              "Rules:\n"
              "- Use \"answer\" for questions, or when the request is not one of the "
              "actions above. Keep answers to one or two sentences; they are spoken "
              "aloud.\n"
              "- Paths may use ~ for home. Prefer paths over app names for folders.\n"
              "- For focus_window, copy a title from the open windows list exactly.\n"
              "- Workspaces are numbered from 1 as the user says them.\n"
              "- For a named site use open_url with a full https:// URL. Use "
              "web_search only when there is no specific site.\n\n";

    prompt << "Workspaces available: 1 to " << context.workspace_count << "\n";

    if (!context.windows.empty()) {
        prompt << "Open windows:\n";
        for (const auto& window : context.windows) {
            prompt << "  - " << window.title << "\n";
        }
    }
    if (!context.focused_window.empty()) {
        prompt << "Focused window: " << context.focused_window << "\n";
    }
    if (!context.selection.empty()) {
        // Truncated: a large selection would dominate the context window.
        const std::string snippet = context.selection.size() > 500
                                        ? context.selection.substr(0, 500) + "..."
                                        : context.selection;
        prompt << "Selected text: " << snippet << "\n";
    }

    if (!context.history.empty()) {
        prompt << "\nEarlier in this conversation:\n";
        for (const auto& [question, answer] : context.history) {
            prompt << "  user: " << question << "\n  you: " << answer << "\n";
        }
    }

    prompt << "\nRequest: " << utterance << "\n";
    return prompt.str();
}

ParseResult parse_action(const std::string& model_output, const CommandContext& context) {
    ParseResult result;

    const auto object = first_json_object(model_output);
    if (!object) {
        result.error = "the model did not return JSON";
        return result;
    }

    const json parsed = json::parse(*object, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        result.error = "the model returned malformed JSON";
        return result;
    }

    const std::string kind = lower(parsed.value("action", std::string{}));
    const std::string target = trim(parsed.value("target", std::string{}));

    // "number" may arrive as a JSON number or a quoted string.
    int number = 0;
    if (const auto it = parsed.find("number"); it != parsed.end()) {
        if (it->is_number_integer())      number = it->get<int>();
        else if (it->is_number_float())   number = static_cast<int>(it->get<double>());
        else if (it->is_string())         number = std::atoi(it->get<std::string>().c_str());
    }

    if (kind == "open_path") {
        const auto resolved = resolve_path(target);
        if (!resolved) {
            result.error = "I could not find " + (target.empty() ? "that path" : target);
            return result;
        }
        if (const auto why = refuse_to_open(*resolved)) {
            result.error = *why;
            return result;
        }
        result.action = Action{ActionKind::OpenPath, resolved->string(), 0};
        return result;
    }

    if (kind == "launch_app") {
        if (!in_path(target)) {
            result.error = (target.empty() ? std::string("that application") : target) +
                           " is not installed";
            return result;
        }
        result.action = Action{ActionKind::LaunchApp, target, 0};
        return result;
    }

    if (kind == "switch_workspace") {
        // Spoken as 1-based, wmctrl is 0-based.
        const int index = number - 1;
        if (index < 0 || index >= context.workspace_count) {
            result.error = "there is no workspace " + std::to_string(number);
            return result;
        }
        result.action = Action{ActionKind::SwitchWorkspace, {}, index};
        return result;
    }

    if (kind == "focus_window") {
        const WindowEntry* window = match_window(context.windows, target);
        if (!window) {
            result.error = "I do not see a window called " +
                           (target.empty() ? std::string("that") : target);
            return result;
        }
        // Carry the resolved id, never a model-supplied one.
        result.action = Action{ActionKind::FocusWindow, window->id, 0};
        return result;
    }

    if (kind == "set_volume") {
        if (number < 0) number = 0;
        if (number > 100) number = 100;
        result.action = Action{ActionKind::SetVolume, {}, number};
        return result;
    }

    if (kind == "open_url") {
        // Scheme allow-list. Without it a model could emit file:// or, worse,
        // something a helper interprets, and xdg-open would hand it to whatever
        // handler is registered.
        const std::string low = lower(target);
        if (!low.starts_with("http://") && !low.starts_with("https://")) {
            result.error = "I can only open http and https addresses";
            return result;
        }
        // Whitespace in a URL means the model concatenated prose; refuse rather
        // than open something unintended.
        if (target.find_first_of(" \t\n") != std::string::npos) {
            result.error = "that did not look like a single address";
            return result;
        }
        result.action = Action{ActionKind::OpenUrl, target, 0};
        return result;
    }

    if (kind == "web_search") {
        if (target.empty()) {
            result.error = "I did not catch what to search for";
            return result;
        }
        result.action = Action{ActionKind::WebSearch, target, 0};
        return result;
    }

    if (kind == "answer") {
        if (target.empty()) {
            result.error = "the model returned an empty answer";
            return result;
        }
        result.action = Action{ActionKind::Answer, target, 0};
        return result;
    }

    // Unknown verb: fail closed. Never fall through to execution.
    result.error = "I do not know how to do that";
    return result;
}

namespace {

// Percent-encode everything outside the unreserved set, so a query with spaces,
// ampersands or quotes cannot alter the URL's structure.
std::string url_encode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool open_in_browser(const CommandContext& context, const std::string& url) {
    // xdg-open respects the user's chosen default browser, so it is preferred over
    // naming one. Config::resolve_commands() already put it first when present.
    const std::string opener = context.browser.empty() ? "xdg-open" : context.browser;
    return spawn_detached({opener, url});
}

}  // namespace

ExecResult execute_action(const Action& action, const CommandContext& context) {
    switch (action.kind) {
        case ActionKind::Answer:
            return {true, action.target};

        case ActionKind::OpenPath:
            if (!spawn_detached({"xdg-open", action.target})) {
                return {false, "I could not open that"};
            }
            return {true, "Opening " + fs::path(action.target).filename().string()};

        case ActionKind::LaunchApp:
            if (!spawn_detached({action.target})) {
                return {false, "I could not start " + action.target};
            }
            return {true, "Starting " + action.target};

        case ActionKind::SwitchWorkspace: {
            if (!switch_workspace(action.number)) {
                return {false, "I could not switch workspace"};
            }
            return {true, "Workspace " + std::to_string(action.number + 1)};
        }

        case ActionKind::FocusWindow: {
            // action.target holds the id resolved at parse time; re-derive a name
            // for the spoken confirmation.
            std::string name;
            for (const auto& window : context.windows) {
                if (window.id == action.target) { name = window.title; break; }
            }
            if (!activate_window(action.target)) {
                return {false, "I could not raise that window"};
            }
            return {true, name.empty() ? "Raised" : "Raised " + name};
        }

        case ActionKind::OpenUrl:
            if (!open_in_browser(context, action.target)) {
                return {false, "I could not open the browser"};
            }
            return {true, "Opening " + action.target};

        case ActionKind::WebSearch: {
            const std::string url = context.search_url + url_encode(action.target);
            if (!open_in_browser(context, url)) {
                return {false, "I could not open the browser"};
            }
            return {true, "Searching for " + action.target};
        }

        case ActionKind::SetVolume: {
            const std::string percent = std::to_string(action.number) + "%";
            // wireplumber first (native to PipeWire), then the pulse shim.
            if (run({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", percent}, false).ok ||
                run({"pactl", "set-sink-volume", "@DEFAULT_SINK@", percent}, false).ok) {
                return {true, "Volume " + percent};
            }
            return {false, "I could not change the volume"};
        }
    }
    return {false, "I do not know how to do that"};
}

}  // namespace auspex
