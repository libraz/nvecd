# nvecd

同じアイテムについて 2 種類の手がかりを保持する、インメモリの検索エンジンです。1 つはアイテムに与えたベクトル、もう 1 つはどのアイテムが一緒に扱われたかという記録です。検索はどちらか一方でも、両方を 1 つの順位に混ぜても答えられます。

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/nvecd/ci.yml?branch=main&label=CI)](https://github.com/libraz/nvecd/actions)
[![Version](https://img.shields.io/github/v/tag/libraz/nvecd?label=version)](https://github.com/libraz/nvecd/tags)
[![codecov](https://codecov.io/gh/libraz/nvecd/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/nvecd)
[![License](https://img.shields.io/badge/license-MIT-blue)](https://github.com/libraz/nvecd/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/nvecd)

![イベントとベクトルを別々に保持し、検索時に統合する](docs/images/overview-ja.svg)

「これを買った人はこれも買っています」と「埋め込みが近いアイテム」は別々の問いで、両方に答えようとすると、ふつうは 2 つのシステムを動かしてアプリケーション側でその答えを突き合わせることになります。nvecd は 2 つの索引を 1 つのプロセスに持ち、1 回の検索の裏側で統合します。

## できること

```bash
./build/bin/nvecd -c examples/config.yaml   # 127.0.0.1:11017 で待ち受け
```

```bash
# 一緒に扱われたものを記録する。ベクトルは使わない。
nvecd-cli -p 11017 EVENT alice ADD widget 100
nvecd-cli -p 11017 EVENT alice ADD gasket 80
nvecd-cli -p 11017 EVENT bob   ADD widget 100
nvecd-cli -p 11017 EVENT bob   ADD flange 95

nvecd-cli -p 11017 SIM widget 10 using=events
# (2 results, showing 2)
# 1) flange (score: 9500)
# 2) gasket (score: 8000)

# 同じアイテムにベクトルを与えると、内容による類似も使えるようになる。
nvecd-cli -p 11017 VECSET widget 0.10 0.20 0.30 0.40
nvecd-cli -p 11017 VECSET gasket 0.15 0.18 0.32 0.41

nvecd-cli -p 11017 SIM widget 10 using=fusion
# (2 results, showing 2)
# 1) gasket (score: 0.5972)
# 2) flange (score: 0.4)

nvecd-cli -p 11017 SIMV 10 0.5 0.3 0.2 0.1          # クエリベクトルに近いもの
# (2 results, showing 2)
# 1) gasket (score: 0.6569)
# 2) widget (score: 0.6139)
```

共起スコアは、対になったイベントの重みの積を累積した値です。`flange` が 9500 なのは、あるコンテキストで `widget` の 100 と並んで 95 で記録されたからです。`[0, 1]` に収まる類似度ではなく、素の合計値です。統合検索は両側をそれぞれ正規化してから混ぜます。3 つ目のクエリが 1 つ目の答えを並べ替えているのはそのためで、ベクトルを得た `gasket` が、ベクトルを持たない `flange` を追い抜いています。

共起は最初のイベントから機能します。埋め込みモデルも学習の工程も要りません。ベクトルは任意で、あとから足せます。両方が揃っているときは `using=fusion` が統合し、その配分を固定値ではなくアイテムごとのデータ量に追随させられます。新しいアイテムは主にベクトルで、十分に観測されたアイテムは主に行動で順位づけられます。

イベントには時刻が伴い、減衰します。古い結びつきは放っておいても薄れます。結果はアイテムのメタデータで絞り込めますし、スコアの下限で切り落とせます。

## ユースケース

| ユースケース | 内容 | ガイド |
|---|---|---|
| ベクトルなしのレコメンド | 操作の流れだけから共起で推薦する。 | [レコメンド](docs/ja/use-cases/recommendations.md) |
| 商品レコメンド | 関与度で重みづけし、内容と行動を検索ごとに統合する。 | [EC サイト](docs/ja/use-cases/e-commerce.md) |
| パーソナライズドフィード | 半減期の短い関与イベントを継続的に流し込む。 | [リアルタイムフィード](docs/ja/use-cases/real-time-feed.md) |
| 意味検索 | 文書コーパスに対するクエリベクトル検索。 | [意味検索](docs/ja/use-cases/semantic-search.md) |

## インストール

C++17 コンパイラ（GCC 9 以降または Clang 10 以降）と CMake 3.15 以降が必要です。依存ライブラリは configure 時に取得するため、システム側に入れておくものはありません。

```sh
git clone https://github.com/libraz/nvecd.git
cd nvecd
make          # configure・ビルドを行い、bin/nvecd、bin/nvecd-cli、libnvecdclient を生成
make test
```

システムパッケージ、ビルドオプション、バイナリのインストール、サービスとしての起動は[インストール](docs/ja/installation.md)で扱っています。

## ドキュメント

エンジンが土台にしている考え方は[はじめに](docs/ja/introduction.md)に、最初の一通りの操作は[使いはじめる](docs/ja/getting-started.md)にあります。

エンジンが何をしているかは概念のページで説明しています。[イベントと共起](docs/ja/events-and-co-occurrence.md)、[ベクトル検索](docs/ja/vector-search.md)、[統合検索](docs/ja/fusion.md)、[キャッシュ](docs/ja/caching.md)、[永続化](docs/ja/persistence.md)です。リファレンスは [TCP プロトコル](docs/ja/protocol.md)、[HTTP API](docs/ja/http-api.md)、[クライアントライブラリ](docs/ja/client-library.md)、[設定](docs/ja/configuration.md)にあります。内部構造は[アーキテクチャ](docs/ja/architecture.md)、計測値は[ベンチマーク](docs/ja/benchmarks.md)にまとめています。

## できないこと

- **埋め込みは作りません。** nvecd はベクトルを保存して検索するだけです。アイテムのエンコードは手元のモデルで行い、その結果を `VECSET` で送ってください。
- **単一ノードで動きます。** データセットは 1 プロセスのメモリに載ります。シャーディングもレプリケーションも、ノードをまたぐ検索もありません。
- **メモリ常駐です。** スナップショットと先行書き込みログによって再起動から復旧できますが、ディスクから読みながら応答することはありません。メモリに載らない規模は対象外です。
- **モデルのサービングは行いません。** 順位づけは 2 つのスコア列の重み付き統合であって、推論ではありません。

このプロジェクトは 1.0 到達前です。ワイヤプロトコルと設定キーはバージョン間で変わることがあります。セキュリティ上の問題は、公開の issue ではなく GitHub の非公開アドバイザリからお知らせください。

## ライセンス

[MIT](LICENSE) で公開しています。
