# Protocol Reference

Nvecd uses a simple text-based protocol over TCP (similar to memcached/Redis).

## Connection

Connect to Nvecd via TCP:

```bash
telnet localhost 11017
```

Or use `nc`:

```bash
nc localhost 11017
```

## Command Format

Commands are text-based, one command per line. Responses are terminated with newline.

---

## EVENT Command

Record an event associating a context with an ID.

### Syntax

```
EVENT <ctx> <id> <score>
```

- `<ctx>`: Context identifier (string)
- `<id>`: Item identifier (string)
- `<score>`: Event score (integer, 0-100)

### Example

```
EVENT user123 item456 95
```

### Response

```
OK
```

### Error Responses

- `(error) Invalid score: must be 0-100`
- `(error) Context buffer overflow`

---

## VECSET Command

Register or update a vector for an item.

### Syntax

```
VECSET <id> <dimension> <v1> <v2> ... <vN>
```

- `<id>`: Item identifier (string)
- `<dimension>`: Vector dimension (integer)
- `<v1> <v2> ... <vN>`: Vector components (floats)

### Example

```
VECSET item456 3 0.1 0.5 0.8
```

### Response

```
OK
```

### Error Responses

- `(error) Dimension mismatch: expected 768, got 512`
- `(error) Invalid vector format`

---

## SIM Command

Find similar items based on an existing item's vector and co-occurrence data.

### Syntax

```
SIM <id> <top_k> <mode>
```

- `<id>`: Item identifier (string)
- `<top_k>`: Number of results to return (integer)
- `<mode>`: Similarity mode (`dot`, `cosine`, or `fusion`)

### Example

```
SIM item456 10 fusion
```

### Response

```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

Example:
```
OK RESULTS 3
item789 0.9245
item101 0.8932
item202 0.8567
```

### Error Responses

- `(error) Item not found: item456`
- `(error) Invalid mode: must be dot, cosine, or fusion`

---

## SIMV Command

Find similar items based on a query vector.

### Syntax

```
SIMV <dimension> <v1> <v2> ... <vN> <top_k> <mode>
```

- `<dimension>`: Vector dimension (integer)
- `<v1> <v2> ... <vN>`: Query vector components (floats)
- `<top_k>`: Number of results to return (integer)
- `<mode>`: Similarity mode (`dot` or `cosine`)

### Example

```
SIMV 3 0.1 0.5 0.8 10 cosine
```

### Response

```
OK RESULTS <count>
<id1> <score1>
<id2> <score2>
...
```

### Error Responses

- `(error) Dimension mismatch`
- `(error) Invalid mode: must be dot or cosine (fusion not supported for SIMV)`

---

## INFO Command

Get comprehensive server information and statistics (Redis-style format).

### Syntax

```
INFO
```

### Response

Returns server information in Redis-style key-value format with multiple sections:

```
OK INFO

# Server
version: 0.1.0
uptime_seconds: 3600

# Stats
total_commands_processed: 10000
total_connections_received: 150

# Commandstats
cmd_event: 5000
cmd_vecset: 2000
cmd_sim: 2500
cmd_simv: 500

# Memory
event_store_contexts: 100
event_store_events: 5000
vector_store_vectors: 2000
co_occurrence_pairs: 1500
```

---

## CONFIG Command

Configuration management commands.

### CONFIG HELP

Show available configuration commands.

```
CONFIG HELP
```

### CONFIG SHOW

Display current configuration.

```
CONFIG SHOW
```

Response:
```
OK CONFIG
server.host: 127.0.0.1
server.port: 11017
server.thread_pool_size: 4
event_store.ctx_buffer_size: 100
event_store.decay_factor: 0.95
vector_store.default_dimension: 768
...
```

### CONFIG VERIFY

Verify configuration file syntax.

```
CONFIG VERIFY
```

Response:
```
OK Configuration is valid
```

---

## DUMP Command

Snapshot persistence commands.

### DUMP SAVE

Save a snapshot to disk.

```
DUMP SAVE [filepath]
```

If `filepath` is omitted, a timestamped filename is generated automatically.

Response:
```
OK Snapshot saved: /path/to/dump_20250118_120000.nvec
```

### DUMP LOAD

Load a snapshot from disk.

```
DUMP LOAD <filepath>
```

Response:
```
OK Snapshot loaded: 5000 events, 2000 vectors
```

### DUMP VERIFY

Verify snapshot integrity.

```
DUMP VERIFY <filepath>
```

Response:
```
OK Snapshot is valid (CRC32: 0x12345678)
```

### DUMP INFO

Display snapshot metadata.

```
DUMP INFO <filepath>
```

Response:
```
OK INFO
version: 1
event_store_count: 5000
vector_store_count: 2000
co_occurrence_count: 1500
file_size: 1048576
created_at: 2025-01-18T12:00:00
```

---

## DEBUG Command

Enable or disable debug output for the current connection.

### DEBUG ON

Enable debug logging.

```
DEBUG ON
```

Response:
```
OK Debug mode enabled
```

### DEBUG OFF

Disable debug logging.

```
DEBUG OFF
```

Response:
```
OK Debug mode disabled
```

---

## Error Responses

All errors follow the format:

```
(error) <error_message>
```

Examples:
- `(error) Unknown command: FOO`
- `(error) Invalid argument count`
- `(error) Item not found: item123`
- `(error) Dimension mismatch: expected 768, got 512`

---

## Similarity Modes

### `dot` - Dot Product

Raw dot product between vectors. Higher values indicate greater similarity.

### `cosine` - Cosine Similarity

Normalized dot product (range: -1.0 to 1.0). Measures angle between vectors.

### `fusion` - Fusion Search (SIM only)

Combines vector similarity with co-occurrence scores from event data. Best for hybrid recommendation systems.

**Note**: `fusion` mode is only available for `SIM` command (not `SIMV`).

---

## Next Steps

- See [Configuration Guide](configuration.md) for configuration options
- See [Snapshot Management](snapshot.md) for persistence details
