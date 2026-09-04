# TCP protocol

nvecd serves a line-delimited text protocol on its TCP port and, when configured, on a Unix domain socket. This page is the reference for every command: its arity, its option tokens, the exact bytes the server sends back, and the privilege it requires.

## Connecting

The TCP listener binds `api.tcp.bind` (default `127.0.0.1`) on `api.tcp.port` (default `11017`). A peer whose address does not match `network.allow_cidrs` is disconnected without a response, and an empty `network.allow_cidrs` denies every address, so a reachable deployment lists at least one CIDR. See [configuration.md](./configuration.md).

Any line-oriented client works. With `nc`:

```bash
$ printf 'VECSET item1 0.1 0.2 0.3 0.4\nSIM item1 5 using=vectors\n' | nc 127.0.0.1 11017
OK VECSET
OK RESULTS 1
item2 0.9940
```

With `telnet`, for an interactive session:

```bash
$ telnet 127.0.0.1 11017
Trying 127.0.0.1...
Connected to localhost.
Escape character is '^]'.
INFO
OK INFO
...
END
```

The server sends no banner on connect; the first bytes a client receives are the response to its first command.

When `api.unix_socket.path` is set, the same protocol is served on that socket. The socket skips the CIDR check entirely — filesystem permissions on the socket file are the access control — and per-IP connection limits and rate limiting do not apply to it:

```bash
$ printf 'INFO\n' | nc -U /var/run/nvecd.sock
```

The shipped [client library](./client-library.md) and `nvecd-cli` speak this protocol; both are described below.

## Framing

A request is one line terminated by `\n` or `\r\n`. A command cannot span lines: an embedded newline or an embedded NUL byte is rejected rather than silently truncated. Several commands may be pipelined into one write; the server answers them in order.

A response is a sequence of CRLF-terminated lines with exactly one trailing CRLF. Handler bodies that build their text with bare `\n` are normalised before the bytes leave the socket, so a client may split on `\r\n` unconditionally.

A request longer than `performance.max_query_length` (default 1 MiB) is answered with `ERROR Request too large` and, when no newline has arrived within the limit, `ERROR Request too large (no newline detected)`. The same `ERROR Request too large` also covers a connection that exhausts the reactor's process-wide buffer budget, `performance.reactor_max_total_buffered_bytes`. When the worker pool cannot accept a request for processing, the reply is `ERROR Server busy`. Both conditions close the connection once the reply has been flushed.

Responses take five shapes:

| Shape | Example | Used by |
|---|---|---|
| Status line | `OK VECSET` | writes, `CACHE`, `DEBUG` |
| Result set | `OK RESULTS <n>` followed by `<id> <score>` lines | `SIM`, `SIMV` |
| Block | `+OK` or `OK <VERB>`, body lines, then `END` | `INFO`, `CONFIG`, `DUMP INFO`, `DUMP STATUS`, `CACHE STATS` |
| RESP bulk or array | `$<len>` then the value; `*<n>` then `$<len>`/value pairs | `GET`, `SHOW VARIABLES` |
| Error | `ERROR <message>` | any failure |

Scores are rendered with exactly four decimal places (`0.9940`). The HTTP surface rounds to the same precision, so the two surfaces report the same number for the same query.

## Authentication and privileges

Every command carries one of three privilege levels. When `security.requirepass` is empty, all three are open. When it is set, only read commands are served on an unauthenticated connection; write and admin commands are refused with `ERROR NOAUTH Authentication required` until `AUTH` succeeds on that connection.

| Privilege | Commands |
|---|---|
| Read | `SIM`, `SIMV`, `INFO`, `CONFIG HELP`, `CONFIG SHOW`, `CACHE STATS`, `DEBUG ON`, `DEBUG OFF`, `GET`, `SHOW VARIABLES` |
| Write | `EVENT`, `VECSET`, `VECDEL`, `METASET`, `SET`, `CACHE CLEAR`, `CACHE ENABLE`, `CACHE DISABLE` |
| Admin | `DUMP SAVE`, `DUMP LOAD`, `DUMP VERIFY`, `DUMP INFO`, `DUMP STATUS`, `CONFIG VERIFY` |

Authentication is per connection and is not carried across reconnects.

### `AUTH <password>`

```text
AUTH <password>
```

The password is everything after the first space or tab, taken as opaque bytes: it is not trimmed, split or upper-cased, so a password containing spaces authenticates as configured. Comparison is constant-time.

```text
> AUTH s3cret
+OK

> AUTH wrong
ERROR ERR invalid password
```

On a server with no password configured, `AUTH` succeeds and says so:

```text
> AUTH anything
+OK (no password required)
```

## Write commands

### `EVENT`

```text
EVENT <ctx> ADD <id> <score> [timestamp=<epoch_sec>]
EVENT <ctx> SET <id> <score> [timestamp=<epoch_sec>]
EVENT <ctx> DEL <id> [timestamp=<epoch_sec>]
```

Records that item `<id>` occurred in context `<ctx>`. `ADD` and `SET` take a score; `DEL` does not. The score is an integer in the inclusive range 0–100, and the subcommand keyword is case-insensitive. `timestamp=` takes unsigned epoch seconds and, when omitted, the server stamps the event with its own clock. All three forms answer `OK EVENT`, including a deduplicated repeat that changes nothing.

```text
> EVENT user_alice ADD item1 100
OK EVENT

> EVENT user_alice SET item2 90 timestamp=1730000000
OK EVENT

> EVENT user_alice DEL item2
OK EVENT
```

`ADD` accumulates, `SET` replaces the stored score for that item in that context, and `DEL` removes it. What each does to the co-occurrence graph is described in [events-and-co-occurrence.md](./events-and-co-occurrence.md).

Both identifiers are validated: neither may be empty, and neither may carry a byte at or below `0x20` or the `0x7F` delete byte, because such a byte in an ID lets one ingestion surface corrupt another's framing. Bytes at or above `0x80` are left alone, so multi-byte UTF-8 identifiers work. A space-delimited command cannot carry either violation, so these rejections are reached through the JSON surface rather than from here.

### `VECSET`

```text
VECSET <id> <f1> <f2> ... <fN>
```

Registers or replaces the vector for `<id>`. The dimension is taken from the number of float tokens and must match the dimension the store is already using, which is `vectors.default_dimension` until the first vector fixes it.

```text
> VECSET item1 0.1 0.2 0.3 0.4
OK VECSET

> VECSET item1 0.1 0.2 0.3
ERROR Vector dimension mismatch: expected 4, got 3
```

The TCP form carries no metadata; use `METASET` for that.

### `VECDEL`

```text
VECDEL <id>
```

Removes the vector and its metadata. An unknown ID is an error, not a silent success.

```text
> VECDEL item2
OK VECDEL

> VECDEL nope
ERROR Vector not found: nope
```

### `METASET`

```text
METASET <id> <key:value[,key:value...]>
```

Attaches metadata to an item that already has a vector. The pairs are one whitespace-free token; the same expression grammar `filter=` uses is accepted here, and each condition's field and value become one metadata entry. Values are typed by their spelling: `true`/`false` become booleans, a bare integer becomes an integer, a bare decimal becomes a double, anything else stays a string.

```text
> METASET item1 category:books,price:12,active:true
OK METASET

> METASET nope category:books
ERROR Vector not found for metadata: nope
```

Because metadata affects filtered results broadly, a successful `METASET` clears the whole query cache.

## Search commands

### `SIM`

```text
SIM <id> <top_k> [using=events|vectors|fusion] [adaptive=on|off] [filter=<expr>] [min_score=<float>]
```

Searches for items similar to `<id>`. `top_k` is a positive integer and must not exceed `similarity.max_top_k`. Options may appear in any order after `top_k`; an unrecognised token is rejected rather than ignored.

| Option | Values | Default |
|---|---|---|
| `using=` | `events`, `vectors`, `fusion` | `fusion` |
| `adaptive=` | `on`, `off` | server's `similarity.adaptive_fusion` |
| `filter=` | a filter expression (see below) | no filter |
| `min_score=` | any finite float | `0.0` |

`adaptive=` applies to fusion mode only. `min_score=` is applied after the search, so it trims the result set rather than widening it.

```text
> SIM item1 5 using=vectors
OK RESULTS 1
item2 0.9940

> SIM item1 5 using=fusion adaptive=on min_score=0.5
OK RESULTS 1
item2 0.9947
```

Two option tokens are recognised and explicitly refused, so a client using them fails loudly instead of getting an unfiltered answer:

```text
> SIM item1 5 candidate_limit=100
ERROR candidate_limit option is not supported

> SIM item1 5 explain=1
ERROR explain option is not supported
```

Any other unknown token is a syntax error:

```text
> SIM item1 5 bogus=1
ERROR Invalid SIM option: bogus=1
```

An ID with no vector cannot seed a vector or fusion search:

```text
> SIM nosuch 5
ERROR Query vector not found: nosuch
```

The three modes are described in [vector-search.md](./vector-search.md) and [fusion.md](./fusion.md).

### `SIMV`

```text
SIMV <top_k> [filter=<expr>] [min_score=<float>] <f1> <f2> ... <fN>
```

Searches by a query vector rather than by an existing ID. Every option token must precede the vector: the parser stops treating tokens as options at the first token containing no `=`, and reads the rest as floats. `SIMV` accepts no `using=` and no `adaptive=`; it always searches vectors.

```text
> SIMV 3 0.1 0.2 0.3 0.4
OK RESULTS 2
item1 1.0000
item2 0.9940

> SIMV 3 filter=active:true 0.1 0.2 0.3 0.4
OK RESULTS 1
item1 1.0000
```

The query vector's dimension must match the store's:

```text
> SIMV 3 1 2 3
ERROR Query vector dimension mismatch: expected 4, got 3
```

### The `filter=` grammar

A filter expression is a comma-separated list of conditions, all of which must match. Each condition is `<field><operator><value>`, with no whitespace anywhere in the token. The field must not be empty and the value must not be empty. Values are typed the same way `METASET` types them.

| Operator | Meaning | Example |
|---|---|---|
| `=` | equal | `filter=category=books` |
| `:` | equal, alias for `=` | `filter=category:books` |
| `!=` | not equal | `filter=category!=books` |
| `>` | greater than | `filter=price>10` |
| `<` | less than | `filter=price<10` |
| `>=` | greater than or equal | `filter=price>=20` |
| `<=` | less than or equal | `filter=price<=5` |
| `in(a\|b\|c)` | member of the list | `filter=category=in(books\|music)` |

`in(...)` is written as a value after `=` or `:`, with `|` separating members; an empty member makes the whole expression invalid. Conditions combine with `,` as a conjunction, and there is no disjunction between conditions and no grouping — `in(...)` is the way to express a set.

```text
> SIM item1 5 using=vectors filter=active:true,price>10
OK RESULTS 1
item2 0.9940

> SIM item1 5 using=vectors filter=bogus
ERROR Invalid filter condition: 'bogus'
```

An item with no metadata matches no condition, so a filter narrows results to items that have been through `METASET` or the HTTP `/metaset` and `/vecset` routes.

## Administrative commands

### `INFO`

```text
INFO
```

Reports server, traffic, memory, cache and data counters as an `END`-terminated block.

```text
> INFO
OK INFO

# Server
version: 0.2.0
uptime_seconds: 29

# Stats
total_commands_processed: 29
failed_commands: 8
total_connections_received: 3
active_connections: 1
event_commands: 4
sim_commands: 10
vecset_commands: 2
wal_replay_records_skipped: 0

# Memory
used_memory_bytes: 16
used_memory_human: 0.00 MB
memory_health: HEALTHY

# Cache
cache_entries: 0
cache_hits: 0
cache_misses: 9
cache_hit_rate: 0.0000

# Data
id_count: 2
ctx_count: 1
vector_count: 1
event_count: 4
END
```

`used_memory_bytes` counts only the vector matrix (`vector_count × dimension × 4`); the HTTP `/info` route reports per-store totals and process RSS as well. `wal_replay_records_skipped` is the running count of WAL records that recovery could not restore, described in [persistence.md](./persistence.md).

### `CONFIG`

```text
CONFIG HELP [path]
CONFIG SHOW [path]
CONFIG VERIFY <filepath>
```

`CONFIG HELP` with no path lists the configuration sections; with a path it prints the type, default, allowed values and description of that key.

```text
> CONFIG HELP similarity.index_type
+OK
similarity.index_type

Type: string (enum)
Default: "flat"
Allowed values:
  - "hnsw"
  - "ivf"
  - "flat"
Description: ANN index type: hnsw, ivf, or flat (brute-force)
END
```

`CONFIG SHOW` prints the running configuration as YAML, optionally narrowed to one section. `security.requirepass` is redacted to `***`, and a derived `security.auth_enabled` flag is shown alongside it.

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

`CONFIG VERIFY` loads and validates a configuration file without applying it. The path is resolved inside the directory the running configuration file came from — or, for a server started without one, inside the snapshot directory. Every path that resolves outside that root, does not exist, or cannot be opened is refused with one message, so the command answers no questions about the surrounding filesystem.

```text
> CONFIG VERIFY /etc/passwd
ERROR Configuration file is not accessible
```

### `DUMP`

```text
DUMP SAVE [filename]
DUMP LOAD <filename>
DUMP VERIFY <filename>
DUMP INFO <filename>
DUMP STATUS
```

Every filename is resolved inside `snapshot.dir`; a path that escapes it is refused with `ERROR Invalid filepath: path traversal detected`. Only `DUMP SAVE` may omit its filename — it then writes `snapshot.default_filename`, which goes through the same validation. `LOAD`, `VERIFY` and `INFO` each reject an empty filepath with their own message.

`DUMP LOAD`, `DUMP VERIFY` and `DUMP INFO` wrap whatever went wrong underneath into one message naming the resolved path, so a missing file and a corrupt file are told apart by the reason rather than by the message shape.

The reply to `DUMP SAVE` differs by `snapshot.mode` and the difference is a durability guarantee, not wording. In `fork` mode the file is not readable yet:

```text
> DUMP SAVE
OK DUMP_SAVE_STARTED /var/lib/nvecd/snapshots/nvecd.nvec
```

In `lock` mode the file is complete when the reply arrives:

```text
> DUMP SAVE
OK DUMP_SAVED /var/lib/nvecd/snapshots/nvecd.nvec
```

`DUMP STATUS` reports the background writer. `status: idle` is the only field when nothing has run; the other three states carry the path and timings, and a failure carries its message.

```text
> DUMP STATUS
OK DUMP_STATUS
status: completed
filepath: /var/lib/nvecd/snapshots/nvecd.nvec
start_time: 1788427643
end_time: 1788427643
END
```

The `in_progress` state adds `pid`, and the `failed` state adds `error`.

`DUMP LOAD` replaces the live stores with the snapshot's contents, rebuilds the ANN index, clears the cache and makes that snapshot the recovery base. `DUMP VERIFY` checks integrity without loading. `DUMP INFO` reads the header:

```text
> DUMP INFO nvecd.nvec
OK DUMP_INFO /var/lib/nvecd/snapshots/nvecd.nvec
version: 1
stores: 4
flags: 16
file_size: 476
timestamp: 1788427643
has_statistics: false
END
```

While a save or load is running, commands that touch the stores are refused with `ERROR READONLY Snapshot in progress` or `ERROR LOADING Snapshot load in progress`. [persistence.md](./persistence.md) covers the mechanics.

### `CACHE`

```text
CACHE STATS
CACHE CLEAR
CACHE ENABLE
CACHE DISABLE
```

`CACHE ENABLE` and `CACHE DISABLE` are shorthands for setting the `cache.enabled` runtime variable; both are write-privileged, as is `CACHE CLEAR`.

None of the four degrades quietly when its dependency is missing. `CACHE STATS` and `CACHE CLEAR` reach the cache controller and fail with `Cache controller is not initialized` when there is none; `CACHE ENABLE` and `CACHE DISABLE` never consult the controller at all — they go through the runtime variable manager and fail with `Runtime variable manager is not initialized` when that is absent.

```text
> CACHE CLEAR
OK CACHE_CLEARED

> CACHE DISABLE
OK CACHE_DISABLED

> CACHE ENABLE
OK CACHE_ENABLED
```

`CACHE STATS` is an `END`-terminated block:

```text
> CACHE STATS
OK CACHE_STATS
cache_enabled: true
cache_entries: 0
cache_memory_bytes: 0
current_memory_mb: 0.00
min_query_cost_ms: 10.00
ttl_seconds: 3600
compression_enabled: true
eviction_batch_size: 10
total_queries: 9
cache_hits: 0
cache_misses: 9
cache_misses_invalidated: 0
cache_misses_not_found: 9
cache_hit_rate: 0.0000
evictions: 0
ttl_expirations: 0
avg_hit_latency_ms: 0.000
avg_miss_latency_ms: 0.000
total_time_saved_ms: 0.00
END
```

[caching.md](./caching.md) explains what the counters mean.

### `DEBUG`

```text
DEBUG ON
DEBUG OFF
```

Debug mode is a property of the connection that issued it, not of the server, and it is dropped when the connection closes.

```text
> DEBUG ON
OK DEBUG_ON
```

While it is on, every `SIM` and `SIMV` response carries an extra block after the results:

```text
> SIM item1 3 using=vectors
OK RESULTS 0
# DEBUG 4
mode: vectors
query_time_us: 2
candidates: 0
results: 0
```

`# DEBUG 4` names the number of fields that follow, which lets a stateful client frame the block. `mode` is the search mode (`vector` for `SIMV`), `candidates` is the result count before `min_score=` was applied, and `results` is the count after. On a cache hit `query_time_us` is `0`, because the timing covers the search the lookup replaced.

### Runtime variables

```text
SET <variable> <value>
GET <variable>
SHOW VARIABLES [LIKE <pattern>]
```

`SET` returns `+OK`; `GET` returns a RESP bulk string. Only five variables are mutable — `logging.level`, `logging.json`, `cache.enabled`, `cache.min_query_cost_ms` and `cache.ttl_seconds` — and every other known variable is readable but rejects a write.

```text
> SET cache.ttl_seconds 600
+OK

> GET cache.enabled
$4
true

> SET vectors.default_dimension 8
ERROR Variable 'vectors.default_dimension' is immutable (requires restart)

> GET nosuch
ERROR Unknown variable: nosuch
```

Boolean values accept `true`/`false`, `on`/`off`, `1`/`0` and `yes`/`no`, and are stored canonically as `true` or `false`.

`SHOW VARIABLES` returns a RESP array of `<name>=<value> (mutable|immutable)` lines. `LIKE` takes a prefix; a trailing `%` is stripped and the rest is used as the prefix.

```text
> SHOW VARIABLES LIKE cache.%
*6
$42
cache.compression_enabled=true (immutable)
$28
cache.enabled=true (mutable)
$40
cache.eviction_batch_size=10 (immutable)
$34
cache.max_memory_mb=32 (immutable)
$43
cache.min_query_cost_ms=10.000000 (mutable)
$31
cache.ttl_seconds=600 (mutable)
```

The `performance.` prefix is also accepted as `perf.` on input, but introspection always reports the `performance.` spelling. The full variable list and its mutability is in [configuration.md](./configuration.md).

## Error responses

Every failure is a single `ERROR <message>` line. A rejected command still counts towards the `total_commands_processed` and `failed_commands` counters, so the failure ratio an operator alerts on cannot exceed one.

| Message | Cause |
|---|---|
| `Empty command` | blank line |
| `Command must be a single line` | embedded CR or LF |
| `Command must not contain embedded NUL bytes` | NUL byte in the request |
| `Unknown command: <NAME>` | unrecognised first token |
| `Request too large` | request exceeds `performance.max_query_length`, or the reactor's shared buffer budget is exhausted |
| `Request too large (no newline detected)` | limit reached before any newline arrived |
| `Server busy` | the worker pool could not accept the request |
| `NOAUTH Authentication required` | write or admin command on an unauthenticated connection |
| `ERR invalid password` | `AUTH` with the wrong password |
| `LOADING Snapshot load in progress` | `DUMP LOAD` is publishing |
| `READONLY Snapshot in progress` | lock-mode `DUMP SAVE` is running |
| `EVENT requires at least 3 arguments: <ctx> <type> <id> [<score>]` | `EVENT` arity |
| `EVENT ADD requires 4-5 arguments: <ctx> ADD <id> <score> [timestamp=<value>]` | `EVENT ADD` arity |
| `Invalid EVENT type: <T> (must be ADD, SET, or DEL)` | unknown `EVENT` subcommand |
| `Score must be in range [0, 100], got <n>` | event score out of range |
| `Invalid integer: <t>` | integer token with trailing characters, such as a decimal score or `top_k` |
| `Failed to parse integer: <t>` | integer token that is not a number at all |
| `Failed to parse timestamp: <v>` | non-numeric `timestamp=` |
| `Context cannot be empty` / `ID cannot be empty` | empty `EVENT` context or item ID |
| `Context must not contain whitespace or control characters` | `EVENT` context carrying a byte at or below `0x20`, or `0x7F` |
| `ID must not contain whitespace or control characters` | the same rule for an `EVENT` item ID |
| `VECSET requires at least 2 arguments: <id> <floats>` | `VECSET` arity |
| `Invalid float: <t>` | float token with trailing characters, or a non-finite spelling such as `nan` or `inf` |
| `Failed to parse float: <t>` | float token that is not a number at all |
| `ID cannot be empty` | `VECSET` with an empty item ID |
| `Vector cannot be empty` | `VECSET` with no components |
| `Vector dimension exceeds maximum of <n>` | `VECSET` above the store's hard ceiling |
| `Vector dimension mismatch: expected <n>, got <m>` | `VECSET` dimension |
| `Query vector cannot be empty` | `SIMV` with no components |
| `Query vector components must be finite` | non-finite component in a `SIMV` query vector |
| `Query vector norm must be finite and non-zero` | `SIMV` query vector whose norm cannot be used |
| `Query vector dimension mismatch: expected <n>, got <m>` | `SIMV` dimension |
| `VECDEL requires 1 argument: <id>` | `VECDEL` arity |
| `Vector not found: <id>` | `VECDEL` on an unknown ID |
| `Vector not found for metadata: <id>` | `METASET` on an item with no vector |
| `METASET requires 2 arguments: <id> <key:value[,key:value...]>` | `METASET` arity |
| `SIM requires at least 2 arguments: <id> <top_k>` | `SIM` arity |
| `SIMV requires at least 2 arguments: <top_k> <floats>` | `SIMV` arity |
| `SIMV requires at least one vector float` | `SIMV` whose tokens were all options |
| `top_k must be positive, got <n>` | non-positive `top_k` |
| `top_k <n> exceeds maximum allowed: <m>` | `top_k` above `similarity.max_top_k` |
| `Invalid using value: <v> (must be events, vectors, or fusion)` | unknown mode |
| `Invalid adaptive value: <v> (must be on or off)` | unknown `adaptive=` value |
| `Invalid SIM option: <t>` / `Invalid SIMV option: <t>` | unknown option token |
| `candidate_limit option is not supported` | `candidate_limit=` |
| `explain option is not supported` | `explain=` |
| `Invalid filter condition: '<pair>'` | malformed `filter=` condition |
| `Query vector not found: <id>` | `SIM` on an ID with no vector |
| `CONFIG requires subcommand: HELP\|SHOW\|VERIFY` | `CONFIG` arity |
| `Unknown CONFIG subcommand: <S>` | unknown `CONFIG` subcommand |
| `Configuration file is not accessible` | `CONFIG VERIFY` path outside the allowed root |
| `DUMP requires subcommand: SAVE\|LOAD\|VERIFY\|INFO\|STATUS` | `DUMP` arity |
| `DUMP LOAD requires a filepath` | `DUMP LOAD` with no argument |
| `DUMP VERIFY requires a filepath` | `DUMP VERIFY` with no argument |
| `DUMP INFO requires a filepath` | `DUMP INFO` with no argument |
| `Invalid filepath: path traversal detected` | snapshot path escapes `snapshot.dir` |
| `Failed to load snapshot from <path>: <reason>` | every `DUMP LOAD` failure, with an integrity detail appended in parentheses when there is one |
| `Snapshot verification failed for <path>: <reason>` | every `DUMP VERIFY` failure, with the same optional detail |
| `Failed to read snapshot info from <path>: <reason>` | `DUMP INFO` could not read the header |
| `Another snapshot load is already in progress` | a second concurrent `DUMP LOAD` |
| `Another snapshot save is already in progress` | a second concurrent lock-mode `DUMP SAVE` |
| `A snapshot save is already in progress` | `DUMP LOAD` while a save holds the store |
| `Cannot save snapshot while a snapshot load is in progress` | `DUMP SAVE` during a load |
| `CACHE requires subcommand: STATS\|CLEAR\|ENABLE\|DISABLE` | `CACHE` arity |
| `Cache controller is not initialized` | `CACHE STATS` or `CACHE CLEAR` on a server with no cache controller |
| `Runtime variable manager is not initialized` | `CACHE ENABLE` or `CACHE DISABLE` with no variable manager |
| `DEBUG requires exactly one argument: ON\|OFF` | `DEBUG` arity |
| `SET requires 2 arguments: <variable_name> <value>` | `SET` arity |
| `Unknown variable: <name>` | unknown runtime variable |
| `Variable '<name>' is immutable (requires restart)` | write to an immutable variable |

Error codes are partitioned by module: 1000–1999 configuration, 2000–2999 event processing, 3000–3999 command parsing, 4000–4999 vector and similarity, 5000–5999 storage and snapshot, 6000–6999 network and server, 7000–7999 client, 8000–8999 cache. The TCP surface reports the message only; the [HTTP surface](./http-api.md) maps the code to a status.

## nvecd-cli

`nvecd-cli` is a REPL and a one-shot command runner over the same protocol. It connects over TCP only; it has no Unix-socket option.

```text
Usage: nvecd-cli [OPTIONS] [COMMAND]
```

| Flag | Meaning |
|---|---|
| `-h HOST` | server hostname or IPv4 address (default `127.0.0.1`) |
| `-p PORT` | server port (default `11017`) |
| `--retry N` | retry the connection N times when it is refused (default `0`) |
| `--wait-ready` | keep retrying until the server accepts, up to 100 attempts |
| `--password-file FILE` | read the `AUTH` password from a private file |
| `--password-env NAME` | read the `AUTH` password from an environment variable |
| `--help` | print usage and exit |

The first argument that is not a recognised flag starts the command; everything from there on is joined with single spaces and sent as one command, and the process exits. With no command, the CLI enters interactive mode, where `help` prints the command summary and `quit` or `exit` leaves.

```bash
$ nvecd-cli -p 11017 INFO
INFO

# Server
version: 0.2.0
...

$ nvecd-cli -p 11017 BOGUS; echo "exit=$?"
(error) Unknown command: BOGUS
exit=1
```

An error response exits non-zero, which makes the one-shot form usable in scripts. Retries apply only to a refused connection: a name that does not resolve, or any other connection error, fails immediately.

`--password-file` and `--password-env` are mutually exclusive, and only one may be given. The password file must be a regular file owned by the current user with mode `0600` or stricter, must not exceed 4096 bytes, and must contain no CR, LF or NUL; one trailing newline is stripped. The password is sent with `AUTH` immediately after connecting, and a rejected password aborts before any command runs.

```bash
$ NVECD_PASSWORD=s3cret nvecd-cli --password-env NVECD_PASSWORD VECSET item1 0.1 0.2 0.3 0.4
VECSET
```

When built with readline available, interactive mode has history and context-aware tab completion for command names, subcommands and `using=` values.
