// Starting Auspex when the user logs in.
//
// The XDG autostart directory, not a systemd user unit: autostart entries are read
// by every desktop session (Xfce, GNOME, KDE, i3 with a session manager), whereas a
// user unit only starts Auspex if the session itself is systemd-managed AND has
// imported the graphical environment. Auspex needs DISPLAY and a running window
// manager to dock against, and the autostart directory is the one mechanism that
// guarantees both by the time it runs.
//
// This is deliberately NOT the xsession entry in share/xsessions. That one replaces
// your whole session and needs root to install; this one adds a program to the
// session you already log into, and is entirely inside the user's own home.
#pragma once

#include <filesystem>
#include <string>

namespace auspex {

// $XDG_CONFIG_HOME/autostart/auspex.desktop, else ~/.config/autostart/auspex.desktop.
std::filesystem::path autostart_path();

// The absolute path of the running binary, from /proc/self/exe.
//
// Absolute and resolved, because the login session's PATH is not the interactive
// shell's: a bare "auspex-shell" in an autostart entry finds nothing when the
// binary lives in a build directory, and the failure is invisible -- you simply log
// in one day and the panel is not there.
std::filesystem::path own_executable();

// $XDG_BIN_HOME/auspex-shell, else ~/.local/bin/auspex-shell.
//
// A fixed place for the autostart entry to point at, so what starts at login does
// not depend on where the source tree happens to live. Pointing an autostart entry
// straight at a build directory works right up until the directory is moved or
// renamed, and then it fails the way autostart failures always do: silently, at
// login, with nothing on screen to say why.
std::filesystem::path stable_executable_path();

// Points `link` at `target`, replacing whatever was there.
//
// A SYMLINK, not a copy. A copy is 120MB that goes stale the moment the shell is
// rebuilt -- you would be running last week's panel and have no way to tell. A link
// follows every rebuild for free, and if the tree really is moved it breaks in one
// obvious place that re-ticking the setting repairs.
//
// Returns the link path on success, empty on failure.
std::filesystem::path install_stable_executable(
    const std::filesystem::path& link, const std::filesystem::path& target);

// auspex-session next to `shell`, or empty when it was not built.
//
// The supervisor is what should actually be started at login: if the panel dies,
// something has to bring it back, and nothing else will. It is looked for beside
// the shell rather than in PATH so a build directory supervises its own shell
// rather than whichever one happens to be installed.
std::filesystem::path supervisor_beside(const std::filesystem::path& shell);

// The desktop entry text that would launch `executable`.
//
// `supervise` adds the flag that tells auspex-session to start the shell and
// NOTHING else -- no window manager, no compositor, no wallpaper. Those belong to
// the case where Auspex owns the session, and starting a second window manager
// inside somebody else's is actively destructive.
std::string autostart_entry(const std::filesystem::path& executable,
                            bool supervise = false);

// Whether Auspex is set to start at login.
//
// An entry that exists is not enough: `Hidden=true` is how desktop session editors
// (Xfce's "Session and Startup" among them) turn an autostart entry off, leaving the
// file in place. Reporting that as enabled would make the checkbox disagree with
// what actually happens at login.
bool autostart_enabled(const std::filesystem::path& path);
bool autostart_enabled();

// True if the entry says Hidden=true. Exposed for testing the parse.
bool autostart_entry_is_hidden(const std::string& contents);

// Turns it on or off. Enabling writes the entry; disabling removes it, which is what
// unticking a box means -- and also clears any Hidden=true a session editor left, so
// the two controls cannot end up disagreeing.
//
// False on an I/O failure, so the caller can say the setting did not take rather
// than showing a tick that means nothing.
bool set_autostart(bool enabled, const std::filesystem::path& path,
                   const std::filesystem::path& executable);
bool set_autostart(bool enabled);

}  // namespace auspex
