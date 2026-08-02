// A coder that is somebody else's agent.
//
// Auspex's own coder loop calls Ollama and drives a fixed verb table. That caps
// the crew at whatever Ollama can serve, and it showed: a 9b model held correct
// code twice with confident wrong reasons, and treated an MCP tool as a Python
// function. ollamadev routes around this by letting a coder BE claude, codex,
// gemini or another agent CLI, each running its own loop with its own model. This
// is that, ported.
//
// WHAT CHANGES AND WHAT DOES NOT. The CLI does its own planning, its own tool use
// and its own file edits -- Auspex does not drive it turn by turn and could not.
// Everything AROUND it is unchanged:
//
//   * it runs in the SANDBOX, a throwaway copy, never the real project
//   * the changeset is captured by comparing the sandbox afterwards, exactly as
//     for a built-in coder
//   * the Auditor reviews it, the overlap guard applies, the secret gate applies
//
// So handing the work to a stronger agent does not hand it the project. The
// containment is in the sandbox and the landing pass, not in the coder.
//
// EVERY INVOCATION IS NON-INTERACTIVE, and that is the fiddly part. Each of these
// tools defaults to asking a human for approval, and a headless run has nobody to
// answer -- so each needs its own flag to stop it blocking forever. The table
// below is ollamadev's, with the reasons kept, because those flags were learned
// the hard way and guessing at them produces a coder that hangs.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "auspex/coder.hpp"
#include "auspex/config.hpp"
#include "auspex/director.hpp"

namespace auspex {

// Backend ids a coder may be. "ollama" means Auspex's own loop.
const std::vector<std::string>& coder_backends();
bool is_cli_backend(const std::string& id);

// The argv for one non-interactive run. Empty for an unknown backend.
//
// `model` empty lets the CLI pick its own default, which is usually right -- these
// tools are configured by their owner and second-guessing that is how you end up
// running an expensive model somebody did not choose.
std::vector<std::string> cli_coder_argv(const std::string& backend,
                                        const std::string& model,
                                        const std::string& prompt);

// True when the prompt goes on stdin rather than in the argv.
//
// claude and codex both read it that way. It matters beyond tidiness: a subtask
// description can be several kilobytes, and argv has a length limit that a long
// one would hit.
bool prompt_on_stdin(const std::string& backend);

// What the agent is told. One block of English -- it has its own tools, so there
// is no verb table here, and telling it about ours would be describing a machine
// it is not running on.
std::string cli_coder_prompt(const PlannedSubtask& subtask,
                             const std::string& lessons = {});

// One-shot text from an agent CLI: prompt in, its answer out.
//
// The same tools, used as a completion rather than as a coder. That is what lets
// the Director and the Auditor run on Claude or Codex too -- they need a reply,
// not file edits, and `claude -p` gives exactly that.
//
// Run in a directory it may read but has no reason to write. Empty on failure,
// which every caller already treats as "the model did not answer".
std::string ask_cli(const std::string& backend, const std::string& model,
                    const std::string& prompt, const std::filesystem::path& cwd = {},
                    int timeout_seconds = 300);

// Runs one as a CODER. Blocking.
//
// Reports as a CoderOutcome so the rest of the engine cannot tell the difference:
// `steps` is empty because the agent's turns are its own business, and `finished`
// means it exited zero. Whatever it wrote is in the sandbox either way, and the
// changeset is captured from there.
CoderOutcome run_cli_coder(const Config& config, const PlannedSubtask& subtask,
                           const std::filesystem::path& sandbox,
                           const std::string& backend, const std::string& model = {},
                           int timeout_seconds = 900,
                           const std::string& lessons = {});

}  // namespace auspex
