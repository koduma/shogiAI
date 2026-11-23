# ShogiAI

C++17 で実装された USI プロトコル対応の将棋エンジンおよび勝率定跡システムです。評価関数部には [やねうらおのFANBOX「つようら王11mini/microを無料公開」](https://yaneurao.fanbox.cc/posts/12761272)（[公式サイト解説記事](https://yaneuraou.yaneu.com/2026/10/14/tsuyou-11-mini-micro-released-under-mit-license/)）で MIT License 公開された **つようら王11mini**（`SFNN_ka2_4096_8_64_progress8`: `nn.bin` + `progress.bin`）アーキテクチャを採用し、[やねうら王 (YaneuraOu)](https://github.com/yaneurao/YaneuraOu) 準拠の Lazy SMP 並列探索部および **水匠5・やねうら王へそのままexe差し替え可能な勝率定跡生成・運用システム**を備えています。

## 主な機能・特徴

| 領域 | 実装内容 |
|---|---|
| **盤面・合法手生成部（完全ビットボード）** | [やねうら王](https://github.com/yaneurao/YaneuraOu) 流の 128-bit `Bitboard`（SSE2/SSE4.1/BMI1/BMI2 PEXT対応）、L1/L2常駐型飛角香スライダー利きテーブル（内側63マス集約 `mid63`）、王手駒 `checkers_` 差分管理、遅延ピン・空き王手ブロッカー一括計算（`slider_blockers`）、`do_move` 不要の合法手・王手回避生成、O(1)二歩マスク、4サイクル王手判定、X-ray対応ビットボード `see_ge`、ターゲットマスク型高速1手詰め `find_mate_1ply`（`perft(5)` 約2.03億nps） |
| **評価関数 (`nn.bin` + `progress.bin`)** | [YaneuraOu `evaluate_nnue.cpp`](https://github.com/yaneurao/YaneuraOu/blob/master/source/eval/nnue/evaluate_nnue.cpp) 互換の `SFNNWithoutPsqt` (`Features=K-A2(Friend)[1791->4096x2],Network=SFNN-4096{LayerStack=8}`, `COMPRESSED_LEB128` 展開対応, 最適 `FV_SCALE=24`)、`progress.bin`（`81×1548` 倍精度KPロジスティック回帰・Q16固定小数点）による8段階 LayerStack 選択、玉プレーン事前畳み込み＋Plyインデックス型アキュムレータスタック（ルートシード付き）による全合法手1〜2要素差分更新、`fc0` 32Bインターリーブ＋4倍アンロール AVX2 / SSE2 SIMD ベクトル化、スレッド独立 `EvalHash` キャッシュ |
| **並列探索・`nodelimit`** | SplitMix64 XOR ロックフリー 4-way クラスタ置換表、Lazy SMP マルチスレッド並列探索（`Threads`）、全スレッド合算ノード数を厳密制限するアトミック `nodelimit`、段階的 `MovePicker`、SEE、1手詰め判定、Singular Extension、IIR・RFP・NMP・LMP・LMR |
| **勝率定跡生成部 (`winrate_book_gen`)** | 平手初形から最大200手までビームサーチを実行。先手視点評価値 `+600` 以上は `(hash, 1.0)`・`-600` 以下は `(hash, 0.0)` を記録して枝刈りし、`-600〜+600` は1局10秒以内の自己対局100局で先手勝率を記録して分岐、次世代ノード選択では `winrate_book.db` 未登録ノードを最優先し、次に先手視点評価値（先手番＝降順ソート、後手番＝昇順ソート）でビーム幅以下に絞って1手ごとに `winrate_book.db` をアトミック更新（複数回実行・`--runs <R>` 対応） |
| **勝率定跡運用部 (`winrate_book_player`)** | ShogiGUI に1つ登録するだけで動作する USI プロキシエンジン。定跡内は `winrate_book.db` を DAG minimax 探索して勝率表示＋即時着手し、定跡外では `shogi_engine.exe` や **水匠5（やねうら王）** へ透過的に指し継ぎ |
| **水匠5 / やねうら王 差し替え自動適応** | 起動時の `usi` 応答から子エンジンの対応オプションを自動判別し、水匠5の内蔵定跡（`USI_OwnBook=false`, `BookMoves=0`, `BookFile=no_book`）自動抑止、ローカルパイプ生成時の `NetworkDelay=0` / `MinimumThinkingTime=1` 自動最適化、`nodelimit` ⇔ `NodesLimit` 自動変換、入玉宣言勝ち（`bestmove win`）および同フォルダ `.exe` 自動検出に対応 |

## Windows (MinGW-w64) ビルド方法（別PC持ち運び対応 `-static`）

```bat
REM ① 通常の将棋AI本体 (shogi_engine.exe)
g++ -std=c++17 -O3 -mavx2 -mfma -mbmi2 -mpopcnt -Wall -Wextra src\main.cpp src\board.cpp src\movegen.cpp src\eval.cpp src\search.cpp -Isrc -static -lpthread -o shogi_engine.exe

REM ② 勝率定跡生成部 (winrate_book_gen.exe)
g++ -std=c++17 -O3 -mavx2 -mfma -mbmi2 -mpopcnt -Wall -Wextra src\book_gen_main.cpp src\board.cpp src\movegen.cpp src\usi_proc.cpp src\winrate_book.cpp -Isrc -static -lpthread -o winrate_book_gen.exe

REM ③ 勝率定跡運用部・ShogiGUI登録用プロキシエンジン (winrate_book_player.exe)
g++ -std=c++17 -O3 -mavx2 -mfma -mbmi2 -mpopcnt -Wall -Wextra src\book_player_main.cpp src\board.cpp src\movegen.cpp src\usi_proc.cpp src\winrate_book.cpp -Isrc -static -lpthread -o winrate_book_player.exe
```

### CMake ビルド（Linux / macOS / Windows）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/shogi_tests
```

## 評価関数ファイル（`nn.bin` / `progress.bin`）の配置

リポジトリ同梱の `src/eval/nn.bin` および `src/eval/progress.bin` は **0バイト（空）のダミーファイル** です。
* **付属の `shogi_engine.exe` を使う場合**: [やねうらおのFANBOX](https://yaneurao.fanbox.cc/posts/12761272) のつようら王11mini `nn.bin` と `progress.bin` を実行ファイル横の `eval/` フォルダに配置してください。
* **水匠5のエンジン `.exe` に差し替える場合**: 水匠5の `.exe` と水匠5の `eval/nn.bin` を使います（追加設定不要で自動認識されます）。

## 勝率定跡の生成と ShogiGUI での運用（`shogi_engine.exe` / 水匠5 両対応）

### 1. 水匠5への差し替え方法（以下のいずれでも追加設定なしでそのまま動作）

1. **リネーム差し替え**: `shogi_engine.exe` を水匠5の `.exe`（リネームして `shogi_engine.exe`）＋水匠5の `eval` フォルダに置き換える
2. **同フォルダ配置（リネームすら不要）**: `shogi_engine.exe` を消して、同じフォルダに水匠5の `.exe`（例: `YaneuraOu-by-gcc.exe`）と水匠5の `eval` フォルダを置く（自動検出されます）
3. **別フォルダ直接指定**: 水匠5フォルダは一切触らず、`winrate_book_gen.exe --engine "C:\...\YaneuraOu-by-gcc.exe"` や ShogiGUI の `ChildEnginePath` に水匠5のフルパスを指定する（水匠5側の `book` フォルダ退避や `NetworkDelay` 変更もすべて自動で行われます）

### 2. 勝率定跡DB（`winrate_book.db`）の生成

```bat
REM デフォルト（shogi_engine.exe または同フォルダの水匠5を自動検出）
winrate_book_gen.exe --engine shogi_engine.exe --book winrate_book.db --beam-width 8 --max-ply 200 --games 100 --game-limit-ms 40000 --rollout-nodes 320000 --branch-nodes 12800000 --eval-nodes 12800000 --workers 1 --threads 4 --hash-mb 1024

REM 別フォルダの水匠5を直接指定する場合
winrate_book_gen.exe --engine "C:\engines\Suisho5\YaneuraOu-by-gcc.exe" --book winrate_book.db --beam-width 8 --max-ply 200 --games 100 --game-limit-ms 10000
```

* 平手ルートから最大200手までビームサーチを行い、1手進めるごとに `winrate_book.db`（`0x<16桁Zobristハッシュ>,<先手勝率0.0〜1.0>`）をアトミック更新します（中断後の自動再開対応）。

### 3. ShogiGUI でのエンジン設定方法

* ShogiGUI の **「ツール」→「エンジン設定」→「追加」では `winrate_book_player.exe` の1つだけを登録**します。
* 設定画面の主な項目：
  * `BookFile`: `winrate_book.db` のパス
  * `BookMinimaxDepth`: 定跡minimax探索の最大深さ（既定 `24`）
  * `ChildEnginePath`: 定跡外で指し継ぐエンジン `.exe`（既定 `shogi_engine.exe`、水匠5のフルパスや同フォルダ自動検出にも対応）
  * `ChildOwnBook`: 定跡外に出た後で子エンジン自身の内蔵定跡を使うか（既定 `false`）
  * `Threads` / `USI_Hash` / `nodelimit`: 子エンジンへ自動転送されます（水匠5使用時は `nodelimit` が自動的に `NodesLimit` へマッピングされ、`FV_SCALE=0` により水匠5固有の評価値スケールがそのまま維持されます）。

## License

MIT License
