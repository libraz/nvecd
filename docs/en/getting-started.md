# Getting started

One session from an empty server to a saved snapshot: a handful of events, the three search modes, a query vector, a metadata filter, and the server statistics. Every command below is run against the same running server, in order. The exhaustive syntax for each command is in [TCP protocol](./protocol.md); this page explains what the answers mean.

## Build

```bash
git clone https://github.com/libraz/nvecd.git
cd nvecd
make
```

The binaries land in `build/bin/`: `nvecd` (the server) and `nvecd-cli` (the client). System packages, build options and installing the binaries are covered in [Installation](./installation.md).

## Start the server

`examples/config.yaml` carries every option at its default. The one value worth changing before a first run is the snapshot directory, which defaults to `/var/lib/nvecd/snapshots` and is not writable by an ordinary user:

```bash
mkdir -p /tmp/nvecd-data
sed 's|/var/lib/nvecd/snapshots|/tmp/nvecd-data|' examples/config.yaml > /tmp/nvecd.yaml
./build/bin/nvecd -c /tmp/nvecd.yaml
```

The log ends with `nvecd server started on 127.0.0.1:11017`. Leave it running and use a second terminal for everything below. Every configuration key is described in [Configuration](./configuration.md).

## Record events

An event says that an item was acted on inside a context. The context is any grouping key — here, three sessions:

```bash
./build/bin/nvecd-cli -p 11017 EVENT alice ADD widget 100
./build/bin/nvecd-cli -p 11017 EVENT alice ADD gasket 80
./build/bin/nvecd-cli -p 11017 EVENT bob   ADD widget 100
./build/bin/nvecd-cli -p 11017 EVENT bob   ADD flange 95
./build/bin/nvecd-cli -p 11017 EVENT carol ADD gasket 70
./build/bin/nvecd-cli -p 11017 EVENT carol ADD flange 60
```

Each one answers `EVENT`. The score is an integer weight in the range 0 to 100; it is not a rating, only a relative strength for that interaction.

## Search on co-occurrence

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=events
```

```text
(2 results, showing 2)
1) flange (score: 9500)
2) gasket (score: 8000)
```

`widget` shared a context with `flange` once (`bob`, weights 100 and 95) and with `gasket` once (`alice`, weights 100 and 80). Each shared context contributes the product of the two event weights, and the contributions add up: 9500 and 8000. Co-occurrence scores are unbounded sums, not similarities in [0, 1] — comparing them across two different query items is meaningless. [Events and co-occurrence](./events-and-co-occurrence.md) covers the scoring rule, decay and pruning.

An item that has never appeared in an event has no entry in the index at all:

```bash
./build/bin/nvecd-cli -p 11017 SIM bolt 10 using=events
```

```text
(0 results)
```

`nvecd-cli` renders the response; it is not the response. The server speaks a line-based text protocol, and the same query on the wire looks like this:

```text
OK RESULTS 2\r\n
flange 9500.0000\r\n
gasket 8000.0000\r\n
```

Scores are always rendered with four decimal places on the wire. The CLI reprints them at the shell's default float precision, which is why the block above shows `9500` and the wire shows `9500.0000`.

## Add vectors

Vectors come from outside — nvecd stores and compares them but does not produce them. Four items, four dimensions each. `bolt` has no events at all, and `widget`, `gasket` and `flange` now carry both signals:

```bash
./build/bin/nvecd-cli -p 11017 VECSET widget 0.10 0.20 0.30 0.40
./build/bin/nvecd-cli -p 11017 VECSET gasket 0.15 0.18 0.32 0.41
./build/bin/nvecd-cli -p 11017 VECSET flange 0.90 0.10 0.05 0.02
./build/bin/nvecd-cli -p 11017 VECSET bolt   0.11 0.21 0.29 0.39
```

Each answers `VECSET`. The first vector stored fixes the dimension for the whole store, and every later vector must have the same length. Four dimensions keep this example readable; `vectors.default_dimension` in the shipped configuration is 768.

## Search on vectors

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=vectors
```

```text
(3 results, showing 3)
1) bolt (score: 0.9994)
2) gasket (score: 0.9954)
3) flange (score: 0.2677)
```

The metric is `vectors.distance_metric`, cosine by default, so the score is a cosine similarity and the query item itself is excluded. `bolt` ranks first even though nothing has ever been done with it: its embedding is close to `widget`'s, and that is all this mode looks at. [Vector search](./vector-search.md) covers the metrics and the ANN index types.

## Blend the two

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=fusion
```

```text
(3 results, showing 3)
1) bolt (score: 0.6)
2) gasket (score: 0.5967)
3) flange (score: 0.4)
```

Three things changed. The candidate set is the union of both searches, so `bolt` (vector only) and `flange` (strong on events, weak on vectors) both survive. Each source's scores are min–max normalized across its own candidate list before the blend, which puts the two incomparable scales — a cosine similarity and a co-occurrence sum — onto the same range. The normalized scores are then combined with the configured weights, `similarity.fusion_alpha` 0.6 for vectors and `similarity.fusion_beta` 0.4 for events.

The normalization is relative, and with only two co-occurrence candidates it has an edge to it: `gasket` is the weaker of the two on the event side, so min–max maps it to 0 there and its 0.5967 comes entirely from the vector side. The effect fades as the candidate list grows.

An item with no events at all shows the other half of the behaviour:

```bash
./build/bin/nvecd-cli -p 11017 SIM bolt 10 using=fusion
```

```text
(3 results, showing 3)
1) widget (score: 1)
2) gasket (score: 0.9942)
3) flange (score: 0)
```

`bolt` has no co-occurrence neighbours, so the event source contributes no candidates, its weight is dropped, and the vector weight is renormalized to one. The scores are not scaled down by the missing signal.

Adding `adaptive=on` makes the weight follow the query item's own data density instead of the fixed 0.6 / 0.4:

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=fusion adaptive=on
```

```text
(3 results, showing 3)
1) bolt (score: 0.872)
2) gasket (score: 0.8672)
3) flange (score: 0.128)
```

`widget` has two co-occurrence neighbours against an `adaptive_maturity_threshold` of 50, so it counts as a new item and the vector weight rises to 0.872. As its neighbour count grows towards the threshold, that weight falls towards `adaptive_min_alpha`. [Fusion](./fusion.md) covers the weights and the normalization in full.

## Search with a query vector

`SIMV` takes the vector instead of an item ID, so the query does not have to correspond to anything stored:

```bash
./build/bin/nvecd-cli -p 11017 SIMV 3 0.50 0.30 0.20 0.10
```

```text
(3 results, showing 3)
1) flange (score: 0.8685)
2) gasket (score: 0.6569)
3) bolt (score: 0.6367)
```

The query vector must have the store's dimension. Nothing is excluded from the candidate set — with no query item, there is no self-match to remove — so `widget` is a possible result here as well.

## Narrow the results

Metadata is attached per item and is only accepted for an item that already has a vector:

```bash
./build/bin/nvecd-cli -p 11017 METASET widget category:tools
./build/bin/nvecd-cli -p 11017 METASET gasket category:seals
./build/bin/nvecd-cli -p 11017 METASET flange category:tools
./build/bin/nvecd-cli -p 11017 METASET bolt   category:tools
```

`filter=` drops any candidate whose metadata does not match. It accepts `=`, `:`, `!=`, `>`, `<`, `>=`, `<=` and `in(a|b|c)`, and several conditions separated by commas are combined with AND:

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=vectors filter=category:tools
```

```text
(2 results, showing 2)
1) bolt (score: 0.9994)
2) flange (score: 0.2677)
```

`min_score=` cuts the tail instead, after scoring:

```bash
./build/bin/nvecd-cli -p 11017 SIM widget 10 using=vectors min_score=0.5
```

```text
(2 results, showing 2)
1) bolt (score: 0.9994)
2) gasket (score: 0.9954)
```

A threshold means different things in different modes: it is a cosine similarity in `using=vectors`, a normalized blend in `using=fusion`, and a raw co-occurrence sum in `using=events`.

## Look at the server state

```bash
./build/bin/nvecd-cli -p 11017 INFO
```

```text
INFO

# Server
version: 0.1.0
uptime_seconds: 15

# Stats
total_commands_processed: 24
failed_commands: 0
total_connections_received: 24
active_connections: 1
event_commands: 6
sim_commands: 9
vecset_commands: 4
wal_replay_records_skipped: 0

# Memory
used_memory_bytes: 64
used_memory_human: 0.00 MB
memory_health: HEALTHY

# Cache
cache_entries: 0
cache_hits: 0
cache_misses: 8
cache_hit_rate: 0.0000

# Data
id_count: 3
ctx_count: 3
vector_count: 4
event_count: 6
```

`id_count` is 3, not 4: it counts items known to the co-occurrence index, and `bolt` has only a vector. `used_memory_bytes` covers the vector matrix alone — four vectors of four floats.

The cache stored nothing. Every query above ran far below `cache.min_query_cost_ms`, which is 10 ms, and a query cheaper than that is not worth caching. The eight misses are the eight `SIM` lookups; an unfiltered `SIMV` is not cached at all under the default policy. [Caching](./caching.md) covers the policies and the invalidation rules.

## Keep the state

Everything so far lives in memory. `DUMP SAVE` writes a snapshot of events, co-occurrence, vectors and metadata into `snapshot.dir`. Startup recovery only considers files whose name ends in `.nvec` or `.dmp`, so give the snapshot such a name — the shipped `snapshot.default_filename` is `nvecd.snapshot`, which is written correctly but is never picked up at the next start:

```bash
./build/bin/nvecd-cli -p 11017 DUMP SAVE nvecd.dmp
```

```text
DUMP_SAVE_STARTED /tmp/nvecd-data/nvecd.dmp
```

The default snapshot mode is `fork`, so the response reports that a background save has started rather than that it has finished, and the path is the resolved absolute form of `snapshot.dir` (on macOS, `/tmp` resolves to `/private/tmp`). `DUMP STATUS` reports the outcome:

```bash
./build/bin/nvecd-cli -p 11017 DUMP STATUS
```

```text
DUMP_STATUS
status: completed
filepath: /tmp/nvecd-data/nvecd.dmp
start_time: 1788427835
end_time: 1788427836
```

Restarting the server with the same configuration loads it — the log reports `Recovery loaded snapshot: /tmp/nvecd-data/nvecd.dmp`, and `SIM widget 10 using=events` answers as it did before the restart. Automatic snapshots (`snapshot.interval_sec`), the write-ahead log and what a restart replays are covered in [Persistence](./persistence.md).

The same commands are available over HTTP when `api.http.enable` is on ([HTTP API](./http-api.md)), and from C and C++ through the client library ([Client library](./client-library.md)).
