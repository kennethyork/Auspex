#include "auspex/audio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "miniaudio/miniaudio.h"
#pragma GCC diagnostic pop

namespace auspex::audio {

namespace {

void set_error(std::string* out, std::string msg) {
    if (out) *out = std::move(msg);
}

// Defined with the device-selection block below; needed by record_impl() above it.
const ma_device_id* chosen_device_id();

// Shared between the audio thread and the caller. miniaudio invokes the capture
// callback on its own thread, so the buffer needs a lock.
struct CaptureSink {
    std::mutex         mutex;
    std::vector<float> pcm;
    std::size_t        frame_cap = 0;   // hard ceiling so a stuck stop-condition
                                        // cannot grow this without bound
};

void capture_callback(ma_device* device, void* /*output*/, const void* input,
                      ma_uint32 frame_count) {
    auto* sink = static_cast<CaptureSink*>(device->pUserData);
    if (!sink || !input) return;

    const auto* samples = static_cast<const float*>(input);

    std::lock_guard lock(sink->mutex);
    if (sink->pcm.size() >= sink->frame_cap) return;

    const std::size_t room = sink->frame_cap - sink->pcm.size();
    const std::size_t take = std::min<std::size_t>(frame_count, room);
    sink->pcm.insert(sink->pcm.end(), samples, samples + take);
}

std::optional<std::vector<float>> record_impl(const std::function<bool()>& should_stop,
                                              std::chrono::milliseconds max_duration,
                                              std::string* error) {
    CaptureSink sink;
    sink.frame_cap = static_cast<std::size_t>(
        kSampleRate * (static_cast<double>(max_duration.count()) / 1000.0));
    if (sink.frame_cap == 0) {
        set_error(error, "max_duration rounds to zero frames");
        return std::nullopt;
    }
    sink.pcm.reserve(sink.frame_cap);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = kChannels;
    cfg.capture.pDeviceID = chosen_device_id();
    cfg.sampleRate       = kSampleRate;
    cfg.dataCallback     = capture_callback;
    cfg.pUserData        = &sink;

    ma_device device;
    if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) {
        set_error(error, "no usable audio input device (is a microphone connected?)");
        return std::nullopt;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        ma_device_uninit(&device);
        set_error(error, "failed to start audio capture");
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + max_duration;
    while (std::chrono::steady_clock::now() < deadline) {
        if (should_stop && should_stop()) break;
        {
            std::lock_guard lock(sink.mutex);
            if (sink.pcm.size() >= sink.frame_cap) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ma_device_stop(&device);
    ma_device_uninit(&device);

    std::lock_guard lock(sink.mutex);
    return std::move(sink.pcm);
}

}  // namespace

std::optional<std::vector<float>> load_mono16k(const std::filesystem::path& path,
                                               std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        set_error(error, "no such file: " + path.string());
        return std::nullopt;
    }

    // Asking the decoder for our target format makes miniaudio do the resampling
    // and downmixing internally.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, kChannels, kSampleRate);

    ma_decoder decoder;
    if (ma_decoder_init_file(path.string().c_str(), &cfg, &decoder) != MA_SUCCESS) {
        set_error(error, "unsupported or corrupt audio file: " + path.string());
        return std::nullopt;
    }

    std::vector<float> pcm;
    constexpr std::size_t kChunkFrames = 16384;
    std::vector<float> chunk(kChunkFrames);

    for (;;) {
        ma_uint64 got = 0;
        const ma_result rc =
            ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &got);
        if (got > 0) pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + got);
        if (rc != MA_SUCCESS || got == 0) break;   // MA_AT_END or a read error
    }

    ma_decoder_uninit(&decoder);

    if (pcm.empty()) {
        set_error(error, "decoded zero samples from " + path.string());
        return std::nullopt;
    }
    return pcm;
}

std::optional<std::vector<float>> record_mono16k(std::chrono::milliseconds duration,
                                                 std::string* error) {
    return record_impl(nullptr, duration, error);
}

std::optional<std::vector<float>> record_mono16k_until(
    const std::function<bool()>& should_stop,
    std::chrono::milliseconds max_duration,
    std::string* error) {
    return record_impl(should_stop, max_duration, error);
}

// ---------------------------------------------------------------------------
// Device selection
// ---------------------------------------------------------------------------
namespace {

// The chosen capture device id, and whether one is chosen at all. Held at file
// scope because miniaudio wants a ma_device_id* at device-init time and the
// selection outlives any single capture.
std::mutex   g_device_mutex;
bool         g_have_device = false;
ma_device_id g_device_id{};
std::string  g_device_name;

std::string lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const ma_device_id* chosen_device_id() {
    std::lock_guard lock(g_device_mutex);
    return g_have_device ? &g_device_id : nullptr;
}

}  // namespace

std::vector<InputDevice> list_input_devices() {
    std::vector<InputDevice> result;

    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) return result;

    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&context, &playback, &playback_count, &capture,
                               &capture_count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < capture_count; ++i) {
            result.push_back({capture[i].name, capture[i].isDefault != 0});
        }
    }

    ma_context_uninit(&context);
    return result;
}

bool select_input_device(std::string_view name_fragment, std::string* error) {
    if (name_fragment.empty()) {
        std::lock_guard lock(g_device_mutex);
        g_have_device = false;
        g_device_name.clear();
        return true;
    }

    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        set_error(error, "could not enumerate audio devices");
        return false;
    }

    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;

    bool found = false;
    if (ma_context_get_devices(&context, &playback, &playback_count, &capture,
                               &capture_count) == MA_SUCCESS) {
        const std::string needle = lower_copy(name_fragment);
        for (ma_uint32 i = 0; i < capture_count && !found; ++i) {
            if (lower_copy(capture[i].name).find(needle) == std::string::npos) continue;
            std::lock_guard lock(g_device_mutex);
            g_device_id   = capture[i].id;
            g_device_name = capture[i].name;
            g_have_device = true;
            found = true;
        }
    }

    ma_context_uninit(&context);

    if (!found) {
        set_error(error, "no input device matching \"" + std::string(name_fragment) + "\"");
    }
    return found;
}

std::string selected_input_device() {
    std::lock_guard lock(g_device_mutex);
    return g_device_name;
}

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------
struct Stream::Impl {
    ma_device device{};
    bool      device_ready = false;

    std::mutex              mutex;
    std::condition_variable cv;
    std::deque<float>       buffer;
    bool                    running = false;

    // Bounded so a reader that stalls cannot grow this without limit; 30s at
    // 16kHz. Oldest audio is dropped, which is the right trade for live capture.
    static constexpr std::size_t kMaxFrames = kSampleRate * 30;
};

namespace {

void stream_callback(ma_device* device, void*, const void* input, ma_uint32 frame_count) {
    auto* impl = static_cast<Stream::Impl*>(device->pUserData);
    if (!impl || !input) return;

    const auto* samples = static_cast<const float*>(input);
    {
        std::lock_guard lock(impl->mutex);
        impl->buffer.insert(impl->buffer.end(), samples, samples + frame_count);
        while (impl->buffer.size() > Stream::Impl::kMaxFrames) {
            impl->buffer.pop_front();
        }
    }
    impl->cv.notify_all();
}

}  // namespace

Stream::Stream() : impl_(new Impl()) {}

Stream::~Stream() {
    stop();
    delete impl_;
}

bool Stream::start(std::string* error) {
    if (!impl_) return false;
    if (impl_->running) return true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_f32;
    cfg.capture.channels = kChannels;
    cfg.capture.pDeviceID = chosen_device_id();
    cfg.sampleRate       = kSampleRate;
    cfg.dataCallback     = stream_callback;
    cfg.pUserData        = impl_;

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        set_error(error, "no usable audio input device");
        return false;
    }
    impl_->device_ready = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->device_ready = false;
        set_error(error, "failed to start audio capture");
        return false;
    }

    {
        std::lock_guard lock(impl_->mutex);
        impl_->running = true;
        impl_->buffer.clear();
    }
    return true;
}

void Stream::stop() {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running && !impl_->device_ready) return;
        impl_->running = false;
    }
    impl_->cv.notify_all();

    if (impl_->device_ready) {
        ma_device_stop(&impl_->device);
        ma_device_uninit(&impl_->device);
        impl_->device_ready = false;
    }
}

bool Stream::running() const {
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->running;
}

void Stream::flush() {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    impl_->buffer.clear();
}

bool Stream::read(std::vector<float>& out, std::size_t frames) {
    if (!impl_ || frames == 0) return false;

    std::unique_lock lock(impl_->mutex);
    impl_->cv.wait(lock, [&] { return !impl_->running || impl_->buffer.size() >= frames; });
    if (!impl_->running && impl_->buffer.size() < frames) return false;

    out.assign(impl_->buffer.begin(), impl_->buffer.begin() + frames);
    impl_->buffer.erase(impl_->buffer.begin(),
                        impl_->buffer.begin() + static_cast<std::ptrdiff_t>(frames));
    return true;
}

float peak_level(const std::vector<float>& pcm) {
    float peak = 0.0f;
    for (const float s : pcm) peak = std::max(peak, std::fabs(s));
    return peak;
}

}  // namespace auspex::audio
