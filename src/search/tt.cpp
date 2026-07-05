#include "tt.h"

#include <algorithm>

namespace search {

namespace {

inline std::size_t round_down_pow2(std::size_t v) noexcept {
    std::size_t r = 1;
    while ((r << 1) <= v) r <<= 1;
    return r;
}

// w0 : [ key32 : 32 ][ depth : 16 ][ flag : 8 ][ gen : 8 ]
inline std::uint64_t pack_w0(std::uint32_t key32, std::int16_t depth,
                             std::int8_t flag, std::uint8_t gen) noexcept {
    return (std::uint64_t(key32) << 32)
         | (std::uint64_t(static_cast<std::uint16_t>(depth)) << 16)
         | (std::uint64_t(static_cast<std::uint8_t>(flag)) << 8)
         |  std::uint64_t(gen);
}
// w1 : [ move : 32 ][ score : 16 ][ eval : 16 ]
inline std::uint64_t pack_w1(std::int32_t move, std::int16_t score,
                             std::int16_t eval) noexcept {
    return (std::uint64_t(static_cast<std::uint32_t>(move)) << 32)
         | (std::uint64_t(static_cast<std::uint16_t>(score)) << 16)
         |  std::uint64_t(static_cast<std::uint16_t>(eval));
}

inline std::uint32_t w0_key32(std::uint64_t w0) noexcept { return std::uint32_t(w0 >> 32); }
inline std::int16_t  w0_depth(std::uint64_t w0) noexcept { return std::int16_t(std::uint16_t(w0 >> 16)); }
inline std::int8_t   w0_flag (std::uint64_t w0) noexcept { return std::int8_t (std::uint8_t (w0 >> 8)); }
inline std::uint8_t  w0_gen  (std::uint64_t w0) noexcept { return std::uint8_t (w0); }
inline std::int32_t  w1_move (std::uint64_t w1) noexcept { return std::int32_t(std::uint32_t(w1 >> 32)); }
inline std::int16_t  w1_score(std::uint64_t w1) noexcept { return std::int16_t(std::uint16_t(w1 >> 16)); }
inline std::int16_t  w1_eval (std::uint64_t w1) noexcept { return std::int16_t(std::uint16_t(w1)); }

constexpr auto RELAXED = std::memory_order_relaxed;

}  // namespace

TranspositionTable::TranspositionTable(std::size_t mb) { resize(mb); }

void TranspositionTable::resize(std::size_t mb) {
    std::size_t bytes   = mb * (1ull << 20);
    std::size_t buckets = round_down_pow2(bytes / sizeof(Bucket));
    if (buckets < 1024) buckets = 1024;

    table_       = std::make_unique<Bucket[]>(buckets);  // value-initialised
    num_buckets_ = buckets;
    mask_        = buckets - 1;
    clear();
}

void TranspositionTable::clear() {
    // Explicitly zero every word — std::atomic's default ctor does not
    // guarantee zero-initialisation on every standard library.
    for (std::size_t i = 0; i < num_buckets_; ++i) {
        for (Slot& s : table_[i].e) {
            s.w0.store(0, RELAXED);
            s.w1.store(0, RELAXED);
        }
    }
    gen_ = 0;
}

bool TranspositionTable::probe(board::BB key, TTEntry& out) const noexcept {
    const Bucket& b   = table_[static_cast<std::size_t>(key) & mask_];
    const auto    k32 = static_cast<std::uint32_t>(key >> 32);

    for (const Slot& s : b.e) {
        std::uint64_t w0 = s.w0.load(RELAXED);
        if (w0_key32(w0) != k32 || w0_flag(w0) == TT_NONE) continue;

        std::uint64_t w1 = s.w1.load(RELAXED);

        // Torn-read guard: if the key word changed between the two loads, the
        // slot was rewritten by another thread mid-probe — skip it.
        if (s.w0.load(RELAXED) != w0) continue;

        out.key32 = k32;
        out.depth = w0_depth(w0);
        out.flag  = w0_flag(w0);
        out.gen   = w0_gen(w0);
        out.move  = w1_move(w1);
        out.score = w1_score(w1);
        out.eval  = w1_eval(w1);
        return true;
    }
    return false;
}

int TranspositionTable::hashfull() const noexcept {
    // Sample the first 1000 slots and report the permill that are in use (a
    // slot is used once it holds a real entry, i.e. flag != TT_NONE).
    constexpr int kSamples = 1000;
    int used = 0;
    int seen = 0;
    for (std::size_t bi = 0; bi < num_buckets_ && seen < kSamples; ++bi) {
        for (const Slot& s : table_[bi].e) {
            if (w0_flag(s.w0.load(RELAXED)) != TT_NONE) ++used;
            if (++seen >= kSamples) break;
        }
    }
    return seen ? used * 1000 / seen : 0;
}

void TranspositionTable::store(board::BB key, int move, int score, int eval,
                               int depth, TTFlag flag, int ply) noexcept {
    Bucket&    b   = table_[static_cast<std::size_t>(key) & mask_];
    const auto k32 = static_cast<std::uint32_t>(key >> 32);

    Slot* victim = &b.e[0];
    int   worst  = 999;

    for (Slot& s : b.e) {
        std::uint64_t w0 = s.w0.load(RELAXED);

        if (w0_flag(w0) == TT_NONE) {       // empty slot — claim it
            victim = &s;
            break;
        }

        if (w0_key32(w0) == k32) {          // same position
            std::uint64_t w1     = s.w1.load(RELAXED);
            int           e_move = w1_move(w1);
            std::int16_t  e_depth= w0_depth(w0);

            // Preserve the old move when the new store has none (qsearch
            // alpha-restore stores move=0; keep the deeper search's move).
            if (move == 0) move = e_move;

            // Don't let a shallow non-exact entry clobber a much deeper one.
            if (flag != TT_EXACT && depth + 4 < e_depth) {
                if (move != 0 && e_move == 0) {
                    // Cheap move-only refresh: rewrite the data word, keep key.
                    s.w1.store(pack_w1(move, w1_score(w1), w1_eval(w1)), RELAXED);
                }
                return;
            }

            victim = &s;
            break;
        }

        // Age-weighted depth — older generations are more replaceable.
        int score_for_replace = w0_depth(w0) + (w0_gen(w0) == gen_ ? 0 : -4);
        if (score_for_replace < worst) {
            worst  = score_for_replace;
            victim = &s;
        }
    }

    int stored_score = score_to_tt(score, ply);
    int clamped_eval = std::clamp(eval, -32768, 32767);

    std::uint64_t new_w1 = pack_w1(move, static_cast<std::int16_t>(stored_score),
                                         static_cast<std::int16_t>(clamped_eval));
    std::uint64_t new_w0 = pack_w0(k32, static_cast<std::int16_t>(depth),
                                        static_cast<std::int8_t>(flag), gen_);

    // Write data first, then commit the key word (a probe re-reads w0).
    victim->w1.store(new_w1, RELAXED);
    victim->w0.store(new_w0, RELAXED);
}

}  // namespace search
