# Recommendations

The smallest thing nvecd does: a recommender built from a stream of interactions, with no vectors, no embedding model and no training step. It answers the question "what else did the people who acted on this item act on".

## The model

An `EVENT` records that one item was acted on inside one context. Two items that appear in the same context become co-occurrence neighbours, and the strength of that association is the product of the two event scores, accumulated over every context the pair appears in. `SIM` returns an item's neighbours, highest score first.

That is the whole model. The index exists from the second event onwards; there is nothing to train and nothing to rebuild.

## Choosing a context key

The context is the grouping unit — it decides what "together" means. It is an opaque string, so any grouping the application can name works:

| Context key | Association it produces |
|---|---|
| `user_<id>` | Items the same person acted on, over their recent history |
| `session_<id>` | Items acted on in one visit |
| `order_<id>` | Items bought in one transaction |
| `playlist_<id>` | Items a curator put side by side |

A context holds the `events.ctx_buffer_size` most recent events in a ring buffer, and only items inside that buffer co-occur. The buffer length is therefore the window over which "together" is defined: a per-user context with a 50-event buffer associates an item with that user's last 50 interactions, while a per-order context associates it with the rest of that order and nothing else. Narrower contexts give sharper associations from more events; wider ones build a usable graph sooner.

`events.max_contexts` caps how many contexts are retained, evicting the least recently active first. It defaults to `0`, which is unlimited.

Context and item identifiers must be non-empty and must not contain whitespace or control characters. Bytes at or above `0x80` are accepted, so UTF-8 identifiers work.

## Scoring an interaction

The score is an integer in the inclusive range `[0, 100]`. Because a pair contributes the product of the two scores, the scale is quadratic rather than linear: two purchases at `100` contribute `10000` to their edge, while two page views at `60` contribute `3600`. A score of `0` creates no edge at all.

A workable ladder for a catalogue application:

| Interaction | Score |
|---|---|
| Purchase | 100 |
| Add to cart | 85 |
| Add to wishlist | 70 |
| Product page view | 60 |
| Impression in a list | 40 |

The absolute numbers matter less than the gaps between them. Choose the gaps so that the ranking they produce is the ranking the application wants.

## ADD, SET and DEL

`EVENT` takes one of three types, which differ in how a repeat is handled:

| Type | Meaning | Repeat behaviour |
|---|---|---|
| `ADD` | A stream event: a click, a view, a purchase | A repeat of the same context, item and score within `events.dedup_window_sec` is dropped |
| `SET` | A state event: a rating, a bookmark, a like | Re-sending the same value for the same context and item is a no-op |
| `DEL` | A removal: an unlike, a removed bookmark | Stored with score `0` whatever the request asked for, so it contributes no association |

`SET` is what makes a client safe to retry. Sending `EVENT dave SET widget 90` twice leaves the index in the same state as sending it once, so a resend after a timeout cannot inflate a score.

## A worked session

Start the server with the shipped example configuration:

```bash
./build/bin/nvecd -c examples/config.yaml
```

Record what three shoppers did. Each shopper is a context; each purchase is an event.

```bash
nvecd-cli -p 11017 EVENT alice ADD widget 100
nvecd-cli -p 11017 EVENT alice ADD gasket 80
nvecd-cli -p 11017 EVENT bob   ADD widget 100
nvecd-cli -p 11017 EVENT bob   ADD flange 95
nvecd-cli -p 11017 EVENT carol ADD gasket 80
nvecd-cli -p 11017 EVENT carol ADD flange 95
```

Each of those prints the command that was accepted:

```text
EVENT
```

Now ask what goes with `widget`:

```bash
nvecd-cli -p 11017 SIM widget 10 using=events
```

```text
(2 results, showing 2)
1) flange (score: 9500)
2) gasket (score: 8000)
```

`flange` scores `9500` because bob bought it alongside `widget` at `100 × 95`. `gasket` scores `8000` from alice's `100 × 80`. Carol's pair contributed to the `gasket`–`flange` edge, not to either of `widget`'s.

## Reading the result

`nvecd-cli` renders the response for a terminal. The wire form is a count line followed by one line per result:

```text
OK RESULTS 2
flange 9500.0000
gasket 8000.0000
```

Scores in `using=events` are the raw accumulated products. They are unbounded, they grow as the same pair keeps appearing, and they are comparable only within one result set — `9500` says nothing on its own.

Omitting `using=` selects fusion, which is the default mode. With no vectors in the store, fusion falls back to the event signal alone and scales each result set to `[0, 1]` by min-max normalisation:

```bash
nvecd-cli -p 11017 SIM widget 10
```

```text
(2 results, showing 2)
1) flange (score: 1)
2) gasket (score: 0)
```

The ranking is identical; only the scale changed. Note that the lowest-ranked result normalises to exactly zero and is still returned, because `min_score` defaults to `0` and the comparison is inclusive. An events-mode query with a raw cutoff is the more predictable filter for this shape of data:

```bash
nvecd-cli -p 11017 SIM widget 10 using=events min_score=9000
```

## Ageing associations out

Co-occurrence scores accumulate without bound unless something removes weight. Two independent mechanisms do that.

Global decay multiplies every score in the index by `events.decay_alpha` every `events.decay_interval_sec` seconds. The shipped defaults are `0.99` every hour. Slower decay suits stable preferences; faster decay suits catalogues where relevance turns over quickly.

Pruning drops what has already decayed into irrelevance. `events.min_support` removes edges whose score falls below a threshold, and `events.max_neighbors_per_item` keeps only the strongest neighbours of each item. Both default to disabled, which is fine for a small catalogue and not fine for a large one — see the [real-time feed guide](./real-time-feed.md) for the settings that keep a high-write workload bounded.

## What co-occurrence alone cannot do

An item nobody has interacted with has no neighbours, and a query for it returns an empty result rather than an error:

```bash
nvecd-cli -p 11017 SIM sprocket 10 using=events
```

```text
(0 results)
```

There is no fallback: no amount of tuning makes an unobserved item recommendable from events. That is the cold-start limit, and it is the reason to add vectors.

Metadata filtering is also unavailable in an events-only deployment. `METASET` requires the item to already have a vector, and fails otherwise:

```bash
nvecd-cli -p 11017 METASET sprocket category:tools
```

```text
(error) Vector not found for metadata: sprocket
```

With no metadata stored, any `filter=` on a query matches nothing.

## When vectors start to pay

Add vectors once either limit above becomes a real cost: a catalogue with continuous new listings that cannot wait for interaction data, or results that need to be constrained by category, availability or price. A vector makes an item recommendable on its content from the moment it is stored, and `using=fusion` blends the two signals per query. The [e-commerce guide](./e-commerce.md) covers that, including weighting that follows how much event data each item has.

Adding vectors does not change anything written above. Events keep working exactly as they did, and `using=events` keeps answering from them alone.

The scoring rules, decay and pruning are described in full in [Events and co-occurrence](../events-and-co-occurrence.md).
