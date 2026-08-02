// How much context this machine can actually afford.
//
// Ollama's default context window is small -- 4096 on most builds -- and a coder
// loop spends its budget fast: a file listing, a transcript, a skill, a diff. Run
// past it and the model does not fail, it FORGETS: the subtask scrolls out of the
// window and the coder starts answering a question nobody asked. That failure
// looks exactly like a stupid model, which is why it is worth sizing deliberately.
//
// The other direction costs too. Asking for 128k on a machine that cannot hold it
// makes Ollama spill the KV cache to system RAM and the run crawls, or the load
// fails outright.
//
// MEASURED, NOT ASSUMED. ollamadev shipped `lowResource` as a hardcoded `true`, so
// a 24GB card was throttled to 8192 exactly as hard as a laptop with no GPU --
// safe for the wrong machine, and silent about it. Their own header says so. This
// reads the hardware, and an explicit setting still wins.
#pragma once

#include <cstdint>
#include <string>

namespace auspex {

struct MachineMemory {
    std::uint64_t ram_bytes  = 0;
    std::uint64_t vram_bytes = 0;   // 0 when there is no GPU we can see

    bool known() const { return ram_bytes > 0 || vram_bytes > 0; }
};

// What this machine has. RAM from /proc/meminfo, VRAM from the same NVML path the
// panel's GPU meter already uses -- one probe, not a second implementation that
// can disagree with the first.
MachineMemory machine_memory();

// A context size to ask Ollama for, in tokens.
//
// Sized against VRAM when there is a GPU, because that is what holds the KV cache;
// against RAM otherwise. Rounded to a power of two, because that is how every
// model's context is specified and an odd number here just gets rounded anyway.
//
// Bounded at both ends. The floor is 8192: below that a coder loop cannot hold a
// file listing and a transcript at once, and a context too small to work in is
// worse than a slow one. The ceiling is 131072, past which the KV cache costs
// more than the answer is worth on any machine this runs on.
int suggested_context(const MachineMemory& memory);

// The number to actually use: the config's `num_ctx` when it is set, else the
// suggestion. An explicit setting always wins -- this measures the machine, it
// does not overrule the person sitting at it.
int context_for(int configured, const MachineMemory& memory);

// One line for a person: what was found and what it implies.
std::string context_report(const MachineMemory& memory, int configured = 0);

}  // namespace auspex
