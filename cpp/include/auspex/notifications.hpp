// Notifications: the log of what was shown, and the switch that stops them.
//
// Auspex does NOT implement org.freedesktop.Notifications. Owning that name would
// take it away from the daemon already running -- xfce4-notifyd here -- and Auspex
// would then be responsible for drawing every notification popup on the desktop,
// which is a display server's worth of work for a panel button.
//
// It watches instead. The session bus can be monitored, so the log is built from
// the Notify calls other applications make to whichever daemon is installed, and
// the daemon keeps doing its job untouched.
#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace auspex {

struct Notification {
    std::string app;      // the sending application's own name for itself
    std::string summary;  // the headline
    std::string body;     // the rest, often empty
    std::string when;     // "HH:MM", local, for display only

    bool operator==(const Notification&) const = default;
};

// A bounded history of what has been shown.
//
// Bounded because this runs for the life of a session and some applications are
// extremely talkative; an unbounded log is a slow memory leak with a bell on it.
// The oldest entries fall off the end.
class NotificationLog {
public:
    static constexpr std::size_t kCapacity = 50;

    // Ignored when both summary and body are empty: some applications send an
    // empty Notify to withdraw an earlier one, and that is not something to show.
    void add(Notification notification);

    // Newest first, which is the order a log is read in.
    std::vector<Notification> recent() const;

    void clear();

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    // How many have arrived since the log was last looked at. What a badge counts.
    std::size_t unseen() const { return unseen_; }
    void mark_seen() { unseen_ = 0; }

private:
    std::deque<Notification> entries_;   // oldest first internally
    std::size_t              unseen_ = 0;
};

// --- do not disturb ----------------------------------------------------------
//
// There is no standard for this. The freedesktop specification covers sending a
// notification and says nothing about suppressing one, so every daemon invented
// its own switch. Auspex drives the one belonging to whichever daemon is actually
// running rather than pretending to a generic capability it cannot have.
//
// xfce4-notifyd is the case handled here, through xfconf. Its property does not
// exist until something writes it, so "not set" reads as off rather than as
// unsupported.

// Whether a daemon whose do-not-disturb switch we know how to reach is present.
bool dnd_supported();

bool dnd_enabled();
bool set_dnd(bool enabled);

}  // namespace auspex
