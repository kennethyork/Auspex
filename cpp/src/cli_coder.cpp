#include "auspex/cli_coder.hpp"

#include <algorithm>
#include <sstream>

#include "auspex/agents.hpp"
#include "auspex/process.hpp"

namespace auspex {

const std::vector<std::string>& coder_backends() {
    // "ollama" first: it is the default and the only one that needs nothing
    // installed. The rest are the agent CLIs, matching the ids in agents.hpp so a
    // backend and a launchable agent are the same thing named once.
    static const std::vector<std::string> kBackends{
        "ollama", "claude", "codex", "gemini", "cursor-agent", "opencode", "qwen",
        "aider",
    };
    return kBackends;
}

bool is_cli_backend(const std::string& id) {
    if (id.empty() || id == "ollama") return false;
    const auto& all = coder_backends();
    return std::find(all.begin(), all.end(), id) != all.end();
}

bool prompt_on_stdin(const std::string& backend) {
    // claude takes it on stdin with -p; codex exec takes a trailing '-' meaning
    // the same. Both matter beyond tidiness: a subtask description runs to
    // kilobytes and argv has a length limit a long one would hit.
    return backend == "claude" || backend == "codex";
}

std::vector<std::string> cli_coder_argv(const std::string& backend,
                                        const std::string& model,
                                        const std::string& prompt) {
    if (!is_cli_backend(backend)) return {};

    const std::string binary = resolve_agent_binary(
        backend == "cursor-agent" ? "cursor-agent" : backend);
    // Absolute, for the same reason agents are launched that way elsewhere: the
    // panel's PATH is the login one and misses everything nvm and friends install.
    if (binary.empty()) return {};

    std::vector<std::string> argv{binary};
    const std::string m = trim(model);

    if (backend == "claude") {
        // acceptEdits, not the default mode. The crew hands it a task and expects
        // the edits to land; a headless run has nobody to approve them.
        argv.insert(argv.end(), {"-p", "--output-format", "text",
                                 "--permission-mode", "acceptEdits"});
        if (!m.empty()) argv.insert(argv.end(), {"--model", m});
        return argv;   // the prompt goes on stdin
    }

    if (backend == "codex") {
        // `codex exec` never prompts, so it needs no approval flag; the sandbox
        // setting is what lets it edit the tree it was pointed at. The trailing
        // '-' means "read the prompt from stdin".
        argv.insert(argv.end(), {"exec", "--skip-git-repo-check", "--color", "never",
                                 "--sandbox", "workspace-write"});
        if (!m.empty()) argv.insert(argv.end(), {"-m", m});
        argv.push_back("-");
        return argv;
    }

    if (backend == "gemini") {
        // --skip-trust is required, not decorative: without it gemini downgrades
        // yolo back to "default" for an untrusted folder and then blocks on an
        // approval prompt a headless run can never answer.
        argv.insert(argv.end(), {"--output-format", "text",
                                 "--approval-mode", "yolo", "--skip-trust"});
        if (!m.empty()) argv.insert(argv.end(), {"-m", m});
        argv.insert(argv.end(), {"-p", prompt});
        return argv;
    }

    if (backend == "qwen") {
        argv.insert(argv.end(), {"--output-format", "text"});
        if (!m.empty()) argv.insert(argv.end(), {"-m", m});
        argv.insert(argv.end(), {"-p", prompt});
        return argv;
    }

    if (backend == "cursor-agent") {
        argv.insert(argv.end(), {"-p", "--output-format", "text", "--force", "--trust"});
        if (!m.empty()) argv.insert(argv.end(), {"--model", m});
        argv.push_back(prompt);
        return argv;
    }

    if (backend == "opencode") {
        argv.insert(argv.end(), {"run", "--format", "default", "--auto"});
        if (!m.empty()) argv.insert(argv.end(), {"-m", m});
        argv.push_back(prompt);
        return argv;
    }

    if (backend == "aider") {
        argv.insert(argv.end(), {"--yes", "--no-pretty", "--no-stream"});
        if (!m.empty()) argv.insert(argv.end(), {"--model", m});
        argv.insert(argv.end(), {"--message", prompt});
        return argv;
    }

    return {};
}

std::string cli_coder_prompt(const PlannedSubtask& subtask,
                             const std::string& lessons) {
    std::ostringstream out;

    // No verb table. This agent has its own tools and runs its own loop; telling
    // it about ours would be describing a machine it is not running on.
    out << "You are working alone on one piece of a larger job, in a private copy "
           "of the project. Nothing you do here affects anyone else's work.\n\n";

    out << "Your piece:\n" << subtask.title << "\n";
    if (!subtask.detail.empty()) out << subtask.detail << "\n";
    out << "\n";

    if (!lessons.empty()) out << lessons << "\n";

    out << "Do this piece and nothing else. Do not reorganise the project, do not "
           "fix unrelated things, and do not commit -- another reviewer reads your "
           "changes before they land anywhere real.\n";
    return out.str();
}

CoderOutcome run_cli_coder(const Config& config, const PlannedSubtask& subtask,
                           const std::filesystem::path& sandbox,
                           const std::string& backend, const std::string& model,
                           int timeout_seconds, const std::string& lessons) {
    (void)config;

    CoderOutcome outcome;
    outcome.model = model.empty() ? backend : backend + ":" + model;

    if (!std::filesystem::is_directory(sandbox)) {
        outcome.error = "the sandbox is missing";
        return outcome;
    }
    if (!is_cli_backend(backend)) {
        outcome.error = backend + " is not a coder backend";
        return outcome;
    }

    const std::string prompt = cli_coder_prompt(subtask, lessons);
    const auto argv = cli_coder_argv(backend, model, prompt);
    if (argv.empty()) {
        outcome.error = backend + " is not installed";
        return outcome;
    }

    // The prompt on stdin where the tool wants it there, and the pipe CLOSED after
    // -- which is how these tools know the prompt has ended. Left open, every one
    // of them waits for more input forever.
    const std::string stdin_text = prompt_on_stdin(backend) ? prompt : std::string{};

    // Generous, because this is a whole agentic run rather than one turn: it will
    // read files, write them, and often run tests. Still bounded, because a tool
    // waiting on an approval prompt we failed to suppress would otherwise wait for
    // the life of the process.
    const LimitedResult ran = run_limited(argv, sandbox.string(), timeout_seconds,
                                          /*max_output=*/24'000, stdin_text);

    outcome.finished = ran.ok;
    if (ran.timed_out) {
        outcome.error = backend + " ran out of time after " +
                        std::to_string(timeout_seconds) + "s";
    } else if (!ran.ok) {
        outcome.error = backend + " exited " + std::to_string(ran.exit_code);
    }

    // The tail of its output as the note. The head is usually banner and setup;
    // what an agent says LAST is its summary of what it did.
    std::string tail = trim(ran.output);
    constexpr std::size_t kNote = 600;
    if (tail.size() > kNote) tail = "…" + tail.substr(tail.size() - kNote);
    outcome.note = tail;

    return outcome;
}

}  // namespace auspex
