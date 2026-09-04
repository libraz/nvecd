# Benchmarks

This page documents the two benchmark binaries in the repository, what each of them measures, and the figures they have produced. Every number here comes from one of those binaries; nothing is estimated unless the text says so. For guidance on acting on these figures, see [Performance](./performance.md).

## The two benchmark binaries

Both live in `tests/benchmark/`, are built with the rest of the tests, and register with CTest under the `benchmark` label. Their test cases are prefixed `DISABLED_`, so an ordinary test run skips them: they take minutes, and a shared machine under load produces a timing that means nothing.

`similarity_benchmark` profiles the components of the search pipeline directly, with no server and no TCP in the path. It measures eight things: the rebuild time of the vector store's defragmentation pass, a brute-force cosine scan with pre-computed norms, the top-k selection that follows it, reservoir sampling, the two end-to-end engine entry points for search by item and search by vector, query cache lookup latency for a hit and for a miss, and query cache mutation cost under the exclusive lock.

`ann_recall_benchmark` measures the only figure that decides whether an approximate index is usable: the trade-off between recall and latency. Any ANN structure can be made arbitrarily fast by returning worse answers, so a latency for `hnsw` or `ivf` on its own says nothing. It sweeps the knob that controls the trade-off — `hnsw_ef_search` for HNSW, `ivf_nprobe` for IVF — and reports recall next to latency at every point on the curve. Ground truth is an exhaustive scan over the same vectors with the same distance function, so the result is reproducible anywhere the binary builds, with no dataset to fetch.

## Running them

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

./build/bin/similarity_benchmark --gtest_also_run_disabled_tests
./build/bin/ann_recall_benchmark --gtest_also_run_disabled_tests
```

A single measurement:

```bash
./build/bin/similarity_benchmark --gtest_also_run_disabled_tests \
  --gtest_filter="*SearchByIdVectors*"

./build/bin/ann_recall_benchmark --gtest_also_run_disabled_tests \
  --gtest_filter="*HnswRecallVsEfSearch*"
```

Build in `Release`. A plain `make` selects no build type and therefore no optimisation level, which makes every figure below several times worse and comparable to nothing.

`ann_recall_benchmark` also asserts. Beyond printing its curves it checks the measured recall against floors recorded in the binary, so a change that degrades graph or partition quality fails the run instead of quietly making the published curve wrong. It builds a 50,000-vector index per dimension and corpus and computes exhaustive ground truth for every query, which is why it is not attached to any automated run.

## Measurement environment

The figures below were produced on:

- **Hardware**: Apple M5 Max (arm64), 128 GB unified memory
- **SIMD**: NEON, selected at run time
- **Build**: `Release`, `-O3 -march=native`, Apple Clang
- **Configuration**: defaults — `distance_metric: cosine`, `sample_size: 10000`, `index_type: flat`
- **Method**: median of 15 iterations against a populated store, dimension 128, `top_k` 10

Apple Silicon has higher memory bandwidth than a typical server memory configuration, and NEON is 128 bits wide against AVX2's 256. Absolute latencies on an x86_64 server therefore differ in both directions and have not been measured; the scaling behaviour with corpus size is what transfers.

## Search latency

End-to-end through `SimilarityEngine`, dimension 128, `top_k` 10, cosine:

| Vectors | Search by item id | Search by query vector |
|---|---|---|
| 1,000 | 0.010 ms | 0.010 ms |
| 10,000 | 0.092 ms | 0.092 ms |
| 50,000 | 0.59 ms | 0.58 ms |
| 100,000 | 0.90 ms | 0.91 ms |

These run at the default `sample_size` of 10,000, and sampling engages once the corpus exceeds twice that value. The two smallest rows are therefore exhaustive scans; the 50,000 and 100,000 rows select a sample first, and the reservoir pass that selects it walks the whole corpus, so their cost grows with corpus size even though the distance scan does not. The two entry points agree to within a percent, which is expected: they differ only in whether the query vector is looked up or supplied.

Dividing through, the 100,000-vector figure is roughly 1,100 queries per second per thread.

## Component costs

Measured separately at 100,000 vectors, dimension 128:

| Component | Median |
|---|---|
| Brute-force cosine scan over the whole corpus, pre-computed norms | 0.73 ms |
| Top-k selection by partial sort over 100,000 candidates | 0.06 ms |

The scan is the dominant cost of an exhaustive search. These are not a decomposition of the end-to-end row above, which samples at this size rather than scanning everything; they are the cost of the exhaustive path that `sample_size: 0` selects.

## Defragmentation

Deleting a vector leaves a tombstone. The store rebuilds itself when tombstones exceed a quarter of its slots, taking an exclusive lock for the swap. With 25% of vectors deleted, dimension 128:

| Vectors | Rebuild |
|---|---|
| 1,000 | 0.03 ms |
| 10,000 | 0.27 ms |
| 50,000 | 1.49 ms |
| 100,000 | 3.07 ms |

## Query cache latency

Lookup latency, measured across caches holding 100, 1,000, 10,000 and 50,000 resident entries:

| Operation | Median |
|---|---|
| Miss — key not present | 0.042 µs, about 42 ns |
| Hit — key present, entry decompressed and returned | 0.21 µs, about 208 ns |

Against the 0.90 ms end-to-end figure at 100,000 vectors, a hit costs about one four-thousandth of a cold query. Lookup cost does not grow measurably with the number of resident entries, so a larger cache is not a slower one.

The mutation benchmark measures what the cache costs a writer rather than a reader: inserting one entry into an already-populated cache, and invalidating the entries that reference one item, at 1,000, 5,000 and 20,000 resident entries. Both hold the exclusive lock. No figures from it are published here; run it to get numbers for your own hardware, and see [Performance](./performance.md) for what to do with them.

## Approximate index recall

Recall at `top_k` 10 against an exhaustive scan of the same corpus, 50,000 vectors, cosine, 200 queries per point, dimensions 128 and 768. Two corpus shapes are measured. A uniform corpus draws independent random directions, which are near-orthogonal in high dimensions and leave neighbours separated by almost nothing — the pathological case for any graph or partition index. A clustered corpus draws points around 200 latent centroids, which is the shape real embeddings have.

The recall floors the benchmark asserts on the clustered corpus:

| Index | Knob | Recall@10 floor |
|---|---|---|
| `hnsw` | `hnsw_ef_search` 10 | 0.97 |
| `hnsw` | `hnsw_ef_search` 32 | 0.99 |
| `hnsw` | `hnsw_ef_search` 64 | 0.99 |
| `ivf` | `ivf_nprobe` 1 | 0.90 |
| `ivf` | `ivf_nprobe` 4 | 0.98 |
| `ivf` | `ivf_nprobe` 8 | 0.98 |

HNSW is measured with `hnsw_m` 16 and `hnsw_ef_construction` 200; IVF with `ivf_nlist` 256. The sweeps run wider than the table — `hnsw_ef_search` from 10 to 512, `ivf_nprobe` from 1 to 128 — and print recall, median and 99th-percentile latency, and the speedup against the exhaustive scan at every point.

The uniform corpus is deliberately left unchecked. Near-orthogonal vectors are where approximate search stops paying for itself, and the conclusion there is that it does not pay rather than that it reaches some particular number.

A third measurement drives IVF through the path the server actually runs, rather than through the index in isolation: vectors arrive one at a time, training runs asynchronously on a sample once `ivf_train_threshold` is crossed, and buffered vectors are published into the inverted lists by a background pass. Its speedups are reported against `index_type: flat` on the same corpus through the same engine, which is the alternative you are actually choosing between. It applies the same recall floors, so the published curve is checked against what the server produces and not against a layout the benchmark built for itself.

## What is not measured

Three things readers ask for that no benchmark here produces, and which therefore appear nowhere on this page:

- **Per-operation SIMD timings.** The scalar, NEON and AVX2 distance kernels are covered by correctness tests, not by a timing harness. The only SIMD figure available is the whole-scan cost above, on NEON.
- **Recall of the sampling path.** `sample_size` trades accuracy for speed, and the recall benchmark measures the ANN indexes rather than the sample. To find the value for your corpus, run the same queries with `sample_size: 0` and with the value you are considering, and compare the result sets.
- **Behaviour beyond 100,000 vectors, and snapshot save times and file sizes.** No benchmark loads a larger corpus or times a snapshot. [Performance](./performance.md) gives a method for measuring both on your own data.
