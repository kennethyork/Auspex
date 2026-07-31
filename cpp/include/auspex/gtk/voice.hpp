// Voice actions for the panel buttons.
//
// Replaces src/magi_shell/widgets/voice.py, src/utils/voice_assistant.py and
// src/magi_shell/desktop_assistant.py. Upstream wired these together as a pipeline
// of Python subprocesses (asr.py | desktop_assistant.py) plus a directory watcher
// for TTS. Everything here is in-process against auspex::Asr / auspex::Tts /
// auspex::OllamaClient.
//
// Whisper inference and TTS playback both block for seconds, so they must never
// run on the GTK thread. One worker thread owns the models; the UI thread only
// posts jobs and receives completions through a Glib::Dispatcher. The Asr model is
// loaded lazily on first use so the panel starts instantly rather than stalling on
// a 1.6GB model load.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <string>
#include <thread>

#include <glibmm/dispatcher.h>

#include "auspex/asr.hpp"
#include "auspex/canvas.hpp"
#include "auspex/config.hpp"
#include "auspex/ollama_client.hpp"
#include "auspex/tts.hpp"
#include "auspex/vad.hpp"
#include "auspex/voice_gate.hpp"

namespace auspex::gtk {

class VoiceController {
public:
    enum class Action {
        Dictate,         // record -> transcribe -> type into the focused window
        AskAloud,        // record -> transcribe -> LLM -> speak the answer
        SpeakSelection,  // speak the X primary selection
        Command,         // record -> transcribe -> LLM -> execute a desktop action
    };

    enum class Mode {
        PushToTalk,  // a click records for a fixed window
        Hold,        // recording lasts as long as the button is held
        Continuous,  // VAD decides utterance boundaries; no button at all
    };

    explicit VoiceController(Config config);
    ~VoiceController();

    VoiceController(const VoiceController&)            = delete;
    VoiceController& operator=(const VoiceController&) = delete;

    // Enqueues an action. Ignored if one is already in flight, so a double-click
    // cannot start two recordings against the same microphone.
    void submit(Action action);

    // Press-and-hold: begin capturing now, stop when release_hold() is called and
    // then run `action` on whatever was captured. Replaces widgets/voice.py's
    // gesture recording, which Auspex previously flattened into a fixed 5s window.
    void press_hold(Action action);
    void release_hold();

    // Continuous listening. Requires Config::vad_model; returns false and reports
    // through on_status if the VAD model is missing or unloadable.
    bool start_continuous(Action action);
    void stop_continuous();
    bool continuous() const { return continuous_.load(); }

    // Cleared by the settings window when the conversation should start fresh.
    void clear_history();

    // Borrowed canvas state for the open_terminal / pan_canvas verbs. The shell
    // owns the Canvas; null (the default) makes those verbs degrade gracefully.
    void set_canvas(Canvas* canvas, const Rect& monitor) {
        canvas_  = canvas;
        monitor_ = monitor;
    }

    // Text chat: asks the model on the worker thread and delivers the reply on the
    // GTK thread via on_reply. Used by ChatWindow, which must not block the UI for
    // the seconds a generation takes.
    void ask_text(std::string question);

    // Called on the GTK thread with each completed chat reply.
    std::function<void(const std::string&)> on_reply;

    // Transcribes a held recording and delivers the text on the GTK thread, so the
    // chat window can dictate into its entry box.
    void dictate_to_callback();
    std::function<void(const std::string&)> on_transcript;

    bool busy() const { return busy_.load(); }

    // Called on the GTK thread whenever status text changes.
    std::function<void(const std::string&)> on_status;

private:
    void worker();
    void run_action(Action action);
    void post_status(std::string text);

    void continuous_worker(Action action);
    void handle_utterance(const std::vector<float>& pcm, Action action);
    void remember(std::string question, std::string answer);
    std::vector<std::pair<std::string, std::string>> history_snapshot() const;

    void do_dictate();
    void do_ask_aloud();
    void do_speak_selection();
    void do_command();

    // The text-processing halves, shared by push-to-talk, hold and continuous.
    void dispatch_command(const std::string& utterance);
    void answer_aloud(const std::string& question);
    void do_ask_text(const std::string& question);
    void do_dictate_to_callback();
    void post_reply(std::string text);
    void post_transcript(std::string text);

    // Loads the model on first use; returns nullptr and reports why on failure.
    Asr* ensure_asr();
    std::optional<std::string> record_and_transcribe();

    Config config_;
    Tts    tts_;
    Canvas* canvas_ = nullptr;
    Rect    monitor_{};
    std::optional<Asr> asr_;
    bool   asr_load_failed_ = false;

    // Continuous listening runs on its own thread so the action worker stays free
    // to transcribe and speak without stalling capture.
    std::thread             continuous_thread_;
    std::atomic<bool>       continuous_{false};
    std::atomic<bool>       stop_continuous_{false};

    // Hold-to-talk capture, owned by the worker thread.
    std::atomic<bool>       holding_{false};

    mutable std::mutex      history_mutex_;
    std::deque<std::pair<std::string, std::string>> history_;

    std::thread             thread_;
    std::mutex              mutex_;
    std::condition_variable wake_;
    std::deque<Action>      queue_;
    bool                    stopping_ = false;
    std::atomic<bool>       busy_{false};

    // Dispatcher is the only sanctioned way to touch GTK from another thread.
    Glib::Dispatcher status_ready_;
    std::mutex       status_mutex_;
    std::string      status_text_;

    // Queued text work, drained by the same worker thread as voice actions so only
    // one generation runs at a time.
    std::deque<std::string> text_queue_;
    bool                    dictate_to_callback_ = false;

    Glib::Dispatcher reply_ready_;
    std::mutex       reply_mutex_;
    std::deque<std::string> replies_;

    Glib::Dispatcher transcript_ready_;
    std::mutex       transcript_mutex_;
    std::deque<std::string> transcripts_;
};

}  // namespace auspex::gtk
