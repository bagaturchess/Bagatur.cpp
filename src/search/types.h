// search/types.h — search-wide constants. Mirrors the relevant pieces of
// bagaturchess.search.api.internal.ISearch.

#pragma once

#include <cstdint>

namespace search {

inline constexpr int MAX_PLY        = 128;
inline constexpr int MAX_MATE       = 30000;  // arbitrary, only its sign+magnitude matters
inline constexpr int MATE_THRESHOLD = MAX_MATE - MAX_PLY;
inline constexpr int SCORE_MIN      = -MAX_MATE - 1;
inline constexpr int SCORE_MAX      =  MAX_MATE + 1;
inline constexpr int SCORE_INF      = SCORE_MAX;
inline constexpr int SCORE_DRAW     = 0;

// TT entry flags — match the semantics of bagaturchess ITTEntry.
enum TTFlag : std::int8_t {
    TT_NONE  = 0,
    TT_EXACT = 1,  // alpha < score < beta, exact value
    TT_LOWER = 2,  // score >= beta, fail-high (lower bound)
    TT_UPPER = 3,  // score <= alpha, fail-low (upper bound)
};

inline bool is_mate_score(int score) noexcept {
    return score > MATE_THRESHOLD || score < -MATE_THRESHOLD;
}

inline int mate_in(int ply) noexcept    { return  MAX_MATE - ply; }
inline int mated_in(int ply) noexcept   { return -MAX_MATE + ply; }

// Score adjustments around TT (mate scores are ply-relative; we store the
// distance from the current node).
inline int score_to_tt(int score, int ply) noexcept {
    if (score >=  MATE_THRESHOLD) return score + ply;
    if (score <= -MATE_THRESHOLD) return score - ply;
    return score;
}
inline int score_from_tt(int score, int ply) noexcept {
    if (score >=  MATE_THRESHOLD) return score - ply;
    if (score <= -MATE_THRESHOLD) return score + ply;
    return score;
}

}  // namespace search
