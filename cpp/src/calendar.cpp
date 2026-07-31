#include "auspex/calendar.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"

namespace auspex {

namespace {

using json = nlohmann::json;

bool all_digits(std::string_view text) {
    if (text.empty()) return false;
    for (const unsigned char c : text) {
        if (std::isdigit(c) == 0) return false;
    }
    return true;
}

std::filesystem::path data_home() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share";
    }
    return std::filesystem::path(".local") / "share";
}

}  // namespace

int days_in_month(int year, int month) {
    static constexpr std::array<int, 12> lengths{31, 28, 31, 30, 31, 30,
                                                 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return lengths[static_cast<std::size_t>(month - 1)];
}

bool is_valid_date(std::string_view text) {
    if (text.size() != 10) return false;
    if (text[4] != '-' || text[7] != '-') return false;

    const std::string year(text.substr(0, 4));
    const std::string month(text.substr(5, 2));
    const std::string day(text.substr(8, 2));
    if (!all_digits(year) || !all_digits(month) || !all_digits(day)) return false;

    const int y = std::stoi(year);
    const int m = std::stoi(month);
    const int d = std::stoi(day);

    if (y < 1 || y > 9999) return false;
    if (m < 1 || m > 12) return false;
    return d >= 1 && d <= days_in_month(y, m);
}

bool is_valid_time(std::string_view text) {
    // Empty is an all-day event, which is a real thing and not the same as midnight.
    if (text.empty()) return true;
    if (text.size() != 5 || text[2] != ':') return false;

    const std::string hour(text.substr(0, 2));
    const std::string minute(text.substr(3, 2));
    if (!all_digits(hour) || !all_digits(minute)) return false;

    return std::stoi(hour) <= 23 && std::stoi(minute) <= 59;
}

std::string format_date(int year, int month, int day) {
    if (year < 1 || year > 9999) return {};
    if (month < 1 || month > 12) return {};
    if (day < 1 || day > days_in_month(year, month)) return {};

    // snprintf, NOT an ostringstream.
    //
    // A stream formats integers through the global locale, and a locale with digit
    // grouping turns 2026 into "2,026". That produces an 11-character key which
    // fails is_valid_date, so every note was silently refused -- in the shell only,
    // because the test binary runs under the C locale and saw nothing wrong. printf
    // applies grouping only for the explicit %'d flag, so this is locale-proof.
    char buffer[16];
    const int written = std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                                      year, month, day);
    if (written != 10) return {};
    return std::string(buffer, 10);
}

int weekday_of(std::string_view date) {
    if (!is_valid_date(date)) return -1;

    const int y = std::stoi(std::string(date.substr(0, 4)));
    const int m = std::stoi(std::string(date.substr(5, 2)));
    const int d = std::stoi(std::string(date.substr(8, 2)));

    // std::chrono rather than a hand-written Zeller: the calendar rules are the
    // standard library's problem, and a date arithmetic bug here would be invisible
    // until some particular March.
    const std::chrono::year_month_day ymd{std::chrono::year{y},
                                          std::chrono::month{static_cast<unsigned>(m)},
                                          std::chrono::day{static_cast<unsigned>(d)}};
    const std::chrono::weekday weekday{std::chrono::sys_days{ymd}};
    return static_cast<int>(weekday.c_encoding());   // 0 = Sunday
}

std::string month_name(int month) {
    static constexpr std::array<const char*, 12> names{
        "January", "February", "March",     "April",   "May",      "June",
        "July",    "August",   "September", "October", "November", "December"};
    if (month < 1 || month > 12) return {};
    return names[static_cast<std::size_t>(month - 1)];
}

std::vector<MonthCell> month_grid(int year, int month) {
    std::vector<MonthCell> cells;
    if (month < 1 || month > 12) return cells;

    const std::string first = format_date(year, month, 1);
    if (first.empty()) return cells;

    // How many days of the previous month are needed to reach the start of the week
    // the 1st falls in.
    const int lead = weekday_of(first);
    if (lead < 0) return cells;

    int previous_year  = year;
    int previous_month = month - 1;
    if (previous_month < 1) {
        previous_month = 12;
        --previous_year;
    }
    const int previous_length = days_in_month(previous_year, previous_month);

    for (int i = lead; i > 0; --i) {
        cells.push_back({.date = format_date(previous_year, previous_month,
                                             previous_length - i + 1),
                         .in_month = false});
    }

    const int length = days_in_month(year, month);
    for (int day = 1; day <= length; ++day) {
        cells.push_back({.date = format_date(year, month, day), .in_month = true});
    }

    int next_year  = year;
    int next_month = month + 1;
    if (next_month > 12) {
        next_month = 1;
        ++next_year;
    }

    // Padded to a full six rows. See the header: a grid that changes height as you
    // page through the year makes everything below it jump.
    int day = 1;
    while (cells.size() < 42) {
        cells.push_back({.date = format_date(next_year, next_month, day++),
                         .in_month = false});
    }

    return cells;
}

std::string repeat_to_string(Repeat repeat) {
    switch (repeat) {
        case Repeat::Daily:   return "daily";
        case Repeat::Weekly:  return "weekly";
        case Repeat::Monthly: return "monthly";
        case Repeat::Yearly:  return "yearly";
        case Repeat::None:    break;
    }
    return "none";
}

Repeat repeat_from_string(std::string_view text) {
    if (text == "daily")   return Repeat::Daily;
    if (text == "weekly")  return Repeat::Weekly;
    if (text == "monthly") return Repeat::Monthly;
    if (text == "yearly")  return Repeat::Yearly;
    // Anything unrecognised is a one-off. A rule we cannot honour must not silently
    // become a different rule.
    return Repeat::None;
}

std::vector<std::pair<Repeat, std::string>> repeat_choices() {
    return {{Repeat::None, "Does not repeat"},
            {Repeat::Daily, "Every day"},
            {Repeat::Weekly, "Every week"},
            {Repeat::Monthly, "Every month"},
            {Repeat::Yearly, "Every year"}};
}

bool repeats_on(const std::string& first, Repeat rule, const std::string& until,
                const std::string& date) {
    if (rule == Repeat::None) return false;
    if (!is_valid_date(first) || !is_valid_date(date)) return false;

    // Never before it started, and never after it was told to stop. Both compare as
    // strings, which is exact for zero-padded ISO dates.
    if (date < first) return false;
    if (!until.empty() && is_valid_date(until) && date > until) return false;

    switch (rule) {
        case Repeat::Daily:
            return true;

        case Repeat::Weekly:
            return weekday_of(date) == weekday_of(first);

        case Repeat::Monthly:
            // Comparing the day NUMBER is what makes the 31st skip February rather
            // than land on the 28th. A calendar that clamps moves an event to a day
            // its owner never chose, and does it silently.
            return date.compare(8, 2, first, 8, 2) == 0;

        case Repeat::Yearly:
            // Month and day both. The 29th of February falls out correctly: no
            // common year has one to match.
            return date.compare(5, 5, first, 5, 5) == 0;

        case Repeat::None:
            break;
    }
    return false;
}

std::filesystem::path EventStore::default_path() {
    return data_home() / "auspex" / "calendar.json";
}

namespace {

// All-day first, then by start time. Stable so two events at the same time keep the
// order they were entered in rather than shuffling on every load.
void sort_day(std::vector<CalendarEvent>& day) {
    std::stable_sort(day.begin(), day.end(),
                     [](const CalendarEvent& a, const CalendarEvent& b) {
                         if (a.start.empty() != b.start.empty()) return a.start.empty();
                         return a.start < b.start;
                     });
}

}  // namespace

EventStore EventStore::load(const std::filesystem::path& path) {
    EventStore store;

    std::ifstream in(path);
    if (!in) return store;

    const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return store;

    for (const auto& [date, value] : document.items()) {
        // Anything that is not a real date or not a list is skipped rather than
        // trusted: this file is meant to be editable by hand.
        if (!is_valid_date(date) || !value.is_array()) continue;

        std::vector<CalendarEvent> day;
        for (const auto& entry : value) {
            CalendarEvent event;

            if (entry.is_string()) {
                // The first version of this file stored plain strings, one note per
                // day. Read as all-day events so nothing written then is lost.
                event.title = trim(entry.get<std::string>());
            } else if (entry.is_object()) {
                if (entry.contains("title") && entry["title"].is_string()) {
                    event.title = trim(entry["title"].get<std::string>());
                }
                if (entry.contains("start") && entry["start"].is_string()) {
                    event.start = trim(entry["start"].get<std::string>());
                }
                if (entry.contains("repeat") && entry["repeat"].is_string()) {
                    event.repeat = repeat_from_string(entry["repeat"].get<std::string>());
                }
                if (entry.contains("until") && entry["until"].is_string()) {
                    event.until = trim(entry["until"].get<std::string>());
                }
            } else {
                continue;
            }

            if (event.title.empty()) continue;
            if (!is_valid_time(event.start)) event.start.clear();
            if (event.repeat == Repeat::None) event.until.clear();
            if (!event.until.empty() && !is_valid_date(event.until)) event.until.clear();
            day.push_back(std::move(event));
        }

        if (!day.empty()) {
            sort_day(day);
            store.events_[date] = std::move(day);
        }
    }

    return store;
}

bool EventStore::save(const std::filesystem::path& path) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    json document = json::object();
    for (const auto& [date, day] : events_) {
        json entries = json::array();
        for (const auto& event : day) {
            json record{{"start", event.start}, {"title", event.title}};
            // Only written when they say something. A file full of "repeat":"none"
            // is noise in something meant to be read and edited by hand.
            if (event.repeat != Repeat::None) {
                record["repeat"] = repeat_to_string(event.repeat);
                if (!event.until.empty()) record["until"] = event.until;
            }
            entries.push_back(std::move(record));
        }
        document[date] = std::move(entries);
    }

    // Temporary then rename, the same as the settings file: an interrupted write
    // must not be able to leave a truncated file where the calendar used to be.
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        out << document.dump(2) << "\n";
        if (!out.good()) return false;
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::vector<Occurrence> EventStore::on(const std::string& date) const {
    std::vector<Occurrence> day;
    if (!is_valid_date(date)) return day;

    for (const auto& [origin, stored] : events_) {
        // Nothing stored after this day can reach back to it.
        if (origin > date) break;

        for (std::size_t i = 0; i < stored.size(); ++i) {
            const CalendarEvent& event = stored[i];

            const bool here = origin == date;
            const bool repeated =
                !here && repeats_on(origin, event.repeat, event.until, date);
            if (!here && !repeated) continue;

            day.push_back({.event = event,
                           .origin = origin,
                           .index = i,
                           .repeating = event.repeat != Repeat::None});
        }
    }

    // Merged from several days, so the order has to be imposed here rather than
    // inherited from any one of them.
    std::stable_sort(day.begin(), day.end(),
                     [](const Occurrence& a, const Occurrence& b) {
                         if (a.event.start.empty() != b.event.start.empty()) {
                             return a.event.start.empty();
                         }
                         return a.event.start < b.event.start;
                     });
    return day;
}

bool EventStore::add(const std::string& date, CalendarEvent event) {
    if (!is_valid_date(date)) return false;

    event.title = trim(std::move(event.title));
    event.start = trim(std::move(event.start));
    // A blank title is a row that shows as empty and can only be removed by
    // guessing which one it is.
    if (event.title.empty()) return false;
    if (!is_valid_time(event.start)) return false;

    event.until = trim(std::move(event.until));
    // A one-off has no end to its repetition; keeping one would be a field that
    // means nothing and shows in the file.
    if (event.repeat == Repeat::None) event.until.clear();
    // An "until" that is not a date is dropped rather than refusing the event: the
    // event is the thing being asked for, and an unbounded repeat is the honest
    // reading of a bound nobody can parse.
    if (!event.until.empty() && !is_valid_date(event.until)) event.until.clear();
    // An end before the beginning would mean an event that never happens at all.
    if (!event.until.empty() && event.until < date) return false;

    events_[date].push_back(std::move(event));
    sort_day(events_[date]);
    return true;
}

bool EventStore::remove(const std::string& date, std::size_t index) {
    const auto found = events_.find(date);
    if (found == events_.end()) return false;
    if (index >= found->second.size()) return false;

    found->second.erase(found->second.begin() + static_cast<std::ptrdiff_t>(index));
    // A day with nothing left is not a day with an empty list: leaving it would
    // keep a mark on a date with nothing on it.
    if (found->second.empty()) events_.erase(found);
    return true;
}

std::map<int, int> EventStore::counts_in_month(int year, int month) const {
    std::map<int, int> counts;
    if (month < 1 || month > 12) return counts;

    // Asked day by day rather than by scanning the month's own keys, because a
    // repeating event stored in a previous year still happens in this month and a
    // key scan would never see it.
    const int length = days_in_month(year, month);
    for (int day = 1; day <= length; ++day) {
        const std::string date = format_date(year, month, day);
        if (date.empty()) continue;
        const auto n = on(date).size();
        if (n > 0) counts[day] = static_cast<int>(n);
    }
    return counts;
}

std::vector<std::string> EventStore::dates() const {
    std::vector<std::string> all;
    all.reserve(events_.size());
    for (const auto& [date, day] : events_) all.push_back(date);
    return all;
}

}  // namespace auspex
