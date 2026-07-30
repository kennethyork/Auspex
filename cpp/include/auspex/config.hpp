// Native replacement for src/magi_shell/utils/config.py.
//
// Reads the same ~/.config/auspex/config.json the Python shell used, so the two
// can coexist during migration. Defaults differ from upstream: the Python
// version fell back to mate-terminal / "mate-panel --run-dialog", which are not
// installed on Mint XFCE.
#pragma once

#include <filesystem>
#include <string>

namespace auspex {

struct Config {
    int         panel_height     = 28;
    int         workspace_count  = 4;
    bool        enable_effects   = true;
    bool        enable_ai        = true;
    // Empty means "detect at runtime". Hardcoding one desktop's binaries would
    // make the shell Xfce-only; these are resolved against PATH on first use so a
    // config written on Mint still works on Fedora/KDE/GNOME/i3.
    std::string terminal;
    std::string launcher;
    std::string settings_command;
    std::string network_command;
    std::string background;
    std::string ollama_model     = "qwen3.5:9b";
    std::string ollama_endpoint  = "http://127.0.0.1:11434";
    std::string whisper_endpoint = "http://127.0.0.1:5000/transcribe";
    int         sample_rate      = 16000;

    // ASR now runs in-process via whisper.cpp, so a GGML model path replaces the
    // Python server's HuggingFace repo id. whisper_endpoint above is retained
    // only so an existing config.json still parses; nothing reads it.
    std::string whisper_model;      // e.g. ~/.local/share/auspex/whisper/ggml-large-v3-turbo.bin
    int         asr_threads   = 0;  // 0 => hardware_concurrency
    std::string asr_language  = "en";

    // settings.py exposed this; capture ignored it entirely until now, so on a box
    // with several inputs voice could be dead with no way to fix it from config.
    // Empty means the system default. Matched as a case-insensitive substring.
    std::string default_microphone;

    // Continuous listening. An empty vad_model disables it, leaving press-and-hold.
    std::string vad_model;
    float       vad_threshold      = 0.5f;
    int         vad_min_speech_ms  = 250;
    int         vad_min_silence_ms = 700;

    // Conversation turns replayed as context. voice_assistant.py kept 5.
    int memory_turns = 5;

    // Palette name, shared by the GTK stylesheet and the web page so one setting
    // themes both front ends. Key is "auspex_theme" (was "magi_theme").
    std::string theme = "Plain";

    // Browser control. Empty browser means "let xdg-open pick".
    std::string browser;
    std::string search_url = "https://duckduckgo.com/?q=";

    // A shell pipeline that reads text on stdin and produces audible sound.
    // Keeping it a full pipeline (engine | player) rather than just the engine
    // means the sample rate and output device are config, not compiled-in
    // guesses, and swapping piper for espeak-ng needs no rebuild.
    std::string tts_command;

    // $XDG_CONFIG_HOME/auspex/config.json, else ~/.config/auspex/config.json.
    // Falls back to the pre-rename .../magi/config.json if only that exists.
    static std::filesystem::path default_path();

    // Private per-user scratch space: $XDG_RUNTIME_DIR/auspex.
    static std::filesystem::path runtime_dir();

    // Fills any empty command field by probing PATH. Called once after load().
    void resolve_commands();

    // Missing file or unreadable JSON yields defaults rather than throwing —
    // a shell that refuses to start because of a typo'd config is worse than
    // one that starts with defaults and says so.
    static Config load(const std::filesystem::path& path = default_path());
};

}  // namespace auspex
