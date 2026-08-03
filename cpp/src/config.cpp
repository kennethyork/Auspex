#include "auspex/config.hpp"

#include <algorithm>

#include "auspex/process.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace auspex {

fs::path Config::default_path() {
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = fs::path(xdg);
    } else if (const char* home = std::getenv("HOME"); home && *home) {
        base = fs::path(home) / ".config";
    } else {
        base = fs::path(".config");
    }

    const fs::path current = base / "auspex" / "config.json";

    // The project was called MAGI until the rename. Fall back to the old location
    // so an existing install keeps working instead of silently reverting to
    // defaults; the new path wins as soon as it exists.
    std::error_code ec;
    if (!fs::exists(current, ec)) {
        const fs::path legacy = base / "magi" / "config.json";
        if (fs::exists(legacy, ec)) return legacy;
    }
    return current;
}

fs::path Config::runtime_dir() {
    // XDG_RUNTIME_DIR is per-user and mode 0700, unlike the shared /tmp/MAGI the
    // Python used with a world-writable context file.
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        return fs::path(xdg) / "auspex";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".cache" / "auspex";
    }
    return fs::temp_directory_path() / "auspex";
}

namespace {

// Pull a key only if present and of the expected type. A config written by an
// older build (or hand-edited) should degrade key-by-key, not wholesale.
template <typename T>
void assign_if(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (const json::exception& e) {
        std::cerr << "auspex: config key '" << key << "' has unexpected type, keeping default ("
                  << e.what() << ")\n";
    }
}

}  // namespace

Config Config::load(const fs::path& path) {
    Config cfg;

    std::error_code ec;
    if (!fs::exists(path, ec)) {
        std::cerr << "auspex: no config at " << path << ", using defaults\n";
        return cfg;
    }

    std::ifstream in(path);
    if (!in) {
        std::cerr << "auspex: cannot read " << path << ", using defaults\n";
        return cfg;
    }

    json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "auspex: " << path << " is not valid JSON, using defaults\n";
        return cfg;
    }

    assign_if(j, "panel_height",      cfg.panel_height);
    assign_if(j, "num_ctx",           cfg.num_ctx);
    assign_if(j, "window_opacity",    cfg.window_opacity);
    // Clamped, because 0.0 is an invisible window somebody then cannot find to
    // fix, and above 1.0 is meaningless.
    cfg.window_opacity = std::clamp(cfg.window_opacity, 0.25, 1.0);
    assign_if(j, "search_endpoint",   cfg.search_endpoint);
    assign_if(j, "workspace_count",   cfg.workspace_count);
    assign_if(j, "enable_effects",    cfg.enable_effects);
    assign_if(j, "enable_ai",         cfg.enable_ai);
    assign_if(j, "clock_24_hour",     cfg.clock_24_hour);
    assign_if(j, "grid_mode",         cfg.grid_mode);

    // Only strings, and only ones that look like an entry id. A config file is
    // hand-edited, and a stray number here should be skipped rather than turned
    // into a pin that resolves to nothing.
    if (j.contains("pinned") && j["pinned"].is_array()) {
        for (const auto& item : j["pinned"]) {
            if (!item.is_string()) continue;
            std::string id = item.get<std::string>();
            if (!id.empty()) cfg.pinned.push_back(std::move(id));
        }
    }
    // Every crew_<role>_model / crew_<role>_backend key, whatever the role is
    // called. Read generically so the config and configurable_roles() cannot drift
    // apart -- a role added to that table needs no change here at all.
    for (const auto& [key, value] : j.items()) {
        if (!value.is_string()) continue;
        const std::string text = value.get<std::string>();
        if (key.rfind("crew_", 0) != 0) continue;

        constexpr std::string_view kModel   = "_model";
        constexpr std::string_view kBackend = "_backend";
        const auto ends_with = [&key](std::string_view suffix) {
            return key.size() > suffix.size() &&
                   key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        if (ends_with(kModel)) {
            cfg.crew_role_models[key.substr(5, key.size() - 5 - kModel.size())] = text;
        } else if (ends_with(kBackend)) {
            cfg.crew_role_backends[key.substr(5, key.size() - 5 - kBackend.size())] =
                text;
        }
    }
    assign_if(j, "terminal",          cfg.terminal);
    assign_if(j, "launcher",          cfg.launcher);
    assign_if(j, "background",        cfg.background);
    assign_if(j, "ollama_model",      cfg.ollama_model);
    assign_if(j, "ollama_endpoint",   cfg.ollama_endpoint);
    assign_if(j, "whisper_endpoint",  cfg.whisper_endpoint);
    assign_if(j, "sample_rate",       cfg.sample_rate);
    assign_if(j, "whisper_model",     cfg.whisper_model);
    assign_if(j, "asr_threads",       cfg.asr_threads);
    assign_if(j, "asr_language",      cfg.asr_language);
    assign_if(j, "default_microphone", cfg.default_microphone);
    assign_if(j, "vad_model",         cfg.vad_model);
    assign_if(j, "vad_threshold",     cfg.vad_threshold);
    assign_if(j, "vad_min_speech_ms", cfg.vad_min_speech_ms);
    assign_if(j, "vad_min_silence_ms", cfg.vad_min_silence_ms);
    assign_if(j, "memory_turns",      cfg.memory_turns);
    assign_if(j, "auspex_theme",      cfg.theme);
    assign_if(j, "browser",           cfg.browser);
    assign_if(j, "search_url",        cfg.search_url);
    assign_if(j, "settings_command",  cfg.settings_command);
    assign_if(j, "network_command",   cfg.network_command);
    assign_if(j, "tts_command",       cfg.tts_command);

    cfg.resolve_commands();
    return cfg;
}

void Config::resolve_commands() {
    // Ordered by likelihood on a given desktop, most specific first. A blank
    // result leaves the corresponding panel button out rather than wiring a
    // button to a binary that is not installed.
    if (terminal.empty()) {
        terminal = first_in_path({"xfce4-terminal", "gnome-terminal", "konsole",
                                  "mate-terminal", "lxterminal", "kitty", "alacritty",
                                  "foot", "xterm"});
    }
    if (launcher.empty()) {
        launcher = first_in_path({"xfce4-appfinder --collapsed", "rofi -show drun",
                                  "wofi --show drun", "dmenu_run", "krunner",
                                  "gnome-shell", "synapse", "albert"});
    }
    if (settings_command.empty()) {
        settings_command = first_in_path({"xfce4-settings-manager", "gnome-control-center",
                                          "systemsettings", "systemsettings5",
                                          "mate-control-center", "cinnamon-settings",
                                          "lxqt-config"});
    }
    if (browser.empty()) {
        // xdg-open is preferred over naming a browser, so the user's own default
        // wins; this list is only a fallback for a machine with no xdg-utils.
        browser = first_in_path({"xdg-open", "firefox", "chromium", "google-chrome",
                                 "brave-browser", "epiphany", "falkon"});
    }
    if (network_command.empty()) {
        network_command = first_in_path({"nm-connection-editor", "nmtui",
                                         "plasma-nm", "cinnamon-settings network"});
    }
}

}  // namespace auspex
