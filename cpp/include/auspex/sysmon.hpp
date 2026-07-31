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
#include <vector>

namespace auspex {

// --- volume -----------------------------------------------------------------
//
// A panel control rather than a tray icon, because the sound icon on a stock Xfce
// desktop is not a tray icon at all: it is libpulseaudio-plugin.so loaded inside
// xfce4-panel's own process. It publishes nothing on StatusNotifierItem or on
// XApp, so no tray implementation can ever adopt it -- the only way for Auspex to
// have a volume control is to have its own.
//
// Which is no loss. Auspex already drives wpctl and pactl for the spoken
// "set the volume to 40" command, so this is the same tested path with a slider on
// it instead of a sentence.

struct VolumeState {
    int  percent = 0;
    bool muted   = false;
    // False when neither tool answered, so a caller can hide the control rather
    // than show a slider that silently does nothing.
    bool known   = false;

    bool operator==(const VolumeState&) const = default;
};

// `wpctl get-volume @DEFAULT_AUDIO_SINK@` prints "Volume: 0.47", and appends
// " [MUTED]" when muted. The value is a fraction, and it can exceed 1.0.
VolumeState parse_wpctl_volume(const std::string& output);

// `pactl get-sink-volume @DEFAULT_SINK@` prints a much noisier line:
//   Volume: front-left: 30801 /  47% / -19.67 dB,  front-right: 30801 / 47% / ...
// The first percentage is taken; channels are almost always in step and a panel
// slider has one handle regardless.
VolumeState parse_pactl_volume(const std::string& output);

// `pactl get-sink-mute @DEFAULT_SINK@` prints "Mute: yes" or "Mute: no".
bool parse_pactl_mute(const std::string& output);

// Reads the current volume, preferring wpctl and falling back to pactl -- the same
// order the spoken command uses, so the two can never disagree about which sink
// they are talking to.
VolumeState current_volume();

// Sets the volume, clamped to 0-100. False when neither tool is present.
bool set_volume(int percent);

// Mutes or unmutes. False when neither tool is present.
bool set_muted(bool muted);

// The icon name for a level, matching the standard audio-volume-* names every
// theme ships.
std::string volume_icon_name(const VolumeState& state);

// --- network ----------------------------------------------------------------

struct NetworkState {
    enum class Kind { None, Wired, Wireless };

    Kind        kind = Kind::None;
    std::string connection;      // "Wired connection 1", an SSID, or empty
    int         signal_percent = 0;  // wireless only
    bool        online = false;      // has actual connectivity, not merely a link
    bool        known  = false;

    bool operator==(const NetworkState&) const = default;
};

// Parses `nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status`.
//
// Colon-separated, and the fields matter: a laptop reports several devices at once
// and only one of them is the one you are actually using. Loopback and bridges are
// skipped -- virbr0 reports "connected (externally)" on any machine with libvirt
// installed, and taking it would report the network as up while the cable is out.
NetworkState parse_nmcli_devices(const std::string& output);

// Parses `nmcli -t -f IN-USE,SSID,SIGNAL device wifi list`, returning the signal of
// the network marked in use. The in-use marker is '*'.
int parse_nmcli_wifi_signal(const std::string& output);

// True when `nmcli -t -f STATE,CONNECTIVITY general status` reports real
// connectivity rather than merely an address. "connected:full" is online;
// "connected:portal" is a hotel wifi that has not been logged into yet.
bool parse_nmcli_connectivity(const std::string& output);

NetworkState current_network();

// One network the radio can see.
struct WifiNetwork {
    std::string ssid;
    int         signal_percent = 0;
    bool        in_use  = false;
    bool        secured = false;

    bool operator==(const WifiNetwork&) const = default;
};

// Parses `nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list`.
//
// Terse output escapes the field separator, so an SSID containing a colon arrives
// as "my\:network" -- splitting naively would cut it in half and connect to a
// network that does not exist.
//
// Deduplicated by SSID keeping the strongest, because a mesh or an access point on
// two bands appears several times and a menu listing the same name four times is
// noise. Sorted strongest first; the network in use is always first of all.
std::vector<WifiNetwork> parse_nmcli_wifi_list(const std::string& output);

// Whether the radio is on. Distinct from "not connected": a disabled radio is why
// the list is empty, and it is the one thing a menu can fix by itself.
bool wifi_radio_enabled();
bool set_wifi_radio(bool enabled);

// True when the machine has a wifi device at all, so a desktop with none is not
// offered an empty list.
bool has_wifi_device();

// Joins a network. Succeeds without a password only for one already saved; a new
// secured network needs the system's own dialog, which is why the caller falls back
// to opening it.
bool connect_wifi(const std::string& ssid);

std::vector<WifiNetwork> scan_wifi();

// Icon name for a state, from the standard network-* names.
std::string network_icon_name(const NetworkState& state);

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
