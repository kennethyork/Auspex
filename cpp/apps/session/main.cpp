// auspex-session — what the login screen runs to start an Auspex desktop.
//
//   auspex-session              start the session (from a display manager)
//   auspex-session --plan       print what it would start, change nothing
//   auspex-session --check      --plan plus a verdict on whether it could start
//
// --plan exists because there is no safe way to test this interactively: starting a
// real session from inside a running one either fails on the WM already owning the
// screen or logs the user out. So the whole decision half -- which components were
// found, what argv each gets, what the restart policy is -- is printable without
// touching anything.
//
// Order matters and is not arbitrary:
//   1. environment, so children see XDG_CURRENT_DESKTOP=Auspex
//   2. window manager, and WAIT for it -- the panel's struts are meaningless
//      until something is there to honour them
//   3. xsettings, so GTK clients pick up the theme before they draw
//   4. compositor and polkit agent, both optional
//   5. wallpaper, which needs the root window to exist but nothing else
//   6. the shell, supervised for the life of the session
//
// The session ends when the shell exits *zero* -- that is a logout. A non-zero exit
// is a crash and goes through plan_restart().
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "auspex/config.hpp"
#include "auspex/process.hpp"
#include "auspex/session.hpp"

namespace {

// Set by the signal handler, read by the supervise loop. sig_atomic_t because a
// handler may write it at any point between two instructions.
volatile std::sig_atomic_t g_terminate = 0;

void on_terminate(int) { g_terminate = 1; }

// Children the session owns, newest first so teardown is reverse of startup.
std::vector<pid_t> g_children;

// Spawns a child WITHOUT double-forking, so the session keeps a pid it can wait on
// and signal. process.hpp's spawn_detached() deliberately orphans its grandchild
// onto init, which is right for a panel launching an app and wrong here: an
// orphaned window manager cannot be shut down when the session ends.
pid_t spawn_tracked(const std::vector<std::string>& argv, bool quiet = true) {
    if (argv.empty()) return -1;

    const pid_t pid = ::fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // A new process group, so one component crashing cannot signal its siblings.
        ::setpgid(0, 0);

        if (quiet) {
            const int devnull = ::open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                ::dup2(devnull, STDIN_FILENO);
                if (devnull > STDERR_FILENO) ::close(devnull);
            }
        }

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);

        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    g_children.insert(g_children.begin(), pid);
    return pid;
}

void set_env(const char* name, const std::string& value) {
    ::setenv(name, value.c_str(), 1);
}

// The shell binary that belongs to THIS build. Resolved next to auspex-session
// first, so running out of a build tree does not silently supervise an installed
// copy from a different commit.
std::string shell_path() {
    std::error_code ec;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const auto sibling = self.parent_path() / "auspex-shell";
        if (std::filesystem::exists(sibling, ec)) return sibling.string();
    }
    return auspex::in_path("auspex-shell") ? "auspex-shell" : std::string{};
}

bool window_manager_is_running() {
    // _NET_SUPPORTING_WM_CHECK is the EWMH handshake: an compliant WM sets it on the
    // root window once it is ready to manage. Polling this rather than sleeping a
    // fixed two seconds -- as start.sh did -- means a fast WM does not cost 2s and a
    // slow one is still waited for.
    const auto result = auspex::run({"xprop", "-root", "_NET_SUPPORTING_WM_CHECK"});
    return result.ok && result.out.find("window id") != std::string::npos;
}

bool wait_for_window_manager(int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (window_manager_is_running()) return true;
        ::usleep(100 * 1000);
    }
    return false;
}

void sleep_interruptibly(int seconds) {
    for (int i = 0; i < seconds && !g_terminate; ++i) ::sleep(1);
}

void terminate_children() {
    // SIGTERM in reverse start order, so the shell goes before the WM it docks into.
    for (const pid_t pid : g_children) {
        if (pid > 0) ::kill(pid, SIGTERM);
    }

    // Give them a moment, reaping as they go, then insist.
    for (int waited = 0; waited < 3000; waited += 100) {
        bool any_alive = false;
        for (pid_t& pid : g_children) {
            if (pid <= 0) continue;
            const pid_t reaped = ::waitpid(pid, nullptr, WNOHANG);
            if (reaped == pid || (reaped < 0 && errno == ECHILD)) {
                pid = -1;
            } else {
                any_alive = true;
            }
        }
        if (!any_alive) return;
        ::usleep(100 * 1000);
    }

    for (const pid_t pid : g_children) {
        if (pid > 0) ::kill(pid, SIGKILL);
    }
}

void print_plan(const auspex::SessionComponents& components, const auspex::Config& config,
                const std::string& shell, const auspex::RestartPolicy& policy) {
    const auto show = [](const char* label, const std::string& value,
                         const char* absent_note) {
        std::cout << "  " << label << std::string(18 - std::strlen(label), ' ');
        if (value.empty()) {
            std::cout << "(none found)  " << absent_note << "\n";
        } else {
            std::cout << value << "\n";
        }
    };

    std::cout << "auspex-session plan\n\n";
    std::cout << "components resolved from PATH:\n";
    show("window manager", components.window_manager, "FATAL: no session possible");
    show("xsettings", components.xsettings_daemon,
         "GTK apps will use raw defaults (visible)");
    show("compositor", components.compositor, "no shadows or transparency (cosmetic)");
    show("polkit agent", components.polkit_agent, "auth prompts will not appear");
    show("wallpaper tool", components.wallpaper_tool, "root window stays black");

    std::cout << "\nshell:\n";
    show("binary", shell, "FATAL: nothing to supervise");

    std::cout << "\nwallpaper:\n";
    if (config.background.empty()) {
        std::cout << "  no `background` set in config; root window left as-is\n";
    } else {
        const auto argv = auspex::wallpaper_command(components.wallpaper_tool,
                                                    config.background);
        if (argv.empty()) {
            std::cout << "  cannot set " << config.background << " with "
                      << (components.wallpaper_tool.empty() ? "no tool"
                                                            : components.wallpaper_tool)
                      << "\n";
        } else {
            std::cout << " ";
            for (const auto& word : argv) std::cout << " " << word;
            std::cout << "\n";
        }
    }

    std::cout << "\nrestart policy for the shell:\n"
              << "  up to " << policy.max_crashes << " crashes per "
              << policy.window_seconds << "s, " << policy.backoff_seconds
              << "s apart\n"
              << "  then a " << policy.cooldown_seconds << "s cooldown; if it keeps "
              << "crashing, the shell is\n"
              << "  abandoned but the window manager stays up so the desktop "
              << "remains usable\n";

    std::cout << "\nenvironment children will see:\n"
              << "  XDG_CURRENT_DESKTOP  Auspex\n"
              << "  XDG_SESSION_DESKTOP  auspex\n"
              << "  XDG_SESSION_TYPE     x11\n";
}

// Runs the shell until it exits cleanly, restarting it when it does not.
//
// `session` says whether Auspex owns the desktop. It changes only what happens when
// the policy gives up: as a session there is a window manager and a wallpaper still
// up, so staying alive keeps a usable desktop; as a guest there is nothing to hold
// open and exiting says so plainly rather than leaving a silent process behind.
int supervise_shell(const std::string& shell, const auspex::RestartPolicy& policy,
                    bool session) {
    std::vector<std::int64_t> crashes;

    while (!g_terminate) {
        const pid_t shell_pid = spawn_tracked({shell}, false);
        if (shell_pid < 0) {
            std::cerr << "auspex-session: could not start " << shell << "\n";
            return 1;
        }

        int status = 0;
        while (::waitpid(shell_pid, &status, 0) < 0) {
            if (errno == EINTR) {
                if (g_terminate) break;
                continue;
            }
            break;
        }
        // The shell is gone; stop tracking it so teardown does not signal its pid.
        std::erase(g_children, shell_pid);

        if (g_terminate) break;

        // Zero is a logout, not a crash -- quitting Auspex from its own menu must
        // not be undone by the thing that is supposed to be protecting it.
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) break;

        const auto plan = auspex::plan_restart(crashes, ::time(nullptr), policy);
        if (plan.exhausted) {
            if (session) {
                std::cerr << "auspex-session: the shell keeps crashing; leaving the "
                             "rest of the session running so the desktop stays "
                             "usable\n";
                // Do NOT tear down. The user has a WM, a wallpaper and their
                // windows, and can open a terminal to read the log. A black screen
                // is the one outcome worse than a missing panel.
                while (!g_terminate) ::pause();
            } else {
                std::cerr << "auspex-session: the shell keeps crashing; giving up. "
                             "Your session is otherwise untouched.\n";
            }
            break;
        }

        std::cerr << "auspex-session: shell exited abnormally; restarting in "
                  << plan.delay_seconds << "s\n";
        sleep_interruptibly(plan.delay_seconds);
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const bool plan_only = !args.empty() && (args[0] == "--plan" || args[0] == "--check");
    const bool check     = !args.empty() && args[0] == "--check";

    // Supervise the shell and start NOTHING else.
    //
    // This is the mode for running inside somebody else's session, which is what
    // Auspex does most of the time. Everything below step 1 -- the window manager,
    // the compositor, the wallpaper, XDG_CURRENT_DESKTOP -- exists for the case
    // where Auspex IS the session, and every one of them is actively harmful when
    // it is not: a second window manager fights the one already running, and
    // rewriting XDG_CURRENT_DESKTOP sends portal requests to a daemon this session
    // never started.
    //
    // What is left is the part that was actually wanted: if the panel dies, bring
    // it back, with the same crash-loop policy the full session uses.
    const bool supervise_only =
        !args.empty() && (args[0] == "--supervise" || args[0] == "--shell-only");

    const auspex::Config       config     = auspex::Config::load();
    const auspex::RestartPolicy policy    = {};
    const auto                 components = auspex::detect_components();
    const std::string          shell      = shell_path();

    if (plan_only) {
        print_plan(components, config, shell, policy);
        if (!check) return 0;

        std::cout << "\nverdict: ";
        if (components.window_manager.empty()) {
            std::cout << "CANNOT START -- no window manager in PATH\n";
            return 1;
        }
        if (shell.empty()) {
            std::cout << "CANNOT START -- auspex-shell not found\n";
            return 1;
        }
        if (!auspex::in_path("xprop")) {
            std::cout << "CANNOT START -- xprop missing, cannot detect the WM\n";
            return 1;
        }
        std::cout << "ready\n";
        return 0;
    }

    if (supervise_only) {
        if (shell.empty()) {
            std::cerr << "auspex-session: auspex-shell not found\n";
            return 1;
        }

        std::signal(SIGTERM, on_terminate);
        std::signal(SIGINT, on_terminate);
        std::signal(SIGCHLD, SIG_DFL);
        std::signal(SIGPIPE, SIG_IGN);

        const int code = supervise_shell(shell, policy, /*session=*/false);
        terminate_children();
        return code;
    }

    if (components.window_manager.empty() || shell.empty()) {
        std::cerr << "auspex-session: nothing to start; run --check for details\n";
        return 1;
    }

    // 1. Environment. Auspex IS the desktop now, so these are set rather than
    //    appended to -- XDG_CURRENT_DESKTOP drives which .desktop files apply and
    //    which portal backend is chosen, and inheriting "XFCE" here would send
    //    portal requests to a daemon this session never started.
    set_env("XDG_CURRENT_DESKTOP", "Auspex");
    set_env("XDG_SESSION_DESKTOP", "auspex");
    set_env("XDG_SESSION_TYPE", "x11");

    std::signal(SIGTERM, on_terminate);
    std::signal(SIGINT, on_terminate);
    // Children are reaped explicitly; SIG_IGN here would make waitpid() fail.
    std::signal(SIGCHLD, SIG_DFL);
    std::signal(SIGPIPE, SIG_IGN);

    // 2. The window manager, and wait for it. Everything after this point assumes
    //    something is managing windows; the panel's struts in particular are inert
    //    until a WM reads them.
    if (spawn_tracked({components.window_manager}) < 0) {
        std::cerr << "auspex-session: could not start " << components.window_manager
                  << "\n";
        return 1;
    }
    if (!wait_for_window_manager(10000)) {
        std::cerr << "auspex-session: " << components.window_manager
                  << " did not claim the screen within 10s\n";
        terminate_children();
        return 1;
    }

    // 3. XSETTINGS before anything draws, so GTK clients get the theme first time
    //    rather than restyling mid-session.
    if (!components.xsettings_daemon.empty()) {
        spawn_tracked({components.xsettings_daemon});
    }

    // 4. Optional cosmetics and authentication.
    if (!components.compositor.empty()) spawn_tracked({components.compositor});
    if (!components.polkit_agent.empty()) spawn_tracked({components.polkit_agent});

    // 5. Wallpaper. Fire-and-forget: these tools set the root pixmap and exit, so
    //    they are not session children to be tracked and torn down.
    if (const auto wallpaper =
            auspex::wallpaper_command(components.wallpaper_tool, config.background);
        !wallpaper.empty()) {
        auspex::spawn_detached(wallpaper);
    }

    // 6. The shell, supervised.
    const int exit_code = supervise_shell(shell, policy, /*session=*/true);

    terminate_children();
    return exit_code;
}
