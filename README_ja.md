# Nvecd

[![CI](https://github.com/libraz/nvecd/actions/workflows/ci.yml/badge.svg)](https://github.com/libraz/nvecd/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/libraz/nvecd/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/nvecd)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)](https://github.com/libraz/nvecd)

**イベントベース共起追跡とフュージョン検索を備えたインメモリベクトル検索エンジン**

## なぜ Nvecd？

レコメンドエンジンは複雑です — ML パイプライン、モデル学習、インフラ構築が必要です。多くのチームが本当に必要としているのは「X を買った人は Y も買っています」だけです。

**Nvecd** は、ユーザー行動追跡とベクトル類似検索を組み合わせたインメモリエンジンで、ML セットアップなしで即座にレコメンドを提供します。

## クイックスタート

### ビルド & 実行

```bash
# ビルド
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# サーバー起動
./build/bin/nvecd -c examples/config.yaml
# → Listening on 127.0.0.1:11017
```

### 基本的な使い方

```bash
# ユーザーの購入を追跡
nvecd-cli -p 11017 EVENT user_alice ADD product123 100
nvecd-cli -p 11017 EVENT user_alice ADD product456 80
nvecd-cli -p 11017 EVENT user_bob ADD product123 100
nvecd-cli -p 11017 EVENT user_bob ADD product789 95

# レコメンドを取得: 「product123 を買った人はこれも買っています...」
nvecd-cli -p 11017 SIM product123 10 using=events
# (2 results, showing 2)
# 1) product789 (score: 0.92)
# 2) product456 (score: 0.75)

# アイテムベクトルを登録（オプション、コンテンツベース類似検索用）
nvecd-cli -p 11017 VECSET product123 0.1 0.2 0.3 0.4
nvecd-cli -p 11017 VECSET product456 0.15 0.18 0.32 0.41

# ハイブリッド検索: 行動 + コンテンツ類似度
nvecd-cli -p 11017 SIM product123 10 using=fusion

# クエリベクトルで検索
nvecd-cli -p 11017 SIMV 10 0.5 0.3 0.2 0.1
```

### インタラクティブモード

```bash
nvecd-cli -p 11017
# nvecd> EVENT user1 ADD item1 100
# OK
# nvecd> SIM item1 10 using=fusion
# (3 results, showing 3)
# 1) item3 (score: 0.85)
# 2) item2 (score: 0.72)
# 3) item4 (score: 0.61)
# nvecd> help
```

### キャッシュパフォーマンスの監視

```bash
nvecd-cli -p 11017 CACHE STATS
# hit_rate: 0.8500
# current_memory_mb: 12.45
# time_saved_ms: 15420.50
```

### テスト

```bash
make test
```

## パフォーマンス

100K ベクトル (dim=128, cosine, Apple M4 Max NEON):

| クエリ | レイテンシ | スループット |
|---|---|---|
| SIM (コールド) | **1.12ms** | 900 QPS/スレッド |
| SIMV (コールド) | **0.98ms** | 1,000 QPS/スレッド |
| キャッシュヒット | **0.00025ms** | 400万 ops/秒 |

1Mベクトル（デフォルトサンプリング）: クエリあたり **約0.12ms**、8スレッドで **64K QPS**。詳細は[ベンチマーク](docs/ja/benchmarks.md)を参照。

## 主な機能

- **行動ベースレコメンド** - ユーザーアクションを追跡し、即座にレコメンドを取得
- **ベクトル類似検索** - エンベディングによる類似アイテム検索 (cosine, dot, L2)
- **ハイブリッドフュージョン検索** - ユーザー行動 + コンテンツ類似度を設定可能な重みで結合
- **リアルタイム更新** - ユーザーのインタラクションに応じてレコメンドが即座に適応
- **スマートキャッシュ** - LZ4圧縮付きLRUキャッシュと選択的無効化
- **SIMD 最適化** - AVX2/NEON によるベクトル演算の高速化
- **フォークベースのスナップショット** - コピーオンライトによるほぼゼロダウンタイムの永続化
- **デュアルプロトコル** - TCP (Redis互換) と HTTP/REST JSON API
- **Unix Domain Socket** - 低レイテンシのローカル接続
- **レート制限** - クライアント単位のトークンバケットレート制限
- **認証** - 書き込みコマンド用のオプションパスワード認証
- **CLI ツール** - タブ補完とインタラクティブモードを備えた `nvecd-cli`
- **クライアントライブラリ** - 言語バインディング用のC++/Cクライアントライブラリ

## Nvecd が適しているケース

**適している:**
- レコメンドシステム（「この商品を買った人はこんな商品も買っています」）
- 埋め込みによるコンテンツベース類似検索
- 行動 + コンテンツを組み合わせたハイブリッドレコメンド
- ML パイプラインなしのリアルタイムパーソナライゼーション
- シンプルなデプロイ要件

**推奨されない:**
- データセットが RAM に収まらない場合
- ノード間の分散検索が必要な場合
- 複雑な ML モデルサービング

## ドキュメント

- [**アーキテクチャ**](docs/ja/architecture.md) - システム設計とコンポーネント概要
- [**プロトコルリファレンス**](docs/ja/protocol.md) - 利用可能な全コマンド
- [**HTTP APIリファレンス**](docs/ja/http-api.md) - REST APIドキュメント
- [**設定ガイド**](docs/ja/configuration.md) - 設定オプション
- [**ユースケース集**](docs/ja/use-cases.md) - 実世界の使用例
- [**スナップショット管理**](docs/ja/snapshot.md) - 永続化とバックアップ
- [**ベンチマーク**](docs/ja/benchmarks.md) - 実測パフォーマンスデータと最適化結果
- [**パフォーマンスチューニング**](docs/ja/performance.md) - キャッシュチューニングと SIMD 最適化
- [**インストールガイド**](docs/ja/installation.md) - ビルドとインストール手順
- [**開発ガイド**](docs/ja/development.md) - 開発者向けガイド
- [**クライアントライブラリ**](docs/ja/libnvecdclient.md) - C/C++ クライアントライブラリ

### English Documentation

- [English docs](docs/en/)

## 要件

- C++17 以降（GCC 9+、Clang 10+）
- CMake 3.15+
- yaml-cpp（third_party/ に同梱）

## ソースからビルド

```bash
# Makefile を使用（推奨）
make

# または CMake を直接使用
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# テスト実行
make test
```

## ライセンス

[MIT License](LICENSE)

## 貢献

貢献を歓迎します！Issue や Pull Request をお気軽に送ってください。

## リンク

- **ドキュメント**: [docs/ja/](docs/ja/)
- **サンプル**: [examples/](examples/)
- **テスト**: [tests/](tests/)
