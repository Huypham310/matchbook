"""Dump the trades QuantLab's Go gateway recorded in Redis to CSV, for `matchbook replay`.

    python tools/export_trades.py --out data/coinbase_trades.csv
    python tools/export_trades.py --redis redis://localhost:6379/0 --stream trades

Columns: ts_ms,symbol,price,size,side (side = taker side). Needs `pip install redis`.
"""

import argparse
import csv
from pathlib import Path

import redis


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--redis", default="redis://localhost:6379/0")
    p.add_argument("--stream", default="trades")
    p.add_argument("--out", default="data/coinbase_trades.csv")
    args = p.parse_args()

    r = redis.Redis.from_url(args.redis, decode_responses=True)
    rows, last = [], "-"
    while True:
        batch = r.xrange(args.stream, min=last, max="+", count=5000)
        if last != "-":
            batch = batch[1:]  # xrange is inclusive: drop the entry we already have
        if not batch:
            break
        for _, f in batch:
            rows.append((int(f["ts_ms"]), f["symbol"], f["price"], f["size"], f["side"]))
        last = batch[-1][0]
    rows.sort()
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["ts_ms", "symbol", "price", "size", "side"])
        w.writerows(rows)
    print(f"{len(rows)} trades -> {out}")


if __name__ == "__main__":
    main()
