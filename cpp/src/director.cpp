#include "auspex/director.hpp"

#include <algorithm>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"
#include "auspex/roles.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

std::string string_field(const json& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto& value = object[key];
    return value.is_string() ? value.get<std::string>() : std::string{};
}

// The subtask array, wherever the model decided to put it.
const json* subtask_array(const json& document) {
    if (document.is_array()) return &document;
    if (!document.is_object()) return nullptr;
    for (const char* key : {"subtasks", "tasks", "plan", "steps"}) {
        if (document.contains(key) && document[key].is_array()) return &document[key];
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
const std::vector<std::string>& director_roles() {
    // Matching ollamadev's `crew role` built-ins, so a plan made here means the
    // same thing as a plan made there.
    static const std::vector<std::string> kRoles{"coder",    "tester",  "docs",
                                                 "refactor", "security", "reviewer"};
    return kRoles;
}

bool is_known_role(const std::string& role) {
    const auto& roles = director_roles();
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

// ---------------------------------------------------------------------------
std::string director_prompt(const std::string& task,
                            const std::vector<std::string>& files,
                            int max_subtasks, const std::string& hint) {
    if (max_subtasks < 1) max_subtasks = 1;

    std::ostringstream out;
    out << "You are the Director of a small engineering crew.\n"
           "Split the task below into independent pieces that different people "
           "could do at the same time, without talking to each other.\n\n";

    out << "Rules:\n"
           "- At most " << max_subtasks << " pieces. Fewer is better. "
           "One piece is a perfectly good plan for a small task.\n"
           "- Pieces must not depend on each other's work, because they run in "
           "parallel in separate copies of the project.\n"
           "- Two pieces must not edit the same file.\n"
           "- Every piece must be worth doing on its own.\n"
           "- role must be one of these, and the role decides how the piece is "
           "done:\n";
    // With descriptions, not as a bare list of words. The Director was previously
    // choosing between six nouns and had to guess what they meant; "reviewer"
    // in particular reads like a coder unless you are told it never edits.
    out << role_catalog(all_personas()) << "\n";

    // The file listing, not the contents. The Director decides how to cut a job
    // up; for that it needs to know what exists. Contents would spend the whole
    // context window and still not fit.
    if (!files.empty()) {
        out << "The project contains these files:\n";
        // Capped, because a large repository would otherwise be the entire prompt.
        // Truncation is STATED rather than silent -- a Director that thinks it has
        // seen everything plans as though it has.
        constexpr std::size_t kMaxListed = 300;
        const std::size_t shown = std::min(files.size(), kMaxListed);
        for (std::size_t i = 0; i < shown; ++i) out << "  " << files[i] << "\n";
        if (files.size() > shown) {
            out << "  ... and " << (files.size() - shown) << " more files not listed\n";
        }
        out << "\n";
    }

    // Before the task, so it is read as context for it rather than as an
    // afterthought. A filename list says what exists; this says what is relevant.
    if (!hint.empty()) out << hint;

    out << "The task:\n" << task << "\n\n";

    out << "Answer with JSON only, in this exact shape:\n"
           "{\"summary\": \"one sentence about the whole job\",\n"
           " \"subtasks\": [{\"role\": \"coder\", \"title\": \"short name for this "
           "piece\", \"detail\": \"what this person should do\"}]}\n";

    return out.str();
}

std::string extract_json(const std::string& text) {
    // A fence first, because its contents are unambiguous when there is one.
    if (const auto fence = text.find("```"); fence != std::string::npos) {
        const auto body_start = text.find('\n', fence);
        if (body_start != std::string::npos) {
            const auto close = text.find("```", body_start);
            if (close != std::string::npos) {
                const std::string inner =
                    text.substr(body_start + 1, close - body_start - 1);
                if (const std::string found = extract_json(inner); !found.empty()) {
                    return found;
                }
            }
        }
    }

    // Otherwise the first balanced object or array. Balanced, not "to the last
    // brace": a reply with prose after the JSON containing a '}' would otherwise
    // swallow it and fail to parse.
    const auto open = text.find_first_of("{[");
    if (open == std::string::npos) return {};

    const char opener = text[open];
    const char closer = opener == '{' ? '}' : ']';

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  in_string = false;
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == opener) ++depth;
        else if (c == closer && --depth == 0) return text.substr(open, i - open + 1);
    }
    return {};
}

Plan parse_plan(const std::string& reply, int max_subtasks) {
    Plan plan;
    if (max_subtasks < 1) max_subtasks = 1;

    const std::string body = extract_json(reply);
    if (body.empty()) {
        plan.error = "the Director did not answer with a plan";
        return plan;
    }

    const auto document = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        plan.error = "the Director's plan did not parse";
        return plan;
    }

    if (document.is_object()) plan.summary = trim(string_field(document, "summary"));

    const json* entries = subtask_array(document);
    if (!entries) {
        plan.error = "the Director's plan listed no pieces";
        return plan;
    }

    for (const auto& entry : *entries) {
        PlannedSubtask subtask;

        if (entry.is_string()) {
            // A bare list of strings. Not the shape asked for, but unambiguous,
            // and refusing it would throw away a usable plan over formatting.
            subtask.title = trim(entry.get<std::string>());
        } else if (entry.is_object()) {
            subtask.title  = trim(string_field(entry, "title"));
            subtask.detail = trim(string_field(entry, "detail"));
            subtask.role   = trim(string_field(entry, "role"));
            if (subtask.title.empty()) subtask.title = trim(string_field(entry, "name"));
            if (subtask.detail.empty()) {
                subtask.detail = trim(string_field(entry, "description"));
            }
        } else {
            continue;
        }

        // A piece with nothing to do is not a piece. Dropping it keeps the
        // numbering dense, which matters because those numbers are what accept and
        // discard act on.
        if (subtask.title.empty() && subtask.detail.empty()) continue;
        if (subtask.title.empty()) subtask.title = subtask.detail.substr(0, 60);

        // The role is checked against the table, never passed through. It reaches
        // prompts and model routing, and "whatever the model said" is not a
        // category either of those can act on.
        if (!is_known_role(subtask.role)) subtask.role = "coder";

        subtask.n = static_cast<int>(plan.subtasks.size()) + 1;
        plan.subtasks.push_back(std::move(subtask));

        // Enforced on the reply as well as stated in the prompt: a model asked for
        // at most four will sometimes return six, and each extra one is a coder
        // that would really run.
        if (static_cast<int>(plan.subtasks.size()) >= max_subtasks) break;
    }

    if (plan.subtasks.empty()) plan.error = "the Director's plan listed no pieces";
    return plan;
}

std::string plan_shape(const Plan& plan) {
    // The SHAPE is how many pieces there are and which roles do them, in order.
    // Two plans that cut a job the same way but word the titles differently are
    // the same plan; comparing text would call them different and then pick
    // between them arbitrarily.
    std::string shape = std::to_string(plan.subtasks.size());
    for (const auto& subtask : plan.subtasks) {
        shape += "|" + subtask.role;
    }
    return shape;
}

Plan modal_plan(const std::vector<Plan>& plans) {
    Plan best;
    best.error = "no plan was produced";

    std::vector<std::pair<std::string, int>> counts;
    for (const auto& plan : plans) {
        if (!plan.ok() || plan.subtasks.empty()) continue;
        const std::string shape = plan_shape(plan);
        bool seen = false;
        for (auto& [key, n] : counts) {
            if (key != shape) continue;
            ++n;
            seen = true;
            break;
        }
        if (!seen) counts.emplace_back(shape, 1);
    }
    if (counts.empty()) return best;

    std::string winner = counts.front().first;
    int         most   = counts.front().second;
    for (const auto& [shape, n] : counts) {
        if (n > most) {
            most   = n;
            winner = shape;
        }
    }

    // The EARLIEST plan of the winning shape. Ties by first occurrence is the only
    // answer that does not depend on iteration order.
    for (const auto& plan : plans) {
        if (plan.ok() && !plan.subtasks.empty() && plan_shape(plan) == winner) {
            return plan;
        }
    }
    return best;
}

Plan plan_amplified(const Config& config, const std::string& task,
                    const std::vector<std::string>& files, int max_subtasks,
                    int attempts, const std::string& model, const std::string& hint) {
    if (attempts < 2) return plan_task(config, task, files, max_subtasks, model, hint);

    std::vector<Plan> plans;
    plans.reserve(static_cast<std::size_t>(attempts));
    for (int i = 0; i < attempts; ++i) {
        plans.push_back(plan_task(config, task, files, max_subtasks, model, hint));
    }
    return modal_plan(plans);
}

Plan plan_task(const Config& config, const std::string& task,
               const std::vector<std::string>& files, int max_subtasks,
               const std::string& model, const std::string& hint) {
    Plan plan;

    const std::string trimmed = trim(task);
    if (trimmed.empty()) {
        plan.error = "there is no task to plan";
        return plan;
    }

    OllamaClient ollama(config);

    GenerateOptions options;
    options.json = true;              // constrain decoding for models that honour it
    options.disable_thinking = true;  // or a reasoning model spends its budget and
                                      // returns an empty `response`
    options.temperature = 0.2;        // planning is not where variety helps

    const auto reply = ollama.generate(model.empty() ? config.ollama_model : model,
                                       director_prompt(trimmed, files, max_subtasks, hint),
                                       options);
    if (!reply) {
        plan.error = "could not reach the model";
        return plan;
    }

    // `thinking` as a fallback: a model that ignored disable_thinking puts its
    // answer there, and refusing it would fail a run that actually succeeded.
    const std::string text =
        reply->response.empty() ? reply->thinking : reply->response;
    return parse_plan(text, max_subtasks);
}

}  // namespace auspex
