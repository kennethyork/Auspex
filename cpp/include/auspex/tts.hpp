// Text-to-speech glue.
//
// Replaces src/utils/voice.py (Coqui TTS) and src/utils/magi_espeak.py. Upstream
// loaded a ~2GB Coqui VITS model into a Python process and wrote WAVs to a
// watched directory; a filesystem watcher then played them. That whole path is
// gone.
//
// Instead, Config::tts_command is a shell pipeline that takes text on stdin and
// produces sound, e.g.
//
//   piper --model .../en_US-lessac-medium.onnx --output-raw
//     | pw-play --rate=22050 --channels=1 --format=s16 -
//
// Consequences, deliberately accepted:
//   * the engine is swappable (piper, espeak-ng, a remote service) with no
//     rebuild, and the sample rate lives next to the voice that determines it
//   * the string IS executed by /bin/sh. config.json is user-owned and the value
//     is a command by design, so this is configuration, not an injection bug --
//     but it does mean the file must stay user-writable only.
#pragma once

#include <string>

namespace auspex {

class Tts {
public:
    explicit Tts(std::string command) : command_(std::move(command)) {}

    // False when no tts_command is configured; speak() would be a no-op.
    bool available() const { return !command_.empty(); }

    // Blocks until the utterance has finished playing. Returns false and sets
    // `error` if the pipeline could not be started or exited non-zero.
    bool speak(const std::string& text, std::string* error = nullptr) const;

    const std::string& command() const { return command_; }

private:
    std::string command_;
};

}  // namespace auspex
