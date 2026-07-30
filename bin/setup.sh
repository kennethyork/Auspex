#!/bin/bash
#
# Auspex setup — builds the native shell on any Linux distribution.
#
# Portability notes, so expectations are right:
#
#   * The C++ core (config, ollama, ASR, TTS, audio, commands) is plain POSIX
#     plus libcurl and vendored headers. It builds anywhere with a C++20 compiler.
#   * The PANEL requires an X11 session. That is a display-server limit, not a
#     distro one: it docks with _NET_WM_WINDOW_TYPE_DOCK and
#     _NET_WM_STRUT_PARTIAL, which is EWMH, so any compliant X11 window manager
#     works (xfwm4, marco, openbox, i3, KWin-X11, Mutter-X11). Wayland is not
#     supported; that would need gtk4-layer-shell instead.
#   * Desktop tools (terminal, launcher, settings, network) are detected at
#     RUNTIME from PATH, so nothing here is tied to Xfce. See
#     Config::resolve_commands().
#   * GPU ASR needs only glslc (Debian/Fedora: 'glslc'; Arch/SUSE/Alpine/Void:
#     'shaderc'). SPIRV-Headers, which ggml-vulkan also requires, is fetched by
#     CMake rather than installed, since its package name differs per distro and
#     it ships nowhere by default. Nothing else is needed for the Vulkan path.
#   * Only the apt path below has been exercised on real hardware. The dnf,
#     pacman, zypper and apk package names are best effort; if one is wrong the
#     script says exactly which packages to install rather than failing silently.
#
set -euo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; BLUE='\033[0;34m'
YELLOW='\033[0;33m'; NC='\033[0m'

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"

info() { echo -e "${BLUE}$*${NC}"; }
good() { echo -e "${GREEN}$*${NC}"; }
warn() { echo -e "${YELLOW}$*${NC}"; }
fail() { echo -e "${RED}$*${NC}"; }

echo -e "${BLUE}Setting up Auspex...${NC}"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
info "\n== Preflight =="

if [ "$(id -u)" -eq 0 ]; then
    fail "Do not run as root; sudo is used only for the package install."
    exit 1
fi

DISTRO_NAME="unknown"
DISTRO_ID=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_NAME="${PRETTY_NAME:-${NAME:-unknown}}"
    DISTRO_ID="${ID:-}"
fi
good "Distro: $DISTRO_NAME"

# Build-only mode still works headless (CI, containers, remote builds); only the
# panel needs a display.
HEADLESS=0
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    HEADLESS=1
    warn "No display detected — will build, but the panel cannot be run here."
elif [ "${XDG_SESSION_TYPE:-x11}" != "x11" ]; then
    warn "Session is '${XDG_SESSION_TYPE}', not x11."
    warn "The CLI tools work, but the panel cannot dock under Wayland."
else
    good "Session: x11"
    good "Desktop: ${XDG_CURRENT_DESKTOP:-unknown} (tools are detected at runtime)"
fi

if command -v nvidia-smi >/dev/null 2>&1; then
    good "GPU: $(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader | head -1 || true)"
else
    warn "No NVIDIA GPU detected; ASR will run on CPU."
fi

# ---------------------------------------------------------------------------
# Package manager detection
# ---------------------------------------------------------------------------
info "\n== Dependencies =="

PM=""
for candidate in apt-get dnf pacman zypper apk xbps-install; do
    if command -v "$candidate" >/dev/null 2>&1; then PM="$candidate"; break; fi
done

# Package sets per manager. Roles, in order:
#   compiler, cmake, ninja, pkg-config, curl-dev, json-dev, gtk4-dev, gtkmm-dev,
#   adwaita-dev, glslc (SPIR-V compiler from shaderc -- NOT glslang-tools, which
#   ships glslangValidator; whisper.cpp execs "glslc" specifically), vulkan-dev,
#   espeak-ng,
#   xdotool, wmctrl, xprop/xwininfo, xclip, xdg-utils
case "$PM" in
    apt-get)
        PKGS=(build-essential cmake ninja-build pkg-config libcurl4-openssl-dev
              nlohmann-json3-dev libgtk-4-dev libgtkmm-4.0-dev libadwaita-1-dev
              glslc libvulkan-dev espeak-ng xdotool wmctrl x11-utils xclip
              xdg-utils)
        INSTALL=(sudo apt-get install -y)
        REFRESH=(sudo apt-get update)
        ;;
    dnf)
        PKGS=(gcc-c++ cmake ninja-build pkgconf-pkg-config libcurl-devel json-devel
              gtk4-devel gtkmm4.0-devel libadwaita-devel glslc vulkan-loader-devel
              espeak-ng xdotool wmctrl xorg-x11-utils xclip xdg-utils)
        INSTALL=(sudo dnf install -y)
        REFRESH=(true)
        ;;
    pacman)
        PKGS=(base-devel cmake ninja pkgconf curl nlohmann-json gtk4 gtkmm-4.0
              libadwaita shaderc vulkan-icd-loader espeak-ng xdotool wmctrl
              xorg-xprop xorg-xwininfo xclip xdg-utils)
        INSTALL=(sudo pacman -S --needed --noconfirm)
        REFRESH=(sudo pacman -Sy)
        ;;
    zypper)
        PKGS=(gcc-c++ cmake ninja pkg-config libcurl-devel nlohmann_json-devel
              gtk4-devel gtkmm4-devel libadwaita-devel shaderc vulkan-devel
              espeak-ng xdotool wmctrl xprop xclip xdg-utils)
        INSTALL=(sudo zypper install -y)
        REFRESH=(sudo zypper refresh)
        ;;
    apk)
        PKGS=(build-base cmake samurai pkgconf curl-dev nlohmann-json gtk4.0-dev
              gtkmm4-dev libadwaita-dev shaderc vulkan-loader-dev espeak-ng xdotool
              wmctrl xprop xclip xdg-utils)
        INSTALL=(sudo apk add)
        REFRESH=(sudo apk update)
        ;;
    xbps-install)
        PKGS=(base-devel cmake ninja pkg-config libcurl-devel nlohmann_json
              gtk4-devel gtkmm4-devel libadwaita-devel shaderc Vulkan-Loader-devel
              espeak-ng xdotool wmctrl xprop xclip xdg-utils)
        INSTALL=(sudo xbps-install -y)
        REFRESH=(sudo xbps-install -S)
        ;;
    *)
        PKGS=()
        warn "No supported package manager found."
        ;;
esac

if [ ${#PKGS[@]} -eq 0 ]; then
    warn "Install these yourself, then re-run:"
    warn "  a C++20 compiler, cmake, ninja, pkg-config"
    warn "  dev packages: libcurl, nlohmann-json, gtk4, gtkmm-4.0, libadwaita"
    warn "  runtime: xdotool, wmctrl, xprop, xwininfo, xclip, xdg-utils, espeak-ng"
    warn "  optional (GPU ASR): glslc (from shaderc), Vulkan loader"
elif [ "${AUSPEX_SKIP_DEPS:-0}" = "1" ]; then
    info "AUSPEX_SKIP_DEPS=1 — skipping the package install."
else
    info "Using $PM to install ${#PKGS[@]} packages..."
    "${REFRESH[@]}" >/dev/null 2>&1 || warn "Repository refresh failed; continuing."
    if "${INSTALL[@]}" "${PKGS[@]}"; then
        good "Dependencies installed"
    else
        # Individual retry: one bad package name should not block the rest, and the
        # user gets a precise list rather than one opaque failure.
        warn "Bulk install failed; retrying individually to isolate it..."
        MISSING=()
        for pkg in "${PKGS[@]}"; do
            "${INSTALL[@]}" "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
        done
        if [ ${#MISSING[@]} -gt 0 ]; then
            warn "Could not install: ${MISSING[*]}"
            warn "The names above may differ on $DISTRO_NAME. Install the equivalents"
            warn "and re-run, or set AUSPEX_SKIP_DEPS=1 if they are already present."
        fi
    fi
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
info "\n== Building =="

# The shell target needs gtkmm; without it, still build the CLI tools rather than
# failing outright.
BUILD_SHELL=OFF
if pkg-config --exists gtkmm-4.0 libadwaita-1 2>/dev/null; then
    BUILD_SHELL=ON
    good "gtkmm-4.0 $(pkg-config --modversion gtkmm-4.0), libadwaita $(pkg-config --modversion libadwaita-1)"
else
    warn "gtkmm-4.0 / libadwaita-1 not found — building CLI tools only (no panel)."
fi

GENERATOR=()
command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
command -v samu  >/dev/null 2>&1 && [ ${#GENERATOR[@]} -eq 0 ] && GENERATOR=(-G Ninja)

cmake -S "$SCRIPT_DIR/cpp" -B "$SCRIPT_DIR/cpp/build" "${GENERATOR[@]}" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DAUSPEX_BUILD_SHELL="$BUILD_SHELL"
cmake --build "$SCRIPT_DIR/cpp/build" --parallel
good "Built into $SCRIPT_DIR/cpp/build"

# ---------------------------------------------------------------------------
# Directories
# ---------------------------------------------------------------------------
info "\n== Directories =="
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/auspex"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/auspex"
mkdir -p "$CONFIG_DIR" "$DATA_DIR/piper" "$DATA_DIR/whisper"
mkdir -p "${XDG_CACHE_HOME:-$HOME/.cache}/auspex"
good "Config: $CONFIG_DIR"

# ---------------------------------------------------------------------------
# Piper TTS
# ---------------------------------------------------------------------------
info "\n== Text to speech =="
PIPER_VOICE="$DATA_DIR/piper/en_US-lessac-medium.onnx"

if [ ! -f "$PIPER_VOICE" ] && command -v curl >/dev/null 2>&1; then
    info "Downloading en_US-lessac-medium voice..."
    BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium"
    if curl -fL --retry 3 -o "$PIPER_VOICE" "$BASE/en_US-lessac-medium.onnx" &&
       curl -fL --retry 3 -o "$PIPER_VOICE.json" "$BASE/en_US-lessac-medium.onnx.json"; then
        good "Voice installed"
    else
        rm -f "$PIPER_VOICE" "$PIPER_VOICE.json"
        warn "Voice download failed"
    fi
elif [ -f "$PIPER_VOICE" ]; then
    info "Voice already present"
fi

# The player's sample rate must match the voice, and only the voice's own json
# knows it. Hardcoding 22050 would pitch-shift any other voice.
TTS_RATE=22050
if [ -f "$PIPER_VOICE.json" ]; then
    # head -1 SIGPIPEs sed; same pipefail hazard as above.
    DETECTED=$(sed -n 's/.*"sample_rate"[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p' \
               "$PIPER_VOICE.json" | head -1 || true)
    [ -n "$DETECTED" ] && TTS_RATE="$DETECTED"
fi

# Audio players differ by stack: PipeWire, PulseAudio, bare ALSA. Detect rather
# than assume, and note the differing raw-format flags.
TTS_COMMAND=""
if command -v piper >/dev/null 2>&1 && [ -f "$PIPER_VOICE" ]; then
    if command -v pw-play >/dev/null 2>&1; then
        PLAYER="pw-play --rate=$TTS_RATE --channels=1 --format=s16 -"
    elif command -v paplay >/dev/null 2>&1; then
        PLAYER="paplay --raw --rate=$TTS_RATE --channels=1 --format=s16le"
    elif command -v aplay >/dev/null 2>&1; then
        PLAYER="aplay -q -r $TTS_RATE -c 1 -f S16_LE -t raw"
    else
        PLAYER=""
    fi

    if [ -n "$PLAYER" ]; then
        TTS_COMMAND="piper --model $PIPER_VOICE --output-raw | $PLAYER"
        good "TTS: piper -> ${PLAYER%% *} (${TTS_RATE}Hz)"
    else
        warn "No audio player found (pw-play/paplay/aplay); TTS disabled."
    fi
elif command -v espeak-ng >/dev/null 2>&1; then
    # espeak-ng plays directly, so it needs no player and no rate matching.
    TTS_COMMAND="espeak-ng --stdin"
    warn "piper unavailable; falling back to espeak-ng."
else
    warn "No TTS engine found. Install piper (pipx install piper-tts) or espeak-ng."
fi

# ---------------------------------------------------------------------------
# Whisper model
# ---------------------------------------------------------------------------
info "\n== Speech recognition =="
# small.en by default. Measured here, 11s of speech, best of 3 runs, inference
# only (model load excluded). CPU = Ryzen 5 2600X, GPU = RTX 3090 via Vulkan:
#
#   model                    size    CPU ms   CPU rt    GPU ms   GPU rt   speedup
#   ggml-base.en.bin         142M      9148    1.20x       103   106.8x      89x
#   ggml-small.en.bin        466M     24239    0.45x       163    67.5x     149x
#   ggml-large-v3-turbo.bin  1.6G     73604    0.15x       198    55.6x     372x
#
# All three transcribed the reference clip correctly. Note whisper's naming is
# counterintuitive -- tiny < base < small < medium < large -- so "small" is bigger
# and slower than "base".
#
# On CPU, small.en is the accuracy/latency compromise and turbo is unusable at 74s
# for 11s of audio. On a GPU the picture inverts: turbo costs only 35ms more than
# small, so if auspex-listen --backends reports Vulkan or CUDA, prefer
#   AUSPEX_WHISPER_MODEL=ggml-large-v3-turbo.bin bin/setup.sh
# Override with AUSPEX_WHISPER_MODEL; drop the .en suffix for multilingual.
WHISPER_NAME="${AUSPEX_WHISPER_MODEL:-ggml-small.en.bin}"
WHISPER_MODEL="$DATA_DIR/whisper/$WHISPER_NAME"

if [ ! -f "$WHISPER_MODEL" ] && command -v curl >/dev/null 2>&1; then
    info "Downloading $WHISPER_NAME..."
    if curl -fL --retry 3 -o "$WHISPER_MODEL" \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$WHISPER_NAME"; then
        good "Model installed"
    else
        rm -f "$WHISPER_MODEL"
        warn "Download failed. Retry, or pick a different model:"
        warn "  AUSPEX_WHISPER_MODEL=ggml-base.en.bin bin/setup.sh   (smaller)"
        warn "  AUSPEX_WHISPER_MODEL=ggml-large-v3-turbo.bin bin/setup.sh  (best)"
    fi
elif [ -f "$WHISPER_MODEL" ]; then
    info "Model already present ($(du -h "$WHISPER_MODEL" | cut -f1))"
fi

# ---------------------------------------------------------------------------
# Voice activity detection
# ---------------------------------------------------------------------------
# Enables continuous listening. 865KB, and it runs on CPU by design -- the Silero
# graph contains ops the Vulkan backend aborts on.
info "\n== Voice activity detection =="
VAD_DIR="$DATA_DIR/vad"
VAD_MODEL="$VAD_DIR/ggml-silero-v5.1.2.bin"
mkdir -p "$VAD_DIR"

if [ ! -f "$VAD_MODEL" ] && command -v curl >/dev/null 2>&1; then
    if curl -fL --retry 3 -o "$VAD_MODEL" \
        "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin"; then
        good "VAD model installed"
    else
        rm -f "$VAD_MODEL"
        warn "VAD download failed; continuous listening will be unavailable"
    fi
elif [ -f "$VAD_MODEL" ]; then
    info "VAD model already present"
fi

# ---------------------------------------------------------------------------
# Microphone
# ---------------------------------------------------------------------------
# Left empty unless there is exactly one obvious choice: writing a guess into the
# config is worse than falling back to the system default, because a wrong value
# silently records from the wrong device.
DEFAULT_MIC=""
if [ -x "$SCRIPT_DIR/cpp/build/auspex-listen" ]; then
    info "\n== Input devices =="
    "$SCRIPT_DIR/cpp/build/auspex-listen" --devices 2>/dev/null | sed 's/^/  /' || true
    warn "Set \"default_microphone\" in config.json to a substring of the one you want."
    warn "Devices named \"Monitor of ...\" are loopbacks: they record system audio,"
    warn "not your voice."
fi

# ---------------------------------------------------------------------------
# Ollama
# ---------------------------------------------------------------------------
info "\n== Language model =="
OLLAMA_MODEL="qwen3.5:9b"
if command -v ollama >/dev/null 2>&1; then
    good "ollama: $(command -v ollama)"
    if curl -sf --max-time 3 http://127.0.0.1:11434/api/version >/dev/null 2>&1; then
        # Prefer a model that is actually pulled over a hardcoded guess.
        # `awk ... exit` SIGPIPEs ollama, which pipefail turns into a failed
        # assignment and set -e turns into a silent exit. Hence || true.
        FIRST=$(ollama list 2>/dev/null | awk 'NR>1 && $1 != "" {print $1; exit}' || true)
        for candidate in qwen3.5:9b gpt-oss:latest "$FIRST"; do
            if [ -n "$candidate" ] && ollama list 2>/dev/null | awk '{print $1}' | grep -qx "$candidate"; then
                OLLAMA_MODEL="$candidate"; break
            fi
        done
        good "Using model: $OLLAMA_MODEL"
    else
        warn "ollama installed but not responding on :11434 — run 'ollama serve'"
    fi
else
    warn "ollama not found; the LLM and voice-command features need it."
    warn "See https://ollama.com/download/linux (not piping it into your shell for you)"
fi

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
info "\n== Config =="
CONFIG_FILE="$CONFIG_DIR/config.json"

if [ -f "$CONFIG_FILE" ]; then
    cp "$CONFIG_FILE" "$CONFIG_FILE.bak"
    warn "Existing config backed up to config.json.bak"
fi

# terminal / launcher / settings_command / network_command are deliberately
# omitted: empty means Config::resolve_commands() probes PATH at startup, which is
# what makes one config portable across desktops.
cat > "$CONFIG_FILE" << EOL
{
    "panel_height": 28,
    "workspace_count": 4,
    "enable_effects": true,
    "enable_ai": true,
    "auspex_theme": "Tokyo Night",
    "ollama_model": "${OLLAMA_MODEL}",
    "ollama_endpoint": "http://127.0.0.1:11434",
    "whisper_model": "${WHISPER_MODEL}",
    "vad_model": "${VAD_MODEL}",
    "vad_threshold": 0.5,
    "vad_min_speech_ms": 250,
    "vad_min_silence_ms": 700,
    "default_microphone": "${DEFAULT_MIC}",
    "memory_turns": 5,
    "asr_threads": 0,
    "asr_language": "en",
    "sample_rate": 16000,
    "tts_command": "${TTS_COMMAND}"
}
EOL
good "Wrote $CONFIG_FILE"

# ---------------------------------------------------------------------------
# Desktop entry
# ---------------------------------------------------------------------------
# Deliberately NOT installing an xsession entry. Auspex runs as an application
# inside your existing session; it does not replace your window manager, so a
# crash costs you a panel rather than a desktop.
APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS_DIR"
cat > "$APPS_DIR/auspex.desktop" << EOL
[Desktop Entry]
Name=Auspex
Comment=Local AI desktop shell
Exec=${SCRIPT_DIR}/cpp/build/auspex-shell
Type=Application
Categories=System;
Icon=preferences-desktop
Terminal=false
EOL
good "Installed $APPS_DIR/auspex.desktop"

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------
info "\n== Verify =="
BUILD="$SCRIPT_DIR/cpp/build"

if "$BUILD/auspex-selftest" >/dev/null 2>&1; then
    good "Selftest passed"
else
    warn "Selftest reported failures — run $BUILD/auspex-selftest for detail"
fi

if "$BUILD/auspex-probe" >/dev/null 2>&1; then
    good "LLM  : reachable ($OLLAMA_MODEL)"
else
    warn "LLM  : not confirmed — run $BUILD/auspex-probe"
fi

ASR_BACKENDS=$("$BUILD/auspex-listen" --backends 2>/dev/null || echo unknown)
if [ -f "$WHISPER_MODEL" ]; then
    good "ASR  : model present, backends: $ASR_BACKENDS"
    case "$ASR_BACKENDS" in
        *Vulkan*|*CUDA*) : ;;
        *) warn "       CPU only. Install glslc (Debian/Fedora: glslc;"
           warn "       Arch/SUSE/Alpine/Void: shaderc), then:"
           warn "       rm -rf cpp/build && bin/setup.sh" ;;
    esac
else
    warn "ASR  : no model installed"
fi

[ -n "$TTS_COMMAND" ] && good "TTS  : configured" || warn "TTS  : not configured"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
good "Auspex setup complete."
echo
info "Binaries (cpp/build/):"
echo "  auspex-shell    the panel (X11 only)"
echo "  auspex-probe    LLM status; 'ask <q>'; 'command <utterance>' (dry run)"
echo "  auspex-listen   speech to text; --mic N to record"
echo "  auspex-say      text to speech"
echo "  auspex-selftest self-checks"
echo
if [ "$HEADLESS" = "1" ]; then
    warn "No display here — run auspex-shell from an X11 session."
else
    info "Start the panel:"
    echo "  $BUILD/auspex-shell"
fi
echo
info "Voice commands: click the centre panel button and speak, e.g."
echo "  \"open my downloads folder\"   \"switch to workspace 2\""
echo "  \"set the volume to 40\"       \"focus firefox\""
echo
warn "Still Python, not ported: settings.py, llm_menu.py, launcher.py's app grid."
