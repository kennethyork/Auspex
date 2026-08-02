// Running a crew: plan, fan out, audit, land.
//
// This is the part that was ollamadev's Crew::run(). It owns the whole shape of a
// run -- Director, N coders in parallel sandboxes, an Auditor on each result, and
// the decision about what lands and what waits for a person.
//
// STATE LIVES IN AUSPEX'S OWN DIRECTORY, NOT OLLAMADEV'S. The panel has always read
// ~/.ollamadev/crew/current.json, and it would have been less work to keep writing
// there. That would mean two engines writing one file with no lock between them,
// and the failure is silent: a board with entries from two runs under one set of
// numbers, where "accept 2" applies something you never saw. The JSON SHAPE is
// kept identical so the existing readers work unchanged; only the path moves.
//
// TWO CODERS, ONE FILE. Coders work in separate copies and cannot see each other,
// so nothing stops two of them editing the same file -- the Director is asked to
// avoid it, and asking is not preventing. Applying both would mean the second
// silently overwrites the first, including the parts the Auditor approved. So
// overlap is detected at landing time and the loser is HELD with a reason naming
// who it collided with. That is the same call ollamadev's --dedupe makes, except
// it is not optional here: silently losing an accepted change is not a mode.
#pragma once

#include <atomic>
#include <map>
#include <optional>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "auspex/auditor.hpp"
#include "auspex/coder.hpp"
#include "auspex/config.hpp"
#include "auspex/crew.hpp"
#include "auspex/director.hpp"
#include "auspex/sandbox.hpp"

namespace auspex {

// $XDG_DATA_HOME/auspex/crew, else ~/.local/share/auspex/crew.
std::filesystem::path auspex_crew_dir();
std::filesystem::path auspex_run_state_path();    // .../crew/current.json
std::filesystem::path auspex_board_path();        // .../board/current.json

// Where a held changeset is kept so it can be applied later.
//
// On disk rather than in memory because a decision outlives the process: a run
// finishes, the board sits there, and you accept something after lunch. A pointer
// into a dead process's heap cannot do that.
std::filesystem::path changeset_store(const std::string& run_id, int n);

bool save_changeset(const std::filesystem::path& dir, const Changeset& changeset,
                    std::string* error = nullptr);
Changeset load_changeset(const std::filesystem::path& dir);

// --- overlap ------------------------------------------------------------------

// Which files two changesets both touch. Empty when they are independent.
//
// Pure, and tested, because this is what decides whether an approved change is
// silently thrown away.
std::vector<std::string> overlapping_files(const Changeset& a, const Changeset& b);

// --- running ------------------------------------------------------------------

struct RunOptions {
    std::filesystem::path project;
    std::string           task;

    // How many pieces the Director may plan, and how many coders run at once.
    // Separate numbers: a plan of six run two at a time is a reasonable thing to
    // want on a small machine.
    int max_subtasks = 4;
    int parallel     = 4;

    // Empty uses config.ollama_model for every role.
    std::string model;

    // Per-role overrides, each falling back to `model`.
    //
    // Separate because the roles are not equally hard. Planning and reviewing are
    // judgement; coding against a fixed verb table is mostly transcription. An
    // observed run had a 9b model hold correct Python twice with a confident,
    // self-contradicting reason -- the Auditor is where a bigger model earns its
    // cost, and where a small one quietly makes the whole crew useless by holding
    // everything.
    std::string director_model;
    std::string coder_model;
    std::string auditor_model;

    // The model a given role should use, after the fallbacks.
    std::string model_for(const std::string& role) const;

    // --- the brain options, as ollamadev spells them ---------------------------
    //
    // All opt-in, and a plain run is unchanged by every one of them. Each buys
    // something at a cost in time and tokens, which is why they are switches
    // rather than a mode and why the honest default is none of them.

    // Which agent does the coding. "ollama" (default) is Auspex's own loop; any
    // other id hands the subtask to that CLI, which runs its own loop with its own
    // model in the sandbox.
    //
    // This is the single biggest lever on quality. Auspex's own loop is capped at
    // what Ollama serves; a CLI backend is capped at whatever its owner configured,
    // which is usually a frontier model.
    std::string coder_backend = "ollama";

    // The Director and the Auditor can be agent CLIs too. They need a reply
    // rather than file edits, and `claude -p` gives exactly that -- so the role
    // that most needs a capable model is not stuck on whatever Ollama serves.
    // The Researcher: a read-only pass BEFORE the Director that reports where
    // things live and what the conventions are. Its findings go to the Director
    // and to every coder.
    //
    // On by default, unlike the other options, because it is the one that makes
    // the rest of the run better rather than more thorough -- a Director planning
    // against a filename list is guessing at structure.
    // Pick a model per SUBTASK by how hard it looks, filling in only where a role
    // model was not chosen. Off by default: it changes which model runs work, and
    // that should be asked for.
    bool        route = false;

    bool        research = true;
    std::string researcher_backend = "ollama";
    std::string researcher_model;

    std::string director_backend = "ollama";
    std::string auditor_backend  = "ollama";

    // Everything else, by role key. Kept as maps rather than a field each so a new
    // role is a row in one table instead of an edit in five files.
    std::map<std::string, std::string> role_models;
    std::map<std::string, std::string> role_backends;

    // The backend for a role, after its fallback chain.
    std::string backend_for(const std::string& role) const;

    // One backend per coder, round-robin, so a plan of three can run on three
    // different providers at once. Empty falls back to coder_backend for all of
    // them.
    //
    // This is the difference between "several coders" and several DIFFERENT
    // coders: two models of the same family tend to make the same mistake, and
    // the point of fanning out is that they do not.
    std::vector<std::string> coder_backends;

    // The backend for coder `n` (1-based), after the fallbacks.
    std::string backend_for_coder(int n) const;

    // Advocate / skeptic / judge on every changeset instead of one Auditor.
    // Three model calls per piece rather than one.
    bool debate = false;

    // N Director plans, keep the shape most of them agree on, and N reviewers
    // voting on every changeset. The most expensive option here by a distance:
    // it multiplies BOTH ends of the run. 0 is off.
    int amplify = 0;

    // A read-only vulnerability hunt instead of building anything. No coder runs
    // and nothing is ever written -- the result is a report.
    bool security = false;

    // Remember what this run teaches, for the next one.
    bool learn = false;

    CoderLimits coder;
    AuditLimits audit;

    bool operator==(const RunOptions&) const = default;
};

// Called on the RUNNER'S thread, not the GTK one. A UI handler must marshal.
//
// `changed` fires after every state write, which is the signal to re-read the
// state file rather than to poll it.
struct RunEvents {
    std::function<void()>                   changed;
    std::function<void(const std::string&)> log;
};

struct RunResult {
    std::string run_id;
    int         applied = 0;
    int         held    = 0;
    // Set when the run could not proceed at all -- no plan, no project. A run that
    // planned and then held everything is not an error.
    std::string error;

    bool operator==(const RunResult&) const = default;
};

// The whole run. Blocking; run it on your own thread.
//
// `cancel` is polled between steps. A cancelled run leaves its sandboxes and its
// state file behind rather than tidying up, because the work already done is the
// thing you most want after stopping early.
RunResult run_crew(const Config& config, const RunOptions& options,
                   const RunEvents& events = {},
                   const std::atomic<bool>* cancel = nullptr);

// Where a message to coder `n` of `run_id` is left.
std::filesystem::path steer_mailbox(const std::string& run_id, int n);

// The run that is going, or the most recent one. Empty when none.
//
// Read from the state file rather than held in memory: the window that steers may
// not be the one that started the run, and after a restart it certainly is not.
std::string current_run_id();

// Say something to a running coder. False when there is no run, or no such coder.
bool steer_coder(int n, const std::string& message, std::string* error = nullptr);

// --- resuming -----------------------------------------------------------------

// Finish an interrupted run WITHOUT calling a model.
//
// A cancelled run leaves its sandboxes behind on purpose -- the work already done
// is the thing you most want after stopping early. This walks them, captures what
// each coder wrote, audits it, and lands or holds it exactly as a finished run
// would. No coder is restarted and no plan is remade, so it is cheap and cannot
// produce anything the interrupted run had not already produced.
//
// `run_id` empty resumes the most recent run.
RunResult resume_crew(const Config& config, const std::filesystem::path& project,
                      const std::string& run_id = {}, const RunEvents& events = {});

// Runs with sandboxes still on disk, newest first. What resume could act on.
std::vector<std::string> resumable_runs();

// --- the security scan --------------------------------------------------------
//
// A run mode, not a coder role. Nothing is sandboxed, nothing is written, and no
// changeset is produced -- it reads the project and returns a report. That is the
// whole safety argument: a vulnerability hunt that could also edit files is a
// vulnerability hunt you have to review as carefully as any other change.

struct Finding {
    std::string file;
    std::string severity;   // "high" | "medium" | "low"
    std::string detail;

    bool operator==(const Finding&) const = default;
};

std::string security_prompt(const std::string& file, const std::string& contents);

// Reads one file's findings. Anything unparseable yields none rather than a
// guess -- an invented vulnerability wastes more time than a missed one.
std::vector<Finding> parse_findings(const std::string& reply, const std::string& file);

// Highest severity first, so the report opens with what matters.
void sort_findings(std::vector<Finding>& findings);

// The report, as text.
std::string security_report(const std::vector<Finding>& findings);

// Scans the project. Blocking, and one model call per file.
RunResult scan_security(const Config& config, const RunOptions& options,
                        const RunEvents& events = {},
                        const std::atomic<bool>* cancel = nullptr,
                        std::vector<Finding>* findings = nullptr);

// --- lessons ------------------------------------------------------------------
//
// `learn` writes what a run discovered to a file, and every later run puts it in
// front of its coders. Small and blunt on purpose: a line per lesson, capped, and
// injected verbatim. Anything cleverer would be a memory system, and a memory
// system that is wrong is worse than none -- it teaches the same mistake forever.

std::filesystem::path lessons_path(const std::filesystem::path& project);

std::vector<std::string> read_lessons(const std::filesystem::path& project);

// Appends, keeping the newest `limit`. Duplicates are dropped: a lesson learned
// twice is not two lessons, and a file of repeats crowds out the rest.
bool write_lessons(const std::filesystem::path& project,
                   const std::vector<std::string>& lessons, std::size_t limit = 40);

// What the run taught, from what was held and why. No model call: the Auditor
// already said why it held each piece, and that sentence IS the lesson.
std::vector<std::string> lessons_from(const std::vector<BoardItem>& held);

// The lessons, as a block for a coder prompt. Empty when there are none.
std::string lessons_note(const std::filesystem::path& project);

// --- packs --------------------------------------------------------------------
//
// A saved set of options under a name. ollamadev keeps these as files; here they
// are entries in Auspex's own config, because there are a handful of switches
// rather than a whole team definition.

struct CrewPack {
    std::string name;
    RunOptions  options;   // project and task are ignored; only the switches
};

std::vector<CrewPack> builtin_packs();
std::optional<CrewPack> find_pack(const std::string& name);

// --- the roles you can point at a model ----------------------------------------
//
// Not every faculty takes one. The secret gate is a regex, the overlap guard is a
// set comparison, landing is a file copy -- giving those a model picker would be
// offering a choice that does nothing. These are the ones where a model really is
// called, and each can have its own.
//
// The three debate voices are separate on purpose. A debate where the advocate and
// the skeptic are the same model is one model arguing with itself, which produces
// agreement rather than scrutiny -- the point of an adversarial review is that the
// two sides do not share a blind spot.

struct CrewRole {
    std::string key;
    std::string label;      // for the Brain
    std::string hint;       // what this one does, one line
    std::string fallback;   // whose setting it borrows when unset; empty = the
                            // config's ollama_model
};

const std::vector<CrewRole>& configurable_roles();

// --- what the crew is made of -------------------------------------------------
//
// ollamadev-qt's Brain pane draws the crew as a stack of faculties with the
// active one lit. This is the same list for Auspex's engine -- and it is a LIST
// OF WHAT IS TRUE, not a copy of theirs: a faculty this engine does not have says
// so rather than being drawn as though it worked.

enum class FacultyState {
    Always,      // part of every run; nothing to turn on
    Optional,    // exists, and is off unless asked for
    Guard,       // always on AND cannot be turned off -- a refusal, not a stage
    Missing,     // ollamadev has it, Auspex does not (yet)
};

struct Faculty {
    std::string  key;
    std::string  label;
    std::string  role;    // one line: what it does
    FacultyState state = FacultyState::Always;
};

// In pipeline order, so the list reads as the run does.
const std::vector<Faculty>& crew_faculties();

// Which faculty a run is in right now, from the state file. Empty when idle.
//
// Derived from the subtasks rather than written by the runner: any coder doing
// means "coders", none started means "director", all done means "landing", and
// anything else means the Auditor has them. One source of truth, and it is the
// same file the panel already watches.
std::string active_faculty(const CrewRun& run);

// --- the board ----------------------------------------------------------------

// What the run left for a person, in the same JSON shape parse_board() reads.
std::string encode_board(const std::vector<BoardItem>& items);

std::vector<BoardItem> read_board();
bool                   write_board(const std::vector<BoardItem>& items);

// Apply a held changeset into its project, or throw it away. Both remove it from
// the board; only the first writes anything.
bool accept_held(int n, std::string* error = nullptr);
bool discard_held(int n, std::string* error = nullptr);

}  // namespace auspex
