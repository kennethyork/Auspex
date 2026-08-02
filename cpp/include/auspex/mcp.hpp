// MCP: tools that live outside Auspex.
//
// An MCP server is a program you configure once -- a database client, a ticket
// tracker, a search API -- that speaks JSON-RPC over its stdin and stdout and
// offers a list of tools. This connects to them and hands what they offer to the
// crew's coders.
//
// WHO CHOOSES WHAT. The same posture as every other verb here, and worth being
// precise about because "arbitrary external tools" sounds like the opposite:
//
//   * YOU choose the servers. They come from a config file you write. Nothing in a
//     model's output can add one, and there is no discovery, no marketplace and no
//     auto-install.
//   * THE SERVER chooses the tools. Auspex asks it for a list and offers exactly
//     that list.
//   * THE MODEL chooses from that list, by name. A name that is not on it is
//     refused before anything is sent.
//
// So the trust boundary is where you put it when you edited the config, and the
// model's freedom is the same as with `read` -- pick one of these, with arguments.
//
// WHAT THIS DOES NOT DO. It does not sandbox the server. An MCP server is a
// program running as you, with your files and your network; that is what it is
// for, and no wrapper here changes it. Configuring one is exactly as much trust as
// installing it.
//
// THE FRAMING IS PURE AND TESTED. Everything on the wire goes through
// encode_frame/decode_frame, which are text-in/values-out -- a framing bug
// deadlocks a pipe rather than throwing, and that is not a thing to debug live.
#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace auspex {

struct McpServerConfig {
    std::string              name;      // what you call it
    std::string              command;   // the program
    std::vector<std::string> args;
    std::map<std::string, std::string> env;   // added to the inherited environment

    bool operator==(const McpServerConfig&) const = default;
};

struct McpTool {
    std::string server;        // which server offers it
    std::string name;          // the server's own tool name
    std::string description;
    std::string schema;        // JSON schema for the arguments, as text

    // How the coder names it: "server.tool". Servers are configured by different
    // people and two of them may well both offer "search".
    std::string qualified() const;

    bool operator==(const McpTool&) const = default;
};

// --- config -------------------------------------------------------------------

// $XDG_CONFIG_HOME/auspex/mcp.json, else ~/.config/auspex/mcp.json.
std::filesystem::path mcp_config_path();

// Parses the config. The shape is the one every other MCP client uses, so a file
// can be copied between them:
//   {"mcpServers": {"name": {"command": "...", "args": [...], "env": {...}}}}
std::vector<McpServerConfig> parse_mcp_config(const std::string& json_text);
std::vector<McpServerConfig> load_mcp_servers();

// --- the wire -----------------------------------------------------------------

// One JSON-RPC message with its Content-Length header.
std::string encode_frame(const std::string& payload);

// Reads one frame out of `buffer`, removing it. nullopt when a whole one has not
// arrived yet -- which is the normal case on a pipe and must not be an error.
//
// Accepts both framings in the wild: the spec's \r\n\r\n and the \n\n some servers
// emit. Getting that wrong hangs forever rather than failing, which is why it is
// tested rather than assumed.
std::optional<std::string> decode_frame(std::string& buffer);

// --- talking to one -----------------------------------------------------------

class McpClient {
public:
    explicit McpClient(McpServerConfig config);
    ~McpClient();

    McpClient(const McpClient&)            = delete;
    McpClient& operator=(const McpClient&) = delete;

    // Starts the server and performs the handshake. False and `error` on failure;
    // a server that will not start is reported, never retried in a loop.
    bool start(std::string* error = nullptr);

    // What it offers. Empty when it offered nothing or could not be asked.
    std::vector<McpTool> tools();

    // Calls one. `arguments` is a JSON object as text. The reply is the tool's
    // text content; `ok` false means the server refused or failed.
    std::string call(const std::string& tool, const std::string& arguments,
                     bool* ok = nullptr,
                     std::chrono::seconds timeout = std::chrono::seconds(60));

    const McpServerConfig& config() const { return config_; }

private:
    struct Impl;
    McpServerConfig       config_;
    std::unique_ptr<Impl> impl_;
};

// Everything every configured server offers. Servers that fail to start are
// skipped with a note rather than failing the lot -- one broken entry in a config
// should not take away the others.
std::vector<McpTool> discover_mcp_tools(std::vector<std::string>* problems = nullptr);

}  // namespace auspex
