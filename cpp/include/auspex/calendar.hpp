// A calendar: days, and the events on them.
//
// Modelled on what a month view actually needs rather than on the iCalendar data
// model. Events have a time, a title and one of four ways of repeating. There are
// no attendees, no invitations and no sync, because each of those turns this from a
// desktop feature into a second application with a protocol behind it.
//
// Stored under XDG_DATA_HOME, not in config.json. Events are user data: they grow
// without bound, they are worth backing up separately, and losing the shell's
// configuration should never mean losing them.
#pragma once

#include <filesystem>
#include <map>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace auspex {

// Days in a month, honouring leap years. Public because more than one feature needs
// it, and a second copy of the leap rule is a second place to get it wrong.
int days_in_month(int year, int month);

// "YYYY-MM-DD", checked as a real calendar date rather than by shape alone: an
// event filed under 2026-02-30 is an event no view will ever show again.
bool is_valid_date(std::string_view text);

// "HH:MM" on a 24-hour clock. Empty is also valid and means an all-day event, which
// is a different thing from an event at midnight.
bool is_valid_time(std::string_view text);

// Builds "YYYY-MM-DD" with zero padding. Empty if the date is not real.
//
// Formatted with snprintf rather than a stream: streams format integers through the
// global locale, and one with digit grouping writes 2026 as "2,026" -- an
// 11-character key that fails validation everywhere it is then used.
std::string format_date(int year, int month, int day);

// Day of the week for a date. 0 is Sunday, matching how a month grid is laid out.
// -1 if the date is not real.
int weekday_of(std::string_view date);

// A human month name, for a view's heading.
std::string month_name(int month);

// One cell of a month view.
struct MonthCell {
    std::string date;             // always a real date
    bool        in_month = true;  // false for the days either side that fill the grid

    bool operator==(const MonthCell&) const = default;
};

// The 6x7 grid a month view draws: the month itself, padded at both ends with the
// days from the months either side, so every week is a full row.
//
// Six rows always, not "as many as needed". A grid that changes height as you page
// through the year makes everything below it jump, and a month can genuinely need
// six rows -- so six is the only size that is both sufficient and stable.
std::vector<MonthCell> month_grid(int year, int month);

// How an event repeats.
//
// Four rules and no more. Weekly-on-several-days, "every third Tuesday" and the
// rest of RFC 5545 are where a calendar stops being a feature and becomes a parser;
// these four are what a personal calendar is actually filled with.
enum class Repeat { None, Daily, Weekly, Monthly, Yearly };

std::string repeat_to_string(Repeat repeat);
Repeat      repeat_from_string(std::string_view text);

// A label for a menu, in the order they should appear in one.
std::vector<std::pair<Repeat, std::string>> repeat_choices();

struct CalendarEvent {
    // "HH:MM", or empty for an all-day event. Sorting puts all-day first, which is
    // where a day view shows them.
    std::string start;
    std::string title;

    Repeat      repeat = Repeat::None;
    // "YYYY-MM-DD" past which the event stops repeating. Empty means it does not
    // stop. Ignored when repeat is None.
    std::string until;

    bool operator==(const CalendarEvent&) const = default;
};

// Whether an event first held on `first` and repeating by `rule` also falls on
// `date`.
//
// A repeating event is stored ONCE, on its first date, and asked about. It is never
// expanded into copies: expanding makes "stop this happening" impossible without
// finding and deleting every copy, and fills the file with entries nobody typed.
//
// The two rules worth stating, because both are places calendars get this wrong:
//
//   * Monthly on the 31st SKIPS months with no 31st, rather than clamping to the
//     30th. Clamping silently moves an event to a day the user did not choose.
//   * Yearly on the 29th of February happens in leap years only, for the same
//     reason. Both fall out of comparing the day number rather than counting.
bool repeats_on(const std::string& first, Repeat rule, const std::string& until,
                const std::string& date);

// One appearance of an event on a particular day.
//
// Carries where it is STORED as well as what it is, because a repeating event shown
// on the 14th lives on the 1st -- and removing it has to reach the entry that
// actually exists rather than the day you happen to be looking at.
struct Occurrence {
    CalendarEvent event;
    std::string   origin;      // the date the event is stored under
    std::size_t   index = 0;   // its position within that day
    bool          repeating = false;

    bool operator==(const Occurrence&) const = default;
};

class EventStore {
public:
    // $XDG_DATA_HOME/auspex/calendar.json, else ~/.local/share/auspex/calendar.json.
    static std::filesystem::path default_path();

    // A missing or corrupt file yields an empty calendar rather than throwing.
    // Events are worth keeping but not worth refusing to start a desktop over.
    static EventStore load(const std::filesystem::path& path = default_path());

    // Written through a temporary and renamed, so an interrupted save cannot leave
    // a half-written file where the calendar used to be.
    bool save(const std::filesystem::path& path = default_path()) const;

    // Everything happening on a day: the events stored on it, plus every repeating
    // event from an earlier date that falls on it. All-day first, then by time.
    std::vector<Occurrence> on(const std::string& date) const;

    // Refuses an unreal date, an invalid time, and a blank title -- each would
    // create an entry no view can show or point at to remove.
    bool add(const std::string& date, CalendarEvent event);

    // Removes the STORED entry -- pass an Occurrence's origin and index, not the
    // day it was seen on. Removing a repeating event removes the whole series,
    // which is the only thing that can be meant by deleting an event that was
    // never copied.
    bool remove(const std::string& date, std::size_t index);

    // How many events each day of a month has, keyed by day-of-month. This is what
    // lets a month view fill its cells without a lookup per cell.
    std::map<int, int> counts_in_month(int year, int month) const;

    // Days with at least one event, earliest first.
    std::vector<std::string> dates() const;

    bool empty() const { return events_.empty(); }

private:
    // Sorted, so the file is in date order too -- plain JSON in the user's own data
    // directory is something people open, and hash order would be hostile.
    std::map<std::string, std::vector<CalendarEvent>> events_;
};

}  // namespace auspex
