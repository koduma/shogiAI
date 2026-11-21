# ShogiAI

C++17 で実装された USI プロトコル対応の将棋エンジンです。評価関数部には [やねうらおのFANBOX「つようら王11mini/microを無料公開」](https://yaneurao.fanbox.cc/posts/12761272)（[公式サイト解説記事](https://yaneuraou.yaneu.com/2026/10/14/tsuyou-11-mini-micro-released-under-mit-license/)）で MIT License 公開された **つようら王11mini**（`SFNN_ka2_4096_8_64_progress8`: `nn.bin` + `progress.bin`）アーキテクチャを採用し、[やねうら王 (YaneuraOu)](https://github.com/yaneurao/YaneuraOu) 準拠の探索部（4-way クラスタ置換表、段階的 `MovePicker`、駒台スイング込み SEE、1手詰め判定、王手延長、1手/2手 Continuation History 等）と組み合わせています。

## 主な機能・特徴

| 領域 | 実装内容 |
|---|---|
| **評価関数 (`nn.bin` + `progress.bin`)** | [YaneuraOu `evaluate_nnue.cpp`](https://github.com/yaneurao/YaneuraOu/blob/master/source/eval/nnue/evaluate_nnue.cpp) 互換の `SFNNWithoutPsqt` (`Features=K-A2(Friend)[1791->4096x2],Network=SFNN-4096{LayerStack=8}`, `COMPRESSED_LEB128` 展開対応, 最適 `FV_SCALE=24`)、`progress.bin`（`81×1548` 倍精度KPロジスティック回帰・Q16固定小数点）による8段階 LayerStack 選択、玉プレーン事前畳み込みによる全合法手1〜2要素差分更新、AVX-512 VNNI / AVX2 / SSE2 SIMD ベクトル化、`EvalHash` キャッシュ |
| **ルール・合法手生成** | 9×9盤面、全駒種・成り・打ち、二歩・行き所のない駒・打ち歩詰め判定、玉との直線ピン判定による `do_move` 不要の高速合法手生成、戦術手／静かな手／静かな王手の段階別生成 |
| **探索部** | 反復深化 + Aspiration Window、PVS、64B アライン 4-way クラスタ置換表（静的評価値キャッシュ・世代管理）、段階的 `MovePicker`、SEE（静的交換評価）、1手詰め判定（`mate1ply`）、王手延長・Singular Extension（Multi-Cut 付き）、IIR・RFP・Razoring・検証付き NMP・Futility・LMP・SEE 枝刈り・履歴連動 LMR、静止探索初手の安全な王手生成 |
| **学習履歴・順序付け** | 手番別 Main/Drop History、Capture History、Countermove、1-ply & 2-ply Continuation History |
| **USI・進行度連動時間管理** | `usi`, `isready`, `setoption` (`USI_Hash`, `EvalDir`, `EvalFile`, `ProgressFile`, `FV_SCALE`), `usinewgame`, `position`, `go` (`btime`/`wtime`/`binc`/`winc`/`byoyomi`/`movetime`/`depth`/`infinite`), `stop`, `quit`、`progress.bin` の学習済み進行度と最善手安定度に応じたソフト／ハード時間制御 |

## ビルド方法

### g++ 直接ビルド（Windows / Linux / macOS）

NNUE（`4096×2` アキュムレータ + `int8` 行列積）の性能を最大限に引き出すため、`-O3 -march=native` の付与を推奨します。

```bash
# Linux / macOS
g++ -std=c++17 -O3 -march=native -Wall -Wextra src/main.cpp src/board.cpp src/movegen.cpp src/eval.cpp src/search.cpp -Isrc -lpthread -o shogi_engine

# Windows (MinGW-w64)
g++ -std=c++17 -O3 -march=native -Wall -Wextra src\main.cpp src\board.cpp src\movegen.cpp src\eval.cpp src\search.cpp -Isrc -lpthread -o shogi_engine.exe
```

### CMake ビルド（デフォルトで Release `-O3` および `-march=native` 自動検出が有効）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 単体テスト（Perft・SFNN差分評価/SIMDビット完全一致検証・progressバケット検証・SEE・詰み探索）
./build/shogi_tests
```

## 評価関数ファイル（`nn.bin` / `progress.bin`）の配置

リポジトリ同梱の `src/eval/nn.bin` および `src/eval/progress.bin` は **0バイト（空）のダミーファイル** です（空ファイル時は自動的に安全な駒割評価 `MATERIAL_FALLBACK` として動作します）。[やねうらおのFANBOX](https://yaneurao.fanbox.cc/posts/12761272) からダウンロードした実ファイル（つようら王11mini `nn.bin`: `7,711,372` bytes、つようら王11micro `nn.bin`: `2048×2` 版も自動判別対応、`progress.bin`: `1,003,104` bytes）を以下のいずれかの方法で配置してください：

1. `src/eval/nn.bin` と `src/eval/progress.bin`（または実行ファイル横の `eval/nn.bin` と `eval/progress.bin`）に上書き配置（自動検出）
2. USI オプション `setoption name EvalDir value /path/to/eval_dir`（または `EvalFile` / `ProgressFile` 個別指定）
3. 環境変数 `SHOGIAI_EVAL_DIR=/path/to/eval_dir`（または `SHOGIAI_EVAL_FILE` / `SHOGIAI_PROGRESS_FILE`）

### `nn.bin` / `progress.bin` アーキテクチャ仕様

本エンジンの評価関数ローダ・推論器は、[YaneuraOu `evaluate_nnue.cpp`](https://github.com/yaneurao/YaneuraOu/blob/master/source/eval/nnue/evaluate_nnue.cpp)、[やねうら王の更新履歴2026](https://github.com/yaneurao/YaneuraOu/wiki/%E3%82%84%E3%81%AD%E3%81%86%E3%82%89%E7%8E%8B%E3%81%AE%E6%9B%B4%E6%96%B0%E5%B1%A5%E6%AD%B42026)、[yaneurao/BulletOu](https://github.com/yaneurao/BulletOu)、および [SH11235/tatara `dense-qat.md`](https://github.com/SH11235/tatara/blob/main/docs/dense-qat.md) の仕様に完全準拠しています：

| ファイル / 層 | 型・圧縮形式 | 形状・構造 | 内容 |
|---|---|---|---|
| `nn.bin` Header | `uint32_t` × 3 + ASCII | `version=0x7af32f16` | `ModelType=SFNNWithoutPsqt;Features=K-A2(Friend)[1791->4096x2],Network=SFNN-4096{LayerStack=8}` |
| FeatureTransformer | `COMPRESSED_LEB128` (`int16_t`) | Bias `[4096]`, Weight `[1791][4096]` | YaneuraOu 筋優先座標（`1一=0 .. 9九=80`）の `BonaPiece` (`0..1547`) + `A2` 両玉共通面 (`1548..1628`) + 自玉面 (`1629..1709`) + 敵玉面 (`1710..1790`)。Pairwise 乗算 `(clamp(a,0,127)*clamp(b,0,127))>>7` により `4096` 次元 `uint8_t` を出力 |
| LayerStack `0..7` (`fc0`→`fc1`→`fc2`) | `int32_t` bias + `int8_t` weight | `4096 → 8 (Dual: Sqr+Lin=16) → 64 → 1` | `SqrClippedReLU` (`(z*z)>>19`) と `ClippedReLU` (`z>>6`) の2系統結合（`H1=8` のため skip なし）、最終出力 `clamp(out / FV_SCALE, -30000, 30000)` |
| `progress.bin` | 生 IEEE-754 `double` | `81 × 1,548` (`1,003,104` bytes) | Q16 整数化（`×65536`）した KP 進行度重み。対数オッズ閾値 `log((i/8)/(1-i/8))` により 8 個の LayerStack バケットを選択 |

## USI オプション一覧

| オプション名 | 型 | デフォルト | 説明 |
|---|---|---|---|
| `USI_Hash` | `spin` | `64` (MiB) | 4-way クラスタ置換表のメモリサイズ（4〜2048 MiB） |
| `EvalDir` | `string` | `eval` | `nn.bin` と `progress.bin` を格納したフォルダパス |
| `EvalFile` | `string` | `eval/nn.bin` | `nn.bin` の個別ファイルパス |
| `ProgressFile` | `string` | `eval/progress.bin` | `progress.bin` の個別ファイルパス |
| `FV_SCALE` | `spin` | `24` | 評価値スケーリング除数（[つようら王11mini 公式推奨値 `24`](https://yaneurao.fanbox.cc/posts/12761272)） |

## 参照プロジェクト・文献

* [やねうらおのFANBOX「つようら王11mini/microを無料公開」](https://yaneurao.fanbox.cc/posts/12761272) / [やねうら王公式サイト記事](https://yaneuraou.yaneu.com/2026/10/14/tsuyou-11-mini-micro-released-under-mit-license/)
* [yaneurao/YaneuraOu](https://github.com/yaneurao/YaneuraOu) / [やねうら王の更新履歴2026](https://github.com/yaneurao/YaneuraOu/wiki/%E3%82%84%E3%81%AD%E3%81%86%E3%82%89%E7%8E%8B%E3%81%AE%E6%9B%B4%E6%96%B0%E5%B1%A5%E6%AD%B42026)
* [yaneurao/BulletOu](https://github.com/yaneurao/BulletOu) / [SH11235/tatara](https://github.com/SH11235/tatara)
* [Bonanza (Wikipedia)](https://ja.wikipedia.org/wiki/Bonanza) / [HiraokaTakuya/apery](https://github.com/HiraokaTakuya/apery)

## License

MIT License（[つようら王11mini/micro の `nn.bin`・`progress.bin` も MIT License](https://yaneurao.fanbox.cc/posts/12761272) で公開されています）
