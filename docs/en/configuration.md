# Configuration Guide

This guide explains all configuration options available in Nvecd.

The option tables below and `examples/config.yaml` are generated from
`src/config/config-schema.json`, which is the authority for every type, default
and range stated here. Edit the schema and run
`python3 support/generate_config_docs.py`; do not edit the tables by hand.

## Configuration File

Nvecd uses YAML format for configuration. The example configuration file is located at `examples/config.yaml`.

## Basic Usage

```bash
# Start nvecd with configuration file
nvecd -c /path/to/config.yaml
```

---

## Configuration Sections

### Event Store Configuration

Controls event tracking and co-occurrence index behavior.

```yaml
events:
  ctx_buffer_size: 50          # Ring buffer size per context
  max_contexts: 0              # Active contexts retained (0 = unlimited)
  max_neighbors_per_item: 0    # Co-occurrence edges retained per item (0 = unlimited)
  min_support: 0.0             # Prune edges below this score (0 = disabled)
  decay_interval_sec: 3600     # Decay interval (seconds)
  decay_alpha: 0.99            # Decay factor (0.0 - 1.0)
  dedup_window_sec: 60         # Deduplication time window (seconds)
  dedup_cache_size: 10000      # Deduplication cache size (LRU)
  temporal_cooccurrence: false # Apply time decay while updating co-occurrence
  temporal_half_life_sec: 86400
  negative_signals: false      # Apply DEL events as negative feedback
  negative_weight: 0.5
```

<!-- BEGIN GENERATED: options events -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `ctx_buffer_size` | int | 50 | Number of events to track per context (ring buffer size) (10-10000) |
| `max_contexts` | int | 0 | Maximum active contexts retained by the LRU; least-recently-active contexts are pruned first (0 = unlimited) (0-1000000) |
| `max_neighbors_per_item` | int | 0 | Maximum retained co-occurrence neighbors per item (0 = unlimited) (0-1000000) |
| `min_support` | float | 0.0 | Prune co-occurrence edges with absolute score below this threshold (0 = disabled) (>= 0.0) |
| `decay_interval_sec` | int | 3600 | Decay interval in seconds (0 = disabled) (0-86400) |
| `decay_alpha` | float | 0.99 | Decay factor (0.0-1.0) |
| `dedup_window_sec` | int | 60 | Deduplication time window in seconds (0 = disabled) (0-86400) |
| `dedup_cache_size` | int | 10000 | Deduplication cache size (LRU); 0 = disable deduplication entirely, including the SET/DEL idempotency tracking (0-1000000) |
| `temporal_cooccurrence` | bool | false | Enable time-decay for co-occurrence updates |
| `temporal_half_life_sec` | float | 86400.0 | Half-life in seconds for temporal decay (score halves per this many seconds of age) (> 0.0) |
| `negative_signals` | bool | false | Enable negative signal (down-ranking) on DEL events |
| `negative_weight` | float | 0.5 | Reduction weight applied for negative signals (0.0-1.0) |
<!-- END GENERATED: options events -->

**Deduplication Behavior:**

Duplicate events are detected when the same `(ctx, id, score)` tuple is received within `dedup_window_sec`. This prevents:
- Retry bugs from inflating statistics
- Network re-transmissions from creating duplicate entries
- Client-side bugs from affecting co-occurrence data

Statistics tracking:
- `total_events`: Total EVENT commands received (including duplicates)
- `deduped_events`: Number of duplicate events ignored
- `stored_events`: Actual events stored in ring buffers (total_events - deduped_events)

---

### Vector Store Configuration

Controls vector storage and search behavior.

```yaml
vectors:
  default_dimension: 768       # Default vector dimension
  distance_metric: "cosine"    # Distance metric: cosine, dot, l2
```

<!-- BEGIN GENERATED: options vectors -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `default_dimension` | int | 768 | Default vector dimension (number of features per vector) (1-4096) |
| `distance_metric` | string | "cosine" | Distance metric for similarity search (`cosine` `dot` `l2`) |
<!-- END GENERATED: options vectors -->

Common embedding dimensions are 768 (BERT), 1536 (OpenAI) and 384 (MiniLM).

---

### Similarity Search Configuration

Controls similarity search and fusion algorithm parameters.

```yaml
similarity:
  default_top_k: 100           # Default number of results
  max_top_k: 1000              # Maximum allowed top_k
  fusion_alpha: 0.6            # Vector similarity weight (fusion mode)
  fusion_beta: 0.4             # Co-occurrence weight (fusion mode)
  adaptive_fusion: false       # Adjust vector weight by item maturity
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 50
```

<!-- BEGIN GENERATED: options similarity -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `default_top_k` | int | 100 | Default number of results to return in similarity searches (1-10000) |
| `max_top_k` | int | 1000 | Maximum number of results allowed in similarity searches (1-10000) |
| `fusion_alpha` | float | 0.6 | Weight for the vector-similarity component in fusion mode (0.0-1.0) |
| `fusion_beta` | float | 0.4 | Weight for the co-occurrence component in fusion mode (0.0-1.0) |
| `sample_size` | int | 10000 | Random sampling size for approximate search; sampling engages only once the corpus exceeds twice this value (0 = exact search), and the maximum is the largest value whose doubling stays inside the 32-bit counter that comparison uses (0-2147483647) |
| `ivf_enabled` | bool | false | Enable IVF (Inverted File) approximate nearest neighbor search |
| `ivf_nlist` | int | 256 | Number of Voronoi cells/clusters for IVF index (0 = auto sqrt(n)) (0-65536) |
| `ivf_nprobe` | int | 8 | Number of clusters to probe at query time (1-65536) |
| `ivf_train_threshold` | int | 10000 | Minimum number of vectors before auto-training IVF index; the maximum is the range of the type it is read into, not a tuning limit (1-4294967295) |
| `ivf_seal_threshold` | int | 100000 | Seal the write buffer when it reaches this number of vectors; the maximum is the range of the type it is read into, not a tuning limit (1-4294967295) |
| `adaptive_fusion` | bool | false | Enable adaptive fusion weight computation based on item maturity |
| `adaptive_min_alpha` | float | 0.2 | Minimum vector weight, used for mature items with many co-occurrences (0.0-1.0) |
| `adaptive_max_alpha` | float | 0.9 | Maximum vector weight, used for new items with few co-occurrences (0.0-1.0) |
| `adaptive_maturity_threshold` | int | 50 | Co-occurrence neighbor count at which an item is considered mature; the maximum is the range of the type it is read into, not a tuning limit (1-4294967295) |
| `index_type` | string | "flat" | ANN index type: hnsw, ivf, or flat (brute-force) (`hnsw` `ivf` `flat`) |
| `hnsw_m` | int | 16 | HNSW number of connections per node; the maximum is the largest value whose doubled bottom-layer link count stays inside the 32-bit counter that holds it (2-2147483647) |
| `hnsw_ef_construction` | int | 200 | HNSW search width during construction; the maximum is the range of the type it is read into, not a tuning limit (1-4294967295) |
| `hnsw_ef_search` | int | 50 | HNSW search width during query; the maximum is the range of the type it is read into, not a tuning limit (1-4294967295) |
| `hnsw_max_elements` | int | 0 | HNSW pre-allocated capacity (0 = dynamic growth); the value is reserved at startup and the maximum is the largest index the snapshot loader accepts, so reserving beyond it produces an index that cannot be reloaded (0-10000000) |
<!-- END GENERATED: options similarity -->

**Note**: Higher `fusion_beta` gives more weight to event-based signals.

#### ANN Index Selection and Tuning

`similarity.index_type` selects the active vector index: `flat` (the default),
`hnsw`, or `ivf`. Exactly one of these implementations is used by the server;
TieredVectorStore, MergeScheduler, and ScalarQuantizer are not runtime features.

```yaml
similarity:
  index_type: hnsw
  hnsw_m: 16
  hnsw_ef_construction: 200
  hnsw_ef_search: 50
  hnsw_max_elements: 0       # 0 = grow dynamically
```

Raising `hnsw_m` or `hnsw_ef_construction` costs memory and ingest time;
raising `hnsw_ef_search` or `ivf_nprobe` costs query latency. All four buy
recall. The table above lists their types, defaults and accepted ranges.

`ivf_enabled` remains a legacy compatibility option; prefer
`index_type: ivf` for new configurations.

##### What the tuning knobs buy

An approximate index is only meaningful as a trade: any of them can be made
faster by returning worse answers. The table below pairs recall against
latency at each setting so the trade is visible rather than implied.

Measured on 50,000 vectors drawn around 200 latent centroids — the shape
embeddings actually have — with an exhaustive scan over the same vectors as
ground truth, `top_k=10`, 200 queries per point. Reproduce with:

```bash
cmake --build build --target ann_recall_benchmark
./build/bin/ann_recall_benchmark --gtest_also_run_disabled_tests
```

Hardware, SIMD configuration and build flags are the same as the
[Benchmark Environment](benchmarks.md#benchmark-environment): Apple M5 Max
(arm64) with NEON, Release (`-O3 -march=native`), Apple Clang. The corpus is
generated from a fixed seed, so a rerun on the same build reproduces the same
recall figures.

**These tables measure the index in isolation.** Each `vs exact scan` figure is
the p50 of 200 calls into the index, divided by the p50 of an exhaustive scan
over the same vectors using the same distance computation. A query arriving over
the wire does more than that: it goes through the engine, which adds metadata
filtering, result assembly and a cache lookup on every query, and that cost does
not shrink as the index gets faster. The ratio you observe end to end is
therefore not the ratio below. On this data at the default `ivf_nprobe: 8`,
dim 128, the engine path measures 9.3x against the engine's own brute-force path
where the index in isolation measures 7.5x — the engine overhead applies to both
sides of that comparison. Treat these tables as the shape of the trade, not as a
throughput prediction for your deployment.

The ratios were taken on a shared machine under other load, with numerator and
denominator measured inside the same run so both see the same conditions. The
benchmark also prints p99, which is not reproduced here.

HNSW, `hnsw_m: 16`, `hnsw_ef_construction: 200`:

| `hnsw_ef_search` | recall@10 (dim 128) | vs exact scan | recall@10 (dim 768) | vs exact scan |
|---|---|---|---|---|
| 10 | 0.996 | 21x | 0.985 | 55x |
| 16 | 1.000 | 17x | 0.997 | 48x |
| 32 | 1.000 | 13x | 1.000 | 38x |
| 64 | 1.000 | 8x | 1.000 | 30x |

IVF, `ivf_nlist: 256`:

| `ivf_nprobe` | recall@10 (dim 128) | vs exact scan | recall@10 (dim 768) | vs exact scan |
|---|---|---|---|---|
| 1 | 0.962 | 32x | 0.981 | 42x |
| 2 | 0.996 | 21x | 0.998 | 27x |
| 4 | 1.000 | 15x | 1.000 | 14x |
| 8 | 1.000 | 8x | 1.000 | 8x |

The defaults (`hnsw_ef_search: 50`, `ivf_nprobe: 8`) sit past the point where
recall has already reached 1.0 on this data, so they are conservative rather
than tuned for throughput. Lowering them is the first thing to try if query
latency matters more than the last fraction of recall.

::: warning Recall depends on how your vectors are distributed
These figures come from data with cluster structure. Vectors spread evenly
across the space — random directions with no grouping — are the worst case
for every approximate index, and recall there is far lower at the same
settings: HNSW reaches only 0.39 at `ef_search: 64` and needs `512` to pass
0.93, by which point it is several times slower than scanning everything.

If your embeddings have little structure, an approximate index will not help.
Measure with the benchmark above against your own vectors before switching
`index_type` away from `flat`.
:::

---

### Snapshot Persistence Configuration

Controls snapshot save/load behavior.

```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"  # Snapshot directory
  default_filename: "nvecd.snapshot" # Default filename
  interval_sec: 0                   # Auto-snapshot interval (0 = disabled)
  retain: 3                         # Number of snapshots to retain
  mode: "fork"                     # Snapshot mode: "fork" (COW) or "lock"
```

<!-- BEGIN GENERATED: options snapshot -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | Snapshot directory path |
| `default_filename` | string | "nvecd.snapshot" | Filename an argument-less DUMP SAVE writes inside the snapshot directory; validated like a client-supplied path, so an absolute or escaping name is refused (empty = fall back to a timestamped name) |
| `interval_sec` | int | 0 | Snapshot interval in seconds (0 = disabled) (0-86400) |
| `retain` | int | 3 | Number of automatic snapshots to retain; manual snapshots are never removed (0 = keep every file) (0-100) |
| `mode` | string | "fork" | Snapshot consistency mode: fork (COW, non-blocking) or lock (global write lock, blocking) (`fork` `lock`) |
<!-- END GENERATED: options snapshot -->

`snapshot.dir` is created if it does not exist.

**Auto-snapshot filenames**: `auto_YYYYMMDD_HHMMSS.nvec`

**Security requirement**: On POSIX systems, `snapshot.dir` must be owned by the
user running nvecd and must not be writable by its group or by other users.
Snapshot files are created with mode `0600`; use a service-private directory
(normally mode `0700`).

---

### Write-Ahead Log Configuration

Use WAL to replay writes made after the most recent snapshot following a restart.
`include_vectors: false` reduces log size, but VECSET payloads then require a
subsequent snapshot to survive a crash.

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
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enabled` | bool | false | Enable WAL-based crash recovery |
| `dir` | string | "/var/lib/nvecd/wal" | Directory holding WAL segment files |
| `max_file_size` | int | 67108864 | Maximum size per WAL file in bytes; the maximum is the largest integer the configuration reader carries, not a tuning limit (1-9223372036854775807) |
| `sync_on_write` | bool | false | fsync after every append (high durability, lower throughput); sync_interval_ms selects the batch interval when this is false |
| `sync_interval_ms` | int | 100 | Batch fsync interval in milliseconds when sync_on_write is false (0 = fsync on every append); the maximum is the range of the type it is read into, not a tuning limit (0-4294967295) |
| `include_vectors` | bool | true | Persist vector bodies in VECSET WAL records; disable only when snapshots provide the required vector durability |
<!-- END GENERATED: options wal -->

`wal.dir` must not be empty when `wal.enabled` is true.

Durability is never off: every accepted record reaches the platter in bounded
time under any schema-valid pair of settings. `sync_on_write: true` fsyncs each
append. With `sync_on_write: false`, `sync_interval_ms` sets the batch interval,
and `sync_interval_ms: 0` fsyncs each append as well, because a zero interval
would otherwise leave nothing to flush the batch.

Segments are named `wal-NNNNNN.log` with a fixed six-digit number, so the
segment number space ends at `wal-999999.log`. Rotation past that point fails
rather than writing a wider name, which recovery and truncation would not match.
A deployment reaches the cap only by rotating a million times without ever
taking a snapshot; a snapshot checkpoint truncates the log and reclaims the
numbers.

The same ownership and non-shared-write requirement applies to `wal.dir`.
WAL directories and segment files are created with modes `0700` and `0600`.

---

### Performance Configuration

Controls server performance and resource limits.

```yaml
performance:
  thread_pool_size: 8          # Worker thread pool size
  max_connections: 1000        # Maximum concurrent connections
  connection_timeout_sec: 300  # Connection timeout (seconds)
  reactor_max_total_buffered_bytes: 268435456  # Aggregate buffered-data cap (256 MiB)
```

<!-- BEGIN GENERATED: options performance -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `thread_pool_size` | int | 8 | Worker thread pool size (0 = one worker per detected hardware thread) (0-128) |
| `max_connections` | int | 1000 | Maximum concurrent connections (1-100000) |
| `max_connections_per_ip` | int | 100 | Maximum connections per IP address (0 = unlimited) (0-100000) |
| `connection_timeout_sec` | int | 300 | Idle timeout, and the deadline for receiving the first complete request, in seconds (1-3600) |
| `recv_buffer_size` | int | 4096 | TCP receive buffer size in bytes (1024-1048576) |
| `send_buffer_size` | int | 65536 | TCP send buffer size in bytes (1024-16777216) |
| `max_query_length` | int | 1048576 | Maximum bytes accepted for a single request (1024-16777216) |
| `shutdown_timeout_ms` | int | 5000 | Graceful shutdown timeout in milliseconds (100-60000) |
| `reactor_max_total_buffered_bytes` | int | 268435456 | Process-wide cap for bytes buffered by the TCP reactor (1048576-1073741824) |
<!-- END GENERATED: options performance -->

Set `thread_pool_size` to the number of CPU cores, and `max_connections` from
the process file-descriptor limit and available memory.

---

### API Server Configuration

Controls TCP and HTTP API server settings.

#### TCP API (Always Enabled)

```yaml
api:
  tcp:
    bind: "127.0.0.1"          # TCP bind address
    port: 11017                # TCP port
```

<!-- BEGIN GENERATED: options api.tcp -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `bind` | string | "127.0.0.1" | TCP bind address ("0.0.0.0" listens on every interface) |
| `port` | int | 11017 | TCP port (1-65535) |
<!-- END GENERATED: options api.tcp -->

Binding to `0.0.0.0` exposes the server to every reachable network; pair it
with `network.allow_cidrs` and `security.requirepass`.

#### HTTP API (Optional)

```yaml
api:
  http:
    enable: false              # Enable HTTP/JSON API
    bind: "127.0.0.1"          # HTTP bind address
    port: 8080                 # HTTP port
    enable_cors: false         # Enable CORS headers
    cors_allow_origin: ""      # Allowed origin
```

<!-- BEGIN GENERATED: options api.http -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enable` | bool | false | Enable HTTP server |
| `bind` | string | "127.0.0.1" | HTTP bind address ("0.0.0.0" listens on every interface) |
| `port` | int | 8080 | HTTP port (1-65535) |
| `enable_cors` | bool | false | Enable CORS headers |
| `cors_allow_origin` | string | "" | Value for Access-Control-Allow-Origin header when CORS is enabled |
| `timeout_sec` | int | 5 | HTTP read/write timeout in seconds (1-300) |
<!-- END GENERATED: options api.http -->

#### Unix Domain Socket (Optional)

```yaml
api:
  unix_socket:
    path: ""                     # Unix socket path (empty = disabled)
```

<!-- BEGIN GENERATED: options api.unix_socket -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `path` | string | "" | Unix socket path (empty string = disabled) |
<!-- END GENERATED: options api.unix_socket -->

**Note**: Unix domain sockets provide lower-latency local connections. They bypass TCP/IP overhead and are ideal for co-located services.

#### Rate Limiting (Optional)

```yaml
api:
  rate_limiting:
    enable: false              # Enable rate limiting
    capacity: 100              # Max burst tokens
    refill_rate: 10            # Tokens per second
    max_clients: 10000         # Max tracked clients
```

<!-- BEGIN GENERATED: options api.rate_limiting -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enable` | bool | false | Enable rate limiting |
| `capacity` | int | 100 | Maximum number of tokens per client (burst size) (1-10000) |
| `refill_rate` | int | 10 | Tokens added per second per client (1-1000) |
| `max_clients` | int | 10000 | Maximum number of tracked clients (for memory management) (10-100000) |
<!-- END GENERATED: options api.rate_limiting -->

---

### Network Security Configuration

Controls IP address access control (CIDR-based).

```yaml
network:
  allow_cidrs:
    - "127.0.0.1/32"           # Localhost only (recommended)
    # - "192.168.1.0/24"       # Example: Local network
    # - "0.0.0.0/0"            # WARNING: Allow all (not recommended)
```

<!-- BEGIN GENERATED: options network -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `allow_cidrs` | list | [] | Allow CIDR list (empty = deny all) |
<!-- END GENERATED: options network -->

**Security Note**: `allow_cidrs` is fail-closed. An empty or absent list
**denies every connection**; you must configure the allowed IP ranges
explicitly. Starting nvecd with no configuration file at all is the one
exception: that path restricts access to `127.0.0.1/32`.

---

### Logging Configuration

Controls logging output format and destination.

```yaml
logging:
  level: "info"                # Log level
  json: true                   # JSON format output
  file: ""                     # Log file path (empty = stdout)
```

<!-- BEGIN GENERATED: options logging -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `level` | string | "info" | Log level (`trace` `debug` `info` `warn` `error`) |
| `json` | bool | true | JSON format output |
| `file` | string | "" | Log file path (empty string = stdout, path = file output) |
<!-- END GENERATED: options logging -->

---

### Security Configuration

Controls authentication for write and admin commands.

```yaml
security:
  requirepass: ""                # Required password (empty = no auth)
```

<!-- BEGIN GENERATED: options security -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `requirepass` | string | "" | Required password for write/admin commands (empty = no auth) |
<!-- END GENERATED: options security -->

When `requirepass` is set, clients must authenticate with `AUTH <password>`
before executing a write or admin command. The classification is:

- **Write**: `EVENT`, `VECSET`, `VECDEL`, `METASET`, `SET`, `CACHE CLEAR`,
  `CACHE ENABLE`, `CACHE DISABLE`
- **Admin**: `DUMP SAVE`, `DUMP LOAD`, `DUMP VERIFY`, `DUMP INFO`,
  `DUMP STATUS`, `CONFIG VERIFY`
- **Read** (never gated): `SIM`, `SIMV`, `INFO`, `CONFIG SHOW`, `CONFIG HELP`,
  `CACHE STATS`, `GET`, `SHOW VARIABLES`, `DEBUG ON`, `DEBUG OFF`

Over HTTP, `Authorization: Bearer <password>` and `Authorization: Basic` (the
username is ignored) authenticate the same command set; gated endpoints answer
`401` when the header is missing or wrong.

---

### Query Result Cache Configuration (Optional)

```yaml
cache:
  enabled: true                # Enable query result cache
  max_memory_mb: 32            # Maximum cache memory (MB)
  min_query_cost_ms: 10.0      # Minimum query cost to cache (ms)
  ttl_seconds: 3600            # Cache entry TTL (seconds)
  compression_enabled: true    # Enable LZ4 compression
  eviction_batch_size: 10      # Eviction batch size
```

<!-- BEGIN GENERATED: options cache -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enabled` | bool | true | Enable/disable cache |
| `max_memory_mb` | int | 32 | Maximum cache memory in MB; the maximum is the range of the type it is read into, not a tuning limit (1-2147483647) |
| `min_query_cost_ms` | float | 10.0 | Minimum query cost to cache (ms) (>= 0.0) |
| `ttl_seconds` | int | 3600 | Cache entry TTL (seconds, 0 = no TTL); the maximum is the range of the type it is read into, not a tuning limit (0-2147483647) |
| `compression_enabled` | bool | true | Enable LZ4 compression |
| `eviction_batch_size` | int | 10 | Number of entries to evict at once; the maximum is the range of the type it is read into, not a tuning limit (1-2147483647) |
<!-- END GENERATED: options cache -->

---

## Minimal Configuration Example

```yaml
# Minimal config for local testing
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

## Production Configuration Example

```yaml
# Production config with security hardening
events:
  ctx_buffer_size: 100
  decay_interval_sec: 7200     # 2 hours
  decay_alpha: 0.95

vectors:
  default_dimension: 768

similarity:
  max_top_k: 500
  fusion_alpha: 0.7
  fusion_beta: 0.3

snapshot:
  dir: "/var/lib/nvecd/snapshots"
  interval_sec: 14400          # 4 hours
  retain: 5

performance:
  thread_pool_size: 16         # 16-core server
  max_connections: 5000
  connection_timeout_sec: 600

api:
  tcp:
    bind: "0.0.0.0"            # All interfaces (use allow_cidrs for security)
    port: 11017

network:
  allow_cidrs:
    - "10.0.0.0/8"             # Private network only
    - "172.16.0.0/12"

logging:
  level: "warn"
  json: true
  file: "/var/log/nvecd/nvecd.log"
```

---

## Verifying Configuration

Use `CONFIG VERIFY` command to check configuration file syntax:

```bash
# Connect to server
nc localhost 11017

# Verify configuration
CONFIG VERIFY
```

Or use `CONFIG SHOW` to display current configuration:

```bash
CONFIG SHOW
```

---

## Next Steps

- See [Protocol Reference](protocol.md) for available commands
- See [Snapshot Management](snapshot.md) for persistence details
- See [Installation Guide](installation.md) for deployment instructions
