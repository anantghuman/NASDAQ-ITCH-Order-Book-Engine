# Architecture

## Data flow

`itch_replay` reads a native two-byte-length-prefixed TotalView-ITCH 5.0
file. A producer thread decodes one frame at a time, creates a typed event,
and publishes it to a bounded SPSC queue. A single consumer owns every book
and applies lifecycle events in feed order.

```
historical ITCH file -> frame reader -> ITCH decoder -> SPSC ring -> matcher -> per-locate books
```

The input reader is streaming, so input size does not add to the application's
working set. Book state naturally grows with the number of active orders.

## ITCH coverage

The decoder fully parses `S`, `R`, `H`, `A`, `F`, `E`, `C`, `X`, `D`, `U`,
`P`, `Q`, `B`, and `I`. The reconstruction engine changes displayed depth only
for the historical lifecycle messages:

| ITCH type | Book effect |
| --- | --- |
| `A` / `F` | Rest a new displayed order at the tail of its price-level FIFO |
| `E` / `C` | Decrement the referenced resting order for an execution |
| `X` | Decrement the referenced resting order for a cancellation |
| `D` | Remove the referenced resting order |
| `U` | Delete the old reference and rest the new reference at tail priority |

Trades, crosses, broken trades, imbalance, directory, and trading-state
messages are retained as typed observable events but do not mutate displayed
depth. Other complete ITCH administrative messages are surfaced as
`SkippedMessage` so a full historical replay continues; the optional common
header is retained when that record has one. Malformed or truncated frames
remain hard errors.

## Book data structure and complexity

Each `Stock Locate` owns an independent `OrderBook`. Its order-ID hash index
points directly to an intrusive FIFO node in a price level. This makes ID
lookup, cancel, execution decrement, and modification expected O(1). A
same-price quantity reduction keeps FIFO priority; any quantity increase or
price change is requeued at the tail. `U` replacement always loses priority.

Price levels are stored sparsely and indexed by a four-tier, 8-bit-per-level
bitmap radix tree over ITCH's complete 32-bit price field. Best bid/ask,
activate, and deactivate take a fixed four-tier traversal, independent of the
number or range of active prices. A match runs in O(number of resting orders
and price levels consumed), which is the required work to produce its fills.

## Concurrency and measurement

`SpscRing<T>` requires exactly one producer and one consumer. Its capacity is a
power of two; producer and consumer indexes are cache-line-separated lock-free
atomics with release/acquire publication. It allocates storage only during
construction.

Each latency histogram is single-writer on the matching thread. `HdrHistogram`
uses HDR-style exponent/sub-bucket compaction, covers the `uint64_t`
nanosecond range, and maintains roughly 0.1% relative precision above its
exact low range. Reported percentiles use nearest-rank semantics.

## Simulation matching

`MatchingEngine::submit_limit` and `submit_market` are simulation-only entry
points. They sweep the opposite best levels in price-time order and write fills
to a caller-provided `std::span<Fill>`, so matching does not allocate. The
result reports total fills and whether the supplied output buffer was too
small. Native historical Add messages use `apply` instead; they are already
the post-match exchange feed and must never be matched a second time.
