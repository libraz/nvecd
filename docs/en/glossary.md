# Glossary

Terms as nvecd uses them. Where general machine-learning usage is broader, the definition here is the one the code implements.

**adaptive fusion** — A fusion search whose blend weight is derived from the query item's co-occurrence neighbour count instead of the configured constants. The vector weight runs from `adaptive_max_alpha` for an item with no neighbours down to `adaptive_min_alpha` at `adaptive_maturity_threshold` neighbours. Enabled per query with `adaptive=on` or globally with `similarity.adaptive_fusion`. See [Fusion](./fusion.md).

**ANN index** — The approximate-nearest-neighbour structure a vector search runs against, owned by the similarity engine and selected by `similarity.index_type`: `flat`, `hnsw` or `ivf`. See [Vector search](./vector-search.md).

**blend weight** — The pair of weights a fusion search applies to the two normalized score lists: `similarity.fusion_alpha` for the vector side and `similarity.fusion_beta` for the event side. When only one source produced candidates, its weight is set to one. See [Fusion](./fusion.md).

**checkpoint sidecar** — A file named `<snapshot>.walseq` written next to a snapshot, holding the write-ahead log sequence number the snapshot covers. Recovery with the WAL enabled replays only records after that sequence. See [Persistence](./persistence.md).

**co-occurrence** — The association between two items that were acted on in the same context. Each shared context contributes the product of the two event scores, and the contributions accumulate into one symmetric score per pair. See [Events and co-occurrence](./events-and-co-occurrence.md).

**co-occurrence search** — `SIM <item> <k> using=events`. Returns the query item's highest-scoring co-occurrence neighbours, with their raw accumulated scores. See [Events and co-occurrence](./events-and-co-occurrence.md).

**context** — The grouping key of an `EVENT` command: a session, a user, a basket, or anything else that makes two interactions belong together. Items in the same context co-occur. See [Events and co-occurrence](./events-and-co-occurrence.md).

**cosine** — The default distance metric. Scores are cosine similarities computed from vector norms recorded at write time; higher is closer. See [Vector search](./vector-search.md).

**decay** — The periodic reduction of every co-occurrence score by the factor `events.decay_alpha`, applied every `events.decay_interval_sec` seconds by a background scheduler, with periodic pruning. It is what keeps old behaviour from dominating and what bounds the growth of the scores. See [Events and co-occurrence](./events-and-co-occurrence.md).

**dedup window** — `events.dedup_window_sec`. An `ADD` event repeating the same context, item and score inside the window is acknowledged but not applied, so a client retry does not inflate a co-occurrence score. `SET` and `DEL` are deduplicated against the recorded state of the pair instead of against a time window. See [Events and co-occurrence](./events-and-co-occurrence.md).

**dimension** — The component count of every vector in the store. It is fixed by the first vector stored and every later vector must match it; `vectors.default_dimension` sizes the ANN index before that first write. See [Vector search](./vector-search.md).

**distance metric** — `vectors.distance_metric`, one of `cosine`, `dot` and `l2`. It applies to every vector comparison the server makes. See [Vector search](./vector-search.md).

**dot** — The dot-product metric. The score is the raw dot product, so it is unbounded and sensitive to vector magnitude. See [Vector search](./vector-search.md).

**event** — One reported interaction: `EVENT <context> <ADD|SET|DEL> <item> [<score>]`. All three forms append to the context's ring buffer and differ in how they are deduplicated: `ADD` against the dedup window, `SET` against the last score recorded for that context and item, `DEL` against whether the pair is already marked deleted. A `DEL` is stored with score 0, so it forms no new co-occurrence contribution. See [Events and co-occurrence](./events-and-co-occurrence.md).

**flat index** — The default `similarity.index_type`. No index structure exists; a query scans the stored vectors directly, which is exact unless sampling engages. See [Vector search](./vector-search.md).

**fusion search** — `SIM <item> <k> using=fusion`, the default mode. Both searches run, their candidate sets are unioned, each source's scores are normalized separately, and the two are combined with the blend weight. See [Fusion](./fusion.md).

**HNSW** — A graph-based ANN index, selected with `index_type: hnsw` and tuned by `hnsw_m`, `hnsw_ef_construction` and `hnsw_ef_search`. See [Vector search](./vector-search.md).

**item ID** — The string a client uses to name an item. Events, vectors and metadata share one ID space, which is what lets the two signals be blended for the same item. See [TCP protocol](./protocol.md).

**IVF** — An inverted-file ANN index that partitions vectors into `ivf_nlist` clusters and searches `ivf_nprobe` of them. It trains once `ivf_train_threshold` vectors exist, and buffers later writes until `ivf_seal_threshold` publishes them into the lists. See [Vector search](./vector-search.md).

**L2** — The Euclidean-distance metric. The reported score is `1 / (1 + distance)`, so higher is closer, as with the other metrics. See [Vector search](./vector-search.md).

**metadata filter** — The `filter=` option on `SIM` and `SIMV`. Conditions are `key<op>value` with `=`, `:`, `!=`, `>`, `<`, `>=`, `<=` or `in(a|b|c)`; several conditions separated by commas must all match. Values are typed as bool, integer, float or string by their form. See [TCP protocol](./protocol.md).

**min score** — The `min_score=` option. Results scoring below it are dropped after scoring. The scale it refers to differs per mode: a similarity in `using=vectors`, a normalized blend in `using=fusion`, a raw co-occurrence sum in `using=events`. See [TCP protocol](./protocol.md).

**negative signal** — With `events.negative_signals` enabled, a `DEL` event subtracts from the removed item's co-occurrence scores against the items still in the context, by each of their scores times `events.negative_weight`. A pair driven to zero or below stops appearing in results. Disabled by default, in which case a `DEL` only ends the item's contribution to new pairs. See [Events and co-occurrence](./events-and-co-occurrence.md).

**normalization (scores)** — The min–max rescaling a fusion search applies to each source's candidate list before blending, which puts an unbounded co-occurrence sum and a bounded similarity on the same range. It is relative to the candidates at hand: the weakest candidate of a source contributes zero from that source. A single candidate, or a list whose scores are all within 1e-4 of each other, is clamped into [0, 1] instead. See [Fusion](./fusion.md).

**normalization (vectors)** — Scaling a vector to unit length. The server stores vectors exactly as sent and does not normalize them; the cosine metric divides by norms recorded at write time instead. A client that wants unit vectors sends unit vectors. See [Vector search](./vector-search.md).

**query cache** — The in-memory cache of `SIM` and `SIMV` results. An entry is stored only if the query took at least `cache.min_query_cost_ms`, keys carry generation counters so any write to the underlying store invalidates the affected space, and an unfiltered `SIMV` is not cached under the default policy. See [Caching](./caching.md).

**recovery** — What a server does at startup: load the newest valid snapshot, and, with the WAL enabled, replay the log records after the snapshot's checkpoint before accepting writes. See [Persistence](./persistence.md).

**ring buffer** — The fixed-size per-context event buffer, sized by `events.ctx_buffer_size`. A new event overwrites the oldest one, and only the events still in the buffer take part in the co-occurrence pairs a later event forms. See [Events and co-occurrence](./events-and-co-occurrence.md).

**sampling** — Approximate scanning controlled by `similarity.sample_size`: once the corpus exceeds twice that value, a vector search scores a random sample rather than every vector. Setting it to 0 makes every search exact. See [Vector search](./vector-search.md).

**score** — Two distinct things. On an `EVENT`, an integer weight in the range 0 to 100 that a client assigns to an interaction. In a `SIM` or `SIMV` result, the ranking value the search produced, rendered on the wire with four decimal places. See [TCP protocol](./protocol.md).

**snapshot** — A single file holding the configuration, the events, the co-occurrence index, the vectors and the metadata at one point in time. Written by `DUMP SAVE`, by the scheduler when `snapshot.interval_sec` is set, and read back by `DUMP LOAD` or by startup recovery. See [Persistence](./persistence.md).

**support (minimum support)** — `events.min_support`. Co-occurrence edges whose absolute score is below it are removed when the index is pruned, which bounds memory in a long-running index whose scores decay. Zero disables it. See [Events and co-occurrence](./events-and-co-occurrence.md).

**temporal co-occurrence** — With `events.temporal_cooccurrence` enabled, each pair's contribution is multiplied by a decay factor derived from the age of the two events against `events.temporal_half_life_sec`, so events far apart in time associate their items less strongly. The decay is computed per pair, so a streaming update produces the same index as a batch rebuild. See [Events and co-occurrence](./events-and-co-occurrence.md).

**top-k** — The number of results a search is asked for, the second argument of `SIM` and the first of `SIMV`. It must be positive and no larger than `similarity.max_top_k`, which is checked when the command is parsed. See [TCP protocol](./protocol.md).

**vector search** — `SIM <item> <k> using=vectors`, or `SIMV <k> <floats>` for a query vector that need not belong to a stored item. Ranks by the configured distance metric and ignores events entirely. See [Vector search](./vector-search.md).

**WAL** — The write-ahead log. Every accepted write is appended to it before it is applied, so a restart can replay whatever a snapshot does not already contain. Disabled by default (`wal.enabled`). See [Persistence](./persistence.md).
