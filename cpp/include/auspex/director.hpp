// The Director: one task in, a plan of subtasks out.
//
// This is the first piece of the crew engine that Auspex owns rather than drives.
// See crew.hpp for the division that used to hold and no longer does; the cost of
// owning it is that this file and ollamadev's Crew.cpp are now two implementations
// of one idea, free to disagree.
//
// WHAT IS PURE AND WHAT IS NOT. Building the prompt and reading the reply are both
// text-in/values-out and are tested exhaustively. Only plan() talks to a model, and
// it is a thin wrapper over the two. That split is deliberate: a Director that
// mis-parses a reply silently produces a plan with the wrong number of coders, and
// that is a bug you want caught by a test rather than by a run that edits files.
//
// THE MODEL NEVER NAMES A FILE OR A COMMAND HERE. A subtask is a role from a fixed
// list plus a sentence of English. What the coder is then allowed to do with that
// sentence is the coder's problem, not the Director's -- and it is bounded by the
// sandbox, not by this prompt.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "auspex/config.hpp"

namespace auspex {

// The personas a subtask may be assigned.
//
// A FIXED LIST, checked on the way out of the model. An unrecognised role becomes
// "coder" rather than being passed through: role names reach prompts and, later,
// model-routing decisions, and "whatever the model said" is not a category.
const std::vector<std::string>& director_roles();
bool is_known_role(const std::string& role);

struct PlannedSubtask {
    int         n = 0;       // 1-based, and the number a person says
    std::string role;
    std::string title;       // one line, what this piece is
    std::string detail;      // what the coder is told; may be empty

    bool operator==(const PlannedSubtask&) const = default;
};

struct Plan {
    std::vector<PlannedSubtask> subtasks;
    // What the Director said about the whole job, if anything. Shown above the
    // lanes so a plan can be judged before any of it runs.
    std::string summary;
    // Set when the model answered but the answer could not be used. Distinct from
    // an empty plan, which is a model saying "there is nothing to do".
    std::string error;

    bool ok() const { return error.empty(); }

    bool operator==(const Plan&) const = default;
};

// The prompt sent to the Director.
//
// `files` is a listing of the project, not its contents: the Director decides how
// to CUT UP a job, and for that it needs to know what is there, not what is in it.
// Sending contents would spend the context window on the wrong thing and still not
// fit.
//
// `max_subtasks` bounds the plan. It is in the prompt AND enforced on the reply,
// because a model asked for at most four will sometimes return six.
std::string director_prompt(const std::string& task,
                            const std::vector<std::string>& files,
                            int max_subtasks);

// Reads the Director's reply.
//
// Expects {"summary": "...", "subtasks": [{"role": "...", "title": "...",
// "detail": "..."}]}. Tolerant of the shapes models actually produce: a bare
// array instead of an object, a "tasks" key instead of "subtasks", and prose
// wrapped around the JSON or fenced in a code block.
//
// Numbering is assigned HERE, not taken from the model. The numbers are what
// accept and discard act on, so they must be dense and start at one however the
// model chose to label things.
Plan parse_plan(const std::string& reply, int max_subtasks);

// Pulls the first balanced JSON object or array out of a reply that may have prose
// or a ``` fence around it. Empty when there is none.
//
// Needed even with Ollama's JSON mode: it constrains decoding for the models that
// honour it, and reasoning models still sometimes emit a fence.
std::string extract_json(const std::string& text);

// Asks the model. Blocking -- run it off the GTK thread.
//
// The model is `config.ollama_model` unless `model` overrides it, which is how
// --route will pick a different one per role later.
Plan plan_task(const Config& config, const std::string& task,
               const std::vector<std::string>& files, int max_subtasks,
               const std::string& model = {});

}  // namespace auspex
