# Configuration

nvecd reads a single YAML file at startup. This page lists every key the parser accepts, section by section, and describes the cross-key rules and operational consequences the option tables do not carry.

The option tables below are rendered from `src/config/config-schema.json`, which is the authority for every type, default, range and description. Editing a table by hand has no effect; correct the schema and regenerate.

## Supplying a configuration file

```bash
nvecd -c /etc/nvecd/config.yaml
nvecd /etc/nvecd/config.yaml
nvecd -t -c /etc/nvecd/config.yaml
```

| Flag | Meaning |
|---|---|
| `-c`, `--config <file>` | configuration file path |
| `-t`, `--config-test` | validate the file, print a summary and exit |
| `-h`, `--help` | print usage and exit |
| `-v`, `--version` | print the version and exit |

The path may also be given as a positional argument; giving two config files is an error. Started with no file at all, the server runs entirely on the built-in defaults shown in the tables below, and `--config-test` without a file is an error.

`examples/config.yaml` is rendered from the same schema and carries every key at its default.

## How a file is validated

Loading happens in three stages, and the first failure aborts startup.

1. The YAML is parsed and converted to JSON.
2. The JSON is checked against the embedded schema: types, enumerations and per-key numeric ranges.
3. The parsed structure is checked semantically, which is where the cross-key rules listed under each section live.

The schema sets `additionalProperties: false` at the root and in every section, so a misspelled key is rejected rather than silently ignored. Sections are optional; an absent section takes its defaults in full. A value that is well-formed YAML but does not fit the type it is read into is reported against its key, for example `Invalid value for events.ctx_buffer_size: '...' is out of range for this setting`.

## `events`

Event ingestion and the co-occurrence graph. See [events-and-co-occurrence.md](./events-and-co-occurrence.md).

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

Semantic rules beyond the ranges above: `ctx_buffer_size` must be greater than zero, `min_support` must not be negative, `decay_alpha` must lie in 0.0–1.0, `temporal_half_life_sec` must be greater than zero, and `negative_weight` must lie in 0.0–1.0.

`temporal_half_life_sec` has no effect unless `temporal_cooccurrence` is true, and `negative_weight` none unless `negative_signals` is. `max_contexts`, `max_neighbors_per_item` and `min_support` are the three bounds on graph growth; left at their defaults the graph is unbounded and grows with the data.

## `vectors`

<!-- BEGIN GENERATED: options vectors -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `default_dimension` | int | 768 | Default vector dimension (number of features per vector) (1-4096) |
| `distance_metric` | string | "cosine" | Distance metric for similarity search (`cosine` `dot` `l2`) |
<!-- END GENERATED: options vectors -->

`default_dimension` must be greater than zero. It fixes the accepted dimension: a `VECSET` whose vector has a different length is rejected with a dimension mismatch. Changing it after data exists means the existing snapshot no longer matches the configuration, so it is a rebuild rather than a tuning change. See [vector-search.md](./vector-search.md).

## `similarity`

Search behaviour, fusion weights and the ANN index. See [vector-search.md](./vector-search.md) and [fusion.md](./fusion.md).

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

Cross-key rules:

- `default_top_k` must be greater than zero and `max_top_k` must be at least `default_top_k`.
- `adaptive_min_alpha` must not exceed `adaptive_max_alpha`, and `adaptive_maturity_threshold` must be greater than zero. The three take effect only when `adaptive_fusion` is true or a request passes `adaptive=on`.
- The IVF keys are validated only when the index is IVF, that is when `index_type: ivf` or `ivf_enabled: true`. `ivf_nprobe` and `ivf_train_threshold` must be greater than zero, and `ivf_nprobe` must not exceed `ivf_nlist` unless `ivf_nlist` is `0`.
- The HNSW keys are validated only when `index_type: hnsw`. `hnsw_m` must be at least 2, and both `ef` values greater than zero.
- `ivf_enabled` is the older spelling of `index_type: ivf`. It is applied only when `index_type` is still `flat`; with `index_type` set to `hnsw` or `ivf`, the flag has no effect even though the IVF keys are still range-checked.
- `fusion_alpha` and `fusion_beta` are independent numbers, not two halves of one budget; they are not required to sum to 1.

`hnsw_max_elements` is reserved at startup, and its maximum is the largest index the snapshot loader accepts — reserving beyond it produces an index that cannot be reloaded.

## `snapshot`

See [persistence.md](./persistence.md).

<!-- BEGIN GENERATED: options snapshot -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `dir` | string | "/var/lib/nvecd/snapshots" | Snapshot directory path |
| `default_filename` | string | "nvecd.nvec" | Filename an argument-less DUMP SAVE writes inside the snapshot directory; validated like a client-supplied path, so an absolute or escaping name is refused (empty = fall back to a timestamped name). Must end in .nvec or .dmp, or startup will not treat the file as a recovery candidate |
| `interval_sec` | int | 0 | Snapshot interval in seconds (0 = disabled) (0-86400) |
| `retain` | int | 3 | Number of automatic snapshots to retain; manual snapshots are never removed (0 = keep every file) (0-100) |
| `mode` | string | "fork" | Snapshot consistency mode: fork (COW, non-blocking) or lock (global write lock, blocking) (`fork` `lock`) |
<!-- END GENERATED: options snapshot -->

`interval_sec` and `retain` must not be negative, and `mode` must be one of the two values.

`default_filename` is validated exactly like a client-supplied path, so an absolute or escaping name is refused rather than resolved against `dir`; an empty value falls back to a timestamped name. `retain` prunes only snapshots the scheduler wrote — a file produced by an explicit `DUMP SAVE` is never removed.

`mode: fork` writes in a forked child over a copy-on-write image and does not block writers, at the cost of a memory spike. `mode: lock` blocks writes for the duration and reports the finished file synchronously. The mode changes what `DUMP SAVE` answers: `OK DUMP_SAVE_STARTED` for `fork`, `OK DUMP_SAVED` for `lock`.

## `performance`

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

`thread_pool_size` must not be negative, and `max_connections` and `connection_timeout_sec` must be greater than zero.

These keys are not TCP-only. The HTTP server derives its own limits from them: `thread_pool_size` becomes its worker count, `max_connections` and `max_connections_per_ip` its admission limits, `max_query_length` its maximum request body, and its listen queue is `max_connections − thread_pool_size`, with a floor of one. A request body above `max_query_length` is refused on both surfaces.

## `api`

### `api.tcp`

<!-- BEGIN GENERATED: options api.tcp -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `bind` | string | "127.0.0.1" | TCP bind address ("0.0.0.0" listens on every interface) |
| `port` | int | 11017 | TCP port (1-65535) |
<!-- END GENERATED: options api.tcp -->

The TCP listener is always started; there is no key that disables it.

### `api.http`

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

`port` is range-checked only when `enable` is true; `timeout_sec` is checked either way. An empty `cors_allow_origin` omits the header rather than sending `null`, so `enable_cors` alone does not permit any origin. A failed HTTP bind is logged and the server keeps running on TCP alone. See [http-api.md](./http-api.md).

### `api.unix_socket`

<!-- BEGIN GENERATED: options api.unix_socket -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `path` | string | "" | Unix socket path (empty string = disabled) |
<!-- END GENERATED: options api.unix_socket -->

The Unix socket serves the same protocol as the TCP port, but bypasses `network.allow_cidrs`, `performance.max_connections_per_ip` and rate limiting — filesystem permissions on the socket file are its access control. A failed Unix-socket bind is logged as a warning and does not stop the server.

### `api.rate_limiting`

<!-- BEGIN GENERATED: options api.rate_limiting -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `enable` | bool | false | Enable rate limiting |
| `capacity` | int | 100 | Maximum number of tokens per client (burst size) (1-10000) |
| `refill_rate` | int | 10 | Tokens added per second per client (1-1000) |
| `max_clients` | int | 10000 | Maximum number of tracked clients (for memory management) (10-100000) |
<!-- END GENERATED: options api.rate_limiting -->

The three numeric keys are checked for positivity only when `enable` is true. Rate limiting keys on the client address, so it does not apply to Unix-socket connections. An HTTP request over the limit is answered `429`.

## `network`

<!-- BEGIN GENERATED: options network -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `allow_cidrs` | list | [] | Allow CIDR list (empty = deny all) |
<!-- END GENERATED: options network -->

The list is fail-closed: an empty list denies every address, so a deployment that omits it accepts no TCP or HTTP traffic at all. An entry that does not parse as a CIDR is logged and skipped, leaving the remaining entries in force. The check applies to the TCP listener and to every HTTP route, and does not apply to the Unix socket.

```yaml
network:
  allow_cidrs:
    - "127.0.0.1/32"
    - "10.0.0.0/8"
```

## `logging`

<!-- BEGIN GENERATED: options logging -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `level` | string | "info" | Log level (`trace` `debug` `info` `warn` `error`) |
| `json` | bool | true | JSON format output |
| `file` | string | "" | Log file path (empty string = stdout, path = file output) |
<!-- END GENERATED: options logging -->

`level` and `json` are both changeable at runtime; `file` is not, because reopening the handle is a restart-level change.

## `cache`

The query cache. See [caching.md](./caching.md).

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

`max_memory_mb` must be greater than zero when `enabled` is true, `ttl_seconds` must not be negative, `min_query_cost_ms` must not be negative, and `eviction_batch_size` must be greater than zero.

`enabled`, `min_query_cost_ms` and `ttl_seconds` are changeable at runtime; the other three are fixed at startup because they decide an allocation or a stored representation.

`min_query_cost_ms` is the reason a cache can show a low hit rate on a small corpus: a search that finishes faster than the threshold is never stored, so there is nothing to hit.

## `security`

<!-- BEGIN GENERATED: options security -->
| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `requirepass` | string | "" | Required password for write/admin commands (empty = no auth) |
<!-- END GENERATED: options security -->

Setting `requirepass` closes both surfaces at once: TCP connections must issue `AUTH` before a write or admin command, and gated HTTP routes require `Authorization: Bearer <password>` or a `Basic` credential whose password matches. Read commands and the health probes stay open on both. The value is redacted to `***` by `CONFIG SHOW` and by the `security.requirepass` runtime variable.

Because the password sits in a plain YAML file, the file's permissions are what protects it.

## `wal`

The write-ahead log. See [persistence.md](./persistence.md).

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

`dir` must not be empty when `enabled` is true. `sync_interval_ms` has no effect while `sync_on_write` is true, which is the trade of durability against write throughput.

`include_vectors` interacts directly with recovery. With it false, `VECSET` records carry no vector body and are not written at all, so a restart restores vectors only as far as the last snapshot; any `VECDEL` or `METASET` in the log that refers to a vector the snapshot does not contain is skipped, counted, and reported by `INFO` as `wal_replay_records_skipped`. Turning it off is a decision to make snapshots the durability boundary for vector data, and it keeps the server startable rather than failing recovery on the resulting gap.

## A minimal configuration

Enough to run and be reachable from the local host:

```yaml
vectors:
  default_dimension: 384

api:
  tcp:
    bind: "127.0.0.1"
    port: 11017

network:
  allow_cidrs:
    - "127.0.0.1/32"
```

Everything else takes its default. Without the `network` section the server starts but refuses every connection.

## A hardened configuration

Authentication on, both surfaces bound to a private interface, rate limiting on, WAL and periodic snapshots on, and the graph bounded:

```yaml
events:
  ctx_buffer_size: 100
  max_contexts: 500000
  max_neighbors_per_item: 200
  min_support: 0.5
  decay_interval_sec: 3600
  decay_alpha: 0.99

vectors:
  default_dimension: 768
  distance_metric: "cosine"

similarity:
  default_top_k: 20
  max_top_k: 200
  index_type: "hnsw"
  hnsw_m: 32
  hnsw_ef_construction: 400
  hnsw_ef_search: 100
  adaptive_fusion: true
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 50

snapshot:
  dir: "/var/lib/nvecd/snapshots"
  default_filename: "nvecd.nvec"
  interval_sec: 900
  retain: 12
  mode: "fork"

wal:
  enabled: true
  dir: "/var/lib/nvecd/wal"
  max_file_size: 134217728
  sync_on_write: false
  sync_interval_ms: 100
  include_vectors: true

performance:
  thread_pool_size: 0
  max_connections: 2000
  max_connections_per_ip: 50
  connection_timeout_sec: 120
  max_query_length: 1048576

api:
  tcp:
    bind: "10.0.1.5"
    port: 11017
  http:
    enable: true
    bind: "10.0.1.5"
    port: 8080
    timeout_sec: 10
  rate_limiting:
    enable: true
    capacity: 200
    refill_rate: 50
    max_clients: 20000

network:
  allow_cidrs:
    - "10.0.1.0/24"

security:
  requirepass: "replace-this"

cache:
  enabled: true
  max_memory_mb: 512
  min_query_cost_ms: 5.0
  ttl_seconds: 600

logging:
  level: "info"
  json: true
  file: "/var/log/nvecd/nvecd.log"
```

Check it before restarting:

```bash
$ nvecd -t -c /etc/nvecd/config.yaml
Configuration file is valid

Configuration summary:
  Events:
    ctx_buffer_size: 100
...
```

## Inspecting configuration at runtime

`CONFIG SHOW` over TCP prints the running configuration as YAML, optionally narrowed to a section:

```text
> CONFIG SHOW cache
+OK
compression_enabled: true
enabled: true
eviction_batch_size: 10
max_memory_mb: 32
min_query_cost_ms: 10.0
ttl_seconds: 3600
END
```

`requirepass` appears as `***`, alongside a derived `auth_enabled` flag that has no counterpart in the file or the schema. `CONFIG HELP <path>` prints the type, default, allowed values and description of one key, read from the same schema the loader validates against.

`GET /config` over HTTP returns a much narrower summary — bind addresses, ports, the password and the CIDR list itself are all withheld — so operational introspection belongs on the TCP surface. Both are described in [protocol.md](./protocol.md) and [http-api.md](./http-api.md).

`CONFIG VERIFY <file>` validates another file without applying it. The path is resolved inside the directory the running configuration came from, or inside `snapshot.dir` for a server started without a configuration file, and anything outside that root is refused.

## Changing configuration at runtime

Five variables can be changed on a running server:

| Variable | Effect of a change |
|---|---|
| `logging.level` | takes effect on the next log record |
| `logging.json` | switches the record format |
| `cache.enabled` | enables or disables the query cache |
| `cache.min_query_cost_ms` | changes which searches are worth caching |
| `cache.ttl_seconds` | changes entry lifetime |

Every other key on this page is readable through `GET` and `SHOW VARIABLES` but rejects a write with `Variable '<name>' is immutable (requires restart)`.

```text
> SET cache.ttl_seconds 600
+OK

> SET logging.level debug
+OK

> GET cache.enabled
$4
true
```

Booleans accept `true`/`false`, `on`/`off`, `1`/`0` and `yes`/`no`, and are stored canonically as `true` or `false`, so a value written with an alias reads back in the canonical spelling. `SHOW VARIABLES LIKE <prefix>%` lists names with their values and mutability. The `performance.` prefix is also accepted as `perf.` on input, but introspection always reports `performance.`.

A runtime change is not written back to the file, so a restart returns to the configured value. `CACHE ENABLE` and `CACHE DISABLE` are shorthands for setting `cache.enabled`, and the HTTP `/cache/enable` and `/cache/disable` routes do the same.
