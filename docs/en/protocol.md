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
```

---

## Protocol Format

- **Transport**: Text-based line protocol (UTF-8)
- **Request**: `COMMAND args...\r\n` (accepts both `\r\n` and `\n`)
- **Response**: `OK data...\r\n` or `ERROR message\r\n`
- **Max request size**: 16MB (configurable)

### Response Format

**Success**:
```
OK [data]\r\n
```
or Redis-style: `+OK [data]\r\n`

**Error**:
```
ERROR <message>\r\n
```
or Redis-style: `-ERR <message>\r\n` or `(error) <message>\r\n`

---

## Command Categories

- **Core Commands**: EVENT, VECSET, SIM, SIMV (nvecd-specific)
- **Admin Commands**: INFO, CONFIG, DUMP, DEBUG (MygramDB-compatible)
- **Cache Commands**: CACHE (query result cache management)

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
→ OK

# State event (like ON)
EVENT user123 SET like:item456 100
→ OK

# State event (like OFF)
EVENT user123 SET like:item456 0
→ OK

# Weighted bookmark (high priority)
EVENT user123 SET bookmark:item789 100
→ OK

# Change bookmark priority (medium)
EVENT user123 SET bookmark:item789 50
→ OK

# Delete bookmark
EVENT user123 DEL bookmark:item789
→ OK
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
→ OK (both succeed, second is deduped)

# SET allows state transitions
EVENT user1 SET bookmark:item1 100  # High priority
EVENT user1 SET bookmark:item1 50   # Medium priority (stored)
EVENT user1 SET bookmark:item1 50   # Duplicate (ignored)
→ OK

# DEL is idempotent
EVENT user1 DEL like:item1
EVENT user1 DEL like:item1  # Already deleted, ignored
→ OK
```

**Error Responses**:
- `(error) Invalid EVENT type: <type> (must be ADD, SET, or DEL)`
- `(error) EVENT ADD requires 4 arguments: <ctx> ADD <id> <score>`
- `(error) EVENT SET requires 4 arguments: <ctx> SET <id> <score>`
- `(error) EVENT DEL requires 3 arguments: <ctx> DEL <id>`
- `(error) Invalid score: must be integer`
- `(error) Context cannot be empty`
- `(error) ID cannot be empty`

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
→ OK
```

**Example with 768-dimensional vector**:
```
VECSET item789 0.11 0.98 -0.22 0.44 ... (768 values)
→ OK
```

**Error Responses**:
- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid vector format`
- `(error) Invalid argument count`

**Notes**:
- Dimension is auto-detected from the number of values
- All vectors must have the same dimension (default: 768, configurable via `vectors.default_dimension`)
- Vectors are automatically normalized based on `vectors.distance_metric` setting

---

### SIM — Similarity search by ID

Find similar items based on an existing item's vector and co-occurrence data.

**Syntax**:
```
SIM <id> <top_k> [using=events|vectors|fusion]
```

**Parameters**:
- `<id>`: Item identifier (string)
- `<top_k>`: Number of results to return (integer, max: `similarity.max_top_k`)
- `using=` (optional): Search mode
  - `events`: Co-occurrence-based (event data only)
  - `vectors`: Vector distance-based (vector data only)
  - `fusion` (default): Hybrid co-occurrence × vector

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
item101 0.95
item789 0.87
```

**Example (vectors only)**:
```
SIM item456 10 using=vectors
→ OK RESULTS 3
item789 0.9245
item202 0.8932
item555 0.8567
```

**Error Responses**:
- `(error) Item not found: item456`
- `(error) Invalid mode: must be events, vectors, or fusion`
- `(error) Invalid top_k: must be > 0 and <= 1000`

**Notes**:
- Fusion mode combines vector similarity (weight: `similarity.fusion_alpha`) with co-occurrence scores (weight: `similarity.fusion_beta`)
- Results are cached if query cost exceeds `cache.min_query_cost_ms` (when cache is enabled)
- Cache is invalidated on VECSET (for vectors mode) or EVENT (for fusion mode)

---

### SIMV — Similarity search by vector

Find similar items based on a query vector.

**Syntax**:
```
SIMV <top_k> <f1> <f2> ... <fN>
```

**Parameters**:
- `<top_k>`: Number of results to return (integer)
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
item789 0.98
item101 0.82
```

**Error Responses**:
- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid vector format`
- `(error) Invalid top_k`

**Notes**:
- Dimension is auto-detected from the number of values
- Only vector similarity is used (fusion mode not supported for query vectors)
- Results are cached if query cost exceeds `cache.min_query_cost_ms`

---

## Admin Commands (MygramDB-compatible)

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
events:
  ctx_buffer_size: Ring buffer size per context (default: 50)
  decay_interval_sec: Decay interval in seconds (default: 3600)
  decay_alpha: Decay factor 0.0-1.0 (default: 0.99)
```

#### CONFIG SHOW

Display current configuration (passwords masked).

**Example**:
```
CONFIG SHOW events.ctx_buffer_size
→ +OK
events:
  ctx_buffer_size: 50
```

#### CONFIG VERIFY

Validate configuration file (usable before server start).

**Response**:
```
+OK Configuration is valid
```

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

Save complete snapshot to disk.

**Example**:
```
DUMP SAVE /data/nvecd.dmp
→ +OK Snapshot saved: /data/nvecd.dmp (512.3 MB) in 2.35s
```

**Without filepath (auto-generated name)**:
```
DUMP SAVE
→ +OK Snapshot saved: /var/lib/nvecd/snapshots/auto_20251118_143000.dmp (512.3 MB) in 2.35s
```

#### DUMP LOAD

Load snapshot from disk (server becomes read-only during load).

**Example**:
```
DUMP LOAD /data/nvecd.dmp
→ +OK Snapshot loaded: /data/nvecd.dmp (5000 events, 2000 vectors) in 1.23s
```

**Error Responses**:
- `(error) File not found: /data/nvecd.dmp`
- `(error) CRC mismatch: file may be corrupted`
- `(error) Unsupported snapshot version`

#### DUMP VERIFY

Verify snapshot integrity without loading.

**Example**:
```
DUMP VERIFY /data/nvecd.dmp
→ +OK Snapshot is valid (CRC32: 0x12345678)
```

#### DUMP INFO

Show snapshot metadata (version, size, CRC32, record counts).

**Example**:
```
DUMP INFO /data/nvecd.dmp
→ +OK INFO
version: 1
flags: 0
timestamp: 2025-11-18T14:30:00Z
event_store_count: 5000
co_occurrence_count: 1500
vector_store_count: 2000
file_size_bytes: 536870912
file_size_human: 512.00 MB
crc32: 0x12345678
```

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
→ OK Debug mode enabled
```

#### DEBUG OFF

Disable debug logging for this connection.

**Example**:
```
DEBUG OFF
→ OK Debug mode disabled
```

**Debug output example** (when DEBUG ON):
```
SIM item456 10 fusion
→ OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567

# DEBUG
query_time_us: 850
event_search_time_us: 320
vector_search_time_us: 410
fusion_time_us: 120
mode: fusion
event_candidates: 15
vector_candidates: 12
```

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
total_queries: 1250
cache_hits: 985
cache_misses: 265
cache_misses_invalidated: 45
cache_misses_not_found: 220
hit_rate: 0.7880
current_entries: 342
current_memory_bytes: 12845632
current_memory_mb: 12.25
evictions: 15
avg_hit_latency_ms: 0.125
avg_miss_latency_ms: 2.450
time_saved_ms: 2418.75
```

**Statistics fields**:
- `total_queries`: Total number of cache lookups
- `cache_hits`: Number of cache hits
- `cache_misses`: Total misses (invalidated + not found)
- `cache_misses_invalidated`: Misses due to invalidation (VECSET/EVENT)
- `cache_misses_not_found`: Misses due to key not in cache
- `hit_rate`: Cache hit rate (0.0 to 1.0)
- `current_entries`: Number of cached entries
- `current_memory_mb`: Current cache memory usage
- `evictions`: Number of LRU evictions
- `avg_hit_latency_ms`: Average cache lookup latency on hit
- `avg_miss_latency_ms`: Average cache lookup latency on miss
- `time_saved_ms`: Total query time saved by cache hits

#### CACHE CLEAR

Clear all cache entries.

**Response**:
```
OK CACHE CLEARED
```

#### CACHE ENABLE

Enable cache (no-op if cache already initialized).

**Response**:
```
OK CACHE ENABLED
```

**Error** (if cache not initialized at startup):
```
-ERR Cache was not initialized at startup
```

**Note**: Cache must be enabled in config.yaml at startup. Runtime enabling is only possible if `cache.enabled=true` in config.

#### CACHE DISABLE

Runtime cache disable is **not supported**.

**Response**:
```
-ERR Runtime cache disable not supported. Set cache.enabled=false in config and restart.
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
(error) <error_message>
```

or Redis-style:

```
-ERR <error_message>
```

**Common error examples**:
- `(error) Unknown command: FOO`
- `(error) Invalid argument count`
- `(error) Item not found: item123`
- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid score: must be 0-100`
- `(error) File not found: /data/dump.dmp`
- `(error) CRC mismatch: file may be corrupted`

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
