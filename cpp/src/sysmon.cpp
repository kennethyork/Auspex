#include "auspex/sysmon.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <dlfcn.h>

#include "auspex/process.hpp"

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

// ---------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------
VolumeState parse_wpctl_volume(const std::string& output) {
    const auto at = output.find("Volume:");
    if (at == std::string::npos) return {};

    // A fraction, not a percentage, and it can legitimately exceed 1.0 -- PipeWire
    // allows over-amplification and reading 1.5 as 1% would be a silent bug.
    double value = 0.0;
    try {
        value = std::stod(output.substr(at + 7));
    } catch (...) {
        return {};
    }

    VolumeState state;
    state.known   = true;
    state.percent = static_cast<int>(value * 100.0 + 0.5);
    state.muted   = output.find("[MUTED]") != std::string::npos;
    return state;
}

VolumeState parse_pactl_volume(const std::string& output) {
    const auto percent = output.find('%');
    if (percent == std::string::npos) return {};

    // Backwards from the '%' to the number in front of it. Reading forwards would
    // find the raw 30801 value first, which is not a percentage at all.
    std::size_t end = percent;
    while (end > 0 && std::isspace(static_cast<unsigned char>(output[end - 1]))) --end;
    std::size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(output[begin - 1]))) --begin;
    if (begin == end) return {};

    VolumeState state;
    try {
        state.percent = std::stoi(output.substr(begin, end - begin));
    } catch (...) {
        return {};
    }
    state.known = true;
    return state;
}

bool parse_pactl_mute(const std::string& output) {
    const auto at = output.find("Mute:");
    if (at == std::string::npos) return false;
    return output.find("yes", at) != std::string::npos;
}

VolumeState current_volume() {
    // wpctl first, matching the order the spoken set_volume command uses. Two
    // different orders would mean the slider and the voice command could end up
    // addressing different sinks on a machine that has both tools.
    if (const auto result = run({"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"});
        result.ok) {
        if (const auto state = parse_wpctl_volume(result.out); state.known) return state;
    }

    if (const auto result = run({"pactl", "get-sink-volume", "@DEFAULT_SINK@"});
        result.ok) {
        VolumeState state = parse_pactl_volume(result.out);
        if (state.known) {
            // pactl reports mute separately, unlike wpctl.
            if (const auto mute = run({"pactl", "get-sink-mute", "@DEFAULT_SINK@"});
                mute.ok) {
                state.muted = parse_pactl_mute(mute.out);
            }
            return state;
        }
    }

    return {};
}

bool set_volume(int percent) {
    percent = std::clamp(percent, 0, 100);
    const std::string as_text = std::to_string(percent);
    return run({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", as_text + "%"}, false).ok ||
           run({"pactl", "set-sink-volume", "@DEFAULT_SINK@", as_text + "%"}, false).ok;
}

bool set_muted(bool muted) {
    const char* wpctl_flag = muted ? "1" : "0";
    const char* pactl_flag = muted ? "1" : "0";
    return run({"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", wpctl_flag}, false).ok ||
           run({"pactl", "set-sink-mute", "@DEFAULT_SINK@", pactl_flag}, false).ok;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
namespace {

std::vector<std::string> split_on(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::string current;
    for (const char c : line) {
        if (c == separator) {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    fields.push_back(current);
    return fields;
}

}  // namespace

NetworkState parse_nmcli_devices(const std::string& output) {
    NetworkState state;

    for (const auto& line : split_lines(output)) {
        const auto fields = split_on(line, ':');
        if (fields.size() < 4) continue;

        const std::string& type       = fields[1];
        const std::string& status     = fields[2];
        const std::string& connection = fields[3];

        // Only real, locally-managed links.
        //
        // "connected (externally)" is NetworkManager saying something else brought
        // this up -- loopback always, and virbr0 on any machine with libvirt. Taking
        // either would report the network as up with the cable unplugged.
        if (type == "loopback" || type == "bridge" || type == "wifi-p2p") continue;
        if (status != "connected") continue;

        if (type == "ethernet") {
            // Wired wins outright. A desktop with both up is using the cable.
            state.kind       = NetworkState::Kind::Wired;
            state.connection = connection;
            state.known      = true;
            return state;
        }
        if (type == "wifi" && state.kind == NetworkState::Kind::None) {
            state.kind       = NetworkState::Kind::Wireless;
            state.connection = connection;
            state.known      = true;
        }
    }

    // Nothing connected is still an answer, and a different one from "nmcli did not
    // run" -- one draws a disconnected icon, the other draws nothing at all.
    if (!output.empty()) state.known = true;
    return state;
}

int parse_nmcli_wifi_signal(const std::string& output) {
    for (const auto& line : split_lines(output)) {
        const auto fields = split_on(line, ':');
        if (fields.size() < 3) continue;
        if (fields[0].find('*') == std::string::npos) continue;
        try {
            return std::stoi(fields[2]);
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

bool parse_nmcli_connectivity(const std::string& output) {
    // "connected:full". Anything else -- limited, portal, none -- is a link without
    // the internet behind it, which is worth showing differently.
    return output.find("full") != std::string::npos;
}

NetworkState current_network() {
    const auto devices = run({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION",
                              "device", "status"});
    if (!devices.ok) return {};

    NetworkState state = parse_nmcli_devices(devices.out);
    if (!state.known) return state;

    if (const auto general = run({"nmcli", "-t", "-f", "STATE,CONNECTIVITY",
                                  "general", "status"});
        general.ok) {
        state.online = parse_nmcli_connectivity(general.out);
    }

    if (state.kind == NetworkState::Kind::Wireless) {
        if (const auto wifi = run({"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL",
                                   "device", "wifi", "list"});
            wifi.ok) {
            state.signal_percent = parse_nmcli_wifi_signal(wifi.out);
        }
    }

    return state;
}

namespace {

// nmcli's terse output escapes ':' and '\' with a backslash, so a naive split cuts
// an SSID like "my:network" in half -- and connecting to half an SSID silently
// fails against a network that does not exist.
std::vector<std::string> split_terse(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            current.push_back(line[i + 1]);
            ++i;
            continue;
        }
        if (line[i] == ':') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(line[i]);
    }
    fields.push_back(current);
    return fields;
}

}  // namespace

std::vector<WifiNetwork> parse_nmcli_wifi_list(const std::string& output) {
    std::vector<WifiNetwork> networks;

    for (const auto& line : split_lines(output)) {
        const auto fields = split_terse(line);
        if (fields.size() < 3) continue;

        WifiNetwork network;
        network.in_use = fields[0].find('*') != std::string::npos;
        network.ssid   = fields[1];
        try {
            network.signal_percent = std::stoi(fields[2]);
        } catch (...) {
            continue;
        }
        // A hidden network broadcasts an empty SSID. There is nothing to click.
        if (network.ssid.empty()) continue;
        if (fields.size() >= 4) {
            const std::string security = fields[3];
            network.secured = !security.empty() && security != "--";
        }

        // One access point per name, keeping the strongest. A mesh or a dual-band
        // router appears several times and a list repeating the same name is noise.
        const auto existing = std::find_if(
            networks.begin(), networks.end(),
            [&](const WifiNetwork& other) { return other.ssid == network.ssid; });
        if (existing != networks.end()) {
            if (network.in_use) existing->in_use = true;
            if (network.signal_percent > existing->signal_percent) {
                existing->signal_percent = network.signal_percent;
            }
            continue;
        }
        networks.push_back(std::move(network));
    }

    std::stable_sort(networks.begin(), networks.end(),
                     [](const WifiNetwork& a, const WifiNetwork& b) {
                         // The one you are on always first; then strongest.
                         if (a.in_use != b.in_use) return a.in_use;
                         return a.signal_percent > b.signal_percent;
                     });
    return networks;
}

bool wifi_radio_enabled() {
    const auto result = run({"nmcli", "radio", "wifi"});
    if (!result.ok) return false;
    return result.out.find("enabled") != std::string::npos;
}

bool set_wifi_radio(bool enabled) {
    return run({"nmcli", "radio", "wifi", enabled ? "on" : "off"}, false).ok;
}

bool has_wifi_device() {
    const auto result = run({"nmcli", "-t", "-f", "TYPE", "device", "status"});
    if (!result.ok) return false;
    for (const auto& line : split_lines(result.out)) {
        if (trim(line) == "wifi") return true;
    }
    return false;
}

bool connect_wifi(const std::string& ssid) {
    if (ssid.empty()) return false;
    // No password argument, deliberately. A saved network connects from this alone;
    // a new secured one fails, and the caller opens the system's own dialog rather
    // than this panel growing a password field and somewhere to keep what is typed
    // into it.
    return run({"nmcli", "device", "wifi", "connect", ssid}, false).ok;
}

std::vector<WifiNetwork> scan_wifi() {
    const auto result = run({"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY",
                             "device", "wifi", "list"});
    if (!result.ok) return {};
    return parse_nmcli_wifi_list(result.out);
}

std::string network_icon_name(const NetworkState& state) {
    if (!state.known || state.kind == NetworkState::Kind::None) {
        return "network-offline-symbolic";
    }

    if (state.kind == NetworkState::Kind::Wired) {
        // Connected but with no route out is its own state, and the one worth
        // seeing: it is the difference between "the cable is out" and "the internet
        // is down", which are fixed in completely different places.
        return state.online ? "network-wired-symbolic"
                            : "network-wired-no-route-symbolic";
    }

    if (!state.online) return "network-wireless-no-route-symbolic";
    if (state.signal_percent >= 75) return "network-wireless-signal-excellent-symbolic";
    if (state.signal_percent >= 50) return "network-wireless-signal-good-symbolic";
    if (state.signal_percent >= 25) return "network-wireless-signal-ok-symbolic";
    if (state.signal_percent > 0)   return "network-wireless-signal-weak-symbolic";
    return "network-wireless-signal-none-symbolic";
}

std::string volume_icon_name(const VolumeState& state) {
    // Muted is muted whatever the level -- a muted slider at 80% must not show a
    // loud speaker, which is the one thing the icon is there to tell you.
    if (state.muted || state.percent <= 0) return "audio-volume-muted-symbolic";
    if (state.percent < 34)  return "audio-volume-low-symbolic";
    if (state.percent < 67)  return "audio-volume-medium-symbolic";
    return "audio-volume-high-symbolic";
}

}  // namespace auspex
