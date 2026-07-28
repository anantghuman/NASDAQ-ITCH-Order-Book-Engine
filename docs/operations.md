# Replay operations runbook

## Required preflight

Use a licensed NASDAQ TotalView-ITCH 5.0 BinaryFile obtained through the
organization's approved Nasdaq historical-data account. Do not place licensed
market data in the repository. Historical TotalView-ITCH access is a secured
service, and full daily files are commonly several gigabytes compressed.

Build the validation binary with warnings promoted to errors:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLOB_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Controlled replay

Choose resource caps from the measured capacity of the deployment host, then
replay with rejection failures and regular checkpoints enabled:

```sh
./build/itch_replay /secured/path/SMMDDYY-v50.txt.gz \
  --ring-capacity 65536 \
  --max-instruments 20000 \
  --max-resting-orders 10000000 \
  --checkpoint-every 1000000 \
  --snapshot-locate 1234 \
  --fail-on-reject
```

Checkpoint lines are structured `key=value` records and include applied
message count, observed instruments, resting orders, and rejected events. A
non-zero result from `--fail-on-reject` is an alert condition: preserve the
input name, byte offset (when a decode error exists), command line, and final
metrics before investigating.

## Acceptance validation

For every approved sample day, capture the final message count, rejected
count, active-order count, and chosen locator depth snapshots. Compare
those outputs with a separately maintained reference decoder/book, or with
approved GLIMPSE snapshots taken at the corresponding sequence points. Do not
approve a new feed-specification version until this comparison is clean.

Run the same command on the deployment host with CPU affinity and its normal
resource limits. Archive the resulting percentile report and establish alert
thresholds from several clean runs; single-run maxima are not useful latency
SLOs.

## CI gates

The repository CI runs warning-free Release, ASan/UBSan, and ThreadSanitizer
builds. A production deployment should require all three to pass, in addition
to a clean licensed-corpus validation run on the intended hardware.
