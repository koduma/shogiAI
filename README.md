# ShogiAI

C++17 で実装した最小構成の USI 対応将棋エンジンです。

---

## 機能

| 項目 | 内容 |
|---|---|
| ルール | 9×9 盤面、成り、持ち駒、打ち歩詰め禁止、二歩、千日手判定 |
| 探索 | iterative deepening alpha-beta、quiescence search、置換表、各種 move ordering |
| 評価 | **実テーブル参照の KPP 3駒関係評価**（互換 `ShogiAI-KPP-v1` モデルを読めた場合）+ 材料点 |
| フォールバック | 互換 KPP モデルが無い場合は **material-only fallback** |
| NNUE | **未実装**。`nn.bin` や NNUE マニフェストを検出しても **NNUE_UNSUPPORTED** として材料点へフォールバック |

---

## ビルド

### 要件

- CMake 3.14 以上
- C++17 対応コンパイラ

### 手順

```bash
git clone https://github.com/koduma/shogiAI.git
cd shogiAI
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/shogi_tests
```

生成物:

- `build/shogi_engine` : USI エンジン
- `build/shogi_tests` : テスト実行ファイル

`src/eval/` 配下は CMake により `build/eval/` へコピーされます。
そのため、`src/eval/kpp.bin` を配置してビルドした場合、`./build/shogi_engine` からは `build/eval/kpp.bin` を自動検出できます。

---

## 実行

```text
$ ./build/shogi_engine
usi
isready
position startpos
go movetime 2000
bestmove 7g7f
```

USI `setoption name EvalFile value <path>` で評価ファイルを明示指定できます。

---

## 評価関数

### 概要

本リポジトリの評価関数は次の 3 状態を明確に区別します。

- `EvalFamily::KPP_TABLE`
  - `ShogiAI-KPP-v1` 形式の互換 KPP モデルをロードできた状態
  - 評価値 = 材料点 + KPP テーブル和
- `EvalFamily::MATERIAL_FALLBACK`
  - 互換モデルが無い / 不正 / 読み込み失敗
  - 評価値 = 材料点のみ
- `EvalFamily::NNUE_UNSUPPORTED`
  - `nn.bin` など NNUE ラベル付き候補を検出したが、このコードでは未対応
  - 評価値 = 材料点のみ

**以前の Manhattan 距離ベースの疑似 KPP ヒューリスティックは、現在の評価経路では使用しません。**

### 自動検出と優先順位

優先順位は次の通りです。

```text
1. setoption name EvalFile value <path>
2. 環境変数 SHOGIAI_EVAL_FILE=<path>
3. 自動検出
   - src/eval/kpp.bin
   - eval/kpp.bin
   - src/eval/nn.bin
   - eval/nn.bin
4. material-only fallback
```

- `src/eval/kpp.bin` / `eval/kpp.bin` は **ShogiAI-KPP-v1 専用**です。
- `src/eval/nn.bin` / `eval/nn.bin` は **NNUE 未対応の明示的な検出対象**です。見つけても NNUE は有効化されません。
- 旧 `src/eval/kpp_weights.txt` は自動検出対象ではありません。移行メモ用の旧ファイルであり、現行評価器を有効化しません。

### ステータスメッセージ

`go` の前に `info string <status>` を出力します。例:

- `kpp table: loaded from eval/kpp.bin [auto-discovered]`
- `kpp table: loaded from /path/to/model.bin [explicit]`
- `material-only fallback (no compatible ShogiAI-KPP-v1 file found; ...)`
- `material-only fallback (invalid ShogiAI-KPP-v1 file: ... )`
- `nnue unsupported (source: eval/nn.bin [auto-discovered]); using material-only fallback`

---

## `ShogiAI-KPP-v1` 形式

### 互換性について重要

この実装は **YaneuraOu / 他エンジンの KPP / KPPT / NNUE バイナリとは互換性がありません。**
特に **YaneuraOu の `nn.bin` はこのコードでは使えません。**

本リポジトリが受け付けるのは、ここで定義する **`ShogiAI-KPP-v1`** のみです。

### 特徴量エンコーディング

KPP の feature は「非玉の盤上駒」および「持ち駒」です。玉は king-square 軸で別管理します。

#### 1. 盤上駒 feature

全 feature 数のうち最初の 2106 個は盤上駒です。

```text
feature = color * (13 * 81) + piece_type_index * 81 + square
```

- `color` : `BLACK=0`, `WHITE=1`
- `square` : `rank * 9 + file`（`src/types.hpp` と同じ）
- `piece_type_index` の順序:
  1. `PAWN`
  2. `LANCE`
  3. `KNIGHT`
  4. `SILVER`
  5. `GOLD`
  6. `BISHOP`
  7. `ROOK`
  8. `PROM_PAWN`
  9. `PROM_LANCE`
  10. `PROM_KNIGHT`
  11. `PROM_SILVER`
  12. `PROM_BISHOP`
  13. `PROM_ROOK`

#### 2. 持ち駒 feature

残り 76 個は持ち駒です。重複枚数は「通し番号付きスロット」で表現します。

```text
base = 2106 + color * 38
feature = base + piece_type_offset + ordinal
```

各色 38 スロットの内訳:

- `PAWN` : 18
- `LANCE` : 4
- `KNIGHT` : 4
- `SILVER` : 4
- `GOLD` : 4
- `BISHOP` : 2
- `ROOK` : 2

したがって総 feature 数は:

```text
2106 + 76 = 2182
```

### KPP テーブルの意味

各 side について、

- その side の玉位置 `king_sq`
- 局面から抽出した feature list（両陣営の非玉盤上駒 + 両陣営の持ち駒）

を使い、**`i < j` の unordered pair のみ**を足し合わせます。
同じ feature 対を二重加算しません。

```text
score(side-to-move)
  = material(side-to-move)
  + KPP(king_of_side_to_move, features)
  - KPP(king_of_opponent, features)
```

### バイナリレイアウト

すべて little-endian です。

#### Header（64 bytes）

| Offset | Size | 内容 |
|---|---:|---|
| 0 | 16 | magic = `ShogiAI-KPP-v1` + NUL padding |
| 16 | 4 | version = `1` |
| 20 | 4 | endianness marker = `0x01020304` |
| 24 | 4 | header size = `64` |
| 28 | 4 | king square count = `81` |
| 32 | 4 | feature count = `2182` |
| 36 | 4 | material value count = `15` (`PT_NB`) |
| 40 | 4 | entry count |
| 44 | 4 | storage kind = `1` (sparse unordered KPP entries) |
| 48 | 4 | value type = `1` (`int16`) |
| 52 | 8 | payload bytes |
| 60 | 4 | FNV-1a 32bit checksum of payload |

#### Payload

1. `piece_value[15]` : 15 × `uint32_t`
2. relation entry array : `entry_count` 個、各 8 bytes

relation entry 1 個のレイアウト:

| Field | Size | 内容 |
|---|---:|---|
| `king_sq` | 2 | 0..80 |
| `feature_a` | 2 | 0..2181 |
| `feature_b` | 2 | 0..2181、かつ `feature_a < feature_b` |
| `value` | 2 | `int16_t` |

制約:

- entry は `(king_sq, feature_a, feature_b)` 昇順で **strictly sorted / unique**
- `payload_bytes` は `15*4 + entry_count*8` と完全一致しなければ拒否
- ヘッダ値が `81` / `2182` / `64` / `storage kind 1` / `value type 1` と一致しなければ拒否
- 大きすぎる payload は確保前に拒否

### モデル容量の目安

現行実装は sparse relation 形式です。ファイルサイズは non-zero entry 数に依存します。

参考として、同じ feature 定義を **密行列**で全格納すると:

- feature pair 数: `2182 * 2181 / 2 = 2,379,471`
- king-square を掛けた relation 数: `192,737,151`
- `int16` 2 byte 格納時: 約 `367.6 MiB`

したがって、実用モデルを作る場合は sparse 化・圧縮・学習方針を自前で設計してください。

### 生成・学習について

本リポジトリは **学習器や既製の強い評価表を同梱しません**。
互換モデルを使いたい場合は、上記 feature 定義とファイル形式に合わせて自前で生成してください。

テストでは小さな決定論的 fixture をその場で生成し、
「本当に `(king, feature_a, feature_b)` テーブル参照で値が変わる」ことだけを検証しています。
通常ビルドで「強いモデルが同梱されている」とは主張しません。

---

## NNUE と外部エンジンについて

- **NNUE は未実装**です。
- `nn.bin` を置いても、このコードは NNUE 推論を行いません。
- `nn.bin` や NNUE マニフェストを検出した場合は `NNUE_UNSUPPORTED` と表示し、材料点へフォールバックします。
- YaneuraOu など外部エンジンの NNUE/KPP/KPPT 形式を「そのままロードできる」とは主張しません。

参考資料（参照のみ。直接互換ではありません）:

- YaneuraOu: <https://github.com/yaneurao/YaneuraOu> （GPLv3）
- ynasu87/nnue: <https://github.com/ynasu87/nnue> （GPLv3）
- YaneuraOu 解説: <https://github.com/yaneurao/YaneuraOu/blob/master/docs/解説.md> （GPLv3）

外部実装・重み・学習器を流用する場合は、各ライセンス条件を確認してください。

---

## ディレクトリ構成

```text
shogiAI/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── eval/
│   │   ├── kpp.bin          # ユーザー配置の互換 KPP モデル（任意）
│   │   └── kpp_weights.txt  # 旧テキスト形式メモ（自動検出されない）
│   ├── types.hpp
│   ├── board.hpp
│   ├── board.cpp
│   ├── movegen.hpp
│   ├── movegen.cpp
│   ├── eval.hpp
│   ├── eval.cpp
│   ├── search.hpp
│   ├── search.cpp
│   └── main.cpp
└── tests/
    └── test_main.cpp
```

---

## ライセンス

MIT
