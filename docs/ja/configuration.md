# 設定

nvecd は起動時に YAML ファイルを 1 つ読みます。このページはパーサーが受け付けるすべてのキーをセクションごとに列挙し、オプション表が扱わないキーどうしの規則と運用上の帰結を説明します。

以下のオプション表は `src/config/config-schema.json` から描画されています。型、既定値、範囲、説明のすべてについて権威となるのはこのスキーマです。表を手で編集しても効果はありません。スキーマを直して再生成します。

## 設定ファイルを渡す

```bash
nvecd -c /etc/nvecd/config.yaml
nvecd /etc/nvecd/config.yaml
nvecd -t -c /etc/nvecd/config.yaml
```

| フラグ | 意味 |
|---|---|
| `-c`、`--config <file>` | 設定ファイルのパス |
| `-t`、`--config-test` | ファイルを検証し、要約を表示して終了する |
| `-h`、`--help` | 使い方を表示して終了する |
| `-v`、`--version` | バージョンを表示して終了する |

パスは位置引数としても渡せます。設定ファイルを 2 つ与えるとエラーになります。ファイルをまったく指定せずに起動した場合、サーバーは以下の表に示す組み込みの既定値だけで動作します。ファイルなしの `--config-test` はエラーです。

`examples/config.yaml` は同じスキーマから描画されており、すべてのキーを既定値のまま載せています。

## 検証の流れ

読み込みは 3 段階で行われ、最初の失敗で起動が中断されます。

1. YAML を解析して JSON に変換します。
2. その JSON を埋め込みスキーマと照合します（型、列挙、キーごとの数値範囲）。
3. 解析済みの構造を意味の面で検査します。各セクションに挙げるキーどうしの規則はここで検査されます。

スキーマはルートおよびすべてのセクションで `additionalProperties: false` を指定しているため、綴りを誤ったキーは黙って無視されるのではなく拒否されます。セクションは省略可能で、存在しないセクションはすべて既定値になります。YAML としては正しくても読み込み先の型に収まらない値は、そのキーを名指しして報告されます。たとえば `Invalid value for events.ctx_buffer_size: '...' is out of range for this setting` のようになります。

## `events`

イベントの取り込みと共起グラフの設定です。[events-and-co-occurrence.md](./events-and-co-occurrence.md) を参照してください。

<!-- BEGIN GENERATED: options events -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `ctx_buffer_size` | int | 50 | コンテキストごとに追跡するイベント数（リングバッファのサイズ） (10-10000) |
| `max_contexts` | int | 0 | LRU が保持するアクティブコンテキストの最大数。上限に達すると最後にアクセスされてから最も時間が経ったコンテキストから削除される（0 = 無制限） (0-1000000) |
| `max_neighbors_per_item` | int | 0 | アイテムごとに保持する共起近傍の最大数（0 = 無制限） (0-1000000) |
| `min_support` | float | 0.0 | 絶対スコアがこの閾値未満の共起エッジを削除する（0 = 無効） (>= 0.0) |
| `decay_interval_sec` | int | 3600 | 減衰間隔（秒、0 = 無効） (0-86400) |
| `decay_alpha` | float | 0.99 | 減衰係数 (0.0-1.0) |
| `dedup_window_sec` | int | 60 | 重複排除の時間窓（秒、0 = 無効） (0-86400) |
| `dedup_cache_size` | int | 10000 | 重複排除キャッシュのサイズ（LRU）。0 = 重複排除を完全に無効化する。SET/DEL の冪等性追跡も無効になる (0-1000000) |
| `temporal_cooccurrence` | bool | false | 共起更新に時間減衰を適用する |
| `temporal_half_life_sec` | float | 86400.0 | 時間減衰の半減期（秒。経過秒数がこの値になるごとにスコアが半分になる） (> 0.0) |
| `negative_signals` | bool | false | DEL イベントによる負のシグナル（ランキング低下）を有効にする |
| `negative_weight` | float | 0.5 | 負のシグナルに適用する減少の重み (0.0-1.0) |
<!-- END GENERATED: options events -->

上の範囲に加えて意味の面で検査される規則があります。`ctx_buffer_size` は 0 より大きく、`min_support` は負でなく、`decay_alpha` は 0.0〜1.0 に収まり、`temporal_half_life_sec` は 0 より大きく、`negative_weight` は 0.0〜1.0 に収まる必要があります。

`temporal_half_life_sec` は `temporal_cooccurrence` が true でなければ効かず、`negative_weight` は `negative_signals` が true でなければ効きません。グラフの成長を抑える境界は `max_contexts`、`max_neighbors_per_item`、`min_support` の 3 つです。既定値のままではグラフは無制限で、データに比例して増えます。

## `vectors`

<!-- BEGIN GENERATED: options vectors -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `default_dimension` | int | 768 | 既定のベクトル次元数（ベクトルごとの特徴量数） (1-4096) |
| `distance_metric` | string | "cosine" | 類似検索に用いる距離メトリック (`cosine` `dot` `l2`) |
<!-- END GENERATED: options vectors -->

`default_dimension` は 0 より大きい必要があります。この値が受け付ける次元を決めます。長さの異なるベクトルを渡した `VECSET` は次元不一致で拒否されます。データが存在する状態でこの値を変えると既存のスナップショットが設定と一致しなくなるため、チューニングではなく作り直しになります。[vector-search.md](./vector-search.md) を参照してください。

## `similarity`

検索の挙動、統合の配分、ANN 索引の設定です。[vector-search.md](./vector-search.md) と [fusion.md](./fusion.md) を参照してください。

<!-- BEGIN GENERATED: options similarity -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `default_top_k` | int | 100 | 類似検索が返す既定の結果件数 (1-10000) |
| `max_top_k` | int | 1000 | 類似検索で許可する結果件数の上限 (1-10000) |
| `fusion_alpha` | float | 0.6 | fusion モードにおけるベクトル類似度成分の重み (0.0-1.0) |
| `fusion_beta` | float | 0.4 | fusion モードにおける共起成分の重み (0.0-1.0) |
| `sample_size` | int | 10000 | 近似検索のランダムサンプリング件数。サンプリングはコーパスがこの値の 2 倍を超えたときにのみ有効になる（0 = 全探索）。上限は、その比較に使う 32 ビットカウンタ内に 2 倍した値が収まる最大値 (0-2147483647) |
| `ivf_enabled` | bool | false | IVF（Inverted File）近似最近傍検索を有効にする |
| `ivf_nlist` | int | 256 | IVF 索引のボロノイセル（クラスタ）数（0 = 自動で sqrt(n)） (0-65536) |
| `ivf_nprobe` | int | 8 | クエリ時に探索するクラスタ数 (1-65536) |
| `ivf_train_threshold` | int | 10000 | IVF 索引を自動学習するまでに必要な最小ベクトル数。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `ivf_seal_threshold` | int | 100000 | 書き込みバッファをこのベクトル数に達した時点で確定する。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `adaptive_fusion` | bool | false | アイテムの成熟度に基づく適応的な fusion 重み計算を有効にする |
| `adaptive_min_alpha` | float | 0.2 | 共起が多い成熟したアイテムに対するベクトル重みの下限 (0.0-1.0) |
| `adaptive_max_alpha` | float | 0.9 | 共起が少ない新しいアイテムに対するベクトル重みの上限 (0.0-1.0) |
| `adaptive_maturity_threshold` | int | 50 | アイテムを成熟とみなす共起近傍数。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `index_type` | string | "flat" | ANN 索引の種類: hnsw、ivf、flat（全探索） (`hnsw` `ivf` `flat`) |
| `hnsw_m` | int | 16 | HNSW のノードあたり接続数。上限は、最下層で 2 倍になるリンク数を保持する 32 ビットカウンタに収まる最大値 (2-2147483647) |
| `hnsw_ef_construction` | int | 200 | HNSW の構築時探索幅。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `hnsw_ef_search` | int | 50 | HNSW のクエリ時探索幅。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `hnsw_max_elements` | int | 0 | HNSW の事前確保容量（0 = 動的に拡張）。値は起動時に確保され、上限はスナップショット読み込みが受け付ける索引の最大サイズ。これを超えて確保すると再読み込みできない索引になる (0-10000000) |
<!-- END GENERATED: options similarity -->

キーどうしの規則は次のとおりです。

- `default_top_k` は 0 より大きく、`max_top_k` は `default_top_k` 以上である必要があります。
- `adaptive_min_alpha` は `adaptive_max_alpha` 以下、`adaptive_maturity_threshold` は 0 より大きい必要があります。この 3 つは `adaptive_fusion` が true のとき、またはリクエストが `adaptive=on` を渡したときにだけ効きます。
- IVF 系のキーは索引が IVF のとき、つまり `index_type: ivf` または `ivf_enabled: true` のときにだけ検証されます。`ivf_nprobe` と `ivf_train_threshold` は 0 より大きく、`ivf_nlist` が `0` でない限り `ivf_nprobe` は `ivf_nlist` 以下である必要があります。
- HNSW 系のキーは `index_type: hnsw` のときにだけ検証されます。`hnsw_m` は 2 以上、2 つの `ef` はどちらも 0 より大きい必要があります。
- `ivf_enabled` は `index_type: ivf` の古い綴りです。`index_type` がまだ `flat` のときにだけ適用され、`index_type` に `hnsw` や `ivf` を設定した場合、IVF 系のキーは範囲検査されるもののこのフラグ自体は効きません。
- `fusion_alpha` と `fusion_beta` は独立した 2 つの数値であり、ひとつの配分を分け合う関係ではありません。合計が 1 になる必要はありません。

`hnsw_max_elements` は起動時に確保され、その上限はスナップショットローダーが受け付ける最大の索引です。上限を超えて確保すると、読み直せない索引ができます。

## `snapshot`

[persistence.md](./persistence.md) を参照してください。

<!-- BEGIN GENERATED: options snapshot -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | スナップショットディレクトリのパス |
| `default_filename` | string | "nvecd.nvec" | 引数なしの DUMP SAVE がスナップショットディレクトリ内に書き込むファイル名。クライアント指定のパスと同様に検証されるため、絶対パスやディレクトリを抜け出す名前は拒否される（空 = タイムスタンプ付きの名前にフォールバック）。.nvec か .dmp で終わる必要があり、そうでないと起動時に復旧候補として扱われない |
| `interval_sec` | int | 0 | スナップショット間隔（秒、0 = 無効） (0-86400) |
| `retain` | int | 3 | 保持する自動スナップショット数。手動スナップショットは削除されない（0 = すべてのファイルを残す） (0-100) |
| `mode` | string | "fork" | スナップショットの一貫性モード: fork（COW、非ブロッキング）または lock（グローバル書き込みロック、ブロッキング） (`fork` `lock`) |
<!-- END GENERATED: options snapshot -->

`interval_sec` と `retain` は負であってはならず、`mode` は 2 つの値のいずれかである必要があります。

`default_filename` はクライアントが渡したパスとまったく同じように検証されるため、絶対パスやディレクトリから逃げる名前は `dir` に対して解決されるのではなく拒否されます。値が空の場合はタイムスタンプ付きの名前にフォールバックします。`retain` が刈り取るのはスケジューラが書いたスナップショットだけで、明示的な `DUMP SAVE` が作ったファイルが削除されることはありません。

`mode: fork` はコピーオンライトのイメージ上でフォークした子プロセスが書き込み、書き込み側をブロックしませんが、メモリ使用量が一時的に跳ね上がります。`mode: lock` はその間書き込みをブロックし、完成したファイルを同期的に報告します。モードは `DUMP SAVE` の応答も変えます。`fork` では `OK DUMP_SAVE_STARTED`、`lock` では `OK DUMP_SAVED` です。

## `performance`

<!-- BEGIN GENERATED: options performance -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `thread_pool_size` | int | 8 | ワーカースレッドプールのサイズ（0 = 検出したハードウェアスレッド数に合わせる） (0-128) |
| `max_connections` | int | 1000 | 最大同時接続数 (1-100000) |
| `max_connections_per_ip` | int | 100 | IP アドレスごとの最大接続数（0 = 無制限） (0-100000) |
| `connection_timeout_sec` | int | 300 | アイドル接続のタイムアウト、および最初の完全なリクエストを受け取るまでの期限（秒） (1-3600) |
| `recv_buffer_size` | int | 4096 | TCP 受信バッファサイズ（バイト） (1024-1048576) |
| `send_buffer_size` | int | 65536 | TCP 送信バッファサイズ（バイト） (1024-16777216) |
| `max_query_length` | int | 1048576 | リクエスト 1 件あたりの最大バイト数 (1024-16777216) |
| `shutdown_timeout_ms` | int | 5000 | グレースフルシャットダウンのタイムアウト（ミリ秒） (100-60000) |
| `reactor_max_total_buffered_bytes` | int | 268435456 | TCP リアクタがバッファするバイト数のプロセス全体での上限 (1048576-1073741824) |
<!-- END GENERATED: options performance -->

`thread_pool_size` は負であってはならず、`max_connections` と `connection_timeout_sec` は 0 より大きい必要があります。

これらのキーは TCP 専用ではありません。HTTP サーバーも自身の上限をここから導きます。`thread_pool_size` がワーカー数、`max_connections` と `max_connections_per_ip` が受け入れ上限、`max_query_length` がリクエストボディの上限になり、待ち行列は `max_connections − thread_pool_size`（最低 1）です。`max_query_length` を超えるリクエストボディは両方の面で拒否されます。

## `api`

### `api.tcp`

<!-- BEGIN GENERATED: options api.tcp -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `bind` | string | "127.0.0.1" | TCP バインドアドレス（"0.0.0.0" は全インターフェースで待ち受ける） |
| `port` | int | 11017 | TCP ポート (1-65535) |
<!-- END GENERATED: options api.tcp -->

TCP リスナーは常に起動します。これを無効にするキーはありません。

### `api.http`

<!-- BEGIN GENERATED: options api.http -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `enable` | bool | false | HTTP サーバーを有効にする |
| `bind` | string | "127.0.0.1" | HTTP バインドアドレス（"0.0.0.0" は全インターフェースで待ち受ける） |
| `port` | int | 8080 | HTTP ポート (1-65535) |
| `enable_cors` | bool | false | CORS ヘッダを有効にする |
| `cors_allow_origin` | string | "" | CORS 有効時の Access-Control-Allow-Origin ヘッダの値 |
| `timeout_sec` | int | 5 | HTTP の読み書きタイムアウト（秒） (1-300) |
<!-- END GENERATED: options api.http -->

`port` の範囲検査は `enable` が true のときにだけ行われ、`timeout_sec` はどちらでも検査されます。`cors_allow_origin` が空の場合はヘッダを `null` として送るのではなく省くため、`enable_cors` だけではどのオリジンも許可されません。HTTP のバインドに失敗した場合はログに記録され、サーバーは TCP のみで動作を続けます。[http-api.md](./http-api.md) を参照してください。

### `api.unix_socket`

<!-- BEGIN GENERATED: options api.unix_socket -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `path` | string | "" | Unix ソケットのパス（空文字列 = 無効） |
<!-- END GENERATED: options api.unix_socket -->

Unix ソケットは TCP ポートと同じプロトコルを提供しますが、`network.allow_cidrs`、`performance.max_connections_per_ip`、レート制限を迂回します。ソケットファイルのファイルシステム権限がそのアクセス制御です。Unix ソケットのバインドに失敗した場合は警告としてログに記録され、サーバーは停止しません。

### `api.rate_limiting`

<!-- BEGIN GENERATED: options api.rate_limiting -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `enable` | bool | false | レート制限を有効にする |
| `capacity` | int | 100 | クライアントごとの最大トークン数（バースト幅） (1-10000) |
| `refill_rate` | int | 10 | クライアントごとに毎秒補充されるトークン数 (1-1000) |
| `max_clients` | int | 10000 | 追跡するクライアントの最大数（メモリ管理用） (10-100000) |
<!-- END GENERATED: options api.rate_limiting -->

3 つの数値キーが正であるかどうかは `enable` が true のときにだけ検査されます。レート制限はクライアントアドレスを鍵にするため、Unix ソケットの接続には適用されません。上限を超えた HTTP リクエストには `429` を返します。

## `network`

<!-- BEGIN GENERATED: options network -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `allow_cidrs` | list | [] | 接続を許可する CIDR のリスト（空 = すべて拒否） |
<!-- END GENERATED: options network -->

このリストはフェイルクローズです。空のリストはすべてのアドレスを拒否するため、これを省いた構成は TCP も HTTP もいっさい受け付けません。CIDR として解析できないエントリはログに記録して読み飛ばし、残りのエントリはそのまま有効です。この検査は TCP リスナーとすべての HTTP ルートに適用され、Unix ソケットには適用されません。

```yaml
network:
  allow_cidrs:
    - "127.0.0.1/32"
    - "10.0.0.0/8"
```

## `logging`

<!-- BEGIN GENERATED: options logging -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `level` | string | "info" | ログレベル (`trace` `debug` `info` `warn` `error`) |
| `json` | bool | true | JSON 形式で出力する |
| `file` | string | "" | ログファイルのパス（空文字列 = 標準出力、パス指定 = ファイル出力） |
<!-- END GENERATED: options logging -->

`level` と `json` はどちらも実行時に変更できます。`file` はできません。ハンドルの開き直しは再起動相当の変更だからです。

## `cache`

クエリキャッシュの設定です。[caching.md](./caching.md) を参照してください。

<!-- BEGIN GENERATED: options cache -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `enabled` | bool | true | キャッシュの有効/無効 |
| `max_memory_mb` | int | 32 | キャッシュの最大メモリ量（MB）。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-2147483647) |
| `min_query_cost_ms` | float | 10.0 | キャッシュ対象とする最小クエリコスト（ms） (>= 0.0) |
| `ttl_seconds` | int | 3600 | キャッシュエントリの TTL（秒、0 = TTL なし）。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (0-2147483647) |
| `compression_enabled` | bool | true | LZ4 圧縮を有効にする |
| `eviction_batch_size` | int | 10 | 一度に退避するエントリ数。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-2147483647) |
<!-- END GENERATED: options cache -->

`enabled` が true のとき `max_memory_mb` は 0 より大きく、`ttl_seconds` は負でなく、`min_query_cost_ms` は負でなく、`eviction_batch_size` は 0 より大きい必要があります。

`enabled`、`min_query_cost_ms`、`ttl_seconds` は実行時に変更できます。残りの 3 つは確保量か保存形式を決めるため、起動時に固定されます。

`min_query_cost_ms` は、小さなコーパスでヒット率が低く見える理由でもあります。閾値より速く終わる検索はそもそも保存されないため、ヒットするものが存在しません。

## `security`

<!-- BEGIN GENERATED: options security -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `requirepass` | string | "" | write/admin コマンドに必要なパスワード（空 = 認証なし） |
<!-- END GENERATED: options security -->

`requirepass` を設定すると両方の面が同時に閉じます。TCP 接続は書き込みや管理のコマンドの前に `AUTH` を発行する必要があり、対象の HTTP ルートは `Authorization: Bearer <password>` またはパスワードが一致する `Basic` 資格情報を要求します。読み取りコマンドとヘルスチェックは両方の面で開放されたままです。この値は `CONFIG SHOW` と実行時変数 `security.requirepass` で `***` に伏せられます。

パスワードは平文の YAML ファイルに置かれるため、それを守るのはファイルの権限です。

## `wal`

先行書き込みログ（WAL）の設定です。[persistence.md](./persistence.md) を参照してください。

<!-- BEGIN GENERATED: options wal -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `enabled` | bool | false | WAL によるクラッシュ復旧を有効にする |
| `dir` | string | "/var/lib/nvecd/wal" | WAL セグメントファイルを置くディレクトリ |
| `max_file_size` | int | 67108864 | WAL ファイル 1 本あたりの最大サイズ（バイト）。上限は設定リーダーが扱える最大の整数であり、チューニング上の制限ではない (1-9223372036854775807) |
| `sync_on_write` | bool | false | 追記ごとに fsync する（高い耐久性、低いスループット）。false のときは sync_interval_ms がバッチ間隔を決める |
| `sync_interval_ms` | int | 100 | sync_on_write が false のときのバッチ fsync 間隔（ミリ秒、0 = 追記ごとに fsync）。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (0-4294967295) |
| `include_vectors` | bool | true | VECSET の WAL レコードにベクトル本体を永続化する。無効化してよいのは、ベクトルの耐久性をスナップショットで満たせる場合だけ |
<!-- END GENERATED: options wal -->

`enabled` が true のとき `dir` は空であってはなりません。`sync_on_write` が true の間は `sync_interval_ms` は効きません。ここが永続性と書き込みスループットの取引になります。

`include_vectors` は復旧に直接影響します。false にすると `VECSET` レコードはベクトル本体を運ばず、そもそも書かれないため、再起動後に復元されるベクトルは最後のスナップショットまでになります。スナップショットに含まれないベクトルを参照するログ中の `VECDEL` や `METASET` は読み飛ばされ、その回数が数えられ、`INFO` の `wal_replay_records_skipped` として報告されます。これを切るのは、ベクトルデータの永続性の境界をスナップショットに置くという判断であり、結果として生じる欠落で復旧を失敗させるのではなくサーバーを起動可能に保ちます。

## 最小限の設定

起動してローカルホストから到達できるだけの最小構成です。

```yaml
vectors:
  default_dimension: 384

api:
  tcp:
    bind: "127.0.0.1"
    port: 11017

network:
  allow_cidrs:
    - "127.0.0.1/32"
```

これ以外はすべて既定値になります。`network` セクションがないとサーバーは起動しますが、すべての接続を拒否します。

## 堅牢寄りの設定

認証を有効にし、両方の面をプライベートなインターフェースにバインドし、レート制限を入れ、WAL と定期スナップショットを有効にし、グラフに上限を設けた構成です。

```yaml
events:
  ctx_buffer_size: 100
  max_contexts: 500000
  max_neighbors_per_item: 200
  min_support: 0.5
  decay_interval_sec: 3600
  decay_alpha: 0.99

vectors:
  default_dimension: 768
  distance_metric: "cosine"

similarity:
  default_top_k: 20
  max_top_k: 200
  index_type: "hnsw"
  hnsw_m: 32
  hnsw_ef_construction: 400
  hnsw_ef_search: 100
  adaptive_fusion: true
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 50

snapshot:
  dir: "/var/lib/nvecd/snapshots"
  default_filename: "nvecd.nvec"
  interval_sec: 900
  retain: 12
  mode: "fork"

wal:
  enabled: true
  dir: "/var/lib/nvecd/wal"
  max_file_size: 134217728
  sync_on_write: false
  sync_interval_ms: 100
  include_vectors: true

performance:
  thread_pool_size: 0
  max_connections: 2000
  max_connections_per_ip: 50
  connection_timeout_sec: 120
  max_query_length: 1048576

api:
  tcp:
    bind: "10.0.1.5"
    port: 11017
  http:
    enable: true
    bind: "10.0.1.5"
    port: 8080
    timeout_sec: 10
  rate_limiting:
    enable: true
    capacity: 200
    refill_rate: 50
    max_clients: 20000

network:
  allow_cidrs:
    - "10.0.1.0/24"

security:
  requirepass: "replace-this"

cache:
  enabled: true
  max_memory_mb: 512
  min_query_cost_ms: 5.0
  ttl_seconds: 600

logging:
  level: "info"
  json: true
  file: "/var/log/nvecd/nvecd.log"
```

再起動する前に検証します。

```bash
$ nvecd -t -c /etc/nvecd/config.yaml
Configuration file is valid

Configuration summary:
  Events:
    ctx_buffer_size: 100
...
```

## 実行中の設定を確認する

TCP の `CONFIG SHOW` は稼働中の設定を YAML で表示し、セクションに絞ることもできます。

```text
> CONFIG SHOW cache
+OK
compression_enabled: true
enabled: true
eviction_batch_size: 10
max_memory_mb: 32
min_query_cost_ms: 10.0
ttl_seconds: 3600
END
```

`requirepass` は `***` として現れ、その隣にファイルにもスキーマにも対応するものがない派生値の `auth_enabled` フラグが並びます。`CONFIG HELP <path>` は 1 つのキーの型、既定値、許容値、説明を表示します。読み出し元はローダーが検証に使うのと同じスキーマです。

HTTP の `GET /config` が返すのはずっと狭い要約で、バインドアドレス、ポート、パスワード、CIDR リストそのものはいずれも伏せられます。したがって運用上の内省は TCP 面の役割です。どちらも [protocol.md](./protocol.md) と [http-api.md](./http-api.md) で説明しています。

`CONFIG VERIFY <file>` は別のファイルを適用せずに検証します。パスは稼働中の設定ファイルがあったディレクトリの内側、設定ファイルなしで起動したサーバーでは `snapshot.dir` の内側で解決され、そのルートの外側にあるものは拒否されます。

## 実行中に設定を変更する

稼働中のサーバーで変更できる変数は 5 つです。

| 変数 | 変更の効果 |
|---|---|
| `logging.level` | 次のログレコードから効く |
| `logging.json` | レコードの形式を切り替える |
| `cache.enabled` | クエリキャッシュを有効・無効にする |
| `cache.min_query_cost_ms` | どの検索をキャッシュする価値があるかを変える |
| `cache.ttl_seconds` | エントリの寿命を変える |

このページのその他のキーは `GET` と `SHOW VARIABLES` で読めますが、書き込みは `Variable '<name>' is immutable (requires restart)` で拒否されます。

```text
> SET cache.ttl_seconds 600
+OK

> SET logging.level debug
+OK

> GET cache.enabled
$4
true
```

真偽値は `true`／`false`、`on`／`off`、`1`／`0`、`yes`／`no` を受け付け、`true` または `false` の正規形で保存されるため、別名で書いた値は正規形で読み出されます。`SHOW VARIABLES LIKE <prefix>%` は名前と値と可変性を一覧します。入力では `performance.` の接頭辞を `perf.` と綴ることもできますが、内省は常に `performance.` で報告します。

実行時の変更はファイルに書き戻されないため、再起動すると設定ファイルの値に戻ります。`CACHE ENABLE` と `CACHE DISABLE` は `cache.enabled` を設定する短縮形で、HTTP の `/cache/enable` と `/cache/disable` も同じことをします。
