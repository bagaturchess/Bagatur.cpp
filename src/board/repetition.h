// Repetition counter — replacement for StackLongInt.
//
// Java's StackLongInt is a chained hashmap keyed by zobrist key, mapping to a
// reference count. We replace it with an open-addressed table using linear
// probing — fewer pointer-chasing memory references, much better cache
// behaviour on every do/undo move.
//
// API kept compatible with the Java usage in ChessBoard:
//   inc(key)         -> increments count for `key`, returns new count
//   dec(key)         -> decrements count for `key`, returns new count (0 -> slot is freed)
//   get(key)         -> returns count, or NO_VALUE (-1) if absent

#pragma once

#include <vector>

#include "types.h"

namespace board {

class RepetitionTable {
public:
    static constexpr int NO_VALUE = -1;

    // capacity must be a power of two; 16384 keeps load factor well below 50%
    // even in worst-case search trees that pre-allocate state.
    explicit RepetitionTable(std::size_t capacity = 16384)
        : mask_(capacity - 1) {
        slots_.resize(capacity);
        for (auto& s : slots_) s.count = 0;
    }

    BAGATUR_FORCE_INLINE int inc(BB key) noexcept {
        std::size_t idx = static_cast<std::size_t>(key) & mask_;
        for (;;) {
            Slot& s = slots_[idx];
            if (s.count == 0) {
                s.key   = key;
                s.count = 1;
                return 1;
            }
            if (s.key == key) {
                return ++s.count;
            }
            idx = (idx + 1) & mask_;
        }
    }

    BAGATUR_FORCE_INLINE int dec(BB key) noexcept {
        std::size_t idx = static_cast<std::size_t>(key) & mask_;
        for (;;) {
            Slot& s = slots_[idx];
            if (s.count == 0) return NO_VALUE;
            if (s.key == key) {
                if (s.count == 1) {
                    s.count = 0;
                    erase_and_repair(idx);
                    return 0;
                }
                return --s.count;
            }
            idx = (idx + 1) & mask_;
        }
    }

    BAGATUR_FORCE_INLINE int get(BB key) const noexcept {
        std::size_t idx = static_cast<std::size_t>(key) & mask_;
        for (;;) {
            const Slot& s = slots_[idx];
            if (s.count == 0) return NO_VALUE;
            if (s.key == key)  return s.count;
            idx = (idx + 1) & mask_;
        }
    }

    void clear() noexcept {
        for (auto& s : slots_) { s.count = 0; }
    }

private:
    struct Slot {
        BB  key   = 0;
        int count = 0;
    };

    // Backward-shift deletion (Knuth, "Algorithm R"): walk forward from the
    // hole and slide each entry back whenever doing so does not violate its
    // probe-chain invariant.
    void erase_and_repair(std::size_t hole) noexcept {
        for (;;) {
            std::size_t next = (hole + 1) & mask_;
            Slot& s = slots_[next];
            if (s.count == 0) return;
            std::size_t natural = static_cast<std::size_t>(s.key) & mask_;
            // `s` may move into `hole` iff its natural slot is not strictly
            // between `hole` and `next` (cyclic order).
            bool in_between =
                (hole < next) ? (natural > hole && natural <= next)
                              : (natural > hole || natural <= next);
            if (!in_between) {
                slots_[hole]  = s;
                s.count       = 0;
                hole          = next;
            } else {
                return;
            }
        }
    }

    std::size_t       mask_;
    std::vector<Slot> slots_;
};

}  // namespace board
