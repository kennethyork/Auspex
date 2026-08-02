# Auspex

A local AI desktop shell for Linux. Native C++, no Python runtime, nothing leaves
your machine.

Auspex docks a panel into your existing X11 session and gives it speech
recognition, a language model, speech synthesis, and the ability to *act* on what
you say — open a folder, switch workspace, focus a window, search the web — through
a fixed whitelist of verbs.

```
you: "open my downloads folder"      → xdg-open ~/Downloads
you: "switch to workspace 3"         → wmctrl -s 2
you: "open github dot com"           → browser
you: "what is the capital of France" → spoken answer
you: "delete all my files"           → spoken refusal (no such verb exists)
```

## Status

Working, but young. Expect rough edges. X11 only.

## What makes it different

**No Python runtime.** Speech recognition is [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
in-process, not a Flask server behind torch. The binaries link `libcurl`,
`libstdc++` and `libc` — that's it. A CPU-only build is about 20MB.

**It acts, but on a leash.** The model never emits a command line. It returns one
JSON object whose `action` must be one of eight verbs, and every target is checked
against reality before use: a path must exist, an app must resolve in `PATH`, a
window must be open, a workspace index must be in range. Execution is always
`execvp` with an argv vector — **no shell is involved anywhere**, so shell
metacharacters in model output are inert. There is no verb that deletes, moves or
overwrites anything.

**Local.** Ollama, whisper and piper all run on your hardware. No cloud call, no
telemetry.

## Speed

Measured on a Ryzen 5 2600X / RTX 3090, 11s of speech, best of 3 runs:

| model | size | CPU | GPU (Vulkan) | speedup |
|---|---|---|---|---|
| `ggml-base.en.bin` | 142M | 1.20x realtime | 106.8x | 89x |
| `ggml-small.en.bin` | 466M | 0.49x | 67.5x | 149x |
| `ggml-large-v3-turbo.bin` | 1.6G | 0.15x | 55.6x | 372x |

`small.en` is the default because it is the right compromise on CPU. **If you have
a GPU, use `large-v3-turbo`** — it costs 35ms more than `small` there and is far
more accurate. Note whisper's naming is counterintuitive: `tiny < base < small <
medium < large`, so "small" is bigger and slower than "base".

## Install

```sh
git clone https://github.com/kennethyork/Auspex.git
cd Auspex
bin/setup.sh
```

`setup.sh` handles apt, dnf, pacman, zypper, apk and xbps. It installs build
dependencies, compiles, downloads the models, and writes a config. Only the apt
path has been tested on real hardware; the other five package name sets are best
effort, and on failure the script tells you exactly which names did not resolve.

For GPU speech recognition, install `glslc` first (Debian/Fedora: `glslc`;
Arch/SUSE/Alpine/Void: `shaderc`) and it is detected automatically.

```sh
bin/detect-arch.sh          # which prebuilt download suits this machine
bin/release.sh              # build redistributable tarballs per CPU level
```

## Use

```sh
cpp/build/auspex-shell                      # the panel (X11)
cpp/build/auspex-serve                      # web UI on http://127.0.0.1:8765
cpp/build/auspex-probe                      # model status
cpp/build/auspex-probe ask "..."            # one-shot question
cpp/build/auspex-probe command "..."        # interpret a command, do NOT run it
cpp/build/auspex-listen --mic 5             # record and transcribe
cpp/build/auspex-listen --devices           # list microphones
cpp/build/auspex-say "hello"                # speak
cpp/build/auspex-selftest                   # 1171 self-checks
```

`auspex-probe command` is the safe way to see how the model interprets a phrase
without anything happening.

The panel's bottom row is: settings · ask/chat · speak-selection · hold-to-dictate ·
terminal · continuous listening. Left-click the centre button to speak a command;
right-click it to open the chat window.

## Projects and agents

**Hover the terminal button** on the bottom panel. You get the folder list, every
coding agent installed, and the everyday `ollamadev` flows — one click deep, without
opening a window. Left-clicking the button still just opens a terminal, as it always
did.

```
Folder   [ Auspex ▾ ]        ← ollamadev's bookmarks + Auspex's recents
/home/you/Documents/Auspex
Other folder…
─────────────────────
Terminal here
Claude Code · Codex · Gemini · Cursor Agent · OpenCode · OllamaDev · Qwen Code
─────────────────────
Crew…  Chat  Ship  Verify  Index  Doctor
```

Picking a CLI **asks which folder to open it in**, seeded from the dropdown — so the
usual folder is one Enter away and a different one is possible without changing the
default first. Cancelling launches nothing. `Terminal here` is the exception: it says
where it goes, so it just goes there.

The menu opens on hover after a short delay and closes about a second after you
leave it, which matters more than tidiness: while a popover is up it holds an input
grab, so one left open makes the panel's own right-click menu appear not to work.

The same thing with more room is at right-click the panel → **Projects**, which adds
your file manager.

The bottom section mirrors [ollamadev-qt](https://github.com/kennethyork/ollamadev)'s
Start pane, so the two front ends offer the same flows and neither is the odd one
out. `Crew…` opens the crew window on the chosen folder rather than starting a run —
a crew needs a task, and nothing that edits files should begin from a hover menu.

The folder list is ollamadev's own bookmarks (`~/.ollamadev/workspaces.json`) plus
Auspex's recents, so a project adopted in either program shows up in both.

**Agents are found outside `$PATH` too.** The panel is started by the display
manager, never by a login shell, so its `PATH` is missing everything nvm, bun, deno,
cargo and friends append to a profile — which on a normal machine hides half the
agents you have installed. Auspex searches those trees as well and launches by
absolute path, which also immunises it against terminals that spawn their window
from a long-lived server process holding a different environment.

**Why this window exists:** every one of these tools reads and writes the tree it is
started in, and the panel had no way to say which tree that was — it inherited its
own working directory, which at login is `$HOME`. The folder is now chosen
explicitly and travels with the command, both as the terminal's own
`--working-directory` flag and as the child process's `chdir`. Neither mechanism
covers every terminal; together they cover what people actually run.

## The crew

Right-click the panel → **Crew** (only shown when `ollamadev` is installed). A task,
the brain options worth choosing, three lanes showing what the crew is doing, and
the changesets it is holding for review — with the diff foldable inline.

The engine is ollamadev: a Director decomposes the task, coders build each piece in
its own git worktree, and an Auditor reads every diff before it lands. Auspex does
not reimplement any of that — it builds an argv, starts it in the folder you chose,
and reads the state files the engine already writes.

The lanes are **To do / Doing / Done / Held**, with Held appearing only when
something is held — the same mapping as ollamadev-qt's board, so the two front ends
never disagree about what a state means. Each card names the model doing the work
and, when `--route` is on, why that model. Coders in flight get their own steer box.

## Team

Right-click the panel → **Team**. Tick several providers, type one prompt, press
Launch: each gets its own terminal running `ollamadev --backend <id> "<prompt>"` in
the chosen folder. Providers that aren't installed are shown greyed rather than
hidden, so the list says what the machine could do as well as what it can.

## Brain

Right-click the panel → **Brain**. Which model each difficulty tier routes to, and a
probe that answers "where would this request go?" without running it. The tiers are
read and written through `ollamadev config`, so a change here is the same change the
Qt front end sees.

Two things worth knowing before you press Accept:

- **The crew edits the folder named at the top of the window.** It is set from
  ollamadev's active workspace on open; change it before you start.
- **The board is global**, shared by every project — so a run left holding changes
  in one folder is listed beside a run in another, under one set of numbers.
  Anything that would land somewhere other than the folder you are looking at says
  so on its own row.

## Requirements

**Hard:** an X11 session. The panel docks with `_NET_WM_WINDOW_TYPE_DOCK` and
`_NET_WM_STRUT_PARTIAL`, which is EWMH, so any compliant X11 window manager works
(xfwm4, marco, openbox, i3, KWin-X11, Mutter-X11). **Wayland is not supported** —
that would need `gtk4-layer-shell` instead.

**Runtime, required** — the panel cannot place or dock itself without these:
`xrandr` (monitor geometry), `wmctrl`, `xdotool`, `xprop`, `xwininfo`.

**Runtime, optional** — each one costs exactly one feature: `xclip` (reading the
primary selection, so speak-selection and "ask about this"), `xdg-utils`
(`open_path` / `open_url`), `pactl` **or** `wpctl` (volume), `xfconf-query` or
`gsettings` (inheriting the desktop wallpaper onto the canvas),
[ollama](https://ollama.com) (every AI feature), and `piper` or `espeak-ng` (speech).

`bin/setup.sh` checks all of these **by binary rather than by package** and names
what breaks for each one that is missing. Package names differ across the six
supported package managers and only the apt set has been run on real hardware, so
a wrong name elsewhere would otherwise surface much later as a feature that
quietly does nothing.

**GTK is optional.** Only the panel and its windows need gtkmm-4.0 and libadwaita.
Without them `setup.sh` builds the CLI tools and the web UI, which together are a
complete front end. Verified: the four CLI binaries link zero GTK libraries.

Desktop tools (terminal, launcher, settings, network) are detected from `PATH` at
runtime, so one config works across Xfce, GNOME, KDE, i3 and others.

## Configuration

`~/.config/auspex/config.json`. Notable keys:

| key | meaning |
|---|---|
| `whisper_model` | path to a GGML whisper model |
| `vad_model` | Silero VAD; empty disables continuous listening |
| `default_microphone` | substring match. **Devices named "Monitor of ..." are loopbacks** and record system audio, not your voice |
| `ollama_model` | any model you have pulled |
| `tts_command` | shell pipeline reading text on stdin |
| `auspex_theme` | `Plain`, `Tokyo Night` or `Forest` |
| `memory_turns` | conversation turns replayed as context |

Leaving `terminal`, `launcher`, `settings_command` and `network_command` empty is
recommended — they are then detected at runtime.

### Infinite canvas

The X11 shell turns the primary monitor into an unbounded plane. The wallpaper
stays fixed and both panels float above the plane without reserving its edges, so
windows can pass behind the bars while panning and reappear on the other side.
Drag the desktop or scroll to pan, use Ctrl+scroll or the panel's −/1:1/+ controls
to zoom, press Home to return to the origin, and press F to fit the open windows.

## The web UI

`auspex-serve` binds **127.0.0.1 only**, and that is not configurable. Because
nothing off-machine can reach it there is no login, no session cookie and no TLS —
none of which can then be got wrong. It is not an isolation boundary: any process on
the machine can reach a loopback port, so treat it as the same trust level as your
desktop session.

Endpoints: `GET /`, `GET /status`, `POST /chat`, `POST /command`, `POST /speak`,
`POST /transcribe`.

## Security notes

- The command whitelist is eight verbs. Unknown verbs fail closed to a spoken
  answer, never to execution.
- `launch_app` runs a `PATH` executable **with no arguments**, and rejects anything
  containing a path separator or shell metacharacter.
- `open_path` refuses `.desktop` entries, script extensions and anything with an
  executable bit, so it cannot become a launch primitive with a model-chosen target.
- `open_url` allows only `http`/`https`.
- `focus_window` never trusts a model-supplied window id; the model names a title
  and the id is looked up from the live window list.
- 34 of the self-checks cover exactly these paths, including injection attempts.
- `tts_command` *is* executed as a shell pipeline — but it is a value you write,
  never model output.

Deliberately **not** ported from the upstream project: its assistant let the model
send arbitrary shell commands into a live terminal, and its chat window had a "run
this code block" button. Both executed model output.

## Building by hand

```sh
cmake -S cpp -B build -DAUSPEX_BUILD_SHELL=ON
cmake --build build
```

| option | values | default |
|---|---|---|
| `AUSPEX_BUILD_SHELL` | `ON`/`OFF` | `OFF` |
| `AUSPEX_WHISPER_BACKEND` | `auto`/`cpu`/`vulkan`/`cuda` | `auto` |
| `AUSPEX_ARCH_LEVEL` | `native`/`v1`/`v2`/`v3`/`v4` | `native` |

`AUSPEX_ARCH_LEVEL=native` uses `-march=native`, which is correct when you build on
the machine that will run it and **wrong for anything you redistribute** — such a
binary dies with SIGILL on an older CPU. `bin/release.sh` builds one artifact per
x86-64 microarchitecture level and verifies each with `objdump`.

## Credits and licence

Auspex began as a port of [MAGI](https://github.com/ruapotato/MAGI) by
[ruapotato](https://github.com/ruapotato) — an experimental AI desktop for Debian
and MATE, now discontinued. The three colour palettes (Plain, Tokyo Night, Forest)
and the stylesheet are carried over from it essentially unchanged, and the original
Python implementation remains in `src/` for reference and history.

Everything under `cpp/` is a rewrite rather than a translation: ASR moved from a
Flask server behind torch to in-process whisper.cpp, TTS from Coqui to piper, and
the spoken-command path from arbitrary shell execution to a validated whitelist.

Licensed **GPLv3**, the same as MAGI. See [LICENSE](LICENSE).

Vendored third-party code under `cpp/third_party/`:
[miniaudio](https://miniaud.io) (public domain),
[nlohmann/json](https://github.com/nlohmann/json) (MIT),
[cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT).
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) (MIT) is fetched at build
time.
