#include "auspex/notifications.hpp"

#include "auspex/process.hpp"

namespace auspex {

namespace {

constexpr const char* kXfceChannel = "xfce4-notifyd";
constexpr const char* kXfceDnd     = "/do-not-disturb";

}  // namespace

void NotificationLog::add(Notification notification) {
    // An empty notification is how some applications withdraw an earlier one.
    // There is nothing to show and a row saying nothing is worse than no row.
    if (notification.summary.empty() && notification.body.empty()) return;

    entries_.push_back(std::move(notification));
    while (entries_.size() > kCapacity) entries_.pop_front();

    // Counted against the capacity, not without bound: a badge reading 900 is the
    // same information as one reading 50 and neither is worth the arithmetic.
    if (unseen_ < kCapacity) ++unseen_;
}

std::vector<Notification> NotificationLog::recent() const {
    return std::vector<Notification>(entries_.rbegin(), entries_.rend());
}

void NotificationLog::clear() {
    entries_.clear();
    unseen_ = 0;
}

bool dnd_supported() {
    // The channel is asked for, not the property: xfce4-notifyd creates the
    // property only once something has written it, so its absence means "never
    // switched on", not "cannot be switched on".
    if (!in_path("xfconf-query")) return false;
    return run({"xfconf-query", "-c", kXfceChannel, "-l"}).ok;
}

bool dnd_enabled() {
    const auto result = run({"xfconf-query", "-c", kXfceChannel, "-p", kXfceDnd});
    // Unset reads as an error, which is off.
    if (!result.ok) return false;
    return result.out.find("true") != std::string::npos;
}

bool set_dnd(bool enabled) {
    // -n -t bool creates the property the first time. Without it the very first
    // press fails with "does not exist" and the switch appears broken until
    // something else has set it once.
    return run({"xfconf-query", "-c", kXfceChannel, "-p", kXfceDnd, "-n", "-t", "bool",
                "-s", enabled ? "true" : "false"},
               false)
        .ok;
}

}  // namespace auspex
