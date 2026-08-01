# ShogiAI

A minimal, working Japanese shogi engine written in C++17, designed as a strong
long-term foundation. It speaks the **USI** protocol so it can be connected to any
compatible shogi GUI (Shogidroid, ShogiGUI, etc.).

---

## Features

| Area | What's implemented |
|---|---|
| **Rules** | Full 9×9 board, all piece types & promotions, drops, check/checkmate detection, nifu (two-pawn) rule, pawn-drop-mate ban, forced-promotion zones |
| **Search** | Iterative-deepening alpha-beta, bounded transposition table, quiescence search, improved move ordering, pruning stats, USI info output |
| **Evaluation** | Material + lightweight KPP-style (King-Piece-Piece) interaction, optional external eval parameter file |
| **Protocol** | USI: `usi`, `isready`, `position startpos/sfen`, `go`, `stop`, `quit` |
| **Repetition** | Sennichite detection via Zobrist hashing (returns draw score) |

---

## Building

### g++ command
g++ -std=c++17 -O2 -Wall -Wextra src\main.cpp src\board.cpp src\movegen.cpp src\eval.cpp src\search.cpp -Isrc -o shogi_engine.exe


### Requirements
- CMake ≥ 3.14
- A C++17-capable compiler (GCC ≥ 7, Clang ≥ 5, MSVC 2017+)

### Steps

```bash
# 1. Clone and enter the repository
git clone https://github.com/koduma/shogiAI.git
cd shogiAI

# 2. Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build
cmake --build build

# 4. (Optional) Run tests
cmake --build build && ./build/shogi_tests
```

The build produces two binaries inside `build/`:

| Binary | Purpose |
|---|---|
| `shogi_engine` | USI engine (connect to a GUI or run interactively) |
| `shogi_tests`  | Unit-test suite |

---

## Running

### Interactive USI session

```
$ ./build/shogi_engine
usi
id name ShogiEngine
id author ShogiAI
usiok
isready
readyok
position startpos
go movetime 2000
info depth 1 seldepth 1 time 5 nodes 1234 nps 246800 score cp 12 pv 7g7f
info depth 2 seldepth 4 time 19 nodes 9650 nps 507894 score cp 25 pv 7g7f 3c3d
bestmove 7g7f
quit
```

### Connecting to a GUI (e.g. ShogiGUI on Windows)

1. Open **Tool → Engines → Add**.
2. Point it at the `shogi_engine` binary.
3. Start a game — the engine will respond to `go` commands automatically.
4. 読み筋/評価値表示を有効化したい場合は、ShogiGUI のエンジン情報欄で USI `info` 行を表示してください
   （本エンジンは `depth` / `seldepth` / `time` / `nodes` / `nps` / `score cp|mate` / `pv` を出力します）。

#### Time management notes (ShogiGUI / Windows)

- Leave **usi_ponder** unchecked (☐ False) — ponder is not implemented.
- The engine uses approximately **1/40 of remaining time per move, capped at 5 seconds**.
  For a 10-minute game this means at most ~5 s per move, so time-outs should not occur.
- For very short time controls (e.g. 10-second byoyomi), the engine uses 90 % of
  the byoyomi period, leaving a small safety margin.
- `go movetime N` is also supported; the engine will return within roughly `N` ms.

---

## Project Structure

```
shogiAI/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── types.hpp      # Square, Color, PieceType, Piece, Move primitives
│   ├── board.hpp      # Board class declaration + Zobrist namespace
│   ├── board.cpp      # Board state, SFEN parse/print, do/undo move, attack detection
│   ├── movegen.hpp    # Legal move generation interface
│   ├── movegen.cpp    # Pseudo-legal generator + legality filter
│   ├── eval.hpp       # Evaluation function interface
│   ├── eval.cpp       # Material evaluation
│   ├── search.hpp     # Search interface + g_stop flag
│   ├── search.cpp     # Iterative-deepening alpha-beta + TT + quiescence
│   └── main.cpp       # USI protocol main loop
└── tests/
    └── test_main.cpp  # Unit tests (no external framework required)
```

---

## Design Notes

* **Square encoding** — `sq = rank × 9 + file`; file 0 = USI file 9 (left), file 8 = USI file 1 (right); rank 0 = rank 'a' (top).
* **Piece encoding** — `Piece = (color << 4) | piece_type`; BLACK = 0, WHITE = 1.
* **Undo stack** — `StateInfo` (captured piece + previous Zobrist hash) is pushed on every `do_move` and popped on `undo_move`.
* **Evaluation** — Built-in lightweight KPP-style評価（King + Piece + Piece の関係）を使用。加えて外部評価パラメータファイルを読み込めます。
* **Eval file format / fallback** — 既定パスは `eval/kpp_weights.txt`（`setoption name EvalFile value <path>` または `SHOGIAI_EVAL_FILE` で変更可）。  
  ファイルが無い・不正な場合は、**安全に built-in KPP フォールバック**へ自動切替します（探索は継続）。
* **Eval file example keys** — `kpp_weight=12`, `king_zone_bonus=6`, `piece_pawn=100`, `piece_prom_rook=1300` などの `key=value` 形式。
* **License / source** — リポジトリ同梱コードは MIT。外部評価ファイルを利用する場合は、配布元ライセンスとの整合性を確認してください。
* **Search details** — 盤面 Zobrist hash を使う固定サイズの置換表、capture/promotion 中心の quiescence search、
  hash move → captures/promotions → killer moves → history heuristic の順序付けを使います。
* **Pruning** — 以前の「静的評価だけで `±INF` を返す」しきい値枝刈りは廃止し、
  葉では quiescence search で戦術的な取り返しや成りを確認します。

---

## License

MIT
