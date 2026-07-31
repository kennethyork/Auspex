#include "auspex/gtk/voice.hpp"

#include <chrono>
#include <utility>

#include "auspex/audio.hpp"
#include "auspex/commands.hpp"
#include "auspex/process.hpp"

namespace auspex::gtk {

namespace {

// Push-to-talk window. Upstream's asr.py ran a continuous VAD loop; a fixed
// capture keeps the first port predictable and avoids holding the microphone open
// for the whole session.
constexpr auto kCaptureDuration = std::chrono::seconds(5);

// Below this peak the capture is silence -- almost always a muted or wrong input
// device. Reporting that is far more useful than feeding noise to whisper and
// showing whatever it hallucinates.
constexpr float kSilenceThreshold = 0.001f;

}  // namespace

VoiceController::VoiceController(Config config)
    : config_(std::move(config)), tts_(config_.tts_command) {
    status_ready_.connect([this]() {
        std::string text;
        {
            std::lock_guard lock(status_mutex_);
            text = status_text_;
        }
        if (on_status) on_status(text);
    });

    // settings.py had this option but nothing honoured it. Applied once here so
    // every capture path (push-to-talk, hold, continuous) uses the same device.
    if (!config_.default_microphone.empty()) {
        std::string error;
        if (!audio::select_input_device(config_.default_microphone, &error)) {
            post_status("Microphone: " + error);
        }
    }

    reply_ready_.connect([this]() {
        std::deque<std::string> pending;
        {
            std::lock_guard lock(reply_mutex_);
            pending.swap(replies_);
        }
        for (auto& text : pending) {
            if (on_reply) on_reply(text);
        }
    });

    transcript_ready_.connect([this]() {
        std::deque<std::string> pending;
        {
            std::lock_guard lock(transcript_mutex_);
            pending.swap(transcripts_);
        }
        for (auto& text : pending) {
            if (on_transcript) on_transcript(text);
        }
    });

    thread_ = std::thread([this] { worker(); });
}

void VoiceController::post_reply(std::string text) {
    {
        std::lock_guard lock(reply_mutex_);
        replies_.push_back(std::move(text));
    }
    reply_ready_.emit();
}

void VoiceController::post_transcript(std::string text) {
    {
        std::lock_guard lock(transcript_mutex_);
        transcripts_.push_back(std::move(text));
    }
    transcript_ready_.emit();
}

void VoiceController::ask_text(std::string question) {
    {
        std::lock_guard lock(mutex_);
        text_queue_.push_back(std::move(question));
    }
    wake_.notify_one();
}

void VoiceController::dictate_to_callback() {
    if (busy_.load() || holding_.load()) return;
    holding_.store(true);
    {
        std::lock_guard lock(mutex_);
        dictate_to_callback_ = true;
        queue_.push_back(Action::Dictate);
    }
    wake_.notify_one();
}

void VoiceController::do_ask_text(const std::string& question) {
    post_status("Asking " + config_.ollama_model + "...");

    OllamaClient ollama(config_);
    std::string prompt;
    for (const auto& [q, a] : history_snapshot()) {
        prompt += "user: " + q + "\nyou: " + a + "\n";
    }
    prompt += "user: " + question + "\nyou:";

    const auto reply = ollama.generate(config_.ollama_model, prompt);
    if (!reply) {
        post_reply("(the model did not answer)");
        post_status("");
        return;
    }
    const std::string answer = reply->response.empty() ? reply->thinking : reply->response;
    remember(question, answer);
    post_reply(answer.empty() ? "(no answer)" : answer);
    post_status("");
}

void VoiceController::do_dictate_to_callback() {
    const auto text = record_and_transcribe();
    post_status("");
    if (text) post_transcript(*text);
}

VoiceController::~VoiceController() {
    stop_continuous();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void VoiceController::clear_history() {
    std::lock_guard lock(history_mutex_);
    history_.clear();
}

void VoiceController::remember(std::string question, std::string answer) {
    if (config_.memory_turns <= 0) return;
    std::lock_guard lock(history_mutex_);
    history_.emplace_back(std::move(question), std::move(answer));
    while (history_.size() > static_cast<std::size_t>(config_.memory_turns)) {
        history_.pop_front();
    }
}

std::vector<std::pair<std::string, std::string>> VoiceController::history_snapshot() const {
    std::lock_guard lock(history_mutex_);
    return {history_.begin(), history_.end()};
}

// ---------------------------------------------------------------------------
// Press-and-hold
// ---------------------------------------------------------------------------
void VoiceController::press_hold(Action action) {
    if (busy_.load() || holding_.load()) return;
    holding_.store(true);
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(action);
    }
    wake_.notify_one();
}

void VoiceController::release_hold() { holding_.store(false); }

// ---------------------------------------------------------------------------
// Continuous listening
// ---------------------------------------------------------------------------
bool VoiceController::start_continuous(Action action) {
    if (continuous_.load()) return true;

    if (config_.vad_model.empty()) {
        post_status("Continuous listening needs \"vad_model\" in config.json");
        return false;
    }

    stop_continuous_.store(false);
    continuous_.store(true);
    continuous_thread_ = std::thread([this, action] { continuous_worker(action); });
    return true;
}

void VoiceController::stop_continuous() {
    stop_continuous_.store(true);
    if (continuous_thread_.joinable()) continuous_thread_.join();
    continuous_.store(false);
}

void VoiceController::continuous_worker(Action action) {
    std::string error;

    // use_gpu stays false: the Silero graph aborts ggml on the Vulkan backend, and
    // VAD is cheap enough that CPU costs nothing measurable.
    auto vad = Vad::open(config_.vad_model,
                         {.threads = config_.asr_threads, .use_gpu = false}, &error);
    if (!vad) {
        post_status("VAD unavailable: " + error);
        continuous_.store(false);
        return;
    }

    audio::Stream stream;
    if (!stream.start(&error)) {
        post_status("Microphone error: " + error);
        continuous_.store(false);
        return;
    }

    VoiceGateConfig gate_config;
    gate_config.threshold        = config_.vad_threshold;
    gate_config.min_speech_ms    = config_.vad_min_speech_ms;
    gate_config.min_silence_ms   = config_.vad_min_silence_ms;
    VoiceGate gate(gate_config);

    // One VAD call per chunk. 16 windows is ~512ms, which keeps latency low while
    // amortising the model call over enough audio to be cheap.
    constexpr std::size_t kWindows = 16;
    constexpr std::size_t kChunkSamples = Vad::kWindowSamples * kWindows;
    constexpr int         kChunkMs      = Vad::kWindowMs * static_cast<int>(kWindows);

    // Pre-roll: the gate needs min_speech_ms of evidence before it opens, so
    // without this the first syllable is always missing from the transcript.
    const std::size_t pre_roll_samples =
        static_cast<std::size_t>(audio::kSampleRate) * gate_config.pre_roll_ms / 1000;

    std::deque<float>  pre_roll;
    std::vector<float> utterance;
    std::vector<float> chunk;

    post_status("Listening continuously...");

    while (!stop_continuous_.load()) {
        if (!stream.read(chunk, kChunkSamples)) break;

        // Never listen while the assistant is speaking, or its own TTS becomes the
        // next utterance and it talks to itself indefinitely.
        if (busy_.load()) {
            gate.reset();
            vad->reset();
            utterance.clear();
            pre_roll.clear();
            stream.flush();
            continue;
        }

        const auto probability = vad->probability(chunk);
        if (!probability) continue;

        const VoiceGate::Event event = gate.feed(*probability, kChunkMs);

        if (gate.speaking()) {
            if (event == VoiceGate::Event::Started) {
                utterance.assign(pre_roll.begin(), pre_roll.end());
            }
            utterance.insert(utterance.end(), chunk.begin(), chunk.end());
        } else {
            pre_roll.insert(pre_roll.end(), chunk.begin(), chunk.end());
            while (pre_roll.size() > pre_roll_samples) pre_roll.pop_front();
        }

        if (event == VoiceGate::Event::Ended || event == VoiceGate::Event::Capped) {
            std::vector<float> captured = std::move(utterance);
            utterance.clear();
            pre_roll.clear();
            vad->reset();

            if (!captured.empty()) handle_utterance(captured, action);
        }
    }

    stream.stop();
    continuous_.store(false);
    post_status("");
}

void VoiceController::handle_utterance(const std::vector<float>& pcm, Action action) {
    Asr* asr = ensure_asr();
    if (!asr) return;

    busy_.store(true);

    std::string error;
    post_status("Transcribing...");
    const auto result = asr->transcribe(pcm, &error);
    if (result && !result->text.empty()) {
        switch (action) {
            case Action::Dictate:
                post_status("Typing: " + result->text);
                type_text(result->text);
                break;
            case Action::Command:
                dispatch_command(result->text);
                break;
            case Action::AskAloud:
                answer_aloud(result->text);
                break;
            case Action::SpeakSelection:
                do_speak_selection();
                break;
        }
    }

    busy_.store(false);
}

void VoiceController::submit(Action action) {
    if (busy_.load()) return;
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(action);
    }
    wake_.notify_one();
}

void VoiceController::post_status(std::string text) {
    {
        std::lock_guard lock(status_mutex_);
        status_text_ = std::move(text);
    }
    status_ready_.emit();
}

void VoiceController::worker() {
    for (;;) {
        Action action;
        std::string question;
        bool have_action = false;
        bool to_callback = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] {
                return stopping_ || !queue_.empty() || !text_queue_.empty();
            });
            if (stopping_) return;

            if (!queue_.empty()) {
                action = queue_.front();
                queue_.pop_front();
                have_action = true;
                to_callback = dictate_to_callback_;
                dictate_to_callback_ = false;
            } else {
                question = std::move(text_queue_.front());
                text_queue_.pop_front();
            }
        }

        busy_.store(true);
        if (have_action) {
            if (to_callback && action == Action::Dictate) do_dictate_to_callback();
            else run_action(action);
        } else {
            do_ask_text(question);
        }
        busy_.store(false);
    }
}

void VoiceController::run_action(Action action) {
    switch (action) {
        case Action::Dictate:        do_dictate(); break;
        case Action::AskAloud:       do_ask_aloud(); break;
        case Action::SpeakSelection: do_speak_selection(); break;
        case Action::Command:        do_command(); break;
    }
}

Asr* VoiceController::ensure_asr() {
    if (asr_) return &*asr_;
    if (asr_load_failed_) return nullptr;

    post_status("Loading whisper model...");
    Asr::silence_logs();

    std::string error;
    asr_ = Asr::open(config_.whisper_model,
                     {.threads = config_.asr_threads, .language = config_.asr_language},
                     &error);
    if (!asr_) {
        asr_load_failed_ = true;   // do not retry a 1.6GB load on every click
        post_status("ASR unavailable: " + error);
        return nullptr;
    }
    return &*asr_;
}

std::optional<std::string> VoiceController::record_and_transcribe() {
    Asr* asr = ensure_asr();
    if (!asr) return std::nullopt;

    post_status("Listening...");
    std::string error;

    // Hold mode records for exactly as long as the button is down, which is what
    // widgets/voice.py's press/release gestures did. Push-to-talk keeps the fixed
    // window.
    std::optional<std::vector<float>> pcm;
    if (holding_.load()) {
        pcm = audio::record_mono16k_until([this] { return !holding_.load(); },
                                          std::chrono::seconds(120), &error);
    } else {
        pcm = audio::record_mono16k(kCaptureDuration, &error);
    }
    if (!pcm) {
        post_status("Microphone error: " + error);
        return std::nullopt;
    }

    if (audio::peak_level(*pcm) < kSilenceThreshold) {
        post_status("Heard nothing - check your input device");
        return std::nullopt;
    }

    post_status("Transcribing...");
    const auto result = asr->transcribe(*pcm, &error);
    if (!result) {
        post_status("Transcription failed: " + error);
        return std::nullopt;
    }
    if (result->text.empty()) {
        post_status("No speech recognised");
        return std::nullopt;
    }
    return result->text;
}

void VoiceController::do_dictate() {
    const auto text = record_and_transcribe();
    if (!text) return;

    post_status("Typing: " + *text);
    if (!type_text(*text)) {
        post_status("Could not type into the focused window");
    }
}

void VoiceController::do_ask_aloud() {
    const auto question = record_and_transcribe();
    if (!question) return;
    answer_aloud(*question);
}

void VoiceController::answer_aloud(const std::string& question) {
    post_status("Asking " + config_.ollama_model + "...");
    OllamaClient ollama(config_);

    // Replay recent turns so follow-ups resolve; voice_assistant.py kept 5.
    std::string prompt;
    for (const auto& [q, a] : history_snapshot()) {
        prompt += "user: " + q + "\nyou: " + a + "\n";
    }
    prompt += "user: " + question + "\nyou:";

    const auto reply = ollama.generate(config_.ollama_model, prompt);
    if (!reply) {
        post_status("Oracle did not answer");
        return;
    }

    // A reasoning model can complete with its budget spent in `thinking` and an
    // empty `response`; speaking nothing at all would look like a hang.
    const std::string answer = reply->response.empty() ? reply->thinking : reply->response;
    if (answer.empty()) {
        post_status("Oracle returned no answer");
        return;
    }

    remember(question, answer);

    post_status("Speaking...");
    std::string error;
    if (!tts_.speak(answer, &error)) {
        post_status("TTS failed: " + error);
        return;
    }
    post_status("");
}

void VoiceController::do_command() {
    const auto utterance = record_and_transcribe();
    if (!utterance) return;
    dispatch_command(*utterance);
}

void VoiceController::dispatch_command(const std::string& utterance) {
    post_status("Interpreting: " + utterance);

    CommandContext context = gather_context(config_);
    context.utterance = utterance;
    context.history = history_snapshot();
    context.canvas  = canvas_;
    context.monitor = monitor_;
    OllamaClient ollama(config_);

    // json + disable_thinking: the reply has to be a parseable object, and a
    // reasoning model would otherwise leave `response` empty.
    const auto reply = ollama.generate(
        config_.ollama_model, build_command_prompt(utterance, context),
        GenerateOptions{.num_predict = 200,
                        .temperature = 0.0,
                        .json = true,
                        .disable_thinking = true});

    if (!reply) {
        post_status("Oracle did not answer");
        return;
    }

    const auto parsed = parse_action(reply->response, context);
    if (!parsed.action) {
        // Speak the refusal: a silent no-op is indistinguishable from a crash.
        post_status(parsed.error);
        std::string tts_error;
        tts_.speak(parsed.error, &tts_error);
        return;
    }

    const auto outcome = execute_action(*parsed.action, context);
    post_status(outcome.message);
    remember(utterance, outcome.message);

    // Answers are worth hearing in full. For an action that succeeded, the panel
    // label is confirmation enough -- speaking "Opening Downloads" every time gets
    // tiresome fast. Failures are always spoken.
    const bool speak_it = parsed.action->kind == ActionKind::Answer || !outcome.ok;
    if (speak_it && !outcome.message.empty()) {
        std::string tts_error;
        tts_.speak(outcome.message, &tts_error);
    }

    if (outcome.ok && parsed.action->kind != ActionKind::Answer) post_status("");
}

void VoiceController::do_speak_selection() {
    // The primary selection rather than Gdk::Clipboard: the panel is a dock window
    // and never holds focus, and the async clipboard API would have to marshal back
    // to this thread anyway. This is also exactly what panel.py's context poller
    // already used.
    // Asked before the read, not after: without a helper the read is empty whatever
    // you have highlighted, and "Nothing selected" would be telling you your own
    // selection does not exist. Naming the fix is the difference between a button
    // that looks broken and one you can do something about.
    if (!can_read_selection()) {
        post_status("Cannot read the selection -- install xclip or xsel");
        return;
    }

    const auto selection = selected_text();
    if (!selection) {
        post_status("Nothing selected");
        return;
    }

    post_status("Speaking selection...");
    std::string error;
    if (!tts_.speak(*selection, &error)) {
        post_status("TTS failed: " + error);
        return;
    }
    post_status("");
}

}  // namespace auspex::gtk
