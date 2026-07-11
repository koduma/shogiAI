# ShogiAI

A minimal, working Japanese shogi engine written in C++17, designed as a strong
long-term foundation. It speaks the **USI** protocol so it can be connected to any
compatible shogi GUI (Shogidroid, ShogiGUI, etc.).

---

## Features

| Area | What's implemented |
|---|---|
| **Rules** | Full 9×9 board, all piece types & promotions, drops, check/checkmate detection, nifu (two-pawn) rule, pawn-drop-mate ban, forced-promotion zones |
| **Search** | Iterative-deepening alpha-beta (negamax), basic MVV-LVA move ordering |
| **Evaluation** | Material count (centipawn values), symmetric |
| **Protocol** | USI: `usi`, `isready`, `position startpos/sfen`, `go`, `stop`, `quit` |
| **Repetition** | Sennichite detection via Zobrist hashing (returns draw score) |

---

## Building

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
bestmove 7g7f
quit
```

### Connecting to a GUI (e.g. ShogiGUI on Windows)

1. Open **Tool → Engines → Add**.
2. Point it at the `shogi_engine` binary.
3. Start a game — the engine will respond to `go` commands automatically.

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
│   ├── search.cpp     # Iterative-deepening alpha-beta
│   └── main.cpp       # USI protocol main loop
└── tests/
    └── test_main.cpp  # Unit tests (no external framework required)
```

---

## Design Notes

* **Square encoding** — `sq = rank × 9 + file`; file 0 = USI file 9 (left), file 8 = USI file 1 (right); rank 0 = rank 'a' (top).
* **Piece encoding** — `Piece = (color << 4) | piece_type`; BLACK = 0, WHITE = 1.
* **Undo stack** — `StateInfo` (captured piece + previous Zobrist hash) is pushed on every `do_move` and popped on `undo_move`.
* **Evaluation** — Currently pure material; designed to be replaced with a neural-network or hand-crafted positional evaluation without touching the search or board code.
* **Future improvements** — Transposition table, quiescence search, null-move pruning, NNUE evaluation.

---

## License

MIT
