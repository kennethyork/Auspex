// Utterance segmentation from speech probabilities.
//
// This is the state machine that turns a stream of per-chunk speech
// probabilities into "an utterance started" / "an utterance ended" events. It
// is the logic half of continuous listening, deliberately separated from the VAD
// model so it can be unit-tested with synthetic probabilities -- no microphone,
// no 40MB model, no timing flakiness.
//
// Replaces the VAD loop in src/utils/asr.py, which ran Silero directly with a
// bare threshold of 0.5 and no hysteresis: a single dip below threshold ended
// the utterance, so normal pauses between words cut sentences in half.
#pragma once

#include <cstdint>

namespace auspex {

struct VoiceGateConfig {
    // Probability above which a chunk counts as speech. Silero's useful range is
    // roughly 0.3-0.7; higher rejects more background noise but clips soft speech.
    float threshold = 0.5f;

    // Speech must persist this long before an utterance opens. Filters keyboard
    // clicks and door slams, which spike for one or two chunks.
    int min_speech_ms = 250;

    // Silence must persist this long before an utterance closes. This is the
    // hysteresis asr.py lacked: people pause mid-sentence, and 700ms is long
    // enough to ride through that without feeling laggy.
    int min_silence_ms = 700;

    // Hard cap so a noisy room cannot grow one utterance without bound.
    int max_utterance_ms = 30000;

    // Audio kept from before the trigger. Speech is always slightly clipped at
    // onset because the gate needs min_speech_ms of evidence first; this hands
    // that back so the transcript does not lose the first syllable.
    int pre_roll_ms = 300;
};

class VoiceGate {
public:
    enum class Event {
        None,     // nothing changed
        Started,  // an utterance just opened
        Ended,    // an utterance just closed and should be transcribed
        Capped,   // closed because max_utterance_ms was hit
    };

    explicit VoiceGate(VoiceGateConfig config = {}) : config_(config) {}

    // Feed one chunk's speech probability and its duration.
    Event feed(float probability, int chunk_ms);

    // Drop all accumulated state, e.g. after the microphone is switched.
    void reset();

    bool speaking() const { return speaking_; }
    int  utterance_ms() const { return utterance_ms_; }

    const VoiceGateConfig& config() const { return config_; }

private:
    VoiceGateConfig config_;

    bool speaking_        = false;
    int  speech_run_ms_   = 0;   // consecutive speech while closed
    int  silence_run_ms_  = 0;   // consecutive silence while open
    int  utterance_ms_    = 0;
};

}  // namespace auspex
