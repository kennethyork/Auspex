// miniaudio's implementation lives in this translation unit alone so the 82k-line
// header is compiled exactly once.
//
// Trimmed to what MAGI uses: decoding (for file input) and capture (for the mic).
// Playback is deliberately excluded — TTS output is handled by the player in the
// configured tts_command pipeline, not by linking an output device here.
#define MINIAUDIO_IMPLEMENTATION

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_NULL

// Silence third-party warnings; this file is not ours to clean up.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "miniaudio/miniaudio.h"
#pragma GCC diagnostic pop
