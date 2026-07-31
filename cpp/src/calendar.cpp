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
            } else {
                continue;
            }

            if (event.title.empty()) continue;
            if (!is_valid_time(event.start)) event.start.clear();
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
            entries.push_back({{"start", event.start}, {"title", event.title}});
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

std::vector<CalendarEvent> EventStore::on(const std::string& date) const {
    const auto found = events_.find(date);
    if (found == events_.end()) return {};
    return found->second;
}

bool EventStore::add(const std::string& date, CalendarEvent event) {
    if (!is_valid_date(date)) return false;

    event.title = trim(std::move(event.title));
    event.start = trim(std::move(event.start));
    // A blank title is a row that shows as empty and can only be removed by
    // guessing which one it is.
    if (event.title.empty()) return false;
    if (!is_valid_time(event.start)) return false;

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

    // The prefix bounds the scan to one month rather than walking every event.
    const std::string first = format_date(year, month, 1);
    if (first.empty()) return counts;
    const std::string prefix = first.substr(0, 8);

    for (auto it = events_.lower_bound(prefix); it != events_.end(); ++it) {
        if (it->first.compare(0, 8, prefix) != 0) break;
        try {
            counts[std::stoi(it->first.substr(8, 2))] =
                static_cast<int>(it->second.size());
        } catch (...) {
            continue;
        }
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
