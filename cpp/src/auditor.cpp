#include "auspex/auditor.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/crew.hpp"
#include "auspex/ollama_client.hpp"
#include "auspex/process.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

std::string string_field(const json& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto& value = object[key];
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number() || value.is_boolean()) return value.dump();
    return {};
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// One pattern plus what to call it when it matches.
struct SecretPattern {
    const char* what;
    const char* regex;
};

const std::vector<SecretPattern>& secret_patterns() {
    // Shapes that are unmistakable, plus assignments to obviously named things.
    // Anchored on the ADDED-line body, which the caller has already stripped of
    // its leading '+'.
    static const std::vector<SecretPattern> kPatterns{
        {"a private key", R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)"},
        {"an AWS access key id", R"(\bAKIA[0-9A-Z]{16}\b)"},
        {"a GitHub token", R"(\bgh[pousr]_[A-Za-z0-9]{20,}\b)"},
        {"a Slack token", R"(\bxox[baprs]-[A-Za-z0-9-]{10,}\b)"},
        {"a Google API key", R"(\bAIza[0-9A-Za-z_\-]{35}\b)"},
        {"a JSON Web Token", R"(\beyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.)"},
        // A credential assigned to a plainly named variable. The value must be
        // long enough and not obviously a placeholder -- see is_placeholder().
        {"a hard-coded credential",
         R"((?:api[_-]?key|secret|token|password|passwd|passphrase|access[_-]?key)"
         R"()\s*[:=]\s*["']([^"']{8,})["'])"},
    };
    return kPatterns;
}

// Values that are plainly not real. Without this, every example, test fixture and
// bit of documentation in a project trips the scanner, and an Auditor that cries
// wolf on every changeset is one whose holds stop being read.
bool is_placeholder(const std::string& value) {
    const std::string v = lowered(value);
    static const std::vector<std::string> kTells{
        "xxx",       "yyy",    "placeholder", "example",  "changeme", "change_me",
        "your_",     "your-",  "insert",      "dummy",    "fake",     "sample",
        "test",      "todo",   "redacted",    "<",        "${",       "os.environ",
        "getenv",    "secret", "password",    "123456",   "abcdef",   "...",
    };
    for (const auto& tell : kTells) {
        if (v.find(tell) != std::string::npos) return true;
    }
    // All one character, e.g. "********".
    return !v.empty() && v.find_first_not_of(v[0]) == std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<std::string> scan_secrets(const std::string& diff) {
    std::vector<std::string> found;

    for (const auto& line : split_lines(diff)) {
        // Added lines only. Removing a secret is the opposite of a problem, and
        // flagging it would make cleaning one up impossible to land.
        if (classify_diff_line(line) != DiffLine::Added) continue;
        const std::string body = line.substr(1);

        for (const auto& pattern : secret_patterns()) {
            std::smatch match;
            const std::regex re(pattern.regex, std::regex::icase);
            if (!std::regex_search(body, match, re)) continue;

            // The credential-assignment pattern captures its value, so a
            // placeholder can be told from the real thing. The fixed-shape
            // patterns capture nothing and are always reported -- there is no
            // such thing as a placeholder AWS key id that matches AKIA + 16.
            if (match.size() > 1 && is_placeholder(match[1].str())) continue;

            found.push_back(std::string(pattern.what) + " in: " + trim(body).substr(0, 80));
            break;   // one finding per line is enough to hold it
        }
    }

    return found;
}

Audit deterministic_audit(const Changeset& changeset, const AuditLimits& limits) {
    Audit audit;
    audit.certain = true;

    if (changeset.empty()) {
        audit.reason = "the coder changed nothing";
        return audit;
    }

    // Containment, again. capture_changeset() produces project-relative paths, so
    // this should be unreachable -- which is exactly why it is checked. The cost
    // is nothing and the failure it guards against is a file written outside the
    // project.
    for (const auto& file : changeset.files) {
        if (!safe_join("/nonexistent-probe-root", file.path)) {
            audit.reason = "a changed path is not inside the project: " + file.path;
            return audit;
        }
    }

    if (static_cast<int>(changeset.files.size()) > limits.max_files) {
        audit.reason = "this touches " + std::to_string(changeset.files.size()) +
                       " files, which is more than one piece of work should";
        return audit;
    }

    if (const auto secrets = scan_secrets(changeset.diff); !secrets.empty()) {
        audit.reason = "this looks like it adds a credential";
        audit.notes  = secrets;
        return audit;
    }

    if (changeset.diff.size() > limits.max_diff_bytes) {
        // Not a judgement on the code. A review of a patch that was not shown in
        // full is not a review, and saying so is more honest than asking a model
        // about the first half.
        audit.reason = "the diff is too large to review in one pass (" +
                       std::to_string(changeset.diff.size() / 1000) + "k)";
        return audit;
    }

    // Nothing decidably wrong. NOT a verdict -- only permission to ask the model.
    audit.verdict = Verdict::Accept;
    audit.reason.clear();
    return audit;
}

// ---------------------------------------------------------------------------
std::string auditor_prompt(const PlannedSubtask& subtask, const Changeset& changeset,
                           const AuditLimits& limits) {
    std::ostringstream out;

    out << "You are the Auditor of a small engineering crew. A coder has produced "
           "the change below. Decide whether it should land in the project or be "
           "held for a person to look at.\n\n";

    out << "The coder was asked to:\n" << subtask.title << "\n";
    if (!subtask.detail.empty()) out << subtask.detail << "\n";
    out << "\n";

    // The rules are NUMBERED and the answer must name one.
    //
    // An observed run held correct Python with the reason "the docstring is
    // incorrectly placed before the return statement instead of after the function
    // definition and before the code" -- which describes the same position twice
    // and was simply wrong. Free-form prose lets a model narrate an objection it
    // has not checked. Making it pick a numbered rule AND quote the line it
    // objects to forces the reason to point at text that really exists.
    out << "Hold it ONLY if one of these is true. Nothing else is a reason to "
           "hold.\n"
           "  1. It does not do what was asked.\n"
           "  2. It does something that was NOT asked for as well.\n"
           "  3. It deletes or rewrites work unrelated to the task.\n"
           "  4. It is broken: a syntax error, or a call to something that does "
           "not exist.\n"
           "  5. It adds a credential, a key, or a password.\n\n";

    out << "Do NOT hold for style, formatting, naming, missing type hints, or "
           "because you would have written it differently. Working code that does "
           "what was asked is an accept even if it is not how you would do it.\n\n";

    out << "Files changed:\n";
    for (const auto& file : changeset.files) {
        out << "  " << file.path << (file.deleted ? "  (deleted)" : "") << "\n";
    }
    out << "\n";

    std::string diff = changeset.diff;
    if (diff.size() > limits.max_diff_bytes) {
        // Should not happen -- deterministic_audit() holds an oversized diff before
        // this is reached -- but if the caller skipped that, the truncation is
        // stated so the model does not review a fragment as though it were whole.
        diff.resize(limits.max_diff_bytes);
        diff += "\n... (diff truncated; you have not seen all of it)\n";
    }
    out << "The change:\n" << diff << "\n\n";

    out << "Answer with JSON only:\n"
           "{\"verdict\": \"accept\" or \"hold\",\n"
           " \"rule\": the number above you are holding under, or 0 to accept,\n"
           " \"quote\": the exact line from the diff that is wrong, when holding,\n"
           " \"reason\": one sentence}\n";

    return out.str();
}

bool quote_is_real(const std::string& quote, const std::string& diff) {
    // Whitespace collapsed and any leading +/-/space dropped on BOTH sides. A model
    // that is right about the text and careless about the margin, or that
    // re-indents while quoting, is making a real objection badly -- not inventing
    // one, which is what this is for.
    const auto flatten = [](const std::string& text) {
        std::string out;
        bool space = false;
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                space = !out.empty();
                continue;
            }
            if (space) out.push_back(' ');
            space = false;
            out.push_back(c);
        }
        return out;
    };

    // The margin is stripped from BOTH sides. A model that copies a line straight
    // out of the patch brings its leading '+' with it, and refusing that would
    // call the most careful kind of quoting an invention.
    const auto strip_margin = [](std::string text) {
        if (!text.empty() && (text[0] == '+' || text[0] == '-' || text[0] == ' ')) {
            text.erase(0, 1);
        }
        return text;
    };

    const std::string needle = flatten(strip_margin(quote));
    if (needle.empty()) return false;

    for (const auto& line : split_lines(diff)) {
        if (flatten(strip_margin(line)).find(needle) != std::string::npos) return true;
    }
    return false;
}

Audit parse_audit(const std::string& reply) {
    Audit audit;   // holds by default, and every path below leaves it that way
                   // unless it finds an explicit accept

    const std::string body = extract_json(reply);
    if (body.empty()) {
        audit.reason = "the Auditor did not answer";
        return audit;
    }

    const auto document = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        audit.reason = "the Auditor's answer did not parse";
        return audit;
    }

    std::string verdict = lowered(trim(string_field(document, "verdict")));
    if (verdict.empty()) verdict = lowered(trim(string_field(document, "decision")));

    const std::string reason = trim(string_field(document, "reason"));

    if (verdict == "accept") {
        audit.verdict = Verdict::Accept;
        audit.reason  = reason;   // may be empty; an accept needs no defence
        return audit;
    }

    if (verdict == "hold") {
        audit.reason = reason.empty() ? "the Auditor held it without saying why"
                                      : reason;
        // NOT trimmed. Indentation is frequently the whole objection ("this is
        // inside the loop"), and a quote shown to a person should look like the
        // line it came from. quote_is_real() flattens whitespace itself, so
        // keeping it costs the comparison nothing.
        audit.quote = string_field(document, "quote");
        return audit;
    }

    // Anything else. "yes", "ok", "approved", "" -- none of these are the word
    // that was asked for, and treating a near-miss as an accept is how a garbled
    // reply lands a patch.
    audit.reason = verdict.empty()
                       ? "the Auditor gave no verdict"
                       : "the Auditor's verdict was not understood: \"" + verdict + "\"";
    if (!reason.empty()) audit.notes.push_back(reason);
    return audit;
}

Audit audit_changeset(const Config& config, const PlannedSubtask& subtask,
                      const Changeset& changeset, const AuditLimits& limits,
                      const std::string& model) {
    // Cheap and certain first. If this holds, no model is asked -- there is
    // nothing a model could say that would make a leaked key acceptable.
    Audit certain = deterministic_audit(changeset, limits);
    if (certain.held()) return certain;

    OllamaClient ollama(config);

    GenerateOptions options;
    options.json = true;
    options.disable_thinking = true;
    // Lowest of the three roles. The Auditor is not being asked to be creative;
    // it is being asked to apply a list.
    options.temperature = 0.1;

    const auto reply = ollama.generate(model.empty() ? config.ollama_model : model,
                                       auditor_prompt(subtask, changeset, limits),
                                       options);
    if (!reply) {
        Audit unreachable;
        unreachable.reason = "the Auditor could not be reached, so this was not reviewed";
        return unreachable;   // Hold. An unreviewed change is not an approved one.
    }

    const std::string text =
        reply->response.empty() ? reply->thinking : reply->response;
    Audit audit = parse_audit(text);

    // A hold that quotes something the patch never contained is an invention. It
    // still holds -- failing closed does not bend for this -- but it is labelled,
    // because "the Auditor objected to a line that is not there" is the single
    // most useful thing a person deciding can be told.
    if (audit.held() && !audit.quote.empty() &&
        !quote_is_real(audit.quote, changeset.diff)) {
        audit.notes.push_back(
            "the Auditor quoted a line that is not in this diff, so its reason may "
            "be invented: \"" + audit.quote + "\"");
    }
    return audit;
}

}  // namespace auspex
