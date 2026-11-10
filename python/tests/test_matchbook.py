import math
import time

import numpy as np
import pytest

from matchbook import Engine, EventType, OrderType, RejectReason, Side, TimeInForce, market_order_cost


def test_rest_then_trade_at_the_maker_price():
    e = Engine()
    (rested,) = e.submit(1, Side.SELL, 101, 5)
    assert rested.type == EventType.RESTED and rested.qty == 5
    (trade, rest2) = e.submit(2, Side.BUY, 102, 8)
    assert trade.type == EventType.TRADE
    assert (trade.id, trade.maker_id, trade.price, trade.qty) == (2, 1, 101, 5)
    assert rest2.type == EventType.RESTED and rest2.qty == 3  # remainder rests at its limit
    assert e.best_bid == 102 and e.best_ask is None
    assert e.trade_count == 1 and e.order_count == 1


def test_fok_ioc_cancel_modify_and_rejects():
    e = Engine()
    e.submit(1, Side.SELL, 100, 5)
    (fok,) = e.submit(2, Side.BUY, 100, 6, tif=TimeInForce.FOK)
    assert fok.type == EventType.CANCELLED  # not enough: no trace
    assert e.depth(Side.SELL) == [(100, 5, 1)]
    ioc = e.submit(3, Side.BUY, 100, 6, tif=TimeInForce.IOC)
    assert [ev.type for ev in ioc] == [EventType.TRADE, EventType.CANCELLED]

    e.submit(4, Side.BUY, 90, 2)
    assert e.modify(4, 91, 2)[0].type == EventType.RESTED
    assert e.cancel(4)[0].type == EventType.CANCELLED
    (rej,) = e.cancel(4)
    assert rej.type == EventType.REJECTED and rej.reason == RejectReason.UNKNOWN_ORDER
    e.submit(5, Side.BUY, 90, 1)
    (dup,) = e.submit(5, Side.BUY, 90, 1)
    assert dup.reason == RejectReason.DUPLICATE_ID
    assert "Trade" in repr(ioc[0])


def _flow(n, seed=0):
    rng = np.random.default_rng(seed)
    ids = np.arange(1, n + 1, dtype=np.uint64)
    sides = rng.integers(0, 2, n).astype(np.int8)
    prices = (1000 + rng.integers(-5, 6, n)).astype(np.int64)
    qtys = rng.integers(1, 20, n).astype(np.int64)
    types = (rng.random(n) < 0.05).astype(np.int8)  # 5% market orders
    tifs = np.zeros(n, dtype=np.int8)
    return ids, sides, prices, qtys, types, tifs


def test_batch_api_gives_the_same_trades_as_one_call_per_order():
    cols = _flow(5_000)
    batch = Engine().submit_many(*cols)

    one = Engine()
    trades = []
    for i, s, p, q, t, f in zip(*cols, strict=True):
        evs = one.submit(int(i), Side(int(s)), int(p), int(q), OrderType(int(t)), TimeInForce(int(f)))
        trades += [(ev.id, ev.maker_id, ev.price, ev.qty) for ev in evs if ev.type == EventType.TRADE]
    got = list(zip(batch["taker_id"], batch["maker_id"], batch["price"], batch["qty"], strict=True))
    assert got == trades
    assert len(trades) > 1_000 and batch["rejected"] == 0


def test_batch_api_validates_its_input():
    ids, sides, prices, qtys, types, tifs = _flow(10)
    with pytest.raises(ValueError):
        Engine().submit_many(ids[:5], sides, prices, qtys, types, tifs)
    sides[3] = 7  # not a side: refused, not cast to an invalid enum
    assert Engine().submit_many(ids, sides, prices, qtys, types, tifs)["rejected"] >= 1


def test_batch_is_much_faster_than_one_call_per_order():
    cols = _flow(50_000, seed=1)
    # build both engines outside the timing: the constructor preallocates the order pool
    # (~50 MB for the default 1M orders), which alone takes longer than the whole batch
    batch_engine, one = Engine(), Engine()
    t0 = time.perf_counter()
    batch_engine.submit_many(*cols)
    batch = time.perf_counter() - t0
    t0 = time.perf_counter()
    for i, s, p, q in zip(*cols[:4], strict=True):
        one.submit(int(i), Side(int(s)), int(p), int(q))
    loop = time.perf_counter() - t0
    assert batch * 5 < loop  # the per-call cost is the Python boundary, not the matching


def test_market_order_cost_walks_the_book():
    asks = [(100, 10), (101, 10), (103, 10)]
    bids = [(99, 10)]
    small = market_order_cost(bids, asks, Side.BUY, 5)
    assert small.filled == 5 and small.avg_price == 100 and small.slippage_bps == 0
    big = market_order_cost(bids, asks, Side.BUY, 25)
    assert big.levels == 3
    assert big.avg_price == pytest.approx((100 * 10 + 101 * 10 + 103 * 5) / 25)
    assert big.slippage_bps == pytest.approx((big.avg_price / 100 - 1) * 1e4)
    too_big = market_order_cost(bids, asks, Side.BUY, 100)
    assert too_big.filled == 30  # the book ran out
    sell = market_order_cost(bids, asks, Side.SELL, 4)
    assert sell.avg_price == 99 and sell.slippage_bps == 0
    assert math.isnan(market_order_cost([], asks, Side.SELL, 1).avg_price)
    with pytest.raises(ValueError):
        market_order_cost(bids, asks, Side.BUY, 0)
