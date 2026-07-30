// auspex-say — native TTS, replacing the Coqui voice server + watched directory.
//
//   auspex-say "text to speak"
//   echo "text" | auspex-say
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "auspex/config.hpp"
#include "auspex/tts.hpp"

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const auspex::Config cfg = auspex::Config::load();
    const auspex::Tts tts(cfg.tts_command);

    if (!tts.available()) {
        std::cerr << "auspex-say: no \"tts_command\" in "
                  << auspex::Config::default_path() << "\n"
                     "         run bin/setup.sh, or set it by hand, e.g.\n"
                     "         \"piper --model VOICE.onnx --output-raw"
                     " | pw-play --rate=22050 --channels=1 --format=s16 -\"\n";
        return 1;
    }

    std::string text;
    if (args.empty()) {
        std::ostringstream in;
        in << std::cin.rdbuf();
        text = in.str();
    } else {
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) text += ' ';
            text += args[i];
        }
    }

    // Trim trailing newline so piper does not emit a second empty utterance.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();

    if (text.empty()) {
        std::cerr << "auspex-say: nothing to say\n";
        return 2;
    }

    std::string err;
    if (!tts.speak(text, &err)) {
        std::cerr << "auspex-say: " << err << "\n";
        std::cerr << "          pipeline: " << tts.command() << "\n";
        return 1;
    }
    return 0;
}
