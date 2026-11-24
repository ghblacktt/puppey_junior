#!/usr/bin/env python3
"""Анализ sweep результатов и поиск прибыльных комбинаций."""

import re
import sys
from dataclasses import dataclass
from pathlib import Path

@dataclass
class Result:
    symbol: str
    min_hs: float
    fills: int
    net_pnl: float
    hit_pct: float = 0.0
    med_30s: float = 0.0

def parse_results(filename: str) -> list[Result]:
    results = []
    current = None

    with open(filename) as f:
        for line in f:
            # === SOLUSDT / hs=8 ===
            m = re.match(r'=== (\w+) / hs=(\d+) ===', line)
            if m:
                if current and current.fills > 0:
                    results.append(current)
                current = Result(symbol=m.group(1), min_hs=float(m.group(2)),
                                 fills=0, net_pnl=0.0)
                continue

            if not current:
                continue

            m = re.match(r'Fills:\s+(\d+)', line)
            if m:
                current.fills = int(m.group(1))

            m = re.match(r'Net:\s+\$([-\d.]+)', line)
            if m:
                current.net_pnl = float(m.group(1))

            m = re.match(r'hit_pct:\s+([\d.]+)%', line)
            if m:
                current.hit_pct = float(m.group(1))

            m = re.match(r'med_30s:\s+([\d.-]+)', line)
            if m:
                current.med_30s = float(m.group(1))

    if current and current.fills > 0:
        results.append(current)

    return results

def main():
    results = parse_results(sys.argv[1])

    # Фильтруем только статистически значимые результаты
    significant = [r for r in results if r.fills >= 20]

    print(f"\n{'='*80}")
    print(f"SWEEP ANALYSIS ({len(results)} total, {len(significant)} significant)")
    print(f"{'='*80}\n")

    # ТОП-5 по Net PnL
    by_pnl = sorted(significant, key=lambda r: r.net_pnl, reverse=True)
    print("🏆 TOP-5 PROFITABLE (min 20 fills):")
    print(f"{'Symbol':<12} {'min_hs':>7} {'Fills':>7} {'Net PnL':>10} {'Hit%':>7} {'med_30s':>8}")
    print("-" * 60)
    for r in by_pnl[:5]:
        print(f"{r.symbol:<12} {r.min_hs:>7.0f} {r.fills:>7} "
              f"${r.net_pnl:>8.4f} {r.hit_pct:>6.1f}% {r.med_30s:>7.2f}")

    # ТОП-5 убыточных
    print(f"\n💀 TOP-5 LOSERS:")
    for r in by_pnl[-5:]:
        print(f"{r.symbol:<12} {r.min_hs:>7.0f} {r.fills:>7} "
              f"${r.net_pnl:>8.4f} {r.hit_pct:>6.1f}% {r.med_30s:>7.2f}")

    # Best min_hs per symbol
    print(f"\n📊 BEST min_hs PER SYMBOL:")
    from collections import defaultdict
    by_sym = defaultdict(list)
    for r in significant:
        by_sym[r.symbol].append(r)

    for sym, rs in sorted(by_sym.items()):
        best = max(rs, key=lambda r: r.net_pnl)
        avg_pnl = sum(r.net_pnl for r in rs) / len(rs)
        print(f"  {sym:<12}: best_hs={best.min_hs:.0f} "
              f"(net=${best.net_pnl:.4f}, fills={best.fills}, "
              f"avg=${avg_pnl:.4f})")

    # Profit Factor estimation
    print(f"\n🎯 PROFITABILITY THRESHOLDS:")
    profitable = [r for r in significant if r.net_pnl > 0]
    print(f"  Profitable configurations: {len(profitable)}/{len(significant)} "
          f"({100*len(profitable)/max(1,len(significant)):.1f}%)")
    if profitable:
        avg_profit = sum(r.net_pnl for r in profitable) / len(profitable)
        print(f"  Average profit when profitable: ${avg_profit:.4f}")

if __name__ == "__main__":
    main()