// auspex-listen — native ASR, replacing the Flask whisper server.
//
//   auspex-listen <file>        transcribe an audio file
//   auspex-listen --mic [secs]  record from the default input then transcribe
//   auspex-listen --backends    show which ggml backends are compiled in
#include <algorithm>
#include <chrono>
#include <span>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "auspex/asr.hpp"
#include "auspex/audio.hpp"
#include "auspex/config.hpp"
#include "auspex/vad.hpp"
#include "auspex/voice_gate.hpp"

namespace {

int usage() {
    std::cerr << "usage: auspex-listen <audio-file>\n"
                 "       auspex-listen --mic [seconds]\n"
                 "       auspex-listen --backends\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();

    auspex::Asr::silence_logs();

    // Runs the real VAD + VoiceGate over a file and reports the utterances it would
    // have segmented. Exercises the whole continuous-listening path without a mic.
    if (args.size() >= 2 && args[0] == "--vad") {
        const auspex::Config cfg = auspex::Config::load();
        auspex::Asr::silence_logs();

        std::string err;
        auto vad = auspex::Vad::open(cfg.vad_model, {.threads = cfg.asr_threads}, &err);
        if (!vad) { std::cerr << "auspex-listen: " << err << "\n"; return 1; }

        auto pcm = auspex::audio::load_mono16k(args[1], &err);
        if (!pcm) { std::cerr << "auspex-listen: " << err << "\n"; return 1; }

        auspex::VoiceGateConfig gc;
        gc.threshold      = cfg.vad_threshold;
        gc.min_speech_ms  = cfg.vad_min_speech_ms;
        gc.min_silence_ms = cfg.vad_min_silence_ms;
        auspex::VoiceGate gate(gc);

        constexpr std::size_t kWindows = 16;
        constexpr std::size_t kChunk   = auspex::Vad::kWindowSamples * kWindows;
        constexpr int         kChunkMs = auspex::Vad::kWindowMs * (int)kWindows;

        int utterances = 0, elapsed_ms = 0, started_ms = 0;
        float peak_prob = 0.0f;
        for (std::size_t off = 0; off + kChunk <= pcm->size(); off += kChunk) {
            const auto p = vad->probability(std::span<const float>(pcm->data() + off, kChunk));
            elapsed_ms += kChunkMs;
            if (!p) continue;
            peak_prob = std::max(peak_prob, *p);
            const auto ev = gate.feed(*p, kChunkMs);
            if (ev == auspex::VoiceGate::Event::Started) started_ms = elapsed_ms;
            if (ev == auspex::VoiceGate::Event::Ended || ev == auspex::VoiceGate::Event::Capped) {
                ++utterances;
                std::cout << "utterance " << utterances << ": "
                          << started_ms / 1000.0 << "s -> " << elapsed_ms / 1000.0 << "s\n";
            }
        }
        if (gate.speaking()) {
            ++utterances;
            std::cout << "utterance " << utterances << ": " << started_ms / 1000.0
                      << "s -> end (still open)\n";
        }
        std::cout << "peak speech probability: " << peak_prob << "\n"
                  << "utterances detected    : " << utterances << "\n";
        return utterances > 0 ? 0 : 1;
    }

    if (args[0] == "--devices") {
        const auto devices = auspex::audio::list_input_devices();
        if (devices.empty()) { std::cerr << "no capture devices found\n"; return 1; }
        for (const auto& d : devices) {
            std::cout << (d.is_default ? "* " : "  ") << d.name << "\n";
        }
        return 0;
    }

    if (args[0] == "--backends") {
        std::cout << auspex::Asr::backends() << "\n";
        return 0;
    }

    const auspex::Config cfg = auspex::Config::load();

    std::string err;
    auto asr = auspex::Asr::open(cfg.whisper_model,
                               {.threads  = cfg.asr_threads,
                                .language = cfg.asr_language},
                               &err);
    if (!asr) {
        std::cerr << "auspex-listen: " << err << "\n";
        return 1;
    }
    std::cerr << "[backends: " << auspex::Asr::backends() << "]\n";

    std::vector<float> pcm;
    if (args[0] == "--mic") {
        const int seconds = args.size() > 1 ? std::atoi(args[1].c_str()) : 5;
        if (seconds <= 0) return usage();

        std::cerr << "[recording " << seconds << "s from default input...]\n";
        auto captured = auspex::audio::record_mono16k(std::chrono::seconds(seconds), &err);
        if (!captured) {
            std::cerr << "auspex-listen: " << err << "\n";
            return 1;
        }
        pcm = std::move(*captured);

        const float peak = auspex::audio::peak_level(pcm);
        std::cerr << "[captured " << auspex::audio::duration_seconds(pcm)
                  << "s, peak " << peak << "]\n";
        if (peak < 0.001f) {
            std::cerr << "auspex-listen: input was silent — check your recording device\n";
            return 1;
        }
    } else {
        auto loaded = auspex::audio::load_mono16k(args[0], &err);
        if (!loaded) {
            std::cerr << "auspex-listen: " << err << "\n";
            return 1;
        }
        pcm = std::move(*loaded);
    }

    const auto result = asr->transcribe(pcm, &err);
    if (!result) {
        std::cerr << "auspex-listen: " << err << "\n";
        return 1;
    }

    std::cerr << "[" << result->audio_seconds << "s audio in "
              << result->elapsed.count() << "ms = "
              << result->realtime_factor() << "x realtime]\n";
    std::cout << result->text << "\n";
    return 0;
}
