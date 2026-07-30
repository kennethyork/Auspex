#include "auspex/asr.hpp"

#include <algorithm>
#include <thread>
#include <utility>

#include "auspex/audio.hpp"

#include <ggml-backend.h>
#include <whisper.h>

namespace auspex {

namespace {

void set_error(std::string* out, std::string msg) {
    if (out) *out = std::move(msg);
}

void discard_log(ggml_log_level /*level*/, const char* /*text*/, void* /*user*/) {}

// whisper returns segment text with a leading space and, for some models, stray
// newlines. Segments are concatenated then squeezed here rather than at each
// call site.
std::string tidy(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = true;   // true so a leading space is dropped
    for (const char c : s) {
        const bool is_space = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (is_space) {
            if (!prev_space) out.push_back(' ');
        } else {
            out.push_back(c);
        }
        prev_space = is_space;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

void Asr::silence_logs() {
    whisper_log_set(discard_log, nullptr);
    ggml_log_set(discard_log, nullptr);
}

std::string Asr::backends() {
    std::string out;
    const std::size_t n = ggml_backend_reg_count();
    for (std::size_t i = 0; i < n; ++i) {
        const char* name = ggml_backend_reg_name(ggml_backend_reg_get(i));
        if (!name) continue;
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out.empty() ? "none" : out;
}

std::optional<Asr> Asr::open(const std::filesystem::path& model, Options options,
                             std::string* error) {
    if (model.empty()) {
        set_error(error, "no whisper model configured (set \"whisper_model\" in config.json)");
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::exists(model, ec)) {
        set_error(error, "whisper model not found: " + model.string());
        return std::nullopt;
    }

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu    = options.use_gpu;
    cparams.flash_attn = options.flash_attn;

    whisper_context* ctx =
        whisper_init_from_file_with_params(model.string().c_str(), cparams);
    if (!ctx) {
        set_error(error, "failed to load whisper model: " + model.string());
        return std::nullopt;
    }

    if (options.threads <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        options.threads = static_cast<int>(hw ? hw : 4u);
    }

    Asr asr;
    asr.ctx_     = ctx;
    asr.options_ = std::move(options);
    return asr;
}

Asr::~Asr() {
    if (ctx_) whisper_free(ctx_);
}

Asr::Asr(Asr&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)), options_(std::move(other.options_)) {}

Asr& Asr::operator=(Asr&& other) noexcept {
    if (this != &other) {
        if (ctx_) whisper_free(ctx_);
        ctx_     = std::exchange(other.ctx_, nullptr);
        options_ = std::move(other.options_);
    }
    return *this;
}

std::optional<Asr::Result> Asr::transcribe(std::span<const float> pcm, std::string* error) {
    if (!ctx_) {
        set_error(error, "Asr used after move");
        return std::nullopt;
    }

    // whisper pads to 30s internally but genuinely empty input just wastes a
    // forward pass and returns noise.
    if (pcm.size() < static_cast<std::size_t>(audio::kSampleRate / 10)) {
        set_error(error, "audio shorter than 100ms");
        return std::nullopt;
    }

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads        = options_.threads;
    wparams.translate        = options_.translate;
    wparams.no_timestamps    = true;
    wparams.print_progress   = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    wparams.suppress_blank   = true;

    // "auto" means let whisper detect; otherwise pin it. Pinning is both faster
    // and stops short commands being misdetected as another language.
    if (options_.language == "auto") {
        wparams.language        = nullptr;
        wparams.detect_language = true;
    } else {
        wparams.language        = options_.language.c_str();
        wparams.detect_language = false;
    }

    const auto started = std::chrono::steady_clock::now();
    if (whisper_full(ctx_, wparams, pcm.data(), static_cast<int>(pcm.size())) != 0) {
        set_error(error, "whisper_full failed");
        return std::nullopt;
    }
    const auto finished = std::chrono::steady_clock::now();

    std::string text;
    const int segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < segments; ++i) {
        if (const char* seg = whisper_full_get_segment_text(ctx_, i)) text += seg;
    }

    Result result;
    result.text = tidy(std::move(text));
    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
    result.audio_seconds = static_cast<double>(pcm.size()) / audio::kSampleRate;
    return result;
}

}  // namespace auspex
