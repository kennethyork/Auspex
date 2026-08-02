#include "auspex/usage.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

namespace auspex {

namespace {

std::mutex& usage_mutex() {
    static std::mutex m;
    return m;
}

// Function-local rather than a file-scope object: this is written to from coder
// threads that may outlive main's static destruction order otherwise.
std::map<std::string, Tally>& usage_table() {
    static std::map<std::string, Tally> table;
    return table;
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 1234567 -> "1.23M". Token counts run to millions in a single crew run, and a
// bare digit string at that length is read wrong more often than it is read.
std::string human(std::int64_t n) {
    std::ostringstream out;
    if (n >= 1'000'000) {
        out.precision(2);
        out << std::fixed << (static_cast<double>(n) / 1'000'000.0) << "M";
    } else if (n >= 1'000) {
        out.precision(1);
        out << std::fixed << (static_cast<double>(n) / 1'000.0) << "k";
    } else {
        out << n;
    }
    return out.str();
}

}  // namespace

bool is_metered_model(const std::string& model) {
    const std::string m = lowered(model);
    if (m.empty()) return false;
    // Ollama's hosted models.
    if (m.find("cloud") != std::string::npos) return true;
    // The agent CLIs, recorded under their own id when a role is handed to one.
    // These are frontier models on somebody's account whatever they are pointed at.
    static const char* kMetered[] = {"claude", "codex", "gemini", "openai", "gpt-",
                                     "anthropic", "opencode", "cursor"};
    for (const char* needle : kMetered) {
        if (m.find(needle) != std::string::npos) return true;
    }
    return false;
}

void record_usage(const std::string& model, int prompt_tokens, int eval_tokens) {
    if (model.empty()) return;
    // A generate() that came back without counts is not a free call, it is an
    // unmeasured one -- send it where unmeasured calls go rather than adding zero.
    if (prompt_tokens <= 0 && eval_tokens <= 0) {
        record_opaque_usage(model);
        return;
    }
    const std::lock_guard<std::mutex> lock(usage_mutex());
    Tally& t = usage_table()[model];
    t.prompt += std::max(0, prompt_tokens);
    t.eval += std::max(0, eval_tokens);
    t.calls += 1;
}

void record_opaque_usage(const std::string& model) {
    if (model.empty()) return;
    const std::lock_guard<std::mutex> lock(usage_mutex());
    usage_table()[model].opaque_calls += 1;
}

std::map<std::string, Tally> usage_snapshot() {
    const std::lock_guard<std::mutex> lock(usage_mutex());
    return usage_table();
}

std::map<std::string, Tally> usage_since(const std::map<std::string, Tally>& before) {
    std::map<std::string, Tally> delta;
    for (const auto& [model, now] : usage_snapshot()) {
        Tally was;
        const auto found = before.find(model);
        if (found != before.end()) was = found->second;

        Tally d;
        d.prompt = now.prompt - was.prompt;
        d.eval = now.eval - was.eval;
        d.calls = now.calls - was.calls;
        d.opaque_calls = now.opaque_calls - was.opaque_calls;
        // A model that did nothing in this window is not in this window's report.
        if (d.calls > 0 || d.opaque_calls > 0 || d.total() > 0) delta[model] = d;
    }
    return delta;
}

Tally usage_total(const std::map<std::string, Tally>& tally) {
    Tally sum;
    for (const auto& [model, t] : tally) {
        (void)model;
        sum.prompt += t.prompt;
        sum.eval += t.eval;
        sum.calls += t.calls;
        sum.opaque_calls += t.opaque_calls;
    }
    return sum;
}

Tally usage_metered_total(const std::map<std::string, Tally>& tally) {
    Tally sum;
    for (const auto& [model, t] : tally) {
        if (!is_metered_model(model)) continue;
        sum.prompt += t.prompt;
        sum.eval += t.eval;
        sum.calls += t.calls;
        sum.opaque_calls += t.opaque_calls;
    }
    return sum;
}

std::string usage_report(const std::map<std::string, Tally>& tally,
                         const std::string& title) {
    std::ostringstream out;
    if (!title.empty()) out << title << "\n";
    if (tally.empty()) {
        out << "  no model calls\n";
        return out.str();
    }

    // Metered first, then by tokens spent. Someone scanning this wants the line
    // that costs money at the top, not in alphabetical order among the free ones.
    std::vector<std::pair<std::string, Tally>> rows(tally.begin(), tally.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        const bool ma = is_metered_model(a.first);
        const bool mb = is_metered_model(b.first);
        if (ma != mb) return ma;
        if (a.second.total() != b.second.total()) return a.second.total() > b.second.total();
        return a.first < b.first;
    });

    for (const auto& [model, t] : rows) {
        out << "  " << (is_metered_model(model) ? "$ " : "  ") << model << "\n";
        if (t.calls > 0) {
            out << "      " << t.calls << " call" << (t.calls == 1 ? "" : "s") << ", "
                << human(t.prompt) << " in, " << human(t.eval) << " out, "
                << human(t.total()) << " total\n";
        }
        if (t.opaque_calls > 0) {
            // Never "0 tokens" -- see the header. Unmeasured is not free.
            out << "      " << t.opaque_calls << " call"
                << (t.opaque_calls == 1 ? "" : "s") << ", tokens not reported\n";
        }
    }

    const Tally all = usage_total(tally);
    const Tally paid = usage_metered_total(tally);
    if (all.calls == 0) {
        // Every call was unmeasured. "total 0 tokens" would be true of the
        // measurement and false about the cost, and it is the cost somebody is
        // reading this for -- so the figure is omitted rather than printed as zero.
        out << "  no measured tokens; " << all.opaque_calls << " call"
            << (all.opaque_calls == 1 ? "" : "s") << " did not report any\n";
    } else {
        out << "  total " << human(all.total()) << " tokens over " << all.calls
            << " call" << (all.calls == 1 ? "" : "s");
        if (all.opaque_calls > 0) out << " (+" << all.opaque_calls << " unmeasured)";
        out << "\n";
    }
    if (paid.calls > 0) {
        out << "  of which metered: " << human(paid.total()) << " tokens over "
            << paid.calls << " call" << (paid.calls == 1 ? "" : "s");
        if (paid.opaque_calls > 0) out << " (+" << paid.opaque_calls << " unmeasured)";
        out << "\n";
    } else if (paid.opaque_calls > 0) {
        out << "  of which metered: " << paid.opaque_calls << " call"
            << (paid.opaque_calls == 1 ? "" : "s") << ", none reporting tokens\n";
    }
    return out.str();
}

void reset_usage() {
    const std::lock_guard<std::mutex> lock(usage_mutex());
    usage_table().clear();
}

}  // namespace auspex
