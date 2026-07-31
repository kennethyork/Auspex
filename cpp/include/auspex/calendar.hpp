// A calendar: days, and the events on them.
//
// Modelled on what a month view actually needs rather than on the iCalendar data
// model. There is no recurrence, no attendees, no invitations and no sync, because
// each of those turns this from a desktop feature into a second application with a
// protocol behind it. What is here is the part people use every day: a month you
// can read at a glance, and events with a time and a title on it.
//
// Stored under XDG_DATA_HOME, not in config.json. Events are user data: they grow
// without bound, they are worth backing up separately, and losing the shell's
// configuration should never mean losing them.
#pragma once

#include <filesystem>
#include <map>
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

struct CalendarEvent {
    // "HH:MM", or empty for an all-day event. Sorting puts all-day first, which is
    // where a day view shows them.
    std::string start;
    std::string title;

    bool operator==(const CalendarEvent&) const = default;
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

    // Events on a day, all-day first and then by start time.
    std::vector<CalendarEvent> on(const std::string& date) const;

    // Refuses an unreal date, an invalid time, and a blank title -- each would
    // create an entry no view can show or point at to remove.
    bool add(const std::string& date, CalendarEvent event);

    // By index within the day, as returned by on().
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
