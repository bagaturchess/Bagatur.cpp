// Go command parser. Mirrors bagaturchess.uci.impl.commands.Go.

#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace uci {

struct Go {
    static constexpr std::int64_t UNDEF_TIME      = std::numeric_limits<std::int64_t>::max();
    static constexpr std::int64_t UNDEF_NODES     = std::numeric_limits<std::int64_t>::max();
    static constexpr int          UNDEF_DEPTH     = std::numeric_limits<int>::max();
    static constexpr int          UNDEF_MOVESTOGO = -1;
    static constexpr std::int64_t UNDEF_MOVETIME  = -1;

    bool          ponder    = false;
    std::int64_t  wtime     = UNDEF_TIME;
    std::int64_t  btime     = UNDEF_TIME;
    std::int64_t  winc      = 0;
    std::int64_t  binc      = 0;
    std::int64_t  nodes     = UNDEF_NODES;
    int           movestogo = UNDEF_MOVESTOGO;
    std::int64_t  movetime  = UNDEF_MOVETIME;
    bool          infinite  = false;
    int           depth     = UNDEF_DEPTH;

    // Reject non-positive limits: `go depth 0` / `go nodes 0` should not
    // pretend to be a real cap — without this, classify() reports FIXED_DEPTH
    // / FIXED_NODES, then the state manager's `if (limit > 0)` guard skips
    // setting the limit at all, and the search runs unbounded.
    bool hasNodes() const noexcept { return nodes != UNDEF_NODES && nodes > 0; }
    bool hasDepth() const noexcept { return depth != UNDEF_DEPTH && depth > 0; }
    bool isAnalyzingMode() const noexcept { return infinite; }

    static Go parse(const std::string& line);
};

}  // namespace uci
