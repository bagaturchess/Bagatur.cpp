# `syzygy` — Syzygy endgame tablebase probing

Wraps **[Fathom](https://github.com/jdart1/Fathom)** (the standalone Syzygy
prober used by the Java Bagatur via JNI) so the C++ search can consult
Win/Draw/Loss and Distance-To-Zero tables. In C++ Fathom links directly — no JNI
bridge. Enabled at runtime with the `SyzygyPath` UCI option; a miss (no path, or
too many pieces) leaves the search untouched.

## Files

```
src/syzygy/
├── tbprobe.{c,h}   Fathom probe core (tbprobe.c #includes tbchess.c — one TU)
├── tbchess.c       Fathom move-gen / attack tables used by the prober
├── tbconfig.h      Fathom build config (TB_PIECES = 7 → up to 7-man tables)
├── stdendian.h     endian helpers used by tbprobe.c
└── syzygy.{h,cpp}  the C++ wrapper (class syzygy::Syzygy) the engine calls
```

Only `syzygy.{h,cpp}` is ours; the rest is vendored Fathom (kept verbatim bar
`tbconfig.h`). The Fathom CLI driver (`apps/fathom.c`) was removed — it has its
own `main()` and is not part of the engine.

## The wrapper — `syzygy::Syzygy`

A process-wide singleton called directly with a `board::ChessBoard`:

```cpp
auto& tb = syzygy::Syzygy::instance();
tb.init("C:\\tb\\syzygy");     // SyzygyPath; "<empty>"/"" unloads
if (tb.available()) {          // true once ≥1 table loaded
    int wdl = tb.probe_wdl(cb);           // in search: WDL_LOSS..WDL_WIN or WDL_FAIL
    int mv  = tb.probe_root(cb, wdl);     // at root: Bagatur move (0 on miss) + WDL
}
```

| Method | Use | Thread-safe |
| ------ | --- | ----------- |
| `init(path)` / `shutdown()` | (un)load tables; `init` sets `max_pieces()` from `TB_LARGEST` | no — call while idle |
| `available()` / `max_pieces()` | is anything loaded / largest table size | yes (plain reads) |
| `probe_wdl(cb)` | in-search WDL cutoff | **yes** — SMP workers probe concurrently |
| `probe_root(cb, wdl)` | root DTZ move (respects the 50-move rule) | **no** — root only, off the search threads |

`probe_wdl` returns `WDL_FAIL` unless `castlingRights == 0`, `rule50 == 0` and
`popcount(allPieces) <= max_pieces()` (Fathom itself rejects castling / rule50 ≠ 0).

## Square orientation — the one subtlety

Fathom expects the standard `A1 = 0` layout; Bagatur's board is **file-reversed**
(`H1 = 0`, `A1 = 7`). Every bitboard and the ep square are horizontally mirrored
(`sq ^ 7`, i.e. flip the file) on the way into Fathom, and the suggested root
move's from/to squares are mirrored back:

```
Bagatur:  H1=0 … A1=7        (file bits reversed)
Fathom:   A1=0 … H1=7        (standard)     →  mirror_files() / sq ^ 7
```

Ranks and side-to-move are already aligned, so only the files flip. A wrong
mirror would surface immediately: `probe_root` would fail to match a legal move
(the move generator rejects it) and yield no `tbhits` — the reason the end-to-end
KQvK / KRvKR checks below are a sufficient orientation test.

## How the engine uses it

* **In search** ([`search.cpp`](../search/search.cpp)) — at non-root nodes,
  `probe_wdl` gives an exact win/draw/loss verdict. The score is returned as a
  cutoff and stored `TT_EXACT` with a boosted depth. Win/loss use the
  `search::TB_WIN_SCORE` band — below `MATE_THRESHOLD` (a real mate is always
  preferred) but far above any eval — with a per-ply term so the win is delivered
  sooner / the loss delayed. Cursed-win / blessed-loss collapse to a draw.
* **At the root** ([`state_manager.cpp`](../uci/state_manager.cpp)
  `cmd_go`) — before any worker thread starts (so the non-thread-safe
  `tb_probe_root` runs single-threaded), a DTZ probe plays the optimal move
  immediately, emitting one `info … tbhits 1` line + `bestmove`, skipping search.
* **`tbhits`** is counted per search and reported on every info line (summed
  across SMP workers in `SearchersMerge`).

## Getting the tablebase files

Fathom only *probes* the tables; the `.rtbw` (WDL) and `.rtbz` (DTZ) files are a
separate download. 3–4–5-man is ~1 GB, 6-man ~150 GB, 7-man far larger. Point
`SyzygyPath` at the directory (`;`-separated on Windows). `TB_PIECES = 7` in
`tbconfig.h` sets the compile-time ceiling; the runtime `max_pieces()` reflects
whatever is actually present.

## License

Fathom is MIT-licensed (basil / Jon Dart), which is compatible with Bagatur.cpp's
GPLv3. `syzygy.{h,cpp}` is part of Bagatur.cpp under GPLv3.

## Smoke test

```text
setoption name SyzygyPath value <dir with .rtbw/.rtbz>
position fen 8/8/8/4k3/8/8/4K3/4Q3 w - - 0 1
go depth 18
info depth 1 seldepth 1 nodes 0 ... tbhits 1 score cp 29743 pv e2d3
bestmove e2d3                       # KQvK: instant DTZ win move

position fen 4k3/7r/8/8/8/8/R7/4K3 w - - 0 1
go depth 18
info depth 1 ... tbhits 1 score cp 0 pv e1f1
bestmove e1f1                       # KRvKR: draw (score 0)
```

For a position with more men than the largest table, the root probe is skipped
and the in-search WDL probe fires as the tree drops into table range — `tbhits`
climbs across depths.
