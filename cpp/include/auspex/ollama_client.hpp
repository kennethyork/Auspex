// Native replacement for src/magi_shell/models/ollama.py.
//
// Behavioural notes vs the Python original:
//   * the endpoint is read from Config instead of being hardcoded to
//     localhost:11434 in five places
//   * generate() returns the model's text instead of `bool`; the Python version
//     threw away the response body and only reported "ok"
//   * no threads here. The Python class spawned daemon threads internally, which
//     made it impossible to use from a non-GLib context. Callers thread this.
//   * the "Oracle" status voice is preserved verbatim — it is part of the shell's
//     character, not incidental logging.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "auspex/config.hpp"

namespace auspex {

enum class OracleState { Error, Starting, Loading, Running };

struct OracleStatus {
    OracleState state = OracleState::Error;
    int         progress = 0;   // 0-100
    std::string message;

    // "Error" / "Starting" / "Loading" / "Running" — matches the label the
    // Python status_callback passed as its first argument.
    std::string_view label() const;
};

using StatusCallback = std::function<void(const OracleStatus&)>;

struct GenerateOptions {
    int    num_predict = -1;      // -1 leaves it to the server
    double temperature = -1.0;    // <0 leaves it to the server

    // Sets Ollama's "format":"json", constraining decoding to valid JSON. Needed
    // for command interpretation, where a prose preamble would be unparseable.
    bool json = false;

    // Sets "think":false. Reasoning models otherwise spend their budget in the
    // `thinking` field and can return an empty `response` -- fine for chat, fatal
    // when the caller needs the JSON.
    bool disable_thinking = false;
};

// Reasoning models (qwen3.x, gpt-oss, deepseek) split their output: chain of
// thought lands in "thinking" and only the final answer in "response". A short
// num_predict can therefore complete successfully with an empty `response`.
// Both fields are surfaced so callers can tell "no answer yet" from "failed".
struct GenerateResult {
    std::string response;
    std::string thinking;
    bool        done = false;
    std::string done_reason;   // "stop", "length", ...

    // True when the server completed the request, regardless of which field the
    // tokens landed in. This is the correct readiness signal.
    bool completed() const { return done; }
};

class OllamaClient {
public:
    explicit OllamaClient(Config config);
    ~OllamaClient();

    OllamaClient(const OllamaClient&)            = delete;
    OllamaClient& operator=(const OllamaClient&) = delete;
    OllamaClient(OllamaClient&&) noexcept;
    OllamaClient& operator=(OllamaClient&&) noexcept;

    // GET /api/version — nullopt if the daemon is unreachable.
    std::optional<std::string> version();

    // GET /api/tags — model names currently pulled. Empty on failure.
    std::vector<std::string> list_models();

    // POST /api/generate with "stream": false. nullopt on transport/HTTP error
    // or unparseable body; a populated result otherwise (possibly with empty
    // `response` — see GenerateResult).
    std::optional<GenerateResult> generate(const std::string& model,
                                           const std::string& prompt,
                                           GenerateOptions opts = {});

    // Single-shot equivalent of the Python check_status(): probes the daemon,
    // then confirms the configured model actually answers.
    OracleStatus check_status(const StatusCallback& on_update = {});

    // Equivalent of _monitor_startup(): polls until the daemon answers or the
    // deadline passes. Blocking — run it on your own thread.
    OracleStatus await_startup(const StatusCallback& on_update = {},
                               std::chrono::seconds timeout = std::chrono::minutes(15),
                               std::chrono::seconds interval = std::chrono::seconds(30));

    const Config& config() const { return config_; }

private:
    struct HttpResult {
        bool        ok = false;
        long        status = 0;
        std::string body;
    };

    HttpResult get(const std::string& path, std::chrono::seconds timeout);
    HttpResult post_json(const std::string& path, const std::string& body,
                         std::chrono::seconds timeout);
    OracleStatus verify_model(const StatusCallback& on_update);

    Config config_;
    void*  curl_ = nullptr;  // CURL* — kept opaque to avoid leaking curl.h here
};

}  // namespace auspex
