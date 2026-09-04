# Real-time feed

An engagement-driven feed, where what the audience is reacting to right now outranks what it reacted to yesterday. This guide covers the two independent decay mechanisms, timestamps, negative signals from skips and dismissals, and the settings that keep a continuous high-rate write stream inside a fixed memory budget.

It assumes the co-occurrence model from the [recommendations guide](./recommendations.md).

## What makes this workload different

A catalogue recommender is read-heavy against a slowly changing dataset. A feed is the opposite: every impression, every scroll past and every dismissal is a write, and the association a query reads was often formed minutes ago. Two consequences follow.

Recency has to be part of the score rather than a post-processing step, because there is no separate "recent" list to intersect with. And memory has to be bounded by configuration rather than by the arrival rate, because nothing about the traffic will bound it.

## Two decays, and they are not the same

nvecd ages associations out in two independent ways. A feed usually wants both.

**Global decay** multiplies every score in the co-occurrence index by `events.decay_alpha` every `events.decay_interval_sec` seconds. It runs on a timer, applies uniformly, and does not care when any particular event arrived. It is what stops scores growing without bound.

**Temporal decay** is per pair and applies at ingestion. With `events.temporal_cooccurrence` enabled, the contribution of a pair is multiplied by an exponential factor derived from how far each of the two events sits behind the newer of them, halving once per `events.temporal_half_life_sec` seconds of that gap. Two events at the same instant contribute their full product; two events an hour apart under a one-hour half-life contribute half.

The distinction matters because they answer different questions. Global decay asks how old the association is; temporal decay asks how far apart the two engagements were. An item watched immediately after another is a stronger signal than one watched an hour later in the same session, and only temporal decay expresses that.

For a feed, set the half-life to the span over which co-engagement is still meaningful — an hour or a few hours for a fast-moving stream — and set the global decay interval short enough that yesterday's trend has visibly faded.

## Timestamps

Event timestamps are Unix seconds. `EVENT` accepts an explicit `timestamp=<seconds>` after the score, and falls back to the server clock when it is omitted:

```bash
nvecd-cli -p 11017 EVENT u_alice ADD clip_sunset 90 timestamp=1788400000
nvecd-cli -p 11017 EVENT u_alice ADD clip_sunset 90
```

Send the explicit form whenever the event is replayed from a queue or a log, so temporal decay is computed against when the engagement happened rather than when the write landed.

## Negative signals

A skip, a dismissal or an unfollow is information, and scoring it as a weak positive keeps it in the graph as a weak positive. `EVENT <ctx> DEL <id>` is the removal form: it is stored with score `0` whatever the request asked for, so it contributes no association of its own.

With `events.negative_signals` enabled it does more. The removed item's edges to everything else still in that context are reduced by `other_event_score × events.negative_weight`. The reduction is symmetric and applies once per prior event of the other item in the buffer.

Two properties are worth knowing before relying on it. The reduction is linear in the other item's score while a positive co-occurrence is the product of two scores, so one dismissal trims an edge rather than erasing it — the mechanism is a correction, not an undo. And a second `DEL` for the same context and item is deduplicated and changes nothing until the item is engaged with again in that context.

For a graded signal, a low `ADD` score is often the better tool: a two-second view at score `5` against a completed view at score `95` already separates them by a factor of nineteen on every pair it forms.

## Repeat engagement

By default a repeat of the same context, item and score within `events.dedup_window_sec` seconds is dropped. That default protects a catalogue application from double-submitted forms, and it silently discards real signal in a feed where re-watching the same clip three times is exactly what the ranking should notice.

Set `events.dedup_window_sec: 0` to count every repeat. Leave `events.dedup_cache_size` alone: setting it to `0` disables deduplication entirely, including the `SET` and `DEL` idempotency tracking that negative signals depend on.

## Bounding memory

Nothing in a continuous write stream stops the index growing. These five keys are what bound it.

| Key | Bounds |
|---|---|
| `events.ctx_buffer_size` | Events retained per context, as a ring buffer. Also the window over which two items count as co-engaged. |
| `events.max_contexts` | Contexts retained, least recently active evicted first. `0` is unlimited. |
| `events.max_neighbors_per_item` | Neighbours kept per item, ranked by absolute score. `0` is unlimited. |
| `events.min_support` | Floor on absolute score below which an edge is removed. `0` disables the check. |
| `events.dedup_cache_size` | Entries in the deduplication LRU. |

`max_contexts` is the one that scales with audience and is unlimited by default. A feed with one context per viewer accumulates one entry per viewer who has ever been seen, so set it to the number of active viewers the process should hold rather than leaving it open.

`min_support` and global decay work as a pair: decay pushes stale edges towards zero on a timer, and `min_support` is what removes them once they get there. With `min_support` at its default of `0`, decayed edges shrink but never leave, and the index only ever grows.

## A worked session

```yaml
events:
  ctx_buffer_size: 30
  max_contexts: 500000
  max_neighbors_per_item: 200
  min_support: 1.0
  dedup_window_sec: 0
  decay_interval_sec: 900
  decay_alpha: 0.9
  temporal_cooccurrence: true
  temporal_half_life_sec: 3600.0
  negative_signals: true
  negative_weight: 0.5
```

Alice watches two clips back to back:

```bash
NOW=$(date +%s)
nvecd-cli -p 11017 EVENT u_alice ADD clip_sunset 90 timestamp=$NOW
nvecd-cli -p 11017 EVENT u_alice ADD clip_reef   95 timestamp=$NOW
nvecd-cli -p 11017 SIM clip_sunset 5 using=events
```

```text
(1 results, showing 1)
1) clip_reef (score: 8550)
```

`90 × 95` with no temporal penalty, because the two engagements share a timestamp.

Bob watched `clip_sunset` an hour ago and `clip_forest` just now:

```bash
nvecd-cli -p 11017 EVENT u_bob ADD clip_sunset 90 timestamp=$((NOW - 3600))
nvecd-cli -p 11017 EVENT u_bob ADD clip_forest 95 timestamp=$NOW
nvecd-cli -p 11017 SIM clip_sunset 5 using=events
```

```text
(2 results, showing 2)
1) clip_reef (score: 8550)
2) clip_forest (score: 4275)
```

The same `90 × 95` product, halved to `4275` by one half-life of separation. That is temporal decay doing the whole job: without it the two pairs would be indistinguishable.

Alice then dismisses `clip_reef`:

```bash
nvecd-cli -p 11017 EVENT u_alice DEL clip_reef
nvecd-cli -p 11017 SIM clip_sunset 5 using=events
```

```text
(2 results, showing 2)
1) clip_reef (score: 8505)
2) clip_forest (score: 4275)
```

`clip_sunset` scored `90`, so the edge lost `90 × 0.5 = 45`. Repeating the same `DEL` changes nothing, because it is deduplicated against the state already recorded for that context and item.

## Limits

The write-ahead log is disabled by default, and a feed generating writes continuously loses everything since the last snapshot on an unclean stop. Enabling `wal.enabled` puts every accepted write on disk before it is acknowledged; `wal.sync_on_write` and `wal.sync_interval_ms` set how much of that is traded back for throughput.

There is no replication and no sharding: one dataset, one process, one node. An operator can build a read-only copy by scripting `DUMP SAVE`, copying the file, then `DUMP LOAD` on the target instance, but that is a point-in-time procedure with nothing continuous behind it, and writes arriving on the source after the save are not in the copy.

This guide quotes no throughput or latency figure, because the numbers depend entirely on the shape of the traffic and the machine. [Benchmarks](../benchmarks.md) records what has been measured and on what hardware.

The scoring rules, both decay mechanisms and the pruning behaviour are described in full in [Events and co-occurrence](../events-and-co-occurrence.md).
