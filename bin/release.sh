#!/bin/bash
#
# Builds redistributable Auspex tarballs, one per x86-64 capability level.
#
# Why more than one artifact: ggml defaults to -march=native, and even with that
# off there is no single x86_64 binary that is both correct on a 2005 Athlon 64 and
# fast on a 2024 Zen 5. The standard microarchitecture levels are the accepted
# compromise -- build one per level and let the user pick:
#
#   v1  SSE2                              every x86_64 CPU ever made
#   v2  + SSE4.2/POPCNT/SSSE3             Nehalem / Bulldozer, ~2009+
#   v3  + AVX/AVX2/BMI/FMA/F16C           Haswell / Excavator, ~2013+
#   v4  + AVX-512                         Skylake-X / Zen 4, ~2017+
#
# A binary built for level N runs on N and above, and SIGILLs below it. Users run
# bin/detect-arch.sh to find their level.
#
# GPU variants are built at v3 and up only: a machine with a Vulkan driver is
# essentially always v3-capable, and pairing a GPU build with v1 would ship a
# large artifact almost nobody needs.
#
set -euo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; BLUE='\033[0;34m'
YELLOW='\033[0;33m'; NC='\033[0m'
info() { echo -e "${BLUE}$*${NC}"; }
good() { echo -e "${GREEN}$*${NC}"; }
warn() { echo -e "${YELLOW}$*${NC}"; }
fail() { echo -e "${RED}$*${NC}"; }

ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
OUT="${AUSPEX_RELEASE_DIR:-$ROOT/dist}"
VERSION="${AUSPEX_VERSION:-$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo dev)}"

# CPU levels always; GPU variants only where glslc exists to compile the shaders.
LEVELS="${AUSPEX_LEVELS:-v1 v2 v3 v4}"
GPU_LEVELS="${AUSPEX_GPU_LEVELS:-v3 v4}"

if ! command -v glslc >/dev/null 2>&1; then
    warn "glslc not found — skipping GPU variants (install glslc / shaderc for them)."
    GPU_LEVELS=""
fi

mkdir -p "$OUT"
info "Auspex $VERSION -> $OUT"

# ---------------------------------------------------------------------------
build_variant() {
    local level="$1" backend="$2"
    local name="auspex-x86-64-${level}-${backend}"
    local dir="$ROOT/cpp/build-rel-${level}-${backend}"

    info "\n== $name =="

    # A fresh directory per variant: ggml caches its ISA decisions, so reusing one
    # would silently emit the previous level's instructions.
    rm -rf "$dir"
    cmake -S "$ROOT/cpp" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DAUSPEX_ARCH_LEVEL="$level" \
        -DAUSPEX_WHISPER_BACKEND="$backend" \
        -DAUSPEX_BUILD_SHELL=ON >/dev/null

    cmake --build "$dir" --parallel >/dev/null
    good "built"

    # Verify the binary really is limited to the requested level. objdump is the
    # only honest check: a wrong -march silently produces a binary that works here
    # and SIGILLs on the target.
    if command -v objdump >/dev/null 2>&1; then
        local bad=""
        case "$level" in
            v1) bad='vaddp|vmulp|vfmadd|vpxor|popcnt|pcmpistr|blendv' ;;
            v2) bad='vaddp|vmulp|vfmadd|vpxor' ;;
            v3) bad='zmm' ;;
            v4) bad='' ;;
        esac
        if [ -n "$bad" ]; then
            local hits
            hits=$(objdump -d --no-show-raw-insn "$dir/auspex-listen" 2>/dev/null \
                   | grep -cE "[[:space:]](${bad})" || true)
            if [ "${hits:-0}" -gt 0 ]; then
                fail "  $name: found $hits instruction(s) above $level — NOT shipping this"
                return 1
            fi
            good "  verified: no instructions above x86-64-$level"
        fi
    else
        warn "  objdump missing; cannot verify the ISA level"
    fi

    # ---- stage ----
    local stage="$OUT/$name"
    rm -rf "$stage"; mkdir -p "$stage/bin"

    for b in auspex-shell auspex-probe auspex-listen auspex-say auspex-selftest; do
        [ -f "$dir/$b" ] && cp "$dir/$b" "$stage/bin/"
    done
    cp "$ROOT/bin/setup.sh" "$ROOT/bin/detect-arch.sh" "$stage/bin/"
    cp "$ROOT/LICENSE" "$stage/" 2>/dev/null || true

    cat > "$stage/README" <<EOF
Auspex $VERSION — x86-64-$level, $backend backend

Built for x86-64-$level. Runs on that level and above; it will crash with
SIGILL on an older CPU. Check yours with:

    ./bin/detect-arch.sh

Requirements
  X11 session          the panel docks via EWMH struts; Wayland is unsupported
  xdotool wmctrl       window/workspace control
  xprop xwininfo       docking
  xclip                selection reading
  xdg-utils            opening files and folders
  ollama               language model, listening on 127.0.0.1:11434
  piper or espeak-ng   speech output
$( [ "$backend" = vulkan ] && echo "  Vulkan driver        GPU speech recognition" )

Models are not bundled (they are hundreds of MB and change independently).
Fetch them and write a config with:

    ./bin/setup.sh

Then start the panel:

    ./bin/auspex-shell

The CLI tools work without a display:
    ./bin/auspex-probe                     language model status
    ./bin/auspex-probe command "open my downloads"   interpret, do not execute
    ./bin/auspex-listen --mic 5            record and transcribe
    ./bin/auspex-say "hello"               speak
    ./bin/auspex-selftest                  self-checks
EOF

    ( cd "$OUT" && tar czf "$name.tar.gz" "$name" && rm -rf "$name" )
    good "  packaged $(du -h "$OUT/$name.tar.gz" | cut -f1)  $name.tar.gz"
}

# ---------------------------------------------------------------------------
FAILED=""
for level in $LEVELS; do
    build_variant "$level" cpu || FAILED="$FAILED x86-64-$level-cpu"
done
for level in $GPU_LEVELS; do
    build_variant "$level" vulkan || FAILED="$FAILED x86-64-$level-vulkan"
done

echo
if [ -n "$FAILED" ]; then
    fail "Failed variants:$FAILED"
fi

info "Artifacts:"
ls -1sh "$OUT"/*.tar.gz 2>/dev/null || warn "  none produced"

( cd "$OUT" && sha256sum ./*.tar.gz > SHA256SUMS 2>/dev/null && good "\nWrote $OUT/SHA256SUMS" )

[ -z "$FAILED" ] || exit 1
