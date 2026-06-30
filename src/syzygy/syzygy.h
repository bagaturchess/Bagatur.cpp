// syzygy.h — thin C++ wrapper around Fathom (tbprobe.c) for Syzygy tablebases.
//
// Bagatur's board uses a file-reversed square layout (H1 = 0, A1 = 7) while
// Fathom expects the standard A1 = 0. The wrapper horizontally mirrors (flips
// files of) every bitboard and the ep square on the way in, and mirrors the
// suggested root move's squares back (sq ^ 7).
//
// One process-wide singleton. The tablebase files are memory-mapped read-only
// after init(), so concurrent WDL probes from the SMP workers are safe
// (tb_probe_wdl is thread-safe). tb_probe_root is NOT thread-safe and must be
// called once at the root, off the search threads.

#pragma once

#include <string>

#include "../board/chess_board.h"

namespace syzygy {

// Win/Draw/Loss categories — mirror Fathom's TB_LOSS..TB_WIN (0..4); WDL_FAIL
// (-1) marks a missing/unprobeable position.
enum WDL : int {
    WDL_FAIL         = -1,
    WDL_LOSS         = 0,
    WDL_BLESSED_LOSS = 1,   // loss, but drawn by the 50-move rule
    WDL_DRAW         = 2,
    WDL_CURSED_WIN   = 3,   // win, but drawn by the 50-move rule
    WDL_WIN          = 4,
};

class Syzygy {
public:
    static Syzygy& instance();

    // (Re)load tablebases from `path` (the SyzygyPath UCI option, OS-native
    // separators, "<empty>" or "" to disable). Returns true if at least one
    // table was found; max_pieces() then reports the largest table size.
    bool init(const std::string& path);
    void shutdown();

    bool available()  const noexcept { return max_pieces_ > 0; }
    int  max_pieces() const noexcept { return max_pieces_; }

    // In-search WDL probe. Returns one of WDL_LOSS..WDL_WIN, or WDL_FAIL when
    // the position is not probeable (castling rights, rule50 != 0, too many
    // pieces, no tables, or a probe miss). Thread-safe.
    int probe_wdl(const board::ChessBoard& cb) const;

    // Root DTZ probe. Returns the Bagatur-encoded best move (0 on miss) and
    // writes its WDL category to `wdl_out`. NOT thread-safe — call once at the
    // root, off the search threads.
    int probe_root(board::ChessBoard& cb, int& wdl_out) const;

private:
    Syzygy() = default;

    bool inited_     = false;
    int  max_pieces_ = 0;
};

}  // namespace syzygy
