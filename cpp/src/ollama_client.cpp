#include "auspex/ollama_client.hpp"

#include <thread>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "auspex/usage.hpp"
#include "auspex/json_util.hpp"

using json = nlohmann::json;

namespace auspex {

std::string_view OracleStatus::label() const {
    switch (state) {
        case OracleState::Error:    return "Error";
        case OracleState::Starting: return "Starting";
        case OracleState::Loading:  return "Loading";
        case OracleState::Running:  return "Running";
    }
    return "Error";
}

namespace {

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

void emit(const StatusCallback& cb, const OracleStatus& s) {
    if (cb) cb(s);
}

// Trim a trailing '/' so config values with or without one both work.
std::string join_url(std::string base, const std::string& path) {
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + path;
}

}  // namespace

OllamaClient::OllamaClient(Config config) : config_(std::move(config)) {
    curl_ = curl_easy_init();
}

OllamaClient::~OllamaClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

OllamaClient::OllamaClient(OllamaClient&& other) noexcept
    : config_(std::move(other.config_)), curl_(std::exchange(other.curl_, nullptr)) {}

OllamaClient& OllamaClient::operator=(OllamaClient&& other) noexcept {
    if (this != &other) {
        if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
        config_ = std::move(other.config_);
        curl_   = std::exchange(other.curl_, nullptr);
    }
    return *this;
}

OllamaClient::HttpResult OllamaClient::get(const std::string& path,
                                           std::chrono::seconds timeout) {
    HttpResult result;
    auto* curl = static_cast<CURL*>(curl_);
    if (!curl) return result;

    const std::string url = join_url(config_.ollama_endpoint, path);

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) return result;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.ok = result.status >= 200 && result.status < 300;
    return result;
}

OllamaClient::HttpResult OllamaClient::post_json(const std::string& path,
                                                 const std::string& body,
                                                 std::chrono::seconds timeout) {
    HttpResult result;
    auto* curl = static_cast<CURL*>(curl_);
    if (!curl) return result;

    const std::string url = join_url(config_.ollama_endpoint, path);

    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) return result;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    result.ok = result.status >= 200 && result.status < 300;
    return result;
}

std::optional<std::string> OllamaClient::version() {
    const auto res = get("/api/version", std::chrono::seconds(5));
    if (!res.ok) return std::nullopt;

    const json j = json::parse(res.body, nullptr, false);
    if (j.is_discarded() || !j.contains("version")) return std::nullopt;
    return j["version"].get<std::string>();
}

std::vector<std::string> OllamaClient::list_models(std::chrono::seconds timeout) {
    std::vector<std::string> names;
    const auto res = get("/api/tags", timeout);
    if (!res.ok) return names;

    const json j = json::parse(res.body, nullptr, false);
    if (j.is_discarded() || !j.contains("models") || !j["models"].is_array()) return names;

    for (const auto& m : j["models"]) {
        if (m.contains("name") && m["name"].is_string()) {
            names.push_back(m["name"].get<std::string>());
        }
    }
    return names;
}

std::optional<GenerateResult> OllamaClient::generate(const std::string& model,
                                                     const std::string& prompt,
                                                     GenerateOptions opts) {
    json req{{"model", model}, {"prompt", prompt}, {"stream", false}};
    if (opts.json) req["format"] = "json";
    if (opts.disable_thinking) req["think"] = false;

    json options = json::object();
    if (opts.num_predict >= 0)  options["num_predict"] = opts.num_predict;
    if (opts.temperature >= 0)  options["temperature"] = opts.temperature;
    if (!options.empty())       req["options"] = std::move(options);

    const auto res = post_json("/api/generate", safe_dump(req), std::chrono::seconds(120));
    if (!res.ok) return std::nullopt;

    const json j = json::parse(res.body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("response")) return std::nullopt;

    GenerateResult out;
    out.response = j.value("response", std::string{});
    out.thinking = j.value("thinking", std::string{});
    out.done     = j.value("done", false);
    out.done_reason = j.value("done_reason", std::string{});
    out.prompt_tokens = j.value("prompt_eval_count", 0);
    out.eval_tokens   = j.value("eval_count", 0);

    // Metered here, at the one place every model call in Auspex's own loop passes
    // through. Threading a counter out to each of the eight roles instead would
    // mean a new role silently costs nothing until somebody remembers to add it.
    record_usage(model, out.prompt_tokens, out.eval_tokens);
    return out;
}

OracleStatus OllamaClient::verify_model(const StatusCallback& on_update) {
    // Mirrors _verify_model(): a cheap zero-temperature generation proves the
    // model is not merely listed but resident and answering.
    //
    // Readiness is judged on `done`, NOT on non-empty response text. A reasoning
    // model spends its budget in "thinking" and can legitimately return an empty
    // "response", which would otherwise read as a permanent failure. num_predict
    // is also 10x the Python's 10 so a thinking model can reach its answer.
    const auto reply = generate(config_.ollama_model, "Are you ready?",
                                GenerateOptions{.num_predict = 100, .temperature = 0.0});

    OracleStatus s;
    if (reply && reply->completed()) {
        s = {OracleState::Running, 100, "Oracle is prophesying"};
    } else {
        s = {OracleState::Loading, 70, "Oracle is meditating..."};
    }
    emit(on_update, s);
    return s;
}

std::vector<float> OllamaClient::embed(const std::string& model,
                                       const std::string& text,
                                       std::chrono::seconds timeout) {
    if (model.empty() || text.empty()) return {};

    json body;
    body["model"]  = model;
    body["prompt"] = text;

    const auto reply = post_json("/api/embeddings", safe_dump(body), timeout);
    if (!reply.ok) return {};

    const auto document = json::parse(reply.body, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return {};
    if (!document.contains("embedding") || !document["embedding"].is_array()) return {};

    std::vector<float> vector;
    vector.reserve(document["embedding"].size());
    for (const auto& value : document["embedding"]) {
        if (value.is_number()) vector.push_back(static_cast<float>(value.get<double>()));
    }
    return vector;
}

OracleStatus OllamaClient::check_status(const StatusCallback& on_update) {
    if (!version()) {
        // The Python original distinguished ConnectionError from Timeout to pick
        // between two messages. libcurl reports both as a failed perform, and the
        // distinction never changed behaviour, so it collapses to one branch.
        const OracleStatus s{OracleState::Error, 0,
                             "Oracle absent - summon with: systemctl start ollama"};
        emit(on_update, s);
        return s;
    }

    emit(on_update, {OracleState::Starting, 20, "Oracle is preparing..."});
    return verify_model(on_update);
}

OracleStatus OllamaClient::await_startup(const StatusCallback& on_update,
                                         std::chrono::seconds timeout,
                                         std::chrono::seconds interval) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int attempts = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        if (version()) return verify_model(on_update);

        ++attempts;
        const int progress = std::min(60, 15 + attempts * 2);
        emit(on_update, {OracleState::Starting, progress, "Awaiting Oracle's arrival..."});
        std::this_thread::sleep_for(interval);
    }

    const OracleStatus s{OracleState::Error, 0, "Oracle got lost - check the pathways"};
    emit(on_update, s);
    return s;
}

}  // namespace auspex
