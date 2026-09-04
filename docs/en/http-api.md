# HTTP API

The HTTP server exposes the same operations as the [TCP protocol](./protocol.md) as JSON routes, plus health probes and a Prometheus scrape endpoint. It is disabled by default and is enabled with `api.http.enable`.

## Enabling the server

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

The listener applies `network.allow_cidrs` before routing, so an address outside the list is answered `403` regardless of the route. An empty list denies every address. Request bodies are capped at `performance.max_query_length`, concurrent connections at `performance.max_connections` and `performance.max_connections_per_ip`, and, when `api.rate_limiting.enable` is set, requests are token-bucketed per client address.

All request bodies are JSON. The server does not require a `Content-Type` header, and every JSON response is served as `application/json` except `/metrics`.

## Route table

| Method | Path | TCP counterpart | Privilege |
|---|---|---|---|
| `POST` | `/event` | `EVENT` | write |
| `POST` | `/vecset` | `VECSET` | write |
| `DELETE` | `/vecset` | `VECDEL` | write |
| `POST` | `/metaset` | `METASET` | write |
| `POST` | `/sim` | `SIM` | read |
| `POST` | `/simv` | `SIMV` | read |
| `GET` | `/info` | `INFO` | read |
| `GET` | `/config` | `CONFIG SHOW` | read |
| `GET` | `/cache/stats` | `CACHE STATS` | read |
| `POST` | `/cache/clear` | `CACHE CLEAR` | write |
| `POST` | `/cache/enable` | `CACHE ENABLE` | write |
| `POST` | `/cache/disable` | `CACHE DISABLE` | write |
| `POST` | `/dump/save` | `DUMP SAVE` | admin |
| `POST` | `/dump/load` | `DUMP LOAD` | admin |
| `POST` | `/dump/verify` | `DUMP VERIFY` | admin |
| `POST` | `/dump/info` | `DUMP INFO` | admin |
| `GET` | `/dump/status` | `DUMP STATUS` | admin |
| `POST` | `/debug/on` | `DEBUG ON` | read |
| `POST` | `/debug/off` | `DEBUG OFF` | read |
| `GET` | `/health` | — | none |
| `GET` | `/health/live` | — | none |
| `GET` | `/health/ready` | — | none |
| `GET` | `/health/detail` | — | none |
| `GET` | `/metrics` | — | none |

A path that is not in this table, or a registered path reached with the wrong method, is answered `404`.

Each route declares the TCP command it is the HTTP form of, and that single declaration decides both its privilege and which per-command counter the request lands in, so the two surfaces cannot drift apart on either.

## Authentication

When `security.requirepass` is empty, every route is open. When it is set, the write and admin routes require credentials on each request; the read routes and the probe endpoints stay open, matching the TCP privilege split.

These routes are gated: `POST /event`, `POST /vecset`, `DELETE /vecset`, `POST /metaset`, `POST /cache/clear`, `POST /cache/enable`, `POST /cache/disable`, `POST /dump/save`, `POST /dump/load`, `POST /dump/verify`, `POST /dump/info`, `GET /dump/status`.

Two credential forms are accepted:

```bash
curl -X POST http://127.0.0.1:8080/vecset \
  -H 'Authorization: Bearer s3cret' \
  -d '{"id":"item1","vector":[0.1,0.2,0.3,0.4]}'

curl -X POST http://127.0.0.1:8080/vecset \
  -u ignored:s3cret \
  -d '{"id":"item1","vector":[0.1,0.2,0.3,0.4]}'
```

With `Basic`, the username is ignored and only the password is compared, mirroring TCP `AUTH`, which validates the password alone. Both comparisons are constant-time. A missing or wrong credential is answered:

```json
{"error":"Authentication required"}
```

with status `401`. The check runs before the handler reads any state or assembles any body.

## Status codes

| Status | Condition |
|---|---|
| `200` | success |
| `204` | CORS preflight (`OPTIONS`) |
| `400` | malformed JSON, missing or mistyped field, invalid `top_k`, dimension mismatch, invalid filter, invalid event score, snapshot path traversal, unsupported cache scope |
| `401` | credentials missing or wrong on a gated route |
| `403` | source address outside `network.allow_cidrs`, or a permission-denied error from a handler |
| `404` | unknown route or method; unknown item ID; snapshot or configuration file not found |
| `410` | `/debug/on` and `/debug/off` |
| `429` | rate limit exceeded |
| `500` | any other handler error, including snapshot read and integrity failures |
| `503` | server is loading or read-only; a write the WAL could not accept |

Handler errors are mapped from the typed error code rather than sniffed from a message, so the same failure produces the same status on every route. Domain errors map as follows: item and file not-found conditions to `404`; argument, parsing, dimension, `top_k` and event-score errors to `400`; permission-denied to `403`; WAL write, rotation and not-open errors to `503`; everything else to `500`.

Two families of client mistake land on `500` rather than `400` because their error code is not in that map. A `POST /event` whose `ctx` or `id` is empty, or carries whitespace or a control character, is answered `500` with `Context cannot be empty`, `ID cannot be empty`, or `… must not contain whitespace or control characters`. A `POST /dump/load`, `/dump/verify` or `/dump/info` naming a file that does not exist is likewise `500`, because the underlying failure is a storage open error rather than a not-found code. Both are request defects; a client that classifies purely on status will read them as server faults.

Every error body is a single-field object:

```json
{"error":"Vector not found: zz"}
```

`POST /dump/verify` is the exception: a failed verification returns a full body rather than only `error` (see below).

## Write routes

### `POST /event`

```json
{"ctx": "user_alice", "id": "item1", "type": "ADD", "score": 100, "timestamp": 1730000000}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `ctx` | string | yes | context ID |
| `id` | string | yes | item ID |
| `type` | string | yes | `ADD`, `SET` or `DEL`, upper or lower case |
| `score` | integer | for `ADD` and `SET` | must be an integer, not a float; range 0–100 |
| `timestamp` | unsigned integer | no | epoch seconds; the server's clock is used when omitted |

```json
{"status":"ok"}
```

A float `score` is rejected rather than truncated, so the integer contract matches TCP.

### `POST /vecset`

```json
{"id": "item1", "vector": [0.1, 0.2, 0.3, 0.4], "metadata": {"category": "books", "price": 19, "active": true}}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | item ID |
| `vector` | array of numbers | yes | every element must be finite and within float range |
| `metadata` | object | no | values may be string, integer, float or boolean; keys must not be empty |

```json
{"dimension":4,"status":"ok"}
```

`dimension` echoes the length of the stored vector. Unlike TCP `VECSET`, this route can attach metadata in the same request.

### `DELETE /vecset`

```json
{"id": "item1"}
```

```json
{"status":"ok"}
```

The ID is read from a JSON body, not from the path or query string, so a client library that drops bodies on `DELETE` has to be configured to send one. An empty or missing `id` is `400`; an unknown ID is `404` with `Vector not found: <id>`.

### `POST /metaset`

```json
{"id": "item1", "metadata": {"category": "books", "price": 19, "active": true}}
```

```json
{"status":"ok"}
```

`metadata` is a structured JSON object here, not the `key:value,key:value` string that TCP `METASET` and the client library take. The item must already have a vector; otherwise the response is `404` with `Vector not found for metadata: <id>`.

## Search routes

### `POST /sim`

```json
{"id": "item1", "top_k": 3, "mode": "vectors", "filter": "category:books", "min_score": 0.1, "adaptive": true}
```

| Field | Type | Required | Default |
|---|---|---|---|
| `id` | string | yes | — |
| `top_k` | integer | no | `similarity.default_top_k` |
| `mode` | string | no | `fusion`; one of `events`, `vectors`, `fusion` |
| `filter` | string | no | no filter |
| `min_score` | finite number | no | `0.0` |
| `adaptive` | boolean | no | the server's `similarity.adaptive_fusion` |

```json
{"count":1,"mode":"vectors","results":[{"id":"item3","score":0.9333}],"status":"ok"}
```

`count` is the number of entries in `results` after `min_score` has been applied. Scores are rounded to four decimal places, the same precision the TCP surface renders.

`top_k` must be positive and must not exceed `similarity.max_top_k`; both violations are `400`. `filter` uses the same grammar as the TCP `filter=` option — `=`, `:`, `!=`, `>`, `<`, `>=`, `<=` and `in(a|b|c)` — documented in [protocol.md](./protocol.md). An `id` with no vector is `404` with `Query vector not found: <id>`.

### `POST /simv`

```json
{"vector": [0.1, 0.2, 0.3, 0.4], "top_k": 3, "filter": "active:true", "min_score": 0.1}
```

| Field | Type | Required | Default |
|---|---|---|---|
| `vector` | array of numbers | yes | — |
| `top_k` | integer | no | `similarity.default_top_k` |
| `filter` | string | no | no filter |
| `min_score` | finite number | no | `0.0` |

```json
{"count":2,"dimension":4,"results":[{"id":"item1","score":1.0},{"id":"item3","score":0.9333}],"status":"ok"}
```

`dimension` echoes the query vector's length. There is no `mode` and no `adaptive`: this route always searches vectors.

## Introspection routes

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

`total_requests` and `total_commands_processed` carry the same counter under two names. `memory.used_memory_bytes` is the sum of the three store totals, which is a different figure from TCP `INFO`, where `used_memory_bytes` counts only the vector matrix. The `process_rss*` group appears only when process memory information is readable, and the `*_system_memory*` group only when system memory information is. A store's entry under `stores` is present only when that store exists. When no cache is attached, `cache` is `{"enabled": false}` alone.

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

This is a deliberately narrow summary: bind addresses, ports, the password and the CIDR list itself are not exposed, and `allow_cidrs_configured` reports only whether the list is non-empty. `CONFIG SHOW` over TCP prints the full running configuration.

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

`ttl_seconds` and `min_query_cost_ms` reflect the live values, which the `cache.ttl_seconds` and `cache.min_query_cost_ms` runtime variables can change. When no cache controller is wired the route answers `500`.

The field set is close to TCP `CACHE STATS` but not identical: the TCP block additionally reports `ttl_expirations`, and it names two fields differently (`cache_enabled` and `cache_entries` here are `enabled` and `current_entries`).

## Cache management

### `POST /cache/clear`

```json
{"scope": "all"}
```

An empty body is treated as `{"scope": "all"}`.

```json
{"entries_removed":0,"scope":"all","status":"ok"}
```

`entries_removed` is the entry count taken before the clear. Any scope other than `all` is `400`:

```json
{"error":"Invalid scope. Only 'all' is supported currently."}
```

A body that is not a JSON object is also `400`.

### `POST /cache/enable` and `POST /cache/disable`

Neither takes a body.

```json
{"message":"Cache enabled","status":"ok"}
```

```json
{"message":"Cache disabled","status":"ok"}
```

Both set the `cache.enabled` runtime variable, so the change survives until it is set again or the server restarts.

## Snapshot management

### `POST /dump/save`

```json
{"filepath": "nvecd.nvec"}
```

`filepath` is optional; an empty body or an omitted field uses `snapshot.default_filename`. The path is resolved inside `snapshot.dir` and one that escapes it is `400`.

```json
{"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}
```

`filepath` is the resolved absolute path the server used. Under the default `snapshot.mode: fork`, the response arrives as soon as the writer child exists and the file is not readable yet — unlike TCP `DUMP SAVE`, whose reply keyword distinguishes the two modes, this body is identical either way. Poll `GET /dump/status` before reading or copying the file.

### `POST /dump/load`

```json
{"filepath": "nvecd.nvec"}
```

`filepath` is required and must be a string.

```json
{"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}
```

A missing file is `500`, with the underlying open failure in the message. A path that escapes `snapshot.dir` is `400`.

### `POST /dump/verify`

```json
{"filepath": "nvecd.nvec"}
```

```json
{"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok","valid":true}
```

A failed verification does not collapse to a bare `error` object; it reports the same shape with `valid` false, so a caller reads one field either way:

```json
{
  "status": "error",
  "filepath": "nope.nvec",
  "valid": false,
  "error": "Snapshot verification failed for /var/lib/nvecd/snapshots/nope.nvec: ..."
}
```

The status is the mapped error status, `500` for an unreadable or corrupt file.

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

Every value under `info` is a string, because the block is parsed out of the shared handler's text output. `filepath` here echoes the request rather than the resolved path.

### `GET /dump/status`

Takes no body. `data` carries the state of the background snapshot writer.

| `data` | Extra fields |
|---|---|
| `IDLE` | none |
| `IN_PROGRESS` | `filepath` |
| `COMPLETED` | `filepath` |
| `FAILED` | `filepath`, `error_message` |

```json
{"data":"COMPLETED","filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}
```

```json
{"data":"IDLE","status":"ok"}
```

A server configured with `snapshot.mode: lock` has no background writer and always answers `IDLE`. The TCP `DUMP STATUS` block reports the same states in lower case and carries `pid`, `start_time` and `end_time`, which this route does not.

## Debug routes

`POST /debug/on` and `POST /debug/off` are registered so the route table matches the command set, and both refuse:

```json
{"error":"HTTP debug mode is not supported; use DEBUG ON on a persistent TCP connection"}
```

with status `410`. Debug mode is a property of one connection, which a request-scoped HTTP call has no equivalent of.

## Health probes

Four probes, none gated and none counted as commands.

`GET /health` — a plain liveness answer, always `200`:

```json
{"status":"ok","timestamp":1788427653}
```

`GET /health/live` — the orchestrator liveness probe, always `200` while the process runs:

```json
{"status":"alive","timestamp":1788427653}
```

`GET /health/ready` — the readiness probe. `200` when the server is not loading a snapshot:

```json
{"loading":false,"status":"ready","timestamp":1788427653}
```

and `503` while it is:

```json
{"loading":true,"reason":"Server is loading","status":"not_ready","timestamp":1788427653}
```

`GET /health/detail` — per-component status, always `200`. This is not the same shape as `/info`:

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

The top-level `status` is `degraded` while a snapshot is loading and `healthy` otherwise; `components.server.status` is `loading` or `ready` for the same condition. A component whose store is not wired is omitted from `components`.

## Metrics

`GET /metrics` serves the Prometheus text exposition format with content type `text/plain; version=0.0.4; charset=utf-8`.

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

| Metric | Type | Meaning |
|---|---|---|
| `nvecd_uptime_seconds` | counter | seconds since start |
| `nvecd_commands_total` | counter | commands processed; per-command series carry `command="event"`, `"vecset"` and `"sim"`, and an unlabelled series carries the total across all commands |
| `nvecd_memory_bytes` | gauge | event store, vector store and co-occurrence index combined |
| `nvecd_vectors_total` | gauge | vectors stored |
| `nvecd_events_total` | gauge | events stored |
| `nvecd_contexts_total` | gauge | active contexts |
| `nvecd_cache_queries_total` | counter | cache lookups |
| `nvecd_cache_hits_total` | counter | cache hits |
| `nvecd_cache_misses_total` | counter | cache misses |
| `nvecd_cache_hit_rate` | gauge | hit ratio |
| `nvecd_cache_entries` | gauge | entries currently held |
| `nvecd_cache_memory_bytes` | gauge | cache memory in use |

The three store metrics appear only when their store is wired, and the six cache metrics only when a cache is attached — a scrape taken with the cache disabled has no `nvecd_cache_*` series at all, which a dashboard has to tolerate.

The unlabelled `nvecd_commands_total` series shares a metric name with the labelled ones. Some scrape configurations reject that mixture; a relabelling rule that drops the unlabelled series, or a recording rule that sums the labelled ones, avoids it.

## CORS

CORS headers are emitted only when `api.http.enable_cors` is true. `api.http.cors_allow_origin` supplies the value of `Access-Control-Allow-Origin`; when it is empty the header is omitted entirely rather than sent as `null`, because `null` names the origin used by sandboxed iframes and `file://` pages. The remaining headers are still set, so a deployment can front the server with a proxy that injects the origin.

```bash
$ curl -i -X OPTIONS http://127.0.0.1:8080/sim -H 'Origin: https://example.com'
HTTP/1.1 204 No Content
Access-Control-Allow-Origin: https://example.com
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, Authorization
Content-Length: 0
```

With CORS disabled, `OPTIONS` is not registered and returns `404`.

## Worked examples

Register two items with metadata, record an event, then search:

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

Search by a query vector and drop weak matches:

```bash
curl -X POST http://127.0.0.1:8080/simv \
  -d '{"vector":[0.1,0.2,0.3,0.4],"top_k":5,"min_score":0.99}'
# {"count":2,"dimension":4,"results":[{"id":"item1","score":1.0},{"id":"item2","score":0.994}],"status":"ok"}
```

Take a snapshot and wait for the background writer to finish:

```bash
curl -X POST http://127.0.0.1:8080/dump/save -H 'Authorization: Bearer s3cret' -d '{}'
# {"filepath":"/var/lib/nvecd/snapshots/nvecd.nvec","status":"ok"}

until curl -s -H 'Authorization: Bearer s3cret' http://127.0.0.1:8080/dump/status \
      | grep -q '"data":"COMPLETED"'; do sleep 1; done
```

Scrape configuration for Prometheus:

```yaml
scrape_configs:
  - job_name: nvecd
    static_configs:
      - targets: ["127.0.0.1:8080"]
```
