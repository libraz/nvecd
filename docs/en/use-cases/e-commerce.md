# E-commerce

A product recommender that answers from two signals at once: what the catalogue says a product is, and what shoppers did with it. This guide covers engagement-weighted events, storing product embeddings, `using=fusion` with weighting that follows each item's data density, and the metadata filter a real catalogue needs on its results.

It assumes the co-occurrence model from the [recommendations guide](./recommendations.md).

## Engagement is not one signal

A view, a cart addition and a purchase are all interactions, and treating them as one event type throws away the difference. `EVENT` carries an integer score in `[0, 100]`, and the co-occurrence contribution of a pair is the product of the two scores, so the gap between the tiers is amplified rather than averaged:

```bash
nvecd-cli -p 11017 EVENT alice ADD lamp 60    # product page view
nvecd-cli -p 11017 EVENT alice ADD lamp 85    # added to cart
nvecd-cli -p 11017 EVENT alice ADD bulb 100   # purchased
```

Those three events leave alice's context holding all three, so the `lamp`–`bulb` edge receives `60 × 100` from the view and `85 × 100` from the cart addition. A purchase pair contributes `100 × 100`; a pair of views contributes `60 × 60`. Nothing in the configuration encodes the ladder — the scores themselves are the ladder.

Use `ADD` for stream events of this kind. For a state a shopper toggles, such as a wishlist entry, use `SET`, which is idempotent on the last value and therefore safe to replay.

## Storing product vectors

nvecd does not produce embeddings. Encode each product with whatever model the catalogue already uses — an image encoder over the product photo, a text encoder over title and description, or a concatenation of both — and send the result:

```bash
nvecd-cli -p 11017 VECSET lamp 0.82 0.31 0.11 0.05
```

The store fixes its dimension on the first vector it accepts, and every later `VECSET` must match that width. `vectors.default_dimension` pre-sizes the ANN index rather than enforcing a width, so a store whose first vector has 768 components serves 768-dimensional queries regardless of what the key says. The examples in this guide use four components so the commands fit on one line.

`vectors.distance_metric` selects how closeness is measured: `cosine` (the default), `dot` or `l2`. Match it to what the encoder was trained for.

Metadata rides along on the same write over HTTP, which avoids a second round trip per product. The HTTP server is off by default; set `api.http.enable: true` to reach it.

```bash
curl -s -X POST http://127.0.0.1:8080/vecset \
  -H 'Content-Type: application/json' \
  -d '{"id":"lamp","vector":[0.82,0.31,0.11,0.05],"metadata":{"category":"lighting","in_stock":true,"price":39.9}}'
```

```json
{"dimension":4,"status":"ok"}
```

Over TCP the equivalent is `METASET`, which takes comma-separated pairs and requires the vector to exist first:

```bash
nvecd-cli -p 11017 METASET lamp category:lighting,in_stock:true
```

## Fusion

`using=fusion` runs both searches and merges them. Each side is min-max normalised to `[0, 1]` within its own result set, then combined as `alpha × vector_score + beta × event_score`. The two weights come from `similarity.fusion_alpha` and `similarity.fusion_beta`.

The union matters: the vector search runs over the whole store rather than over the event neighbours, so a content-similar product that has never been co-purchased can still surface, and a co-purchased product with no vector can too.

## Adaptive weighting

A fixed blend serves a mature catalogue and fails a new listing. A product stored yesterday has no purchase history, so its event side contributes nothing and a fixed `beta` spends part of the score budget on an empty signal.

`similarity.adaptive_fusion` makes the blend follow how much event data the query item actually has. The weight is interpolated from the item's co-occurrence neighbour count:

```text
ratio = min(1, neighbour_count / adaptive_maturity_threshold)
alpha = adaptive_max_alpha - ratio × (adaptive_max_alpha - adaptive_min_alpha)
beta  = 1 - alpha
```

A new item sits near `adaptive_max_alpha` and is ranked mostly by its vector; a well-observed one approaches `adaptive_min_alpha` and is ranked mostly by behaviour. The neighbour count is read from the co-occurrence index directly rather than from the truncated result list, so a small `top_k` cannot make a mature item look new.

Per query, `adaptive=on` and `adaptive=off` override the configured default.

## A worked session

```yaml
events:
  ctx_buffer_size: 100
  decay_interval_sec: 86400
  decay_alpha: 0.98

vectors:
  default_dimension: 768
  distance_metric: "cosine"

similarity:
  fusion_alpha: 0.6
  fusion_beta: 0.4
  adaptive_fusion: true
  adaptive_min_alpha: 0.2
  adaptive_max_alpha: 0.9
  adaptive_maturity_threshold: 50
```

Load a four-product catalogue. `shade` is the new listing: it has a vector and metadata, but no shopper has touched it.

```bash
nvecd-cli -p 11017 VECSET lamp  0.82 0.31 0.11 0.05
nvecd-cli -p 11017 VECSET bulb  0.79 0.35 0.14 0.04
nvecd-cli -p 11017 VECSET shade 0.71 0.44 0.20 0.09
nvecd-cli -p 11017 VECSET desk  0.12 0.20 0.88 0.40

nvecd-cli -p 11017 METASET lamp  category:lighting,in_stock:true
nvecd-cli -p 11017 METASET bulb  category:lighting,in_stock:true
nvecd-cli -p 11017 METASET shade category:lighting,in_stock:false
nvecd-cli -p 11017 METASET desk  category:furniture,in_stock:true

nvecd-cli -p 11017 EVENT alice ADD lamp 60
nvecd-cli -p 11017 EVENT alice ADD lamp 85
nvecd-cli -p 11017 EVENT alice ADD bulb 100
nvecd-cli -p 11017 EVENT bob   ADD lamp 100
nvecd-cli -p 11017 EVENT bob   ADD desk 60
```

Each signal on its own. Behaviour:

```bash
nvecd-cli -p 11017 SIM lamp 5 using=events
```

```text
(2 results, showing 2)
1) bulb (score: 14500)
2) desk (score: 6000)
```

Content:

```bash
nvecd-cli -p 11017 SIM lamp 5 using=vectors
```

```text
(3 results, showing 3)
1) bulb (score: 0.9978)
2) shade (score: 0.975)
3) desk (score: 0.315)
```

Behaviour knows nothing about `shade`; content ranks it second. Fusion, with adaptive weighting on:

```bash
nvecd-cli -p 11017 SIM lamp 5 using=fusion
```

```text
(3 results, showing 3)
1) bulb (score: 1)
2) shade (score: 0.8429)
3) desk (score: 0)
```

`lamp` has two co-occurrence neighbours against a maturity threshold of 50, so the ratio is `0.04` and `alpha` lands at `0.872`. The same query with the fixed weights shows what that bought:

```bash
nvecd-cli -p 11017 SIM lamp 5 using=fusion adaptive=off
```

```text
(3 results, showing 3)
1) bulb (score: 1)
2) shade (score: 0.58)
3) desk (score: 0)
```

`shade` scores `0.58` under the fixed `0.6 / 0.4` split and `0.8429` under the adaptive one, because the adaptive weights recognise that `lamp`'s event signal is thin and lean on its vector instead.

Querying the new product itself works from the first moment it has a vector:

```bash
nvecd-cli -p 11017 SIM shade 5 using=fusion
```

```text
(3 results, showing 3)
1) bulb (score: 1)
2) lamp (score: 0.9786)
3) desk (score: 0)
```

`shade` has no events at all. When only one side has candidates, the contributing weight is renormalised to one, so the surviving signal is not shrunk by the weight of the missing one.

## Filtering the catalogue

A recommendation that names an out-of-stock product or crosses a category boundary is a bug in the storefront. `filter=` constrains the result against item metadata:

```bash
nvecd-cli -p 11017 SIM lamp 5 using=fusion filter=category=lighting,in_stock=true
```

```text
(1 results, showing 1)
1) bulb (score: 0.9981)
```

Conditions are comma-separated and combined with AND. The available operators are `=`, `:` (an alias for `=`), `!=`, `>`, `<`, `>=`, `<=` and `in(a|b|c)`, so `price<50` and `category=in(lighting|decor)` are both valid. Values are typed on the way in: `true` and `false` become booleans, anything parsing as an integer or a float becomes a number, and everything else stays a string.

An item with no metadata never matches a non-empty filter. Since `METASET` requires a vector, only items that have vectors are filterable at all.

The score for `bulb` changed from `1` to `0.9981` when the filter was applied. Normalisation is computed over the candidates that survive filtering, so scores are relative within one result set and are not comparable across queries. Rank the results; do not threshold them against a number remembered from another query.

## Limits

nvecd holds one dataset in the memory of one process. There is no replication, no sharding and no cross-node query, and nothing in the server copies data to another instance. An operator can build a read-only copy by scripting it: `DUMP SAVE` on the source, copy the resulting file, `DUMP LOAD` on the target. That is a manual, point-in-time procedure with no continuous synchronisation behind it — writes that arrive on the source after the save are simply not in the copy.

Query cost is set by catalogue size and configuration, not by anything on this page; see [Benchmarks](../benchmarks.md) for what has actually been measured.

The weighting rules, the normalisation and the single-source fallback are described in full in [Fusion](../fusion.md).
