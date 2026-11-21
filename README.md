# ShogiAI

C++17 で実装された USI プロトコル対応の将棋エンジンです。Bonanza 6.0 形式の3駒関係評価関数（`fv.bin`: KPP + KKP）の差分計算と、[やねうら王 (YaneuraOu)](https://github.com/yaneurao/YaneuraOu) の探索アーキテクチャ（4-way クラスタ置換表、段階的 `MovePicker`、将棋固有の駒台スイング込み SEE、1手詰め判定、王手延長、1手/2手 Continuation History 等）を備えています。

## 主な機能・特徴

| 領域 | 実装内容 |
|---|---|
| **ルール・合法手生成** | 9×9盤面、全駒種・成り・打ち、二歩・行き所のない駒・打ち歩詰め判定、玉との直線ピン判定による `do_move` 不要の高速合法手生成、戦術手／静かな手／静かな王手の段階別生成 |
| **評価関数 (`fv.bin`)** | [Bonanza 6.0](https://ja.wikipedia.org/wiki/Bonanza) 生形式 `fv.bin`（`186,268,248` バイト: KPP `int16_t` + KKP `int16_t`, `FV_SCALE=32`）完全対応、差分駒割、親子局面間の KPP/KKP 差分更新（片玉移動時の片側再利用含む）、`EvalHash` キャッシュ |
| **探索部** | 反復深化 + 非対称 Aspiration Window、PVS、64B アライン 4-way クラスタ置換表（静的評価値保存・世代管理）、段階的 `MovePicker`、SEE（静的交換評価）、1手詰め判定（`mate1ply`）、王手延長・Singular Extension（Multi-Cut 付き）、IIR・RFP・Razoring・検証付き NMP・Futility・LMP・SEE 枝刈り・履歴連動 LMR、静止探索初手の安全な王手生成 |
| **学習履歴・順序付け** | 手番別 Main/Drop History、Capture History、Countermove、1-ply & 2-ply Continuation History |
| **USI・時間管理** | `usi`, `isready`, `setoption` (`EvalFile`, `USI_Hash`, `FV_SCALE`), `usinewgame`, `position`, `go` (`btime`/`wtime`/`binc`/`winc`/`byoyomi`/`movetime`/`depth`/`infinite`), `stop`, `quit`、最善手安定度に応じたソフト／ハード時間制御 |

## ビルド方法

### g++ 直接ビルド（Windows / Linux / macOS）

```bash
# Linux / macOS
g++ -std=c++17 -O3 -Wall -Wextra src/main.cpp src/board.cpp src/movegen.cpp src/eval.cpp src/search.cpp -Isrc -lpthread -o shogi_engine

# Windows (MinGW-w64)
g++ -std=c++17 -O3 -Wall -Wextra src\main.cpp src\board.cpp src\movegen.cpp src\eval.cpp src\search.cpp -Isrc -lpthread -o shogi_engine.exe
```

### CMake ビルド（デフォルトで Release `-O3` が適用されます）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 単体テスト（Perft・差分評価一致検証・SEE・詰み探索含む）
./build/shogi_tests
```

## 評価関数ファイル（Bonanza 6.0 `fv.bin`）の配置

リポジトリ同梱の `src/eval/fv.bin` は **1バイトのダミーファイル** です。お手元の本物の Bonanza 6.0 `fv.bin`（`186,268,248` バイト ≒ 177.6 MiB / 186.3 MB）を以下のいずれかで指定してください：

1. `src/eval/fv.bin`（または実行ファイル横の `eval/fv.bin`）に上書き配置（自動検出）
2. USI オプション `setoption name EvalFile value /path/to/fv.bin` で指定
3. 環境変数 `SHOGIAI_EVAL_FILE=/path/to/fv.bin` で指定

### `fv.bin` バイナリ構造と座標・添字仕様

本エンジンは [Bonanza](https://github.com/ianfab/bonanza) の `src/client/ini.c` (`load_fv`) および `src/client/shogi.h` / `evaluate.c`（解説: [やねうら王「30分でBonanzaの評価関数の要点を話します！」](https://yaneuraou.yaneu.com/2015/11/02/30%E5%88%86%E3%81%A7bonanza%E3%81%AE%E8%A9%95%E4%BE%A1%E9%96%A2%E6%95%B0%E3%81%AE%E8%A6%81%E7%82%B9%E3%82%92%E8%A9%B1%E3%81%97%E3%81%BE%E3%81%99%EF%BC%81/)）そのままの構造を読み込みます：

| # | テーブル | 型 | 要素数 | バイト数 |
|---|---|---|---|---|
| 1 | KPP（玉・駒・駒） | `int16_t` | `81 × 1,090,026` (`fe_end = 1476` 三角配列) | 176,584,212 |
| 2 | KKP（玉・玉・駒） | `int16_t` | `81 × 81 × 738` (`kkp_end = 738`) | 9,684,036 |
| **合計** | | | | **186,268,248 bytes** |

* **盤面座標 (`A9=0 .. I1=80`)**: Bonanza の升番号は段優先（`rank * 9 + file`, 9一=0, 1一=8, ..., 1九=80）であり、`src/types.hpp` の `Square` と完全に一致します。
* **持ち駒添字**: KPP では 1 枚目から `f_hand_* + 1 .. + count`（1-origin）、KKP では 0 枚を含む `kkp_hand_* + count`（0-origin）を参照します。
* **KPP/KKP 差分計算**: 非玉移動時は変化した 1〜2 駒分の KPP 行のみ差分更新し、非捕獲の玉移動時は相手玉側の `ΣKPP` をそのまま再利用します（参考: [やねうら王「KPP差分計算の高速化」](https://yaneuraou.yaneu.com/2015/09/24/kpp%E5%B7%AE%E5%88%86%E8%A8%88%E7%AE%97%E3%81%AE%E9%AB%98%E9%80%9F%E5%8C%96bonasse%E3%81%AE%E5%8A%9F%E7%BD%AA%E3%81%AB%E3%81%A4%E3%81%84%E3%81%A6/)）。

## USI オプション一覧

| オプション名 | 型 | デフォルト | 説明 |
|---|---|---|---|
| `USI_Hash` | `spin` | `64` (MiB) | 4-way クラスタ置換表のメモリサイズ（4〜2048 MiB） |
| `EvalFile` | `string` | `eval/fv.bin` | Bonanza 6.0 `fv.bin` のファイルパス |
| `FV_SCALE` | `spin` | `32` | 評価値スケーリング除数（Bonanza 6.0 標準値は `32`） |

## 参照プロジェクト

* [Bonanza (Wikipedia)](https://ja.wikipedia.org/wiki/Bonanza) / [ianfab/bonanza](https://github.com/ianfab/bonanza)
* [HiraokaTakuya/apery](https://github.com/HiraokaTakuya/apery)
* [yaneurao/YaneuraOu](https://github.com/yaneurao/YaneuraOu)

## License

MIT（本物の `fv.bin` を組み合わせる場合は Bonanza のライセンス条件に従ってください）
