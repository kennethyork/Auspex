// Audio capture and decoding for MAGI.
//
// Replaces the sounddevice/numpy path in src/utils/asr.py. Uses miniaudio (the
// same single-header library whisper.cpp itself vendors) so there is no
// PortAudio link dependency and no subprocess spawning for capture.
//
// Everything here produces exactly what whisper wants: 32-bit float, mono,
// 16 kHz. Resampling and channel mixing are miniaudio's job, not the caller's.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace auspex::audio {

// whisper.cpp requires 16 kHz input. This is not configurable on the model side,
// so it is a constant rather than a Config field. (Config::sample_rate is kept
// only for compatibility with existing config.json files.)
inline constexpr int kSampleRate = 16000;
inline constexpr int kChannels   = 1;

// Decode any format miniaudio understands (wav/flac/mp3) to mono 16 kHz float.
std::optional<std::vector<float>> load_mono16k(const std::filesystem::path& path,
                                               std::string* error = nullptr);

// Capture from the default input device for a fixed duration.
std::optional<std::vector<float>> record_mono16k(std::chrono::milliseconds duration,
                                                 std::string* error = nullptr);

// Capture until `should_stop()` returns true, polled roughly every 50ms.
// Used for push-to-talk, where the caller decides when the utterance ends.
std::optional<std::vector<float>> record_mono16k_until(
    const std::function<bool()>& should_stop,
    std::chrono::milliseconds max_duration = std::chrono::seconds(120),
    std::string* error = nullptr);

// Peak absolute sample value, for a level meter / silence rejection.
float peak_level(const std::vector<float>& pcm);

// ---------------------------------------------------------------------------
// Device selection
// ---------------------------------------------------------------------------
// settings.py exposed a "default_microphone" option; until now every capture used
// whatever miniaudio considered the default, so on a box with several inputs
// (webcam, HDMI, interface) voice could be silently dead with no way to fix it
// from config.
struct InputDevice {
    std::string name;
    bool        is_default = false;
};

std::vector<InputDevice> list_input_devices();

// Substring, case-insensitive match against the device names above. Empty selects
// the system default. Returns false if a non-empty name matched nothing, so the
// caller can say so instead of silently recording from the wrong device.
bool select_input_device(std::string_view name_fragment, std::string* error = nullptr);

// The device currently selected, or "" for the system default.
std::string selected_input_device();

// ---------------------------------------------------------------------------
// Continuous capture
// ---------------------------------------------------------------------------
// An always-on capture stream that hands out fixed-size chunks. This is what
// continuous VAD listening runs on: record_mono16k() cannot be used because it
// owns a fixed duration and only returns once finished.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream&)            = delete;
    Stream& operator=(const Stream&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool running() const;

    // Blocks until `frames` samples are available or the stream stops. Returns
    // false once stopped. `frames` should be a multiple of Vad::kWindowSamples.
    bool read(std::vector<float>& out, std::size_t frames);

    // Drop anything buffered, e.g. after speaking so the assistant does not hear
    // its own TTS output as the next utterance.
    void flush();

    // Public only because miniaudio's data callback is a plain function pointer
    // and so cannot be a member; it needs to see this type.
    struct Impl;

private:
    Impl* impl_ = nullptr;
};

inline double duration_seconds(const std::vector<float>& pcm) {
    return static_cast<double>(pcm.size()) / kSampleRate;
}

}  // namespace auspex::audio
