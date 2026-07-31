// Session composition and supervision policy.
//
// Stage 1 of making Auspex a desktop environment rather than a guest in someone
// else's. auspex-session is what /usr/share/xsessions/auspex.desktop executes: it
// brings up a window manager, a compositor, a wallpaper and the shell, supervises
// the shell, and tears the session down when the user logs out.
//
// WHY A SUPERVISOR AND NOT A SHELL SCRIPT: as long as Auspex was a panel, a crash
// meant a missing panel. Once Auspex is the session, a crash means the user is
// staring at a bare root window with no way to recover. The old Python
// bin/start.sh knew this and had a restart loop; the C++ rewrite dropped it, and
// it has to come back before Stage 2 makes Auspex the window manager too.
//
// WHY THE WM IS STILL BORROWED: Stage 1 deliberately runs a stock EWMH window
// manager. That makes this the smallest change that turns Auspex into a real
// session -- the panel docks exactly as it already does, and nothing about window
// management changes. Stage 2 replaces the borrowed WM with auspex-wm; only
// pick_window_manager() below and the ordering in main() change when it does.
//
// WHAT IS TESTABLE HERE: everything that is a decision rather than a syscall. The
// restart policy takes the clock as an argument, so the crash-loop behaviour that
// used to be untestable shell is now checked by the selftests. Component choice is
// a candidate list resolved against PATH, so it can be reasoned about without
// installing five desktops.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace auspex {

// How the supervisor reacts to the shell exiting non-zero.
//
// Defaults are the old start.sh's numbers: five crashes inside five minutes is a
// crash loop rather than bad luck, and a three-second gap between restarts is long
// enough that a loop does not saturate a core.
struct RestartPolicy {
    int max_crashes      = 5;    // inside window_seconds
    int window_seconds   = 300;
    int backoff_seconds  = 3;    // between ordinary restarts
    int cooldown_seconds = 300;  // after max_crashes is hit
};

struct RestartPlan {
    bool restart       = true;
    int  delay_seconds = 0;

    // True when the policy has given up on the shell. The session does NOT end:
    // the window manager and the wallpaper stay up, so the user keeps a usable
    // desktop and a terminal with which to look at the log. A black screen is the
    // one outcome worse than a missing panel.
    bool exhausted = false;

    bool operator==(const RestartPlan&) const = default;
};

// Decides what to do after a crash at `now`. `crashes` is the history in unix
// seconds, oldest first; it is updated in place -- the new crash is appended and
// anything outside the window is dropped, so the caller keeps no bookkeeping.
//
// The clock is a parameter rather than a call to time() so this is a pure function
// and the crash-loop behaviour can be tested without waiting five real minutes.
RestartPlan plan_restart(std::vector<std::int64_t>& crashes, std::int64_t now,
                         const RestartPolicy& policy = {});

// Everything the session starts, resolved against PATH. An empty string means
// nothing suitable is installed, which is survivable for every field except the
// window manager.
struct SessionComponents {
    std::string window_manager;
    std::string compositor;
    std::string wallpaper_tool;
    std::string polkit_agent;
    std::string xsettings_daemon;

    bool operator==(const SessionComponents&) const = default;
};

// Candidate lists, most-preferred first. Exposed so the selftests can assert the
// ordering is deliberate rather than accidental.
//
// The window-manager order is EWMH-compliance first: the panel docks by setting
// _NET_WM_WINDOW_TYPE_DOCK and _NET_WM_STRUT_PARTIAL, so a WM that honours struts
// correctly matters more than how pretty it is. xfwm4 leads because it is what the
// panel was developed against and what Mint ships.
const std::vector<std::string>& window_manager_candidates();
const std::vector<std::string>& compositor_candidates();
const std::vector<std::string>& wallpaper_tool_candidates();
const std::vector<std::string>& polkit_agent_candidates();
const std::vector<std::string>& xsettings_daemon_candidates();

// Resolves every candidate list against PATH.
SessionComponents detect_components();

// The argv that sets `image` as the desktop wallpaper using `tool`. Empty when the
// tool is unrecognised or the image path is empty.
//
// Each tool takes a different flag for "scale to fill", and getting it wrong gives
// a tiled or letterboxed background, so the mapping is explicit rather than a
// guess with a fallback.
std::vector<std::string> wallpaper_command(const std::string& tool,
                                           const std::string& image);

}  // namespace auspex
