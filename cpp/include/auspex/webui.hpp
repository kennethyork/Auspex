// Localhost web interface.
//
// Replaces local_API.py's endpoints (/chat, /speak, /transcribe) and gives them a
// page to drive from, which is what web_access.py did -- minus everything that made
// web_access.py risky.
//
// SECURITY POSTURE, deliberately narrow:
//   * Binds 127.0.0.1 only. Never 0.0.0.0. Nothing on the LAN can reach it, so
//     there is no login form, no session cookie, no credential file and no
//     self-signed TLS certificate to get wrong. web_access.py shipped all four.
//   * Any process on this machine running as any user can reach a loopback port, so
//     this is not an isolation boundary -- it is the same trust level as the
//     desktop session itself. That is why there is no way to enable a non-loopback
//     bind: it would silently turn a local convenience into a network service.
//   * Commands go through the same whitelist as the voice path. The web UI cannot
//     do anything the panel cannot.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "auspex/asr.hpp"
#include "auspex/config.hpp"
#include "auspex/tts.hpp"

namespace auspex {

class WebUi {
public:
    explicit WebUi(Config config);
    ~WebUi();

    WebUi(const WebUi&)            = delete;
    WebUi& operator=(const WebUi&) = delete;

    // Starts the server on 127.0.0.1:port. Returns false if the port is taken.
    // Blocking; call from its own thread or use start_background().
    bool listen(int port, std::string* error = nullptr);

    bool start_background(int port, std::string* error = nullptr);
    void stop();

    bool running() const { return running_.load(); }
    int  port() const { return port_; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    Config            config_;
    int               port_ = 0;
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

}  // namespace auspex
