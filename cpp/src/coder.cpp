#include "auspex/coder.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"
#include "auspex/code_index.hpp"
#include "auspex/sandbox.hpp"
#include "auspex/symbols.hpp"
#include "auspex/roles.hpp"
#include "auspex/json_util.hpp"

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
        case CoderTool::Replace: return "replace";
        case CoderTool::Delete: return "delete";
        case CoderTool::Run:    return "run";
        case CoderTool::Skill:  return "skill";
        case CoderTool::Mcp:    return "mcp";
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
    if (lower == "replace") return CoderTool::Replace;
    if (lower == "delete") return CoderTool::Delete;
    if (lower == "finish") return CoderTool::Finish;
    if (lower == "run")    return CoderTool::Run;
    if (lower == "skill")  return CoderTool::Skill;
    if (lower == "mcp")    return CoderTool::Mcp;

    // Synonyms models reach for unprompted. Accepted because the alternative is a
    // wasted turn spent telling it the word it wanted is spelled differently --
    // and every one of these maps to a verb that is already in the table, so
    // nothing new becomes reachable.
    if (lower == "ls" || lower == "list_files" || lower == "dir") return CoderTool::List;
    if (lower == "read_file" || lower == "open" || lower == "cat") return CoderTool::Read;
    if (lower == "write_file" || lower == "create") return CoderTool::Write;
    // "edit" means a PART of a file to most models, which is what replace is.
    // Mapping it to write was how a one-line intention became a whole-file
    // rewrite.
    if (lower == "edit" || lower == "str_replace" || lower == "substitute") {
        return CoderTool::Replace;
    }
    if (lower == "open_skill" || lower == "use_skill" || lower == "load_skill") {
        return CoderTool::Skill;
    }
    if (lower == "remove" || lower == "rm" || lower == "delete_file") {
        return CoderTool::Delete;
    }
    if (lower == "done" || lower == "complete" || lower == "stop") return CoderTool::Finish;
    if (lower == "test" || lower == "exec" || lower == "shell" || lower == "bash" ||
        lower == "sh"   || lower == "command") {
        // Mapped to Run, NOT refused: the model wanting to run something is a
        // legitimate intent spelled badly. It still hits the allowlist, so asking
        // for "bash" gets it a refusal that names bash rather than a refusal that
        // says "bash is not a tool" and invites it to try "sh" next.
        return CoderTool::Run;
    }

    return CoderTool::Unknown;
}

const std::vector<std::string>& runnable_programs() {
    // Build and test drivers only. What is ABSENT is the important half: every
    // shell, env, xargs, find, sudo, ssh, curl, wget, nc, git -- anything whose
    // job is to run something else, reach the network, or change the machine
    // outside the sandbox.
    static const std::vector<std::string> kAllowed{
        "pytest", "python", "python3", "tox", "nox", "ruff", "mypy", "black",
        "node", "npm", "npx", "yarn", "pnpm", "jest", "vitest", "tsc", "eslint",
        "cargo", "rustc", "rustfmt", "clippy-driver",
        "go", "gofmt",
        "make", "cmake", "ctest", "ninja",
        "mvn", "gradle", "javac", "java",
        "dotnet", "phpunit", "composer", "rspec", "rake", "bundle",
        "swift", "dart", "flutter", "zig", "gcc", "g++", "clang", "clang++",
        "shellcheck", "luacheck", "busted",
    };
    return kAllowed;
}

bool is_runnable(const std::string& program) {
    // Matched on the WHOLE name, not a prefix or a path. "/usr/bin/pytest" and
    // "pytest-evil" are both refused: allowing a path would let a coder run
    // something it had just written into the sandbox and named convincingly.
    if (program.empty()) return false;
    if (program.find('/') != std::string::npos) return false;
    const auto& allowed = runnable_programs();
    return std::find(allowed.begin(), allowed.end(), program) != allowed.end();
}

int CoderOutcome::writes() const {
    int n = 0;
    for (const auto& step : steps) {
        if (step.result.ok &&
            (step.call.tool == CoderTool::Write || step.call.tool == CoderTool::Delete ||
             step.call.tool == CoderTool::Replace)) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
std::string take_steer(const std::filesystem::path& mailbox) {
    if (mailbox.empty()) return {};
    std::ifstream in(mailbox);
    if (!in) return {};
    std::ostringstream contents;
    contents << in.rdbuf();
    in.close();

    // Consumed, not merely read. A message left in place would be re-injected on
    // every remaining turn, and the coder would take it as being said again.
    std::error_code ec;
    std::filesystem::remove(mailbox, ec);
    return trim(contents.str());
}

bool leave_steer(const std::filesystem::path& mailbox, const std::string& message) {
    if (mailbox.empty()) return false;
    const std::string text = trim(message);
    if (text.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(mailbox.parent_path(), ec);

    // Appended, so a second message sent before the coder has looked does not
    // silently replace the first.
    std::ofstream out(mailbox, std::ios::app);
    if (!out) return false;
    out << text << "\n";
    return static_cast<bool>(out);
}

std::string coder_prompt(const PlannedSubtask& subtask,
                         const std::vector<std::string>& files,
                         const std::vector<CoderStep>& steps,
                         const CoderLimits& limits,
                         const std::string& steered, const SkillSet& skills,
                         const McpAccess& mcp, const std::string& hint) {
    std::ostringstream out;

    out << "You are working alone on one piece of a larger job.\n"
           "You are in a private copy of the project. Nothing you do here touches "
           "anyone else's work.\n\n";

    // The role, with what it means. "You are a tester" is a label; the persona is
    // the part that changes what gets written -- match the existing framework,
    // do not touch production code, and so on.
    if (const std::string role = persona_block(persona_for(subtask.role));
        !role.empty()) {
        out << role << "\n";
    }

    out << "Your piece:\n" << subtask.title << "\n";
    if (!subtask.detail.empty()) out << subtask.detail << "\n";
    out << "\n";

    // Where the names in this piece live. Before the tool table, because it
    // changes the FIRST call a coder makes -- read from line 1165 rather than
    // read and hope.
    if (!trim(hint).empty()) out << hint << "\n";

    // A person has said something. Placed high and marked, because the whole point
    // of steering is that it outranks the plan the coder is working to.
    if (!steered.empty()) {
        out << "IMPORTANT -- a person watching you has just said:\n"
            << steered << "\n"
               "Take that into account before your next step.\n\n";
    }

    // The tools, spelled exactly as they must be answered.
    out << "Answer with ONE JSON object per turn, naming one tool:\n"
           "  {\"tool\":\"list\"}                                   what files exist\n"
           "  {\"tool\":\"read\",\"path\":\"file.py\"}                 read a file\n"
           "  {\"tool\":\"read\",\"path\":\"file.py\",\"from\":400}      ... from a line\n"
           "  {\"tool\":\"replace\",\"path\":\"file.py\",\"find\":\"…\",\"replace\":\"…\"}\n"
           "                                                    change PART of a file\n"
           "  {\"tool\":\"write\",\"path\":\"file.py\",\"contents\":\"…\"}  replace a file "
           "entirely, or create it\n"
           "  {\"tool\":\"delete\",\"path\":\"file.py\"}               remove a file\n"
           "  {\"tool\":\"finish\",\"note\":\"what you did\"}          when the piece is "
           "done\n";
    if (limits.allow_run) {
        out << "  {\"tool\":\"run\",\"command\":[\"pytest\",\"-q\"]}        run tests or a "
               "build\n";
    }
    if (!skills.empty()) {
        out << "  {\"tool\":\"skill\",\"skill\":\"name\"}                 read one of the "
               "skills below\n";
    }
    if (!mcp.empty()) {
        out << "  {\"tool\":\"mcp\",\"name\":\"server.tool\",\"arguments\":{}}  call an "
               "external tool\n";
    }
    out << "\n";

    // The CATALOGUE only -- one line each. The bodies arrive when asked for, which
    // is the whole idea: putting every skill's full text in every prompt would
    // spend the context window on instructions that do not apply.
    if (!skills.empty()) {
        out << "Skills available (read one before doing work it covers):\n"
            << skills.catalog << "\n";
    }

    // Name and one line each. The argument SCHEMAS are deliberately not here: a
    // dozen JSON schemas would be most of the prompt, and a wrong call costs one
    // turn and an error message that says what was wrong.
    if (!mcp.empty()) {
        out << "External tools available:\n";
        for (const auto& tool : mcp.tools) {
            out << "  " << tool.qualified();
            if (!tool.description.empty()) out << " — " << tool.description;
            out << "\n";
        }
        out << "\n";
    }

    if (limits.allow_run) {
        out << "You may run: ";
        for (std::size_t i = 0; i < runnable_programs().size(); ++i) {
            if (i) out << ", ";
            out << runnable_programs()[i];
        }
        out << ".\nNothing else, and no shell.\n\n";
    }

    out << "Rules:\n"
           "- write replaces the WHOLE file. Read it first unless you are creating "
           "it.\n"
           "- Paths are relative to the project root. No leading slash, no \"..\".\n"
           "- Put a file where the piece says, next to the files already \n"
           "  here. Do NOT invent a directory: writing src/thing.py when the \n"
           "  project has no src/ is the single most common way this goes \n"
           "  wrong, and the file is then in the wrong place.\n"
           "- If you change HOW something is called -- its name, its \n"
           "  arguments, what it returns -- find every place that calls it and \n"
           "  update those too. A change that is right in one file and breaks \n"
           "  another is not finished.\n"
           "- Only these tools. There is no shell.\n"
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
        // WHICH READS ARE REPLAYED IN FULL.
        //
        // Newest first, until the budget is spent; everything older becomes a line
        // naming the file. A read is capped at 24KB, so five of them replayed every
        // turn is 125KB per call -- measured on this very project, where one run
        // spent 1.6 MILLION input tokens on a task whose answer was one line. The
        // cost is per turn and multiplies by the step budget, which is why it is
        // invisible until the files are real.
        //
        // The newest read is what the coder is working from. An older one it still
        // needs it can read again -- and the repeat guard nudges rather than kills,
        // so re-reading is cheap in a way it did not used to be.
        std::vector<bool> replay(steps.size(), false);
        {
            std::size_t spent = 0;
            for (std::size_t i = steps.size(); i-- > 0;) {
                if (steps[i].call.tool != CoderTool::Read) continue;
                const std::size_t size = steps[i].result.output.size();
                if (spent + size > limits.max_replayed_reads && spent > 0) break;
                replay[i] = true;
                spent += size;
            }
        }

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
                // Nudged to stop rewriting the same file. A model that has just
                // written the file it was asked to change will otherwise write it
                // again, slightly differently, until the budget runs out -- an
                // observed run spent six of nine steps that way.
                //
                // EVERY part, not "your piece". The old wording said "if your piece
                // is done, finish now" after every single write, which on a piece
                // that spans two files is the prompt telling the coder to stop
                // halfway. Watched on the two-files eval: signature changed, caller
                // never opened.
                out << "     -> "
                    << (step.result.ok
                            ? (step.result.no_op
                                   ? step.result.output + " -- stop rewriting it"
                                   : std::string("written. If EVERY part of your "
                                                 "piece is done, finish now."))
                            : step.result.output)
                    << "\n";
            } else if (step.call.tool == CoderTool::Read && !replay[i]) {
                // Stated, not silently dropped. A coder that cannot see a file it
                // read must know it can read it again, or it will assume the
                // contents were empty.
                out << "     -> read earlier ("
                    << step.result.output.size() / 1000
                    << "KB); read it again if you still need it\n";
            } else {
                out << "     -> " << step.result.output << "\n";
            }
        }
        out << "\n";

        // What has actually changed, gathered in one place.
        //
        // The numbered list above is a history and reads as one; a coder four
        // writes deep has to reconstruct "which files have I dealt with" from it
        // every turn, and a mid-sized model does not reliably manage that. This is
        // the same information as state rather than as narrative, which is what a
        // model needs to answer "what is left".
        std::vector<std::string> changed;
        for (const CoderStep& step : steps) {
            if (!step.result.ok || step.result.no_op) continue;
            if (step.call.tool != CoderTool::Write &&
                step.call.tool != CoderTool::Delete) {
                continue;
            }
            if (std::find(changed.begin(), changed.end(), step.call.path) ==
                changed.end()) {
                changed.push_back(step.call.path);
            }
        }
        if (!changed.empty()) {
            out << "Files you have already changed:\n";
            for (const auto& path : changed) out << "  " << path << "\n";
            out << "Do not write these again unless something is actually wrong with "
                   "them. If your piece needs another file changed, do that now.\n\n";
        }
    }

    out << "Your next tool call, as JSON:\n";
    return out.str();
}

std::string researcher_prompt(const std::string& task,
                              const std::vector<std::string>& files,
                              const std::vector<CoderStep>& steps,
                              const CoderLimits& limits) {
    std::ostringstream out;

    out << "You are the Researcher. Investigate this project and report what a team "
           "of coders needs to know before they start.\n\n";

    out << "Report:\n"
           "- where the relevant things live\n"
           "- the conventions actually in use here, not the ones you would prefer\n"
           "- the files this work will need to touch\n\n";

    out << "You may only look. There is no verb here that changes anything.\n\n";

    out << "Answer with ONE JSON object per turn:\n"
           "  {\"tool\":\"list\"}                       what files exist\n"
           "  {\"tool\":\"read\",\"path\":\"file.py\"}     read one\n"
           "  {\"tool\":\"finish\",\"note\":\"…\"}         your report, when you have "
           "seen enough\n\n";

    out << "Finish within " << limits.max_steps
        << " steps. The note you finish with IS the report -- write it for somebody "
           "who has not seen the code.\n\n";

    out << "The task the team was given:\n" << task << "\n\n";

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
        out << "What you have looked at so far:\n";
        for (std::size_t i = 0; i < steps.size(); ++i) {
            out << "  " << (i + 1) << ". " << tool_name(steps[i].call.tool);
            if (!steps[i].call.path.empty()) out << " " << steps[i].call.path;
            out << "\n";
            // Read CONTENTS are replayed, unlike a coder's writes: what it saw is
            // the whole of what it has to reason from, and dropping it would make
            // the report a summary of the last file only.
            out << "     -> " << steps[i].result.output.substr(0, 2000) << "\n";
        }
        out << "\n";
    }

    out << "Your next tool call, as JSON:\n";
    return out.str();
}

std::string run_researcher(const Config& config, const std::string& task,
                           const std::filesystem::path& project,
                           const CoderLimits& limits, const std::string& model) {
    if (!std::filesystem::is_directory(project) || trim(task).empty()) return {};

    CoderLimits bounded = limits;
    bounded.read_only = true;      // whatever the caller passed
    bounded.allow_run = false;
    if (bounded.max_steps > 8) bounded.max_steps = 8;   // looking, not building

    OllamaClient ollama(config);
    GenerateOptions options;
    options.json = true;
    options.disable_thinking = true;
    options.temperature = 0.2;

    std::vector<CoderStep> steps;
    bool asked_again = false;
    // What it has already opened.
    //
    // The Researcher had its own loop and never got run_coder's repeat guard, so
    // nothing stopped it re-reading one file until the budget was gone. Traced on
    // a real repository: reads of router.cpp at steps 0, 1, 3 and 4, then a report
    // at step 5 -- and on the runs where it read once more than that, no report at
    // all. That was the "nothing reported" seen in about a third of runs.
    std::vector<std::string> already_read;

    for (int step = 0; step < bounded.max_steps; ++step) {
        const auto files = list_file_names(project);
        const auto reply = ollama.generate(
            model.empty() ? config.ollama_model : model,
            researcher_prompt(task, files, steps, bounded), options);
        if (!reply) break;

        CoderStep current;
        current.call = parse_tool_call(reply->response.empty() ? reply->thinking
                                                               : reply->response);
        current.result = run_tool(current.call, project, bounded);

        // Re-reading a file it has already seen. The contents are still in the
        // transcript, so sending them again buys nothing and costs a step out of
        // eight -- the scarcest thing this pass has.
        if (current.call.tool == CoderTool::Read && current.result.ok) {
            if (std::find(already_read.begin(), already_read.end(), current.call.path) !=
                already_read.end()) {
                current.result.output =
                    "You have already read " + current.call.path +
                    " -- its contents are above. Read something else, or call "
                    "finish now with what you have found.";
            } else {
                already_read.push_back(current.call.path);
            }
        }

        if (current.call.tool == CoderTool::Finish) {
            if (!trim(current.call.note).empty()) return current.call.note;

            // FINISHED WITHOUT SAYING ANYTHING.
            //
            // Measured at roughly one run in three against a real repository, and
            // it silently cost the whole research pass -- the Director and every
            // coder then worked with no findings at all, and the run log said only
            // "nothing reported". For a role whose entire product is its closing
            // note, an empty note is not an answer, it is a dropped turn.
            //
            // So: say so, once, and let it try again. Returning empty here was
            // giving up without telling anybody, which is the same mistake the
            // repeat guard used to make.
            if (asked_again) break;
            asked_again = true;
            current.result.ok = false;
            current.result.output =
                "You finished without reporting anything. The note IS the product "
                "of this pass -- nothing else you did here is kept. Call finish "
                "again and put what you found in the note: where the relevant code "
                "lives, the conventions in use, and which files this work will "
                "touch.";
        }

        // The last step is spent asking for the report rather than on another
        // read, because a report that never arrives is worth less than one built
        // on slightly less reading.
        if (step == bounded.max_steps - 2 && !asked_again) {
            current.result.output +=
                "\n\nThis is your last look. Next turn, call finish and put "
                "everything you have found in the note.";
        }

        steps.push_back(std::move(current));
    }

    // Out of steps without a report. What it read is not nothing, but it is not a
    // report either, and inventing one from the transcript would be putting words
    // in its mouth for every downstream role to trust.
    return {};
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

    for (const char* key : {"find", "old", "old_string", "search"}) {
        if (document.contains(key) && document[key].is_string()) {
            call.find = document[key].get<std::string>();
            break;
        }
    }
    for (const char* key : {"replace", "new", "new_string", "with"}) {
        if (document.contains(key) && document[key].is_string()) {
            call.replace_with = document[key].get<std::string>();
            break;
        }
    }

    // Where a read should start. Several spellings, because a model asked for an
    // offset will reach for whichever word it knows.
    for (const char* key : {"from", "from_line", "offset", "start", "start_line"}) {
        if (document.contains(key) && document[key].is_number_integer()) {
            call.from_line = document[key].get<int>();
            break;
        }
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

    if (call.tool == CoderTool::Skill) {
        call.skill = trim(string_field(document, "skill"));
        if (call.skill.empty()) call.skill = trim(string_field(document, "name"));
        if (call.skill.empty()) call.skill = trim(string_field(document, "path"));
        if (call.skill.empty()) {
            call.error = "skill needs a \"skill\" name from the list above";
            call.tool  = CoderTool::Unknown;
            return call;
        }
    }

    if (call.tool == CoderTool::Mcp) {
        call.mcp_tool = trim(string_field(document, "mcp_tool"));
        if (call.mcp_tool.empty()) call.mcp_tool = trim(string_field(document, "name"));
        if (call.mcp_tool.empty()) call.mcp_tool = trim(string_field(document, "tool_name"));
        if (call.mcp_tool.empty()) {
            call.error = "mcp needs a \"name\" from the list above, like "
                         "\"server.tool\"";
            call.tool  = CoderTool::Unknown;
            return call;
        }
        // Arguments as an OBJECT, kept as text. Re-serialised rather than passed
        // through so what reaches the server is JSON we produced, not a string
        // the model claimed was JSON.
        if (document.contains("arguments") && document["arguments"].is_object()) {
            call.mcp_arguments = safe_dump(document["arguments"]);
        } else if (document.contains("args") && document["args"].is_object()) {
            call.mcp_arguments = safe_dump(document["args"]);
        } else {
            call.mcp_arguments = "{}";
        }
    }

    // The command, as an ARRAY. A string would have to be word-split, and the
    // splitter is where a shell creeps back in -- so a string is refused with a
    // note saying what shape is wanted.
    if (call.tool == CoderTool::Run) {
        for (const char* key : {"command", "argv", "cmd"}) {
            if (!document.contains(key)) continue;
            const auto& value = document[key];
            if (value.is_array()) {
                for (const auto& part : value) {
                    if (part.is_string()) call.command.push_back(part.get<std::string>());
                    else if (part.is_number()) call.command.push_back(part.dump());
                }
                break;
            }
            if (value.is_string() && call.command.empty()) {
                call.error = "give \"command\" as an array of words, e.g. "
                             "[\"pytest\", \"-q\"], not as one string";
                call.tool  = CoderTool::Unknown;
                return call;
            }
        }
        if (call.command.empty() && call.error.empty()) {
            call.error = "run needs a \"command\" array, e.g. [\"pytest\", \"-q\"]";
            call.tool  = CoderTool::Unknown;
            return call;
        }
    }

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
                    const CoderLimits& limits, const SkillSet& skills,
                    const McpAccess& mcp) {
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

    // Enforced here rather than by leaving the verbs out of the prompt: a model
    // that asks for `write` anyway must get a refusal, not a written file.
    if (limits.read_only &&
        (call.tool == CoderTool::Write || call.tool == CoderTool::Delete ||
         call.tool == CoderTool::Replace || call.tool == CoderTool::Run)) {
        result.output = "this is a read-only pass; you may only list, read and finish";
        return result;
    }

    // The user's gate, after the built-in refusals and before anything happens.
    //
    // After, because a hook should not be woken up to re-refuse what read_only has
    // already refused. Before, because a hook that ran after the write would be a
    // log entry rather than a gate.
    if (!limits.hooks.empty()) {
        const std::string detail =
            call.tool == CoderTool::Run
                ? (call.command.empty() ? std::string{} : call.command[0])
                : call.path;
        const HookOutcome gate = run_pre_tool_hooks(std::string(tool_name(call.tool)),
                                                    detail, limits.hooks);
        if (gate.blocked) {
            // Handed back as an ordinary tool failure, so the model reads the
            // reason and can try something else -- which is what a person setting
            // a gate usually wants, rather than the run dying.
            result.output = gate.reason;
            return result;
        }
    }

    if (call.tool == CoderTool::Run) {
        if (!limits.allow_run) {
            result.output = "running commands is turned off for this crew";
            return result;
        }
        if (call.command.empty()) {
            result.output = "run needs a command";
            return result;
        }
        if (!is_runnable(call.command.front())) {
            // Named, so the model can pick something else rather than guessing at
            // what is allowed. Listing the whole allowlist here would be a wall of
            // text on every mistake; the prompt already carries it.
            result.output = "refused: \"" + call.command.front() +
                            "\" is not a program this crew may run";
            return result;
        }

        const LimitedResult ran = run_limited(call.command, sandbox.string(),
                                              limits.run_timeout_seconds,
                                              limits.max_run_output);

        std::string report;
        if (ran.timed_out) {
            report = "timed out after " + std::to_string(limits.run_timeout_seconds) +
                     "s and was killed\n";
        } else {
            report = "exit " + std::to_string(ran.exit_code) + "\n";
        }
        report += ran.output;
        if (ran.truncated) report += "\n... (output truncated)";

        // A non-zero exit is NOT a failed tool call. The command ran; it reported
        // failing tests, which is exactly the information the coder asked for and
        // needs to act on. Marking it failed would trip the no-progress guard on
        // the most useful turn in the loop.
        result.ok = !ran.timed_out;
        result.output = report;
        return result;
    }

    if (call.tool == CoderTool::Skill) {
        for (const auto& skill : skills.skills) {
            if (skill.name != call.skill) continue;
            result.ok = true;
            // The BODY, which is the whole point: the catalogue said one line, and
            // this is the pages behind it, delivered only now that it is wanted.
            result.output = skill.body.empty() ? "(that skill has no instructions)"
                                               : skill.body;
            return result;
        }
        result.output = "there is no skill called \"" + call.skill + "\"";
        return result;
    }

    if (call.tool == CoderTool::Mcp) {
        if (mcp.empty()) {
            result.output = "no MCP servers are configured";
            return result;
        }
        // Checked against the DISCOVERED list before anything is sent. The server
        // said which tools it has; a name that is not among them is refused here
        // rather than forwarded for the server to reject.
        const bool known = std::any_of(
            mcp.tools.begin(), mcp.tools.end(),
            [&call](const McpTool& t) { return t.qualified() == call.mcp_tool; });
        if (!known) {
            result.output = "there is no MCP tool called \"" + call.mcp_tool + "\"";
            return result;
        }

        const auto [ok, text] = mcp.call(call.mcp_tool, call.mcp_arguments);
        result.ok     = ok;
        result.output = text;
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
            // From a line, when asked. This is what makes a file bigger than the
            // cap reachable at all -- see ToolCall::from_line.
            int first_line = 1;
            if (call.from_line > 1) {
                std::size_t at = 0;
                int line = 1;
                while (line < call.from_line && at < contents.size()) {
                    const auto next = contents.find('\n', at);
                    if (next == std::string::npos) { at = contents.size(); break; }
                    at = next + 1;
                    ++line;
                }
                if (at >= contents.size()) {
                    result.output = call.path + " has fewer than " +
                                    std::to_string(call.from_line) + " lines";
                    return result;
                }
                contents = contents.substr(at);
                first_line = line;
            }

            // Truncated, and SAID to be -- with the line to ask for next, so the
            // notice is something the coder can act on rather than just a warning.
            // A coder that thinks it has seen a whole file will rewrite it from
            // the part it saw and delete the rest.
            if (contents.size() > limits.max_read_bytes) {
                contents.resize(limits.max_read_bytes);
                const int shown =
                    first_line +
                    static_cast<int>(std::count(contents.begin(), contents.end(), '\n'));
                contents += "\n... (truncated. This file is longer than shown; do "
                            "not rewrite it whole from this. To see the rest, read "
                            "it again with \"from\": " + std::to_string(shown) + ")";
            }
            result.ok = true;
            result.output = contents.empty() ? "(empty file)" : contents;
            if (first_line > 1) {
                result.output = "(from line " + std::to_string(first_line) + ")\n" +
                                result.output;
            }
            return result;
        }

        case CoderTool::Replace: {
            if (!std::filesystem::is_regular_file(*target, ec)) {
                result.output = "no such file: " + call.path;
                return result;
            }
            if (call.find.empty()) {
                result.output = "replace needs the exact text to find";
                return result;
            }

            const std::string contents = read_file(*target);
            const auto at = contents.find(call.find);
            if (at == std::string::npos) {
                // Named precisely: "not found" sends a model looking for a
                // different file, when the real answer is almost always that its
                // copy of the text differs by whitespace.
                result.output =
                    "that exact text is not in " + call.path +
                    ". It must match character for character, including indentation. "
                    "Read the file again and copy the text from what you see.";
                return result;
            }

            // AMBIGUITY IS REFUSED. If the text appears twice, replacing the first
            // is a guess -- and a wrong guess edits a line the coder never looked
            // at, in a file it cannot see all of. Refusing costs one turn; being
            // wrong costs a silent corruption.
            if (contents.find(call.find, at + 1) != std::string::npos) {
                result.output =
                    "that text appears more than once in " + call.path +
                    ". Include enough surrounding lines to make it unique.";
                return result;
            }

            if (call.find == call.replace_with) {
                result.no_op = true;
                result.ok = true;
                result.output = "no change: the new text is the same as the old";
                return result;
            }

            std::string updated = contents;
            updated.replace(at, call.find.size(), call.replace_with);
            {
                std::ofstream out(*target, std::ios::binary | std::ios::trunc);
                if (!out) {
                    result.output = "could not write " + call.path;
                    return result;
                }
                out << updated;
                if (!out) {
                    result.output = "could not write " + call.path;
                    return result;
                }
            }

            const int line =
                1 + static_cast<int>(std::count(contents.begin(),
                                                contents.begin() + static_cast<long>(at),
                                                '\n'));
            result.ok = true;
            result.output = "replaced, at line " + std::to_string(line);
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
                       const CoderLimits& limits, const std::string& model,
                       const std::filesystem::path& mailbox, const SkillSet& skills,
                       const McpAccess& mcp, const CoderProgress& on_step) {
    // Where the names in this subtask are DEFINED, worked out ONCE.
    //
    // Parsing the project costs a few seconds and cannot change while this coder
    // runs, so doing it per turn would be a tax on every call for an answer that
    // is already known. It is the difference between a coder jumping to line 1165
    // and one scanning 24KB at a time until its budget is gone.
    const std::string where =
        symbols_note(sandbox, subtask.title + " " + subtask.detail);

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
        const std::vector<std::string> files = list_file_names(sandbox);

        // Checked between turns, so a message lands at the next decision rather
        // than needing the coder to be interrupted mid-call.
        const std::string prompt =
            coder_prompt(subtask, files, outcome.steps, limits, take_steer(mailbox),
                         skills, mcp, where);

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
        current.result = run_tool(current.call, sandbox, limits, skills, mcp);

        if (on_step) on_step(current);

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
            ++repeats;
            // SAY SO FIRST, END SECOND.
            //
            // This used to end the run outright the moment the counter tripped, and
            // that threw away everything the coder had not got to yet. Watched on
            // the two-files eval: asked to change a signature AND update its caller,
            // the coder rewrote the signature file four times, tripped this, and the
            // loop returned before it ever opened the caller. The task was then
            // scored as a model failure. It was not one -- the model was never given
            // the chance, and the guard meant to stop waste was throwing away work.
            //
            // A coder that is told it is repeating itself usually moves on, which is
            // the whole point of a transcript it can read. So the first trip is a
            // message, and only a coder that keeps going after being told is stopped.
            if (repeats == limits.max_repeats) {
                current.result.output +=
                    "\n\nYou have now made this same call " + std::to_string(repeats) +
                    " times without getting anywhere. Stop repeating it. If this "
                    "piece has another part you have not done yet -- another file to "
                    "change, a caller to update -- do that next. If it is genuinely "
                    "done, finish.";
            } else if (repeats > limits.max_repeats) {
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
