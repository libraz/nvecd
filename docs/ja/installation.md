# インストール

このページでは、nvecd をソースからビルドしてインストールする手順、サーバーの起動方法、およびビルドや起動に失敗したときの切り分け方を説明します。

## 対応プラットフォーム

nvecd は Linux と macOS の x86_64 および 64 ビット ARM でビルドして動作します。I/O リアクタが `epoll` または `kqueue` を使うため POSIX 系のシステムが前提であり、Windows 向けのビルドはありません。

継続的インテグレーションがビルドとテストを行うのは Ubuntu だけです。macOS は対応プラットフォームであり、ビルド設定にも macOS 固有の記述がありますが、自動実行されるジョブはないため、macOS でのリグレッションはその環境でビルドした人が見つけることになります。

## コンパイラと CMake の要件

- CMake 3.15 以上。
- C++17 対応コンパイラ、すなわち GCC 9 以上または Clang 10 以上。ビルドは GCC と Clang だけが受け付けるフラグを設定するため、これ以外のコンパイラは想定していません。
- zlib の開発ヘッダ。`find_package(ZLIB REQUIRED)` で探索します。
- pthreads の実装。`find_package(Threads REQUIRED)` で探索します。
- Python 3 インタープリター。テストをビルドする場合、つまり既定の設定では、configure 時点で必須です。見つからないと configure が失敗します。
- 任意で `readline`。見つかると `nvecd-cli` にタブ補完と履歴が入り、見つからなければその機能なしでビルドされます。

ここに挙げたバージョンがプロジェクト全体の互換性の下限であり、他のページでは繰り返しません。

## 同梱される依存ライブラリ

7 つの依存ライブラリを CMake の `FetchContent` が configure 時に取得し、プロジェクトの一部としてビルドします。事前にインストールしておくものはありませんが、クリーンなツリーの最初の configure ではネットワーク接続が必要です。

| 依存ライブラリ | 用途 |
|---|---|
| yaml-cpp | YAML 設定ファイルの解析 |
| GoogleTest | テストスイート。`BUILD_TESTS` が有効なときだけ取得します |
| spdlog | サーバーのログ出力 |
| lz4 | キャッシュした検索結果の圧縮 |
| nlohmann/json | HTTP API の JSON リクエストと応答 |
| cpp-httplib | HTTP サーバー |
| json-schema-validator | 埋め込みスキーマによる設定の検証 |

いずれも変更されないタグまたはコミットに固定してあるため、クリーンビルドは常に同じソースに解決されます。macOS では Homebrew のプレフィックスを `find_package` と `find_library` のためだけに `CMAKE_PREFIX_PATH` へ追加し、インクルードパスには意図的に加えません。Homebrew 側にこれらのライブラリが入っていても、固定したバージョンを覆い隠すことはありません。

## システムパッケージ

### Debian と Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git python3 zlib1g-dev
```

CLI の行編集と、フォーマットおよび lint のターゲットを使う場合は次も入れます。

```bash
sudo apt-get install -y libreadline-dev clang-format clang-tidy
```

### macOS

Xcode コマンドラインツールを入れてから、ビルドツールを入れます。

```bash
xcode-select --install
brew install cmake
```

上と同じ用途で必要になるものは次のとおりです。

```bash
brew install readline llvm
```

`clang-format` と `clang-tidy` はコマンドラインツールではなく `llvm` の formula に含まれ、その formula は既定では `PATH` にリンクされません。

## make でビルドする

`Makefile` は CMake のラッパーです。`build/` へ configure したうえで、検出したコア数だけ並列にビルドします。

```bash
git clone https://github.com/libraz/nvecd.git
cd nvecd
make
```

このラッパーはビルドタイプを渡さないため、素の `make` では警告は有効なまま最適化レベルが選択されないビルドができます。計測や配備に使うビルドでは、configure の段階でビルドタイプを指定してください。

```bash
make CMAKE_OPTIONS="-DCMAKE_BUILD_TYPE=Release" configure
make
```

`CMAKE_OPTIONS` を読むのは `configure` ターゲットだけです。`build/` に CMake のキャッシュができた後の `make` はそれを再利用するので、オプションを変えるには `configure` をやり直します。

## CMake を直接使ってビルドする

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

どちらの方法で configure した場合も、実行ファイルは `build/bin/`、ライブラリは `build/lib/` に出力されます。

## ビルドタイプ

| ビルドタイプ | 追加されるフラグ |
|---|---|
| `Debug` | `-g -O0 -fno-omit-frame-pointer` |
| `Release` | `-O3 -DNDEBUG` |
| `RelWithDebInfo` | そのタイプに対する CMake の既定値 |
| （未指定） | どちらも設定されず、共通の警告フラグとアーキテクチャフラグだけが効きます |

どのビルドにも `-Wall -Wextra -Wpedantic` と、後述する `NVECD_PORTABLE_BUILD` が決めるアーキテクチャフラグが付きます。さらに nvecd 自身のターゲットでは `-Wswitch` をエラーに格上げしてあるため、列挙子を追加するとその値について判断しているすべての switch でビルドが止まります。

## CMake オプション

| オプション | 既定値 | 効果 |
|---|---|---|
| `BUILD_TESTS` | `ON` | テスト実行ファイルをビルドし、GoogleTest を取得し、Python 3 インタープリターを必須にします |
| `ENABLE_ASAN` | `OFF` | `-fsanitize=address -fno-omit-frame-pointer` を追加します |
| `ENABLE_TSAN` | `OFF` | `-fsanitize=thread` を追加します |
| `ENABLE_COVERAGE` | `OFF` | `--coverage -O0 -g` を追加し、`coverage` と `coverage-clean` ターゲットを定義します |
| `NVECD_PORTABLE_BUILD` | 無効 | ビルドホストへの最適化ではなくベースラインのアーキテクチャを選びます |

`NVECD_PORTABLE_BUILD` はアーキテクチャフラグを決めます。無効のままだと `-march=native` が付き、コンパイルしたマシンに合わせて最適化されるため、より古い CPU では不正命令で落ちる可能性があります。有効にすると x86_64 では `-march=x86-64`、64 ビット ARM では `-march=armv8-a` が付き、同じアーキテクチャファミリーならどこでも動きます。ビルドホストと実行ホストが同じマシンでない場合は有効にしてください。継続的インテグレーションはこれを有効にしてビルドしています。

上の 4 つと違い、`NVECD_PORTABLE_BUILD` は CMake の `option()` で宣言されていません。キャッシュエントリもヘルプテキストもなく、`cmake -LH` の出力にも現れないため、存在を知らせるものが何もありません。明示的に指定してください。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNVECD_PORTABLE_BUILD=ON
```

サニタイザーは排他です。`ENABLE_ASAN` と `ENABLE_TSAN` を両方有効にしたビルドはリンクできません。

バイナリに埋め込まれるバージョンは、環境変数 `NVECD_VERSION` が設定されていればその値、なければ `git describe --tags --abbrev=0` の結果です。

## 生成される成果物

ビルドが完了すると 3 つのものができます。

- `build/bin/nvecd` — サーバー。
- `build/bin/nvecd-cli` — TCP プロトコル用の対話式および単発実行クライアント。
- Linux では `build/lib/libnvecdclient.so`、macOS では `build/lib/libnvecdclient.dylib` — 共有クライアントライブラリ。詳細は [クライアントライブラリ](./client-library.md) を参照してください。

## インストール

### 既定のプレフィックスへ

```bash
make
sudo make install
```

既定のプレフィックスは `/usr/local` です。インストールされるファイルは次のとおりです。

| パス | 内容 |
|---|---|
| `bin/nvecd`、`bin/nvecd-cli` | サーバーと CLI |
| `lib/libnvecdclient.*` | 共有クライアントライブラリ |
| `include/nvecd/` | `nvecdclient.h`、`nvecdclient_c.h`、および `utils/error.h`、`utils/expected.h` |
| `lib/cmake/nvecd/` | `nvecd::client` ターゲットをエクスポートする CMake パッケージ |
| `etc/nvecd/config.yaml` | 注釈付きの設定例 |
| `etc/nvecd/config-schema.json` | サーバーが設定を検証するスキーマ |
| `share/doc/nvecd/` | `README.md` と両言語のドキュメント一式 |

CMake プロジェクトからは次のようにインストール済みのクライアントライブラリを使います。

```cmake
find_package(nvecd CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE nvecd::client)
```

### 別のプレフィックスへ

```bash
make PREFIX=/opt/nvecd install
```

CMake を直接使う場合は次のようにします。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/nvecd
cmake --build build --parallel
sudo cmake --install build
```

`Makefile` は `PREFIX` を configure の段階で `CMAKE_INSTALL_PREFIX` として渡すため、`install` の行で指定するとツリーが再 configure されます。

### アンインストール

```bash
sudo make uninstall
```

これは CMake のターゲットを直接呼ぶのと同じです。

```bash
sudo cmake --build build --target nvecd-uninstall
```

削除はビルドディレクトリの `install_manifest.txt` を読むため、そのビルドがインストールしたものだけを正確に取り除きます。クリーンしてしまったビルドツリーからは、以前のインストールをアンインストールできません。

## インストールの確認

```bash
nvecd --version
nvecd --help
```

前者はバイナリのバージョンと 1 行の説明を、後者は受け付けるオプションを表示します。

サーバーを起動せずに設定ファイルを検証するには次のようにします。

```bash
nvecd --config-test -c /etc/nvecd/config.yaml
```

ファイルが解析および検証を通れば終了ステータスは 0 です。成功時にはイベント、ベクトル、類似度、API、パフォーマンスの各セクションの要約が表示されるので、編集が反映されたかを確認する最短の方法になります。

## サーバーの起動

サーバーは設定ファイルを `-c`／`--config` で受け取るほか、位置引数としても受け取ります。

```bash
nvecd -c /etc/nvecd/config.yaml
```

| オプション | 意味 |
|---|---|
| `-c`、`--config` | 設定ファイルのパス |
| `-t`、`--config-test` | ファイルを検証し、要約を表示して終了します |
| `-h`、`--help` | 使い方を表示して終了します |
| `-v`、`--version` | バージョン情報を表示して終了します |

設定ファイルなしで起動した場合はコンパイル時の既定値で動作し、`network.allow_cidrs` は `127.0.0.1/32` に制限されます。誤って起動したサーバーがネットワークから到達可能にならないためです。それ以外の設定はすべて既定値のままです。設定項目の一覧は [設定](./configuration.md) を参照してください。

サーバー自身のオプションに環境変数は一切使いません。すべて設定ファイルで指定します。`SIGINT` と `SIGTERM` は正常終了を要求します。設定を再読み込みするシグナルはないため、実行時に変更できる変数以外の設定変更には再起動が必要です。

### CLI の認証

`security.requirepass` を設定している場合、`nvecd-cli` にはパスワードが必要です。渡し方は 2 通りあり、どちらもパスワードをプロセスの引数リストに載せません。引数リストは同じマシンのどのユーザーからも読めるためです。

```bash
chmod 600 /path/to/nvecd-password
nvecd-cli --password-file /path/to/nvecd-password INFO
```

パスワードファイルは CLI を実行するユーザーが所有する通常ファイルで、グループとその他の権限がないことが条件です。それ以外は CLI が拒否し、シンボリックリンクもたどりません。

もう一方が、nvecd がオプションのために環境を参照する唯一の場所です。`--password-env` は読み取る変数名を指定するものであり、固定の変数名を読むわけではありません。

```bash
export NVECD_PASSWORD='replace-me'
nvecd-cli --password-env NVECD_PASSWORD INFO
```

この 2 つは排他です。復帰改行や改行を含むパスワードは、行指向のプロトコルでは扱えないため拒否されます。

## systemd での運用

サーバーはフォアグラウンドで動作し `SIGTERM` で停止するため、単純なサービスユニットで足ります。

```text
[Unit]
Description=nvecd vector search engine
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=nvecd
Group=nvecd
ExecStart=/usr/local/bin/nvecd -c /etc/nvecd/config.yaml
Restart=on-failure
RestartSec=5

NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/nvecd

[Install]
WantedBy=multi-user.target
```

`ReadWritePaths` には `snapshot.dir` と、先行書き込みログを有効にしている場合は `wal.dir` を含める必要があります。`ProtectSystem=strict` のもとでこれらが欠けていると、サーバーは起動したうえで最初の書き込みで失敗します。

ログをジャーナルに集めるには `logging.file` を空文字列にして標準出力に出し、機械可読な形式が必要なら `logging.json` を `true` にします。

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now nvecd
journalctl -u nvecd -f
```

## ファイル権限と専用ユーザー

自分のデータだけを所有する非特権アカウントでサーバーを動かします。

```bash
sudo useradd --system --shell /usr/sbin/nologin --home-dir /var/lib/nvecd nvecd
sudo mkdir -p /var/lib/nvecd/snapshots /var/lib/nvecd/wal
sudo chown -R nvecd:nvecd /var/lib/nvecd
sudo chmod 700 /var/lib/nvecd/snapshots /var/lib/nvecd/wal
```

設定ファイルは `security.requirepass` を平文で保持するため、サービスアカウントだけが読めるようにします。

```bash
sudo chown root:nvecd /etc/nvecd/config.yaml
sudo chmod 640 /etc/nvecd/config.yaml
```

サーバーはスナップショットと先行書き込みログのセグメントを自身のアカウントで書き込み、`fork` モードのスナップショットは fork した子プロセスが書き込みます。したがって両方のディレクトリは、起動時だけでなくプロセスが動いている間ずっとそのアカウントから書き込み可能である必要があります。

## トラブルシューティング

**最初の configure が依存ライブラリの取得で失敗する。** `FetchContent` は configure 時に固定した依存ライブラリを clone します。プロキシ配下や外向きのネットワークがない環境では、コンパイルが始まる前に configure が失敗します。接続できるマシンで一度 configure するか、Git のリモートに到達できるようにしてください。

**Python インタープリターが見つからず configure が失敗する。** テストツリーが必須としており、テストは既定で有効です。Python 3 を入れるか、テストが不要なら `-DBUILD_TESTS=OFF` を指定して configure してください。

**別のマシンでバイナリが不正命令で落ちる。** `NVECD_PORTABLE_BUILD` なしのビルドはビルドホスト向けに `-march=native` でコンパイルされています。`-DNVECD_PORTABLE_BUILD=ON` で再ビルドしてください。

**`nvecd-cli` でタブ補完が効かない。** CLI を configure した時点で `readline` が見つかっていません。インストールしてから configure をやり直してください。探索結果はキャッシュされるため、再ビルドだけでは反映されません。

**サーバーが bind できずに終了する。** 別のプロセスがポートを掴んでいます。`lsof -i :11017` で確認し、そのプロセスを止めるか `api.tcp.port` を変更してください。

**サーバーは起動するがすべての接続を拒否する。** `network.allow_cidrs` の既定値は空であり、これはすべて拒否を意味します。また設定ファイルなしで起動したサーバーは `127.0.0.1/32` だけを許可します。クライアントのネットワークを明示的に列挙してください。

**スナップショットや先行書き込みログの書き込みが権限エラーになる。** `snapshot.dir` と `wal.dir` が指すディレクトリは、サーバーを動かすアカウントから書き込める状態で存在している必要があります。権限のない親ディレクトリをサーバーが作成することはありません。
