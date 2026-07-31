#include "auspex/session.hpp"

#include <algorithm>

#include "auspex/process.hpp"

namespace auspex {

RestartPlan plan_restart(std::vector<std::int64_t>& crashes, std::int64_t now,
                         const RestartPolicy& policy) {
    crashes.push_back(now);

    // Drop everything older than the window. start.sh instead reset the counter to
    // zero whenever the *last* crash was old, which meant four crashes spread over
    // an hour still counted as four the moment a fifth arrived quickly. Trimming by
    // age is what "five crashes in five minutes" actually means.
    const std::int64_t cutoff = now - policy.window_seconds;
    crashes.erase(std::remove_if(crashes.begin(), crashes.end(),
                                 [cutoff](std::int64_t at) { return at < cutoff; }),
                  crashes.end());

    RestartPlan plan;
    if (static_cast<int>(crashes.size()) >= policy.max_crashes) {
        // A crash loop. Wait out the cooldown once, then start counting again from
        // nothing -- if it is still broken after that, the next five crashes hit
        // this branch again and the plan comes back exhausted.
        if (crashes.size() > static_cast<std::size_t>(policy.max_crashes)) {
            plan.restart   = false;
            plan.exhausted = true;
            return plan;
        }
        plan.delay_seconds = policy.cooldown_seconds;
        return plan;
    }

    plan.delay_seconds = policy.backoff_seconds;
    return plan;
}

const std::vector<std::string>& window_manager_candidates() {
    // EWMH strut handling first, then availability. xfwm4 is what the panel was
    // developed against; marco is what upstream MAGI used; the rest are ordered by
    // how completely they implement _NET_WM_STRUT_PARTIAL. i3 is last because it
    // tiles, which fights the canvas rather than cooperating with it.
    static const std::vector<std::string> candidates = {
        "xfwm4", "marco", "openbox", "muffin", "mutter", "kwin_x11", "i3",
    };
    return candidates;
}

const std::vector<std::string>& compositor_candidates() {
    // picom is maintained; xcompmgr is what start.sh used and is still the fallback
    // on older installs. Both are optional -- without one, windows simply have no
    // shadows or transparency, which is cosmetic.
    static const std::vector<std::string> candidates = {"picom", "compton", "xcompmgr"};
    return candidates;
}

const std::vector<std::string>& wallpaper_tool_candidates() {
    // xwallpaper and hsetroot exist only to set a background and exit. feh is
    // listed because start.sh used it and it is very widely installed, despite
    // being an image viewer that happens to do this.
    static const std::vector<std::string> candidates = {"xwallpaper", "hsetroot", "feh"};
    return candidates;
}

const std::vector<std::string>& polkit_agent_candidates() {
    // Without one of these, anything asking for authentication (mounting a disk,
    // installing an update) fails silently with no prompt.
    static const std::vector<std::string> candidates = {
        "polkit-gnome-authentication-agent-1",
        "polkit-mate-authentication-agent-1",
        "lxpolkit",
        "polkit-kde-authentication-agent-1",
    };
    return candidates;
}

const std::vector<std::string>& xsettings_daemon_candidates() {
    // Supplies the GTK theme, cursor theme, font hinting and DPI to every toolkit
    // client over XSETTINGS. Without it GTK apps fall back to raw defaults, which
    // is the single most visible "this is not a real desktop" tell.
    static const std::vector<std::string> candidates = {
        "xsettingsd", "xfsettingsd", "mate-settings-daemon", "gsd-xsettings",
    };
    return candidates;
}

SessionComponents detect_components() {
    SessionComponents components;
    components.window_manager   = first_in_path(window_manager_candidates());
    components.compositor       = first_in_path(compositor_candidates());
    components.wallpaper_tool   = first_in_path(wallpaper_tool_candidates());
    components.polkit_agent     = first_in_path(polkit_agent_candidates());
    components.xsettings_daemon = first_in_path(xsettings_daemon_candidates());
    return components;
}

std::vector<std::string> wallpaper_command(const std::string& tool,
                                           const std::string& image) {
    if (tool.empty() || image.empty()) return {};

    // Each tool spells "scale to fill" differently. A wrong flag is not an error,
    // it just looks wrong -- tiled or letterboxed -- so the mapping is exhaustive
    // and an unknown tool returns nothing rather than being guessed at.
    if (tool == "xwallpaper") return {tool, "--zoom", image};
    if (tool == "hsetroot")   return {tool, "-fill", image};
    if (tool == "feh")        return {tool, "--bg-fill", image};
    return {};
}

}  // namespace auspex
