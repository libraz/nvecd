# HTTP API Guide

Nvecd provides a RESTful JSON API for easy integration with web applications and HTTP clients.

## Configuration

Enable the HTTP server in your `config.yaml`:

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

**Security Note**: HTTP server binds to loopback by default. If you must expose it publicly, configure `network.allow_cidrs` to restrict access to trusted IP ranges, and use a reverse proxy with TLS/authentication.

## Authentication

When `security.requirepass` is set, the HTTP server enforces authentication on
all mutating and administrative endpoints, mirroring the TCP `AUTH` gate. Read-only
endpoints (health, `/info`, `/config`, `/metrics`, `/cache/stats`, `/sim`, `/simv`,
`/dump/status`) remain open.

Provide the password via the `Authorization` request header, using either scheme:

- `Authorization: Bearer <password>`
- `Authorization: Basic base64(<user>:<password>)` — the username is ignored; only
  the password is compared (matching TCP `AUTH`).

Gated endpoints: `POST /event`, `POST /vecset`, `POST /metaset`,
`POST /cache/clear`, `POST /cache/enable`, `POST /cache/disable`,
`POST /dump/save`, `POST /dump/load`, `POST /dump/verify`, `POST /dump/info`.

Requests without a valid credential receive `401 Unauthorized`:

```json
{
  "error": "Authentication required"
}
```

When `security.requirepass` is empty (the default) no authentication is required.

## API Endpoints

All responses are in JSON format with `Content-Type: application/json`.

### POST /event

Track user behavior (e.g., product views, purchases, interactions).

**Request:**

This endpoint requires authentication when `security.requirepass` is set.

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

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ctx` | string | Yes | Context ID (e.g., user ID, session ID) |
| `id` | string | Yes | Item ID (e.g., product ID, article ID) |
| `type` | string | Yes | Event type: `ADD`, `SET`, or `DEL` |
| `score` | integer | For `ADD`/`SET` | Event score (0-100, e.g., 100=purchase, 80=view) |
| `timestamp` | integer | No | Event timestamp (epoch seconds) |

**Response (200 OK):**

```json
{
  "status": "ok"
}
```

**Error Response (400 Bad Request):**

```json
{
  "error": "Missing required field: ctx"
}
```

### POST /vecset

Register or update a vector embedding for an item.

This endpoint requires authentication when `security.requirepass` is set.

**Request:**

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

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Item ID |
| `vector` | array of floats | Yes | Embedding vector (dimension must match existing vectors) |
| `metadata` | object | No | Metadata used by `filter=` queries. Values may be string, integer, float, or bool. |

**Response (200 OK):**

```json
{
  "status": "ok"
}
```

**Error Response (400 Bad Request):**

```json
{
  "error": "Dimension mismatch: expected 768, got 512"
}
```

### POST /metaset

Set (replace) the metadata for an existing item. Metadata is keyed by item ID and
is used by `filter=` queries. The target item must already have a vector registered
via `/vecset`; otherwise the request returns `404 Not Found`.

This endpoint requires authentication when `security.requirepass` is set.

**Request:**

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

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Item ID (a vector with this ID must already exist) |
| `metadata` | object | Yes | Metadata map. Values may be string, integer, float, or bool. |

**Response (200 OK):**

```json
{
  "status": "ok"
}
```

**Error Response (404 Not Found):**

```json
{
  "error": "Vector not found for metadata: product123"
}
```

### POST /sim

Search for similar items by ID.

**Request:**

```http
POST /sim HTTP/1.1
Content-Type: application/json

{
  "id": "product123",
  "top_k": 10,
  "mode": "fusion"
}
```

**Request Body Parameters:**

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `id` | string | Yes | - | Query item ID |
| `top_k` | integer | No | 10 | Number of results to return |
| `mode` | string | No | "fusion" | Search mode: "vectors", "events", or "fusion" |
| `filter` | string | No | - | Metadata filter (e.g., "category:electronics,type:laptop") |
| `min_score` | float | No | 0.0 | Minimum score threshold (results below this are excluded) |
| `adaptive` | boolean | No | false | Enable adaptive fusion (auto-adjusts weights by data density) |

**Search Modes:**

| Mode | Description |
|------|-------------|
| `vectors` | Content-based similarity (uses vector embeddings) |
| `events` | Behavior-based similarity (co-occurrence from events) |
| `fusion` | Hybrid: combines vectors + events |

**Response (200 OK):**

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

**Response Fields:**

| Field | Description |
|-------|-------------|
| `status` | `"ok"` on success |
| `count` | Number of results returned |
| `mode` | The search mode that was used |
| `results` | Array of similar items (sorted by score, descending) |
| `results[].id` | Item ID |
| `results[].score` | Similarity score (0.0-1.0, higher = more similar) |

**Error Response (404 Not Found):**

```json
{
  "error": "Vector not found: product123"
}
```

### POST /simv

Search for similar items by raw vector query.

**Request:**

```http
POST /simv HTTP/1.1
Content-Type: application/json

{
  "vector": [0.1, 0.2, 0.3, 0.4, 0.5],
  "top_k": 10
}
```

**Request Body Parameters:**

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `vector` | array of floats | Yes | - | Query vector |
| `top_k` | integer | No | 10 | Number of results to return |
| `filter` | string | No | - | Metadata filter (e.g., "type:article") |
| `min_score` | float | No | 0.0 | Minimum score threshold |

`/simv` always performs a vector-space search; it has no `mode` parameter.

**Response (200 OK):**

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

**Use Case:**

- Search by user query embedding (e.g., "red running shoes" → vector)
- Find items matching a computed vector (e.g., average of liked items)

### GET /info

Server statistics and monitoring information (Redis-style).

**Request:**

```http
GET /info HTTP/1.1
```

**Response (200 OK):**

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
    "memory_health": "ok"
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

**Response Fields:**

| Category | Field | Description |
|----------|-------|-------------|
| **Server** | `server` | Server name (nvecd) |
| | `version` | Server version |
| | `uptime_seconds` | Server uptime in seconds |
| | `total_requests` | Total requests processed |
| | `total_commands_processed` | Total commands processed |
| | `failed_commands` | Number of commands that returned an error |
| **Memory** | `used_memory_bytes` | Total tracked store memory (bytes) |
| | `used_memory_events` | Human-readable event store memory |
| | `used_memory_vectors` | Human-readable vector store memory |
| | `used_memory_co_occurrence` | Human-readable co-occurrence index memory |
| | `peak_memory_bytes` | Peak process RSS (bytes) |
| | `memory_health` | Memory health status |
| **Stores** | `stores.vector_store.vectors` | Number of vectors stored |
| | `stores.vector_store.dimension` | Vector dimension |
| | `stores.event_store.contexts` | Number of contexts (users/sessions) |
| | `stores.event_store.total_events` | Total events tracked |
| | `stores.co_index.tracked_ids` | Number of IDs tracked by the co-occurrence index |
| **Cache** | `cache.enabled` | Whether the query cache is enabled |
| | `cache.hit_rate` | Cache hit rate (0.0-1.0) |
| | `cache.current_memory_bytes` | Current cache memory usage (bytes) |
| | `cache.time_saved_ms` | Total latency saved by cache |

This endpoint is suitable for monitoring tools and health checks. The `memory`
object also includes system-level fields (`total_system_memory`,
`available_system_memory`, `system_memory_usage_ratio`) when the platform
exposes them.

### GET /health

Simple health check endpoint for load balancers.

**Request:**

```http
GET /health HTTP/1.1
```

**Response (200 OK):**

```json
{
  "status": "ok"
}
```

### GET /health/live

Kubernetes liveness probe (always returns 200 if server is running).

**Request:**

```http
GET /health/live HTTP/1.1
```

**Response (200 OK):**

```json
{
  "status": "alive",
  "timestamp": 1705564800
}
```

### GET /health/ready

Kubernetes readiness probe (returns 503 if server is loading snapshot).

**Request:**

```http
GET /health/ready HTTP/1.1
```

**Response (200 OK):**

```json
{
  "status": "ready",
  "loading": false
}
```

**Response (503 Service Unavailable):**

```json
{
  "status": "not_ready",
  "loading": true,
  "reason": "Server is loading"
}
```

### GET /health/detail

Detailed health information with metrics (same as `/info`).

**Request:**

```http
GET /health/detail HTTP/1.1
```

**Response (200 OK):**

Same format as `/info` endpoint.

### GET /metrics

Get server metrics in Prometheus text exposition format
(`Content-Type: text/plain; version=0.0.4`).

**Request:**

```http
GET /metrics HTTP/1.1
```

**Response (200 OK):**

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

Cache, vector, event, and context gauges are also emitted (e.g.
`nvecd_cache_hit_rate`, `nvecd_vectors_total`, `nvecd_events_total`).

### GET /config

Current server configuration summary (sensitive values omitted).

**CORS**: When `api.http.enable_cors` is `true`, the server adds `Access-Control-Allow-Origin` headers and handles OPTIONS preflight requests.

**Request:**

```http
GET /config HTTP/1.1
```

**Response (200 OK):**

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

Bind addresses and ports are intentionally omitted from `/config` for security;
use `CONFIG SHOW` over TCP for full details.

### POST /dump/save

Save server snapshot to disk.

**Request:**

```http
POST /dump/save HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

This endpoint requires authentication when `security.requirepass` is set.

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `filepath` | string | No | Snapshot file path (auto-generated if omitted) |

**Response (200 OK):**

```json
{
  "status": "ok",
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**Error Response (5xx):**

If the save fails, the endpoint returns a non-2xx status (e.g. `500`) with the
real error message; it never reports `status: ok` for a failed save.

```json
{
  "error": "Failed to save snapshot to /backup/snapshot-20250118.dmp: ..."
}
```

### POST /dump/load

Load server snapshot from disk.

**Request:**

```http
POST /dump/load HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

This endpoint requires authentication when `security.requirepass` is set.

**Response (200 OK):**

```json
{
  "status": "ok",
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**Error Response (404 Not Found):**

A missing snapshot file maps to `404`; other failures map to `400`/`500` based on
the underlying error.

```json
{
  "error": "Failed to load snapshot from /backup/snapshot-20250118.dmp: ..."
}
```

### POST /dump/verify

Verify snapshot file integrity.

**Request:**

```http
POST /dump/verify HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

This endpoint requires authentication when `security.requirepass` is set.

**Response (200 OK):**

```json
{
  "status": "ok",
  "filepath": "/backup/snapshot-20250118.dmp",
  "valid": true
}
```

**Response (verification failed):**

When integrity verification fails, the endpoint returns a non-2xx status with
`valid: false` and the real error message (never a hardcoded `valid: true`):

```json
{
  "status": "error",
  "filepath": "/backup/snapshot-20250118.dmp",
  "valid": false,
  "error": "Snapshot verification failed for ...: CRC mismatch"
}
```

### POST /dump/info

Get snapshot file metadata.

**Request:**

```http
POST /dump/info HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

This endpoint requires authentication when `security.requirepass` is set.

**Response (200 OK):**

```json
{
  "status": "ok",
  "filepath": "/backup/snapshot-20250118.dmp",
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

Get the status of background snapshot operations.

**Request:**

```http
GET /dump/status HTTP/1.1
```

**Response (200 OK):**

```json
{"status": "ok", "data": "IDLE"}
```

### GET /cache/stats

Get cache statistics (hit rate, entries, memory usage).

**Request:**

```http
GET /cache/stats HTTP/1.1
```

**Response (200 OK):**

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

Clear the similarity cache.

This endpoint requires authentication when `security.requirepass` is set
(as do `/cache/enable` and `/cache/disable`).

**Request:**

```http
POST /cache/clear HTTP/1.1
Content-Type: application/json

{
  "scope": "all"
}
```

**Response (200 OK):**

```json
{
  "status": "ok",
  "scope": "all",
  "entries_removed": 2450
}
```

### POST /cache/enable

Enable the similarity cache.

**Request:** No body required.

**Response (200 OK):**

```json
{"status": "ok", "message": "Cache enabled"}
```

### POST /cache/disable

Disable the similarity cache.

**Request:** No body required.

**Response (200 OK):**

```json
{"status": "ok", "message": "Cache disabled"}
```

### POST /debug/on

Enable debug mode (shows detailed query timing in logs).

**Request:**

```http
POST /debug/on HTTP/1.1
```

**Response (200 OK):**

```json
{
  "status": "ok"
}
```

### POST /debug/off

Disable debug mode.

**Request:**

```http
POST /debug/off HTTP/1.1
```

**Response (200 OK):**

```json
{
  "status": "ok"
}
```

## CORS Support

Set `api.http.enable_cors: true` to enable CORS headers for browser clients, then specify the trusted origin via `api.http.cors_allow_origin`. Keep CORS disabled when the API is not accessed directly from browsers.

**CORS Headers:**

```
Access-Control-Allow-Origin: https://app.example.com
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

## Usage Examples

### cURL

**Track event:**

```bash
curl -X POST http://localhost:8080/event \
  -H "Content-Type: application/json" \
  -d '{
    "ctx": "user_alice",
    "id": "product123",
    "score": 100
  }'
```

**Register vector:**

```bash
curl -X POST http://localhost:8080/vecset \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "vector": [0.1, 0.2, 0.3, 0.4]
  }'
```

**Search similar items:**

```bash
curl -X POST http://localhost:8080/sim \
  -H "Content-Type: application/json" \
  -d '{
    "id": "product123",
    "top_k": 10,
    "mode": "fusion"
  }'
```

**Search with filter and min_score:**

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

**Health check:**

```bash
curl http://localhost:8080/health
```

**Server info:**

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

### Complete Example: E-commerce Recommendations

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

## Performance Considerations

- **Connection Pooling**: Use HTTP keep-alive for better performance
- **Caching**: Check `/info` cache metrics to verify cache is working
- **Network Security**: Use `network.allow_cidrs` to restrict access
- **Reverse Proxy**: Consider nginx/HAProxy in front of nvecd for TLS/auth

## Error Handling

All error responses follow this format:

```json
{
  "error": "Error message description"
}
```

**HTTP Status Codes:**

| Code | Description |
|------|-------------|
| 200 | Success |
| 400 | Bad Request (invalid input) |
| 401 | Unauthorized (missing/invalid credential on a gated endpoint) |
| 403 | Forbidden (blocked by `network.allow_cidrs`) |
| 404 | Not Found (vector/snapshot doesn't exist) |
| 500 | Internal Server Error |
| 503 | Service Unavailable (server loading) |

## Monitoring

The HTTP API provides multiple endpoints for monitoring:

- **Health Check**: `GET /health` - Simple health check
- **Liveness Probe**: `GET /health/live` - Kubernetes liveness
- **Readiness Probe**: `GET /health/ready` - Kubernetes readiness
- **Detailed Metrics**: `GET /health/detail` - Full statistics

### Kubernetes Deployment

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

### Prometheus Metrics (Future)

Currently, use `/info` endpoint for JSON metrics. Prometheus format is planned for future releases.
