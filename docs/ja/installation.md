# インストールガイド

このガイドでは、nvecd をソースからビルドしてインストールする詳細な手順を説明します。

## 前提条件

### システム要件

- **オペレーティングシステム**: Linux (Ubuntu 20.04+、Debian 11+) または macOS (10.15+)
- **コンパイラ**: C++17 対応コンパイラ
  - GCC 9+ (Linux)
  - Clang 10+ (macOS/Linux)
- **CMake**: バージョン 3.15 以降
- **メモリ**: ビルドに最低 1GB RAM、実行に 512MB 以上

### 必要な依存関係

すべての依存関係は `third_party/` ディレクトリにバンドルされており、ビルド時に自動的に取得されます:

- **yaml-cpp**: YAML 設定パーサー (バンドル済み)
- **GoogleTest**: テストフレームワーク (バンドル済み、テスト用のみ)
- **spdlog**: 高速ログライブラリ (バンドル済み)

### システム依存関係

#### Ubuntu/Debian

```bash
# パッケージリストの更新
sudo apt-get update

# ビルドツールのインストール
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  pkg-config \
  libz-dev
```

#### macOS

```bash
# Homebrew のインストール (未インストールの場合)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 依存関係のインストール
brew install cmake
```

---

## ソースからのビルド

### クイックスタート (Makefile を使用)

nvecd をビルドする最も簡単な方法は、提供されている Makefile を使用することです:

```bash
# リポジトリのクローン
git clone https://github.com/yourusername/nvecd.git
cd nvecd

# ビルド (CMake を自動設定し、並列ビルドを実行)
make

# テストの実行 (218 テスト)
make test

# 利用可能なコマンドの表示
make help
```

**ビルド出力**: バイナリは `build/bin/nvecd` に生成されます

### 手動ビルド (CMake を直接使用)

CMake を直接使用する場合:

```bash
# ビルド設定 (Release モード)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 並列ビルド
cmake --build build --parallel

# テストの実行
cd build
ctest --output-on-failure
```

**ビルド出力**: バイナリは `build/bin/nvecd` に生成されます

---

## ビルドオプション

### ビルドタイプ

```bash
# Debug ビルド (シンボル付き、最適化なし)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Release ビルド (最適化、デバッグシンボルなし)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# RelWithDebInfo (最適化とデバッグシンボル付き)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

### オプション機能

#### テストの無効化

テストのビルドをスキップする場合:

```bash
cmake -B build -DBUILD_TESTS=OFF
```

または Makefile で:

```bash
make CMAKE_OPTIONS="-DBUILD_TESTS=OFF" configure
make
```

#### AddressSanitizer の有効化 (メモリエラー検出)

```bash
cmake -B build -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

または Makefile で:

```bash
make CMAKE_OPTIONS="-DENABLE_ASAN=ON" configure
make
```

#### ThreadSanitizer の有効化 (競合状態検出)

```bash
cmake -B build -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

または Makefile で:

```bash
make CMAKE_OPTIONS="-DENABLE_TSAN=ON" configure
make
```

**注意**: ASAN と TSAN を同時に有効にしないでください。

#### コードカバレッジの有効化

```bash
cmake -B build -DENABLE_COVERAGE=ON
cmake --build build --parallel

# カバレッジ付きでテストを実行
make coverage

# カバレッジレポートの表示
open build/coverage/html/index.html
```

---

## バイナリのインストール

### システム全体へのインストール (推奨)

`/usr/local` にインストール (デフォルトの場所):

```bash
# まずビルド
make

# インストール (sudo が必要)
sudo make install
```

これにより以下がインストールされます:

- **バイナリ**: `/usr/local/bin/nvecd`
- **設定サンプル**: `/usr/local/etc/nvecd/config.yaml.example`
- **ドキュメント**: `/usr/local/share/doc/nvecd/`

### カスタムインストール場所

カスタムディレクトリにインストール (例: `/opt/nvecd`):

```bash
# カスタムプレフィックスで設定
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/nvecd

# ビルドとインストール
cmake --build build --parallel
sudo cmake --install build
```

または Makefile で:

```bash
make PREFIX=/opt/nvecd install
```

### アンインストール

インストールされたファイルを削除するには:

```bash
sudo make uninstall
```

または CMake で:

```bash
sudo cmake --build build --target uninstall
```

---

## テストの実行

nvecd には包括的なテストカバレッジが含まれています (218 テスト)。

### Makefile を使用

```bash
make test
```

### CTest を直接使用

```bash
cd build
ctest --output-on-failure
```

### 特定のテストの実行

```bash
# イベントストアのテストのみ実行
cd build
./bin/event_store_test

# 詳細出力で実行
./bin/event_store_test --gtest_verbose

# 特定のテストケースを実行
./bin/event_store_test --gtest_filter="EventStoreTest.BasicIngest"
```

### テストカバレッジ

- **イベントストア**: 35 テスト
- **ベクトルストア**: 28 テスト
- **共起インデックス**: 22 テスト
- **類似度検索**: 31 テスト
- **設定パーサー**: 18 テスト
- **サーバー & ハンドラ**: 45 テスト
- **キャッシュシステム**: 24 テスト
- **クライアントライブラリ**: 15 テスト

**合計**: 218 テスト、100% 成功 ✅

---

## インストールの確認

インストール後、バイナリがアクセス可能であることを確認します:

```bash
# サーバーバイナリの確認
nvecd --help

# 期待される出力:
# Usage: nvecd [OPTIONS]
#
# Options:
#   -c, --config <file>    Configuration file path
#   -h, --help             Show this help message
#   -v, --version          Show version information
```

バージョンの確認:

```bash
nvecd --version

# 期待される出力:
# nvecd version 0.1.0
```

---

## サーバーの実行

### 基本的な使用方法

```bash
# サンプル設定で実行
./build/bin/nvecd -c examples/config.yaml

# サーバーは 127.0.0.1:11017 でリッスンを開始します
```

期待される出力:

```
[2025-11-18 14:30:00.123] [info] nvecd version 0.1.0 starting...
[2025-11-18 14:30:00.125] [info] Loading configuration from examples/config.yaml
[2025-11-18 14:30:00.127] [info] TCP server listening on 127.0.0.1:11017
[2025-11-18 14:30:00.128] [info] nvecd ready to accept connections
```

### サーバーのテスト

`nc` (netcat) を使用して接続:

```bash
# サーバーに接続
nc localhost 11017

# コマンドを試す
EVENT user1 item1 100
VECSET item1 3 0.1 0.5 0.8
SIM item1 10 fusion

# 期待されるレスポンス
OK
OK
OK RESULTS 0
```

### システムサービスとして実行 (systemd)

**注意**: systemd サービス設定は将来のリリースで計画されています。

現在は、nvecd を手動で実行するか、`supervisord` などのプロセスマネージャーを使用できます。

---

## 設定

本番環境で nvecd を実行する前に、設定ファイルを作成してください:

```bash
# サンプル設定のコピー
sudo mkdir -p /etc/nvecd
sudo cp examples/config.yaml /etc/nvecd/config.yaml

# 設定の編集
sudo nano /etc/nvecd/config.yaml
```

詳細な設定オプションについては [設定ガイド](configuration.md) を参照してください。

---

## 開発ビルド

デバッグを有効にした開発作業用:

```bash
# サニタイザー付き Debug ビルド
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_ASAN=ON

cmake --build build --parallel

# デバッグログ付きで実行
./build/bin/nvecd -c examples/config.yaml --log-level debug
```

### コードフォーマット

```bash
# すべてのソースファイルをフォーマット
make format

# または手動で
find src tests -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

### 静的解析

```bash
# clang-tidy の実行
make lint

# または手動で
clang-tidy src/**/*.cpp -- -std=c++17
```

---

## トラブルシューティング

### ビルド失敗

**問題**: CMake が yaml-cpp を見つけられない

**解決策**: yaml-cpp はバンドルされています。最初のビルド時にインターネットアクセスがあることを確認してください (CMake が依存関係を取得します)。

```bash
# クリーンして再ビルド
make clean
make
```

**問題**: コンパイラのバージョンが古い

**解決策**: GCC 9+ または Clang 10+ にアップデート:

```bash
# Ubuntu/Debian
sudo apt-get install -y gcc-9 g++-9
export CC=gcc-9
export CXX=g++-9

# または Clang を使用
sudo apt-get install -y clang-10
export CC=clang-10
export CXX=clang++-10
```

### テスト失敗

**問題**: サニタイザーエラーでテストが失敗する

**解決策**: これは実際のバグを示している可能性があります。まずサニタイザーなしでテストを実行してください:

```bash
# サニタイザーなしでビルド
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cd build && ctest --output-on-failure
```

### ランタイムの問題

**問題**: サーバーがポートにバインドできない

**解決策**: ポート 11017 が既に使用されているか確認:

```bash
# ポートを使用しているプロセスを確認
lsof -i :11017

# または config.yaml で別のポートを使用
api:
  tcp:
    port: 12017
```

**問題**: スナップショットディレクトリへのアクセス権限がない

**解決策**: 適切な権限でディレクトリを作成:

```bash
sudo mkdir -p /var/lib/nvecd/snapshots
sudo chown $(whoami) /var/lib/nvecd/snapshots
chmod 755 /var/lib/nvecd/snapshots
```

---

## セキュリティ注意事項

### 本番環境へのデプロイ

1. **root 以外のユーザーで実行**:

```bash
# 専用ユーザーを作成
sudo useradd -r -s /bin/false nvecd

# 所有権の変更
sudo chown -R nvecd:nvecd /var/lib/nvecd
```

2. **設定ファイルの保護**:

```bash
# 設定ファイルのセキュア化
sudo chmod 600 /etc/nvecd/config.yaml
sudo chown nvecd:nvecd /etc/nvecd/config.yaml
```

3. **CIDR フィルタリングの使用**:

config.yaml で `network.allow_cidrs` を設定してアクセスを制限:

```yaml
network:
  allow_cidrs:
    - "10.0.0.0/8"      # プライベートネットワークのみ
    - "172.16.0.0/12"
```

4. **ログの監視**:

```bash
# ログの追跡
tail -f /var/log/nvecd/nvecd.log
```

---

## 次のステップ

インストール成功後:

1. **nvecd の設定**: [設定ガイド](configuration.md) を参照
2. **プロトコルの学習**: [プロトコルリファレンス](protocol.md) を参照
3. **クライアントライブラリの使用**: [クライアントライブラリガイド](libnvecdclient.md) を参照
4. **永続化の設定**: [スナップショット管理](snapshot.md) を参照
5. **パフォーマンス最適化**: [パフォーマンスガイド](performance.md) を参照

---

## ヘルプの入手

- **ドキュメント**: [docs/ja/](.)
- **問題報告**: GitHub Issues
- **サンプル**: [examples/](../../examples/)
