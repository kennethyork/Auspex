// System resource monitoring.
//
// Port of src/magi_shell/widgets/system.py, replacing its two Python dependencies:
//   psutil  -> /proc/stat and /proc/meminfo
//   pynvml  -> libnvidia-ml.so.1 loaded with dlopen
//
// pynvml was never installed by upstream's setup.sh (it went into the whisper
// venv, not the interpreter running the shell), and system.py imports it at module
// level. panel.py imports SystemMonitor at module level in turn, so on a stock
// install the import chain failed and the whole panel died before drawing. Loading
// NVML dynamically means a machine with no NVIDIA driver degrades to CPU+RAM
// exactly as system.py intended, instead of taking the shell down.
#pragma once

#include <optional>
#include <string>

namespace auspex {

struct GpuStats {
    double utilization_percent = 0.0;
    double vram_percent        = 0.0;
    unsigned long long vram_used_bytes  = 0;
    unsigned long long vram_total_bytes = 0;
};

class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    SystemMonitor(const SystemMonitor&)            = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    // CPU load since the previous call. The first call has no previous sample to
    // difference against and returns 0.0, matching psutil.cpu_percent(interval=None).
    double cpu_percent();

    // Percentage of RAM in use: (MemTotal - MemAvailable) / MemTotal, which is what
    // psutil.virtual_memory().percent reports.
    double ram_percent();

    // nullopt when no NVIDIA GPU is usable.
    std::optional<GpuStats> gpu_stats();

    bool has_gpu() const;

    // The panel label, formatted exactly as system.py did:
    //   "CPU:  12.5% | RAM:  40.1% | GPU:   3.0% | VRAM:  15.2%"
    // GPU and VRAM are omitted when no GPU is present.
    std::string format_label();

private:
    struct CpuSample {
        unsigned long long idle  = 0;
        unsigned long long total = 0;
        bool valid = false;
    };

    static CpuSample read_cpu_sample();

    CpuSample previous_{};

    struct Nvml;
    Nvml* nvml_ = nullptr;
};

}  // namespace auspex
