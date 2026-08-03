#include "auspex/ollama_client.hpp"

#include <mutex>
#include <thread>
#include <utility>

#include <curl/curl.h>

#include "auspex/smoke.hpp"
#include <nlohmann/json.hpp>

#include "auspex/context_tuner.hpp"
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

// curl_easy_perform, except that smoke mode does not reach the network.
//
// Wrapped rather than guarded at each call site: there are two here and two in
// the other file that talks to the network, and a guard you have to remember to
// add to the next one is a guard that will be missing from the next one.
CURLcode guarded_curl_perform(CURL* curl) {
    if (smoke_refuse("http")) return CURLE_COULDNT_CONNECT;
    return curl_easy_perform(curl);
}

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
    // curl_global_init BEFORE any easy handle, exactly once, and safe to race on.
    //
    // libcurl will do this implicitly from curl_easy_init if nobody has -- and
    // that implicit path is explicitly NOT thread-safe. Auspex builds a client per
    // model call and the crew makes those from coder worker threads, so the
    // implicit init is reached from several threads at once.
    //
    // What it looks like when it goes wrong is not a crash: it is a request that
    // quietly fails. Chased for a while as "the Auditor could not be reached",
    // which was true and said nothing about why -- the Auditor is simply the call
    // that happens after the threads have started.
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

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

    const CURLcode rc = guarded_curl_perform(curl);
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

    // Retried on 429 and 5xx, and ONLY those.
    //
    // A hosted model answering "temporarily overloaded, please retry shortly" is
    // not a failure, it is a queue -- and treating it as one threw away a coder's
    // entire finished work, because an audit that cannot happen holds. Measured:
    // the Auditor failed on three runs out of three with a 503 while a smaller
    // request to the same model on the same connection succeeded.
    //
    // Only on a real HTTP status, never on a transport error. A request that hung
    // has already spent the whole timeout, and retrying it would multiply a stall
    // by three rather than recover from anything.
    constexpr int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        result.body.clear();
        const CURLcode rc = guarded_curl_perform(curl);
        if (rc != CURLE_OK) break;

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
        result.ok = result.status >= 200 && result.status < 300;
        if (result.ok) break;

        const bool worth_retrying =
            result.status == 429 || (result.status >= 500 && result.status < 600);
        if (!worth_retrying || attempt == kAttempts - 1) break;

        // Backing off rather than hammering: the server has just said it is out of
        // capacity, and asking again immediately is how a queue becomes a spiral.
        std::this_thread::sleep_for(std::chrono::seconds(1 + 2 * attempt));
    }

    curl_slist_free_all(headers);
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

    // The context window, sized against this machine when the config does not say.
    //
    // Measured once per process, not per call: reading /proc/meminfo and NVML on
    // every model call would be a probe per turn for a number that cannot change.
    static const int kContext = [this] {
        return context_for(config_.num_ctx, machine_memory());
    }();
    options["num_ctx"] = kContext;

    req["options"] = std::move(options);

    last_error_.clear();

    const auto res = post_json("/api/generate", safe_dump(req), std::chrono::seconds(120));
    if (!res.ok) {
        // The server's own words where it gave any -- "temporarily overloaded" is
        // a different problem from "nothing is listening", and only one of them is
        // fixed by picking another model.
        const json body = json::parse(res.body, nullptr, false);
        if (!body.is_discarded() && body.is_object() && body.contains("error") &&
            body["error"].is_string()) {
            last_error_ = body["error"].get<std::string>();
        } else if (res.status > 0) {
            last_error_ = "the model server answered " + std::to_string(res.status);
        } else {
            last_error_ = "the model server could not be reached";
        }
        return std::nullopt;
    }

    const json j = json::parse(res.body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("response")) {
        last_error_ = "the model server sent a reply that could not be understood";
        return std::nullopt;
    }

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
