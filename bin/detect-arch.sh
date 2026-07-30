#!/bin/sh
#
# Prints which Auspex x86_64 download this machine can run.
#
# These are the standard x86-64 microarchitecture levels (AMD/Intel, and what
# glibc's hwcaps and GCC's -march=x86-64-vN use). A binary built for a level runs
# on that level and everything above it, and dies with SIGILL below it.
#
# POSIX sh, no bash and no external tools beyond grep, so it runs on a rescue
# shell or a minimal container.
set -u

FLAGS=$(grep -m1 '^flags' /proc/cpuinfo 2>/dev/null | cut -d: -f2)
if [ -z "$FLAGS" ]; then
    echo "unknown: cannot read /proc/cpuinfo" >&2
    exit 1
fi

has() {
    for want in "$@"; do
        case " $FLAGS " in
            *" $want "*) ;;
            *) return 1 ;;
        esac
    done
    return 0
}

LEVEL=v1
has cx16 lahf_lm popcnt sse4_1 sse4_2 ssse3            && LEVEL=v2
[ "$LEVEL" = v2 ] && has avx avx2 bmi1 bmi2 f16c fma movbe xsave && LEVEL=v3
[ "$LEVEL" = v3 ] && has avx512f avx512bw avx512cd avx512dq avx512vl && LEVEL=v4

MODEL=$(grep -m1 '^model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//')

# A GPU build is only worth recommending if a Vulkan driver is actually present;
# the binary would otherwise fall back to CPU while being larger.
GPU=no
for icd in /usr/share/vulkan/icd.d/*.json; do
    [ -e "$icd" ] || continue
    case "$icd" in
        *nvidia*|*radeon*|*amd*|*intel*) GPU=yes ;;
    esac
done

echo "cpu       : ${MODEL:-unknown}"
echo "level     : x86-64-$LEVEL"
echo "vulkan    : $GPU"
if [ "$GPU" = yes ] && [ "$LEVEL" != v1 ]; then
    echo "download  : auspex-x86-64-$LEVEL-vulkan.tar.gz"
else
    echo "download  : auspex-x86-64-$LEVEL-cpu.tar.gz"
fi
