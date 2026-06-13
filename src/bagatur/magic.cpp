// Magic-bitboard initialisation. Magic constants are copied verbatim from
// MagicUtil.java so the resulting indexing is bit-identical.

#include "magic.h"

#include <vector>

namespace bagatur::magic {

std::array<BB, 64>  rook_movement_masks{};
std::array<BB, 64>  bishop_movement_masks{};
std::array<U32, 64> rook_shifts{};
std::array<U32, 64> bishop_shifts{};
std::array<BB*, 64> rook_magic_moves{};
std::array<BB*, 64> bishop_magic_moves{};

// Backing storage for the tables. Tables are variable-sized per square; total
// ≈ 800 KB (rook) + 40 KB (bishop). Allocated once during init().
namespace {

std::vector<BB> rook_table_storage;
std::vector<BB> bishop_table_storage;

void calc_rook_masks() noexcept {
    for (int index = 0; index < 64; ++index) {
        // up
        for (int j = index + 8; j < 64 - 8; j += 8)
            rook_movement_masks[index] |= 1ULL << j;
        // down
        for (int j = index - 8; j >= 8; j -= 8)
            rook_movement_masks[index] |= 1ULL << j;
        // left
        for (int j = index + 1; j % 8 != 0 && j % 8 != 7; ++j)
            rook_movement_masks[index] |= 1ULL << j;
        // right
        for (int j = index - 1; j % 8 != 7 && j % 8 != 0 && j > 0; --j)
            rook_movement_masks[index] |= 1ULL << j;
    }
}

void calc_bishop_masks() noexcept {
    for (int index = 0; index < 64; ++index) {
        for (int j = index + 7; j < 64 - 7 && j % 8 != 7 && j % 8 != 0; j += 7)
            bishop_movement_masks[index] |= 1ULL << j;
        for (int j = index + 9; j < 64 - 9 && j % 8 != 7 && j % 8 != 0; j += 9)
            bishop_movement_masks[index] |= 1ULL << j;
        for (int j = index - 9; j >= 9 && j % 8 != 7 && j % 8 != 0; j -= 9)
            bishop_movement_masks[index] |= 1ULL << j;
        for (int j = index - 7; j >= 7 && j % 8 != 7 && j % 8 != 0; j -= 7)
            bishop_movement_masks[index] |= 1ULL << j;
    }
}

void calc_shifts() noexcept {
    for (int i = 0; i < 64; ++i) {
        rook_shifts[i]   = 64u - static_cast<U32>(popcount(rook_movement_masks[i]));
        bishop_shifts[i] = 64u - static_cast<U32>(popcount(bishop_movement_masks[i]));
    }
}

// Enumerate all 2^bitcount subsets of `mask` — Carry-Rippler trick.
std::vector<BB> enumerate_occupancy(BB mask) {
    int bits = popcount(mask);
    std::vector<BB> result;
    result.reserve(1u << bits);
    BB occ = 0;
    do {
        result.push_back(occ);
        occ = (occ - mask) & mask;
    } while (occ != 0);
    return result;
}

BB slide_rook(int from, BB occupancy) noexcept {
    BB moves = 0;
    for (int j = from + 8; j < 64; j += 8) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    for (int j = from - 8; j >= 0; j -= 8) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    for (int j = from + 1; j % 8 != 0; ++j) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    for (int j = from - 1; j % 8 != 7 && j >= 0; --j) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    return moves;
}

BB slide_bishop(int from, BB occupancy) noexcept {
    BB moves = 0;
    for (int j = from + 7; j % 8 != 7 && j < 64; j += 7) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    for (int j = from + 9; j % 8 != 0 && j < 64; j += 9) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    for (int j = from - 9; j % 8 != 7 && j >= 0; j -= 9) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    for (int j = from - 7; j % 8 != 0 && j >= 0; j -= 7) { moves |= 1ULL << j; if (occupancy & (1ULL << j)) break; }
    return moves;
}

void build_tables() {
    // First pass: compute totals so we can reserve flat storage.
    std::size_t rook_total = 0, bishop_total = 0;
    for (int i = 0; i < 64; ++i) {
        rook_total   += 1ull << popcount(rook_movement_masks[i]);
        bishop_total += 1ull << popcount(bishop_movement_masks[i]);
    }
    rook_table_storage.assign(rook_total, 0);
    bishop_table_storage.assign(bishop_total, 0);

    std::size_t rook_off = 0, bishop_off = 0;
    for (int i = 0; i < 64; ++i) {
        std::size_t rook_size   = 1ull << popcount(rook_movement_masks[i]);
        std::size_t bishop_size = 1ull << popcount(bishop_movement_masks[i]);
        rook_magic_moves[i]   = rook_table_storage.data()   + rook_off;
        bishop_magic_moves[i] = bishop_table_storage.data() + bishop_off;
        rook_off   += rook_size;
        bishop_off += bishop_size;
    }

    for (int i = 0; i < 64; ++i) {
        auto rook_occs = enumerate_occupancy(rook_movement_masks[i]);
        for (BB occ : rook_occs) {
            U32 idx = static_cast<U32>((occ * rook_magic_numbers[i]) >> rook_shifts[i]);
            rook_magic_moves[i][idx] = slide_rook(i, occ);
        }
        auto bishop_occs = enumerate_occupancy(bishop_movement_masks[i]);
        for (BB occ : bishop_occs) {
            U32 idx = static_cast<U32>((occ * bishop_magic_numbers[i]) >> bishop_shifts[i]);
            bishop_magic_moves[i][idx] = slide_bishop(i, occ);
        }
    }
}

}  // anonymous

// Magic numbers copied verbatim from MagicUtil.java.
std::array<BB, 64> rook_magic_numbers = {
    0xa180022080400230ULL, 0x40100040022000ULL,   0x80088020001002ULL,   0x80080280841000ULL,
    0x4200042010460008ULL, 0x4800a0003040080ULL,  0x400110082041008ULL,  0x8000a041000880ULL,
    0x10138001a080c010ULL, 0x804008200480ULL,     0x10011012000c0ULL,    0x22004128102200ULL,
    0x200081201200cULL,    0x202a001048460004ULL, 0x81000100420004ULL,   0x4000800380004500ULL,
    0x208002904001ULL,     0x90004040026008ULL,   0x208808010002001ULL,  0x2002020020704940ULL,
    0x8048010008110005ULL, 0x6820808004002200ULL, 0xa80040008023011ULL,  0xb1460000811044ULL,
    0x4204400080008ea0ULL, 0xb002400180200184ULL, 0x2020200080100380ULL, 0x10080080100080ULL,
    0x2204080080800400ULL, 0xa40080360080ULL,     0x2040604002810b1ULL,  0x8c218600004104ULL,
    0x8180004000402000ULL, 0x488c402000401001ULL, 0x4018a00080801004ULL, 0x1230002105001008ULL,
    0x8904800800800400ULL, 0x42000c42003810ULL,   0x8408110400b012ULL,   0x18086182000401ULL,
    0x2240088020c28000ULL, 0x1001201040c004ULL,   0xa02008010420020ULL,  0x10003009010060ULL,
    0x4008008008014ULL,    0x80020004008080ULL,   0x282020001008080ULL,  0x50000181204a0004ULL,
    0x102042111804200ULL,  0x40002010004001c0ULL, 0x19220045508200ULL,   0x20030010060a900ULL,
    0x8018028040080ULL,    0x88240002008080ULL,   0x10301802830400ULL,   0x332a4081140200ULL,
    0x8080010a601241ULL,   0x1008010400021ULL,    0x4082001007241ULL,    0x211009001200509ULL,
    0x8015001002441801ULL, 0x801000804000603ULL,  0xc0900220024a401ULL,  0x1000200608243ULL,
};

std::array<BB, 64> bishop_magic_numbers = {
    0x2910054208004104ULL, 0x2100630a7020180ULL,  0x5822022042000000ULL, 0x2ca804a100200020ULL,
    0x204042200000900ULL,  0x2002121024000002ULL, 0x80404104202000e8ULL, 0x812a020205010840ULL,
    0x8005181184080048ULL, 0x1001c20208010101ULL, 0x1001080204002100ULL, 0x1810080489021800ULL,
    0x62040420010a00ULL,   0x5028043004300020ULL, 0xc0080a4402605002ULL, 0x8a00a0104220200ULL,
    0x940000410821212ULL,  0x1808024a280210ULL,   0x40c0422080a0598ULL,  0x4228020082004050ULL,
    0x200800400e00100ULL,  0x20b001230021040ULL,  0x90a0201900c00ULL,    0x4940120a0a0108ULL,
    0x20208050a42180ULL,   0x1004804b280200ULL,   0x2048020024040010ULL, 0x102c04004010200ULL,
    0x20408204c002010ULL,  0x2411100020080c1ULL,  0x102a008084042100ULL, 0x941030000a09846ULL,
    0x244100800400200ULL,  0x4000901010080696ULL, 0x280404180020ULL,     0x800042008240100ULL,
    0x220008400088020ULL,  0x4020182000904c9ULL,  0x23010400020600ULL,   0x41040020110302ULL,
    0x412101004020818ULL,  0x8022080a09404208ULL, 0x1401210240484800ULL, 0x22244208010080ULL,
    0x1105040104000210ULL, 0x2040088800c40081ULL, 0x8184810252000400ULL, 0x4004610041002200ULL,
    0x40201a444400810ULL,  0x4611010802020008ULL, 0x80000b0401040402ULL, 0x20004821880a00ULL,
    0x8200002022440100ULL, 0x9431801010068ULL,    0x1040c20806108040ULL, 0x804901403022a40ULL,
    0x2400202602104000ULL, 0x208520209440204ULL,  0x40c000022013020ULL,  0x2000104000420600ULL,
    0x400000260142410ULL,  0x800633408100500ULL,  0x2404080a1410ULL,     0x138200122002900ULL,
};

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    calc_rook_masks();
    calc_bishop_masks();
    calc_shifts();
    build_tables();
}

}  // namespace bagatur::magic
