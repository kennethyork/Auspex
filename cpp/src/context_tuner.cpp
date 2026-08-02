#include "auspex/context_tuner.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "auspex/sysmon.hpp"

namespace auspex {

namespace {

std::uint64_t ram_total_bytes() {
    // The same file the panel's RAM meter reads. One source, so the two can never
    // disagree about how much memory this machine has.
    std::ifstream in("/proc/meminfo");
    if (!in) return 0;

    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (in >> key >> value >> unit) {
        if (key == "MemTotal:") return value * 1024;   // reported in kB
    }
    return 0;
}

std::string human_bytes(std::uint64_t bytes) {
    std::ostringstream out;
    out.precision(1);
    if (bytes >= (1ULL << 30)) {
        out << std::fixed << (static_cast<double>(bytes) / (1ULL << 30)) << "GB";
    } else if (bytes >= (1ULL << 20)) {
        out << std::fixed << (static_cast<double>(bytes) / (1ULL << 20)) << "MB";
    } else {
        out << bytes << "B";
    }
    return out.str();
}

}  // namespace

MachineMemory machine_memory() {
    MachineMemory memory;
    memory.ram_bytes = ram_total_bytes();

    // NVML through the panel's own monitor rather than shelling out to
    // nvidia-smi: it is already linked, already handles the no-GPU case, and a
    // second implementation is a second thing that can be wrong.
    SystemMonitor monitor;
    if (const auto gpu = monitor.gpu_stats()) memory.vram_bytes = gpu->vram_total_bytes;
    return memory;
}

int suggested_context(const MachineMemory& memory) {
    // Floor and ceiling first, so every path below lands between them.
    constexpr int kFloor = 8'192;
    constexpr int kCeiling = 131'072;

    if (!memory.known()) return kFloor;   // we cannot tell; take the safe end

    // The KV cache is what the context costs, and it lives in VRAM when there is a
    // GPU. Sizing against RAM on a GPU machine would promise a window the card
    // cannot hold.
    const std::uint64_t budget =
        memory.vram_bytes > 0 ? memory.vram_bytes : memory.ram_bytes;

    // Roughly a gigabyte per 8k of context, which is the right order for a 7-14B
    // model at the quantisations Ollama serves, and deliberately conservative:
    // being wrong downward costs speed, being wrong upward costs the run. Half the
    // budget, because the weights need the other half.
    const std::uint64_t gigabytes = budget / (1ULL << 30);
    std::uint64_t tokens = (gigabytes / 2) * 8'192;

    // To a power of two. Every model's context is specified that way and an odd
    // number is rounded by the server anyway -- better to round where it can be
    // read than where it cannot.
    int rounded = kFloor;
    while (rounded * 2 <= static_cast<int>(std::min<std::uint64_t>(tokens, kCeiling))) {
        rounded *= 2;
    }
    return std::clamp(rounded, kFloor, kCeiling);
}

int context_for(int configured, const MachineMemory& memory) {
    // An explicit setting wins outright, and is not clamped: somebody who has
    // written num_ctx in a config file knows something about their machine that a
    // heuristic does not, and silently overriding it would be the same bug as a
    // hardcoded lowResource -- safe for the wrong machine, and quiet about it.
    if (configured > 0) return configured;
    return suggested_context(memory);
}

std::string context_report(const MachineMemory& memory, int configured) {
    std::ostringstream out;

    if (!memory.known()) {
        out << "could not read this machine's memory; using "
            << suggested_context(memory) << " tokens";
        return out.str();
    }

    out << "RAM " << human_bytes(memory.ram_bytes);
    if (memory.vram_bytes > 0) out << ", VRAM " << human_bytes(memory.vram_bytes);
    else out << ", no GPU found";

    const int chosen = context_for(configured, memory);
    out << " -> " << chosen << " tokens";
    if (configured > 0) out << " (set in config; the machine suggests "
                            << suggested_context(memory) << ")";
    return out.str();
}

}  // namespace auspex
