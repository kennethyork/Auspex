#include "auspex/vad.hpp"

#include <algorithm>
#include <thread>
#include <utility>

#include <whisper.h>

namespace auspex {

std::optional<Vad> Vad::open(const std::filesystem::path& model, VadOptions options,
                             std::string* error) {
    const auto set_error = [error](std::string msg) {
        if (error) *error = std::move(msg);
    };

    if (model.empty()) {
        set_error("no VAD model configured (set \"vad_model\" in config.json)");
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::exists(model, ec)) {
        set_error("VAD model not found: " + model.string());
        return std::nullopt;
    }

    whisper_vad_context_params cparams = whisper_vad_default_context_params();
    cparams.use_gpu = options.use_gpu;
    if (options.threads > 0) {
        cparams.n_threads = options.threads;
    } else {
        const unsigned hw = std::thread::hardware_concurrency();
        cparams.n_threads = static_cast<int>(hw ? hw : 4u);
    }

    whisper_vad_context* ctx =
        whisper_vad_init_from_file_with_params(model.string().c_str(), cparams);
    if (!ctx) {
        set_error("failed to load VAD model: " + model.string());
        return std::nullopt;
    }

    Vad vad;
    vad.ctx_ = ctx;
    return vad;
}

Vad::~Vad() {
    if (ctx_) whisper_vad_free(ctx_);
}

Vad::Vad(Vad&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

Vad& Vad::operator=(Vad&& other) noexcept {
    if (this != &other) {
        if (ctx_) whisper_vad_free(ctx_);
        ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

void Vad::reset() {
    if (ctx_) whisper_vad_reset_state(ctx_);
}

std::optional<float> Vad::probability(std::span<const float> pcm) {
    if (!ctx_) return std::nullopt;
    if (pcm.size() < static_cast<std::size_t>(kWindowSamples)) return std::nullopt;

    if (!whisper_vad_detect_speech_no_reset(ctx_, pcm.data(), static_cast<int>(pcm.size()))) {
        return std::nullopt;
    }

    const int n = whisper_vad_n_probs(ctx_);
    const float* probs = whisper_vad_probs(ctx_);
    if (n <= 0 || !probs) return std::nullopt;

    // Max rather than mean: a chunk containing the start of a word is speech even
    // if most of its windows are still silence, and under-reporting onsets is what
    // makes a gate feel unresponsive.
    return *std::max_element(probs, probs + n);
}

}  // namespace auspex
