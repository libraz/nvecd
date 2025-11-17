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
  decay_interval_sec: 3600     # Decay interval (seconds)
  decay_alpha: 0.99            # Decay factor (0.0 - 1.0)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `ctx_buffer_size` | int | 50 | Number of events to keep per context. Older events are overwritten. |
| `decay_interval_sec` | int | 3600 | How often co-occurrence scores decay (0 = disabled). |
| `decay_alpha` | float | 0.99 | Score multiplier at each decay (0.0-1.0, higher = slower decay). |

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
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `default_top_k` | int | 100 | Default number of results when not specified. |
| `max_top_k` | int | 1000 | Maximum allowed top_k (prevents memory issues). |
| `fusion_alpha` | float | 0.6 | Weight for vector similarity in fusion mode (alpha + beta = 1.0). |
| `fusion_beta` | float | 0.4 | Weight for co-occurrence in fusion mode. |

**Note**: Higher `fusion_beta` gives more weight to event-based signals.

---

### Snapshot Persistence Configuration

Controls snapshot save/load behavior.

```yaml
snapshot:
  dir: "/var/lib/nvecd/snapshots"  # Snapshot directory
  default_filename: "nvecd.snapshot" # Default filename
  interval_sec: 0                   # Auto-snapshot interval (0 = disabled)
  retain: 3                         # Number of snapshots to retain
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | Snapshot directory (created if doesn't exist). |
| `default_filename` | string | "nvecd.snapshot" | Default filename for manual saves. |
| `interval_sec` | int | 0 | Auto-snapshot interval in seconds (0 = disabled). |
| `retain` | int | 3 | Number of auto-snapshots to retain (manual snapshots unaffected). |

**Auto-snapshot filenames**: `auto_YYYYMMDD_HHMMSS.snapshot`

---

### Performance Configuration

Controls server performance and resource limits.

```yaml
performance:
  thread_pool_size: 8          # Worker thread pool size
  max_connections: 1000        # Maximum concurrent connections
  connection_timeout_sec: 300  # Connection timeout (seconds)
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `thread_pool_size` | int | 8 | Worker thread pool size (recommended: number of CPU cores). |
| `max_connections` | int | 1000 | Maximum concurrent connections (set based on system limits). |
| `connection_timeout_sec` | int | 300 | Idle connection timeout in seconds. |

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

> ⚠️ **Not Implemented Yet** - HTTP/JSON API is planned for future releases.

```yaml
api:
  http:
    enable: false              # Enable HTTP/JSON API
    bind: "127.0.0.1"          # HTTP bind address
    port: 8080                 # HTTP port
    enable_cors: false         # Enable CORS headers
    cors_allow_origin: ""      # Allowed origin
```

#### Rate Limiting (Optional)

> ⚠️ **Not Implemented Yet** - Rate limiting is planned for future releases.

```yaml
api:
  rate_limiting:
    enable: false              # Enable rate limiting
    capacity: 100              # Max burst tokens
    refill_rate: 10            # Tokens per second
    max_clients: 10000         # Max tracked clients
```

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

### Query Result Cache Configuration (Optional)

> ⚠️ **Not Implemented Yet** - Query result caching is planned for future releases.

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
