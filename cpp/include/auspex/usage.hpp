// What a run cost, in the only unit the server actually reports: tokens.
//
// The crew now routes eight roles across local Ollama and metered cloud models,
// and until this existed nothing told you which one you had just spent an
// afternoon on. A debate is three model calls; a panel of five Auditors is five;
// amplify multiplies the coders. Those are all deliberate trades, and a trade you
// cannot see the price of is a guess.
//
// WHAT THIS CAN AND CANNOT SEE. Ollama reports `prompt_eval_count` and
// `eval_count` on every /api/generate, so every call Auspex's own loop makes is
// counted exactly. A role handed to an agent CLI -- claude, codex -- is a child
// process that reports nothing to us, so it is counted as CALLS ONLY, with no
// token figures at all. That gap is shown as "-" rather than as zero: a zero
// would read as "free", and those are the calls most likely not to be.
//
// DELIBERATELY NO PRICES. Per-token rates change, differ per account, and are not
// discoverable from here. A hardcoded table would be wrong at some point and
// wrong silently, which is worse than absent -- so this reports the tokens, which
// are a fact, and leaves money to the person who knows their own rates. Local and
// metered models are separated in the report, because that split is knowable and
// is the one most decisions actually turn on.
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace auspex {

struct Tally {
    std::int64_t prompt = 0;   // tokens in
    std::int64_t eval   = 0;   // tokens out
    std::int64_t calls  = 0;
    // Calls we could not measure: a role handed to an agent CLI. Kept apart from
    // `calls` so a report can say "and 4 more we cannot see" instead of implying
    // the tokens for those were zero.
    std::int64_t opaque_calls = 0;

    std::int64_t total() const { return prompt + eval; }

    bool operator==(const Tally&) const = default;
};

// True when a model name is served by somebody who bills for it.
//
// Ollama spells its hosted models with a "-cloud" or ":cloud" suffix, and an
// agent CLI is metered by definition. Anything else is assumed local, which is
// the safe way round: mislabelling a metered model as local would understate a
// bill, so the test errs toward calling things metered.
bool is_metered_model(const std::string& model);

// THREAD-SAFE, and it has to be: coders run concurrently, and a run with amplify
// on has several of them calling this at once.
void record_usage(const std::string& model, int prompt_tokens, int eval_tokens);

// A call whose token counts nobody reported -- an agent CLI, or a generate() that
// answered without them.
void record_opaque_usage(const std::string& model);

// Per-model totals as of now.
std::map<std::string, Tally> usage_snapshot();

// What happened BETWEEN two snapshots. This is how a crew run reports its own
// cost without needing every call site to thread a counter through: snapshot at
// the start, snapshot at the end, subtract.
std::map<std::string, Tally> usage_since(const std::map<std::string, Tally>& before);

// Human-readable, sorted by tokens spent, metered models first and marked.
//
// `title` heads the block; empty prints no heading.
std::string usage_report(const std::map<std::string, Tally>& tally,
                         const std::string& title = {});

// Totals across every model, and across the metered ones alone.
Tally usage_total(const std::map<std::string, Tally>& tally);
Tally usage_metered_total(const std::map<std::string, Tally>& tally);

// Drops everything. For tests, and for a "reset the meter" action.
void reset_usage();

}  // namespace auspex
