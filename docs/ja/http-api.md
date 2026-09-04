# HTTP API

HTTP サーバーは [TCP プロトコル](./protocol.md)と同じ操作を JSON のルートとして公開し、加えてヘルスチェックと Prometheus 用のスクレイプエンドポイントを提供します。既定では無効で、`api.http.enable` で有効にします。

## サーバーを有効にする

```yaml
api:
  http:
    enable: true
    bind: "127.0.0.1"
    port: 8080
    timeout_sec: 5
network:
  allow_cidrs:
    - "127.0.0.1/32"
```

リスナーはルーティングの前に `network.allow_cidrs` を適用するため、リストの外側のアドレスにはルートを問わず `403` を返します。空のリストはすべてのアドレスを拒否します。リクエストボディは `performance.max_query_length` で、同時接続は `performance.max_connections` と `performance.max_connections_per_ip` で上限が決まり、`api.rate_limiting.enable` を設定した場合はクライアントアドレスごとにトークンバケットで制限されます。

リクエストボディはすべて JSON です。サーバーは `Content-Type` ヘッダを要求せず、`/metrics` を除くすべての JSON 応答を `application/json` として返します。

## ルート一覧

| メソッド | パス | 対応する TCP コマンド | 権限 |
|---|---|---|---|
| `POST` | `/event` | `EVENT` | 書き込み |
| `POST` | `/vecset` | `VECSET` | 書き込み |
| `DELETE` | `/vecset` | `VECDEL` | 書き込み |
| `POST` | `/metaset` | `METASET` | 書き込み |
| `POST` | `/sim` | `SIM` | 読み取り |
| `POST` | `/simv` | `SIMV` | 読み取り |
| `GET` | `/info` | `INFO` | 読み取り |
| `GET` | `/config` | `CONFIG SHOW` | 読み取り |
| `GET` | `/cache/stats` | `CACHE STATS` | 読み取り |
| `POST` | `/cache/clear` | `CACHE CLEAR` | 書き込み |
| `POST` | `/cache/enable` | `CACHE ENABLE` | 書き込み |
| `POST` | `/cache/disable` | `CACHE DISABLE` | 書き込み |
| `POST` | `/dump/save` | `DUMP SAVE` | 管理 |
| `POST` | `/dump/load` | `DUMP LOAD` | 管理 |
| `POST` | `/dump/verify` | `DUMP VERIFY` | 管理 |
| `POST` | `/dump/info` | `DUMP INFO` | 管理 |
| `GET` | `/dump/status` | `DUMP STATUS` | 管理 |
| `POST` | `/debug/on` | `DEBUG ON` | 読み取り |
| `POST` | `/debug/off` | `DEBUG OFF` | 読み取り |
| `GET` | `/health` | — | なし |
| `GET` | `/health/live` | — | なし |
| `GET` | `/health/ready` | — | なし |
| `GET` | `/health/detail` | — | なし |
| `GET` | `/metrics` | — | なし |

この表にないパス、および登録済みのパスに誤ったメソッドで到達した場合は `404` を返します。

各ルートは自分がどの TCP コマンドの HTTP 形であるかを宣言し、その 1 つの宣言が権限とリクエストの計上先カウンタの両方を決めます。そのため 2 つの面がどちらの点でも食い違うことはありません。

## 認証

`security.requirepass` が空の場合、すべてのルートが開放されています。設定した場合、書き込みルートと管理ルートはリクエストごとに資格情報を要求し、読み取りルートとヘルスチェックは開放されたままです。これは TCP の権限区分と一致します。

対象となるのは `POST /event`、`POST /vecset`、`DELETE /vecset`、`POST /metaset`、`POST /cache/clear`、`POST /cache/enable`、`POST /cache/disable`、`POST /dump/save`、`POST /dump/load`、`POST /dump/verify`、`POST /dump/info`、`GET /dump/status` です。

資格情報の形は 2 つ受け付けます。

```bash
curl -X POST http://127.0.0.1:8080/vecset \
  -H 'Authorization: Bearer s3cret' \
  -d '{"id":"item1","vector":[0.1,0.2,0.3,0.4]}'

curl -X POST http://127.0.0.1:8080/vecset \
  -u ignored:s3cret \
  -d '{"id":"item1","vector":[0.1,0.2,0.3,0.4]}'
```

`Basic` ではユーザー名は無視され、パスワードだけが比較されます。これはパスワードのみを検証する TCP の `AUTH` に合わせたものです。どちらの比較も定数時間です。資格情報がない、または誤っている場合は次を返します。

```json
{"error":"Authentication required"}
```

ステータスは `401` です。この検査はハンドラが状態を読む前、応答本文を組み立てる前に走ります。

## ステータスコード

| ステータス | 条件 |
|---|---|
| `200` | 成功 |
| `204` | CORS プリフライト（`OPTIONS`） |
| `400` | JSON の不正、フィールドの欠落や型違い、`top_k` の不正、次元不一致、フィルタの不正、イベントスコアの不正、スナップショットパスのトラバーサル、未対応のキャッシュスコープ |
| `401` | 対象ルートで資格情報がない、または誤っている |
| `403` | 送信元アドレスが `network.allow_cidrs` の外側、またはハンドラからの権限エラー |
| `404` | 未知のルートまたはメソッド、未知のアイテム ID、スナップショットや設定ファイルが見つからない |
| `410` | `/debug/on` と `/debug/off` |
| `429` | レート制限超過 |
| `500` | その他のハンドラエラー（スナップショットの読み取り失敗や整合性検査の失敗を含む） |
| `503` | サーバーが読み込み中または読み取り専用、あるいは WAL が受け付けられなかった書き込み |

ハンドラのエラーはメッセージ文字列を見るのではなく型付きのエラーコードから対応付けられるため、同じ失敗はどのルートでも同じステータスになります。対応は、アイテムやファイルが見つからない条件が `404`、引数・解析・次元・`top_k`・イベントスコアのエラーが `400`、権限拒否が `403`、WAL の書き込み・ローテーション・未オープンのエラーが `503`、それ以外が `500` です。

クライアント側の誤りのうち 2 系統は、エラーコードがこの対応表にないため `400` ではなく `500` になります。`POST /event` の `ctx` や `id` が空、あるいは空白や制御文字を含む場合は `500` で `Context cannot be empty`、`ID cannot be empty`、または `… must not contain whitespace or control characters` が返ります。存在しないファイルを指す `POST /dump/load`、`/dump/verify`、`/dump/info` も同様に `500` です。下層の失敗が not-found のコードではなくストレージの open エラーだからです。どちらもリクエスト側の不備であり、ステータスだけで分類するクライアントはこれをサーバー障害と読み違えます。

エラー本文はすべて 1 フィールドのオブジェクトです。

```json
{"error":"Vector not found: zz"}
```

例外は `POST /dump/verify` で、検証に失敗した場合は `error` だけでなく完全な本文を返します（後述）。

## 書き込みルート

### `POST /event`

```json
{"ctx": "user_alice", "id": "item1", "type": "ADD", "score": 100, "timestamp": 1730000000}
```

| フィールド | 型 | 必須 | 備考 |
|---|---|---|---|
| `ctx` | 文字列 | はい | コンテキスト ID |
| `id` | 文字列 | はい | アイテム ID |
| `type` | 文字列 | はい | `ADD`、`SET`、`DEL`（大文字小文字どちらでも可） |
| `score` | 整数 | `ADD` と `SET` で必須 | 浮動小数点数ではなく整数、範囲 0〜100 |
| `timestamp` | 符号なし整数 | いいえ | epoch 秒、省略時はサーバーの時計 |

```json
{"status":"ok"}
```

浮動小数点数の `score` は切り詰められずに拒否されるため、整数という契約が TCP と一致します。

### `POST /vecset`

```json
{"id": "item1", "vector": [0.1, 0.2, 0.3, 0.4], "metadata": {"category": "books", "price": 19, "active": true}}
```

| フィールド | 型 | 必須 | 備考 |
|---|---|---|---|
| `id` | 文字列 | はい | アイテム ID |
| `vector` | 数値の配列 | はい | 各要素は有限かつ float の範囲内 |
| `metadata` | オブジェクト | いいえ | 値は文字列、整数、浮動小数点数、真偽値、キーは空不可 |

```json
{"dimension":4,"status":"ok"}
```

`dimension` は保存されたベクトルの長さをそのまま返します。TCP の `VECSET` と違い、このルートは同じリクエストでメタデータを付けられます。

### `DELETE /vecset`

```json
{"id": "item1"}
```

```json
{"status":"ok"}
```

ID はパスやクエリ文字列ではなく JSON ボディから読むため、`DELETE` でボディを送らないクライアントライブラリは送るように設定する必要があります。`id` が空または欠落している場合は `400`、未知の ID の場合は `404` で `Vector not found: <id>` を返します。

### `POST /metaset`

```json
{"id": "item1", "metadata": {"category": "books", "price": 19, "active": true}}
```

```json
{"status":"ok"}
```

ここでの `metadata` は構造化された JSON オブジェクトであり、TCP の `METASET` やクライアントライブラリが取る `key:value,key:value` 文字列ではありません。対象のアイテムはすでにベクトルを持っている必要があり、そうでない場合は `404` で `Vector not found for metadata: <id>` を返します。

## 検索ルート

### `POST /sim`

```json
{"id": "item1", "top_k": 3, "mode": "vectors", "filter": "category:books", "min_score": 0.1, "adaptive": true}
```

| フィールド | 型 | 必須 | 既定値 |
|---|---|---|---|
| `id` | 文字列 | はい | — |
| `top_k` | 整数 | いいえ | `similarity.default_top_k` |
| `mode` | 文字列 | いいえ | `fusion`（`events`、`vectors`、`fusion` のいずれか） |
| `filter` | 文字列 | いいえ | フィルタなし |
| `min_score` | 有限の数値 | いいえ | `0.0` |
| `adaptive` | 真偽値 | いいえ | サーバーの `similarity.adaptive_fusion` |

```json
{"count":1,"mode":"vectors","results":[{"id":"item3","score":0.9333}],"status":"ok"}
```

`count` は `min_score` 適用後の `results` の要素数です。スコアは小数点以下 4 桁に丸められ、これは TCP 面が描画する精度と同じです。

`top_k` は正で、かつ `similarity.max_top_k` 以下でなければなりません。どちらの違反も `400` です。`filter` は TCP の `filter=` と同じ文法（`=`、`:`、`!=`、`>`、`<`、`>=`、`<=`、`in(a|b|c)`）を使い、[protocol.md](./protocol.md) に記載しています。ベクトルを持たない `id` は `404` で `Query vector not found: <id>` を返します。

### `POST /simv`

```json
{"vector": [0.1, 0.2, 0.3, 0.4], "top_k": 3, "filter": "active:true", "min_score": 0.1}
```

| フィールド | 型 | 必須 | 既定値 |
|---|---|---|---|
| `vector` | 数値の配列 | はい | — |
| `top_k` | 整数 | いいえ | `similarity.default_top_k` |
| `filter` | 文字列 | いいえ | フィルタなし |
| `min_score` | 有限の数値 | いいえ | `0.0` |

```json
{"count":2,"dimension":4,"results":[{"id":"item1","score":1.0},{"id":"item3","score":0.9333}],"status":"ok"}
```

`dimension` はクエリベクトルの長さをそのまま返します。`mode` も `adaptive` もありません。このルートは常にベクトル検索を行います。

## 内省ルート

### `GET /info`

```json
{
  "server": "nvecd",
  "version": "0.2.0",
  "uptime_seconds": 44,
  "total_requests": 52,
  "total_commands_processed": 52,
  "failed_commands": 12,
  "memory": {
    "used_memory_bytes": 4240,
    "used_memory_human": "4.14KB",
    "used_memory_events": "3.08KB",
    "used_memory_vectors": "578B",
    "used_memory_co_occurrence": "504B",
    "peak_memory_bytes": 9961472,
    "peak_memory_human": "9.50MB",
    "process_rss": 9961472,
    "process_rss_human": "9.50MB",
    "process_rss_peak": 9961472,
    "process_rss_peak_human": "9.50MB",
    "total_system_memory": 137438953472,
    "total_system_memory_human": "128GB",
    "available_system_memory": 67508912128,
    "available_system_memory_human": "62.9GB",
    "system_memory_usage_ratio": 0.5088080167770386,
    "memory_health": "HEALTHY"
  },
  "stores": {
    "event_store": {"contexts": 1, "total_events": 4},
    "vector_store": {"vectors": 1, "dimension": 4},
    "co_index": {"tracked_ids": 2}
  },
  "cache": {
    "enabled": true,
    "total_queries": 10,
    "cache_hits": 0,
    "cache_misses": 10,
    "hit_rate": 0.0,
    "current_entries": 0,
    "current_memory_bytes": 0,
    "evictions": 0,
    "time_saved_ms": 0.0
  }
}
```

`total_requests` と `total_commands_processed` は同じカウンタを 2 つの名前で運んでいます。`memory.used_memory_bytes` は 3 つのストアの合計であり、ベクトル行列だけを数える TCP の `INFO` の `used_memory_bytes` とは別の数値です。`process_rss*` の一群はプロセスのメモリ情報が読める場合にだけ、`*_system_memory*` の一群はシステムのメモリ情報が読める場合にだけ現れます。`stores` の下のエントリは、そのストアが存在する場合にだけ現れます。キャッシュが接続されていない場合、`cache` は `{"enabled": false}` だけになります。

### `GET /config`

```json
{
  "network": {"tcp_enabled": true, "http_enabled": true, "allow_cidrs_configured": true},
  "events": {
    "ctx_buffer_size": 50,
    "max_contexts": 0,
    "max_neighbors_per_item": 0,
    "min_support": 0.0,
    "decay_interval_sec": 3600
  },
  "vectors": {"default_dimension": 4},
  "similarity": {"default_top_k": 100, "fusion_alpha": 0.6},
  "notes": "Sensitive configuration values are redacted. Use CONFIG SHOW over TCP for full details."
}
```

これは意図的に狭くした要約です。バインドアドレス、ポート、パスワード、CIDR リストそのものは公開されず、`allow_cidrs_configured` はリストが空でないかどうかだけを報告します。稼働中の設定の全体は TCP の `CONFIG SHOW` が表示します。

### `GET /cache/stats`

```json
{
  "enabled": true,
  "total_queries": 10,
  "cache_hits": 0,
  "cache_misses": 10,
  "cache_misses_invalidated": 0,
  "cache_misses_not_found": 10,
  "hit_rate": 0.0,
  "current_entries": 0,
  "current_memory_bytes": 0,
  "current_memory_mb": 0.0,
  "min_query_cost_ms": 10.0,
  "ttl_seconds": 600,
  "compression_enabled": true,
  "eviction_batch_size": 10,
  "evictions": 0,
  "avg_hit_latency_ms": 0.0,
  "avg_miss_latency_ms": 8.329999999999999e-05,
  "time_saved_ms": 0.0
}
```

`ttl_seconds` と `min_query_cost_ms` は現在値を反映し、実行時変数 `cache.ttl_seconds` と `cache.min_query_cost_ms` で変更できます。キャッシュコントローラが接続されていない場合、このルートは `500` を返します。

フィールドの集合は TCP の `CACHE STATS` に近いものの同一ではありません。TCP のブロックは `ttl_expirations` も報告し、2 つのフィールドの名前が異なります（ここでの `enabled` と `current_entries` が `cache_enabled` と `cache_entries` にあたります）。

## キャッシュ管理

### `POST /cache/clear`

```json
{"scope": "all"}
```

空のボディは `{"scope": "all"}` として扱われます。

```json
{"entries_removed":0,"scope":"all","status":"ok"}
```

`entries_removed` は消去前に取得したエントリ数です。`all` 以外のスコープは `400` になります。

```json
{"error":"Invalid scope. Only 'all' is supported currently."}
```

JSON オブジェクトでないボディも `400` です。

### `POST /cache/enable` と `POST /cache/disable`

どちらもボディを取りません。

```json
{"message":"Cache enabled","status":"ok"}
```

```json
{"message":"Cache disabled","status":"ok"}
```

どちらも実行時変数 `cache.enabled` を設定するため、変更は次に設定し直すか再起動するまで残ります。

## スナップショット管理

### `POST /dump/save`

```json
{"filepath": "nvecd.nvec"}
```

`filepath` は省略できます。ボディが空、またはフィールドがない場合は `snapshot.default_filename` を使います。パスは `snapshot.dir` の内側で解決され、そこから逃げるパスは `400` です。

```json
{"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}
```

`filepath` はサーバーが使った解決済みの絶対パスです。既定の `snapshot.mode: fork` では、書き込みプロセスの子が生成された時点で応答が返り、ファイルはまだ読めません。応答のキーワードで 2 つのモードを区別する TCP の `DUMP SAVE` と違い、この本文はどちらのモードでも同一です。ファイルを読んだりコピーしたりする前に `GET /dump/status` をポーリングしてください。

### `POST /dump/load`

```json
{"filepath": "nvecd.nvec"}
```

`filepath` は必須で、文字列でなければなりません。

```json
{"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}
```

ファイルが存在しない場合は `500` で、メッセージに元の open 失敗が入ります。`snapshot.dir` から逃げるパスは `400` です。

### `POST /dump/verify`

```json
{"filepath": "nvecd.nvec"}
```

```json
{"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok","valid":true}
```

検証に失敗しても `error` だけのオブジェクトには縮退せず、`valid` を false にした同じ形を返します。呼び出し側はどちらの場合も同じフィールドを読めます。

```json
{
  "status": "error",
  "filepath": "nope.nvec",
  "valid": false,
  "error": "Snapshot verification failed for /var/lib/nvecd/snapshots/nope.nvec: ..."
}
```

ステータスは対応付けられたエラーステータスで、読めないファイルや壊れたファイルの場合は `500` です。

### `POST /dump/info`

```json
{"filepath": "nvecd.nvec"}
```

```json
{
  "status": "ok",
  "filepath": "nvecd.nvec",
  "info": {
    "version": "1",
    "stores": "4",
    "flags": "16",
    "file_size": "575",
    "timestamp": "1788427664",
    "has_statistics": "false"
  }
}
```

`info` の下の値はすべて文字列です。このブロックは共通ハンドラのテキスト出力を解析して作られるためです。ここでの `filepath` は解決済みのパスではなくリクエストの値をそのまま返します。

### `GET /dump/status`

ボディを取りません。`data` がバックグラウンドのスナップショット書き込みプロセスの状態を運びます。

| `data` | 追加フィールド |
|---|---|
| `IDLE` | なし |
| `IN_PROGRESS` | `filepath` |
| `COMPLETED` | `filepath` |
| `FAILED` | `filepath`、`error_message` |

```json
{"data":"COMPLETED","filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}
```

```json
{"data":"IDLE","status":"ok"}
```

`snapshot.mode: lock` で設定されたサーバーにはバックグラウンドの書き込みプロセスがないため、常に `IDLE` を返します。TCP の `DUMP STATUS` ブロックは同じ状態を小文字で報告し、このルートにはない `pid`、`start_time`、`end_time` を運びます。

## デバッグルート

`POST /debug/on` と `POST /debug/off` はルート一覧をコマンド集合に一致させるために登録されており、どちらも次のように拒否します。

```json
{"error":"HTTP debug mode is not supported; use DEBUG ON on a persistent TCP connection"}
```

ステータスは `410` です。デバッグモードはひとつの接続の性質であり、リクエスト単位で完結する HTTP の呼び出しにはそれに相当するものがありません。

## ヘルスチェック

4 つあり、いずれも認証の対象外で、コマンドとしても計上されません。

`GET /health` — 素朴な生存確認で、常に `200` です。

```json
{"status":"ok","timestamp":1788427653}
```

`GET /health/live` — オーケストレータ向けの liveness プローブで、プロセスが動いている間は常に `200` です。

```json
{"status":"alive","timestamp":1788427653}
```

`GET /health/ready` — readiness プローブです。スナップショットを読み込んでいなければ `200` を返します。

```json
{"loading":false,"status":"ready","timestamp":1788427653}
```

読み込み中は `503` です。

```json
{"loading":true,"reason":"Server is loading","status":"not_ready","timestamp":1788427653}
```

`GET /health/detail` — コンポーネントごとの状態で、常に `200` です。これは `/info` と同じ形ではありません。

```json
{
  "status": "healthy",
  "timestamp": 1788427653,
  "uptime_seconds": 44,
  "components": {
    "server": {"status": "ready", "loading": false},
    "event_store": {"status": "ok", "contexts": 1, "total_events": 4},
    "vector_store": {"status": "ok", "vectors": 1, "dimension": 4},
    "co_index": {"status": "ok", "tracked_ids": 2}
  }
}
```

最上位の `status` はスナップショット読み込み中が `degraded`、それ以外が `healthy` です。`components.server.status` は同じ条件で `loading` または `ready` になります。ストアが接続されていないコンポーネントは `components` から省かれます。

## メトリクス

`GET /metrics` は Prometheus のテキスト表現形式を、コンテンツタイプ `text/plain; version=0.0.4; charset=utf-8` で返します。

```text
# HELP nvecd_uptime_seconds Server uptime in seconds
# TYPE nvecd_uptime_seconds counter
nvecd_uptime_seconds 61

# HELP nvecd_commands_total Total commands processed
# TYPE nvecd_commands_total counter
nvecd_commands_total{command="event"} 6
nvecd_commands_total{command="vecset"} 3
nvecd_commands_total{command="sim"} 14
nvecd_commands_total 72

# HELP nvecd_memory_bytes Current memory usage in bytes
# TYPE nvecd_memory_bytes gauge
nvecd_memory_bytes 7060

# HELP nvecd_vectors_total Total vectors stored
# TYPE nvecd_vectors_total gauge
nvecd_vectors_total 1

# HELP nvecd_events_total Total events stored
# TYPE nvecd_events_total gauge
nvecd_events_total 6

# HELP nvecd_contexts_total Total contexts stored
# TYPE nvecd_contexts_total gauge
nvecd_contexts_total 2
```

| メトリクス | 型 | 意味 |
|---|---|---|
| `nvecd_uptime_seconds` | counter | 起動からの秒数 |
| `nvecd_commands_total` | counter | 処理したコマンド数。コマンド別の系列は `command="event"`、`"vecset"`、`"sim"` のラベルを持ち、ラベルなしの系列が全コマンドの合計を運ぶ |
| `nvecd_memory_bytes` | gauge | イベントストア、ベクトルストア、共起索引の合計 |
| `nvecd_vectors_total` | gauge | 保持しているベクトル数 |
| `nvecd_events_total` | gauge | 保持しているイベント数 |
| `nvecd_contexts_total` | gauge | アクティブなコンテキスト数 |
| `nvecd_cache_queries_total` | counter | キャッシュ参照回数 |
| `nvecd_cache_hits_total` | counter | キャッシュヒット数 |
| `nvecd_cache_misses_total` | counter | キャッシュミス数 |
| `nvecd_cache_hit_rate` | gauge | ヒット率 |
| `nvecd_cache_entries` | gauge | 現在保持しているエントリ数 |
| `nvecd_cache_memory_bytes` | gauge | キャッシュの使用メモリ |

ストア系の 3 メトリクスはそのストアが接続されている場合にだけ、キャッシュ系の 6 メトリクスはキャッシュが接続されている場合にだけ現れます。キャッシュを無効にした状態でスクレイプすると `nvecd_cache_*` の系列はひとつもないため、ダッシュボードはその状態を許容する必要があります。

ラベルなしの `nvecd_commands_total` 系列はラベル付きの系列とメトリクス名を共有しています。この混在を拒否するスクレイプ設定もあるため、ラベルなしの系列を落とすリラベルルール、あるいはラベル付きを合計する記録ルールで回避します。

## CORS

CORS ヘッダは `api.http.enable_cors` が true のときだけ出力されます。`Access-Control-Allow-Origin` の値は `api.http.cors_allow_origin` から取り、空の場合はヘッダ自体を省きます。`null` として送らないのは、`null` がサンドボックス化された iframe や `file://` ページのオリジンを指す名前だからです。残りのヘッダは設定されるため、オリジンを注入するプロキシをサーバーの前段に置く構成も取れます。

```bash
$ curl -i -X OPTIONS http://127.0.0.1:8080/sim -H 'Origin: https://example.com'
HTTP/1.1 204 No Content
Access-Control-Allow-Origin: https://example.com
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, Authorization
Content-Length: 0
```

CORS が無効の場合、`OPTIONS` は登録されず `404` を返します。

## 実例

メタデータ付きでアイテムを 2 つ登録し、イベントを記録してから検索します。

```bash
curl -X POST http://127.0.0.1:8080/vecset \
  -d '{"id":"item1","vector":[0.1,0.2,0.3,0.4],"metadata":{"category":"books","price":12}}'
# {"dimension":4,"status":"ok"}

curl -X POST http://127.0.0.1:8080/vecset \
  -d '{"id":"item2","vector":[0.1,0.2,0.3,0.5],"metadata":{"category":"books","price":30}}'
# {"dimension":4,"status":"ok"}

curl -X POST http://127.0.0.1:8080/event \
  -d '{"ctx":"user_alice","id":"item1","type":"ADD","score":100}'
# {"status":"ok"}

curl -X POST http://127.0.0.1:8080/sim \
  -d '{"id":"item1","top_k":5,"mode":"vectors","filter":"price>10"}'
# {"count":1,"mode":"vectors","results":[{"id":"item2","score":0.9940}],"status":"ok"}
```

クエリベクトルで検索し、スコアの低い結果を落とします。

```bash
curl -X POST http://127.0.0.1:8080/simv \
  -d '{"vector":[0.1,0.2,0.3,0.4],"top_k":5,"min_score":0.99}'
# {"count":2,"dimension":4,"results":[{"id":"item1","score":1.0},{"id":"item2","score":0.994}],"status":"ok"}
```

スナップショットを取り、バックグラウンドの書き込みが終わるまで待ちます。

```bash
curl -X POST http://127.0.0.1:8080/dump/save -H 'Authorization: Bearer s3cret' -d '{}'
# {"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}

until curl -s -H 'Authorization: Bearer s3cret' http://127.0.0.1:8080/dump/status \
      | grep -q '"data":"COMPLETED"'; do sleep 1; done
```

Prometheus のスクレイプ設定は次のようになります。

```yaml
scrape_configs:
  - job_name: nvecd
    static_configs:
      - targets: ["127.0.0.1:8080"]
```
