// A mode where every button can be pressed and nothing happens to your machine.
//
// The shell could never be interaction-tested. Synthesising input is off the
// table here for a good reason -- it once typed into a live chat -- so "click
// every control and see what breaks" was simply unavailable, and every claim
// about whether the GUI worked rested on my reading of the code.
//
// It was unavailable for the wrong reason. Pressing a button does not require
// the input system: GTK will activate a widget through its own API, in-process,
// without going near the pointer, the keyboard or any window but the one the
// test built. What actually made it unsafe is what the buttons DO. "Start the
// crew" runs models against a project. "Talk" opens the microphone. "Launch"
// spawns applications. Saving Settings rewrites config.json.
//
// So this guards the SINKS rather than the buttons. There are four ways for this
// program to affect anything outside itself -- spawning a process, an HTTP
// request, recording audio, and writing a file -- and each one refuses while
// smoke mode is on. Guarding the handful of things that reach outside is sound
// in a way that a list of buttons known to be dangerous never could be: a new
// button is safe by default, and the day somebody adds a fifth kind of sink is
// the day this needs revisiting, which is a much rarer day.
//
// A refusal is RECORDED, not silent. The count is the evidence that pressing the
// button actually reached the handler -- a smoke test where every click quietly
// did nothing would pass just as happily against a window whose buttons were not
// connected to anything at all.
#pragma once

#include <string>
#include <vector>

namespace auspex {

// True when AUSPEX_SMOKE is set in the environment.
//
// An environment variable rather than a flag threaded through every constructor:
// the guards are in leaf functions four call layers below the window that owns
// the button, and a parameter would have to be carried through all of them.
bool smoke_mode();

// Refuse `what`, and record that something tried it.
//
// Returns true when smoke mode is on, so a caller reads as:
//     if (smoke_refuse("spawn")) return {};
// and does nothing at all when smoke mode is off.
bool smoke_refuse(const std::string& what);

// Everything refused so far, in the order it was attempted.
std::vector<std::string> smoke_refusals();

// Forget the refusals so far. Called between windows, so each window's report
// is about that window.
void smoke_reset();

}  // namespace auspex
