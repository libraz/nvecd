# Protocol Reference

Nvecd uses a simple text-based protocol over TCP (similar to Redis/Memcached), with MygramDB-compatible admin commands.

**Protocol Format**: Text-based, line-delimited, UTF-8 encoded

## Connection

Connect to nvecd via TCP:

```bash
# Using netcat
nc localhost 11017

# Using telnet
telnet localhost 11017

# Using Unix domain socket (if configured)
nc -U /var/run/nvecd.sock
```

---

## Protocol Format

- **Transport**: Text-based line protocol (UTF-8)
- **Request**: `COMMAND args...\r\n` (accepts both `\r\n` and `\n`)
- **Response**: `OK data...\r\n` or `ERROR message\r\n`
- **Max request size**: `performance.max_query_length` bytes (default 1 MiB, up to 16 MiB)

### Response Format

**Success**:
```
OK [data]\r\n
```

A command that returns nothing but an acknowledgement echoes its own name, for
example `OK VECSET`. `AUTH` is the single exception and answers `+OK\r\n`.

**Error**:
```
ERROR <message>\r\n
```

This is the only error shape on the wire; no Redis-style variant is emitted.

---

## Command Categories

- **Core Commands**: EVENT, VECSET, SIM, SIMV (nvecd-specific)
- **Admin Commands**: AUTH, INFO, CONFIG, DUMP, DEBUG (MygramDB-compatible)
- **Cache Commands**: CACHE (query result cache management)
- **Runtime Variables**: SET, GET, SHOW VARIABLES

---

## Core Commands

### EVENT — Ingest co-occurrence event

Record an event associating a context with an ID. Supports three event types:
- **ADD**: Stream events (clicks, views) with time-window deduplication
- **SET**: State events (likes, bookmarks, ratings) with idempotent updates
- **DEL**: Deletion events (unlike, unbookmark) with idempotent removal

**Syntax**:
```
EVENT <ctx> ADD <id> <score>
EVENT <ctx> SET <id> <score>
EVENT <ctx> DEL <id>
```

**Parameters**:
- `<ctx>`: Context identifier (string, e.g., user ID, session ID)
- `<type>`: Event type: `ADD`, `SET`, or `DEL`
- `<id>`: Item identifier (string, e.g., item ID, action ID)
- `<score>`: Event score (integer, 0-100) — required for ADD/SET, ignored for DEL

**Examples**:

```bash
# Stream event (click tracking)
EVENT user123 ADD view:item456 95
→ OK EVENT

# State event (like ON)
EVENT user123 SET like:item456 100
→ OK EVENT

# State event (like OFF)
EVENT user123 SET like:item456 0
→ OK EVENT

# Weighted bookmark (high priority)
EVENT user123 SET bookmark:item789 100
→ OK EVENT

# Change bookmark priority (medium)
EVENT user123 SET bookmark:item789 50
→ OK EVENT

# Delete bookmark
EVENT user123 DEL bookmark:item789
→ OK EVENT
```

**Event Type Behavior**:

| Type | Use Case | Deduplication | Example |
|------|----------|---------------|---------|
| **ADD** | Stream events (clicks, views, plays) | Time-window based (default: 60 sec) | `EVENT user1 ADD view:item1 100` |
| **SET** | State events (likes, bookmarks, ratings) | Same value = duplicate (idempotent) | `EVENT user1 SET like:item1 100` |
| **DEL** | Deletion events | Already deleted = duplicate (idempotent) | `EVENT user1 DEL like:item1` |

**Idempotency Guarantees**:

```bash
# SET is idempotent for same value
EVENT user1 SET like:item1 100
EVENT user1 SET like:item1 100  # Duplicate, ignored
→ OK EVENT (both succeed, second is deduped)

# SET allows state transitions
EVENT user1 SET bookmark:item1 100  # High priority
EVENT user1 SET bookmark:item1 50   # Medium priority (stored)
EVENT user1 SET bookmark:item1 50   # Duplicate (ignored)
→ OK EVENT

# DEL is idempotent
EVENT user1 DEL like:item1
EVENT user1 DEL like:item1  # Already deleted, ignored
→ OK EVENT
```

**Error Responses**:
- `ERROR Invalid EVENT type: <type> (must be ADD, SET, or DEL)`
- `ERROR EVENT ADD requires 4 arguments: <ctx> ADD <id> <score>`
- `ERROR EVENT SET requires 4 arguments: <ctx> SET <id> <score>`
- `ERROR EVENT DEL requires 3 arguments: <ctx> DEL <id>`
- `ERROR Invalid score: must be integer`
- `ERROR Context cannot be empty`
- `ERROR ID cannot be empty`

**Notes**:
- Events are stored in a ring buffer per context (size: `events.ctx_buffer_size`)
- Deduplication cache size: `events.dedup_cache_size` (default: 10,000 entries)
- Time window for ADD type: `events.dedup_window_sec` (default: 60 seconds)
- SET/DEL use last-value tracking for idempotency (no time window)
- Co-occurrence scores are automatically tracked between IDs in the same context
- Scores decay over time based on `events.decay_interval_sec` and `events.decay_alpha`

---

### VECSET — Register vector

Register or update a vector for an item.

**Syntax**:
```
VECSET <id> <f1> <f2> ... <fN>
```

**Parameters**:
- `<id>`: Item identifier (string)
- `<f1> <f2> ... <fN>`: Vector components (floats)

**Example**:
```
VECSET item456 0.1 0.5 0.8
→ OK VECSET
```

**Example with 768-dimensional vector**:
```
VECSET item789 0.11 0.98 -0.22 0.44 ... (768 values)
→ OK VECSET
```

**Error Responses**:
- `ERROR Dimension mismatch: expected 768, got 512`
- `ERROR Invalid vector format`
- `ERROR Invalid argument count`

**Notes**:
- Dimension is auto-detected from the number of values
- All vectors must have the same dimension (default: 768, configurable via `vectors.default_dimension`)
- Vectors are automatically normalized based on `vectors.distance_metric` setting

---

### VECDEL — Delete vector

Remove an item's vector, its metadata, and its cached results.

**Syntax**:
```
VECDEL <id>
```

**Parameters**:
- `<id>`: Item identifier (string)

**Example**:
```
VECDEL item456
→ OK VECDEL
```

**Error Responses**:
- `ERROR VECDEL requires 1 argument: <id>`
- `ERROR Vector not found: item456`

**Notes**:
- Requires authentication when `security.requirepass` is set
- Metadata registered with `METASET` is removed together with the vector
- The active ANN index is rebuilt, so a delete is more expensive than a write
- Co-occurrence edges recorded by `EVENT` are not affected

### METASET — Register item metadata

Attach metadata to an existing vector item for `filter=` queries.

**Syntax**:
```
METASET <id> <key:value[,key:value...]>
```

**Example**:
```
METASET product456 category:electronics,active:true,rank:10
SIM product123 10 using=vectors filter=category:electronics
```

Values are auto-typed the same way as `filter=` values: string, integer, float, or bool. The item must already exist in `VectorStore` via `VECSET`.

---

### SIM — Similarity search by ID

Find similar items based on an existing item's vector and co-occurrence data.

**Syntax**:
```
SIM <id> <top_k> [using=events|vectors|fusion] [filter=<expr>] [min_score=<float>] [adaptive=on|off]
```

**Parameters**:
- `<id>`: Item identifier (string)
- `<top_k>`: Number of results to return (integer, max: `similarity.max_top_k`)
- `using=` (optional): Search mode
  - `events`: Co-occurrence-based (event data only)
  - `vectors`: Vector distance-based (vector data only)
  - `fusion` (default): Hybrid co-occurrence × vector
- `filter=` (optional): Metadata filter expression. Use `key:value` (or `key=value`) for equality; `!=`, `>`, `>=`, `<`, `<=` for comparisons; and `key=in(value1|value2)` for membership. Comma-separated conditions are ANDed. Values are auto-typed (string, int, float, bool).
- `min_score=` (optional, default: 0.0): Minimum score threshold. Results with score < min_score are excluded from the response.
- `adaptive=` (optional): Adaptive fusion mode. When `on`, automatically adjusts the vector/co-occurrence weight balance based on item data density. Only applicable in fusion mode. Configurable via `similarity.adaptive_*` settings.

**Response Format**:
```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

**Example (fusion mode)**:
```
SIM item456 10 using=fusion
→ OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567
```

**Example (events only)**:
```
SIM item456 10 using=events
→ OK RESULTS 2
item101 0.9500
item789 0.8700
```

**Example (vectors only)**:
```
SIM item456 10 using=vectors
→ OK RESULTS 3
item789 0.9245
item202 0.8932
item555 0.8567
```

**Example (with filter)**:
```
SIM item456 10 filter=category:electronics
→ OK RESULTS 2
item789 0.9245
item101 0.8932
```

**Example (with min_score)**:
```
SIM item456 10 min_score=0.85
→ OK RESULTS 2
item789 0.9245
item101 0.8932
```

**Example (adaptive fusion)**:
```
SIM new_item 10 using=fusion adaptive=on
→ OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567
```

**Error Responses**:
- `ERROR Item not found: item456`
- `ERROR Invalid mode: must be events, vectors, or fusion`
- `ERROR Invalid top_k: must be > 0 and <= 1000`

**Notes**:
- Fusion mode combines vector similarity (weight: `similarity.fusion_alpha`) with co-occurrence scores (weight: `similarity.fusion_beta`)
- Results are cached if query cost exceeds `cache.min_query_cost_ms` (when cache is enabled)
- Cache is invalidated on VECSET (for vectors mode) or EVENT (for fusion mode)

---

### SIMV — Similarity search by vector

Find similar items based on a query vector.

**Syntax**:
```
SIMV <top_k> [filter=<expr>] [min_score=<float>] <f1> <f2> ... <fN>
```

**Parameters**:
- `<top_k>`: Number of results to return (integer)
- `filter=` (optional): Same as SIM filter. Metadata filter expression.
- `min_score=` (optional, default: 0.0): Minimum score threshold.
- `<f1> <f2> ... <fN>`: Query vector components (floats)

**Response Format**:
```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

**Example**:
```
SIMV 5 0.1 0.9 -0.2 0.5
→ OK RESULTS 2
item789 0.9800
item101 0.8200
```

**Example (with filter and min_score)**:
```
SIMV 5 filter=type:article min_score=0.7 0.1 0.9 -0.2 0.5
→ OK RESULTS 1
item789 0.9800
```

**Error Responses**:
- `ERROR Dimension mismatch: expected 768, got 512`
- `ERROR Invalid vector format`
- `ERROR Invalid top_k`

**Notes**:
- Dimension is auto-detected from the number of values
- Only vector similarity is used (fusion mode not supported for query vectors)
- Results are cached if query cost exceeds `cache.min_query_cost_ms`

---

## Admin Commands (MygramDB-compatible)

### AUTH — Authenticate connection

Authenticate the current connection with a password. Required when `security.requirepass` is configured.

**Syntax**:
```
AUTH <password>
```

**Parameters**:
- `<password>`: Server password (must match `security.requirepass` in config)

**Example**:
```bash
AUTH mysecretpassword
→ +OK

# Without auth, write commands are rejected:
VECSET item1 0.1 0.2 0.3
→ ERROR NOAUTH Authentication required
```

**Notes**:
- Authentication is per-connection (resets on disconnect)
- Read commands (SIM, SIMV, INFO, CONFIG SHOW) work without auth
- Write/admin commands (VECSET, DUMP SAVE/LOAD, SET) require auth when `requirepass` is set
- If `requirepass` is empty (default), AUTH is not needed

---

### INFO — Server statistics

Get comprehensive server information and statistics (Redis-style format).

**Syntax**:
```
INFO
```

**Response**:
```
OK INFO

# Server
version: 0.1.0
uptime_seconds: 3600

# Stats
total_commands_processed: 100000
total_connections_received: 150

# Memory
used_memory_bytes: 536870912
used_memory_human: 512.00 MB
memory_health: HEALTHY

# Data
id_count: 12345
ctx_count: 6789
vector_count: 12000
event_count: 987654

# Commandstats
cmd_event: 50000
cmd_vecset: 20000
cmd_sim: 25000
cmd_simv: 5000
```

**Memory Health**:
- `HEALTHY`: >20% system memory available
- `WARNING`: 10-20% available
- `CRITICAL`: <10% available

---

### CONFIG — Configuration management

**Commands**:
```
CONFIG HELP [path]
CONFIG SHOW [path]
CONFIG VERIFY
```

#### CONFIG HELP

Show configuration documentation.

**Example**:
```
CONFIG HELP events
→ +OK
Available paths under 'events':
  ctx_buffer_size  - Number of events to track per context (ring buffer size)
  decay_alpha      - Decay factor
  ...
END
```

`CONFIG HELP`, `CONFIG SHOW` and `CONFIG VERIFY` answer `+OK` followed by a
multi-line body and the terminator `END`. Read until `END` rather than until the
first blank line.

#### CONFIG SHOW

Display current configuration (passwords masked).

**Example**:
```
CONFIG SHOW events.ctx_buffer_size
→ +OK
50
END
```

#### CONFIG VERIFY

Validate configuration file (usable before server start).

`CONFIG VERIFY` requires a filepath and reports a summary of the parsed file:

**Response**:
```
CONFIG VERIFY /etc/nvecd/config.yaml
→ +OK
Configuration is valid
  Vectors:
    dimension: 768
    distance_metric: cosine
  Events:
    ctx_buffer_size: 50
    decay_interval_sec: 3600
  API:
    tcp: 127.0.0.1:11017
END
```

**Error Responses**:
- `ERROR CONFIG VERIFY requires a filepath`
- `ERROR Configuration validation failed: <reason>`

---

### DUMP — Snapshot management

**Commands**:
```
DUMP SAVE [<filepath>]
DUMP LOAD [<filepath>]
DUMP VERIFY [<filepath>]
DUMP INFO [<filepath>]
```

Single binary `.dmp` format, MygramDB-compatible.

#### DUMP SAVE

Save a complete snapshot to disk. The response depends on `snapshot.mode`.

**Example (`snapshot.mode: lock`)**:
```
DUMP SAVE /data/nvecd.nvec
→ OK DUMP_SAVED /data/nvecd.nvec
```

`OK DUMP_SAVED` means the whole durability handshake completed: the snapshot was
written, its sidecar was made durable, and the WAL was checkpointed and
truncated. A failure at any of those steps is reported as an error rather than
logged under an OK response, so the acknowledgement never outruns the data.

**Example (`snapshot.mode: fork`)**:
```
DUMP SAVE /data/nvecd.nvec
→ OK DUMP_SAVE_STARTED /data/nvecd.nvec
```

Fork mode acknowledges that the background child started, not that it finished.
Poll `DUMP STATUS` for the outcome.

**Without filepath**:
```
DUMP SAVE
→ OK DUMP_SAVED /var/lib/nvecd/snapshots/nvecd.snapshot
```

An argument-less save writes to `snapshot.default_filename`, resolved inside the
snapshot directory under the same path validation as a client-supplied name. If
that setting is empty, the name falls back to `snapshot_YYYYMMDD_HHMMSS.dmp`.

**Error Responses**:
- `ERROR Another snapshot save is already in progress`
- `ERROR Cannot save snapshot while a snapshot load is in progress`
- `ERROR Failed to save snapshot to /data/nvecd.nvec: <reason>`

#### DUMP LOAD

Load a snapshot from disk (the server becomes read-only during the load).

**Example**:
```
DUMP LOAD /data/nvecd.nvec
→ OK DUMP_LOADED /data/nvecd.nvec
```

A load makes the loaded snapshot the durable recovery base and discards the
pre-load WAL tail, so a later restart does not replay the mutations the operator
rolled back.

**Error Responses**:
- `ERROR File not found: /data/nvecd.nvec`
- `ERROR CRC mismatch: file may be corrupted`
- `ERROR Unsupported snapshot version`

#### DUMP VERIFY

Verify snapshot integrity without loading.

**Example**:
```
DUMP VERIFY /data/nvecd.nvec
→ OK DUMP_VERIFIED /data/nvecd.nvec
```

**Error Responses**:
- `ERROR Snapshot verification failed for /data/nvecd.nvec: <reason>`

#### DUMP INFO

Show snapshot metadata.

**Example**:
```
DUMP INFO /data/nvecd.nvec
→ OK DUMP_INFO /data/nvecd.nvec
version: 1
stores: 3
flags: 0
file_size: 536870912
timestamp: 1705564800
has_statistics: true
END
```

#### DUMP STATUS

Check the status of a background snapshot operation (fork-based).

**Syntax**:
```
DUMP STATUS
```

**Response**:
```
OK DUMP_STATUS
status: idle|in_progress|completed|failed
filepath: <path>
pid: <pid>
start_time: <timestamp>
end_time: <timestamp>
error: <message>
END
```

Which fields follow `status:` depends on the status itself: `idle` carries none,
`in_progress` adds `filepath`, `pid` and `start_time`, `completed` adds
`end_time` instead of `pid`, and `failed` adds `error` as well. Read until `END`.

**Example**:
```bash
DUMP STATUS
→ OK DUMP_STATUS
status: completed
filepath: /var/lib/nvecd/snapshots/dump_20250325_120000.nvec
start_time: 1711360800
end_time: 1711360802
END
```

**Notes**:
- Shows the state of the most recent background snapshot
- `in_progress`: Fork child is writing the snapshot
- `completed`: Last snapshot saved successfully
- `failed`: Last snapshot encountered an error
- `idle`: No snapshot has been started

---

### DEBUG — Debug mode

Per-connection debug mode. Shows detailed execution info for SIM commands.

**Commands**:
```
DEBUG ON
DEBUG OFF
```

#### DEBUG ON

Enable debug logging for this connection.

**Example**:
```
DEBUG ON
→ OK DEBUG_ON
```

#### DEBUG OFF

Disable debug logging for this connection.

**Example**:
```
DEBUG OFF
→ OK DEBUG_OFF
```

**Debug output example** (when DEBUG ON):
```
SIM item456 10 using=fusion
→ OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567
# DEBUG 4
mode: fusion
query_time_us: 850
candidates: 15
results: 3
```

The debug block is appended after the normal SIM/SIMV results for connections with
DEBUG mode enabled. The `# DEBUG` header carries the number of fields that
follow, so a stateful client can frame the block without parsing it. Fields:
- `mode`: search mode (`events`, `vectors`, `fusion`, or `vector` for SIMV)
- `query_time_us`: total query execution time in microseconds
- `candidates`: number of candidates produced before `min_score` filtering
- `results`: number of results returned to the client

---

## Cache Commands

### CACHE — Cache management

Query result cache management commands.

**Commands**:
```
CACHE STATS
CACHE CLEAR
CACHE ENABLE
CACHE DISABLE
```

#### CACHE STATS

Returns detailed cache statistics.

**Response**:
```
OK CACHE_STATS
cache_enabled: true
cache_entries: 342
cache_memory_bytes: 12845632
current_memory_mb: 12.25
total_queries: 1250
cache_hits: 985
cache_misses: 265
cache_misses_invalidated: 45
cache_misses_not_found: 220
cache_hit_rate: 0.7880
evictions: 15
ttl_expirations: 8
avg_hit_latency_ms: 0.125
avg_miss_latency_ms: 2.450
total_time_saved_ms: 2418.75
END
```

When no cache instance is configured, only the enabled flag and entry count are returned:
```
OK CACHE_STATS
cache_enabled: false
cache_entries: 0
END
```

**Statistics fields**:
- `cache_enabled`: Whether the cache is currently enabled (`true`/`false`)
- `cache_entries`: Number of cached entries
- `cache_memory_bytes`: Current cache memory usage in bytes
- `current_memory_mb`: Current cache memory usage in mebibytes
- `total_queries`: Total number of cache lookups
- `cache_hits`: Number of cache hits
- `cache_misses`: Total misses (invalidated + not found)
- `cache_misses_invalidated`: Misses due to invalidation (VECSET/EVENT)
- `cache_misses_not_found`: Misses due to key not in cache, TTL expiration, or decompression failure
- `cache_hit_rate`: Cache hit rate (0.0 to 1.0)
- `evictions`: Number of LRU evictions
- `ttl_expirations`: Number of entries removed due to TTL expiration
- `avg_hit_latency_ms`: Average cache lookup latency on hit
- `avg_miss_latency_ms`: Average cache lookup latency on miss
- `total_time_saved_ms`: Total query time saved by cache hits

#### CACHE CLEAR

Clear all cache entries.

**Response**:
```
OK CACHE_CLEARED
```

When no cache instance is configured, the response notes that there was nothing to clear:
```
OK CACHE_CLEARED (no cache)
```

#### CACHE ENABLE

Enable the cache at runtime.

**Response**:
```
OK CACHE_ENABLED
```

When no cache instance was configured at startup, the command is acknowledged but has no effect:
```
OK CACHE_ENABLED (no cache instance)
```

**Note**: A cache instance must be configured in `config.yaml` at startup. When no instance exists, runtime enabling has no effect.

#### CACHE DISABLE

Disable the cache at runtime. Existing entries are retained but no new lookups or insertions are served until the cache is re-enabled with `CACHE ENABLE`.

**Response**:
```
OK CACHE_DISABLED
```

When no cache instance was configured at startup, the command is acknowledged but has no effect:
```
OK CACHE_DISABLED (no cache instance)
```

**Cache Behavior**:
- SIM/SIMV query results are cached if `query_cost_ms >= min_query_cost_ms`
- Cache entries are invalidated on VECSET (for SIM queries) and EVENT (for fusion queries)
- LRU eviction occurs when `current_memory_bytes >= max_memory_bytes`
- Results are compressed with LZ4 to reduce memory usage

**Configuration** (`config.yaml`):
```yaml
cache:
  enabled: true               # Enable/disable cache
  max_memory_mb: 32           # Maximum cache memory
  min_query_cost_ms: 10.0     # Minimum query cost to cache
  ttl_seconds: 3600           # Cache entry TTL (0 = no TTL)
  compression_enabled: true   # Enable LZ4 compression
```

---

## Error Responses

All errors follow a consistent format:

```
ERROR <error_message>
```

**Common error examples**:
- `ERROR Unknown command: FOO`
- `ERROR Invalid argument count`
- `ERROR Item not found: item123`
- `ERROR Dimension mismatch: expected 768, got 512`
- `ERROR Invalid score: must be 0-100`
- `ERROR File not found: /data/dump.dmp`
- `ERROR CRC mismatch: file may be corrupted`

---

## Similarity Modes

### `events` - Event-based (Co-occurrence)

Uses only co-occurrence scores from event data. Best for collaborative filtering without content features.

**Use case**: "Users who interacted with this also interacted with..."

### `vectors` - Vector-based

Uses only vector similarity (dot product or cosine). Best for content-based recommendations.

**Use case**: "Items with similar content/features..."

### `fusion` - Fusion Search (SIM only)

Combines vector similarity (weight: `similarity.fusion_alpha`) with co-occurrence scores (weight: `similarity.fusion_beta`).

**Use case**: Hybrid recommendations combining content similarity + user behavior.

**Formula**:
```
fusion_score = (alpha × vector_similarity) + (beta × co_occurrence_score)
where alpha + beta = 1.0
```

**Note**: `fusion` mode is only available for `SIM` command (not `SIMV`).

---

## Best Practices

### Performance Tips

1. **Use appropriate top_k**: Lower values are faster
2. **Enable caching**: Set `cache.enabled=true` for repeated queries
3. **Tune fusion weights**: Adjust `fusion_alpha` and `fusion_beta` based on your use case
4. **Use events mode for cold items**: Items without vectors can still be recommended via events
5. **Monitor cache hit rate**: Use `CACHE STATS` to check performance

### Data Management

1. **Regular snapshots**: Use `DUMP SAVE` for backups
2. **Verify snapshots**: Use `DUMP VERIFY` before loading
3. **Monitor memory**: Use `INFO` to track memory usage
4. **Decay configuration**: Adjust `decay_interval_sec` based on your data freshness needs

### Debugging

1. **Enable DEBUG mode**: Use `DEBUG ON` to see query execution details
2. **Check INFO stats**: Monitor command counts and performance
3. **Test with small datasets**: Verify behavior before scaling

---

## Next Steps

- See [Configuration Guide](configuration.md) for tuning options
- See [Snapshot Management](snapshot.md) for persistence details
- See [Client Library Guide](libnvecdclient.md) for programmatic access
- See [Performance Guide](performance.md) for optimization tips
