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

// The desktop entry text that would launch `executable`.
std::string autostart_entry(const std::filesystem::path& executable);

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
