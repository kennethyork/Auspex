#include "auspex/sysmon.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <dlfcn.h>

namespace auspex {

namespace {

// Mirrors the NVML ABI. Declared here rather than including nvml.h so the build
// needs no CUDA toolkit -- only the driver's runtime library, which any machine
// with an NVIDIA GPU already has.
struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

using NvmlInit           = int (*)();
using NvmlShutdown       = int (*)();
using NvmlHandleByIndex  = int (*)(unsigned int, void**);
using NvmlGetUtilization = int (*)(void*, NvmlUtilization*);
using NvmlGetMemory      = int (*)(void*, NvmlMemory*);

}  // namespace

struct SystemMonitor::Nvml {
    void* handle = nullptr;
    void* device = nullptr;

    NvmlShutdown       shutdown        = nullptr;
    NvmlGetUtilization get_utilization = nullptr;
    NvmlGetMemory      get_memory      = nullptr;
};

SystemMonitor::SystemMonitor() {
    previous_ = read_cpu_sample();

    void* handle = ::dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return;

    // The _v2 entry points are the current ABI; fall back to the unsuffixed names
    // for older drivers.
    auto sym = [handle](const char* primary, const char* fallback) -> void* {
        if (void* s = ::dlsym(handle, primary)) return s;
        return ::dlsym(handle, fallback);
    };

    auto init      = reinterpret_cast<NvmlInit>(sym("nvmlInit_v2", "nvmlInit"));
    auto by_index  = reinterpret_cast<NvmlHandleByIndex>(
        sym("nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex"));
    auto util      = reinterpret_cast<NvmlGetUtilization>(
        ::dlsym(handle, "nvmlDeviceGetUtilizationRates"));
    auto memory    = reinterpret_cast<NvmlGetMemory>(
        ::dlsym(handle, "nvmlDeviceGetMemoryInfo"));
    auto shutdown  = reinterpret_cast<NvmlShutdown>(::dlsym(handle, "nvmlShutdown"));

    if (!init || !by_index || !util || !memory) {
        ::dlclose(handle);
        return;
    }

    if (init() != 0) {
        ::dlclose(handle);
        return;
    }

    void* device = nullptr;
    if (by_index(0, &device) != 0 || !device) {
        if (shutdown) shutdown();
        ::dlclose(handle);
        return;
    }

    nvml_ = new Nvml{handle, device, shutdown, util, memory};
}

SystemMonitor::~SystemMonitor() {
    if (!nvml_) return;
    if (nvml_->shutdown) nvml_->shutdown();
    if (nvml_->handle) ::dlclose(nvml_->handle);
    delete nvml_;
}

bool SystemMonitor::has_gpu() const { return nvml_ != nullptr; }

// /proc/stat's first line:
//   cpu user nice system idle iowait irq softirq steal guest guest_nice
SystemMonitor::CpuSample SystemMonitor::read_cpu_sample() {
    CpuSample sample;

    std::ifstream in("/proc/stat");
    if (!in) return sample;

    std::string label;
    in >> label;
    if (label != "cpu") return sample;

    unsigned long long value = 0;
    unsigned long long total = 0;
    unsigned long long idle_total = 0;
    for (int field = 0; in >> value; ++field) {
        total += value;
        // Fields 3 (idle) and 4 (iowait) both count as not-working.
        if (field == 3 || field == 4) idle_total += value;
        if (field >= 9) break;
    }

    sample.idle  = idle_total;
    sample.total = total;
    sample.valid = total > 0;
    return sample;
}

double SystemMonitor::cpu_percent() {
    const CpuSample now = read_cpu_sample();
    if (!now.valid || !previous_.valid) {
        previous_ = now;
        return 0.0;
    }

    const auto total_delta = now.total - previous_.total;
    const auto idle_delta  = now.idle - previous_.idle;
    previous_ = now;

    if (total_delta == 0) return 0.0;

    const double busy = static_cast<double>(total_delta - idle_delta);
    const double load = busy / static_cast<double>(total_delta) * 100.0;
    return load < 0.0 ? 0.0 : (load > 100.0 ? 100.0 : load);
}

double SystemMonitor::ram_percent() {
    std::ifstream in("/proc/meminfo");
    if (!in) return 0.0;

    unsigned long long total = 0;
    unsigned long long available = 0;

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string key;
        unsigned long long value = 0;
        if (!(fields >> key >> value)) continue;

        if (key == "MemTotal:") total = value;
        else if (key == "MemAvailable:") available = value;

        if (total && available) break;
    }

    if (total == 0) return 0.0;
    if (available > total) available = total;

    return static_cast<double>(total - available) / static_cast<double>(total) * 100.0;
}

std::optional<GpuStats> SystemMonitor::gpu_stats() {
    if (!nvml_) return std::nullopt;

    NvmlUtilization utilization{};
    NvmlMemory memory{};

    // system.py swallowed NVML errors and reported 0 for both. Reporting nullopt
    // instead lets the caller distinguish "GPU idle" from "GPU unreadable".
    if (nvml_->get_utilization(nvml_->device, &utilization) != 0) return std::nullopt;
    if (nvml_->get_memory(nvml_->device, &memory) != 0) return std::nullopt;
    if (memory.total == 0) return std::nullopt;

    GpuStats stats;
    stats.utilization_percent = static_cast<double>(utilization.gpu);
    stats.vram_used_bytes     = memory.used;
    stats.vram_total_bytes    = memory.total;
    stats.vram_percent =
        static_cast<double>(memory.used) / static_cast<double>(memory.total) * 100.0;
    return stats;
}

std::string SystemMonitor::format_label() {
    const double cpu = cpu_percent();
    const double ram = ram_percent();

    std::array<char, 160> buffer{};

    if (const auto gpu = gpu_stats()) {
        // Field widths match system.py's f"{value:>5.1f}".
        std::snprintf(buffer.data(), buffer.size(),
                      "CPU: %5.1f%% | RAM: %5.1f%% | GPU: %5.1f%% | VRAM: %5.1f%%",
                      cpu, ram, gpu->utilization_percent, gpu->vram_percent);
    } else {
        std::snprintf(buffer.data(), buffer.size(),
                      "CPU: %5.1f%% | RAM: %5.1f%%", cpu, ram);
    }

    return std::string(buffer.data());
}

}  // namespace auspex
