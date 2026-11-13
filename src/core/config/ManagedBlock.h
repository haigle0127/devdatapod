#pragma once

#include <QString>

namespace devdatapod::config {

// Result of trying to keep a managed block in sync inside a config file.
enum class BlockOutcome {
    Written,     // created, or the block between the markers was replaced
    Preserved,   // the file exists but has no markers: it belongs to the user
    Failed       // could not read or write
};

// Rule 6 (AGENTS.md): the panel renders the options it owns, and never silently overwrites
// what the user edited. The boundary is an explicit marker pair; everything outside it is
// copied through verbatim.
//
// A file with no markers is *not* treated as "ours to generate": on the next round-trip the
// panel would drop whatever the user added, which is the exact failure this exists to
// prevent. Those files make the owning service fall back to text-editor mode (P4).
namespace managed {

inline constexpr const char* kBeginMarker = "# --- devdatapod managed block (do not edit inside) ---";
inline constexpr const char* kEndMarker = "# --- end managed block ---";

// Replace or insert the block in `path`. `body` is the rendered text for the options the
// panel owns, without the markers.
BlockOutcome apply(const QString& path, const QString& body, QString* error = nullptr);

// True when the file already has our markers (so the panel may render into it).
bool hasBlock(const QString& path);

}

}
