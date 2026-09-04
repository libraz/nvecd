# Performance

This page is a tuning guide: which settings change what, what each one costs, and how to tell from the server's own output whether a change helped. Measured figures live in [Benchmarks](./benchmarks.md) and are not repeated here.

## Choosing the search mode

`SIM` and `SIMV` take `using=` to select which signal answers the query. The three modes have different costs because they read different structures.

| Mode | Reads | Cost |
|---|---|---|
| `using=vectors` | The vector store only | A distance scan, or an ANN index lookup |
| `using=events` | The co-occurrence index only | A lookup on the queried item's neighbour set |
| `using=fusion` | Both, blended | The sum of the two, plus the blend |

Co-occurrence search is bounded by how many neighbours the queried item has accumulated, not by corpus size, so it stays cheap as the corpus grows. Vector search scales with the number of vectors scanned. Fusion search costs what both cost.

Pick the cheapest mode that answers the question. Content similarity needs only vectors; "items that appear alongside this one" needs only events. Reach for fusion when a result has to reflect both, and when the extra latency is worth it — see [Fusion](./fusion.md) for what the blend does.

`SIMV` takes a query vector rather than an item id, and accepts no `using=` option at all: with no item to look the neighbours of up, it is always vector search.

## Sizing the event structures

Three settings bound how much the event side keeps, and each is a memory-against-quality decision.

```yaml
events:
  ctx_buffer_size: 50
  max_contexts: 0
  max_neighbors_per_item: 0
  min_support: 0.0
```

`ctx_buffer_size` is the ring buffer length per context: how many recent events a single context remembers. Every event that arrives co-occurs with the events still in the buffer, so the buffer length sets how far back an association can reach, and the work an `EVENT` costs grows with it. A short buffer models a session; a long one models a history.

`max_contexts` caps how many contexts stay resident, pruning least-recently-active first. Left at 0 it is unlimited, and a workload that mints a fresh context per visitor grows without bound. Set it whenever contexts are not drawn from a closed set.

`max_neighbors_per_item` caps the neighbour set an item accumulates. The co-occurrence index is a map of maps, so this is the setting that decides its width; unbounded, a popular item accumulates an edge to everything it has ever appeared with, and both memory and the cost of a co-occurrence query grow with that count.

`min_support` drops edges whose score falls below a threshold, which removes the long tail of associations seen once. It prunes by score rather than by count, so it interacts with decay: with `decay_alpha` below 1, an edge that stops being reinforced eventually falls under the threshold and is dropped.

Set at least one of `max_neighbors_per_item` and `min_support` on a workload with heavy-tailed popularity. Leaving both unbounded is the usual reason event memory grows without a ceiling.

## Choosing an index type

`similarity.index_type` selects the structure that answers a vector query.

```yaml
similarity:
  index_type: "flat"
  sample_size: 10000
```

`flat` scans every vector and returns the exact top-k. It is the default, it needs no build step and no training, and it is the right answer for a corpus small enough that a scan fits the latency budget. Its cost is linear in both corpus size and dimension.

`hnsw` builds a navigable graph. Recall is tuned by `hnsw_ef_search`, the search width at query time: higher means more of the graph explored, higher recall and more latency. `hnsw_m` and `hnsw_ef_construction` govern the graph itself and take effect at build time, so changing them means rebuilding. `hnsw_max_elements` reserves capacity at startup; left at 0 the graph grows on demand.

`ivf` partitions the corpus into `ivf_nlist` cells and probes `ivf_nprobe` of them per query. Recall is tuned by `ivf_nprobe`. The index has to be trained before it can answer anything: training starts asynchronously once `ivf_train_threshold` vectors have arrived, and vectors written while it runs are buffered and published into the inverted lists by a background pass. Until both finish, queries fall back to scanning, so an IVF server is not at its steady-state latency immediately after a cold start or a bulk load.

Choose the knob value from the recall you need rather than the latency you want. The [recall floors in Benchmarks](./benchmarks.md#approximate-index-recall) give the shape of both curves on clustered data: recall rises steeply at first and then flattens, so the last few points of recall cost disproportionately more latency than the first. Verify the setting on your own vectors — recall depends on how clustered the corpus is, and a corpus of near-orthogonal vectors is one where no approximate index pays for itself.

`sample_size` is an independent approximation that applies to the scanning path. Above twice its value, a query scores a random sample instead of the whole corpus. It bounds the distance work, but the reservoir pass that draws the sample still walks every vector, so it bounds scan cost rather than making query cost constant. Set it to 0 for exact answers.

## Tuning the query cache

```yaml
cache:
  enabled: true
  max_memory_mb: 32
  min_query_cost_ms: 10.0
  ttl_seconds: 3600
  compression_enabled: true
  eviction_batch_size: 10
```

`min_query_cost_ms` is the setting that decides whether the cache does anything at all. A result is only stored if producing it took at least this long, so on a corpus where queries finish in well under the threshold nothing is ever cached, and the cache costs memory and lookup time while returning nothing. Compare the threshold against the query latency you actually observe before raising `max_memory_mb`.

`max_memory_mb` bounds resident entries; when it is reached, `eviction_batch_size` entries are evicted at once rather than one at a time, which amortises the work under the exclusive lock. `ttl_seconds` expires entries by age; 0 disables expiry, leaving invalidation and eviction as the only ways an entry leaves. `compression_enabled` runs results through LZ4, trading a little lookup time for more entries in the same budget.

### Reading the cache statistics

```bash
nvecd-cli CACHE STATS
```

The response opens with `OK CACHE_STATS` and lists the counters one per line, ending with `END`:

```text
OK CACHE_STATS
cache_enabled: true
cache_entries: 2450
cache_memory_bytes: 13056921
current_memory_mb: 12.45
min_query_cost_ms: 10.00
ttl_seconds: 3600
compression_enabled: true
eviction_batch_size: 10
total_queries: 10000
cache_hits: 8500
cache_misses: 1500
cache_misses_invalidated: 320
cache_misses_not_found: 1180
cache_hit_rate: 0.8500
evictions: 320
ttl_expirations: 12
avg_hit_latency_ms: 0.000
avg_miss_latency_ms: 0.920
total_time_saved_ms: 7820.00
END
```

Four readings decide whether the cache earns its memory.

`total_time_saved_ms` against `current_memory_mb` is the direct answer: it is the latency the cache has actually removed, in exchange for that much resident memory. A cache saving little while holding its full budget is not paying for itself.

`cache_hit_rate` alone can mislead. A low rate with `total_queries` barely above `cache_hits + cache_misses` means the workload has little repetition and nothing will fix that; a low rate with `evictions` climbing means the budget is too small for the working set.

`cache_misses_invalidated` against `cache_misses_not_found` separates the two kinds of miss. Not-found misses are queries never seen before. Invalidated misses are queries whose answer was cached and then thrown away because the underlying item changed — a high proportion of these means writes are outrunning reads on the same items, and more cache memory will not help.

`avg_hit_latency_ms` against `avg_miss_latency_ms` is the ratio the cache is buying. When they are close, queries are already fast enough that `min_query_cost_ms` is the setting to look at rather than the budget.

`CACHE CLEAR` empties the cache; `CACHE ENABLE` and `CACHE DISABLE` turn it on and off without a restart, which makes an A/B measurement of a live workload possible.

## Vector dimension

`vectors.default_dimension` is fixed for the store. It multiplies three things at once: the memory each vector occupies, the work a distance computation does, and the size of an ANN index built over the vectors.

The scanning path is linear in dimension, so halving the dimension roughly halves scan cost. Only dimension 128 has measured figures, in [Benchmarks](./benchmarks.md#search-latency); other dimensions follow that scaling but have not been measured. Where the embedding model allows a choice, a smaller dimension is cheaper in every dimension of cost at once, and the quality difference is a property of the model rather than of nvecd.

`vectors.distance_metric` selects `cosine`, `dot` or `l2`. Cosine normalises, which lets the store keep a precomputed norm per vector and reduce the per-candidate work to a dot product.

## Confirming SIMD is active

The distance kernels have AVX2, NEON and scalar implementations, selected at run time from the CPU's reported features. The server logs the selection once at startup:

```text
[info] Vector SIMD: NEON
```

The value is `AVX2` on x86_64 with AVX2, `NEON` on 64-bit ARM, and `Scalar` where neither is available. `Scalar` on a machine that should have one of the others points at the build rather than the CPU: an AVX2 build is only produced when the compiler accepted `-mavx2` at configure time, which CMake reports as a status line during configuration.

## Memory sizing

Vector memory is arithmetic rather than a measurement. The store keeps one contiguous matrix, one norm per vector, one bit each for the normalised and deleted flags, and the item id string with its index entry:

```text
bytes ≈ vectors × dimension × 4      (the matrix, float32)
      + vectors × 4                  (norms)
      + vectors × 2 / 8              (normalised and deleted bits)
      + vectors × (id length + index overhead)
```

The matrix dominates as soon as the dimension is more than a few dozen: at dimension 768, a million vectors is about 3 GB of matrix, and everything else is rounding. Deleting a vector does not shrink the matrix — it sets a tombstone, and the space returns when the store defragments.

Event memory has no comparable formula, because the co-occurrence index is a map of maps whose width depends on the data. It grows with the number of items that have neighbours, times the neighbour count each has accumulated, and both string keys are stored. `max_neighbors_per_item` is what bounds it.

Cache memory is bounded by `max_memory_mb` and reported exactly by `CACHE STATS`.

Rather than estimating the total, read it. `INFO` reports `used_memory_bytes`, but that figure counts only the vector matrix — vectors times dimension times four — so it understates the process. The HTTP metrics endpoint reports `nvecd_memory_bytes`, which sums the event store, the vector store and the co-occurrence index, and excludes the cache. Neither includes allocator overhead, so size the machine against what the operating system reports for the process, using these to attribute it.

## Monitoring

### INFO

```bash
nvecd-cli INFO
```

The response is grouped into sections and ends with `END`. `total_commands_processed` and `failed_commands` give the error rate; `active_connections` against `performance.max_connections` gives connection headroom; `event_commands`, `sim_commands` and `vecset_commands` give the read-write mix; `memory_health` is the server's own assessment of memory pressure; `cache_hit_rate` repeats the cache's headline number; and `id_count`, `ctx_count`, `vector_count` and `event_count` give the data volume the rest of the numbers apply to.

### The metrics endpoint

With `api.http.enable` set, the server exposes Prometheus text format:

```bash
curl http://localhost:8080/metrics
```

It publishes `nvecd_uptime_seconds`, `nvecd_commands_total` broken down by command and in total, `nvecd_memory_bytes`, `nvecd_vectors_total`, `nvecd_events_total`, `nvecd_contexts_total`, and the cache series `nvecd_cache_queries_total`, `nvecd_cache_hits_total`, `nvecd_cache_misses_total`, `nvecd_cache_hit_rate` and `nvecd_cache_entries`.

Rate of change is what to alert on. A `nvecd_cache_hit_rate` that falls while `nvecd_commands_total` for `vecset` rises means writes are invalidating faster than reads can repopulate; `nvecd_memory_bytes` rising while `nvecd_vectors_total` is flat means the growth is on the event side.

### Health endpoints

Four endpoints, all uncounted, so probing them does not move the command counters:

| Endpoint | Purpose |
|---|---|
| `GET /health` | A bare liveness answer with a timestamp |
| `GET /health/live` | Liveness probe; 200 while the process is running |
| `GET /health/ready` | Readiness probe; 200 when ready, 503 while a snapshot is loading |
| `GET /health/detail` | Per-component status, uptime, and counts for the event store, vector store and co-occurrence index |

`GET /health/ready` is the one to put behind a load balancer: it reports `not_ready` with a 503 while the server is loading a snapshot, which is exactly the window in which it is listening but cannot answer usefully.

```bash
curl -s http://localhost:8080/health/detail
```

The response carries `status`, `uptime_seconds` and a `components` object, whose `vector_store` names the vector count and dimension, `event_store` the context and event counts, and `co_index` the number of items with neighbours.

## Diagnosing a slow query

Work through the settings in the order they affect cost.

1. Check the startup log for the SIMD selection. `Scalar` where AVX2 or NEON was expected costs more than any configuration change will recover.
2. Turn on debug output with `DEBUG ON` and re-run the query in the same session — the setting is per-connection, so a separate one-shot invocation does not inherit it. Each `SIM` and `SIMV` response then carries an appended block giving the mode that answered, the query time in microseconds, the number of candidates considered and the number of results returned. Candidates far above results means the query is scoring far more than it needs to.
3. Read `CACHE STATS`. A hit rate near zero with `total_queries` climbing usually means `min_query_cost_ms` is above the query's actual cost, so nothing is being stored.
4. Check whether the mode is doing more work than the question needs — a fusion query where `using=vectors` would answer pays for both signals.
5. Check the corpus size against the index type. A `flat` index scans everything; if the corpus has outgrown the latency budget, that is a structural answer, not a tuning one.

For memory rather than latency, read `nvecd_memory_bytes` first to see which side is growing, then bound that side: `max_memory_mb` for the cache, `max_neighbors_per_item` and `max_contexts` for events, and dimension or corpus size for vectors.

## Running on more than one machine

nvecd has no replication, no clustering, no cross-node snapshot sync and no distributed query. A second server is a second independent server, and everything that makes two of them agree is something an operator writes.

The one pattern that works is to derive a read replica from a snapshot:

```bash
nvecd-cli DUMP SAVE replica-sync.dmp
scp /var/lib/nvecd/snapshots/replica-sync.dmp replica:/var/lib/nvecd/snapshots/
ssh replica nvecd-cli DUMP LOAD replica-sync.dmp
```

What this gives, stated exactly: the replica holds the state as of the moment the snapshot was taken. Every write accepted by the primary since then is missing, and stays missing until the next copy. The staleness window is the interval between copies, and it is on the operator to decide whether the workload tolerates it.

Consequences worth being explicit about. Writes must go to one server; there is no conflict resolution, because there is no protocol between servers in which a conflict could be detected. A `DUMP LOAD` replaces the receiving server's state, so a replica cannot also accept its own writes. `GET /health/ready` returns 503 during the load, so a replica taken out of a load balancer's rotation returns by itself once it is ready. Read-only traffic can be spread across replicas freely — each one is a complete, self-contained copy — as long as the application tolerates different replicas answering from slightly different states.

To measure what a snapshot costs on your data, since no benchmark in the repository produces it: time the `DUMP SAVE` round trip and read the resulting file's size.

```bash
time nvecd-cli DUMP SAVE
ls -l /var/lib/nvecd/snapshots/
```

Run it against a populated server at the scale you actually operate at. In `fork` mode the save happens in a forked child, so the round trip is close to the fork cost rather than the write cost, and the file appears after the command has already returned; `DUMP STATUS` reports whether one is still in progress. See [Persistence](./persistence.md) for what the modes do.
