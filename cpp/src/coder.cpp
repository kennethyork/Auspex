#include "auspex/coder.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"
#include "auspex/sandbox.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

std::string string_field(const json& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto& value = object[key];
    if (value.is_string()) return value.get<std::string>();
    // A model that answers with a number or a bool where a string belongs has
    // still said something usable; dumping it beats discarding the turn.
    if (value.is_number() || value.is_boolean()) return value.dump();
    return {};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

// ---------------------------------------------------------------------------
std::string_view tool_name(CoderTool tool) {
    switch (tool) {
        case CoderTool::List:   return "list";
        case CoderTool::Read:   return "read";
        case CoderTool::Write:  return "write";
        case CoderTool::Delete: return "delete";
        case CoderTool::Finish: return "finish";
        case CoderTool::Unknown: break;
    }
    return "unknown";
}

CoderTool tool_from_name(const std::string& name) {
    std::string lower = trim(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "list")   return CoderTool::List;
    if (lower == "read")   return CoderTool::Read;
    if (lower == "write")  return CoderTool::Write;
    if (lower == "delete") return CoderTool::Delete;
    if (lower == "finish") return CoderTool::Finish;

    // Synonyms models reach for unprompted. Accepted because the alternative is a
    // wasted turn spent telling it the word it wanted is spelled differently --
    // and every one of these maps to a verb that is already in the table, so
    // nothing new becomes reachable.
    if (lower == "ls" || lower == "list_files" || lower == "dir") return CoderTool::List;
    if (lower == "read_file" || lower == "open" || lower == "cat") return CoderTool::Read;
    if (lower == "write_file" || lower == "edit" || lower == "create") {
        return CoderTool::Write;
    }
    if (lower == "remove" || lower == "rm" || lower == "delete_file") {
        return CoderTool::Delete;
    }
    if (lower == "done" || lower == "complete" || lower == "stop") return CoderTool::Finish;

    return CoderTool::Unknown;
}

int CoderOutcome::writes() const {
    int n = 0;
    for (const auto& step : steps) {
        if (step.result.ok &&
            (step.call.tool == CoderTool::Write || step.call.tool == CoderTool::Delete)) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
std::string coder_prompt(const PlannedSubtask& subtask,
                         const std::vector<std::string>& files,
                         const std::vector<CoderStep>& steps,
                         const CoderLimits& limits) {
    std::ostringstream out;

    out << "You are a " << (subtask.role.empty() ? "coder" : subtask.role)
        << " working alone on one piece of a larger job.\n"
           "You are in a private copy of the project. Nothing you do here touches "
           "anyone else's work.\n\n";

    out << "Your piece:\n" << subtask.title << "\n";
    if (!subtask.detail.empty()) out << subtask.detail << "\n";
    out << "\n";

    // The tools, spelled exactly as they must be answered.
    out << "Answer with ONE JSON object per turn, naming one tool:\n"
           "  {\"tool\":\"list\"}                                   what files exist\n"
           "  {\"tool\":\"read\",\"path\":\"src/x.py\"}                 read a file\n"
           "  {\"tool\":\"write\",\"path\":\"src/x.py\",\"contents\":\"…\"}  replace a file "
           "entirely, or create it\n"
           "  {\"tool\":\"delete\",\"path\":\"src/x.py\"}               remove a file\n"
           "  {\"tool\":\"finish\",\"note\":\"what you did\"}          when the piece is "
           "done\n\n";

    out << "Rules:\n"
           "- write replaces the WHOLE file. Read it first unless you are creating "
           "it.\n"
           "- Paths are relative to the project root. No leading slash, no \"..\".\n"
           "- You cannot run commands, tests, or anything else. Only these tools.\n"
           "- Do the piece you were given and nothing else.\n"
           "- Finish as soon as it is done. You have "
        << limits.max_steps << " steps in total.\n\n";

    if (!files.empty()) {
        out << "Files here:\n";
        constexpr std::size_t kMaxListed = 200;
        const std::size_t shown = std::min(files.size(), kMaxListed);
        for (std::size_t i = 0; i < shown; ++i) out << "  " << files[i] << "\n";
        if (files.size() > shown) {
            out << "  ... and " << (files.size() - shown) << " more\n";
        }
        out << "\n";
    }

    if (!steps.empty()) {
        out << "What you have done so far:\n";
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const CoderStep& step = steps[i];
            out << "  " << (i + 1) << ". " << tool_name(step.call.tool);
            if (!step.call.path.empty()) out << " " << step.call.path;
            out << "\n";

            // A write's CONTENTS are not replayed. They are already on disk, and
            // re-sending them would double the context every time the coder wrote
            // anything -- which is exactly when the context is most needed.
            if (step.call.tool == CoderTool::Write) {
                // Nudged to stop. A model that has just written the file it was
                // asked to change will otherwise write it again, slightly
                // differently, until the budget runs out -- an observed run spent
                // six of nine steps that way. One sentence in the transcript is a
                // far cheaper fix than a larger step budget.
                out << "     -> "
                    << (step.result.ok
                            ? (step.result.no_op
                                   ? step.result.output + " -- stop rewriting it"
                                   : std::string("written. If your piece is done, "
                                                 "finish now."))
                            : step.result.output)
                    << "\n";
            } else {
                out << "     -> " << step.result.output << "\n";
            }
        }
        out << "\n";
    }

    out << "Your next tool call, as JSON:\n";
    return out.str();
}

ToolCall parse_tool_call(const std::string& reply) {
    ToolCall call;

    const std::string body = extract_json(reply);
    if (body.empty()) {
        call.error = "no JSON in the reply; answer with one JSON object";
        return call;
    }

    const auto document = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        call.error = "that did not parse as a JSON object";
        return call;
    }

    // "tool" is the asked-for key; the others are what models produce anyway.
    std::string name = string_field(document, "tool");
    if (name.empty()) name = string_field(document, "action");
    if (name.empty()) name = string_field(document, "name");
    if (name.empty()) name = string_field(document, "command");

    call.tool = tool_from_name(name);
    if (call.tool == CoderTool::Unknown) {
        call.error = name.empty()
                         ? "no tool named; use list, read, write, delete or finish"
                         : "\"" + name +
                               "\" is not a tool; use list, read, write, delete or finish";
        return call;
    }

    call.path = trim(string_field(document, "path"));
    if (call.path.empty()) call.path = trim(string_field(document, "file"));
    if (call.path.empty()) call.path = trim(string_field(document, "filename"));

    // NOT trimmed. Leading whitespace is significant in most of the languages this
    // will ever write, and a trailing newline is the difference between a file
    // that ends properly and one that does not.
    call.contents = string_field(document, "contents");
    if (call.contents.empty()) call.contents = string_field(document, "content");
    if (call.contents.empty()) call.contents = string_field(document, "text");

    call.note = trim(string_field(document, "note"));
    if (call.note.empty()) call.note = trim(string_field(document, "summary"));
    if (call.note.empty()) call.note = trim(string_field(document, "message"));

    // Verbs that need a path, checked here so the failure is one the model is told
    // about rather than one that silently does nothing.
    const bool needs_path = call.tool == CoderTool::Read ||
                            call.tool == CoderTool::Write ||
                            call.tool == CoderTool::Delete;
    if (needs_path && call.path.empty()) {
        call.error = std::string(tool_name(call.tool)) + " needs a \"path\"";
        call.tool  = CoderTool::Unknown;
    }

    return call;
}

ToolResult run_tool(const ToolCall& call, const std::filesystem::path& sandbox,
                    const CoderLimits& limits) {
    ToolResult result;

    if (call.tool == CoderTool::Unknown) {
        result.output = call.error.empty() ? "not a tool" : call.error;
        return result;
    }

    if (call.tool == CoderTool::Finish) {
        result.ok = true;
        result.output = "finished";
        return result;
    }

    if (call.tool == CoderTool::List) {
        std::ostringstream out;
        int n = 0;
        for (const auto& [path, _] : list_files(sandbox)) {
            out << path << "\n";
            ++n;
        }
        result.ok = true;
        result.output = n ? out.str() : "(no files)";
        return result;
    }

    // Everything below touches a path. The sandbox is a throwaway copy, but a copy
    // is not a jail -- "../.." walks out of the temp directory into the real
    // filesystem exactly as it would anywhere else.
    const auto target = safe_join(sandbox, call.path);
    if (!target) {
        result.output = "refused: \"" + call.path +
                        "\" is not a path inside the project";
        return result;
    }

    std::error_code ec;
    switch (call.tool) {
        case CoderTool::Read: {
            if (!std::filesystem::is_regular_file(*target, ec)) {
                result.output = "no such file: " + call.path;
                return result;
            }
            std::string contents = read_file(*target);
            if (contents.find('\0') != std::string::npos) {
                result.output = call.path + " is not a text file";
                return result;
            }
            // Truncated, and SAID to be: a coder that thinks it has seen a whole
            // file will rewrite it from the part it saw and delete the rest.
            if (contents.size() > limits.max_read_bytes) {
                contents.resize(limits.max_read_bytes);
                contents += "\n... (truncated; this file is longer than shown. Do "
                            "not rewrite it whole from this.)";
            }
            result.ok = true;
            result.output = contents.empty() ? "(empty file)" : contents;
            return result;
        }

        case CoderTool::Write: {
            // A rewrite with the contents the file already has is not an error and
            // not progress. Saying so plainly is what lets the coder notice it has
            // already done the thing, rather than doing it again.
            if (std::filesystem::is_regular_file(*target, ec) &&
                read_file(*target) == call.contents) {
                result.ok    = true;
                result.no_op = true;
                result.output = "no change: " + call.path +
                                " already contains exactly this";
                return result;
            }

            std::filesystem::create_directories(target->parent_path(), ec);
            std::ofstream out(*target, std::ios::binary | std::ios::trunc);
            if (!out) {
                result.output = "could not write " + call.path;
                return result;
            }
            out << call.contents;
            if (!out) {
                result.output = "could not write " + call.path;
                return result;
            }
            result.ok = true;
            result.output = "written (" + std::to_string(call.contents.size()) + " bytes)";
            return result;
        }

        case CoderTool::Delete: {
            if (!std::filesystem::exists(*target, ec)) {
                result.output = "no such file: " + call.path;
                return result;
            }
            // remove(), never remove_all(): a coder deleting a directory tree by
            // naming it is not a thing any subtask needs, and it is the single
            // most destructive action available in a sandbox that will later be
            // diffed against the real project.
            if (std::filesystem::is_directory(*target, ec)) {
                result.output = call.path + " is a directory; only files can be deleted";
                return result;
            }
            std::filesystem::remove(*target, ec);
            result.ok = !ec;
            result.output = ec ? "could not delete " + call.path : "deleted";
            return result;
        }

        default:
            break;
    }

    result.output = "not a tool";
    return result;
}

// ---------------------------------------------------------------------------
CoderOutcome run_coder(const Config& config, const PlannedSubtask& subtask,
                       const std::filesystem::path& sandbox,
                       const CoderLimits& limits, const std::string& model) {
    CoderOutcome outcome;

    if (!std::filesystem::is_directory(sandbox)) {
        outcome.error = "the sandbox is missing";
        return outcome;
    }

    outcome.model = model.empty() ? config.ollama_model : model;

    OllamaClient ollama(config);

    GenerateOptions options;
    options.json = true;
    options.disable_thinking = true;
    // Slightly above the Director's: writing code benefits from a little variety,
    // and a coder that answers identically to a failing call every turn is the
    // failure mode the repeat guard below exists for.
    options.temperature = 0.3;

    ToolCall last;
    int repeats = 0;

    for (int step = 0; step < limits.max_steps; ++step) {
        std::vector<std::string> files;
        for (const auto& [path, _] : list_files(sandbox)) files.push_back(path);

        const std::string prompt =
            coder_prompt(subtask, files, outcome.steps, limits);

        const auto reply = ollama.generate(
            model.empty() ? config.ollama_model : model, prompt, options);
        if (!reply) {
            outcome.error = "could not reach the model";
            return outcome;
        }

        const std::string text =
            reply->response.empty() ? reply->thinking : reply->response;

        CoderStep current;
        current.call   = parse_tool_call(text);
        current.result = run_tool(current.call, sandbox, limits);

        if (current.call.tool == CoderTool::Finish) {
            outcome.finished = true;
            outcome.note     = current.call.note;
            outcome.steps.push_back(std::move(current));
            return outcome;
        }

        // Stuck rather than working.
        //
        // Three things count as not-progress at the same target: a call that
        // failed, a write that changed nothing, and a REPEATED WRITE -- because a
        // coder rewriting one file turn after turn without reading it in between
        // is guessing, not converging. Only the first of these used to count, and
        // an observed run spent six of nine steps rewriting a single file.
        const bool same_target = current.call.tool == last.tool &&
                                 current.call.path == last.path;
        const bool no_progress = !current.result.ok || current.result.no_op ||
                                 current.call.tool == CoderTool::Write;

        if (same_target && no_progress) {
            if (++repeats >= limits.max_repeats) {
                outcome.steps.push_back(std::move(current));
                outcome.error = "the coder stopped making progress";
                return outcome;
            }
        } else {
            repeats = 0;
        }
        last = current.call;

        outcome.steps.push_back(std::move(current));
    }

    // Out of budget. Not an error in the sense of "nothing happened" -- whatever
    // was written is still in the sandbox and will be captured -- but it did not
    // finish, and the board should say so.
    outcome.error = "the coder ran out of steps";
    return outcome;
}

}  // namespace auspex
