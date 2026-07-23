# Configuration Guide

This guide explains all configuration options available in Nvecd.

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `ctx_buffer_size` | int | 50 | Number of events to keep per context. Older events are overwritten. |
| `max_contexts` | int | 0 | Maximum active contexts retained; `0` means unlimited. Least-recently-active contexts are pruned first. |
| `max_neighbors_per_item` | int | 0 | Maximum co-occurrence edges retained per item; `0` means unlimited. |
| `min_support` | float | 0.0 | Prune co-occurrence edges below this score; `0` disables pruning. |
| `decay_interval_sec` | int | 3600 | How often co-occurrence scores decay (0 = disabled). |
| `decay_alpha` | float | 0.99 | Score multiplier at each decay (0.0-1.0, higher = slower decay). |
| `dedup_window_sec` | int | 60 | Time window for duplicate detection in seconds. Duplicate events (same ctx, id, score) within this window are ignored. Set to 0 to disable deduplication. |
| `dedup_cache_size` | int | 10000 | Maximum number of recent events tracked for deduplication (LRU cache). Oldest entries are evicted when full. |
| `temporal_cooccurrence` | bool | false | Enable time-decayed co-occurrence contributions. |
| `temporal_half_life_sec` | float | 86400 | Decay half-life in seconds when temporal co-occurrence is enabled. |
| `negative_signals` | bool | false | Treat DEL events as negative feedback. |
| `negative_weight` | float | 0.5 | Negative-feedback strength (0.0-1.0). |

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `default_dimension` | int | 768 | Default vector dimension (common: 768=BERT, 1536=OpenAI, 384=MiniLM). |
| `distance_metric` | string | "cosine" | Distance metric: `cosine`, `dot`, `l2`. |

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `default_top_k` | int | 100 | Default number of results when not specified. |
| `max_top_k` | int | 1000 | Maximum allowed top_k (prevents memory issues). |
| `fusion_alpha` | float | 0.6 | Weight for vector similarity in fusion mode (alpha + beta = 1.0). |
| `fusion_beta` | float | 0.4 | Weight for co-occurrence in fusion mode. |
| `adaptive_fusion` | bool | false | Adjust vector/fusion weights from the item's co-occurrence maturity. |
| `adaptive_min_alpha` | float | 0.2 | Vector weight for mature items. |
| `adaptive_max_alpha` | float | 0.9 | Vector weight for new items. |
| `adaptive_maturity_threshold` | int | 50 | Neighbor count at which an item is considered mature. |

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

| Option | Default | Description |
|--------|---------|-------------|
| `index_type` | `flat` | `flat`, `hnsw`, or `ivf`. |
| `hnsw_m` | 16 | Connections per HNSW node; higher values use more memory and can improve recall. |
| `hnsw_ef_construction` | 200 | Build-time search width; higher values trade ingest time for recall. |
| `hnsw_ef_search` | 50 | Query-time search width; higher values trade latency for recall. |
| `hnsw_max_elements` | 0 | Preallocated HNSW capacity; `0` grows dynamically. |
| `ivf_nlist` | 256 | Number of IVF clusters. |
| `ivf_nprobe` | 8 | Clusters searched per query; higher values trade latency for recall. |
| `ivf_train_threshold` | 10000 | Vectors required before IVF auto-training. |
| `ivf_seal_threshold` | 100000 | Buffered vectors before an IVF segment is sealed. |

`ivf_enabled` remains a legacy compatibility option; prefer
`index_type: ivf` for new configurations.

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | Snapshot directory (created if doesn't exist). |
| `default_filename` | string | "nvecd.snapshot" | Default filename for manual saves. |
| `interval_sec` | int | 0 | Auto-snapshot interval in seconds (0 = disabled). |
| `retain` | int | 3 | Number of auto-snapshots to retain (manual snapshots unaffected). |
| `mode` | string | "fork" | Snapshot strategy. `fork`: Copy-on-write via fork() -- parent continues serving. `lock`: Global write lock during save. |

**Auto-snapshot filenames**: `auto_YYYYMMDD_HHMMSS.snapshot`

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

| Option | Default | Description |
|--------|---------|-------------|
| `enabled` | false | Enable WAL crash recovery. |
| `dir` | `/var/lib/nvecd/wal` | WAL segment directory; required when enabled. |
| `max_file_size` | 67108864 | Segment rotation size in bytes. |
| `sync_on_write` | false | fsync every append for maximum durability. |
| `sync_interval_ms` | 100 | Batched fsync interval when `sync_on_write` is false. |
| `include_vectors` | true | Persist VECSET payloads. Disable only when snapshots provide the required vector durability. |

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `thread_pool_size` | int | 8 | Worker thread pool size (recommended: number of CPU cores). |
| `max_connections` | int | 1000 | Maximum concurrent connections (set based on system limits). |
| `connection_timeout_sec` | int | 300 | Idle timeout and deadline for receiving the first complete request, in seconds. |
| `reactor_max_total_buffered_bytes` | int | 268435456 | Maximum bytes the TCP reactor may hold across all pending and unsent data. |

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `bind` | string | "127.0.0.1" | Bind address ("0.0.0.0" = all interfaces, **security risk**). |
| `port` | int | 11017 | TCP listen port. |

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

#### Unix Domain Socket (Optional)

```yaml
api:
  unix_socket:
    path: ""                     # Unix socket path (empty = disabled)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `path` | string | "" | Unix domain socket path. Empty string disables Unix socket. |

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enable` | bool | false | Enable per-client rate limiting (token bucket algorithm). |
| `capacity` | int | 100 | Maximum burst tokens per client. |
| `refill_rate` | int | 10 | Tokens refilled per second per client. |
| `max_clients` | int | 10000 | Maximum number of tracked client IPs. |

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

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `allow_cidrs` | list | `["127.0.0.1/32"]` | Allowed CIDR ranges. **Empty = deny all (fail-closed)**. |

**Security Note**: Empty `allow_cidrs` will **deny all connections** by default. You must explicitly configure allowed IP ranges.

---

### Logging Configuration

Controls logging output format and destination.

```yaml
logging:
  level: "info"                # Log level
  json: true                   # JSON format output
  file: ""                     # Log file path (empty = stdout)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `level` | string | "info" | Log level: `trace`, `debug`, `info`, `warn`, `error`. |
| `json` | bool | true | Enable JSON structured logging. |
| `file` | string | "" | Log file path (empty = stdout, for systemd/Docker). |

---

### Security Configuration

Controls authentication for write and admin commands.

```yaml
security:
  requirepass: ""                # Required password (empty = no auth)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `requirepass` | string | "" | Password for write/admin commands. Empty = no authentication. |

**Note**: When `requirepass` is set, clients must authenticate with `AUTH <password>` before executing write or admin commands. `EVENT`, `VECSET`, `METASET`, and DUMP/SET are writes; SIM, SIMV, INFO, and CONFIG SHOW are read commands.

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
