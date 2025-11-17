# Nvecd

**イベントベース共起追跡機能を備えたインメモリベクトル検索エンジン**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](https://opensource.org/licenses/MIT)

## Nvecd とは？

Nvecd は、ユーザー行動から学習するレコメンドエンジンです。ユーザーの行動を追跡するだけで、パターンに基づいた即座のレコメンドを取得できます。

### シンプルな例：「この商品を買った人はこんな商品も買っています」

```javascript
const net = require('net');

// nvecd に接続
const client = net.createConnection({ port: 11017 }, () => {
  // ユーザーの購入を追跡
  client.write('EVENT user_alice item1 100\n');
  client.write('EVENT user_alice item2 100\n');
  client.write('EVENT user_bob item1 100\n');
  client.write('EVENT user_bob item3 100\n');

  // レコメンドを取得: 「item1 を買った人はこれも買っています...」
  client.write('SIM item1 10 fusion\n');
});

client.on('data', (data) => {
  console.log(data.toString());
  // → item3 0.85
  // → item2 0.72
});
```

**これだけです！** ML モデルも複雑なセットアップも不要。ユーザーの行動を追跡してレコメンドを取得するだけです。

## ⚠️ アルファ開発中

このプロジェクトは現在**アルファ開発中**です。
本番環境での使用は推奨されません。

## 主な機能

- **行動ベースレコメンド** - ユーザーアクションを追跡し、即座にレコメンドを取得
- **ベクトル類似検索** - 埋め込みを使用した類似アイテム検索（オプション）
- **ハイブリッドフュージョン** - ユーザー行動 + コンテンツ類似度を組み合わせ
- **リアルタイム更新** - ユーザーのインタラクションに応じてレコメンドが適応
- **永続ストレージ** - スナップショット対応（DUMP コマンド）
- **シンプルなプロトコル** - TCP 経由のテキストベースコマンド

## クイックスタート

### 1. ビルド & 実行

```bash
# ビルド
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# サーバー起動
./build/bin/nvecd -c examples/config.yaml
# → Listening on 127.0.0.1:11017
```

### 2. 試してみる

```bash
# 接続
nc localhost 11017

# ユーザーインタラクションを追跡
EVENT user1 product123 100
EVENT user1 product456 80
EVENT user2 product123 100
EVENT user2 product789 95

# レコメンドを取得
SIM product123 10 fusion
# → product789 0.92
# → product456 0.75
```

### 3. テスト

```bash
make test
# → All 173 tests passing ✅
```

## ユースケース

- 🛒 **E コマース** - 商品レコメンド
- 📰 **ニュース/コンテンツ** - 記事レコメンド
- 🎵 **音楽/動画** - パーソナライズドプレイリスト
- 📱 **ソーシャルメディア** - コンテンツフィード（TikTok スタイル）
- 🔍 **検索** - 埋め込みを使ったセマンティック検索

詳細な例は [**ユースケースガイド**](docs/ja/use-cases.md) を参照してください。

## ドキュメント

### はじめに
- [**インストールガイド**](docs/ja/installation.md) - ビルドとインストール手順
- [**クイックスタートガイド**](docs/ja/development.md) - 5分で始める
- [**ユースケース集**](docs/ja/use-cases.md) - 実世界の例とコード

### リファレンス
- [**プロトコルリファレンス**](docs/ja/protocol.md) - 利用可能な全コマンド
- [**設定ガイド**](docs/ja/configuration.md) - 設定オプション
- [**スナップショット管理**](docs/ja/snapshot.md) - 永続化とバックアップ

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

MIT License

## 貢献

貢献を歓迎します！Issue や Pull Request をお気軽に送ってください。

## リンク

- **ドキュメント**: [docs/ja/](docs/ja/)
- **サンプル**: [examples/](examples/)
- **テスト**: [tests/](tests/)
