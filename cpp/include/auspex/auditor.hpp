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

// The whole pass: deterministic checks, then the model. Blocking.
Audit audit_changeset(const Config& config, const PlannedSubtask& subtask,
                      const Changeset& changeset, const AuditLimits& limits = {},
                      const std::string& model = {});

}  // namespace auspex
