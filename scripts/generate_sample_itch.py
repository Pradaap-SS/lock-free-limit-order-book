#!/usr/bin/env python3
"""
Generate a synthetic NASDAQ ITCH 5.0 binary file for testing and benchmarking.

Produces a realistic mix of messages for one or more symbols:
  - Stock Directory  ('R') — one per symbol at the start
  - Add Order        ('A') — ~60 % of trading messages
  - Order Delete     ('D') — ~25 % of trading messages
  - Order Executed   ('E') — ~10 % of trading messages
  - Order Replace    ('U') —  ~5 % of trading messages

Usage:
  python3 scripts/generate_sample_itch.py
  python3 scripts/generate_sample_itch.py --out data/big.itch --messages 5000000
  python3 scripts/generate_sample_itch.py --symbols AAPL MSFT --messages 500000
"""

import argparse
import struct
import random
import os
import time
from collections import defaultdict


# ── Binary helpers (big-endian ITCH wire format) ──────────────────────────────

def be16(v: int) -> bytes: return struct.pack('>H', v & 0xFFFF)
def be32(v: int) -> bytes: return struct.pack('>I', v & 0xFFFFFFFF)
def be48(v: int) -> bytes: return struct.pack('>Q', v)[2:]
def be64(v: int) -> bytes: return struct.pack('>Q', v & 0xFFFFFFFFFFFFFFFF)

def stock_field(sym: str) -> bytes:
    """8-byte ASCII symbol, right-padded with spaces."""
    return sym[:8].ljust(8).encode('ascii')

def frame(payload: bytes) -> bytes:
    """Prepend 2-byte big-endian length header (length excludes the header)."""
    return be16(len(payload)) + payload


# ── ITCH message constructors ─────────────────────────────────────────────────

def msg_stock_directory(locate: int, ts_ns: int, sym: str) -> bytes:
    return frame(
        b'R' + be16(locate) + be16(0) + be48(ts_ns) +
        stock_field(sym) + b'Q' + b'N' + be32(100) +
        b'N' + b'N' + b'N' + be32(0) + b'N'
    )

def msg_add_order(locate: int, ts_ns: int, ref: int,
                  side: str, shares: int, sym: str, price: int) -> bytes:
    return frame(
        b'A' + be16(locate) + be16(0) + be48(ts_ns) +
        be64(ref) + side.encode() + be32(shares) +
        stock_field(sym) + be32(price)
    )

def msg_order_delete(locate: int, ts_ns: int, ref: int) -> bytes:
    return frame(b'D' + be16(locate) + be16(0) + be48(ts_ns) + be64(ref))

def msg_order_executed(locate: int, ts_ns: int,
                       ref: int, exec_shares: int, match: int) -> bytes:
    return frame(
        b'E' + be16(locate) + be16(0) + be48(ts_ns) +
        be64(ref) + be32(exec_shares) + be64(match)
    )

def msg_order_replace(locate: int, ts_ns: int,
                      orig_ref: int, new_ref: int,
                      shares: int, price: int) -> bytes:
    return frame(
        b'U' + be16(locate) + be16(0) + be48(ts_ns) +
        be64(orig_ref) + be64(new_ref) + be32(shares) + be32(price)
    )


# ── Generator ─────────────────────────────────────────────────────────────────

class ItchGenerator:
    """Stateful generator that maintains a realistic resting order set."""

    BASE_PRICES = {
        "AAPL": 150.00, "MSFT": 280.00, "TSLA": 220.00,
        "AMZN": 130.00, "GOOG": 140.00, "META": 320.00,
        "NVDA": 450.00, "SPY":  420.00, "QQQ":  380.00,
    }

    def __init__(self, symbols: list[str]):
        self.symbols      = symbols
        self.locates      = {sym: i + 1 for i, sym in enumerate(symbols)}
        self.base_itch    = {sym: int(self.BASE_PRICES.get(sym, 100.0) * 10_000)
                             for sym in symbols}
        self._ref         = 1
        self._match       = 1
        self.resting: dict[str, list[int]] = defaultdict(list)

    def _next_ref(self) -> int:
        r = self._ref; self._ref += 1; return r

    def generate(self, n_messages: int, rng: random.Random) -> bytes:
        out = bytearray()

        # Stock directory preamble
        ts = 34_200_000_000_000  # 09:30:00.000 in nanoseconds
        for sym in self.symbols:
            out += msg_stock_directory(self.locates[sym], ts, sym)
            ts  += 1_000_000

        # Trading messages
        SHARES_CHOICES = (100, 100, 200, 200, 300, 500, 1000)
        for i in range(n_messages):
            ts  += rng.randint(200, 5_000)
            sym  = rng.choice(self.symbols)
            loc  = self.locates[sym]
            base = self.base_itch[sym]
            rest = self.resting[sym]

            r = rng.random()

            if r < 0.60 or not rest:
                # Add Order
                side   = 'B' if rng.random() < 0.5 else 'S'
                spread = rng.randint(1, 15) * 100
                price  = (base - spread) if side == 'B' else (base + spread)
                price  = max(100, price)
                shares = rng.choice(SHARES_CHOICES)
                ref    = self._next_ref()
                out   += msg_add_order(loc, ts, ref, side, shares, sym, price)
                rest.append(ref)

            elif r < 0.85 and rest:
                # Delete random resting order
                idx  = rng.randrange(len(rest))
                ref  = rest.pop(idx)
                out += msg_order_delete(loc, ts, ref)

            elif r < 0.95 and rest:
                # Partial execution
                ref        = rest[rng.randrange(len(rest))]
                exec_qty   = rng.choice((50, 100, 200))
                out       += msg_order_executed(loc, ts, ref, exec_qty, self._match)
                self._match += 1

            elif rest:
                # Replace
                idx       = rng.randrange(len(rest))
                orig_ref  = rest.pop(idx)
                new_ref   = self._next_ref()
                new_price = max(100, base + rng.randint(-30, 30) * 100)
                shares    = rng.choice(SHARES_CHOICES)
                out      += msg_order_replace(loc, ts, orig_ref, new_ref, shares, new_price)
                rest.append(new_ref)

        return bytes(out)


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Synthetic ITCH 5.0 file generator")
    ap.add_argument("--out",      default="data/sample.itch",
                    help="Output file (default: data/sample.itch)")
    ap.add_argument("--messages", type=int, default=1_000_000,
                    help="Trading messages to generate (default: 1,000,000)")
    ap.add_argument("--symbols",  nargs="+", default=["AAPL", "MSFT", "TSLA"],
                    help="Symbols to include (default: AAPL MSFT TSLA)")
    ap.add_argument("--seed",     type=int, default=42,
                    help="Random seed (default: 42)")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    gen = ItchGenerator(args.symbols)
    rng = random.Random(args.seed)

    print(f"Generating {args.messages:,} messages for {args.symbols}...")
    t0   = time.monotonic()
    data = gen.generate(args.messages, rng)
    t1   = time.monotonic()

    with open(args.out, "wb") as f:
        f.write(data)

    elapsed = t1 - t0
    print(f"Written:  {args.out}")
    print(f"  Size:     {len(data)/1e6:.2f} MB  ({len(data):,} bytes)")
    print(f"  Elapsed:  {elapsed:.2f}s  ({args.messages/elapsed:,.0f} msg/s)")
    print()
    print("Replay commands:")
    for sym in args.symbols:
        print(f"  ./build/replay {args.out} {sym}")


if __name__ == "__main__":
    main()
