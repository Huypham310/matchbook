"""Per-order cost through the Python bindings: one call per order vs one batch call.

    python python/examples/bench_bindings.py
"""

import time

import numpy as np

from matchbook import Engine, Side, market_order_cost


def flow(n: int, seed: int = 0):
    rng = np.random.default_rng(seed)
    ids = np.arange(1, n + 1, dtype=np.uint64)
    sides = rng.integers(0, 2, n).astype(np.int8)
    prices = (1000 + rng.integers(-5, 6, n)).astype(np.int64)
    qtys = rng.integers(1, 20, n).astype(np.int64)
    types = (rng.random(n) < 0.05).astype(np.int8)
    return ids, sides, prices, qtys, types, np.zeros(n, dtype=np.int8)


def main() -> None:
    n = 200_000
    cols = flow(n)
    e1, e2 = Engine(), Engine()

    t0 = time.perf_counter()
    for i, s, p, q in zip(*cols[:4], strict=True):
        e1.submit(int(i), Side(int(s)), int(p), int(q))
    loop = time.perf_counter() - t0

    t0 = time.perf_counter()
    out = e2.submit_many(*cols)
    batch = time.perf_counter() - t0

    print(f"{n:,} orders, {len(out['qty']):,} trades")
    print(f"  one call per order: {loop / n * 1e9:7.0f} ns/order")
    print(f"  submit_many:        {batch / n * 1e9:7.0f} ns/order  ({loop / batch:.0f}x)")

    # slippage curve of a market buy against a 10-level book, 100 lots per level, 1-tick steps
    asks = [(10_000 + k, 100) for k in range(10)]
    bids = [(9_999 - k, 100) for k in range(10)]
    for qty in (50, 200, 500, 1000):
        c = market_order_cost(bids, asks, Side.BUY, qty)
        print(f"  buy {qty:5d}: avg {c.avg_price:9.2f}, {c.levels} levels, {c.slippage_bps:5.2f} bps")


if __name__ == "__main__":
    main()
