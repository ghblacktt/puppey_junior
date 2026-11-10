import argparse
import numpy as np
import pandas as pd
from clickhouse_driver import Client
from tick.hawkes import HawkesExpKern

def load_events(symbol: str, hours: int):
    cli = Client(host='localhost', user='robot', password='robot_pass',
                 database='market_data')
    q = """
        SELECT ts_local, venue, sz, aggressor, bbo_bid, bbo_ask
        FROM raw_trades
        WHERE symbol = %(sym)s
          AND venue IN ('bybit','binance')
          AND ts_local >= now() * 1000 - %(h)s * 3600 * 1000
        ORDER BY ts_local
    """
    df = cli.query_dataframe(q, {'sym': symbol, 'h': hours})
    if df.empty:
        raise SystemExit("no data")
    df['mid'] = 0.5 * (df['bbo_bid'] + df['bbo_ask'])
    df['sz_norm'] = df['sz'] / df['sz'].rolling(50, min_periods=1).median()
    df['sz_norm'] = df['sz_norm'].clip(upper=1.0)
    return df

def calibrate(df: pd.DataFrame, decay_grid=(1, 5, 10, 20, 50, 100)):
    """
    Многомерный Hawkes: 4 размерности = [bybit_buy, bybit_sell,
                                         binance_buy, binance_sell]
    Возвращает: baseline(4), adjacency(4x4), decays(4x4)
    """
    dims = ['bybit_buy', 'bybit_sell', 'binance_buy', 'binance_sell']
    timestamps = []
    for d in dims:
        if d.startswith('bybit'):
            v, side = 'bybit', ('buy' if d.endswith('buy') else 'sell')
        else:
            v, side = 'binance', ('buy' if d.endswith('buy') else 'sell')
        mask = (df['venue'] == v) & (
            (df['aggressor'] > 0) if side == 'buy' else (df['aggressor'] < 0)
        )
        ts = df.loc[mask, 'ts_local'].values.astype(np.float64) / 1000.0
        timestamps.append(np.unique(ts))

    # Для MLE с фиксированным decay: пробуем сетку, выбираем лучший по likelihood
    best = None
    for dec in decay_grid:
        try:
            learner = HawkesExpKern(
                decays=dec,
                gofit='likelihood',
                penalty='l2',
                C=1e3,
                solver='bfgs',
                max_iter=200,
                verbose=False,
            )
            learner.fit(timestamps)
            score = learner.score()
            # Branching ratio: sum_j a_ij / b_ij по строкам
            A = learner.adjacency          # shape (4,4), alpha_ij
            B = np.asarray(learner.decays) # shape (4,4), beta_ij
            br = float(np.max(A / np.maximum(B, 1e-9)))
            if best is None or score > best['score']:
                best = dict(dec=dec, score=score,
                            baseline=learner.baseline,
                            adjacency=A, decays=B, branching=br)
        except Exception as e:
            print(f"decay={dec}: failed ({e})")
    if best is None:
        raise SystemExit("all calibrations failed")
    return dims, best

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('symbol')
    ap.add_argument('--hours', type=int, default=48)
    ap.add_argument('--out', default='hawkes_params.json')
    args = ap.parse_args()

    df = load_events(args.symbol, args.hours)
    print(f"trades: {len(df)}")

    dims, best = calibrate(df)
    print(f"best decay={best['dec']}  score={best['score']:.2f}  "
          f"branching={best['branching']:.3f}")
    if not (0.15 <= best['branching'] <= 0.6):
        print("WARNING: branching outside [0.15, 0.6] — calibration unstable")

    import json
    out = {
        'symbol': args.symbol,
        'dims': dims,
        'decay_scalar': best['dec'],
        'baseline': best['baseline'].tolist(),
        'adjacency': best['adjacency'].tolist(),
        'decays': np.asarray(best['decays']).tolist(),
        'branching_ratio': best['branching'],
    }
    with open(args.out, 'w') as f:
        json.dump(out, f, indent=2)
    print(f"written {args.out}")

if __name__ == '__main__':
    main()