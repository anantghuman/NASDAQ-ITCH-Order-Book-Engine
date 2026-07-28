# TotalView-ITCH 5.0 limit order book

A C++20 full-depth, price-time-priority limit order book reconstructed from native
NASDAQ TotalView-ITCH 5.0 historical data. The hot path is a single producer
(decode/ingest) and single consumer (book/matcher) pipeline.

The project intentionally separates two modes:

- **reconstruction** applies real ITCH lifecycle events to recover displayed depth;
- **simulation** submits hypothetical orders against that recovered book without
  contaminating historical data.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run a real native ITCH sample (uncompressed or gzip-compressed when CMake
finds zlib):

```sh
./build/itch_replay /path/to/01302019.NASDAQ_ITCH50.gz \
  --max-instruments 20000 --max-resting-orders 10000000 \
  --checkpoint-every 1000000 --snapshot-locate 1234 --fail-on-reject
```

The executable streams one frame at a time, decodes on its producer thread,
and reconstructs books on its consumer thread. It prints message throughput
and HDR-style p50/p99/p99.9 latency for add, cancel, execution/match, replace,
and enqueue-to-apply timing.

The resource limits are explicit deployment guardrails. `--fail-on-reject`
turns any invalid lifecycle event or breached limit into a failing replay,
which is appropriate for validation and production health checks.

See [the architecture notes](docs/architecture.md) for message coverage,
matching semantics, and complexity guarantees.
Use the [operations runbook](docs/operations.md) before replaying licensed
historical data in a deployment environment.

To benchmark this specific machine without requiring market-data access:

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DLOB_BUILD_BENCHMARKS=ON
cmake --build build-bench
./build-bench/lob_latency_bench 200000
```
