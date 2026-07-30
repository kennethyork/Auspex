#include "auspex/voice_gate.hpp"

namespace auspex {

void VoiceGate::reset() {
    speaking_       = false;
    speech_run_ms_  = 0;
    silence_run_ms_ = 0;
    utterance_ms_   = 0;
}

VoiceGate::Event VoiceGate::feed(float probability, int chunk_ms) {
    if (chunk_ms <= 0) return Event::None;

    const bool is_speech = probability >= config_.threshold;

    if (!speaking_) {
        if (is_speech) {
            speech_run_ms_ += chunk_ms;
            if (speech_run_ms_ >= config_.min_speech_ms) {
                speaking_       = true;
                silence_run_ms_ = 0;
                // The run that opened the gate is already part of the utterance.
                utterance_ms_   = speech_run_ms_;
                speech_run_ms_  = 0;
                return Event::Started;
            }
        } else {
            // Any silence resets the evidence. Speech has to be contiguous, or a
            // steady tick of noise would eventually accumulate past the threshold.
            speech_run_ms_ = 0;
        }
        return Event::None;
    }

    // Open: accumulate everything, including the trailing silence, so the
    // transcriber receives the natural end of the phrase rather than a hard cut.
    utterance_ms_ += chunk_ms;

    if (utterance_ms_ >= config_.max_utterance_ms) {
        speaking_       = false;
        speech_run_ms_  = 0;
        silence_run_ms_ = 0;
        return Event::Capped;
    }

    if (is_speech) {
        silence_run_ms_ = 0;
        return Event::None;
    }

    silence_run_ms_ += chunk_ms;
    if (silence_run_ms_ >= config_.min_silence_ms) {
        speaking_       = false;
        speech_run_ms_  = 0;
        silence_run_ms_ = 0;
        return Event::Ended;
    }
    return Event::None;
}

}  // namespace auspex
