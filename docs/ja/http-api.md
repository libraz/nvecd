# HTTP API ガイド

Nvecd は Web アプリケーションや HTTP クライアントとの統合を容易にする RESTful JSON API を提供します。

## 設定

`config.yaml` で HTTP サーバーを有効化します：

```yaml
api:
  tcp:
    bind: "127.0.0.1"
    port: 11017
  http:
    enable: true          # HTTP サーバーを有効化
    bind: "127.0.0.1"     # バインドアドレス（デフォルト: ローカルホストのみ）
    port: 8080            # HTTP ポート（デフォルト: 8080）
    enable_cors: false    # オプション: ブラウザに公開する場合のみ有効化
    cors_allow_origin: "" # CORS 有効時に許可するオリジン
```

**セキュリティ注意:** HTTP サーバーはデフォルトでループバックにバインドされます。公開する必要がある場合は、`network.allow_cidrs` を設定して信頼できる IP 範囲からのアクセスに制限し、リバースプロキシで TLS/認証を使用してください。

## API エンドポイント

すべてのレスポンスは JSON 形式で `Content-Type: application/json` です。

### POST /event

ユーザー行動を追跡（例: 商品閲覧、購入、インタラクション）。

**リクエスト:**

```http
POST /event HTTP/1.1
Content-Type: application/json

{
  "ctx": "user_alice",
  "id": "product123",
  "score": 100
}
```

**リクエストボディパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-----------|---|------|------|
| `ctx` | string | Yes | コンテキスト ID（例: ユーザー ID、セッション ID） |
| `id` | string | Yes | アイテム ID（例: 商品 ID、記事 ID） |
| `score` | integer | Yes | イベントスコア（0-100、例: 100=購入、80=閲覧） |

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

**エラーレスポンス（400 Bad Request）:**

```json
{
  "error": "Missing required field: ctx"
}
```

### POST /vecset

アイテムのベクトル埋め込みを登録または更新します。

**リクエスト:**

```http
POST /vecset HTTP/1.1
Content-Type: application/json

{
  "id": "product123",
  "vector": [0.1, 0.2, 0.3, 0.4, 0.5]
}
```

**リクエストボディパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-----------|---|------|------|
| `id` | string | Yes | アイテム ID |
| `vector` | array of floats | Yes | 埋め込みベクトル（次元は既存ベクトルと一致する必要あり） |

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

**エラーレスポンス（400 Bad Request）:**

```json
{
  "error": "Dimension mismatch: expected 768, got 512"
}
```

### POST /sim

ID で類似アイテムを検索します。

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

**リクエストボディパラメータ:**

| フィールド | 型 | 必須 | デフォルト | 説明 |
|-----------|---|------|-----------|------|
| `id` | string | Yes | - | クエリアイテム ID |
| `top_k` | integer | No | 10 | 返す結果数 |
| `mode` | string | No | "vectors" | 検索モード: "vectors", "events", または "fusion" |

**検索モード:**

| モード | 説明 |
|--------|------|
| `vectors` | コンテンツベース類似性（ベクトル埋め込みを使用） |
| `events` | 行動ベース類似性（イベントからの共起） |
| `fusion` | ハイブリッド: ベクトル + イベントを組み合わせ |

**レスポンス（200 OK）:**

```json
{
  "count": 3,
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
|-----------|------|
| `count` | 返された結果数 |
| `results` | 類似アイテムの配列（スコアの降順でソート） |
| `results[].id` | アイテム ID |
| `results[].score` | 類似度スコア（0.0-1.0、高いほど類似） |

**エラーレスポンス（404 Not Found）:**

```json
{
  "error": "Vector not found: product123"
}
```

### POST /simv

生のベクトルクエリで類似アイテムを検索します。

**リクエスト:**

```http
POST /simv HTTP/1.1
Content-Type: application/json

{
  "vector": [0.1, 0.2, 0.3, 0.4, 0.5],
  "top_k": 10,
  "mode": "vectors"
}
```

**リクエストボディパラメータ:**

| フィールド | 型 | 必須 | デフォルト | 説明 |
|-----------|---|------|-----------|------|
| `vector` | array of floats | Yes | - | クエリベクトル |
| `top_k` | integer | No | 10 | 返す結果数 |
| `mode` | string | No | "vectors" | 検索モード（通常は "vectors"） |

**レスポンス（200 OK）:**

`/sim` エンドポイントと同じ形式。

**ユースケース:**

- ユーザークエリ埋め込みで検索（例: "赤いランニングシューズ" → ベクトル）
- 計算されたベクトルにマッチするアイテムを検索（例: お気に入りアイテムの平均）

### GET /info

サーバー統計情報とモニタリング情報（Redis スタイル）。

**リクエスト:**

```http
GET /info HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "server": "nvecd",
  "version": "0.1.0",
  "uptime_seconds": 3600,
  "total_requests": 15000,
  "connections": 5,
  "memory": {
    "vector_store_bytes": 307200000,
    "vector_store_human": "293.00 MB",
    "event_store_bytes": 524288000,
    "event_store_human": "500.00 MB",
    "co_occurrence_index_bytes": 104857600,
    "co_occurrence_index_human": "100.00 MB",
    "cache_bytes": 13107200,
    "cache_human": "12.50 MB",
    "total_bytes": 949452800,
    "total_human": "905.50 MB"
  },
  "vector_store": {
    "total_vectors": 100000,
    "dimension": 768
  },
  "event_store": {
    "total_contexts": 50000,
    "total_events": 1000000,
    "ctx_buffer_size": 1000
  },
  "co_occurrence_index": {
    "total_pairs": 250000,
    "decay_factor": 0.99
  },
  "cache": {
    "enabled": true,
    "total_queries": 10000,
    "cache_hits": 8500,
    "cache_misses": 1500,
    "hit_rate": 0.8500,
    "current_entries": 2450,
    "current_memory_mb": 12.45,
    "max_memory_mb": 32.00,
    "evictions": 320,
    "avg_hit_latency_ms": 0.05,
    "time_saved_ms": 15420.50
  }
}
```

**レスポンスフィールド:**

| カテゴリ | フィールド | 説明 |
|---------|-----------|------|
| **サーバー** | `server` | サーバー名（nvecd） |
| | `version` | サーバーバージョン |
| | `uptime_seconds` | サーバー稼働時間（秒） |
| | `total_requests` | 処理されたリクエスト総数 |
| | `connections` | 現在のアクティブ接続数 |
| **メモリ** | `vector_store_bytes` | ベクトルストアが使用するメモリ |
| | `event_store_bytes` | イベントストアが使用するメモリ |
| | `co_occurrence_index_bytes` | 共起インデックスが使用するメモリ |
| | `cache_bytes` | キャッシュが使用するメモリ |
| | `total_bytes` | 総メモリ使用量 |
| **ベクトルストア** | `total_vectors` | 保存されているベクトル数 |
| | `dimension` | ベクトル次元 |
| **イベントストア** | `total_contexts` | コンテキスト数（ユーザー/セッション） |
| | `total_events` | 追跡されたイベント総数 |
| **キャッシュ** | `hit_rate` | キャッシュヒット率（0.0-1.0） |
| | `time_saved_ms` | キャッシュにより節約された総レイテンシ |

このエンドポイントはモニタリングツールやヘルスチェックに適しています。

### GET /health

ロードバランサー用のシンプルなヘルスチェックエンドポイント。

**リクエスト:**

```http
GET /health HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

### GET /health/live

Kubernetes liveness probe（サーバーが稼働していれば常に 200 を返す）。

**リクエスト:**

```http
GET /health/live HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

### GET /health/ready

Kubernetes readiness probe（サーバーがスナップショット読み込み中の場合は 503 を返す）。

**リクエスト:**

```http
GET /health/ready HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

**レスポンス（503 Service Unavailable）:**

```json
{
  "status": "loading"
}
```

### GET /health/detail

メトリクス付き詳細ヘルス情報（`/info` と同じ）。

**リクエスト:**

```http
GET /health/detail HTTP/1.1
```

**レスポンス（200 OK）:**

`/info` エンドポイントと同じ形式。

### GET /config

現在のサーバー設定の概要（機密値は省略）。

**リクエスト:**

```http
GET /config HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "api": {
    "tcp": {
      "enabled": true,
      "port": 11017
    },
    "http": {
      "enabled": true,
      "port": 8080,
      "cors_enabled": false
    }
  },
  "network": {
    "allow_cidrs_configured": true
  },
  "cache": {
    "enabled": true,
    "max_memory_mb": 32
  },
  "events": {
    "ctx_buffer_size": 1000,
    "decay_factor": 0.99
  }
}
```

### POST /dump/save

サーバースナップショットをディスクに保存します。

**リクエスト:**

```http
POST /dump/save HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**リクエストボディパラメータ:**

| フィールド | 型 | 必須 | 説明 |
|-----------|---|------|------|
| `filepath` | string | No | スナップショットファイルパス（省略時は自動生成） |

**レスポンス（200 OK）:**

```json
{
  "status": "ok",
  "filepath": "/backup/snapshot-20250118.dmp",
  "size_bytes": 314572800
}
```

### POST /dump/load

ディスクからサーバースナップショットを読み込みます。

**リクエスト:**

```http
POST /dump/load HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok",
  "vectors_loaded": 100000,
  "events_loaded": 1000000
}
```

**エラーレスポンス（404 Not Found）:**

```json
{
  "error": "Snapshot file not found"
}
```

### POST /dump/verify

スナップショットファイルの整合性を検証します。

**リクエスト:**

```http
POST /dump/verify HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok",
  "checksum_valid": true
}
```

### POST /dump/info

スナップショットファイルのメタデータを取得します。

**リクエスト:**

```http
POST /dump/info HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**レスポンス（200 OK）:**

```json
{
  "version": 1,
  "created_at": 1705564800,
  "vector_count": 100000,
  "event_count": 1000000,
  "size_bytes": 314572800
}
```

### POST /debug/on

デバッグモードを有効化（ログに詳細なクエリタイミングを表示）。

**リクエスト:**

```http
POST /debug/on HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

### POST /debug/off

デバッグモードを無効化します。

**リクエスト:**

```http
POST /debug/off HTTP/1.1
```

**レスポンス（200 OK）:**

```json
{
  "status": "ok"
}
```

## CORS サポート

ブラウザクライアント向けに CORS ヘッダーを有効にするには `api.http.enable_cors: true` を設定し、`api.http.cors_allow_origin` で信頼できるオリジンを指定します。ブラウザから直接アクセスしない場合は CORS を無効のままにしてください。

**CORS ヘッダー:**

```
Access-Control-Allow-Origin: https://app.example.com
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

## 使用例

### cURL

**イベント追跡:**

```bash
curl -X POST http://localhost:8080/event \
  -H "Content-Type: application/json" \
  -d '{
    "ctx": "user_alice",
    "id": "product123",
    "score": 100
  }'
```

**ベクトル登録:**

```bash
curl -X POST http://localhost:8080/vecset \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "vector": [0.1, 0.2, 0.3, 0.4]
  }'
```

**類似アイテム検索:**

```bash
curl -X POST http://localhost:8080/sim \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "top_k": 10,
    "mode": "fusion"
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
// ユーザー購入を追跡
await fetch('http://localhost:8080/event', {
  method: 'POST',
  headers: {
    'Content-Type': 'application/json'
  },
  body: JSON.stringify({
    ctx: 'user_alice',
    id: 'product123',
    score: 100
  })
});

// 推薦を取得
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
console.log(`${data.count} 件の推薦が見つかりました`);
data.results.forEach(item => {
  console.log(`  ${item.id}: ${item.score}`);
});
```

### Python (requests)

```python
import requests

# イベント追跡
requests.post('http://localhost:8080/event', json={
    'ctx': 'user_alice',
    'id': 'product123',
    'score': 100
})

# ベクトル登録
requests.post('http://localhost:8080/vecset', json={
    'id': 'product123',
    'vector': [0.1, 0.2, 0.3, 0.4]
})

# 推薦を取得
response = requests.post('http://localhost:8080/sim', json={
    'id': 'product123',
    'top_k': 10,
    'mode': 'fusion'
})

data = response.json()
print(f"{data['count']} 件の推薦が見つかりました")
for item in data['results']:
    print(f"  {item['id']}: {item['score']}")
```

### 完全な例: ECサイトの推薦

```python
import requests

BASE_URL = 'http://localhost:8080'

# 1. 商品埋め込みを登録（ML モデルから）
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

# 2. ユーザー行動を追跡
events = [
    ('user_alice', 'laptop_001', 100),  # 購入
    ('user_alice', 'laptop_002', 80),   # 閲覧
    ('user_bob', 'laptop_001', 100),    # 購入
    ('user_bob', 'phone_001', 90)       # 閲覧
]

for ctx, product_id, score in events:
    requests.post(f'{BASE_URL}/event', json={
        'ctx': ctx,
        'id': product_id,
        'score': score
    })

# 3. コンテンツベース推薦を取得
response = requests.post(f'{BASE_URL}/sim', json={
    'id': 'laptop_001',
    'top_k': 5,
    'mode': 'vectors'
})
print("コンテンツベース推薦:", response.json()['results'])

# 4. 行動ベース推薦を取得
response = requests.post(f'{BASE_URL}/sim', json={
    'id': 'laptop_001',
    'top_k': 5,
    'mode': 'events'
})
print("行動ベース推薦:", response.json()['results'])

# 5. ハイブリッド推薦を取得（fusion）
response = requests.post(f'{BASE_URL}/sim', json={
    'id': 'laptop_001',
    'top_k': 5,
    'mode': 'fusion'
})
print("ハイブリッド推薦:", response.json()['results'])
```

## パフォーマンス考慮事項

- **コネクションプーリング**: HTTP keep-alive でパフォーマンス向上
- **キャッシング**: `/info` でキャッシュメトリクスを確認してキャッシュが機能しているか確認
- **ネットワークセキュリティ**: `network.allow_cidrs` でアクセスを制限
- **リバースプロキシ**: TLS/認証のため、nvecd の前に nginx/HAProxy を配置することを検討

## エラーハンドリング

すべてのエラーレスポンスは次の形式に従います：

```json
{
  "error": "エラーメッセージの説明"
}
```

**HTTP ステータスコード:**

| コード | 説明 |
|-------|------|
| 200 | 成功 |
| 400 | Bad Request（無効な入力） |
| 404 | Not Found（ベクトル/スナップショットが存在しない） |
| 500 | Internal Server Error |
| 503 | Service Unavailable（サーバー読み込み中） |

## モニタリング

HTTP API はモニタリング用に複数のエンドポイントを提供します：

- **ヘルスチェック**: `GET /health` - シンプルなヘルスチェック
- **Liveness Probe**: `GET /health/live` - Kubernetes liveness
- **Readiness Probe**: `GET /health/ready` - Kubernetes readiness
- **詳細メトリクス**: `GET /health/detail` - 完全な統計情報

### Kubernetes デプロイメント

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

### Prometheus メトリクス（将来対応予定）

現在は JSON メトリクス用に `/info` エンドポイントを使用してください。Prometheus 形式は将来のリリースで対応予定です。
