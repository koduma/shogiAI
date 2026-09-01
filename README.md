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

## 評価関数ファイル（Bonanza 6.0 fv.bin / 3駒関係評価）

### 概要

本エンジンは「Bonanza 6.0 形式の fv.bin」を読み込む、本格的な3駒関係評価関数
（KPP: King-Piece-Piece / KKP: King-King-Piece / KK / KP テーブル）に対応しています。

> **重要な注意**
> リポジトリ同梱の `src/eval/fv.bin` は **1バイトのダミーファイル** です。
> 実際に評価関数を有効化するには、ユーザーが手元に持っている
> **Bonanza 6.0 の本物の fv.bin（bonanza_v6.0.zip 由来、無改変）** を
> `src/eval/fv.bin` に上書き配置するか、`setoption` / 環境変数で別の場所を指定してください。
> 170MB 前後になる本物の fv.bin は本リポジトリには含まれておらず、含める予定もありません
> （Git に巨大バイナリを追加しない方針のため）。
> 評価ファイルはランタイムにネットワークからダウンロードしません。

---

### fv.bin のフォーマット（実装の根拠）

このエンジンが期待するバイト列は、**推測ではなく** 実在する Bonanza→Apery 変換ツール
（[HiraokaTakuya/apery](https://github.com/HiraokaTakuya/apery) の
`utils/bonanzatoapery/main.cpp`）が実際に読み込んでいる、本物の Bonanza 6 fv.bin の構造を
そのまま踏襲しています。具体的には、以下の4つのテーブルをこの順番で連結したバイナリです：

| # | テーブル | 型 | 要素数 | バイト数 |
|---|---|---|---|---|
| 1 | KPP（王・駒・駒の3駒関係） | `int16_t` | `81 × pos_n`（`pos_n = fe_end×(fe_end+1)/2`） | 176,584,212 (~168.4 MiB) |
| 2 | KKP（王・王・駒） | `int32_t` | `81 × 81 × fe_end` | 38,736,144 (~36.9 MiB) |
| 3 | KK（王・王） | `int32_t` | `81 × 81` | 26,244 |
| 4 | KP（王・駒） | `int32_t` | `81 × fe_end` | 478,224 |

ここで `fe_end = 1476`（Bonanza の特徴量空間のサイズ。持ち駒 90 + 盤上駒 14種×81マス）。
**合計 215,824,824 バイト（約205.8 MiB）** がこのエンジンが要求する fv.bin の正確なサイズです。
KPP テーブル単体で約168MBあるため、「手元の fv.bin はだいたい170MB」という体感とも一致します。

> **本サンドボックス環境での制約：** 本物の約170MBの fv.bin はこの開発環境に存在しないため、
> 上記フォーマットに対するバイト単位の実データ検証（数値の妥当性チェック）は行えていません。
> 検証できているのは (a) ファイルサイズ／読み込みエラーの検出ロジック、および
> (b) 全要素ゼロの同サイズファイルを読み込んだ場合に `evaluate()` が常に 0 を返すことです。
> 実際の重み配置後は `info string <status>` の内容（読み込み成功・サイズ不一致など）で
> 状態を必ず確認してください。

評価式（3駒関係）は、着手側 `us` の視点から見た単一方向の合計として実装しています：

```
score = KK[my_king][opp_king]
      + Σ_i KP[my_king][feature_i]
      + Σ_i KKP[my_king][opp_king][feature_i]
      + Σ_{i<j} KPP[my_king][feature_i][feature_j]
```

`feature_i` は玉を除く盤上・持ち駒すべての駒を、Bonanza の f_/e_（味方/敵）特徴量方式でエンコードしたものです。
オリジナルの Bonanza/Apery が行う「相手玉視点の KPP を鏡像計算して減算する」という二重パスは実装していません
（本物の fv.bin なしに数値的な正しさを検証できないため、意図的な簡略化です）。

---

### 自動検出（オートディスカバリ）

`setoption` や環境変数で明示指定がない場合、起動時の作業ディレクトリから以下の順に探索します：

| 優先順位 | パス | 典型的なシナリオ |
|---|---|---|
| 1 | `src/eval/fv.bin` | リポジトリルートから実行 |
| 2 | `eval/fv.bin` | ビルドディレクトリから実行（CMake が自動コピー） |

CMake でビルドすると `src/eval/` の内容が `build/eval/` にコピーされるため、
`./build/shogi_engine` はビルドディレクトリから正しく評価ファイルを見つけられます。

**注意：** 自動検出はランタイムのダウンロードを行いません。本物の fv.bin をリポジトリの
`src/eval/` 以下に配置してください。

---

### 明示指定（優先順位）

評価ファイルの読み込み優先順位：

```
1. setoption name EvalFile value <path>  ←最優先
2. 環境変数 SHOGIAI_EVAL_FILE=<path>
3. 自動検出（上記の順）
4. 内蔵の駒割のみフォールバック（ファイルなし・不正）
```

```bash
# 環境変数で指定する例
SHOGIAI_EVAL_FILE=/path/to/fv.bin ./build/shogi_engine

# USI setoption で指定する例（GUI から設定することが多い）
setoption name EvalFile value /path/to/fv.bin
```

`setoption name EvalFile value ` （値を空に）すると自動検出に戻ります。

---

### 評価ステータスメッセージ

`go` コマンドの直前に `info string <status>` が出力されます：

| ステータス | 意味 |
|---|---|
| `bonanza-v6 fv.bin loaded from <path> [explicit]` | 明示指定のファイルを読み込んだ |
| `bonanza-v6 fv.bin loaded from <path> [auto-discovered]` | 自動検出でファイルを見つけた |
| `material-only fallback (fv.bin missing)` | ファイルなし、駒割のみで評価 |
| `material-only fallback (file not found: ...)` | 指定ファイルが見つからない |
| `material-only fallback (invalid Bonanza v6 fv.bin size: got X bytes, expected Y bytes; ...)` | サイズが一致しない（ダミー fv.bin を含む） |
| `material-only fallback (failed to read fv.bin: ...)` | 読み込み中に I/O エラー |
| `unsupported evaluator: nnue (...)` | NNUE らしきファイルを検出、非対応 |

同梱の `src/eval/fv.bin`（1バイトのダミー）は常に「サイズ不一致」メッセージとなり、
`EvalFamily::MATERIAL_FALLBACK` にフォールバックします。

---

### コンパクト評価 vs 本格的な3駒関係評価 vs NNUE の違い

| 評価手法 | このエンジンの対応 | 説明 |
|---|---|---|
| **本格的な3駒関係 (KPP/KKP)**（本エンジン） | ✅ 対応（要・本物の fv.bin） | Bonanza 6.0 の fv.bin テーブルをそのまま読み込み使用 |
| **駒割のみフォールバック** | ✅ 対応 | fv.bin が無い・不正なときの内蔵デフォルト |
| **NNUE** | ❌ 非対応（検出のみ） | Neural Network Unified Evaluator。独自の特徴量エンコーディングとインクリメンタル計算が必要 |

---

### 外部評価実装への参照

以下は参照のみです。これらのコードやウェイトファイルは**本エンジンでは直接使用できません**（NNUE の場合）。

| プロジェクト | URL | ライセンス |
|---|---|---|
| HiraokaTakuya/apery（Bonanza→Apery fv.bin 変換ツールを含む） | https://github.com/HiraokaTakuya/apery | GPLv3 |
| ynasu87/nnue（将棋 NNUE の先駆的実装） | https://github.com/ynasu87/nnue | GPLv3 |
| YaneuraOu（強豪将棋エンジン、NNUE 評価含む） | https://github.com/yaneurao/YaneuraOu | GPLv3 |

> **ライセンス注意：** 上記プロジェクトは GPLv3 ライセンスです。本リポジトリは現在ライセンスを明示していません。
> 本物の Bonanza 6.0 fv.bin を使用する場合は、Bonanza のライセンス条件をご自身で確認してください。

---

## Project Structure

```
shogiAI/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── eval/
│   │   └── fv.bin         # Dummy placeholder (1 byte); replace with a real Bonanza 6.0 fv.bin
│   ├── types.hpp      # Square, Color, PieceType, Piece, Move primitives
│   ├── board.hpp      # Board class declaration + Zobrist namespace
│   ├── board.cpp      # Board state, SFEN parse/print, do/undo move, attack detection
│   ├── movegen.hpp    # Legal move generation interface
│   ├── movegen.cpp    # Pseudo-legal generator + legality filter
│   ├── eval.hpp       # Evaluation function interface + EvalFamily enum
│   ├── eval.cpp       # Bonanza v6 fv.bin (KPP/KKP/KK/KP) evaluation, auto-discovery
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
* **Evaluation** — Bonanza 6.0 fv.bin 形式の3駒関係評価（KPP/KKP/KK/KP テーブル）。fv.bin が無い・不正な場合は駒割のみのフォールバック。
* **Eval file auto-discovery** — 起動ディレクトリから `src/eval/fv.bin`、次に `eval/fv.bin` の順に探索。  
  `setoption name EvalFile value <path>` または `SHOGIAI_EVAL_FILE` 環境変数で明示指定すると自動検出より優先。  
  ファイルが無い・不正・非対応形式の場合は**安全に駒割のみフォールバック**へ自動切替します（探索は継続）。
* **EvalFamily** — `EvalFamily::BONANZA_V6_FV`（fv.bin から読み込み）/ `MATERIAL_FALLBACK`（内蔵デフォルト）/ `NNUE_UNSUPPORTED`（NNUE ファイル検出、非対応）の 3 種類。NNUE ファイルを検出しても正常読み込みとして誤報告しません。
* **Eval file format** — Bonanza 6.0 の生の fv.bin バイナリ（無改変）。期待サイズ・テーブル構成は上記「fv.bin のフォーマット」節を参照。
* **License / source** — リポジトリ同梱コードは MIT。本物の fv.bin を利用する場合は、Bonanza のライセンス条件との整合性を確認してください。
* **Search details** — 盤面 Zobrist hash を使う固定サイズの置換表、capture/promotion 中心の quiescence search、
  hash move → captures/promotions → killer moves → history heuristic の順序付けを使います。
* **Pruning** — 以前の「静的評価だけで `±INF` を返す」しきい値枝刈りは廃止し、
  葉では quiescence search で戦術的な取り返しや成りを確認します。
* **Alpha-beta score clamping** — 各ノードは着手側視点（negamax の「max side」）で評価するため、
  評価値が -1000 以下になった場合は必ず -1000 として return します（mate スコアは対象外）。
  親ノードは子の戻り値を符号反転して使う（`-search(...)`）ため、この下限クランプは
  相手側（min side）から見た +1000 の上限クランプとしても自動的に成立します。

---

## License

MIT
