// Serialising JSON that came from somebody else's disk.
//
// nlohmann's dump() THROWS on a string that is not valid UTF-8, and the throw is
// not catchable anywhere useful -- it happens deep inside a serialiser called from
// a dozen places, so in practice the process dies.
//
// Every string this program puts in JSON can come from a file it did not write: a
// prompt carries file contents, a manifest carries a diff, the run state carries
// paths. So "not valid UTF-8" is not a hypothetical. Pointed at a second real
// repository, the very first run died on `json.exception.type_error.316` before
// the Director had said anything -- the project had two files whose bytes are not
// UTF-8, and reading them was enough.
//
// A shell that a file in your project can crash is not a shell. Every dump goes
// through here, where an invalid sequence becomes U+FFFD instead of an exception.
//
// LOSSY, AND THAT IS THE POINT. What comes out is for a model to read, a person to
// read, or state to be reloaded from -- never for reconstructing the file's bytes.
// A changed file's CONTENTS are stored as a raw blob next to the manifest, exactly
// so the lossy path and the exact path stay separate. Do not use this for anything
// that has to round-trip.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace auspex {

inline std::string safe_dump(const nlohmann::json& document, int indent = -1) {
    return document.dump(indent, ' ', /*ensure_ascii=*/false,
                         nlohmann::json::error_handler_t::replace);
}

}  // namespace auspex
