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

## Build

Requirements: CMake 3.24+, Ninja, and GCC 12+ or Clang 16+.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/matchbook --info
```

The `asan` preset builds with AddressSanitizer and UBSan (Linux / macOS; MinGW has no ASan runtime).

## License

MIT
