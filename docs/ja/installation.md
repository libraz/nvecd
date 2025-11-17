# インストールガイド

このガイドでは、Nvecd のビルドとインストールの詳細な手順を説明します。

## 前提条件

- C++17 対応コンパイラ（GCC 9+、Clang 10+）
- CMake 3.15+
- yaml-cpp（third_party/ に同梱）

### 依存関係のインストール

#### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake g++
```

#### macOS

```bash
brew install cmake
```

## ソースからのビルド

### Makefile を使用（推奨）

```bash
# リポジトリのクローン
git clone https://github.com/yourusername/nvecd.git
cd nvecd

# ビルド
make

# テスト実行
make test

# ビルドをクリーン
make clean

# その他の便利なコマンド
make help      # 利用可能なコマンド一覧を表示
make rebuild   # クリーン後に再ビルド
make format    # clang-format でコード整形
```

### CMake を直接使用

```bash
# ビルドディレクトリの作成
mkdir build && cd build

# 設定とビルド
cmake ..
cmake --build . --parallel

# テスト実行
ctest --output-on-failure
```

## バイナリのインストール

### システム全体へのインストール

`/usr/local` にインストール（デフォルト）：

```bash
sudo make install
```

以下がインストールされます：
- バイナリ: `/usr/local/bin/nvecd`
- 設定サンプル: `/usr/local/etc/nvecd/config.yaml`
- ドキュメント: `/usr/local/share/doc/nvecd/`

### カスタムディレクトリへのインストール

任意のディレクトリにインストール：

```bash
make PREFIX=/opt/nvecd install
```

### アンインストール

インストールしたファイルを削除：

```bash
sudo make uninstall
```

## テストの実行

Makefile を使用:

```bash
make test
```

または CTest を直接使用:

```bash
cd build
ctest --output-on-failure
```

現在のテストカバレッジ: **173 テスト、100% 合格**

## ビルドオプション

Makefile 使用時に CMake オプションを設定できます：

```bash
# AddressSanitizer を有効化
make CMAKE_OPTIONS="-DENABLE_ASAN=ON" configure

# ThreadSanitizer を有効化
make CMAKE_OPTIONS="-DENABLE_TSAN=ON" configure

# テストを無効化
make CMAKE_OPTIONS="-DBUILD_TESTS=OFF" configure
```

## インストールの確認

インストール後、バイナリが利用可能か確認：

```bash
# サーバーバイナリの確認
nvecd --help
```

## サーバーの実行

### 手動実行

```bash
# 設定ファイルを指定して実行
nvecd -c examples/config.yaml

# またはカスタム設定で実行
nvecd -c /etc/nvecd/config.yaml
```

### サービスとして実行 (systemd)

> ⚠️ **未実装** - systemd サービス設定は将来のリリースで予定されています。

## セキュリティに関する注意

- 設定ファイルは nvecd ユーザーのみが読み取り可能にすべきです（モード 600）
- 本番環境では非 root ユーザーで実行してください
- ダンプディレクトリを適切なファイルパーミッションで保護してください

## 次のステップ

インストール完了後：

1. [設定ガイド](configuration.md) を参照して設定ファイルをセットアップ
2. [プロトコル仕様](protocol.md) でテキストプロトコルを学習
3. `nvecd -c config.yaml` でサーバーを起動
