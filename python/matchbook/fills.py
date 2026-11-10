"""Execution cost of a market order against a book snapshot, the piece a backtester needs.

QuantLab's backtester charges a flat slippage in basis points; with an order book snapshot
this gives the real cost of walking the book for a given size instead.
"""

from __future__ import annotations

from dataclasses import dataclass

from matchbook._matchbook import Engine, EventType, OrderType, Side, TimeInForce


@dataclass(frozen=True)
class FillCost:
    filled: int  # quantity actually filled (less than asked if the book ran out)
    avg_price: float  # volume-weighted fill price, nan if nothing filled
    slippage_bps: float  # avg price vs the best price, positive = worse for the taker
    levels: int  # price levels touched


def market_order_cost(
    bids: list[tuple[int, int]], asks: list[tuple[int, int]], side: Side, qty: int
) -> FillCost:
    """Fill a market order of `qty` against `bids` / `asks` given as (price, qty) levels.

    Prices and quantities are integers (ticks and lots), as in the engine. Uses a fresh engine,
    so the snapshot isn't modified.
    """
    if qty <= 0:
        raise ValueError("qty must be positive")
    book = asks if side == Side.BUY else bids
    if not book:
        return FillCost(0, float("nan"), float("nan"), 0)
    e = Engine(max_orders=len(bids) + len(asks) + 16, max_levels=len(bids) + len(asks) + 16)
    oid = 1
    for px, q in bids:
        e.submit(oid, Side.BUY, px, q)
        oid += 1
    for px, q in asks:
        e.submit(oid, Side.SELL, px, q)
        oid += 1
    best = min(p for p, _ in asks) if side == Side.BUY else max(p for p, _ in bids)
    events = e.submit(oid, side, 0, qty, OrderType.MARKET, TimeInForce.IOC)
    trades = [ev for ev in events if ev.type == EventType.TRADE]
    filled = sum(t.qty for t in trades)
    if filled == 0:
        return FillCost(0, float("nan"), float("nan"), 0)
    avg = sum(t.price * t.qty for t in trades) / filled
    sign = 1.0 if side == Side.BUY else -1.0
    return FillCost(
        filled=filled,
        avg_price=avg,
        slippage_bps=sign * (avg / best - 1.0) * 1e4,
        levels=len({t.price for t in trades}),
    )
