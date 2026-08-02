#include "auspex/commands.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "auspex/agents.hpp"
#include "auspex/crew.hpp"
#include "auspex/display.hpp"
#include "auspex/process.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace auspex {

namespace {

std::vector<std::string> split_words(const std::string& command) {
    std::vector<std::string> words;
    std::istringstream in(command);
    std::string word;
    while (in >> word) words.push_back(word);
    return words;
}

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
        case ActionKind::OpenTerminal:    return "open_terminal";
        case ActionKind::OpenAgent:       return "open_agent";
        case ActionKind::RunCrew:         return "run_crew";
        case ActionKind::CrewAccept:      return "crew_accept";
        case ActionKind::CrewDiscard:     return "crew_discard";
        case ActionKind::CrewSteer:       return "crew_steer";
        case ActionKind::ShowBoard:       return "show_board";
        case ActionKind::CrewResume:      return "crew_resume";
        case ActionKind::PanCanvas:       return "pan_canvas";
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

std::string crew_task_from_utterance(std::string_view utterance) {
    std::string text = trim(std::string(utterance));
    if (text.empty()) return {};

    // Peel the request phrasing off the front, longest first so "ask the crew to"
    // is not half-matched by "ask the crew". Only the front: the task itself is
    // whatever remains, untouched, because it is the user's specification and
    // rewording it would change what gets built.
    static const std::vector<std::string> kPrefixes = {
        "have the crew to ", "have the crew ", "ask the crew to ", "ask the crew ",
        "tell the crew to ", "tell the crew ", "get the crew to ", "get the crew ",
        "run the crew on ", "run the crew ", "crew, ", "crew ",
        "use the crew to ", "use the crew ",
    };

    for (bool changed = true; changed;) {
        changed = false;
        const std::string lowered = lower(text);
        for (const auto& prefix : kPrefixes) {
            if (lowered.rfind(prefix, 0) == 0) {
                text = trim(text.substr(prefix.size()));
                changed = true;
                break;
            }
        }
    }

    // Trailing punctuation from dictation.
    while (!text.empty() && (text.back() == '.' || text.back() == '?' ||
                             text.back() == '!' || text.back() == ',')) {
        text.pop_back();
    }
    text = trim(std::move(text));

    // Strip dangling connectors left at the END.
    //
    // "ask the crew to" strips to "to": the longest matching prefix needs a space
    // after it, so a sentence that stops there falls through to the shorter form
    // and leaves the preposition behind. Sending "to" as a task to a tool that
    // edits files is worse than refusing, and the same guard covers any request
    // that simply trailed off mid-sentence.
    static const std::vector<std::string> kDangling = {
        "to", "on", "for", "with", "about", "the", "a", "an", "and", "please",
    };
    for (bool changed = true; changed && !text.empty();) {
        changed = false;
        const std::size_t space = text.find_last_of(' ');
        const std::string last = lower(space == std::string::npos
                                           ? text
                                           : text.substr(space + 1));
        if (std::find(kDangling.begin(), kDangling.end(), last) != kDangling.end()) {
            text = trim(space == std::string::npos ? std::string{} : text.substr(0, space));
            changed = true;
        }
    }
    return text;
}

CommandContext gather_context(const Config& config) {
    CommandContext context;
    context.workspace_count = config.workspace_count > 0 ? config.workspace_count : 4;
    context.windows         = list_user_windows();
    context.browser         = config.browser;
    context.search_url      = config.search_url;
    context.terminal        = config.terminal;

    if (const auto focused = focused_window_title()) context.focused_window = *focused;
    if (const auto selected = selected_text())       context.selection = *selected;
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
              "  {\"action\":\"open_terminal\"}\n"
              "  {\"action\":\"open_agent\",\"target\":\"<agent name>\"}\n"
              "  {\"action\":\"run_crew\"}\n"
              "  {\"action\":\"show_board\"}\n"
              "  {\"action\":\"crew_resume\"}\n"
              "  {\"action\":\"crew_accept\",\"number\":<int>}\n"
              "  {\"action\":\"crew_discard\",\"number\":<int>}\n"
              "  {\"action\":\"crew_steer\",\"number\":<int>}\n"
              "  {\"action\":\"pan_canvas\",\"target\":"
              "\"left|right|up|down|home\"}\n"
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

    if (in_path("ollamadev")) {
        prompt << "The crew is a parallel bench of coding agents that edits files in "
                  "this project. Use run_crew when the user asks the CREW to do "
                  "something -- \"have the crew add rate limiting\", \"ask the crew "
                  "to write tests\". It takes no target: the user's own words are "
                  "used verbatim, so do not summarise the task.\n\n";
    }

    // Naming the installed agents rather than describing them: a small model given a
    // closed list copies from it, and given only a description invents plausible
    // neighbours ("copilot", "chatgpt") that then fail to resolve.
    if (const auto agents = available_agents(); !agents.empty()) {
        prompt << "Coding agents installed (use open_agent, target must be one of "
                  "these exactly):\n";
        for (const auto& agent : agents) {
            prompt << "  " << agent.key << "  (" << agent.label << ")\n";
        }
        prompt << "\n";
    }

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

    if (kind == "open_terminal") {
        // No target to validate: the terminal comes from Config, never the model.
        result.action = Action{ActionKind::OpenTerminal, {}, 0};
        return result;
    }

    if (kind == "open_agent") {
        // The single gate between model output and a process. The model supplies a
        // NAME, which is matched against the fixed table in agents.hpp; the binary
        // that eventually reaches execvp is a literal from that table and never
        // anything the model wrote. An unrecognised name stops here.
        const auto agent = resolve_agent(target);
        if (!agent) {
            std::string known;
            for (const auto& tool : available_agents()) {
                if (!known.empty()) known += ", ";
                known += tool.label;
            }
            result.error = known.empty()
                               ? "no coding agents are installed"
                               : "I can open " + known;
            return result;
        }
        // Not in_path(): agents installed by nvm, bun, deno or cargo are not on the
        // login PATH the panel inherits from the display manager. resolve_agent()
        // searches those trees too, so this asks whether it was found ANYWHERE
        // rather than only where a login shell would have looked.
        if (agent->path.empty()) {
            result.error = agent->label + " is not installed";
            return result;
        }
        result.action = Action{ActionKind::OpenAgent, agent->key, 0};
        return result;
    }

    if (kind == "run_crew") {
        if (!in_path("ollamadev")) {
            result.error = "the crew needs ollamadev, which is not installed";
            return result;
        }

        // Deliberately ignoring whatever the model put in "target". The crew edits
        // files, and the description of what to edit must be the user's own words
        // rather than a paraphrase that may have drifted. The model decided the
        // verb; that is all it is trusted with here.
        const std::string task = crew_task_from_utterance(context.utterance);
        if (task.empty()) {
            result.error = "I did not catch what the crew should work on";
            return result;
        }
        result.action = Action{ActionKind::RunCrew, task, 0};
        return result;
    }

    if (kind == "crew_resume") {
        if (!crew_available()) {
            result.error = "the crew needs ollamadev, which is not installed";
            return result;
        }
        result.action = Action{ActionKind::CrewResume, {}, 0};
        return result;
    }

    if (kind == "show_board") {
        if (!crew_available()) {
            result.error = "the crew needs ollamadev, which is not installed";
            return result;
        }
        result.action = Action{ActionKind::ShowBoard, {}, 0};
        return result;
    }

    if (kind == "crew_accept" || kind == "crew_discard" || kind == "crew_steer") {
        if (!crew_available()) {
            result.error = "the crew needs ollamadev, which is not installed";
            return result;
        }

        // The number is checked against the board that actually exists. This is the
        // same rule as focus_window never trusting a model-supplied window id: a
        // hallucinated 7 must not apply somebody else's changeset just because the
        // integer parsed. Accept and discard are not reversible.
        const auto items = board_items();
        if (items.empty()) {
            result.error = "the crew is not holding anything right now";
            return result;
        }
        const auto item = board_item(items, number);
        if (!item) {
            std::string held;
            for (const auto& i : items) {
                if (!held.empty()) held += ", ";
                held += std::to_string(i.n);
            }
            result.error = "there is no change " + std::to_string(number) +
                           " on the board; it is holding " + held;
            return result;
        }

        if (kind == "crew_accept") {
            result.action = Action{ActionKind::CrewAccept, item->summary, number};
            return result;
        }
        if (kind == "crew_discard") {
            result.action = Action{ActionKind::CrewDiscard, item->summary, number};
            return result;
        }

        // Steer carries free text to a running coder. Same rule as run_crew: the
        // words are the user's, taken from the transcript, never the model's.
        const std::string instruction = crew_task_from_utterance(context.utterance);
        if (instruction.empty()) {
            result.error = "I did not catch what to tell the coder";
            return result;
        }
        result.action = Action{ActionKind::CrewSteer, instruction, number};
        return result;
    }

    if (kind == "pan_canvas") {
        const std::string where = lower(target);
        for (const char* allowed : {"left", "right", "up", "down", "home"}) {
            if (where == allowed) {
                result.action = Action{ActionKind::PanCanvas, where, 0};
                return result;
            }
        }
        result.error = "I can pan left, right, up, down or home";
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

// Launches argv and puts whatever window it produces onto the canvas.
//
// Shared by open_terminal and open_agent because the hard part is identical and
// worth having in one place: there is no portable way to ask "which window did this
// pid create" -- a terminal may hand the request to an already-running server
// process, so the pid that appears is not the pid that was forked. Diffing the
// window list before and after is the only thing that works across terminals.
ExecResult launch_onto_canvas(const CommandContext& context,
                              const std::vector<std::string>& argv,
                              const std::string& placed_message,
                              const std::string& failure_message) {
    if (argv.empty()) return {false, failure_message};

    if (!context.canvas) {
        // Still useful without a canvas: just launch it normally.
        if (!spawn_detached(argv)) return {false, failure_message};
        return {true, placed_message};
    }

    std::vector<std::string> before;
    for (const auto& window : list_user_windows()) before.push_back(window.id);

    if (!spawn_detached(argv)) return {false, failure_message};

    for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (const auto& window : list_user_windows()) {
            if (std::find(before.begin(), before.end(), window.id) != before.end()) {
                continue;
            }
            // Canvas-mode bars are overlays. They stay usable because they are
            // sticky/above, but they do not cut edges out of the infinite plane.
            context.canvas->set_insets(0, 0);

            // Size the slot from the window's own geometry, so a terminal that
            // remembers a large size is not overlapped by the next one.
            int tile_w = 640, tile_h = 420;
            if (const auto geometry = display().window_geometry(window.id)) {
                tile_w = std::max(200, geometry->width);
                tile_h = std::max(150, geometry->height);
            }
            context.canvas->place_next(window.id, tile_w, tile_h);
            // The monitor union, so a window that does not fit is parked off every
            // screen rather than onto the one next door. Cached; see screen_bounds.
            const auto screen = screen_bounds();
            apply_positions(context.canvas->resolve(), context.monitor,
                            screen ? &*screen : nullptr);
            return {true, placed_message};
        }
    }
    return {true, placed_message};   // launched, just not seen in time
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

        case ActionKind::OpenTerminal: {
            if (context.terminal.empty()) {
                return {false, "no terminal is configured"};
            }
            return launch_onto_canvas(context, {context.terminal},
                                      "Terminal placed on the canvas",
                                      "I could not start the terminal");
        }

        case ActionKind::OpenAgent: {
            if (context.terminal.empty()) {
                return {false, "no terminal is configured"};
            }
            // action.target is a key that already survived resolve_agent() during
            // parsing. Looking it up again rather than carrying the binary through
            // the Action means the executable is read from the table at the point of
            // use -- an Action built by hand, or replayed from somewhere else, still
            // cannot smuggle a binary in.
            const auto agent = resolve_agent(action.target);
            if (!agent) return {false, "I do not know that agent"};

            const auto argv = agent_terminal_command(context.terminal, *agent);
            return launch_onto_canvas(context, argv,
                                      agent->label + " opened on the canvas",
                                      "I could not start " + agent->label);
        }

        case ActionKind::RunCrew: {
            if (context.terminal.empty()) {
                return {false, "no terminal is configured"};
            }
            // argv, not a shell string: the task is one argument however many
            // spaces, quotes or semicolons it contains.
            std::vector<std::string> argv = split_words(context.terminal);
            argv.push_back("-e");
            argv.push_back("ollamadev");
            argv.push_back("crew");
            argv.push_back(action.target);

            return launch_onto_canvas(context, argv,
                                      "Crew working on: " + action.target,
                                      "I could not start the crew");
        }

        case ActionKind::CrewResume: {
            if (context.terminal.empty()) return {false, "no terminal is configured"};
            // On the canvas, like run_crew: resuming re-plans and re-runs coders, so
            // it is a long job with output worth watching, not a fire-and-forget.
            std::vector<std::string> argv = split_words(context.terminal);
            argv.push_back("-e");
            const auto resume = crew_resume_command();
            argv.insert(argv.end(), resume.begin(), resume.end());
            return launch_onto_canvas(context, argv, "Resuming the crew run",
                                      "I could not reach the crew");
        }

        case ActionKind::ShowBoard: {
            const auto items = board_items();
            if (items.empty()) return {true, "The crew is not holding anything"};

            std::string spoken = std::to_string(items.size()) +
                                 (items.size() == 1 ? " change held: " : " changes held: ");
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i) spoken += "; ";
                spoken += std::to_string(items[i].n) + ", " + items[i].summary;
            }
            return {true, spoken};
        }

        case ActionKind::CrewAccept: {
            const auto argv = crew_accept_command(action.number);
            if (argv.empty()) return {false, "that is not a change I can accept"};
            if (!spawn_detached(argv)) return {false, "I could not run the crew"};
            return {true, "Accepting change " + std::to_string(action.number)};
        }

        case ActionKind::CrewDiscard: {
            const auto argv = crew_discard_command(action.number);
            if (argv.empty()) return {false, "that is not a change I can discard"};
            if (!spawn_detached(argv)) return {false, "I could not run the crew"};
            return {true, "Discarding change " + std::to_string(action.number)};
        }

        case ActionKind::CrewSteer: {
            const auto argv = crew_steer_command(action.number, action.target);
            if (argv.empty()) return {false, "I need something to tell the coder"};
            if (!spawn_detached(argv)) return {false, "I could not reach the crew"};
            return {true, "Told coder " + std::to_string(action.number) + ": " + action.target};
        }

        case ActionKind::PanCanvas: {
            if (!context.canvas) return {false, "the canvas is not running"};

            const auto& viewport = context.canvas->viewport();
            // Pan by three quarters of a screen: a full screen loses all context,
            // and a small step needs too many commands to be useful by voice.
            const int step_x = viewport.width  * 3 / 4;
            const int step_y = viewport.height * 3 / 4;

            if (action.target == "home") {
                Viewport home = viewport;
                home.x = 0;
                home.y = 0;
                context.canvas->set_viewport(home);
            } else if (action.target == "left")  context.canvas->pan_by(-step_x, 0);
            else if (action.target == "right")   context.canvas->pan_by(step_x, 0);
            else if (action.target == "up")      context.canvas->pan_by(0, -step_y);
            else if (action.target == "down")    context.canvas->pan_by(0, step_y);

            const auto screen = screen_bounds();
            apply_positions(context.canvas->resolve(), context.monitor,
                            screen ? &*screen : nullptr);
            return {true, action.target == "home" ? "Back to the origin"
                                                  : "Panned " + action.target};
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
