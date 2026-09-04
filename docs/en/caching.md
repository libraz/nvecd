# Query cache

nvecd caches similarity results in memory so a repeated query does not re-run the
search. This page covers what goes in the cache and under what key, the cost
threshold that decides whether a result is stored at all, compression, the
eviction and expiry rules, how a write invalidates cached results, the `CACHE`
command surface, and how to read the statistics.

The cache is enabled by default and holds 32 MB. Every key named here is
described alongside its range in [configuration.md](./configuration.md).

## What is cached

Complete `SIM` and `SIMV` result lists, as returned by the engine — the IDs and
their scores, before `min_score` is applied. Nothing else is cached: writes,
`INFO`, `CONFIG` and the snapshot commands never consult it.

Each entry is classified into one of three search types, which carry independent
policies:

| Search type | Query | Enabled | TTL |
|---|---|---|---|
| item search | `SIM` without `filter=` | yes | `cache.ttl_seconds` |
| vector search | `SIMV` without `filter=` | no | — |
| filtered search | either command with `filter=` | yes | 60 seconds |

Vector search is not cached. A `SIMV` query carries its query vector inline, so
an exact repeat of the same vector is rare enough that caching it would spend
memory without earning hits — but a `SIMV` that carries a `filter=` is a filtered
search and *is* cached. These policies are compiled in, not configuration keys.

## The cache key

The key is the MD5 digest of a string built from the query and the state it was
answered against. For `SIM`:

```text
SIM:<id>:<top_k>:<mode>:a<adaptive>:g<co_occurrence_gen>:v<vector_gen>:m<metadata_gen>:d<dataset_gen>[:f<filter>]
```

For `SIMV` the item ID is replaced by an MD5 digest of the raw bytes of the query
vector, and the co-occurrence generation and mode are absent:

```text
SIMV:<vector_digest>:<top_k>:v<vector_gen>:m<metadata_gen>:d<dataset_gen>[:f<filter>]
```

`<adaptive>` is `on`, `off`, or `default` when the query did not specify it —
which means a query that relies on the configured default and one that states the
same value explicitly are different keys.

Everything that changes the result is in the key, so predicting a miss is a
matter of checking whether any component differs:

- A different `top_k` is a different key. Asking for 10 results does not serve a
  cached request for 20, and asking for 5 does not reuse the first 5 of a cached
  20.
- A different `using=` mode is a different key.
- The filter expression enters the key **verbatim**, as the caller wrote it.
  `filter=a=1,b=2` and `filter=b=2,a=1` are different keys for the same filter.
- Four generation counters are in the key. `co_occurrence_gen` advances on any
  event that changes a co-occurrence score, and on each decay cycle;
  `vector_gen` on any `VECSET` or `VECDEL`; `metadata_gen` on any `METASET` and
  on any write that carries metadata; `dataset_gen` on a `DUMP LOAD` and on
  startup recovery.

`min_score` is deliberately absent. It is applied to the result after the cache
returns it, so one cached list serves every cutoff — a query at `min_score=0.5`
and the same query at `min_score=0.9` both hit the same entry.

## The cost threshold

A result is stored only if the search took at least `cache.min_query_cost_ms`
(default 10.0). The measurement covers the engine call itself, not parsing or
response formatting.

The threshold exists because a cheap query gains nothing from being cached and
still costs memory. A `flat` search over a few thousand vectors finishes in well
under a millisecond and will never be stored at the default; an `hnsw` search
over a large corpus with a selective filter can exceed it easily. Lowering the
threshold to 0 caches everything, which is useful when diagnosing but fills the
memory bound with entries that saved little.

Because the threshold is checked against the *uncached* cost, a query that never
crosses it is re-run every time and the cache reports a permanent miss for it.
That is the intended outcome, not a fault.

## Compression

Results are serialized to a fixed 260 bytes per row — a 256-byte ID buffer plus a
4-byte score — and, with `cache.compression_enabled` (default true), LZ4-compressed
before being stored.

The fixed-width layout is what makes compression pay: real item IDs are far
shorter than 256 bytes, so most of each record is zero padding that LZ4 removes
almost entirely. The saving grows with the number of rows per entry, so it is
largest exactly where entries are largest. Disabling compression stores the
serialized form as-is, trading that memory for the decompression on each hit.

An item ID of 256 bytes or longer, or containing a NUL byte, cannot be stored
without truncation, so the whole result set is refused rather than cached wrong.
Such a query is simply never cached.

## Eviction, memory and TTL

The cache is bounded by `cache.max_memory_mb` (default 32). The accounting covers
the compressed payloads, the keys, the container overhead and the invalidation
index, and is maintained incrementally rather than recomputed, so it stays
accurate as entries come and go.

When an insert would exceed the bound, entries are evicted from the
least-recently-used end until it fits, and at least `cache.eviction_batch_size`
(default 10) entries go in one pass. Evicting in batches keeps the cache from
running an eviction on every insert once it is full. A single result set larger
than the whole bound is refused outright.

`cache.ttl_seconds` (default 3600) expires entries by age. An entry stores the
absolute expiry computed from the TTL in force when it was inserted, and a
lookup also re-checks the age against the current global TTL, so lowering the TTL
at runtime takes effect on entries already resident. A TTL of 0 disables
expiration.

Expiry is lazy: an entry is removed when a lookup finds it expired, not by a
background sweep. An expired entry that is never looked up again holds its memory
until LRU pressure evicts it.

## Invalidation

Two mechanisms work together, and neither is a full clear.

The generation counters in the key make stale entries unreachable. Any `VECSET`
advances `vector_gen`, which appears in every `SIM` and `SIMV` key, so no query
issued afterwards can hash to an entry computed before it. This is what
guarantees correctness — including for a brand-new item, which no existing entry
references and which a per-item scheme would therefore miss.

The reverse index reclaims the memory. Each entry records the item IDs it
involves — the query ID plus the result IDs, up to 50 per entry — and a write to
any of them erases the entries referencing it immediately, rather than leaving
them to age out. `EVENT`, `VECSET` and `VECDEL` all invalidate this way, on the
mutated ID only.

Selective invalidation is what makes the cache usable under a live event stream.
A recommendation workload writes events continuously; if every event cleared the
cache, nothing would ever survive to be hit. Erasing only the entries that
mention the touched item leaves the rest of the working set intact.

Two writes are broader. `METASET`, and a `VECSET` that carries metadata, clear
the cache outright: a metadata change alters which items *any* filtered query
returns, and metadata is not what the reverse index tracks, so there is no
narrower set of entries to erase.

The insert side is guarded to match. A result is stored only if the cache is
still the same published instance it was looked up in, still enabled, and all
four generation counters still hold the values captured at lookup time. A write
that lands while a query is running therefore discards that query's result rather
than caching a list computed against state that has already moved on.

## The CACHE command

```bash
CACHE STATS
CACHE CLEAR
CACHE ENABLE
CACHE DISABLE
```

`CACHE CLEAR` empties the cache and answers `OK CACHE_CLEARED`. `CACHE ENABLE`
and `CACHE DISABLE` answer `OK CACHE_ENABLED` and `OK CACHE_DISABLED`; they set
the `cache.enabled` runtime variable, so `SET cache.enabled true` and the command
do the same thing.

Disabling unpublishes the cache from the query handlers — lookups and inserts
stop — but does not discard what it holds. Re-enabling exposes the same entries
again, subject to their TTL and to the generation counters, which will have moved
on if anything was written meanwhile. Clear explicitly if the memory is what
matters.

`cache.min_query_cost_ms` and `cache.ttl_seconds` are also settable at runtime
through `SET`.

The same operations are on the HTTP surface as `GET /cache/stats`,
`POST /cache/clear`, `POST /cache/enable` and `POST /cache/disable`. See
[http-api.md](./http-api.md).

## Reading the statistics

`CACHE STATS` answers with `OK CACHE_STATS`, one `field: value` line per
statistic, and `END`:

```text
OK CACHE_STATS
cache_enabled: true
cache_entries: 1284
cache_memory_bytes: 6291456
current_memory_mb: 6.00
min_query_cost_ms: 10.00
ttl_seconds: 3600
compression_enabled: true
eviction_batch_size: 10
total_queries: 48210
cache_hits: 31877
cache_misses: 16333
cache_misses_invalidated: 0
cache_misses_not_found: 16333
cache_hit_rate: 0.6612
evictions: 903
ttl_expirations: 1160
avg_hit_latency_ms: 0.041
avg_miss_latency_ms: 0.002
total_time_saved_ms: 402118.55
END
```

`GET /cache/stats` returns the same figures as JSON, under `enabled`,
`total_queries`, `cache_hits`, `cache_misses`, `cache_misses_invalidated`,
`cache_misses_not_found`, `hit_rate`, `current_entries`, `current_memory_bytes`,
`current_memory_mb`, `min_query_cost_ms`, `ttl_seconds`, `compression_enabled`,
`eviction_batch_size`, `evictions`, `avg_hit_latency_ms`, `avg_miss_latency_ms`
and `time_saved_ms`.

Whether the cache is earning its memory is one comparison: `total_time_saved_ms`
is the sum of the original cost of every query a hit replaced, and it is what the
cache bought. Weigh it against `cache_memory_bytes`. A cache holding 32 MB and
saving minutes of query time per hour is earning its keep; one saving
milliseconds is not.

`cache_misses_not_found` means the key was not present — a genuinely new query,
or one whose generation component had moved on. A high not-found count with a low
hit rate usually means the query mix has little repetition, or that writes are
advancing a generation counter faster than queries repeat; neither is fixed by
giving the cache more memory. `cache_misses_invalidated` counts lookups that
found an entry already flagged stale, but the write paths erase entries rather
than flagging them, so it stays at 0 and every miss lands in
`cache_misses_not_found`.

`evictions` rising steadily while `cache_hit_rate` stays low means the working
set does not fit in `cache.max_memory_mb`, and entries are being dropped before
they are hit a second time. That is the case where raising the bound helps.

`ttl_expirations` rising against a low hit rate means entries are aging out
before they are reused; raise `cache.ttl_seconds`, but only after confirming the
underlying data is stable enough for an older result to still be correct.

`avg_hit_latency_ms` covers lookup and decompression. Comparing it against
`avg_miss_latency_ms` is not the useful comparison — a miss is measured only up
to the point the lookup fails, not including the search that follows. The
comparison that matters is `avg_hit_latency_ms` against
`cache.min_query_cost_ms`, the cost a stored query had to exceed.
