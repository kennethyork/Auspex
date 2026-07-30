#include "auspex/tts.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace auspex {

namespace {

void set_error(std::string* out, std::string msg) {
    if (out) *out = std::move(msg);
}

constexpr int kExecFailed = 127;

std::string describe_exit(int status) {
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        // 127 is ambiguous: bash uses it for "command not found", and the child
        // uses it when exec'ing bash itself fails. Command-not-found in the
        // pipeline is overwhelmingly the likelier cause, so lead with that.
        if (code == kExecFailed) {
            return "a command in the tts pipeline was not found (exit 127) — "
                   "check the engine and player named in tts_command";
        }
        return "tts pipeline exited with code " + std::to_string(code);
    }
    if (WIFSIGNALED(status)) {
        return "tts pipeline killed by signal " + std::to_string(WTERMSIG(status));
    }
    return "tts pipeline failed";
}

// Write the whole buffer, tolerating short writes and EINTR. Returns false on a
// real error, including EPIPE when the pipeline dies early.
bool write_all(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

bool Tts::speak(const std::string& text, std::string* error) const {
    if (!available()) {
        set_error(error, "no tts_command configured");
        return false;
    }
    if (text.empty()) return true;

    int fds[2];
    if (::pipe(fds) != 0) {
        set_error(error, std::string("pipe() failed: ") + std::strerror(errno));
        return false;
    }

    // popen() is deliberately not used here. It runs /bin/sh, which on Mint is
    // dash, and a POSIX shell reports only the LAST stage of a pipeline as its
    // exit status. With "piper ... | pw-play ...", piper could crash while
    // pw-play happily played zero bytes and exited 0 -- so a completely broken
    // voice reported success. bash with -o pipefail propagates the failure.
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        set_error(error, std::string("fork() failed: ") + std::strerror(saved));
        return false;
    }

    if (pid == 0) {
        // Child: the pipeline reads the utterance on stdin.
        ::close(fds[1]);
        if (::dup2(fds[0], STDIN_FILENO) < 0) ::_exit(kExecFailed);
        ::close(fds[0]);
        ::execlp("bash", "bash", "-o", "pipefail", "-c", command_.c_str(),
                 static_cast<char*>(nullptr));
        ::_exit(kExecFailed);
    }

    ::close(fds[0]);

    // A dead pipeline makes writes raise SIGPIPE, which would kill the caller.
    // Suppress it and detect the failure through the exit status instead.
    struct sigaction ignore{};
    struct sigaction previous{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &previous);

    // piper reads one utterance per line, so the newline is required or it waits
    // on stdin until close and adds latency.
    bool wrote = write_all(fds[1], text.data(), text.size());
    if (wrote) wrote = write_all(fds[1], "\n", 1);

    ::close(fds[1]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            sigaction(SIGPIPE, &previous, nullptr);
            set_error(error, std::string("waitpid() failed: ") + std::strerror(errno));
            return false;
        }
    }

    sigaction(SIGPIPE, &previous, nullptr);

    // Exit status is checked before the write result: when the pipeline dies on
    // startup the write fails as a consequence, and its exit code is the more
    // useful diagnostic.
    if (status != 0) {
        set_error(error, describe_exit(status));
        return false;
    }
    if (!wrote) {
        set_error(error, "tts pipeline closed its input early");
        return false;
    }
    return true;
}

}  // namespace auspex
