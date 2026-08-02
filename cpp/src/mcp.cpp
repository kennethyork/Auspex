#include "auspex/mcp.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "auspex/process.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

std::string string_field(const json& object, const char* key) {
    if (!object.contains(key)) return {};
    const auto& value = object[key];
    return value.is_string() ? value.get<std::string>() : std::string{};
}

}  // namespace

// ---------------------------------------------------------------------------
std::string McpTool::qualified() const {
    return server.empty() ? name : server + "." + name;
}

std::filesystem::path mcp_config_path() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "auspex" / "mcp.json";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "auspex" / "mcp.json";
    }
    return {};
}

std::vector<McpServerConfig> parse_mcp_config(const std::string& json_text) {
    std::vector<McpServerConfig> servers;

    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return servers;

    // "mcpServers" is what every other client calls it, so a config can be copied
    // straight across. "servers" accepted as well, because it is the obvious guess.
    const json* block = nullptr;
    for (const char* key : {"mcpServers", "servers"}) {
        if (document.contains(key) && document[key].is_object()) {
            block = &document[key];
            break;
        }
    }
    if (!block) return servers;

    for (const auto& [name, entry] : block->items()) {
        if (!entry.is_object()) continue;

        McpServerConfig server;
        server.name    = name;
        server.command = string_field(entry, "command");
        // A server with no command cannot be started, and listing it would offer
        // tools that can never be called.
        if (server.name.empty() || server.command.empty()) continue;

        if (entry.contains("args") && entry["args"].is_array()) {
            for (const auto& argument : entry["args"]) {
                if (argument.is_string()) server.args.push_back(argument.get<std::string>());
            }
        }
        if (entry.contains("env") && entry["env"].is_object()) {
            for (const auto& [key, value] : entry["env"].items()) {
                if (value.is_string()) server.env[key] = value.get<std::string>();
            }
        }
        servers.push_back(std::move(server));
    }

    std::sort(servers.begin(), servers.end(),
              [](const McpServerConfig& a, const McpServerConfig& b) {
                  return a.name < b.name;
              });
    return servers;
}

std::vector<McpServerConfig> load_mcp_servers() {
    std::ifstream in(mcp_config_path());
    if (!in) return {};
    std::ostringstream contents;
    contents << in.rdbuf();
    return parse_mcp_config(contents.str());
}

// ---------------------------------------------------------------------------
std::string encode_frame(const std::string& payload) {
    return "Content-Length: " + std::to_string(payload.size()) + "\r\n\r\n" + payload;
}

std::optional<std::string> decode_frame(std::string& buffer) {
    // Both framings in the wild: the spec says \r\n\r\n, some servers emit \n\n.
    // Whichever ends the header FIRST is the one in use -- a \r\n\r\n contains a
    // \n\n, so searching for the shorter one alone would cut the header in half.
    const auto crlf = buffer.find("\r\n\r\n");
    const auto lf   = buffer.find("\n\n");

    std::size_t header_end = std::string::npos;
    std::size_t skip       = 0;
    if (crlf != std::string::npos && (lf == std::string::npos || crlf <= lf)) {
        header_end = crlf;
        skip       = 4;
    } else if (lf != std::string::npos) {
        header_end = lf;
        skip       = 2;
    }
    if (header_end == std::string::npos) return std::nullopt;   // not here yet

    // Content-Length, case-insensitively: the header name is not guaranteed to
    // arrive in the spelling the spec uses.
    std::size_t length = 0;
    bool        found  = false;
    {
        std::istringstream header(buffer.substr(0, header_end));
        std::string line;
        while (std::getline(header, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;

            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (key != "content-length") continue;

            const std::string value = trim(line.substr(colon + 1));
            if (value.empty()) continue;
            try {
                length = static_cast<std::size_t>(std::stoull(value));
                found  = true;
            } catch (...) {
                return std::nullopt;
            }
            break;
        }
    }
    if (!found) {
        // A header block with no length is unusable and will never become usable.
        // Dropped, so a malformed server does not wedge the stream forever.
        buffer.erase(0, header_end + skip);
        return std::nullopt;
    }

    const std::size_t body_start = header_end + skip;
    if (buffer.size() < body_start + length) return std::nullopt;   // body still coming

    std::string payload = buffer.substr(body_start, length);
    buffer.erase(0, body_start + length);
    return payload;
}

// ---------------------------------------------------------------------------
struct McpClient::Impl {
    pid_t pid       = -1;
    int   to_child  = -1;   // we write
    int   from_child = -1;  // we read
    std::string buffer;
    int   next_id   = 1;
    bool  ready     = false;

    ~Impl() { stop(); }

    void stop() {
        if (to_child >= 0)   { ::close(to_child);   to_child = -1; }
        if (from_child >= 0) { ::close(from_child); from_child = -1; }
        if (pid > 0) {
            // TERM, then reap. Closing its stdin is the polite signal and most
            // servers exit on it; the kill is for the ones that do not.
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    bool write_frame(const std::string& payload) {
        if (to_child < 0) return false;
        const std::string framed = encode_frame(payload);
        std::size_t sent = 0;
        while (sent < framed.size()) {
            const ssize_t n = ::write(to_child, framed.data() + sent, framed.size() - sent);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    // Reads until a frame arrives or the deadline passes.
    std::optional<std::string> read_frame(std::chrono::seconds timeout) {
        if (auto ready_frame = decode_frame(buffer)) return ready_frame;
        if (from_child < 0) return std::nullopt;

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::array<char, 4096> chunk{};

        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;

            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            pollfd waiting{from_child, POLLIN, 0};
            const int state = ::poll(&waiting, 1, static_cast<int>(left.count()));
            if (state < 0) {
                if (errno == EINTR) continue;
                return std::nullopt;
            }
            if (state == 0) return std::nullopt;

            const ssize_t n = ::read(from_child, chunk.data(), chunk.size());
            if (n > 0) {
                buffer.append(chunk.data(), static_cast<std::size_t>(n));
                if (auto frame = decode_frame(buffer)) return frame;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            return std::nullopt;   // closed
        }
    }

    // One request/response. Replies that are not ours are skipped rather than
    // returned: a server may send notifications and progress at any time, and
    // taking the first frame that arrives would match a request to the wrong reply.
    std::optional<json> request(const std::string& method, const json& params,
                                std::chrono::seconds timeout) {
        const int id = next_id++;

        json message;
        message["jsonrpc"] = "2.0";
        message["id"]      = id;
        message["method"]  = method;
        if (!params.is_null()) message["params"] = params;

        if (!write_frame(message.dump())) return std::nullopt;

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;

            const auto left =
                std::chrono::duration_cast<std::chrono::seconds>(deadline - now);
            const auto frame = read_frame(left.count() > 0 ? left
                                                           : std::chrono::seconds(1));
            if (!frame) return std::nullopt;

            const auto reply = json::parse(*frame, nullptr, /*allow_exceptions=*/false);
            if (reply.is_discarded() || !reply.is_object()) continue;
            if (!reply.contains("id") || !reply["id"].is_number_integer()) continue;
            if (reply["id"].get<int>() != id) continue;
            return reply;
        }
    }

    bool notify(const std::string& method) {
        json message;
        message["jsonrpc"] = "2.0";
        message["method"]  = method;
        return write_frame(message.dump());
    }
};

McpClient::McpClient(McpServerConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {}

McpClient::~McpClient() = default;

bool McpClient::start(std::string* error) {
    const auto fail = [error](const std::string& what) {
        if (error) *error = what;
        return false;
    };

    if (config_.command.empty()) return fail("no command");
    if (impl_->ready) return true;

    int in_pipe[2]  = {-1, -1};   // parent writes -> child stdin
    int out_pipe[2] = {-1, -1};   // child stdout -> parent reads
    if (::pipe(in_pipe) != 0) return fail("could not create a pipe");
    if (::pipe(out_pipe) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return fail("could not create a pipe");
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) ::close(fd);
        return fail("could not start the server");
    }

    if (pid == 0) {
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        // stderr to /dev/null. Servers log freely there, and mixing it into the
        // protocol stream is the classic way to corrupt a JSON-RPC pipe.
        if (const int devnull = ::open("/dev/null", O_WRONLY); devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) ::close(fd);

        for (const auto& [key, value] : config_.env) {
            ::setenv(key.c_str(), value.c_str(), /*overwrite=*/1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(config_.command.c_str()));
        for (const auto& a : config_.args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        // execvp with an argv vector. The command and its arguments are separate
        // fields in the config for exactly this reason -- there is no shell string
        // to be glued together and re-split.
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    impl_->pid        = pid;
    impl_->to_child   = in_pipe[1];
    impl_->from_child = out_pipe[0];

    // The handshake. A server that does not answer this is not one we can use, and
    // finding that out now beats finding it out mid-run.
    json params;
    params["protocolVersion"] = "2024-11-05";
    params["capabilities"]    = json::object();
    params["clientInfo"]      = json{{"name", "auspex"}, {"version", "0.2.0"}};

    const auto reply = impl_->request("initialize", params, std::chrono::seconds(20));
    if (!reply) {
        impl_->stop();
        return fail("'" + config_.command + "' did not complete the MCP handshake");
    }

    impl_->notify("notifications/initialized");
    impl_->ready = true;
    return true;
}

std::vector<McpTool> McpClient::tools() {
    std::vector<McpTool> tools;
    if (!impl_->ready) return tools;

    const auto reply =
        impl_->request("tools/list", json::object(), std::chrono::seconds(20));
    if (!reply || !reply->contains("result")) return tools;

    const auto& result = (*reply)["result"];
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) {
        return tools;
    }

    for (const auto& entry : result["tools"]) {
        if (!entry.is_object()) continue;

        McpTool tool;
        tool.server      = config_.name;
        tool.name        = string_field(entry, "name");
        tool.description = string_field(entry, "description");
        if (entry.contains("inputSchema")) tool.schema = entry["inputSchema"].dump();
        if (tool.name.empty()) continue;   // unnameable is uncallable
        tools.push_back(std::move(tool));
    }
    return tools;
}

std::string McpClient::call(const std::string& tool, const std::string& arguments,
                            bool* ok, std::chrono::seconds timeout) {
    if (ok) *ok = false;
    if (!impl_->ready) return "the server is not running";

    auto parsed = json::parse(arguments.empty() ? "{}" : arguments, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) parsed = json::object();

    json params;
    params["name"]      = tool;
    params["arguments"] = parsed;

    const auto reply = impl_->request("tools/call", params, timeout);
    if (!reply) return "the server did not answer";

    if (reply->contains("error") && (*reply)["error"].is_object()) {
        return "the server refused: " + string_field((*reply)["error"], "message");
    }
    if (!reply->contains("result") || !(*reply)["result"].is_object()) {
        return "the server gave no result";
    }

    const auto& result = (*reply)["result"];

    // The content blocks, flattened to their text. Anything that is not text --
    // an image, a resource -- is named rather than dropped, so a coder is not left
    // wondering why a call that succeeded returned nothing.
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& block : result["content"]) {
            if (!block.is_object()) continue;
            const std::string type = string_field(block, "type");
            if (type == "text") {
                text += string_field(block, "text");
                text += "\n";
            } else if (!type.empty()) {
                text += "(" + type + " content, not shown)\n";
            }
        }
    }

    // isError is the server saying the TOOL failed, which is different from the
    // call failing. Reported as text, and not as ok.
    const bool tool_failed = result.contains("isError") &&
                             result["isError"].is_boolean() &&
                             result["isError"].get<bool>();
    if (ok) *ok = !tool_failed;

    if (text.empty()) text = tool_failed ? "the tool reported an error"
                                         : "(no output)";
    return text;
}

// ---------------------------------------------------------------------------
std::vector<McpTool> discover_mcp_tools(std::vector<std::string>* problems) {
    std::vector<McpTool> all;

    for (const auto& server : load_mcp_servers()) {
        McpClient client(server);
        std::string error;
        if (!client.start(&error)) {
            // Skipped, not fatal. One broken entry in a config should not take
            // away every other server's tools.
            if (problems) problems->push_back(server.name + ": " + error);
            continue;
        }
        for (auto& tool : client.tools()) all.push_back(std::move(tool));
    }
    return all;
}

}  // namespace auspex
