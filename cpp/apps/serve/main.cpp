// auspex-serve — the localhost web interface.
//
// Binds 127.0.0.1 only; see auspex/webui.hpp for why that is deliberately not
// configurable. Needs no display, so this is the complete GTK-free front end:
// chat, voice commands, dictation and spoken replies, all from a browser.
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

#include "auspex/config.hpp"
#include "auspex/webui.hpp"

namespace {

auspex::WebUi* g_server = nullptr;

void on_signal(int) {
    // Only async-signal-safe work: stop() just flips httplib's internal flag.
    if (g_server) g_server->stop();
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    int port = 8765;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--port" || args[i] == "-p") && i + 1 < args.size()) {
            port = std::atoi(args[++i].c_str());
        } else if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "usage: auspex-serve [--port N]   (default 8765)\n"
                         "\n"
                         "Serves http://127.0.0.1:N and nothing else -- not reachable\n"
                         "from the network, so there is no login and no TLS.\n"
                         "\n"
                         "endpoints:\n"
                         "  GET  /            the page\n"
                         "  GET  /status      model and ASR backend\n"
                         "  POST /chat        {\"text\":...} -> {\"reply\":...}\n"
                         "  POST /command     {\"text\":...} -> whitelisted action\n"
                         "  POST /speak       {\"text\":...} -> speaks it\n"
                         "  POST /transcribe  raw int16 mono 16kHz -> {\"text\":...}\n";
            return 0;
        }
    }

    if (port <= 0 || port > 65535) {
        std::cerr << "auspex-serve: port out of range\n";
        return 2;
    }

    auspex::WebUi server(auspex::Config::load());
    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "auspex-serve: http://127.0.0.1:" << port << "\n";
    std::cout.flush();

    std::string error;
    if (!server.listen(port, &error)) {
        std::cerr << "auspex-serve: " << error << "\n";
        return 1;
    }
    return 0;
}
