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

// --- what the crew is made of -------------------------------------------------
//
// ollamadev-qt's Brain pane draws the crew as a stack of faculties with the
// active one lit. This is the same list for Auspex's engine -- and it is a LIST
// OF WHAT IS TRUE, not a copy of theirs: a faculty this engine does not have says
// so rather than being drawn as though it worked.

enum class FacultyState {
    Always,      // part of every run; nothing to turn on
    Optional,    // exists, and is off unless asked for
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
