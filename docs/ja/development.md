# 開発

このページでは nvecd 自体に手を入れる際の情報、すなわちコードの配置、ビルドと反復のループ、テストの層、サニタイザーとカバレッジのビルド、フォーマットと lint、継続的インテグレーションの実際の内容、そして変更が従うべき取り決めを扱います。

初回のビルド手順は [インストール](./installation.md) にあります。以下は configure 済みの `build/` ディレクトリがある前提です。

## リポジトリの構成

ソースは関心事ごとに `src/` の下で分かれており、各サブディレクトリが 1 つの静的ライブラリになります。

| ディレクトリ | 内容 |
|---|---|
| `src/utils/` | `Expected`、`Error`、構造化ログ、文字列・メモリ・パス・ネットワークのヘルパー、ファイルディスクリプタとフラグの RAII ガード |
| `src/config/` | YAML パーサー、検証に使う JSON スキーマ、ビルド時に生成されるその埋め込みコピー、`CONFIG SHOW` の整形、実行時変数マネージャ |
| `src/events/` | イベントのリングバッファ、共起索引、重複排除キャッシュ、状態キャッシュ |
| `src/vectors/` | ベクトルストア、HNSW と IVF の索引、メタデータストアとフィルタ、CPU 機能検出、スカラー・AVX2・NEON の距離カーネル |
| `src/similarity/` | ANN 索引を所有し、ベクトルと共起の信号を組み合わせる検索エンジン |
| `src/cache/` | クエリキャッシュ、そのキー生成と MD5、LZ4 による結果圧縮、実行時に有効・無効を切り替えるコントローラ |
| `src/storage/` | スナップショット形式、fork 方式とロック方式のスナップショットセッション、チェックポイントのサイドカーを持つ先行書き込みログ |
| `src/server/` | 接続アクセプタ、I/O リアクタとその多重化バックエンド、スレッドプール、コマンドパーサーとディスパッチャ、コマンドごとのハンドラ、HTTP サーバー、レート制限、バックグラウンドのスケジューラ |
| `src/client/` | C++ と C のクライアントライブラリ。1 つの共有ライブラリとしてビルドされます |
| `src/cli/` | `nvecd-cli` |

`src/main.cpp` はサーバーのエントリポイントで、引数の解析、ログ設定の適用、起動と停止の制御しか行いません。

`src/` の外では、`tests/` が CTest のツリー、`e2e/` が Python のエンドツーエンドスイート、`examples/` が注釈付きの設定ファイル、`support/` が開発用スクリプトと設定ドキュメントの生成器、`third_party/` が `FetchContent` の宣言、`cmake/` がパッケージ設定とアンインストールのテンプレートを保持します。

新しいモジュールは自分のサブディレクトリと、`src/CMakeLists.txt` 内の自分の静的ライブラリ、そして同ファイル末尾の `-Werror=switch` 対象リストへの登録を持ちます。依存は内向きです。`nvecd_utils` は nvecd の他のどれにも依存せず、`nvecd_server` はすべてに依存してかまいません。

## ビルドと反復

```bash
make            # 必要なら configure してからコア数だけ並列にビルド
make test       # ビルドしてから CTest を実行
make rebuild    # build/ を削除して一からビルド
make run        # ビルドして examples/config.yaml でサーバーを起動
```

`make` は既存の CMake キャッシュを再利用します。CMake オプションを変えるには、新しい値で configure をやり直します。

```bash
make CMAKE_OPTIONS="-DENABLE_ASAN=ON" configure
make
```

テストの実行は 3 つの変数で調整します。

```bash
make test TEST_JOBS=1                  # 逐次実行
make test TEST_JOBS=2 TEST_VERBOSE=1   # 2 並列かつ詳細出力
make test TEST_DEBUG=1                 # CTest に --debug を渡す
```

`TEST_JOBS` の既定値は 4 です。`TEST_VERBOSE` と `TEST_DEBUG` の既定値は 0 で、それぞれ `--verbose` と `--debug` を追加します。

## Makefile のターゲット

| ターゲット | 内容 |
|---|---|
| `help` | ターゲット一覧とテスト用変数を表示します |
| `configure` | `PREFIX` を `CMAKE_INSTALL_PREFIX` として、`CMAKE_OPTIONS` があればそれも添えて `build/` へ CMake を実行します |
| `build` | 既定のターゲット。configure してから並列にビルドします |
| `test` | ビルドしてから、`TEST_JOBS`、`TEST_VERBOSE`、`TEST_DEBUG` を反映して CTest を実行します |
| `test-full` | コア数と同じ並列度で `test` を実行します |
| `test-sequential` | 並列度 1 で `test` を実行します。競合下でのみ失敗するテストの切り分けに使います |
| `test-verbose` | CTest の詳細出力付きで `test` を実行します |
| `quick-test` | ビルドしてから、イベントストアとベクトルストアに名前が一致するテストだけを実行します。速い内側のループ用です |
| `clean` | `build/` を削除します |
| `rebuild` | `clean` に続けて `build` を実行します |
| `install` | ビルドしてから `PREFIX` へインストールします |
| `uninstall` | そのビルドのインストールマニフェストに載っているファイルを削除します |
| `format` | `src/` と `tests/` の `.cpp` と `.h` をすべて `clang-format` で書き換えます |
| `format-check` | 同じファイル群を検査のみのモードで確認し、変更が必要な最初のファイルで失敗します |
| `lint` | フォーマットとビルドを行い、すべてのソースに `clang-tidy` をかけます |
| `lint-diff` | 同じことを、作業ツリーで変更された `src/` 配下の `.cpp` に限って行います |
| `lint-diff-main` | 同じことを、`main` からの差分に限って行います |
| `run` | ビルドしてから `examples/config.yaml` でサーバーを起動します。ファイルがなければ失敗します |
| `e2e-test` | ビルドしてから Python スイート全体を実行します |
| `e2e-test-smoke` | ビルドしてから Python のスモークテストだけを実行します |
| `e2e-test-commands` | ビルドしてから Python のコマンド別テストだけを実行します |

既定値を上書きする変数は 3 つあります。インストール先の `PREFIX`、フォーマッターのバイナリを指す `CLANG_FORMAT`、configure への追加引数を渡す `CMAKE_OPTIONS` です。

## テストの層

層は 3 つあり、既定で走るのは最初のものだけです。

### CTest のユニットテスト

スイートの大半は `gtest_discover_tests` で登録された GoogleTest のバイナリで、領域ごとに 1 つの実行ファイルが `tests/` の下に `src/` と同じ構成で並びます。まとめて実行するには CTest を使います。

```bash
ctest --test-dir build --output-on-failure
```

バイナリを直接実行することもできます。こちらのほうが速く、GoogleTest 自身の絞り込みが使えます。

```bash
./build/bin/event_store_test
./build/bin/event_store_test --gtest_filter="EventStoreTest.*"
```

この層のうち 2 つは GoogleTest のバイナリではありません。ラベル `docs` を持つ `docs_contract_test` は、ドキュメントをソースと突き合わせます。生成される設定リファレンスと設定例が最新であること、スキーマの各キーが解析され参照可能で `src/config/` の外から実際に読まれていること、プロトコルのページが約束するフィールド名とステータス語がソースに同じ綴りで存在すること、英語版と日本語版が同じ事実を述べていること、そしてすべてのエラーコードに生成箇所があることを確認します。存在しない応答フィールドをドキュメントに書けばここで落ちます。ラベル `install` を持つ `install_consumer_smoke` は、使い捨てのプレフィックスへプロジェクトをインストールし、エクスポートされた CMake パッケージに対して小さな利用側をビルドします。

テストにはこの 2 つ以外にもラベルが付いています。`integration`、`benchmark`、`http`、`client`、`e2e`、`sanitizer`、`concurrency`、`crash` です。`ctest -L` と `ctest --label-exclude` で選択または除外します。

### integration ラベルの付いたテスト

`tests/integration/` には、実際のソケットでサーバーを起動したり、スナップショットのために fork したりする、ミリ秒ではなく秒単位のテストが入っています。コマンドのエンドツーエンドの網羅、負荷下でのリアクタの挙動、並行性、敵対的な入力、キャッシュの挙動、メトリクス、スケールのシナリオ、そしてクラッシュを含む先行書き込みログの復旧です。これらは `integration` ラベルで登録され、ローカルの既定の `ctest` 実行には含まれます。

```bash
ctest --test-dir build -L integration --output-on-failure
ctest --test-dir build --label-exclude integration --output-on-failure
```

継続的インテグレーションが実行するのは後者の形です。つまりこれらのテストは、ローカルで実行した人がいるかどうかでしか検証されません。

### Python のエンドツーエンドスイート

`e2e/` は CTest が関知しない独立した層です。ビルド済みサーバーを実際の TCP と HTTP の口から `pytest` で駆動するもので、`e2e-test` 系のターゲットからしか到達できません。

```bash
make e2e-test
make e2e-test-smoke
make e2e-test-commands
```

このスイートには Python 3 のインタープリターと、インストール済みの `pytest`、`pytest-timeout` が必要です。要求されるインタープリターのバージョンは `e2e/pyproject.toml` に記録されています。テストは `smoke`、`commands`、`edge_cases`、`workflows` に分かれ、pytest のマーカーとして登録されているため `-m` でも部分選択できます。フィクスチャーはセッションごとに 1 つのサーバーを、実行時に選んだポートと専用の一時スナップショットディレクトリと専用のパスワードで起動し、バイナリが `build/bin/nvecd` にあることを前提とします。別のビルドツリー、たとえばサニタイザービルドのバイナリを使うには `NVECD_E2E_BINARY` を指定します。

### ベンチマーク

`tests/benchmark/` には計測用のバイナリが 2 つあります。テストには `DISABLED_` の接頭辞が付いているため通常の実行では飛ばされ、コストはかかりません。何を測り、どう実行するかは [ベンチマーク](./benchmarks.md) を参照してください。

## サニタイザービルド

サニタイザーは configure 時のオプションなので、使用中のツリーを再 configure するのではなく、それぞれ別のビルドディレクトリを用意します。

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan -L concurrency --output-on-failure
```

この 2 つを 1 つのビルドで組み合わせることはできません。ThreadSanitizer で走らせる価値が最も高いのは `sanitizer` と `concurrency` のラベルが付いたテスト、すなわち ANN 索引のテスト、索引の共通契約テスト、階層ストアのテスト、類似度の並行ストレステストです。

## カバレッジ

```bash
cmake -B build-coverage -DENABLE_COVERAGE=ON
cmake --build build-coverage --parallel
cmake --build build-coverage --target coverage
```

`coverage` ターゲットはカウンタを 0 に戻し、スイート全体を並列に実行し、データを収集し、`src/` に絞り込んだうえで `build-coverage/coverage/html/` に HTML を出力します。これは `lcov` と `genhtml` の両方がパス上にあるときにだけ定義されます。なければ CMake が configure 時に警告し、ターゲットは作られません。`coverage-clean` はカウンタを 0 に戻して出力ディレクトリを削除します。

## フォーマット

フォーマットはリポジトリ直下の `.clang-format` に従う `clang-format` です。Google スタイル、C++17、1 行 120 桁、インデント 2 スペース、ポインタは型側に寄せ、インクルードはソートして再グループ化します。

```bash
make format
make format-check
```

`format` はファイルをその場で書き換えます。`format-check` はその検査専用の対になるターゲットで、同じファイル群を `--dry-run --Werror` で確認します。マージ前のチェックに使う想定のものですが、継続的インテグレーションは現在これを実行していません。

どちらも `src/` と `tests/` の `.cpp` と `.h` をすべて対象にします。マシン上でバイナリ名が `clang-format` でない場合は `CLANG_FORMAT` を使います。

```bash
make CLANG_FORMAT=clang-format-18 format
```

## Lint

```bash
make lint
make lint-diff
make lint-diff-main
```

いずれも `support/dev/run-clang-tidy.sh` に処理を委ねます。このスクリプトの作りから 2 つのことが導かれます。

まず、コンパイル済みのビルドが必要です。翻訳単位ごとの正確なフラグを `build/compile_commands.json` から読むためで、ビルドディレクトリかそのファイルがなければエラーで停止します。コンパイルデータベースなしにソースへ直接 `clang-tidy` をかけても動きません。ソースは生成されたヘッダや取得されたヘッダを取り込んでおり、その場所はビルドしか知らないためです。

次に、すべてのファイルに 1 つの設定を適用します。リポジトリ直下の `.clang-tidy` を `--config-file` で明示的に渡します。この設定はコンパイラ診断とアナライザー診断に加えて `cppcoreguidelines`、`modernize`、`performance`、`readability` の各系統を有効にし、ヘッダの診断は `src/` に限定します。警告は助言的な扱いです。スクリプトは警告を表示して件数を報告し、どちらの場合も正常終了するため、lint の出力は失敗を当てにするのではなく読む必要があります。

`lint-diff` は `HEAD` との差分がある `src/` 配下の `.cpp` を、ステージ済みかどうかにかかわらず選びます。`lint-diff-main` は `main` との差分を選びます。どちらも逐次実行にフォールバックし、全体の `make lint` は `run-clang-tidy` が使えるときは並列に実行します。3 つとも `format` と `build` に依存するため、確認の前にファイルを書き換えてコンパイルします。

## 生成されるドキュメント

ドキュメントの一部は書かれたものではなく生成されたものです。`src/config/config-schema.json` が設定オプションのすべて、すなわち型、既定値、受け付ける範囲、両言語の説明文についての唯一の権威であり、`support/generate_config_docs.py` がそれを 3 つの成果物へ描画します。

- `examples/config.yaml`。ヘッダコメントを含めて全体を生成します。
- `docs/en/configuration.md` のオプション表。スキーマのセクションごとに 1 つです。
- `docs/ja/configuration.md` のオプション表。同じ組み合わせです。

2 つの設定ガイドについて、生成器が所有するのはファイル全体ではなく、`<!-- BEGIN GENERATED: options <section> -->` と `<!-- END GENERATED: options <section> -->` で区切られた領域だけです。末端のキーを持つスキーマのセクションごとに 1 組あります。マーカーの外はすべて手書きの文章で、生成器は手を触れません。マーカーの間はすべて、実行のたびに上書きされます。

したがって設定オプションを追加または変更するには、スキーマを編集して生成器を再実行します。

```bash
python3 support/generate_config_docs.py
```

書き換えた成果物を表示し、すでに最新のものには何もしません。描画された表や `examples/config.yaml` を手で編集してはいけません。次回の実行で失われます。これは日本語の文言についても同じです。日本語の表は各キーの `description_ja` から文言を取り、その項目がなければ `description` にフォールバックします。つまりオプションを翻訳するとは、日本語の表を編集することではなく、スキーマに `description_ja` を足すことです。

マーカーは双方向の取り決めです。スキーマに存在しないセクションを指す領域があると生成器はエラーで止まるため、スキーマのセクションを改名するときはその領域も改名します。逆に、あるセクションのマーカーがガイドから欠けていると、そこには何も描画されないまま黙って通り、その言語の表だけが手作業で乖離していきます。

チェックモードは何も書かずに報告します。

```bash
python3 support/generate_config_docs.py --check
```

スキーマと食い違っている成果物をすべて挙げ、終了ステータスを 0 以外にします。`--source-root` を使うと、スクリプト自身が置かれている checkout とは別の checkout を対象にできます。

`Makefile` にも、ワークフローファイルのどのステップにも、生成器を実行するものはありません。自動的に実行される唯一の経路は `docs_contract_test` で、そこでは最初のチェックとしてチェックモードで呼ばれます。このテストのラベルは `integration` ではなく `docs` なので、継続的インテグレーションが実行するテストに含まれます。スキーマを変更して再生成せずにコミットすれば、そこで失敗します。

## 継続的インテグレーション

`.github/workflows/ci.yml` のワークフローは `main` への push と `main` に対するプルリクエストで動き、変更が Markdown、`docs/`、ライセンスに限られる場合は実行をスキップします。実際に行っていることを、実態以上の網羅性を前提にしないように率直に挙げます。

- Ubuntu だけで実行します。macOS は対応プラットフォームですが自動的な検証はありません。
- `NVECD_PORTABLE_BUILD=ON` で `Debug` ビルドを configure するため、テストされるバイナリはランナー自身ではなくベースラインのアーキテクチャ向けです。
- カバレッジは `ENABLE_COVERAGE` オプションではなく、`CMAKE_CXX_FLAGS` と `CMAKE_C_FLAGS` に `--coverage` を手で足して有効にします。したがって `coverage` ターゲットとその `lcov` による絞り込みは関与せず、生の `gcov` データがアップロードされます。
- `ctest --label-exclude integration` を実行するため、`integration` ラベルのテストは 1 つも走りません。スナップショット、fork、先行書き込みログの復旧に関するテストは、ローカルで検証されるか、まったく検証されないかのどちらかです。
- `lint`、`format-check`、いずれのサニタイザー、Python のエンドツーエンドスイートも実行しません。

最後の 2 点に挙げたものは、変更を提出する前に貢献者が自分で確認する範囲です。

## 取り決め

コミットの件名は Conventional Commits に従い、上で述べた領域に対応する 6 つのスコープのいずれかを使います。`cache`、`server`、`events`、`vectors`、`similarity`、`client` です。

エラーコードはモジュールごとに帯で分割されており、新しいモジュールは他のモジュールの帯を延長せずに未使用の帯を取ります。

| 範囲 | モジュール |
|---|---|
| 0-999 | 一般 |
| 1000-1999 | 設定 |
| 2000-2999 | イベント処理 |
| 3000-3999 | コマンド解析 |
| 4000-4999 | ベクトルと類似度 |
| 5000-5999 | ストレージとスナップショット |
| 6000-6999 | ネットワークとサーバー |
| 7000-7999 | クライアント |
| 8000-8999 | キャッシュ |

列挙に加える新しいコードには、少なくとも 1 つの生成箇所が必要です。`docs_contract_test` は生成できないコードで失敗します。ただし縮小のみが許される固定の一覧を同テストが保持しており、そこに載っているものは例外です。したがって、それを送出するコードパスより先にコードだけを追加するとスイートが壊れます。

コメントと識別子は英語です。失敗は例外ではなく `Expected<T, Error>` で伝播するため、失敗しうる新しい関数はこれを返すのであって、別経路で通知するのではありません。
