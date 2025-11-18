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

## API Endpoints

All responses are in JSON format with `Content-Type: application/json`.

### POST /event

Track user behavior (e.g., product views, purchases, interactions).

**Request:**

```http
POST /event HTTP/1.1
Content-Type: application/json

{
  "ctx": "user_alice",
  "id": "product123",
  "score": 100
}
```

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ctx` | string | Yes | Context ID (e.g., user ID, session ID) |
| `id` | string | Yes | Item ID (e.g., product ID, article ID) |
| `score` | integer | Yes | Event score (0-100, e.g., 100=purchase, 80=view) |

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

**Request:**

```http
POST /vecset HTTP/1.1
Content-Type: application/json

{
  "id": "product123",
  "vector": [0.1, 0.2, 0.3, 0.4, 0.5]
}
```

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `id` | string | Yes | Item ID |
| `vector` | array of floats | Yes | Embedding vector (dimension must match existing vectors) |

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
| `mode` | string | No | "vectors" | Search mode: "vectors", "events", or "fusion" |

**Search Modes:**

| Mode | Description |
|------|-------------|
| `vectors` | Content-based similarity (uses vector embeddings) |
| `events` | Behavior-based similarity (co-occurrence from events) |
| `fusion` | Hybrid: combines vectors + events |

**Response (200 OK):**

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

**Response Fields:**

| Field | Description |
|-------|-------------|
| `count` | Number of results returned |
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
  "top_k": 10,
  "mode": "vectors"
}
```

**Request Body Parameters:**

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `vector` | array of floats | Yes | - | Query vector |
| `top_k` | integer | No | 10 | Number of results to return |
| `mode` | string | No | "vectors" | Search mode (typically "vectors") |

**Response (200 OK):**

Same format as `/sim` endpoint.

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

**Response Fields:**

| Category | Field | Description |
|----------|-------|-------------|
| **Server** | `server` | Server name (nvecd) |
| | `version` | Server version |
| | `uptime_seconds` | Server uptime in seconds |
| | `total_requests` | Total requests processed |
| | `connections` | Current active connections |
| **Memory** | `vector_store_bytes` | Memory used by vector store |
| | `event_store_bytes` | Memory used by event store |
| | `co_occurrence_index_bytes` | Memory used by co-occurrence index |
| | `cache_bytes` | Memory used by cache |
| | `total_bytes` | Total memory usage |
| **Vector Store** | `total_vectors` | Number of vectors stored |
| | `dimension` | Vector dimension |
| **Event Store** | `total_contexts` | Number of contexts (users/sessions) |
| | `total_events` | Total events tracked |
| **Cache** | `hit_rate` | Cache hit rate (0.0-1.0) |
| | `time_saved_ms` | Total latency saved by cache |

This endpoint is suitable for monitoring tools and health checks.

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
  "status": "ok"
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
  "status": "ok"
}
```

**Response (503 Service Unavailable):**

```json
{
  "status": "loading"
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

### GET /config

Current server configuration summary (sensitive values omitted).

**Request:**

```http
GET /config HTTP/1.1
```

**Response (200 OK):**

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

Save server snapshot to disk.

**Request:**

```http
POST /dump/save HTTP/1.1
Content-Type: application/json

{
  "filepath": "/backup/snapshot-20250118.dmp"
}
```

**Request Body Parameters:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `filepath` | string | No | Snapshot file path (auto-generated if omitted) |

**Response (200 OK):**

```json
{
  "status": "ok",
  "filepath": "/backup/snapshot-20250118.dmp",
  "size_bytes": 314572800
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

**Response (200 OK):**

```json
{
  "status": "ok",
  "vectors_loaded": 100000,
  "events_loaded": 1000000
}
```

**Error Response (404 Not Found):**

```json
{
  "error": "Snapshot file not found"
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

**Response (200 OK):**

```json
{
  "status": "ok",
  "checksum_valid": true
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

**Response (200 OK):**

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
