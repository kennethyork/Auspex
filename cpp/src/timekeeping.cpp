#include "auspex/timekeeping.hpp"

#include <algorithm>
#include <cctype>

#include "auspex/calendar.hpp"
#include "auspex/process.hpp"

namespace auspex {

namespace {

// The text after "Label:" on the line carrying it, trimmed.
std::string field_after(const std::string& output, const std::string& label) {
    for (const auto& line : split_lines(output)) {
        const auto at = line.find(label);
        if (at == std::string::npos) continue;
        return trim(line.substr(at + label.size()));
    }
    return {};
}

bool all_digits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace

TimeSettings parse_timedatectl(const std::string& output) {
    TimeSettings settings;
    if (output.empty()) return settings;

    settings.local_time = field_after(output, "Local time:");

    // "America/New_York (EDT, -0400)" -- the zone is the first token, the rest is
    // the abbreviation and offset, which change twice a year and are not an id.
    const std::string zone_line = field_after(output, "Time zone:");
    if (const auto space = zone_line.find(' '); space != std::string::npos) {
        settings.timezone = zone_line.substr(0, space);
    } else {
        settings.timezone = zone_line;
    }

    settings.ntp_active   = field_after(output, "NTP service:") == "active";
    settings.synchronized = field_after(output, "System clock synchronized:") == "yes";

    settings.known = !settings.local_time.empty() || !settings.timezone.empty();
    return settings;
}

TimeSettings current_time_settings() {
    const auto result = run({"timedatectl"});
    if (!result.ok) return {};
    return parse_timedatectl(result.out);
}

std::vector<std::string> list_timezones() {
    const auto result = run({"timedatectl", "list-timezones"});
    if (!result.ok) return {};

    std::vector<std::string> zones;
    for (const auto& line : split_lines(result.out)) {
        const std::string zone = trim(line);
        if (!zone.empty()) zones.push_back(zone);
    }
    return zones;
}

bool is_known_timezone(const std::string& zone, const std::vector<std::string>& known) {
    if (zone.empty()) return false;
    return std::find(known.begin(), known.end(), zone) != known.end();
}

bool is_valid_datetime(const std::string& text) {
    // Exactly "YYYY-MM-DD HH:MM:SS". Nothing longer, nothing shorter, no leniency:
    // this becomes an argument to a command run as root.
    if (text.size() != 19) return false;
    if (text[4] != '-' || text[7] != '-' || text[10] != ' ' ||
        text[13] != ':' || text[16] != ':') {
        return false;
    }

    const std::string year   = text.substr(0, 4);
    const std::string month  = text.substr(5, 2);
    const std::string day    = text.substr(8, 2);
    const std::string hour   = text.substr(11, 2);
    const std::string minute = text.substr(14, 2);
    const std::string second = text.substr(17, 2);

    for (const auto* part : {&year, &month, &day, &hour, &minute, &second}) {
        if (!all_digits(*part)) return false;
    }

    const int y = std::stoi(year);
    const int m = std::stoi(month);
    const int d = std::stoi(day);
    const int h = std::stoi(hour);
    const int min = std::stoi(minute);
    const int s = std::stoi(second);

    // Shape is not enough. "2026-02-30 25:00:00" parses as digits in the right
    // places and is not a time; refusing it here means the user is not asked for a
    // password to run a command that was always going to fail.
    if (y < 1970 || y > 2999) return false;
    if (m < 1 || m > 12) return false;
    if (d < 1 || d > days_in_month(y, m)) return false;
    if (h > 23 || min > 59) return false;
    // 60 is a leap second, which timedatectl will not take either.
    if (s > 59) return false;

    return true;
}

bool set_automatic_time(bool automatic) {
    // pkexec, and an argv vector -- never a shell. The prompt the user sees comes
    // from polkit, which is the only thing that should be deciding whether this is
    // allowed.
    return run({"pkexec", "timedatectl", "set-ntp", automatic ? "true" : "false"},
               false)
        .ok;
}

bool set_system_timezone(const std::string& zone) {
    // Validated against the system's own list rather than by pattern. A zone is a
    // path fragment that ends up in a root command; "is it one of the 598 the
    // system named" is the only check that cannot be argued with.
    if (!is_known_timezone(zone, list_timezones())) return false;
    return run({"pkexec", "timedatectl", "set-timezone", zone}, false).ok;
}

bool set_system_time(const std::string& text) {
    if (!is_valid_datetime(text)) return false;
    return run({"pkexec", "timedatectl", "set-time", text}, false).ok;
}

}  // namespace auspex
