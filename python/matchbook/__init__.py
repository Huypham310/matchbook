"""Python bindings for the MatchBook C++ matching engine.

    >>> from matchbook import Engine, Side
    >>> e = Engine()
    >>> e.submit(1, Side.SELL, price=101, qty=5)
    [Event(Rested, id=1, price=101, qty=5)]
    >>> e.submit(2, Side.BUY, price=101, qty=3)
    [Event(Trade, id=2, maker_id=1, price=101, qty=3)]
"""

from matchbook._matchbook import (
    Engine,
    Event,
    EventType,
    OrderType,
    RejectReason,
    Side,
    TimeInForce,
)
from matchbook.fills import market_order_cost

__all__ = [
    "Engine",
    "Event",
    "EventType",
    "OrderType",
    "RejectReason",
    "Side",
    "TimeInForce",
    "market_order_cost",
]
