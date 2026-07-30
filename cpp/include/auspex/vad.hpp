// Voice activity detection.
//
// Wraps whisper.cpp's built-in Silero VAD (whisper_vad_*). asr.py loaded Silero
// through torch.hub, which meant torch, a network fetch on first run, and a
// Python process; the same model is available here as a ~40MB GGML file through
// the whisper library already linked.
//
// This class only answers "how likely is this chunk speech?". Turning that into
// utterance boundaries is VoiceGate's job.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>

struct whisper_vad_context;

namespace auspex {

struct VadOptions {
    int threads = 0;   // 0 => hardware_concurrency

    // CPU by default, and you almost certainly want to leave it there. The Silero
    // graph contains ops the Vulkan backend cannot run, and ggml does not fall back
    // gracefully -- it aborts the process with
    //   "pre-allocated tensor (leaf_0) in a buffer (Vulkan0) that cannot run the
    //    operation (NONE)"
    // The model is 865KB and runs in well under a millisecond per chunk, so there
    // is nothing to gain from a GPU here regardless.
    bool use_gpu = false;
};

class Vad {
public:
    // Silero expects 16 kHz mono, and it operates on 512-sample windows (32ms).
    // Chunks fed to probability() should be a multiple of this.
    static constexpr int kWindowSamples = 512;
    static constexpr int kWindowMs      = 32;

    static std::optional<Vad> open(const std::filesystem::path& model,
                                   VadOptions options = VadOptions{},
                                   std::string* error = nullptr);

    ~Vad();
    Vad(const Vad&)            = delete;
    Vad& operator=(const Vad&) = delete;
    Vad(Vad&&) noexcept;
    Vad& operator=(Vad&&) noexcept;

    // Highest speech probability across the windows in `pcm` (mono f32 @16kHz).
    // Uses the no-reset entry point so the LSTM state carries across calls, which
    // is what makes streaming detection stable; call reset() between utterances.
    std::optional<float> probability(std::span<const float> pcm);

    void reset();

private:
    Vad() = default;

    whisper_vad_context* ctx_ = nullptr;
};

}  // namespace auspex
