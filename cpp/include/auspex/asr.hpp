// In-process speech recognition via whisper.cpp.
//
// Replaces src/utils/whisper_server.py and src/utils/asr.py. Differences that
// matter:
//   * no Flask, no HTTP, no port. The Python server bound 0.0.0.0:5000, which
//     exposed transcription to the LAN; there is now no socket to expose.
//   * no torch and no transformers. The model is a single GGML file.
//   * no systemd unit. The model loads with the process that uses it.
//
// A loaded model is ~1.6GB of state, so Asr is move-only and meant to be created
// once and reused for every utterance.
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct whisper_context;

namespace auspex {

// Declared at namespace scope, not nested in Asr: a nested class's default
// member initializers cannot be used in a default argument of the enclosing
// class, because the enclosing class is still incomplete at that point.
// Asr::Options remains available as an alias below.
struct AsrOptions {
    int         threads    = 0;      // 0 => hardware_concurrency
    std::string language   = "en";   // "auto" to detect
    bool        translate  = false;
    bool        use_gpu    = true;   // ignored if built CPU-only
    bool        flash_attn = false;
};

class Asr {
public:
    using Options = AsrOptions;

    struct Result {
        std::string               text;
        std::chrono::milliseconds elapsed{0};
        double                    audio_seconds = 0.0;

        // >1 means faster than realtime.
        double realtime_factor() const {
            const double s = static_cast<double>(elapsed.count()) / 1000.0;
            return s > 0 ? audio_seconds / s : 0.0;
        }
    };

    // nullopt if the model file is missing or fails to load.
    static std::optional<Asr> open(const std::filesystem::path& model,
                                   Options options = AsrOptions{},
                                   std::string* error = nullptr);

    ~Asr();
    Asr(const Asr&)            = delete;
    Asr& operator=(const Asr&) = delete;
    Asr(Asr&&) noexcept;
    Asr& operator=(Asr&&) noexcept;

    // pcm must be mono 32-bit float at auspex::audio::kSampleRate.
    std::optional<Result> transcribe(std::span<const float> pcm,
                                     std::string* error = nullptr);

    // Which ggml backends the linked whisper was built with, e.g. "CPU" or
    // "CPU, Vulkan". Reported by auspex-listen so the user can see whether the
    // GPU is actually in play.
    static std::string backends();

    // Quiets whisper.cpp/ggml's very chatty stderr output. Call once at startup.
    static void silence_logs();

private:
    Asr() = default;

    whisper_context* ctx_ = nullptr;
    Options          options_{};
};

}  // namespace auspex
