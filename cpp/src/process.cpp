#include "auspex/process.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace auspex {

ProcessResult run(const std::vector<std::string>& argv, bool capture,
                  const std::string& cwd) {
    ProcessResult result;
    if (argv.empty()) return result;

    int fds[2] = {-1, -1};
    if (capture && ::pipe(fds) != 0) return result;

    const pid_t pid = ::fork();
    if (pid < 0) {
        if (capture) {
            ::close(fds[0]);
            ::close(fds[1]);
        }
        return result;
    }

    if (pid == 0) {
        // Before anything else, and fatal if it fails: everything below assumes
        // the child is standing in the right tree.
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);

        const int devnull = ::open("/dev/null", O_WRONLY);

        if (capture) {
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            ::close(fds[1]);
        } else if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
        }

        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);

        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    if (capture) {
        ::close(fds[1]);
        std::array<char, 4096> buffer{};
        for (;;) {
            const ssize_t n = ::read(fds[0], buffer.data(), buffer.size());
            if (n > 0) {
                result.out.append(buffer.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        ::close(fds[0]);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return result;
    }

    result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
}

bool spawn_detached(const std::vector<std::string>& argv, const std::string& cwd) {
    if (argv.empty()) return false;

    const pid_t first = ::fork();
    if (first < 0) return false;

    if (first == 0) {
        // Intermediate child: fork again and exit, so the grandchild is orphaned
        // onto init and never needs reaping by the panel.
        const pid_t second = ::fork();
        if (second < 0) ::_exit(127);
        if (second > 0) ::_exit(0);

        ::setsid();

        // Same rule as run(): the wrong directory is worse than no run at all.
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);

        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);

        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    // Reap the intermediate child immediately; it exits at once.
    int status = 0;
    while (::waitpid(first, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return true;
}

std::string resolve_in_path(std::string_view program) {
    if (program.empty()) return {};
    if (program.find('/') != std::string_view::npos) return {};
    if (program.find_first_of(" \t\n;|&$`<>()") != std::string_view::npos) return {};

    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};

    std::istringstream parts(path_env);
    std::string dir;
    while (std::getline(parts, dir, ':')) {
        if (dir.empty()) continue;
        std::error_code ec;
        const std::filesystem::path candidate = std::filesystem::path(dir) / program;
        if (std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return candidate.string();
        }
    }
    return {};
}

bool in_path(std::string_view program) { return !resolve_in_path(program).empty(); }

std::string first_in_path(const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        // Only the program name is probed, so an entry may carry arguments.
        const std::size_t space = candidate.find(' ');
        const std::string program = space == std::string::npos ? candidate
                                                              : candidate.substr(0, space);
        if (in_path(program)) return candidate;
    }
    return {};
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

}  // namespace auspex
