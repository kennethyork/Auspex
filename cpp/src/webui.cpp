#include "auspex/webui.hpp"

#include <deque>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

// Loopback only, so no TLS is compiled in. Defining this keeps httplib from
// pulling in OpenSSL and removes any chance of an https listener appearing later.
#include "httplib/httplib.h"

#include <nlohmann/json.hpp>

#include "auspex/audio.hpp"
#include "auspex/commands.hpp"
#include "auspex/ollama_client.hpp"
#include "auspex/theme.hpp"

using json = nlohmann::json;

namespace auspex {

namespace {

// The page. Styled from the configured palette so the browser front end matches
// the panel rather than looking like a separate product.
//
// Audio is captured as raw PCM and resampled to 16kHz in the browser rather than
// using MediaRecorder: MediaRecorder produces webm/opus, and decoding that server
// side would mean linking an Opus decoder purely for this path. Raw PCM needs none.
constexpr std::string_view kPage = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Auspex</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
:root{--bg:$panel_bg;--fg:$panel_fg;--btn:$button_bg;--hov:$button_hover;
--accent:$accent;--entry:$entry_bg;--entryfg:$entry_fg;--border:$entry_border;
--sub:$subtitle_fg;--err:$error}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif;
display:flex;flex-direction:column;height:100vh}
header{padding:10px 14px;border-bottom:1px solid var(--border);display:flex;
align-items:center;gap:10px}
header b{color:var(--accent)}
#status{color:var(--sub);font-size:13px;margin-left:auto}
#log{flex:1;overflow-y:auto;padding:14px;display:flex;flex-direction:column;gap:10px}
.msg{max-width:75ch;padding:8px 10px;border-radius:6px;white-space:pre-wrap;
word-wrap:break-word}
.you{background:var(--btn);align-self:flex-end}
.aus{background:var(--entry);color:var(--entryfg);align-self:flex-start}
.err{background:var(--btn);color:var(--err);align-self:flex-start}
form{display:flex;gap:8px;padding:12px;border-top:1px solid var(--border)}
input,button,select{font:inherit;border-radius:6px;border:1px solid var(--border)}
input{flex:1;padding:9px;background:var(--entry);color:var(--entryfg)}
button{padding:9px 14px;background:var(--btn);color:var(--fg);cursor:pointer}
button:hover{background:var(--hov)}
button.on{background:var(--accent);color:var(--bg)}
select{padding:9px;background:var(--btn);color:var(--fg)}
</style></head><body>
<header><b>Auspex</b>
<select id="mode">
  <option value="command">command</option>
  <option value="chat">chat</option>
</select>
<span id="status">ready</span></header>
<div id="log"></div>
<form id="f">
  <input id="t" placeholder="ask, or say &quot;open my downloads&quot;..." autocomplete="off">
  <button type="submit">Send</button>
  <button type="button" id="mic" title="Hold to talk">Talk</button>
</form>
<script>
const log=document.getElementById('log'),status=document.getElementById('status');
const input=document.getElementById('t'),mode=document.getElementById('mode');
function add(text,cls){const d=document.createElement('div');
d.className='msg '+cls;d.textContent=text;log.appendChild(d);
log.scrollTop=log.scrollHeight;return d}
async function post(path,body,type){
  const r=await fetch(path,{method:'POST',body:body,
    headers:type?{'Content-Type':type}:{}});
  if(!r.ok)throw new Error(await r.text());return r.json()}
async function send(text){
  if(!text.trim())return;
  add(text,'you');input.value='';status.textContent='thinking...';
  try{
    const path=mode.value==='chat'?'/chat':'/command';
    const j=await post(path,JSON.stringify({text:text}),'application/json');
    add(j.reply||j.message||'(no reply)','aus');
    status.textContent=j.action?('action: '+j.action):'ready';
  }catch(e){add(String(e.message||e),'err');status.textContent='error'}
}
document.getElementById('f').addEventListener('submit',e=>{
  e.preventDefault();send(input.value)});

// ---- hold-to-talk ----
let ctx,stream,node,chunks=[],recording=false;
const mic=document.getElementById('mic');
function resample(buf,from,to){
  if(from===to)return buf;
  const ratio=from/to,out=new Float32Array(Math.floor(buf.length/ratio));
  for(let i=0;i<out.length;i++){
    const start=i*ratio,end=Math.min(buf.length,start+ratio);
    let sum=0,n=0;
    for(let j=Math.floor(start);j<end;j++){sum+=buf[j];n++}
    out[i]=n?sum/n:0}
  return out}
async function startRec(){
  if(recording)return;recording=true;chunks=[];
  status.textContent='listening...';mic.classList.add('on');
  try{
    stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1}});
    ctx=new AudioContext();
    const src=ctx.createMediaStreamSource(stream);
    node=ctx.createScriptProcessor(4096,1,1);
    node.onaudioprocess=e=>{if(recording)chunks.push(new Float32Array(
      e.inputBuffer.getChannelData(0)))};
    src.connect(node);node.connect(ctx.destination);
  }catch(e){recording=false;mic.classList.remove('on');
    add('microphone: '+e.message,'err');status.textContent='error'}
}
async function stopRec(){
  if(!recording)return;recording=false;mic.classList.remove('on');
  const rate=ctx?ctx.sampleRate:48000;
  if(node)node.disconnect();if(stream)stream.getTracks().forEach(t=>t.stop());
  if(ctx)await ctx.close();
  let total=0;chunks.forEach(c=>total+=c.length);
  if(!total){status.textContent='ready';return}
  const all=new Float32Array(total);let off=0;
  chunks.forEach(c=>{all.set(c,off);off+=c.length});
  const pcm=resample(all,rate,16000);
  const i16=new Int16Array(pcm.length);
  for(let i=0;i<pcm.length;i++){const s=Math.max(-1,Math.min(1,pcm[i]));
    i16[i]=s<0?s*0x8000:s*0x7FFF}
  status.textContent='transcribing...';
  try{
    const j=await post('/transcribe',i16.buffer,'application/octet-stream');
    if(j.text){input.value=j.text;await send(j.text)}
    else{status.textContent='no speech recognised'}
  }catch(e){add(String(e.message||e),'err');status.textContent='error'}
}
mic.addEventListener('mousedown',startRec);
mic.addEventListener('mouseup',stopRec);
mic.addEventListener('mouseleave',()=>{if(recording)stopRec()});
mic.addEventListener('touchstart',e=>{e.preventDefault();startRec()});
mic.addEventListener('touchend',e=>{e.preventDefault();stopRec()});

fetch('/status').then(r=>r.json()).then(j=>{
  status.textContent=j.model+' | '+j.backends}).catch(()=>{});
</script></body></html>)HTML";

std::string render_page(const Palette& palette) {
    // Same $name substitution the GTK stylesheet uses, so one palette definition
    // drives both front ends.
    const std::pair<std::string_view, std::string_view> vars[] = {
        {"$panel_bg", palette.panel_bg},       {"$panel_fg", palette.panel_fg},
        {"$button_bg", palette.button_bg},     {"$button_hover", palette.button_hover},
        {"$accent", palette.accent},           {"$entry_bg", palette.entry_bg},
        {"$entry_fg", palette.entry_fg},       {"$entry_border", palette.entry_border},
        {"$subtitle_fg", palette.subtitle_fg}, {"$error", palette.error},
    };

    std::string out(kPage);
    // Longest-first is unnecessary here because every placeholder is followed by a
    // non-identifier character in the CSS, but replace the longer names first anyway
    // so $panel_bg cannot be clipped by a hypothetical $panel.
    for (const auto& [key, value] : vars) {
        for (std::size_t pos = out.find(key); pos != std::string::npos;
             pos = out.find(key, pos + value.size())) {
            out.replace(pos, key.size(), value);
        }
    }
    return out;
}

std::string json_error(const std::string& message) {
    return json{{"error", message}}.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
class WebUi::Impl {
public:
    explicit Impl(Config config) : config_(std::move(config)), tts_(config_.tts_command) {}

    httplib::Server server;

    Config config_;
    Tts    tts_;

    std::mutex         asr_mutex;   // Asr holds model state; one request at a time
    std::optional<Asr> asr;
    bool               asr_failed = false;

    std::mutex history_mutex;
    std::deque<std::pair<std::string, std::string>> history;

    Asr* ensure_asr(std::string* error) {
        if (asr) return &*asr;
        if (asr_failed) {
            if (error) *error = "speech recognition unavailable";
            return nullptr;
        }
        Asr::silence_logs();
        std::string err;
        asr = Asr::open(config_.whisper_model,
                        {.threads = config_.asr_threads, .language = config_.asr_language},
                        &err);
        if (!asr) {
            asr_failed = true;
            if (error) *error = err;
            return nullptr;
        }
        return &*asr;
    }

    void remember(std::string q, std::string a) {
        if (config_.memory_turns <= 0) return;
        std::lock_guard lock(history_mutex);
        history.emplace_back(std::move(q), std::move(a));
        while (history.size() > static_cast<std::size_t>(config_.memory_turns)) {
            history.pop_front();
        }
    }

    std::vector<std::pair<std::string, std::string>> snapshot() {
        std::lock_guard lock(history_mutex);
        return {history.begin(), history.end()};
    }
};

WebUi::WebUi(Config config)
    : impl_(std::make_unique<Impl>(config)), config_(std::move(config)) {
    auto& server = impl_->server;

    server.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        // Rendered per request rather than cached, so editing the theme in
        // config.json shows up on reload without restarting the server.
        const Config fresh = Config::load();
        res.set_content(render_page(theme_by_name(fresh.theme)),
                        "text/html; charset=utf-8");
    });

    server.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"model", config_.ollama_model},
                             {"backends", Asr::backends()},
                             {"memory_turns", config_.memory_turns}}
                            .dump(),
                        "application/json");
    });

    server.Post("/chat", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("text")) {
            res.status = 400;
            res.set_content(json_error("expected {\"text\": ...}"), "application/json");
            return;
        }
        const std::string text = body.value("text", std::string{});

        std::string prompt;
        for (const auto& [q, a] : impl_->snapshot()) {
            prompt += "user: " + q + "\nyou: " + a + "\n";
        }
        prompt += "user: " + text + "\nyou:";

        OllamaClient ollama(config_);
        const auto reply = ollama.generate(config_.ollama_model, prompt);
        if (!reply) {
            res.status = 502;
            res.set_content(json_error("the model did not answer"), "application/json");
            return;
        }
        const std::string answer =
            reply->response.empty() ? reply->thinking : reply->response;
        impl_->remember(text, answer);
        res.set_content(json{{"reply", answer}}.dump(), "application/json");
    });

    server.Post("/command", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("text")) {
            res.status = 400;
            res.set_content(json_error("expected {\"text\": ...}"), "application/json");
            return;
        }
        const std::string text = body.value("text", std::string{});

        CommandContext context = gather_context(config_);
        context.utterance = text;
        context.history = impl_->snapshot();

        OllamaClient ollama(config_);
        const auto reply = ollama.generate(
            config_.ollama_model, build_command_prompt(text, context),
            GenerateOptions{.num_predict = 200, .temperature = 0.0,
                            .json = true, .disable_thinking = true});
        if (!reply) {
            res.status = 502;
            res.set_content(json_error("the model did not answer"), "application/json");
            return;
        }

        const auto parsed = parse_action(reply->response, context);
        if (!parsed.action) {
            impl_->remember(text, parsed.error);
            res.set_content(json{{"message", parsed.error}, {"action", "refused"}}.dump(),
                            "application/json");
            return;
        }

        // Same whitelist as the voice path; the browser gains no extra authority.
        const auto outcome = execute_action(*parsed.action, context);
        impl_->remember(text, outcome.message);
        res.set_content(json{{"message", outcome.message},
                             {"action", std::string(to_string(parsed.action->kind))},
                             {"ok", outcome.ok}}
                            .dump(),
                        "application/json");
    });

    server.Post("/speak", [this](const httplib::Request& req, httplib::Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        const std::string text =
            body.is_discarded() ? req.body : body.value("text", std::string{});
        if (text.empty()) {
            res.status = 400;
            res.set_content(json_error("nothing to speak"), "application/json");
            return;
        }
        std::string error;
        if (!impl_->tts_.speak(text, &error)) {
            res.status = 500;
            res.set_content(json_error(error), "application/json");
            return;
        }
        res.set_content(json{{"ok", true}}.dump(), "application/json");
    });

    // Raw little-endian int16 mono @16kHz, as produced by the page's resampler.
    server.Post("/transcribe", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.body.size() < 2) {
            res.status = 400;
            res.set_content(json_error("no audio"), "application/json");
            return;
        }

        const std::size_t samples = req.body.size() / 2;
        std::vector<float> pcm(samples);
        const auto* raw = reinterpret_cast<const unsigned char*>(req.body.data());
        for (std::size_t i = 0; i < samples; ++i) {
            const auto lo = static_cast<std::uint16_t>(raw[i * 2]);
            const auto hi = static_cast<std::uint16_t>(raw[i * 2 + 1]);
            const auto v  = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8)));
            pcm[i] = static_cast<float>(v) / 32768.0f;
        }

        std::lock_guard lock(impl_->asr_mutex);
        std::string error;
        Asr* asr = impl_->ensure_asr(&error);
        if (!asr) {
            res.status = 503;
            res.set_content(json_error(error), "application/json");
            return;
        }
        const auto result = asr->transcribe(pcm, &error);
        if (!result) {
            res.status = 500;
            res.set_content(json_error(error), "application/json");
            return;
        }
        res.set_content(json{{"text", result->text},
                             {"seconds", result->audio_seconds}}
                            .dump(),
                        "application/json");
    });
}

WebUi::~WebUi() { stop(); }

bool WebUi::listen(int port, std::string* error) {
    port_ = port;
    running_.store(true);
    // 127.0.0.1, never 0.0.0.0. See the header for why this is not configurable.
    const bool ok = impl_->server.listen("127.0.0.1", port);
    running_.store(false);
    if (!ok && error) {
        *error = "could not bind 127.0.0.1:" + std::to_string(port) +
                 " (already in use?)";
    }
    return ok;
}

bool WebUi::start_background(int port, std::string* error) {
    port_ = port;

    if (!impl_->server.bind_to_port("127.0.0.1", port)) {
        if (error) {
            *error = "could not bind 127.0.0.1:" + std::to_string(port) +
                     " (already in use?)";
        }
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this] {
        impl_->server.listen_after_bind();
        running_.store(false);
    });
    return true;
}

void WebUi::stop() {
    if (impl_) impl_->server.stop();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

}  // namespace auspex
