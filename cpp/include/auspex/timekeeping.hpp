// The system clock: reading it, and changing it.
//
// Everything here shells out to timedatectl, and everything that CHANGES something
// goes through pkexec. Setting the clock or the timezone is a root operation on
// every systemd machine, and the alternatives are worse than asking: a setuid
// helper of our own, or writing /etc/localtime directly and leaving systemd's view
// of the world disagreeing with the filesystem's.
//
// THE THING THAT MAKES THIS NOT A ONE-LINER: with NTP running, timedatectl refuses
// a manual time outright --
//
//     Failed to set time: Automatic time synchronization is enabled
//
// -- and it refuses it on stderr with a zero-ish looking failure that is easy to
// drop. A "set time" button that does not know this appears to work and changes
// nothing. Automatic sync has to be turned off first, deliberately and visibly,
// which is why that is a control here rather than something done behind the user's
// back.
#pragma once

#include <string>
#include <vector>

namespace auspex {

struct TimeSettings {
    std::string local_time;    // "Fri 2026-07-31 18:31:28 EDT"
    std::string timezone;      // "America/New_York"
    bool        ntp_active   = false;   // the NTP service is running
    bool        synchronized = false;   // and has actually synced
    // False when timedatectl could not be run at all, so a caller can hide the
    // controls rather than offer settings that cannot be applied.
    bool        known = false;

    bool operator==(const TimeSettings&) const = default;
};

// Parses `timedatectl` with no arguments. The labels are stable across systemd
// versions; the values after them are not, so only the labels are matched.
TimeSettings parse_timedatectl(const std::string& output);

TimeSettings current_time_settings();

// `timedatectl list-timezones`, one per line. Around 600 entries.
std::vector<std::string> list_timezones();

// Whether `zone` is one of `known`.
//
// This is a validation, not a formality. The zone reaches a command run as ROOT,
// and the only safe way to build that argument is to require it to be one the
// system itself listed. Nothing typed or pasted goes through unchecked -- the same
// rule the voice commands follow for every argument they pass to a subprocess.
bool is_known_timezone(const std::string& zone, const std::vector<std::string>& known);

// Strict "YYYY-MM-DD HH:MM:SS", with a real calendar check on the date.
//
// Same reasoning: this string becomes an argument to a privileged command. It is
// validated by shape AND by meaning, so "2026-02-30 25:00:00" is refused here
// rather than by timedatectl after a password prompt the user did not need to see.
bool is_valid_datetime(const std::string& text);

// Turns automatic (NTP) synchronisation on or off.
bool set_automatic_time(bool automatic);

// Both refuse rather than run when the argument does not validate, so a caller
// cannot skip the checks above by accident.
bool set_system_timezone(const std::string& zone);
bool set_system_time(const std::string& text);

}  // namespace auspex
