# HTTP API ガイド

Nvecd は、Web アプリケーションや HTTP クライアントから容易に統合できる RESTful な JSON API を提供します。

## 設定

`config.yaml` で HTTP サーバーを有効にします。

```yaml
api:
  tcp:
    bind: "127.0.0.1"
    port: 11017
  http:
    enable: true          # Enable HTTP server
    bind: "127.0.0.1"     # Bind address (default: localhost only)
    port: 8080            # HTTP port (default: 8080)
    enable_cors: false    # Optional: enable only when exposing to browsers
    cors_allow_origin: "" # Optional origin allowed when CORS is enabled
```

**セキュリティ注意**: HTTP サーバーは既定でループバックにバインドします。外部に公開する必要がある場合は、`network.allow_cidrs` で信頼できる IP 範囲に限定し、TLS と認証を備えたリバースプロキシを前段に置いてください。

## 認証

`security.requirepass` が設定されている場合、HTTP サーバーは TCP の `AUTH` ゲートと同じ範囲で、すべての変更系および管理系エンドポイントに認証を要求します。読み取り専用のエンドポイント（health、`/info`、`/config`、`/metrics`、`/cache/stats`、`/sim`、`/simv`）は開いたままです。`/dump/status` は管理系エンドポイントであり、認証が必要です。

パスワードは `Authorization` リクエストヘッダーで渡します。次のどちらの方式も使えます。

- `Authorization: Bearer <password>`
- `Authorization: Basic base64(<user>:<password>)` — ユーザー名は無視され、パスワードのみが比較されます（TCP の `AUTH` と同じ挙動）。

ゲート対象のエンドポイント: `POST /event`、`POST /vecset`、`DELETE /vecset`、`POST /metaset`、`POST /cache/clear`、`POST /cache/enable`、`POST /cache/disable`、`POST /dump/save`、`POST /dump/load`、`POST /dump/verify`、`POST /dump/info`、`GET /dump/status`。

有効な資格情報を持たないリクエストは `401 Unauthorized` を受け取ります。

```json
{
  "error": "Authentication required"
}
```

`security.requirepass` が空（既定）の場合、認証は不要です。

## API エンドポイント

すべてのレスポンスは `Content-Type: application/json` の JSON 形式です。

### POST /event

ユーザー行動（商品閲覧、購入、操作など）を追跡します。

**リクエスト:**

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

```http
POST /event HTTP/1.1
Content-Type: application/json

{
  "ctx": "user_alice",
  "id": "product123",
  "type": "ADD",
  "score": 100
}
```

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-------|------|----------|-------------|
| `ctx` | string | はい | コンテキスト ID（ユーザー ID、セッション ID など） |
| `id` | string | はい | アイテム ID（商品 ID、記事 ID など） |
| `type` | string | はい | イベント種別: `ADD`、`SET`、`DEL` |
| `score` | integer | `ADD`/`SET` では必須 | イベントスコア（0-100、例: 100=購入、80=閲覧） |
| `timestamp` | integer | いいえ | イベントのタイムスタンプ（epoch 秒） |

**レスポンス (200 OK):**

```json
{
  "status": "ok"
}
```

**エラーレスポンス (400 Bad Request):**

```json
{
  "error": "Missing required field: ctx"
}
```

### POST /vecset

アイテムの埋め込みベクトルを登録または更新します。

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**リクエスト:**

```http
POST /vecset HTTP/1.1
Content-Type: application/json

{
  "id": "product123",
  "vector": [0.1, 0.2, 0.3, 0.4, 0.5],
  "metadata": {
    "category": "electronics",
    "active": true
  }
}
```

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-------|------|----------|-------------|
| `id` | string | はい | アイテム ID |
| `vector` | float の配列 | はい | 埋め込みベクトル（次元は既存ベクトルと一致する必要があります） |
| `metadata` | object | いいえ | `filter=` クエリが使用するメタデータ。値は string、integer、float、bool のいずれか。 |

**レスポンス (200 OK):**

```json
{
  "status": "ok"
}
```

**エラーレスポンス (400 Bad Request):**

```json
{
  "error": "Dimension mismatch: expected 768, got 512"
}
```

### DELETE /vecset

アイテムのベクトル、そのメタデータ、キャッシュ済みの結果を削除します。

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**リクエスト:**

```http
DELETE /vecset HTTP/1.1
Content-Type: application/json

{
  "id": "product123"
}
```

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-------|------|----------|-------------|
| `id` | string | はい | アイテム ID |

**レスポンス (200 OK):**

```json
{
  "status": "ok"
}
```

**エラーレスポンス (404 Not Found):**

```json
{
  "error": "Vector not found: product123"
}
```

### POST /metaset

既存アイテムのメタデータを設定（置換）します。メタデータはアイテム ID をキーとし、`filter=` クエリで使用されます。対象アイテムには `/vecset` でベクトルが登録済みである必要があり、そうでない場合は `404 Not Found` を返します。

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**リクエスト:**

```http
POST /metaset HTTP/1.1
Content-Type: application/json

{
  "id": "product123",
  "metadata": {
    "category": "electronics",
    "active": true,
    "price": 199
  }
}
```

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-------|------|----------|-------------|
| `id` | string | はい | アイテム ID（この ID のベクトルが既に存在する必要があります） |
| `metadata` | object | はい | メタデータのマップ。値は string、integer、float、bool のいずれか。 |

**レスポンス (200 OK):**

```json
{
  "status": "ok"
}
```

**エラーレスポンス (404 Not Found):**

```json
{
  "error": "Vector not found for metadata: product123"
}
```

### POST /sim

ID を指定して類似アイテムを検索します。

**リクエスト:**

```http
POST /sim HTTP/1.1
Content-Type: application/json

{
  "id": "product123",
  "top_k": 10,
  "mode": "fusion"
}
```

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 既定値 | 説明 |
|-------|------|----------|---------|-------------|
| `id` | string | はい | - | クエリ対象のアイテム ID |
| `top_k` | integer | いいえ | 10 | 返す結果数 |
| `mode` | string | いいえ | "fusion" | 検索モード: "vectors"、"events"、"fusion" |
| `filter` | string | いいえ | - | メタデータフィルタ（例: "category:electronics,type:laptop"） |
| `min_score` | float | いいえ | 0.0 | 最小スコア閾値（これを下回る結果は除外されます） |
| `adaptive` | boolean | いいえ | false | adaptive fusion を有効化（データ密度に応じて重みを自動調整） |

**検索モード:**

| モード | 説明 |
|------|-------------|
| `vectors` | コンテンツベースの類似度（ベクトル埋め込みを使用） |
| `events` | 行動ベースの類似度（イベントからの共起） |
| `fusion` | ハイブリッド: ベクトルとイベントを統合 |

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "count": 3,
  "mode": "fusion",
  "results": [
    {
      "id": "product456",
      "score": 0.9245
    },
    {
      "id": "product789",
      "score": 0.8932
    },
    {
      "id": "product101",
      "score": 0.8501
    }
  ]
}
```

**レスポンスフィールド:**

| フィールド | 説明 |
|-------|-------------|
| `status` | 成功時は `"ok"` |
| `count` | 返した結果数 |
| `mode` | 実際に使用された検索モード |
| `results` | 類似アイテムの配列（スコア降順） |
| `results[].id` | アイテム ID |
| `results[].score` | 類似度スコア（0.0-1.0、大きいほど類似） |

**エラーレスポンス (404 Not Found):**

```json
{
  "error": "Vector not found: product123"
}
```

### POST /simv

ベクトルを直接指定して類似アイテムを検索します。

**リクエスト:**

```http
POST /simv HTTP/1.1
Content-Type: application/json

{
  "vector": [0.1, 0.2, 0.3, 0.4, 0.5],
  "top_k": 10
}
```

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 既定値 | 説明 |
|-------|------|----------|---------|-------------|
| `vector` | float の配列 | はい | - | クエリベクトル |
| `top_k` | integer | いいえ | 10 | 返す結果数 |
| `filter` | string | いいえ | - | メタデータフィルタ（例: "type:article"） |
| `min_score` | float | いいえ | 0.0 | 最小スコア閾値 |

`/simv` は常にベクトル空間の検索を行うため、`mode` パラメータはありません。

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "count": 3,
  "dimension": 5,
  "results": [
    { "id": "product456", "score": 0.9245 }
  ]
}
```

**ユースケース:**

- ユーザークエリの埋め込みで検索する（例: 「赤いランニングシューズ」→ ベクトル）
- 計算したベクトルに一致するアイテムを探す（例: 気に入ったアイテムの平均）

### GET /info

サーバーの統計情報と監視情報（Redis スタイル）を返します。

**リクエスト:**

```http
GET /info HTTP/1.1
```

**レスポンス (200 OK):**

```json
{
  "server": "nvecd",
  "version": "0.1.0",
  "uptime_seconds": 3600,
  "total_requests": 15000,
  "total_commands_processed": 15000,
  "failed_commands": 12,
  "memory": {
    "used_memory_bytes": 949452800,
    "used_memory_human": "905.50 MB",
    "used_memory_events": "500.00 MB",
    "used_memory_vectors": "293.00 MB",
    "used_memory_co_occurrence": "100.00 MB",
    "peak_memory_bytes": 1010000000,
    "peak_memory_human": "963.20 MB",
    "process_rss": 990000000,
    "process_rss_human": "944.13 MB",
    "memory_health": "HEALTHY"
  },
  "stores": {
    "event_store": {
      "contexts": 50000,
      "total_events": 1000000
    },
    "vector_store": {
      "vectors": 100000,
      "dimension": 768
    },
    "co_index": {
      "tracked_ids": 250000
    }
  },
  "cache": {
    "enabled": true,
    "total_queries": 10000,
    "cache_hits": 8500,
    "cache_misses": 1500,
    "hit_rate": 0.85,
    "current_entries": 2450,
    "current_memory_bytes": 13107200,
    "evictions": 320,
    "time_saved_ms": 15420.50
  }
}
```

**レスポンスフィールド:**

| 分類 | フィールド | 説明 |
|----------|-------|-------------|
| **サーバー** | `server` | サーバー名（nvecd） |
| | `version` | サーバーバージョン |
| | `uptime_seconds` | サーバー稼働時間（秒） |
| | `total_requests` | `total_commands_processed` の別名。両者は同じカウンタを返します |
| | `total_commands_processed` | 処理したコマンド総数 |
| | `failed_commands` | エラーを返したコマンド数 |
| **メモリ** | `used_memory_bytes` | 追跡対象ストアの合計メモリ（バイト） |
| | `used_memory_events` | イベントストアのメモリ（可読形式） |
| | `used_memory_vectors` | ベクトルストアのメモリ（可読形式） |
| | `used_memory_co_occurrence` | 共起インデックスのメモリ（可読形式） |
| | `peak_memory_bytes` | プロセス RSS のピーク（バイト） |
| | `memory_health` | メモリの健全性ステータス |
| **ストア** | `stores.vector_store.vectors` | 格納されているベクトル数 |
| | `stores.vector_store.dimension` | ベクトルの次元数 |
| | `stores.event_store.contexts` | コンテキスト数（ユーザー / セッション） |
| | `stores.event_store.total_events` | 追跡したイベント総数 |
| | `stores.co_index.tracked_ids` | 共起インデックスが追跡している ID 数 |
| **キャッシュ** | `cache.enabled` | クエリキャッシュが有効かどうか |
| | `cache.hit_rate` | キャッシュヒット率（0.0-1.0） |
| | `cache.current_memory_bytes` | 現在のキャッシュメモリ使用量（バイト） |
| | `cache.time_saved_ms` | キャッシュによって短縮された累計時間 |

このエンドポイントは監視ツールやヘルスチェックに適しています。プラットフォームが公開している場合、`memory` オブジェクトにはシステム全体のフィールド（`total_system_memory`、`available_system_memory`、`system_memory_usage_ratio`）も含まれます。

### GET /health

ロードバランサ向けの簡易ヘルスチェックです。

**リクエスト:**

```http
GET /health HTTP/1.1
```

**レスポンス (200 OK):**

```json
{
  "status": "ok"
}
```

### GET /health/live

Kubernetes の liveness プローブです（サーバーが動作していれば常に 200 を返します）。

**リクエスト:**

```http
GET /health/live HTTP/1.1
```

**レスポンス (200 OK):**

```json
{
  "status": "alive",
  "timestamp": 1705564800
}
```

### GET /health/ready

Kubernetes の readiness プローブです（スナップショット読み込み中は 503 を返します）。

**リクエスト:**

```http
GET /health/ready HTTP/1.1
```

**レスポンス (200 OK):**

```json
{
  "status": "ready",
  "loading": false
}
```

**レスポンス (503 Service Unavailable):**

```json
{
  "status": "not_ready",
  "loading": true,
  "reason": "Server is loading"
}
```

### GET /health/detail

メトリクスを含む詳細なヘルス情報（`/info` と同じ内容）を返します。

**リクエスト:**

```http
GET /health/detail HTTP/1.1
```

**レスポンス (200 OK):**

`/info` エンドポイントと同じ形式です。

### GET /metrics

サーバーのメトリクスを Prometheus テキスト形式（`Content-Type: text/plain; version=0.0.4`）で返します。

**リクエスト:**

```http
GET /metrics HTTP/1.1
```

**レスポンス (200 OK):**

```text
# HELP nvecd_uptime_seconds Server uptime in seconds
# TYPE nvecd_uptime_seconds counter
nvecd_uptime_seconds 3600

# HELP nvecd_commands_total Total commands processed
# TYPE nvecd_commands_total counter
nvecd_commands_total{command="event"} 4200
nvecd_commands_total{command="vecset"} 1800
nvecd_commands_total{command="sim"} 9000
nvecd_commands_total 15000

# HELP nvecd_memory_bytes Current memory usage in bytes
# TYPE nvecd_memory_bytes gauge
nvecd_memory_bytes 949452800
```

キャッシュ・ベクトル・イベント・コンテキストのゲージも出力されます（`nvecd_cache_hit_rate`、`nvecd_vectors_total`、`nvecd_events_total` など）。

### GET /config

現在のサーバー設定の要約を返します（機微な値は省略されます）。

**CORS**: `api.http.enable_cors` が `true` のとき、サーバーは `Access-Control-Allow-Origin` ヘッダーを付与し、OPTIONS のプリフライトリクエストを処理します。

**リクエスト:**

```http
GET /config HTTP/1.1
```

**レスポンス (200 OK):**

```json
{
  "network": {
    "tcp_enabled": true,
    "http_enabled": true,
    "allow_cidrs_configured": true
  },
  "events": {
    "ctx_buffer_size": 1000,
    "decay_interval_sec": 60
  },
  "vectors": {
    "default_dimension": 768
  },
  "similarity": {
    "default_top_k": 10,
    "fusion_alpha": 0.6
  },
  "notes": "Sensitive configuration values are redacted. Use CONFIG SHOW over TCP for full details."
}
```

バインドアドレスとポートはセキュリティ上の理由から `/config` では意図的に省略されています。完全な内容が必要な場合は TCP の `CONFIG SHOW` を使用してください。

### POST /dump/save

サーバーのスナップショットをディスクに保存します。

**リクエスト:**

```http
POST /dump/save HTTP/1.1
Content-Type: application/json

{
  "filepath": "snapshot-20250118.dmp"
}
```

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**リクエストボディのパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-------|------|----------|-------------|
| `filepath` | string | いいえ | スナップショットのファイルパス（省略時は自動生成） |

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "filepath": "snapshot-20250118.dmp"
}
```

**エラーレスポンス (5xx):**

保存に失敗した場合、エンドポイントは実際のエラーメッセージとともに 2xx 以外のステータス（`500` など）を返します。保存が失敗したのに `status: ok` を返すことはありません。

```json
{
  "error": "Failed to save snapshot to snapshot-20250118.dmp: ..."
}
```

### POST /dump/load

ディスクからサーバーのスナップショットを読み込みます。

**リクエスト:**

```http
POST /dump/load HTTP/1.1
Content-Type: application/json

{
  "filepath": "snapshot-20250118.dmp"
}
```

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "filepath": "snapshot-20250118.dmp"
}
```

**エラーレスポンス (404 Not Found):**

スナップショットファイルが存在しない場合は `404` になります。それ以外の失敗は、内部エラーに応じて `400` または `500` になります。

```json
{
  "error": "Failed to load snapshot from snapshot-20250118.dmp: ..."
}
```

### POST /dump/verify

スナップショットファイルの整合性を検証します。

**リクエスト:**

```http
POST /dump/verify HTTP/1.1
Content-Type: application/json

{
  "filepath": "snapshot-20250118.dmp"
}
```

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "filepath": "snapshot-20250118.dmp",
  "valid": true
}
```

**レスポンス（検証失敗時）:**

整合性の検証に失敗した場合、エンドポイントは `valid: false` と実際のエラーメッセージとともに 2xx 以外のステータスを返します（`valid: true` を固定で返すことはありません）。

```json
{
  "status": "error",
  "filepath": "snapshot-20250118.dmp",
  "valid": false,
  "error": "Snapshot verification failed for ...: CRC mismatch"
}
```

### POST /dump/info

スナップショットファイルのメタデータを取得します。

**リクエスト:**

```http
POST /dump/info HTTP/1.1
Content-Type: application/json

{
  "filepath": "snapshot-20250118.dmp"
}
```

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "filepath": "snapshot-20250118.dmp",
  "info": {
    "version": "1",
    "stores": "3",
    "flags": "0",
    "file_size": "314572800",
    "timestamp": "1705564800",
    "has_statistics": "true"
  }
}
```

### GET /dump/status

バックグラウンドスナップショット処理の状態を取得します。

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します。

**リクエスト:**

```http
GET /dump/status HTTP/1.1
```

**レスポンス (200 OK):**

```json
{"status": "ok", "data": "IDLE"}
```

### GET /cache/stats

キャッシュの統計情報（ヒット率、エントリ数、メモリ使用量）を取得します。

**リクエスト:**

```http
GET /cache/stats HTTP/1.1
```

**レスポンス (200 OK):**

```json
{
  "enabled": true,
  "total_queries": 10000,
  "cache_hits": 8500,
  "cache_misses": 1500,
  "hit_rate": 0.85,
  "current_entries": 2450,
  "current_memory_mb": 12.45,
  "evictions": 320
}
```

### POST /cache/clear

類似検索キャッシュをクリアします。

`security.requirepass` が設定されている場合、このエンドポイントは認証を要求します（`/cache/enable` と `/cache/disable` も同様です）。

**リクエスト:**

```http
POST /cache/clear HTTP/1.1
Content-Type: application/json

{
  "scope": "all"
}
```

**レスポンス (200 OK):**

```json
{
  "status": "ok",
  "scope": "all",
  "entries_removed": 2450
}
```

### POST /cache/enable

類似検索キャッシュを有効にします。

**リクエスト:** ボディは不要です。

**レスポンス (200 OK):**

```json
{"status": "ok", "message": "Cache enabled"}
```

### POST /cache/disable

類似検索キャッシュを無効にします。

**リクエスト:** ボディは不要です。

**レスポンス (200 OK):**

```json
{"status": "ok", "message": "Cache disabled"}
```

### POST /debug/on

非推奨です。HTTP はステートレスであり、接続単位のデバッグモードをサポートしません。このエンドポイントは `410 Gone` を返します。持続的な TCP 接続上で `DEBUG ON` を使用してください。

**リクエスト:**

```http
POST /debug/on HTTP/1.1
```

**レスポンス (410 Gone):**

```json
{
  "error": "HTTP debug mode is not supported; use DEBUG ON on a persistent TCP connection"
}
```

### POST /debug/off

非推奨です。このエンドポイントは `410 Gone` を返します。同じ持続的な TCP 接続上で `DEBUG OFF` を使用してください。

**リクエスト:**

```http
POST /debug/off HTTP/1.1
```

**レスポンス (410 Gone):**

```json
{
  "error": "HTTP debug mode is not supported; use DEBUG OFF on a persistent TCP connection"
}
```

## CORS サポート

ブラウザクライアント向けに CORS ヘッダーを有効にするには `api.http.enable_cors: true` を設定し、`api.http.cors_allow_origin` で信頼するオリジンを指定します。ブラウザから直接アクセスしない場合は CORS を無効のままにしてください。

**CORS ヘッダー:**

```
Access-Control-Allow-Origin: https://app.example.com
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

## 使用例

### cURL

**イベントの追跡:**

```bash
curl -X POST http://localhost:8080/event \
  -H "Content-Type: application/json" \
  -d '{
    "ctx": "user_alice",
    "id": "product123",
    "type": "ADD",
    "score": 100
  }'
```

**ベクトルの登録:**

```bash
curl -X POST http://localhost:8080/vecset \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "vector": [0.1, 0.2, 0.3, 0.4]
  }'
```

**類似アイテムの検索:**

```bash
curl -X POST http://localhost:8080/sim \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "top_k": 10,
    "mode": "fusion"
  }'
```

**フィルタと min_score を指定した検索:**

```bash
curl -X POST http://localhost:8080/sim \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "top_k": 10,
    "mode": "fusion",
    "filter": "category:electronics",
    "min_score": 0.5
  }'
```

**ヘルスチェック:**

```bash
curl http://localhost:8080/health
```

**サーバー情報:**

```bash
curl http://localhost:8080/info | jq .
```

### JavaScript (fetch)

```javascript
// Track user purchase
await fetch('http://localhost:8080/event', {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json'
  },
  body: JSON.stringify({
    ctx: 'user_alice',
    id: 'product123',
    type: 'ADD',
    score: 100
  })
});

// Get recommendations
const response = await fetch('http://localhost:8080/sim', {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json'
  },
  body: JSON.stringify({
    id: 'product123',
    top_k: 10,
    mode: 'fusion'
  })
});

const data = await response.json();
console.log(`Found ${data.count} recommendations`);
data.results.forEach(item => {
  console.log(`  ${item.id}: ${item.score}`);
});
```

### Python (requests)

```python
import requests

# Track event
requests.post('http://localhost:8080/event', json={
    'ctx': 'user_alice',
    'id': 'product123',
    'type': 'ADD',
    'score': 100
})

# Register vector
requests.post('http://localhost:8080/vecset', json={
    'id': 'product123',
    'vector': [0.1, 0.2, 0.3, 0.4]
})

# Get recommendations
response = requests.post('http://localhost:8080/sim', json={
    'id': 'product123',
    'top_k': 10,
    'mode': 'fusion'
})

data = response.json()
print(f"Found {data['count']} recommendations")
for item in data['results']:
    print(f"  {item['id']}: {item['score']}")
```

### 完全な例: EC サイトの推薦

```python
import requests

BASE_URL = 'http://localhost:8080'

# 1. Register product embeddings (from ML model)
products = {
    'laptop_001': [0.1, 0.2, 0.3, 0.4],
    'laptop_002': [0.15, 0.25, 0.28, 0.38],
    'phone_001': [0.8, 0.7, 0.6, 0.5]
}

for product_id, vector in products.items():
    requests.post(f'{BASE_URL}/vecset', json={
        'id': product_id,
        'vector': vector
    })

# 2. Track user behavior
events = [
    ('user_alice', 'laptop_001', 100),  # Purchased
    ('user_alice', 'laptop_002', 80),   # Viewed
    ('user_bob', 'laptop_001', 100),    # Purchased
    ('user_bob', 'phone_001', 90)       # Viewed
]

for ctx, product_id, score in events:
    requests.post(f'{BASE_URL}/event', json={
        'ctx': ctx,
        'id': product_id,
        'type': 'ADD',
        'score': score
    })

# 3. Get content-based recommendations
response = requests.post(f'{BASE_URL}/sim', json={
    'id': 'laptop_001',
    'top_k': 5,
    'mode': 'vectors'
})
print("Content-based recommendations:", response.json()['results'])

# 4. Get behavior-based recommendations
response = requests.post(f'{BASE_URL}/sim', json={
    'id': 'laptop_001',
    'top_k': 5,
    'mode': 'events'
})
print("Behavior-based recommendations:", response.json()['results'])

# 5. Get hybrid recommendations (fusion)
response = requests.post(f'{BASE_URL}/sim', json={
    'id': 'laptop_001',
    'top_k': 5,
    'mode': 'fusion'
})
print("Hybrid recommendations:", response.json()['results'])
```

## パフォーマンス上の考慮点

- **接続の再利用**: HTTP keep-alive を使うと性能が向上します
- **キャッシュ**: `/info` のキャッシュメトリクスでキャッシュが効いているか確認します
- **ネットワークセキュリティ**: `network.allow_cidrs` でアクセスを制限します
- **リバースプロキシ**: TLS と認証のために nginx / HAProxy を nvecd の前段に置くことを検討します

## エラーハンドリング

すべてのエラーレスポンスは次の形式に従います。

```json
{
  "error": "Error message description"
}
```

**HTTP ステータスコード:**

| コード | 説明 |
|------|-------------|
| 200 | 成功 |
| 400 | Bad Request（入力が不正） |
| 401 | Unauthorized（ゲート対象エンドポイントで資格情報が無いか不正） |
| 403 | Forbidden（`network.allow_cidrs` によってブロック） |
| 404 | Not Found（ベクトルまたはスナップショットが存在しない） |
| 500 | Internal Server Error |
| 503 | Service Unavailable（サーバーが読み込み中） |

## 監視

HTTP API は監視向けに複数のエンドポイントを提供します。

- **ヘルスチェック**: `GET /health` - 簡易ヘルスチェック
- **Liveness プローブ**: `GET /health/live` - Kubernetes の liveness
- **Readiness プローブ**: `GET /health/ready` - Kubernetes の readiness
- **詳細メトリクス**: `GET /health/detail` - 完全な統計情報

### Kubernetes へのデプロイ

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: nvecd
spec:
  containers:
  - name: nvecd
    image: nvecd:latest
    ports:
    - containerPort: 8080
    livenessProbe:
      httpGet:
        path: /health/live
        port: 8080
      initialDelaySeconds: 10
      periodSeconds: 30
    readinessProbe:
      httpGet:
        path: /health/ready
        port: 8080
      initialDelaySeconds: 5
      periodSeconds: 10
```

### Prometheus メトリクス

現在 `GET /metrics` は Prometheus テキスト形式を公開します。JSON レスポンスが扱いやすい場合は `/info` または `/health/detail` を使用してください。
