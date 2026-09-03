# 設定ガイド

このガイドでは、Nvecd で利用可能なすべての設定オプションについて説明します。

以下のオプション表と `examples/config.yaml` は `src/config/config-schema.json` から生成されます。ここに書かれている型・既定値・範囲の権威はそのスキーマです。変更するときはスキーマを編集して `python3 support/generate_config_docs.py` を実行してください。表を直接編集しないでください。

## 設定ファイル

Nvecd は設定に YAML 形式を使用します。サンプル設定ファイルは `examples/config.yaml` にあります。

## 基本的な使い方

```bash
# 設定ファイルを指定して nvecd を起動
nvecd -c /path/to/config.yaml
```

---

## 設定セクション

### イベントストア設定

イベント追跡と共起インデックスの動作を制御します。

```yaml
events:
  ctx_buffer_size: 50          # コンテキストごとのリングバッファサイズ
  max_contexts: 0              # 保持するアクティブコンテキスト数（0 = 無制限）
  max_neighbors_per_item: 0    # アイテムごとの共起エッジ数（0 = 無制限）
  min_support: 0.0             # このスコア未満のエッジを削除（0 = 無効）
  decay_interval_sec: 3600     # 減衰間隔（秒）
  decay_alpha: 0.99            # 減衰係数（0.0 - 1.0）
  dedup_window_sec: 60         # 重複排除の時間窓（秒）
  dedup_cache_size: 10000      # 重複排除キャッシュサイズ（LRU）
  temporal_cooccurrence: false # 共起更新に時間減衰を適用
  temporal_half_life_sec: 86400
  negative_signals: false      # DEL イベントを負のフィードバックとして扱う
  negative_weight: 0.5
```

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

**重複排除の動作:**

同じ `(ctx, id, score)` の組み合わせが `dedup_window_sec` 内に受信された場合、重複として検出されます。これにより以下を防ぎます：
- リトライバグによる統計の肥大化
- ネットワーク再送信による重複エントリの作成
- クライアント側のバグによる共起データへの影響

統計の追跡：
- `total_events`: 受信したEVENTコマンドの総数（重複を含む）
- `deduped_events`: 無視された重複イベントの数
- `stored_events`: リングバッファに実際に格納されたイベント数（total_events - deduped_events）

---

### ベクトルストア設定

ベクトルストレージと検索動作を制御します。

```yaml
vectors:
  default_dimension: 768       # デフォルトベクトル次元数
  distance_metric: "cosine"    # 距離メトリック: cosine, dot, l2
```

<!-- BEGIN GENERATED: options vectors -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `default_dimension` | int | 768 | 既定のベクトル次元数（ベクトルごとの特徴量数） (1-4096) |
| `distance_metric` | string | "cosine" | 類似検索に用いる距離メトリック (`cosine` `dot` `l2`) |
<!-- END GENERATED: options vectors -->

一般的な埋め込み次元数は 768（BERT）、1536（OpenAI）、384（MiniLM）です。

---

### 類似検索設定

類似検索とフュージョンアルゴリズムのパラメータを制御します。

```yaml
similarity:
  default_top_k: 100           # デフォルト結果数
  max_top_k: 1000              # 最大許容 top_k
  fusion_alpha: 0.6            # ベクトル類似度の重み（フュージョンモード）
  fusion_beta: 0.4             # 共起の重み（フュージョンモード）
  adaptive_fusion: false       # アイテム成熟度に応じてベクトル重みを調整
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 50
```

<!-- BEGIN GENERATED: options similarity -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `default_top_k` | int | 100 | 類似検索が返す既定の結果件数 (1-10000) |
| `max_top_k` | int | 1000 | 類似検索で許可する結果件数の上限 (1-10000) |
| `fusion_alpha` | float | 0.6 | fusion モードにおけるベクトル類似度成分の重み (0.0-1.0) |
| `fusion_beta` | float | 0.4 | fusion モードにおける共起成分の重み (0.0-1.0) |
| `sample_size` | int | 10000 | 近似検索のランダムサンプリング件数。サンプリングはコーパスがこの値の 2 倍を超えたときにのみ有効になる（0 = 全探索）。上限は、その比較に使う 32 ビットカウンタ内に 2 倍した値が収まる最大値 (0-2147483647) |
| `ivf_enabled` | bool | false | IVF（Inverted File）近似最近傍検索を有効にする |
| `ivf_nlist` | int | 256 | IVF インデックスのボロノイセル（クラスタ）数（0 = 自動で sqrt(n)） (0-65536) |
| `ivf_nprobe` | int | 8 | クエリ時に探索するクラスタ数 (1-65536) |
| `ivf_train_threshold` | int | 10000 | IVF インデックスを自動学習するまでに必要な最小ベクトル数。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `ivf_seal_threshold` | int | 100000 | 書き込みバッファをこのベクトル数に達した時点で確定する。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `adaptive_fusion` | bool | false | アイテムの成熟度に基づく適応的な fusion 重み計算を有効にする |
| `adaptive_min_alpha` | float | 0.2 | 共起が多い成熟したアイテムに対するベクトル重みの下限 (0.0-1.0) |
| `adaptive_max_alpha` | float | 0.9 | 共起が少ない新しいアイテムに対するベクトル重みの上限 (0.0-1.0) |
| `adaptive_maturity_threshold` | int | 50 | アイテムを成熟とみなす共起近傍数。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `index_type` | string | "flat" | ANN インデックスの種類: hnsw、ivf、flat（全探索） (`hnsw` `ivf` `flat`) |
| `hnsw_m` | int | 16 | HNSW のノードあたり接続数。上限は、最下層で 2 倍になるリンク数を保持する 32 ビットカウンタに収まる最大値 (2-2147483647) |
| `hnsw_ef_construction` | int | 200 | HNSW の構築時探索幅。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `hnsw_ef_search` | int | 50 | HNSW のクエリ時探索幅。上限は読み込み先の型の範囲であり、チューニング上の制限ではない (1-4294967295) |
| `hnsw_max_elements` | int | 0 | HNSW の事前確保容量（0 = 動的に拡張）。値は起動時に確保され、上限はスナップショット読み込みが受け付けるインデックスの最大サイズ。これを超えて確保すると再読み込みできないインデックスになる (0-10000000) |
<!-- END GENERATED: options similarity -->

**注意**: `fusion_beta` を高くすると、イベントベースのシグナルがより重視されます。

#### ANN インデックスの選択とチューニング

`similarity.index_type` でアクティブなベクトルインデックスを選択します。値は既定の
`flat`、`hnsw`、`ivf` です。サーバーが利用するのはこのいずれか 1 つだけであり、
TieredVectorStore、MergeScheduler、ScalarQuantizer はランタイム機能としては提供していません。

```yaml
similarity:
  index_type: hnsw
  hnsw_m: 16
  hnsw_ef_construction: 200
  hnsw_ef_search: 50
  hnsw_max_elements: 0       # 0 = 動的に拡張
```

`hnsw_m` と `hnsw_ef_construction` を上げるとメモリと投入時間を、`hnsw_ef_search` と `ivf_nprobe` を上げるとクエリ遅延を消費します。いずれも対価は recall です。型・既定値・許容範囲は上の表にあります。

`ivf_enabled` は互換性のため残されています。新しい設定では `index_type: ivf` を使用してください。

##### チューニングパラメータで何が得られるか

近似インデックスは常にトレードオフです。答えの質を落とせばどれだけでも速くできるので、速度だけの数値には意味がありません。以下の表は各設定での recall と速度を並べ、その取引を明示しています。

計測条件は、200個の潜在重心の周辺に分布させた5万ベクトル（実際の埋め込みが持つ形状）に対し、同一ベクトル集合の全探索を正解とし、`top_k=10`、1点あたり200クエリ。再現手順:

```bash
cmake --build build --target ann_recall_benchmark
./build/bin/ann_recall_benchmark --gtest_also_run_disabled_tests
```

ハードウェア・SIMD構成・ビルドフラグは[計測環境](benchmarks.md#計測環境)と同じで、
Apple M5 Max（arm64）の NEON、Release（`-O3 -march=native`）、Apple Clang です。
コーパスは固定シードから生成するため、同じビルドで再実行すれば同じ recall が得られます。

**以下の表はインデックス単体を測ったものです。** 「全探索比」は、インデックスへの200回の
呼び出しの p50 を、同一ベクトル集合に対して同じ距離計算で行う全探索の p50 で割った値です。
ワイヤ越しに届くクエリはこれ以上の仕事をします。エンジンを通るため、メタデータの絞り込み・
結果の組み立て・キャッシュ参照が毎クエリ加わり、その分はインデックスが速くなっても縮みません。
したがって、利用者が端から端まで観測する比率は下表の値とは一致しません。このデータで既定の
`ivf_nprobe: 8`、128次元の場合、インデックス単体が7.5倍のところ、エンジン経路では
エンジン自身の総当たり経路に対して9.3倍を計測しています（この比較では分母側にも同じ
エンジンのオーバーヘッドが乗っています）。下表は取引の形として読み、そのままスループットの
予測値としては使わないでください。

比率は他の負荷が乗った共有マシンで取得しており、分子と分母は同一 run の内側で測っているため
条件は揃っています。ベンチマークは p99 も出力しますが、ここには載せていません。

HNSW（`hnsw_m: 16`、`hnsw_ef_construction: 200`）:

| `hnsw_ef_search` | recall@10（128次元） | 全探索比 | recall@10（768次元） | 全探索比 |
|---|---|---|---|---|
| 10 | 0.996 | 21倍 | 0.985 | 55倍 |
| 16 | 1.000 | 17倍 | 0.997 | 48倍 |
| 32 | 1.000 | 13倍 | 1.000 | 38倍 |
| 64 | 1.000 | 8倍 | 1.000 | 30倍 |

IVF（`ivf_nlist: 256`）:

| `ivf_nprobe` | recall@10（128次元） | 全探索比 | recall@10（768次元） | 全探索比 |
|---|---|---|---|---|
| 1 | 0.962 | 32倍 | 0.981 | 42倍 |
| 2 | 0.996 | 21倍 | 0.998 | 27倍 |
| 4 | 1.000 | 15倍 | 1.000 | 14倍 |
| 8 | 1.000 | 8倍 | 1.000 | 8倍 |

既定値（`hnsw_ef_search: 50`、`ivf_nprobe: 8`）は、このデータでは recall が既に 1.0 に達した先に位置します。つまりスループット重視ではなく安全側に振った設定です。recall の最後のわずかな差よりクエリ遅延が重要なら、まずここを下げるのが有効です。

::: warning recall はベクトルの分布に依存します
上記はクラスタ構造を持つデータでの数値です。空間に均等に散らばったベクトル、つまり方向がばらばらでまとまりの無いデータは、あらゆる近似インデックスにとって最悪ケースであり、同じ設定でも recall は大幅に下がります。HNSW は `ef_search: 64` で 0.39 にとどまり、0.93 を超えるには `512` が必要で、その時点では全件走査より数倍遅くなります。

埋め込みに構造が乏しい場合、近似インデックスは役に立ちません。`index_type` を `flat` から変更する前に、上記のベンチマークを自分のベクトルに対して実行して確認してください。
:::

---

### スナップショット永続化設定

スナップショットの保存/読み込み動作を制御します。

```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"  # スナップショットディレクトリ
  default_filename: "nvecd.snapshot" # デフォルトファイル名
  interval_sec: 0                   # 自動スナップショット間隔（0 = 無効）
  retain: 3                         # 保持するスナップショット数
  mode: "fork"                     # スナップショットモード: "fork"（COW）または "lock"
```

<!-- BEGIN GENERATED: options snapshot -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | スナップショットディレクトリのパス |
| `default_filename` | string | "nvecd.snapshot" | 引数なしの DUMP SAVE がスナップショットディレクトリ内に書き込むファイル名。クライアント指定のパスと同様に検証されるため、絶対パスやディレクトリを抜け出す名前は拒否される（空 = タイムスタンプ付きの名前にフォールバック） |
| `interval_sec` | int | 0 | スナップショット間隔（秒、0 = 無効） (0-86400) |
| `retain` | int | 3 | 保持する自動スナップショット数。手動スナップショットは削除されない（0 = すべてのファイルを残す） (0-100) |
| `mode` | string | "fork" | スナップショットの一貫性モード: fork（COW、非ブロッキング）または lock（グローバル書き込みロック、ブロッキング） (`fork` `lock`) |
<!-- END GENERATED: options snapshot -->

`snapshot.dir` は存在しない場合に作成されます。

**自動スナップショットのファイル名**: `auto_YYYYMMDD_HHMMSS.nvec`

**セキュリティ要件**: POSIX 環境では `snapshot.dir` は nvecd を実行するユーザーが所有し、
グループまたは他ユーザーから書き込み可能であってはなりません。スナップショットは
`0600` で作成されるため、通常はサービス専用の `0700` ディレクトリを使用してください。

---

### Write-Ahead Log（WAL）設定

WAL は直近のスナップショット以降の書き込みを再起動時に再生します。
`include_vectors: false` はログを小さくしますが、VECSET の内容は次のスナップショットまでクラッシュ復旧できません。

```yaml
wal:
  enabled: false
  dir: "/var/lib/nvecd/wal"
  max_file_size: 67108864
  sync_on_write: false
  sync_interval_ms: 100
  include_vectors: true
```

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

`wal.enabled` が true のとき `wal.dir` は空にできません。

耐久性が無効になる設定は存在しません。スキーマ上正当な組み合わせであれば、受理されたレコードは必ず有限時間内にディスクへ到達します。`sync_on_write: true` は追記ごとに fsync します。`sync_on_write: false` のときは `sync_interval_ms` がバッチ間隔になり、`sync_interval_ms: 0` も追記ごとの fsync になります。間隔が 0 だとバッチを書き出すものが無くなってしまうためです。

セグメントは 6 桁固定の番号を持つ `wal-NNNNNN.log` という名前になるため、セグメント
番号空間は `wal-999999.log` で終わります。これを超えるローテーションは、より桁数の
多い名前を書き出す代わりに失敗します。そのような名前は復旧と切り詰めのどちらからも
認識されないためです。この上限に達するのは、一度もスナップショットを取らずに 100 万回
ローテーションした場合だけです。スナップショットのチェックポイントはログを切り詰め、
番号を回収します。

同じ所有者・非共有書込みの要件が `wal.dir` にも適用されます。WAL のディレクトリと
セグメントはそれぞれ `0700`、`0600` で作成されます。

---

### パフォーマンス設定

サーバーパフォーマンスとリソース制限を制御します。

```yaml
performance:
  thread_pool_size: 8          # ワーカースレッドプールサイズ
  max_connections: 1000        # 最大同時接続数
  connection_timeout_sec: 300  # 接続タイムアウト（秒）
  reactor_max_total_buffered_bytes: 268435456  # 全接続の保留データ上限（256 MiB）
```

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
| `reactor_max_total_buffered_bytes` | int | 268435456 | TCP リアクターがバッファするバイト数のプロセス全体での上限 (1048576-1073741824) |
<!-- END GENERATED: options performance -->

`thread_pool_size` は CPU コア数に、`max_connections` はプロセスのファイルディスクリプタ上限と利用可能メモリに合わせて設定してください。

---

### API サーバー設定

TCP および HTTP API サーバーの設定を制御します。

#### TCP API（常に有効）

```yaml
api:
  tcp:
    bind: "127.0.0.1"          # TCP バインドアドレス
    port: 11017                # TCP ポート
```

<!-- BEGIN GENERATED: options api.tcp -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `bind` | string | "127.0.0.1" | TCP バインドアドレス（"0.0.0.0" は全インターフェースで待ち受ける） |
| `port` | int | 11017 | TCP ポート (1-65535) |
<!-- END GENERATED: options api.tcp -->

`0.0.0.0` へのバインドは到達可能な全ネットワークにサーバーを公開します。`network.allow_cidrs` と `security.requirepass` を併用してください。

#### HTTP API（オプション）

```yaml
api:
  http:
    enable: false              # HTTP/JSON API を有効化
    bind: "127.0.0.1"          # HTTP バインドアドレス
    port: 8080                 # HTTP ポート
    enable_cors: false         # CORS ヘッダーを有効化
    cors_allow_origin: ""      # 許可されたオリジン
```

<!-- BEGIN GENERATED: options api.http -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `enable` | bool | false | HTTP サーバーを有効にする |
| `bind` | string | "127.0.0.1" | HTTP バインドアドレス（"0.0.0.0" は全インターフェースで待ち受ける） |
| `port` | int | 8080 | HTTP ポート (1-65535) |
| `enable_cors` | bool | false | CORS ヘッダーを有効にする |
| `cors_allow_origin` | string | "" | CORS 有効時の Access-Control-Allow-Origin ヘッダーの値 |
| `timeout_sec` | int | 5 | HTTP の読み書きタイムアウト（秒） (1-300) |
<!-- END GENERATED: options api.http -->

#### Unix ドメインソケット（オプション）

```yaml
api:
  unix_socket:
    path: ""                     # Unix ソケットパス（空 = 無効）
```

<!-- BEGIN GENERATED: options api.unix_socket -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `path` | string | "" | Unix ソケットのパス（空文字列 = 無効） |
<!-- END GENERATED: options api.unix_socket -->

**注意**: Unix ドメインソケットは低遅延のローカル接続を提供します。TCP/IP のオーバーヘッドをバイパスし、同一ホスト上のサービス間通信に最適です。

#### レート制限（オプション）

```yaml
api:
  rate_limiting:
    enable: false              # レート制限を有効化
    capacity: 100              # 最大バーストトークン
    refill_rate: 10            # 秒あたりのトークン
    max_clients: 10000         # 追跡する最大クライアント数
```

<!-- BEGIN GENERATED: options api.rate_limiting -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `enable` | bool | false | レート制限を有効にする |
| `capacity` | int | 100 | クライアントごとの最大トークン数（バースト幅） (1-10000) |
| `refill_rate` | int | 10 | クライアントごとに毎秒補充されるトークン数 (1-1000) |
| `max_clients` | int | 10000 | 追跡するクライアントの最大数（メモリ管理用） (10-100000) |
<!-- END GENERATED: options api.rate_limiting -->

---

### ネットワークセキュリティ設定

IP アドレスアクセス制御（CIDR ベース）を制御します。

```yaml
network:
  allow_cidrs:
    - "127.0.0.1/32"           # ローカルホストのみ（推奨）
    # - "192.168.1.0/24"       # 例: ローカルネットワーク
    # - "0.0.0.0/0"            # 警告: すべて許可（非推奨）
```

<!-- BEGIN GENERATED: options network -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `allow_cidrs` | list | [] | 接続を許可する CIDR のリスト（空 = すべて拒否） |
<!-- END GENERATED: options network -->

**セキュリティ注意**: `allow_cidrs` はフェイルクローズです。空または未指定のリストは**すべての接続を拒否**するため、許可する IP 範囲を明示的に設定する必要があります。唯一の例外は設定ファイルを一切指定せずに nvecd を起動した場合で、この経路ではアクセスが `127.0.0.1/32` に制限されます。

---

### ログ設定

ログ出力形式と出力先を制御します。

```yaml
logging:
  level: "info"                # ログレベル
  json: true                   # JSON 形式出力
  file: ""                     # ログファイルパス（空 = stdout）
```

<!-- BEGIN GENERATED: options logging -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `level` | string | "info" | ログレベル (`trace` `debug` `info` `warn` `error`) |
| `json` | bool | true | JSON 形式で出力する |
| `file` | string | "" | ログファイルのパス（空文字列 = 標準出力、パス指定 = ファイル出力） |
<!-- END GENERATED: options logging -->

---

### セキュリティ設定

書き込みおよび管理コマンドの認証を制御します。

```yaml
security:
  requirepass: ""                # 必須パスワード（空 = 認証なし）
```

<!-- BEGIN GENERATED: options security -->
| オプション | 型 | 既定値 | 説明 |
|--------|------|---------|-------------|
| `requirepass` | string | "" | write/admin コマンドに必要なパスワード（空 = 認証なし） |
<!-- END GENERATED: options security -->

`requirepass` が設定されている場合、クライアントは write または admin コマンドを実行する前に `AUTH <password>` で認証する必要があります。分類は次のとおりです。

- **write**: `EVENT`、`VECSET`、`VECDEL`、`METASET`、`SET`、`CACHE CLEAR`、`CACHE ENABLE`、`CACHE DISABLE`
- **admin**: `DUMP SAVE`、`DUMP LOAD`、`DUMP VERIFY`、`DUMP INFO`、`DUMP STATUS`、`CONFIG VERIFY`
- **read**（認証不要）: `SIM`、`SIMV`、`INFO`、`CONFIG SHOW`、`CONFIG HELP`、`CACHE STATS`、`GET`、`SHOW VARIABLES`、`DEBUG ON`、`DEBUG OFF`

HTTP では `Authorization: Bearer <password>` と `Authorization: Basic`（ユーザー名は無視されます）が同じコマンド集合を認証します。ゲート対象のエンドポイントは、ヘッダーが無いか誤っている場合に `401` を返します。

---

### クエリ結果キャッシュ設定（オプション）

```yaml
cache:
  enabled: true                # クエリ結果キャッシュを有効化
  max_memory_mb: 32            # 最大キャッシュメモリ（MB）
  min_query_cost_ms: 10.0      # キャッシュする最小クエリコスト（ms）
  ttl_seconds: 3600            # キャッシュエントリ TTL（秒）
  compression_enabled: true    # LZ4 圧縮を有効化
  eviction_batch_size: 10      # 削除バッチサイズ
```

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

---

## 最小設定例

```yaml
# ローカルテスト用の最小設定
events:
  ctx_buffer_size: 50

vectors:
  default_dimension: 768

api:
  tcp:
    bind: "127.0.0.1"
    port: 11017

network:
  allow_cidrs:
    - "127.0.0.1/32"

logging:
  level: "info"
  json: true
```

---

## 本番環境設定例

```yaml
# セキュリティ強化された本番設定
events:
  ctx_buffer_size: 100
  decay_interval_sec: 7200     # 2時間
  decay_alpha: 0.95

vectors:
  default_dimension: 768

similarity:
  max_top_k: 500
  fusion_alpha: 0.7
  fusion_beta: 0.3

snapshot:
  dir: "/var/lib/nvecd/snapshots"
  interval_sec: 14400          # 4時間
  retain: 5

performance:
  thread_pool_size: 16         # 16コアサーバー
  max_connections: 5000
  connection_timeout_sec: 600

api:
  tcp:
    bind: "0.0.0.0"            # 全インターフェース（allow_cidrs でセキュリティ確保）
    port: 11017

network:
  allow_cidrs:
    - "10.0.0.0/8"             # プライベートネットワークのみ
    - "172.16.0.0/12"

logging:
  level: "warn"
  json: true
  file: "/var/log/nvecd/nvecd.log"
```

---

## 設定の検証

`CONFIG VERIFY` コマンドで設定ファイルの構文をチェックできます：

```bash
# サーバーに接続
nc localhost 11017

# 設定を検証
CONFIG VERIFY
```

または `CONFIG SHOW` で現在の設定を表示：

```bash
CONFIG SHOW
```

---

## 次のステップ

- 利用可能なコマンドについては [プロトコルリファレンス](protocol.md) を参照
- 永続化の詳細については [スナップショット管理](snapshot.md) を参照
- デプロイ手順については [インストールガイド](installation.md) を参照
