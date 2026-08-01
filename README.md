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
| **Evaluation** | Material + compact KPP-style (King-Piece-Piece) interaction; external KPP parameter file with auto-discovery |
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

## 評価関数ファイル（外部評価パラメータ）

### 概要

本エンジンは「コンパクト KPP スタイル（King-Piece-Piece 相互作用）」の評価関数を内蔵しています。
外部ファイルを用意することでパラメータを上書きできます。

> **重要な注意**
> 本エンジンが対応しているのは、このリポジトリで定義した**独自のコンパクト KPP テキスト形式**のみです。
> YaneuraOu や他のソフトの `.bin` 形式のファイルや、NNUE 重みファイルは直接読み込めません（後述）。
> 評価ファイルはランタイムにネットワークからダウンロードしません。
> ユーザーが手動でファイルを配置することで自動検出されます。

---

### 自動検出（オートディスカバリ）

`setoption` や環境変数で明示指定がない場合、起動時の作業ディレクトリから以下の順に探索します：

| 優先順位 | パス | 典型的なシナリオ |
|---|---|---|
| 1 | `src/eval/kpp_weights.txt` | リポジトリルートから実行 |
| 2 | `eval/kpp_weights.txt` | ビルドディレクトリから実行（CMake が自動コピー） |

CMake でビルドすると `src/eval/` の内容が `build/eval/` にコピーされるため、
`./build/shogi_engine` はビルドディレクトリから正しく評価ファイルを見つけられます。

**注意：** 自動検出はランタイムのダウンロードを行いません。ファイルをリポジトリの
`src/eval/` 以下に配置してください。

---

### 明示指定（優先順位）

評価ファイルの読み込み優先順位：

```
1. setoption name EvalFile value <path>  ←最優先
2. 環境変数 SHOGIAI_EVAL_FILE=<path>
3. 自動検出（上記の順）
4. 内蔵 KPP フォールバック（ファイルなし）
```

```bash
# 環境変数で指定する例
SHOGIAI_EVAL_FILE=/path/to/my_kpp.txt ./build/shogi_engine

# USI setoption で指定する例（GUI から設定することが多い）
setoption name EvalFile value /path/to/my_kpp.txt
```

`setoption name EvalFile value ` （値を空に）すると自動検出に戻ります。

---

### 評価ファイルの形式（KPP テキスト形式）

```
# コメント行（# で始まる行）は無視されます
model_type=kpp                  # 必須：kpp または three-piece-relation

# スカラーパラメータ
kpp_weight=12
king_zone_bonus=6

# 駒の価値（センチポーン単位）
piece_pawn=100
piece_lance=300
piece_knight=300
piece_silver=500
piece_gold=600
piece_bishop=800
piece_rook=1000
piece_prom_pawn=600
piece_prom_lance=600
piece_prom_knight=600
piece_prom_silver=600
piece_prom_bishop=1100
piece_prom_rook=1300
```

- `model_type=kpp` または `model_type=three-piece-relation` のみ対応。
- 不明なキーは無視されます（前方互換性のため）。
- 省略されたキーは内蔵のデフォルト値が使われます。
- サンプルファイル：[src/eval/kpp_weights.txt](src/eval/kpp_weights.txt)

---

### 評価ステータスメッセージ

`go` コマンドの直前に `info string <status>` が出力されます：

| ステータス | 意味 |
|---|---|
| `kpp: loaded from <path> [explicit]` | 明示指定のファイルを読み込んだ |
| `kpp: loaded from <path> [auto-discovered]` | 自動検出でファイルを見つけた |
| `kpp: built-in fallback` | ファイルなし、内蔵パラメータを使用 |
| `kpp: built-in fallback (file not found: ...)` | 指定ファイルが見つからない |
| `kpp: built-in fallback (parse error: ...)` | ファイルは存在するが有効なキーが見つからない |
| `unsupported evaluator: nnue (falling back to built-in kpp; ...)` | NNUE ファイルを検出、非対応 |

---

### コンパクト KPP vs 本格的な3駒関係評価 vs NNUE の違い

| 評価手法 | このエンジンの対応 | 説明 |
|---|---|---|
| **コンパクト KPP**（本エンジン） | ✅ 完全対応 | King + Piece + Piece の距離ベース相互作用スコア。フルテーブルではなくスカラーパラメータ数個で表現 |
| **本格的な3駒関係 (KPP/KKP/KPPT)** | ❌ 非対応 | KPP テーブル（駒の全組み合わせ × 王位置）を持つ大規模テーブル評価。YaneuraOu 等で使われる |
| **NNUE** | ❌ 非対応（検出のみ） | Neural Network Unified Evaluator。独自の特徴量エンコーディングとインクリメンタル計算が必要 |

---

### 外部評価実装への参照

以下は参照のみです。これらのコードやウェイトファイルは**本エンジンでは直接使用できません**。

| プロジェクト | URL | ライセンス |
|---|---|---|
| ynasu87/nnue（将棋 NNUE の先駆的実装） | https://github.com/ynasu87/nnue | GPLv3 |
| YaneuraOu（強豪将棋エンジン、NNUE 評価含む） | https://github.com/yaneurao/YaneuraOu | GPLv3 |
| YaneuraOu NNUE ドキュメント | https://github.com/yaneurao/YaneuraOu/blob/master/docs/解説.md | GPLv3 |

> **ライセンス注意：** 上記プロジェクトは GPLv3 ライセンスです。本リポジトリは現在ライセンスを明示していません。
> 将来的に NNUE を本格対応する場合は、ライセンスの整合性を確認してください。

---

### 将来の NNUE 対応に向けて

真の NNUE 対応を実装したい場合は、以下のすべてが必要です：

1. **特徴量エンコーディングの移植** — YaneuraOu や ynasu87/nnue と互換性のある HalfKP または HalfKAv2 特徴量変換
2. **モデルローダーの実装** — バイナリ重みファイルのパーサー（アーキテクチャ固有）
3. **インクリメンタル計算** — アキュムレータの差分更新（NNUE 高速化の要）
4. **ライセンスの確認** — 使用するコードや重みファイルのライセンスが本プロジェクトと整合するか確認
5. **本エンジン改修** — `model_type=nnue` をパースして `EvalFamily::NNUE` でディスパッチする

現時点では `model_type=nnue` のファイルを検出した場合、「unsupported evaluator: nnue」ステータスを返し、
内蔵 KPP フォールバックを使用します。NNUE が「有効」であるかのような誤情報は出力しません。

---

## Project Structure

```
shogiAI/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── eval/
│   │   └── kpp_weights.txt  # Sample KPP parameter file (checked in; safe for VCS)
│   ├── types.hpp      # Square, Color, PieceType, Piece, Move primitives
│   ├── board.hpp      # Board class declaration + Zobrist namespace
│   ├── board.cpp      # Board state, SFEN parse/print, do/undo move, attack detection
│   ├── movegen.hpp    # Legal move generation interface
│   ├── movegen.cpp    # Pseudo-legal generator + legality filter
│   ├── eval.hpp       # Evaluation function interface + EvalFamily enum
│   ├── eval.cpp       # KPP evaluation, auto-discovery, model-family detection
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
* **Evaluation** — Built-in compact KPP-style評価（King + Piece + Piece の距離ベース相互作用）。外部テキストファイルでパラメータを上書き可能。
* **Eval file auto-discovery** — 起動ディレクトリから `src/eval/kpp_weights.txt`、次に `eval/kpp_weights.txt` の順に探索。  
  `setoption name EvalFile value <path>` または `SHOGIAI_EVAL_FILE` 環境変数で明示指定すると自動検出より優先。  
  ファイルが無い・不正・非対応形式の場合は**安全に内蔵 KPP フォールバック**へ自動切替します（探索は継続）。
* **EvalFamily** — `EvalFamily::KPP`（外部ファイルから読み込み）/ `FALLBACK`（内蔵デフォルト）/ `NNUE_UNSUPPORTED`（NNUE ファイル検出、非対応）の 3 種類。NNUE ファイルを検出しても KPP として誤報告しません。
* **Eval file format** — `key=value` プレーンテキスト。`model_type=kpp`（または `three-piece-relation`）が必須識別子。  
  `kpp_weight=12`, `king_zone_bonus=6`, `piece_pawn=100` など。詳細は `src/eval/kpp_weights.txt` 参照。
* **License / source** — リポジトリ同梱コードは MIT。外部評価ファイルを利用する場合は、配布元ライセンスとの整合性を確認してください。
* **Search details** — 盤面 Zobrist hash を使う固定サイズの置換表、capture/promotion 中心の quiescence search、
  hash move → captures/promotions → killer moves → history heuristic の順序付けを使います。
* **Pruning** — 以前の「静的評価だけで `±INF` を返す」しきい値枝刈りは廃止し、
  葉では quiescence search で戦術的な取り返しや成りを確認します。

---

## License

MIT
