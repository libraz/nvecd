# Semantic search

Retrieval over a document corpus where the query is a piece of text the user typed, not an item already in the store. This guide covers `SIMV`, the ANN index choice and what it costs in recall, `min_score` as a relevance cutoff, and the metadata filter that scopes a search.

Events play no part here. Nothing on this page needs `EVENT`, and the co-occurrence index stays empty.

## Why `SIMV` and not `SIM`

`SIM` starts from an item that is already stored: it looks the item up, takes its vector or its co-occurrence neighbours, and searches from there. A search box has no such item. The user typed a sentence, an encoder turned it into a vector, and that vector exists only for the duration of the request.

`SIMV` takes the query vector directly:

```text
SIMV <top_k> [filter=<expr>] [min_score=<float>] <f1> <f2> ... <fN>
```

The options must come before the vector components. Option parsing stops at the first token without an `=`, and everything from there on is read as a float:

```bash
nvecd-cli -p 11017 SIMV 3 0.90 0.25 0.12 0.05 filter=lang=en
```

```text
(error) Failed to parse float: filter=lang=en
```

The query vector is not an item, so nothing is excluded from the result on identity grounds. A stored document whose vector equals the query is returned like any other candidate, at the top of the ranking.

## Indexing the corpus

nvecd does not produce embeddings. Encode each document with the model the application already uses and store the result under a stable identifier:

```bash
nvecd-cli -p 11017 VECSET doc_sorting  0.91 0.22 0.10 0.04
nvecd-cli -p 11017 VECSET doc_indexing 0.86 0.31 0.15 0.07
nvecd-cli -p 11017 VECSET doc_billing  0.09 0.14 0.90 0.35
```

The store fixes its dimension on the first vector it accepts, and every later `VECSET` must match. Query vectors must match it too; a mismatch is rejected rather than padded:

```text
(error) Query vector dimension mismatch: expected 4, got 3
```

`vectors.default_dimension` pre-sizes the ANN index rather than enforcing a width. `vectors.distance_metric` selects `cosine` (the default), `dot` or `l2`, and must match what the encoder was trained for — a model trained for cosine similarity ranked by raw dot product gives systematically wrong answers on documents of differing length.

Attach whatever the search needs to scope on:

```bash
nvecd-cli -p 11017 METASET doc_sorting  lang:en,year:2024
nvecd-cli -p 11017 METASET doc_indexing lang:en,year:2021
nvecd-cli -p 11017 METASET doc_billing  lang:ja,year:2024
```

`METASET` requires the vector to exist first, so index the document before annotating it.

## Querying

```bash
nvecd-cli -p 11017 SIMV 3 0.90 0.25 0.12 0.05
```

```text
(3 results, showing 3)
1) doc_sorting (score: 0.9992)
2) doc_indexing (score: 0.9964)
3) doc_billing (score: 0.2613)
```

The wire form is the same as `SIM`: a count line, then one `<id> <score>` line per result, with scores at four decimal places.

```text
OK RESULTS 3
doc_sorting 0.9992
doc_indexing 0.9964
doc_billing 0.2613
```

## `min_score` as a relevance cutoff

`SIMV` scores are raw metric values. Nothing normalises them, so under `cosine` they land in `[-1, 1]` and mean the same thing in every query the corpus ever serves. That is what makes an absolute threshold usable here, where the same threshold on a fusion query would not be — fusion rescales each result set to its own range.

`top_k` alone always returns `top_k` documents, however irrelevant. A search box wants an empty result for a query that matches nothing, which is what `min_score` gives:

```bash
nvecd-cli -p 11017 SIMV 3 min_score=0.8 0.90 0.25 0.12 0.05
```

```text
(2 results, showing 2)
1) doc_sorting (score: 0.9992)
2) doc_indexing (score: 0.9964)
```

The comparison is inclusive, and the cutoff is applied after retrieval: it removes results from the `top_k` that were found, it does not cause more of them to be fetched. Choose the value by sampling real queries against the real corpus — the right threshold is a property of the encoder, not of nvecd.

## Scoping with a filter

```bash
nvecd-cli -p 11017 SIMV 3 filter=lang=en,year>=2022 0.90 0.25 0.12 0.05
```

```text
(1 results, showing 1)
1) doc_sorting (score: 0.9992)
```

Conditions are comma-separated and combined with AND, over the operators `=`, `:`, `!=`, `>`, `<`, `>=`, `<=` and `in(a|b|c)`. Values are typed on the way in, so `year>=2022` compares numbers and `lang=en` compares strings. A document with no metadata never matches a non-empty filter.

Filtering happens after the metric ranks the candidates, so a highly selective filter costs more per query than an unfiltered one: with an ANN index the search widens its fetch and retries until it has `top_k` surviving results or the index is exhausted.

## Index choice and recall

`similarity.index_type` selects how a query finds its candidates.

**`flat`** (the default) scans the corpus and is exact — with one caveat that matters at scale. `similarity.sample_size` defaults to `10000`, and once the corpus holds more than twice that many vectors, the scan covers a random sample of that size instead of everything. That is a recall loss inside the supposedly exact path. Set `similarity.sample_size: 0` to keep `flat` genuinely exhaustive, and accept that the scan then grows linearly with the corpus.

**`hnsw`** builds a navigable graph and answers from it. `similarity.hnsw_ef_search` is the query-time knob: a larger search width explores more of the graph and recovers more of the true neighbours, at proportionally more work per query. `similarity.hnsw_m` and `similarity.hnsw_ef_construction` govern the graph itself and are build-time costs — raising them makes insertion slower and the graph better connected. The index accepts insertions incrementally, so a corpus can grow without a rebuild.

**`ivf`** partitions the corpus into `similarity.ivf_nlist` cells and probes `similarity.ivf_nprobe` of them per query. Recall rises with `ivf_nprobe` and falls as `ivf_nlist` grows for a fixed `ivf_nprobe`. The partition has to be trained: until the corpus reaches `similarity.ivf_train_threshold` vectors the index is not ready and queries fall back to the exhaustive path. New vectors land in a write buffer and are published into the inverted lists in batches governed by `similarity.ivf_seal_threshold`; a query still reaches buffered vectors, but by scanning the buffer rather than through the cell probes.

The recall each of these delivers on a given corpus is measurable rather than guessable; `tests/benchmark/ann_recall_benchmark.cpp` is what measures it, and [Benchmarks](../benchmarks.md) records the results.

## Fusion is not available for `SIMV`

`SIMV` accepts two options, `filter` and `min_score`. `using=` is not among them:

```bash
nvecd-cli -p 11017 SIMV 3 using=fusion 0.90 0.25 0.12 0.05
```

```text
(error) Invalid SIMV option: using=fusion
```

The HTTP `/simv` endpoint has no `mode` field either. The reason is structural: fusion blends an item's vector neighbours with its co-occurrence neighbours, and co-occurrence is keyed on an item identifier. A query vector has no identifier and therefore no co-occurrence neighbours, so there is no second signal to blend.

An application that wants both can run `SIMV` to find the documents, then `SIM <id> <k> using=fusion` on a result to expand it with behaviour. That is two round trips and two rankings, not one blended query.

## A worked integration

The HTTP API carries the same operations as JSON, which suits a search service better than the line protocol. Set `api.http.enable: true` to expose it.

```python
import json
import urllib.request

BASE = "http://127.0.0.1:8080"


def post(path, payload):
    request = urllib.request.Request(
        BASE + path,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def index_document(doc_id, vector, metadata):
    post("/vecset", {"id": doc_id, "vector": vector, "metadata": metadata})


def search(query_vector, top_k=10, min_score=0.0, metadata_filter=None):
    body = {"vector": query_vector, "top_k": top_k, "min_score": min_score}
    if metadata_filter is not None:
        body["filter"] = metadata_filter
    return post("/simv", body)["results"]


index_document("doc_pricing", [0.88, 0.28, 0.13, 0.06], {"lang": "en", "year": 2023})

for hit in search(encode("how to sort a large file"), top_k=3, min_score=0.8, metadata_filter="lang=en"):
    print(hit["id"], hit["score"])
```

`encode` is the application's own embedding call. `POST /vecset` takes the vector and its metadata in one request, so indexing a document is a single round trip. `POST /simv` answers with the count, the query dimension and the ranked hits:

```json
{"count":2,"dimension":4,"results":[{"id":"doc_sorting","score":0.9992},{"id":"doc_indexing","score":0.9964}],"status":"ok"}
```

Scores in the JSON response are rounded to the same four decimal places the TCP surface renders, so the two surfaces agree digit for digit.

## Limits

The corpus lives in the memory of one process. A dataset larger than RAM is out of scope, and there is no replication, no sharding and no cross-node query. An operator can build a read-only copy by scripting `DUMP SAVE`, copying the file, then `DUMP LOAD` on the target instance; nothing in the server does that, and it is a point-in-time copy with no continuous synchronisation.

There is no text processing of any kind: no tokenizer, no keyword index, no hybrid lexical-and-vector ranking. nvecd ranks vectors, and everything upstream of the vector belongs to the application.

The metrics, the index implementations and the exact search paths are described in full in [Vector search](../vector-search.md).
