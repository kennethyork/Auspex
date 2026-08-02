// The Auditor: the last thing between a model's output and your files.
//
// It reads a changeset and returns one of two answers -- land it, or hold it for a
// person. Everything else in the crew is about producing work; this is the only
// part whose job is to refuse it.
//
// FAILING CLOSED IS THE WHOLE DESIGN. A model that cannot be reached, a reply that
// does not parse, a verdict that is not one of the two words, a diff too large to
// have been read properly: every one of those is a HOLD. "I could not tell" and
// "it is fine" are the same answer if you treat them the same way, and only one of
// them is true. There is no code path here that produces Accept by accident --
// Verdict starts as Hold and is only ever moved by an explicit, understood "accept".
//
// CHEAP CERTAIN CHECKS COME FIRST. A secret in an added line, an empty changeset,
// a diff that escapes the project: these are decidable without a model, so they
// are decided without one. The model is asked only about the thing it is actually
// better at -- whether the code does what the subtask asked for.
#pragma once

#include <string>
#include <vector>

#include "auspex/config.hpp"
#include "auspex/director.hpp"
#include "auspex/sandbox.hpp"

namespace auspex {

enum class Verdict {
    Hold,     // a person must look at this. The default, and the fallback.
    Accept,   // land it
};

struct Audit {
    // Hold unless something explicitly said otherwise. Deliberately the first
    // enumerator and the default member value, so a forgotten assignment holds
    // rather than lands.
    Verdict     verdict = Verdict::Hold;
    // Why. Always populated for a hold, because this sentence is what a person
    // reads before deciding, and a hold with no reason is a coin flip.
    std::string reason;
    // Anything the Auditor noticed that did not on its own justify a hold. Shown
    // under the reason rather than replacing it.
    std::vector<std::string> notes;
    // The line from the diff the Auditor says is wrong, when it named one.
    //
    // Asked for so the objection has to point at text that really exists. Whether
    // it DOES is checkable: see quote_is_real(), which is how a hold that quotes
    // something the diff never contained gets marked as the invention it is.
    std::string quote;

    // True when the verdict came from a deterministic check rather than a model.
    // Worth surfacing: "a secret was found" is a fact, "this looks wrong" is an
    // opinion, and a person deciding should know which they are reading.
    bool        certain = false;

    bool held() const { return verdict != Verdict::Accept; }
};

struct AuditLimits {
    // The diff is truncated to this before going in the prompt. A diff longer than
    // this is HELD without asking -- not because it is wrong, but because a review
    // of a patch that was not shown in full is not a review.
    std::size_t max_diff_bytes = 40'000;

    // A changeset touching more files than this is held for the same reason: the
    // Director was asked for independent pieces, and one that rewrites half the
    // project is not the piece that was planned.
    int max_files = 20;

    bool operator==(const AuditLimits&) const = default;
};

// --- checks that need no model -----------------------------------------------

// Lines ADDED by this changeset that look like a credential.
//
// Added only: removing a secret is the opposite of a problem, and flagging it
// would make cleaning one up impossible to land.
//
// HEURISTIC, and named as such. It catches the shapes that are unmistakable -- a
// PEM header, an AWS key id, a GitHub token -- plus assignments to obviously named
// variables. It will miss a secret that looks like ordinary text, so it is a
// reason to hold and never a certificate that there is nothing there.
std::vector<std::string> scan_secrets(const std::string& diff);

// The deterministic pass. Returns a hold with `certain` set when something is
// decidably wrong, and Accept when nothing is -- which is NOT a verdict on its
// own, only permission to go on and ask the model.
Audit deterministic_audit(const Changeset& changeset, const AuditLimits& limits);

// --- the model's opinion ------------------------------------------------------

std::string auditor_prompt(const PlannedSubtask& subtask, const Changeset& changeset,
                           const AuditLimits& limits);

// Reads the Auditor's reply.
//
// Anything that is not an explicit, understood accept is a hold, with a reason
// saying which way it failed. A reply of "yes" is not an accept: the word asked
// for is "accept", and accepting near-misses is how a garbled reply lands a patch.
Audit parse_audit(const std::string& reply);

// Whether the Auditor's quoted line actually appears in the diff it reviewed.
//
// A hold whose evidence is not in the patch is an invention, and this is the one
// way to tell that apart from a real objection without reading the code yourself.
// Compared with whitespace collapsed and any leading +/- dropped, so a quote that
// is right about the text and careless about the margin still counts.
bool quote_is_real(const std::string& quote, const std::string& diff);

// --- several opinions ---------------------------------------------------------
//
// One Auditor is one model's opinion, and a small model's opinion has been wrong
// in both directions on this project -- holding correct code twice, and accepting
// a call to a function that did not exist. These are the two ways ollamadev buys
// confidence, ported: ask more than once, and make disagreement visible.

// One voice in a debate.
struct Opinion {
    std::string role;      // "advocate", "skeptic", "judge"
    bool        for_it = false;
    std::string reason;
};

struct Debate {
    Opinion advocate;
    Opinion skeptic;
    Opinion judge;
    // The judge decides. It is given both arguments and rules on them, so a
    // skeptic that invents an objection can be overruled rather than being
    // silently decisive.
    bool landed() const { return judge.for_it; }
};

std::string advocate_prompt(const PlannedSubtask& subtask, const Changeset& changeset,
                            const AuditLimits& limits);
std::string skeptic_prompt(const PlannedSubtask& subtask, const Changeset& changeset,
                           const AuditLimits& limits);
std::string judge_prompt(const PlannedSubtask& subtask, const std::string& advocate,
                         const std::string& skeptic);

// Reads one advocate/skeptic reply: {"argument": "..."} or plain prose.
std::string parse_argument(const std::string& reply);

// Reads the judge: {"verdict":"accept"|"hold","reason":"..."}. Same fail-closed
// rule as parse_audit -- anything unclear holds.
Audit parse_judgement(const std::string& reply);

// One model per voice. Empty falls back to the config's.
//
// Separate because a debate whose advocate and skeptic are the same model is one
// model arguing with itself, which produces agreement rather than scrutiny. Two
// different models do not share a blind spot, and that is the entire value of an
// adversarial review over a second opinion.
struct DebateModels {
    std::string advocate;
    std::string skeptic;
    std::string judge;
};

// advocate -> skeptic -> judge. Three model calls, so three times the cost of one
// Auditor; that is the trade and it is why it is opt-in.
Audit debate_changeset(const Config& config, const PlannedSubtask& subtask,
                       const Changeset& changeset, const AuditLimits& limits = {},
                       const DebateModels& models = {}, Debate* detail = nullptr);

// `voters` independent Auditors, majority rules, TIES HOLD.
//
// A tie holding is the whole point of counting: if the reviewers cannot agree,
// that is precisely the case a person should look at. An even number of voters is
// therefore not a bug.
Audit audit_panel(const Config& config, const PlannedSubtask& subtask,
                  const Changeset& changeset, int voters,
                  const AuditLimits& limits = {}, const std::string& model = {},
                  std::vector<Audit>* votes = nullptr);

// Which way a set of votes goes. Pure, so the tie rule is testable without models.
Audit tally(const std::vector<Audit>& votes);

// The whole pass: deterministic checks, then the model. Blocking.
Audit audit_changeset(const Config& config, const PlannedSubtask& subtask,
                      const Changeset& changeset, const AuditLimits& limits = {},
                      const std::string& model = {});

}  // namespace auspex
