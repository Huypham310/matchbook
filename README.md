# MatchBook

A low-latency limit order book and matching engine in C++20.

MatchBook is a single-threaded matching engine with price-time priority, built to see where the
nanoseconds go: how the book is laid out in memory, what allocations sit on the hot path, and how
orders move between threads without locks. Every design choice is measured with latency histograms
(p50 / p99 / p99.9) instead of averages.

> Status: early work in progress.

## Planned features

- Order book with O(1) cancel and modify: intrusive FIFO lists per price level, a pre-allocated
  order pool, and no heap allocation on the matching path
- Matching with price-time priority for limit, market, IOC and FOK orders, partial fills, cancel
  and modify (losing queue priority on size increase or price change)
- Compact binary order protocol with zero-copy parsing
- Lock-free single-producer / single-consumer ring buffer between the gateway, engine and
  market-data threads, with cache-line padding to avoid false sharing
- Differential tests against a naive reference book, fuzzing, AddressSanitizer / UBSan
- Replay of synthetic Poisson order flow and real Coinbase trades recorded by
  [QuantLab](https://github.com/Huypham310/quantlab)
- Google Benchmark suites and per-message latency histograms
- Python bindings (pybind11) so QuantLab's backtester can match against a real book

## Design notes

Prices are integers counted in ticks (`int64_t`), never floating point: `0.1 + 0.2 != 0.3` is not
something a matching engine can afford. Text prices from exchanges are converted to ticks once, at
the edge.

## Order book

`matchbook::OrderBook` holds the resting orders of one instrument:

- **Price levels in a sorted vector per side, best price at the back.** Most activity happens at
  or near the top of the book, so a new best level is a `push_back` and an emptied top level a
  `pop_back`; other levels are found by binary search.
- **Each level is an intrusive FIFO list** (`prev` / `next` live inside the order), so time
  priority is the list order and removing an order from the middle is O(1) without a search.
- **Orders and levels come from fixed-size pools** allocated up front; the id index and the
  ladders reserve their capacity in the constructor. Running out of slots is reported as
  `AddResult::BookFull` instead of allocating.
- **Modify** keeps priority only when the size goes down at the same price, like most
  exchanges; a price change or a size increase re-queues the order.

Correctness is checked with a differential test: the book and a deliberately naive reference
(a flat map, everything recomputed) get the same 200,000 random add / cancel / modify operations
over a narrow price band, and every depth snapshot and every level's queue order must match. A
mutation check (letting same-price size changes keep priority) is caught within 2,500 steps.

## Matching

`matchbook::MatchingEngine` puts price-time priority on top of the book:

- An incoming order walks the opposite side from the best price, filling the oldest order at each
  level first; every trade prints at the **resting (maker) price**.
- **GTC** limit orders rest whatever is left at their limit; **IOC** and market orders drop it;
  **FOK** checks the available quantity first and either fills completely or leaves no trace.
- **Modify** that reprices an order through the spread turns it into an aggressive order and
  matches it; a passive modify keeps the book rules (priority kept only when shrinking).
- Events (trade, rested, cancelled, rejected) go into a reserved buffer per call, so an ordinary
  order doesn't allocate.

The book can never be crossed: a limit order only rests after everything it could trade with is
gone. A differential test checks the engine against a deliberately slow reference matcher (a flat
map, re-sorted for every order) on 120,000 random limit / market / IOC / FOK / cancel / modify
operations: identical event streams, identical book depth, and no crossed book at any step.
Turning the FOK check from `<` into `<=` is caught within 9 steps.

## Wire protocol

Orders and events travel as fixed-size, little-endian binary messages (OUCH / ITCH style)
behind an 8-byte header (`u16 length, u8 type, u8 version, u32 seq`):

| Type | Dir | Size | Body |
|---|---|---|---|
| `N` new order | in | 40 | id, price, qty, side, order type, time in force |
| `X` cancel | in | 16 | id |
| `M` modify | in | 32 | id, price, qty |
| `T` trade | out | 40 | maker id, taker id, price, qty |
| `R` rested / `C` cancelled | out | 32 | id, price, qty |
| `J` rejected | out | 24 | id, reason |

- **Zero-copy decode:** `proto::decode` validates the header (type, version, exact length,
  enum bytes) and returns a `MessageView` that reads each field straight out of the receive
  buffer with an unaligned load. It never throws, never allocates and never reads past the
  buffer; 20,000 random buffers in the tests check that.
- **`proto::Session`** turns a byte stream into engine calls and encodes the resulting events.
  Messages split across reads are stitched together; feeding the same stream in random 1-120
  byte chunks gives byte-identical output to feeding it whole. The first malformed message
  stops the session, since fixed-size framing has no way to resync.
- The `NewOrder` layout is pinned by a golden-bytes test, and an end-to-end test checks that
  3,000 orders sent over the wire produce exactly the events of calling the engine directly.

```text
$ matchbook bench-protocol 2000000      # GCC 16 -O3, Ryzen AI 9 HX 370 laptop, Windows
2000000 messages (75.2 MB), 1346720 trades, 95708368 bytes out
decode only:               3.1 ns/msg  (318.5 M msg/s)
decode + match + encode:  82.2 ns/msg  (12.2 M msg/s)
```

A quick single-run number; proper Google Benchmark runs with latency percentiles come in the
`bench` phase.

## Threaded pipeline

`matchbook::SpscRing<T, N>` is a bounded, lock-free single-producer / single-consumer queue
(power-of-two capacity, ever-growing counters so "full" needs no wasted slot, acquire/release
only). The producer's and consumer's counters sit on separate padded cache lines, and each side
keeps a private copy of the other's counter, re-reading the shared atomic only when the queue
*looks* full or empty.

`matchbook::Pipeline` wires three threads together with two rings:

```
gateway (decode wire bytes) --commands--> engine (match) --events--> publisher (encode)
```

Each stage owns its state (only the engine thread touches the book, only the publisher touches
the output), so nothing is locked. Threads can be pinned to cores, and the gateway can pace
its input at a fixed rate.

- Two-thread stress tests push millions of items through both ring variants and check order,
  sums and that 24-byte structs never arrive torn; the pipeline's encoded output is checked
  byte-for-byte against the single-threaded `Session` on 50,000 random orders, cancels and
  modifies, pinned and unpinned.

```text
$ matchbook bench-queue          # Ryzen AI 9 HX 370 laptop, Windows 11, GCC 16 -O3
spsc ring, 2 threads:  plain 227.7 M ops/s, padded + cached indices 281.0 M ops/s (1.2x)
single thread (Session): 11.7 M msg/s
3-thread pipeline:       7.2 M msg/s, 2697459 events
unpinned paced 1M msg/s: p50 247 ns, p99 11073 ns, p99.9 9162744 ns
pinned   paced 1M msg/s: p50 300 ns, p99 2049815 ns, p99.9 11163685 ns
pinned   paced 3M msg/s: p50 280 ns, p99 7925999 ns, p99.9 12610530 ns
pinned   paced 5M msg/s: p50 370 ns, p99 12264333 ns, p99.9 13624285 ns
```

What the numbers say:

- **A message crosses two thread hops in ~250-370 ns (median)**, decode to published event,
  timed with the TSC (Windows' `steady_clock` has 100 ns resolution).
- **The pipeline is slower than one thread** (7.2 vs 11.7 M msg/s). Each stage does only
  30-80 ns of work, and every message costs two cache-line transfers between cores; at that
  granularity the handoff costs more than it saves. The point of the split is isolation
  (decoding and fan-out can't stall matching), not throughput; batching handoffs would be the
  next step if it had to be faster.
- **Padding + cached indices: 1.2x** on the raw ring. Real, but smaller than the folklore,
  because with a deep ring the two threads are rarely on the same slot's line anyway.
- **Unpaced latency is meaningless**: the gateway outruns the engine, the rings fill, and
  "latency" becomes queue depth (8 ms). Latency has to be measured at an offered load the
  system can sustain, hence the paced runs.
- **The tails are the OS, not the code.** A single scheduler preemption of a spinning thread
  (Windows' quantum is ~15 ms) shows up as millisecond p99 / p99.9 values; pinning keeps the
  median flat up to 5M msg/s but can't stop preemption. Microsecond tails need isolated cores
  (Linux `isolcpus` + real-time priority), which a laptop doesn't give you.

## Replay

`matchbook replay` pushes a stream of wire messages through a `Session` one message at a time and times each one with the TSC (decode + match + encode the events).

**Synthetic flow** (`replay::synthetic_flow`): a merged Poisson process of limit orders (60%), marketable orders (10%) and cancels (30%). Limit prices sit 1 + Geometric ticks from a slowly drifting mid, sizes are geometric; cancels pick a random earlier order, which may already be filled (the engine then rejects it, as a real one would). Same seed, same bytes.

**Real Coinbase prints** (`replay::tape_flow`): `tools/export_trades.py` dumps the trades QuantLab's Go gateway recorded into Redis (3,827 BTC-USD / ETH-USD prints from a 10-minute live run). Each print becomes an IOC order of its real size and side; before it, synthetic market makers pull their ladder and re-quote 5 levels per side, one tick apart, at the median print size, around the previous print.

```text
$ matchbook replay --synthetic 2000000
synthetic flow: 1199702 limits, 200061 markets, 600236 cancels
synthetic: 1999999 messages in 0.179 s (11.2 M msg/s incl. timing), 1269117 trades, 25383 resting at the end
per message (decode + match + encode): p50 50 ns, p99 370 ns, p99.9 1532 ns

$ matchbook replay --trades data/coinbase_trades_sample.csv --symbol BTC-USD
BTC-USD: 2744 prints (0 bad rows skipped) -> 57614 messages
per message (decode + match + encode): p50 30 ns, p99 190 ns, p99.9 1322 ns
tape: 2744 prints, 5.6% of their volume filled against the synthetic makers, volume-weighted slippage vs the real prints -0.03 bps
      the largest 1% of prints carry 25.3% of the volume; the median print is 31833 lots, the largest 58080543
```

- **A message costs ~30-50 ns at the median and under 0.4 µs at p99.** The p99.9 (~1.5 µs) is mostly the sweeps: a marketable order that walks several levels emits one trade event per resting order it hits.
- **Median-sized quotes absorb almost none of the real volume** (5.6% on BTC, 13.3% on ETH): print sizes are heavy-tailed, the largest BTC print is ~1,800x the median and the top 1% of prints carry a quarter of the volume. A realistic liquidity model has to size quotes by volume, not by count; for the latency numbers it doesn't matter.
- Tests: the synthetic event mix matches its rates within 1%, limit prices stay on their side of the mid, the book is never crossed after any message of either flow, the CSV loader skips malformed rows, and every taker fill in the tape replay is against a maker.

## Benchmarks

`bench/` is a Google Benchmark suite (`./build/release/bench/matchbook_bench`):

| Benchmark | Time | What |
|---|---:|---|
| `BM_AddCancel` | 49 ns per pair | book only: add near the top of a 200-order book, then cancel it |
| `BM_EnginePassiveLimitCancel` | 39 ns per pair | same through the engine, events included |
| `BM_MarketSweep/1` | 30 ns per sweep | market order filling one resting order |
| `BM_MarketSweep/4` | 107 ns | ... walking 4 price levels |
| `BM_MarketSweep/16` | 628 ns | ... 16 levels |
| `BM_MarketSweep/64` | 1.76 µs | ... 64 levels (~27 ns per level and trade event) |
| `BM_ProtocolDecode` | 165 M msg/s | zero-copy decode, 5.1 GiB/s |
| `BM_SessionSyntheticFlow/100000` | 7.1 M msg/s | decode + match + encode on synthetic Poisson flow |
| `BM_SpscRing/padded` | 212 M items/s | producer + consumer thread through the ring |
| `BM_SpscRing/plain` | 134 M items/s | same ring without padding / cached indices (1.6x slower) |

The first version of the engine benchmarks reported ~210 ns for a single passive order and a
one-level sweep. That was mostly `state.PauseTiming()` / `ResumeTiming()`, which cost about
200 ns per call here: more than the work being measured. The passive order is now timed together
with its cancel, and sweeps rebuild the book for 1,000 of them per pause.

`matchbook replay` also prints a latency histogram (power-of-two buckets) because percentiles hide
the shape:

```text
$ matchbook replay --synthetic 2000000
per message (decode + match + encode): p50 50 ns, p99 480 ns, p99.9 1562 ns
           8 - 16         ns     278757  13.938%  #######
          16 - 32         ns     337524  16.876%  ########
          32 - 64         ns     698268  34.913%  #################
          64 - 128        ns     237786  11.889%  ######
         128 - 256        ns     353238  17.662%  #########
         256 - 512        ns      71461   3.573%  ##
         512 - 1024       ns      13358   0.668%
        1024 - 2048       ns       1975   0.099%
        ...  (a handful of messages at 10 µs - 10 ms: OS preemption)
```

Two humps: cancels and passive orders at 8-64 ns, and messages that trade (and encode trade
events) at 128-256 ns. Everything past ~4 µs is a few dozen messages out of 2 million that were
interrupted by the OS.

## Python

`pip install .` builds the engine as a pybind11 module (scikit-build-core drives CMake with `MATCHBOOK_BUILD_PYTHON=ON`):

```python
from matchbook import Engine, Side, TimeInForce, market_order_cost

e = Engine()
e.submit(1, Side.SELL, price=101, qty=5)        # [Event(Rested, id=1, price=101, qty=5)]
e.submit(2, Side.BUY, price=101, qty=3)         # [Event(Trade, id=2, maker_id=1, price=101, qty=3)]
e.depth(Side.SELL)                              # [(101, 2, 1)]  price, qty, orders

# numpy in, numpy out: one call for a whole batch (GIL released while matching)
trades = Engine().submit_many(ids, sides, prices, qtys, types, tifs)

# what a backtester needs: the cost of walking a book snapshot
market_order_cost(bids, asks, Side.BUY, qty=500)  # FillCost(filled, avg_price, slippage_bps, levels)
```

```text
$ python python/examples/bench_bindings.py          # Linux container, Python 3.12
200,000 orders, 161,915 trades
  one call per order:     984 ns/order
  submit_many:             68 ns/order  (14x)
```

The engine itself takes ~50 ns per order; one Python call per order costs ~1 µs, almost all of it
crossing the Python / C++ boundary and building `Event` objects. The batch API keeps a backtest
at engine speed. `market_order_cost` replaces a flat slippage assumption with the actual cost of
walking a book: on 10 levels of 100 lots one tick apart, a 1,000-lot market buy pays 4.5 bps.

Tests (`pytest`, 6 cases): matching and events through the bindings, FOK / IOC / cancel / modify /
rejects, `submit_many` producing exactly the trades of one call per order on 5,000 random orders,
input validation (mismatched lengths, out-of-range enum bytes are rejected rather than cast), the
batch speed-up, and `market_order_cost` against hand-computed fills.

## Build

Requirements: CMake 3.24+, Ninja, and GCC 12+ or Clang 16+.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/matchbook --info
```

The `asan` preset builds with AddressSanitizer and UBSan (Linux / macOS; MinGW has no ASan runtime).

## Tests

64 GoogleTest cases (book, matching, protocol, session, SPSC queue, replay) including randomized
differential tests against a naive reference book, plus the 6 pytest cases above.

`scripts/check_linux.sh` runs the heavier checks in one go (I run it in a plain Ubuntu container
from Windows):

```bash
docker run --rm -v "$PWD:/src" -w /src ubuntu:24.04 bash scripts/check_linux.sh 60
```

1. the full suite under ASan + UBSan (GCC, Debug)
2. line coverage with gcov / gcovr (`-DMATCHBOOK_COVERAGE=ON`)
3. 60 s of libFuzzer on `fuzz/fuzz_session.cpp` (Clang, `-DMATCHBOOK_FUZZ=ON`): random bytes,
   cut into random-sized chunks, go through `Session` into a real engine, and after every chunk the
   book invariants are checked and the book must not be crossed

| check | result |
|---|---|
| ASan + UBSan | full suite passes, no reports |
| line coverage (`src/` + `include/`) | 95% (738 / 770 lines) |
| libFuzzer, 60 s | 1.03M inputs (~16.8k/s), 372 new coverage units, no crashes |

Uncovered lines are mostly the `check_invariants()` failure paths (they throw only if the book is
already corrupt), the "level table full" reject, the busy-wait branch when a queue is empty, and
outbound message types sent back into `Session`, which are skipped.

## License

MIT
