// The Router: how hard is this piece, and what should do it.
//
// The role settings say what KIND of thinking a part of the crew does. This says
// how MUCH of it a particular piece needs. They compose rather than compete: a
// plan containing "rename a variable" and "design a cache layer" should not run
// both coders on the same model, and no per-role setting can express that because
// the difference is in the subtask, not the role.
//
// HEURISTIC, AND DELIBERATELY SO. Classifying costs no model call: it is word
// lists and lengths, ported from ollamadev's Router with its ordering intact.
// Asking a model how hard a task is would spend a call to decide how to spend a
// call, and be wrong in more interesting ways. Being occasionally wrong here is
// cheap -- the worst case is a strong model on an easy piece.
//
// PRECEDENCE, WHICH IS THE PART WORTH GETTING RIGHT: an explicitly chosen model
// always wins. The router only fills in what you did not decide. A person who set
// the Auditor to a particular model did so on purpose, and a router that
// overrode that would be a setting that silently does nothing -- which this
// project has already shipped once and should not again.
#pragma once

#include <string>
#include <vector>

#include "auspex/config.hpp"

namespace auspex {

// The three tiers, in order of difficulty.
const std::vector<std::string>& router_tiers_list();

struct Difficulty {
    std::string tier;     // "simple" | "moderate" | "hard"
    std::string reason;   // why, in a few words -- shown so a decision can be argued with
};

// Which tier a piece of work falls into.
//
// Order matters and is preserved from ollamadev: code and hard words first, then
// length, then the short-lookup test. Checking brevity earlier would let
// "design a cache" through as simple because it is short.
Difficulty classify_difficulty(const std::string& text);

// The tier a ROLE works at when nothing else decides.
//
// The Director reasons over a whole codebase to cut a job up; the Researcher
// reads and summarises. They are not the same difficulty, and neither depends on
// the subtask -- so they are fixed rather than classified.
std::string tier_for_role(const std::string& role);

// The model for a tier, from config (`crew_tier_<name>_model`). Empty when unset.
std::string tier_model(const Config& config, const std::string& tier);

// What should actually run this.
//
// `chosen` is whatever the role settings already decided. Non-empty wins outright:
// the router fills gaps, it does not overrule a person.
std::string route_model(const Config& config, const std::string& chosen,
                        const std::string& tier);

}  // namespace auspex
