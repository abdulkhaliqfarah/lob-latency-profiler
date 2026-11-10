#!/usr/bin/env python3
"""Differential test: a naive Python model of the matching rules vs the
profiled C++ engine (tools/run_stream).

The model is deliberately dumb -- plain dicts and linear scans, written
straight from the matching rules: strict price-time priority, execution at
the maker's price, partial fills rest, market-order remainders cancelled.
It generates a random operation stream, runs it through its own model and
through the C++ binary (which itself cross-checks a bare run against a
profiled run), and diffs the full trade log, every final level quantity,
and the live order count.

Usage: python3 scripts/model.py <path-to-run_stream-binary> [num_ops] [seed]
Exit code 0 = exact agreement.
"""

import random
import subprocess
import sys


class Model:
    def __init__(self):
        self.orders = {}   # id -> [side, price, qty] for resting orders
        self.levels = {}   # (side, price) -> list of ids, FIFO order
        self.trade_seq = 0
        self.trades = []   # "T id taker maker price qty"

    def _rest(self, oid, side, price, qty):
        self.orders[oid] = [side, price, qty]
        self.levels.setdefault((side, price), []).append(oid)

    def _remove(self, oid):
        side, price, _ = self.orders.pop(oid)
        queue = self.levels[(side, price)]
        queue.remove(oid)
        if not queue:
            del self.levels[(side, price)]

    def add(self, oid, side, otype, price, qty):
        remaining = qty
        opp = "S" if side == "B" else "B"
        while remaining > 0:
            prices = [p for (s, p) in self.levels if s == opp]
            if not prices:
                break
            best = max(prices) if opp == "B" else min(prices)
            if otype == "L":
                if side == "B" and best > price:
                    break
                if side == "S" and best < price:
                    break
            queue = self.levels[(opp, best)]
            while remaining > 0 and queue:
                maker_id = queue[0]
                maker = self.orders[maker_id]
                fill = min(remaining, maker[2])
                self.trade_seq += 1
                self.trades.append(
                    f"T {self.trade_seq} {oid} {maker_id} {best} {fill}")
                remaining -= fill
                maker[2] -= fill
                if maker[2] == 0:
                    self._remove(maker_id)
        if remaining > 0 and otype == "L":
            self._rest(oid, side, price, remaining)

    def cancel(self, oid):
        if oid in self.orders:
            self._remove(oid)

    def quantity_at(self, side, price):
        queue = self.levels.get((side, price), [])
        return sum(self.orders[i][2] for i in queue)


def main():
    if len(sys.argv) < 2:
        print("usage: model.py <run_stream-binary> [num_ops] [seed]")
        return 2
    binary = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261110
    rng = random.Random(seed)
    mid = 100000

    model = Model()
    lines = []
    live = []
    next_id = 1
    for _ in range(n):
        r = rng.random()
        if r < 0.55 or not live:
            buy = rng.random() < 0.5
            crossing = rng.random() < 0.25
            if crossing:
                price = mid + rng.randint(1, 5) if buy else mid - rng.randint(1, 5)
            else:
                price = mid - rng.randint(5, 24) if buy else mid + rng.randint(5, 24)
            otype = "M" if rng.random() < 0.05 else "L"
            qty = rng.randint(1, 100)
            lines.append(f"A {next_id} {'B' if buy else 'S'} {otype} {price} {qty}")
            model.add(next_id, "B" if buy else "S", otype, price, qty)
            if otype == "L":
                live.append(next_id)
            next_id += 1
        else:
            idx = rng.randrange(len(live))
            oid = live.pop(idx)
            lines.append(f"C {oid}")
            model.cancel(oid)

    # Queries: every level the model thinks exists, plus absent prices.
    query_keys = sorted(model.levels.keys())
    for absent in [(("B"), mid - 500), (("S"), mid + 500)]:
        query_keys.append(absent)
    for side, price in query_keys:
        lines.append(f"Q {side} {price}")
    lines.insert(len(lines) - len(query_keys), "END")

    proc = subprocess.run([binary], input="\n".join(lines) + "\n",
                          capture_output=True, text=True)
    if proc.returncode != 0:
        print("run_stream failed:", proc.stderr)
        return 1
    out = proc.stdout.strip().split("\n")

    expected = list(model.trades)
    for side, price in query_keys:
        expected.append(f"Q {side} {price} {model.quantity_at(side, price)}")
    expected.append(f"LIVE {len(model.orders)}")

    if out == expected:
        n_trades = sum(1 for l in expected if l.startswith("T "))
        print(f"AGREE: {n} ops, {n_trades} trades, "
              f"{len(query_keys)} level queries, live={len(model.orders)}")
        return 0
    for i, (a, b) in enumerate(zip(out, expected)):
        if a != b:
            print(f"first divergence at output line {i}:\n  cpp:    {a}\n  python: {b}")
            break
    print(f"line counts: cpp={len(out)} python={len(expected)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
