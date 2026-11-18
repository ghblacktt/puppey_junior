\echo '════════════════════════════════════════════════════════════'
\echo '  LAYER 1: PIPELINE HEALTH'
\echo '════════════════════════════════════════════════════════════'

SELECT
  'uptime_hours' AS metric,
  ROUND(EXTRACT(EPOCH FROM MAX(stopped_at) - MIN(started_at))/3600, 2) AS value
FROM mm_runs WHERE run_tag = (SELECT MAX(run_tag) FROM mm_runs);

SELECT
  COUNT(*)                                    AS orders,
  COUNT(*) FILTER (WHERE status='FILLED')     AS filled,
  COUNT(*) FILTER (WHERE status='CANCELLED')  AS cancelled,
  COUNT(*) FILTER (WHERE status='REJECTED')   AS rejected,
  COUNT(*) FILTER (WHERE status='OPEN')       AS still_open,
  ROUND(100.0 * COUNT(*) FILTER (WHERE status='FILLED')
        / NULLIF(COUNT(*),0), 2)              AS fill_pct,
  ROUND(100.0 * COUNT(*) FILTER (WHERE status='REJECTED')
        / NULLIF(COUNT(*),0), 3)              AS reject_pct
FROM mm_orders;

\echo '════════════════════════════════════════════════════════════'
\echo '  LAYER 2: DISCOVERY LOOP'
\echo '════════════════════════════════════════════════════════════'

-- Promoted symbols (auto)
SELECT
  symbol, note, updated_at
FROM mm_symbols
WHERE note LIKE 'auto-promoted%'
  AND updated_at > NOW() - INTERVAL '24 hours'
ORDER BY updated_at;

-- Flip-flop: symbols promoted >1 раз (по note)
SELECT
  symbol,
  (LENGTH(note) - LENGTH(REPLACE(note, 'auto-promoted', ''))) / LENGTH('auto-promoted')
    AS promote_count_in_note,
  enabled, note
FROM mm_symbols
WHERE note LIKE '%auto-promoted%'
  AND (LENGTH(note) - LENGTH(REPLACE(note, 'auto-promoted', ''))) / LENGTH('auto-promoted') > 1
ORDER BY promote_count_in_note DESC;

-- Screener snapshot trend
SELECT
  date_trunc('hour', snapshot_time) AS hour,
  COUNT(*)                          AS snapshots,
  ROUND(AVG(shadow_net_edge_bps)::numeric, 2) AS avg_net_edge,
  MAX(shadow_net_edge_bps)                    AS best_net_edge,
  COUNT(*) FILTER (WHERE is_promoted) AS promoted
FROM mm_screening_stats
WHERE snapshot_time > NOW() - INTERVAL '24 hours'
GROUP BY hour
ORDER BY hour;

\echo '════════════════════════════════════════════════════════════'
\echo '  LAYER 3: FILL QUALITY (Sprint 3a/4 validation)'
\echo '════════════════════════════════════════════════════════════'

-- Общие метрики по open fills
SELECT
  'OPEN' AS kind,
  COUNT(*)                                AS n,
  ROUND(AVG(mk_100ms_bps)::numeric, 2)    AS mk_100ms,
  ROUND(AVG(mk_1s_bps)::numeric, 2)       AS mk_1s,
  ROUND(AVG(mk_5s_bps)::numeric, 2)       AS mk_5s,
  ROUND(AVG(mk_30s_bps)::numeric, 2)      AS mk_30s,
  ROUND(100.0 * COUNT(*) FILTER (WHERE mk_30s_bps > 0)
        / NULLIF(COUNT(*),0), 2)          AS hit_pct_30s
FROM mm_markouts
WHERE finalized = true AND is_close = false
  AND fill_time_ms > extract(epoch from NOW() - INTERVAL '24 hours')*1000

UNION ALL

SELECT
  'CLOSE' AS kind,
  COUNT(*), ROUND(AVG(mk_100ms_bps)::numeric, 2),
  ROUND(AVG(mk_1s_bps)::numeric, 2),
  ROUND(AVG(mk_5s_bps)::numeric, 2),
  ROUND(AVG(mk_30s_bps)::numeric, 2),
  ROUND(100.0 * COUNT(*) FILTER (WHERE mk_30s_bps > 0)
        / NULLIF(COUNT(*),0), 2)
FROM mm_markouts
WHERE finalized = true AND is_close = true
  AND fill_time_ms > extract(epoch from NOW() - INTERVAL '24 hours')*1000;

-- Per-symbol open fills (ranking)
SELECT
  symbol,
  COUNT(*)                                AS n,
  ROUND(AVG(mk_1s_bps)::numeric, 2)       AS mk_1s,
  ROUND(AVG(mk_30s_bps)::numeric, 2)      AS mk_30s,
  COUNT(*) FILTER (WHERE mk_30s_bps > 0) AS pos_fills,
  COUNT(*) FILTER (WHERE mk_30s_bps < -10) AS very_adverse
FROM mm_markouts
WHERE finalized = true AND is_close = false
  AND fill_time_ms > extract(epoch from NOW() - INTERVAL '24 hours')*1000
GROUP BY symbol
HAVING COUNT(*) >= 5
ORDER BY mk_30s DESC;

\echo '════════════════════════════════════════════════════════════'
\echo '  LAYER 4: PNL ATTRIBUTION'
\echo '════════════════════════════════════════════════════════════'

SELECT
  ROUND(SUM(spread_income)::numeric, 4)   AS total_spread_income,
  ROUND(SUM(fee_usd)::numeric, 4)         AS total_fees,
  ROUND((SUM(spread_income) - SUM(fee_usd))::numeric, 4) AS net_pnl,
  COUNT(*)                                AS fills,
  COUNT(DISTINCT symbol)                  AS symbols
FROM mm_pnl_attribution
WHERE fill_time_ms > extract(epoch from NOW() - INTERVAL '24 hours')*1000;

SELECT
  symbol,
  COUNT(*)                                AS fills,
  ROUND(SUM(spread_income)::numeric, 4)   AS spread_income,
  ROUND(SUM(fee_usd)::numeric, 4)         AS fees,
  ROUND((SUM(spread_income)-SUM(fee_usd))::numeric, 4) AS net_pnl
FROM mm_pnl_attribution
WHERE fill_time_ms > extract(epoch from NOW() - INTERVAL '24 hours')*1000
GROUP BY symbol
ORDER BY net_pnl ASC
LIMIT 15;

\echo '════════════════════════════════════════════════════════════'
\echo '  LAYER 5: ORDER LIFETIME (execution quality)'
\echo '════════════════════════════════════════════════════════════'

SELECT
  CASE
    WHEN order_age_ms < 1000 THEN '<1s'
    WHEN order_age_ms < 5000 THEN '1-5s'
    WHEN order_age_ms < 30000 THEN '5-30s'
    WHEN order_age_ms < 120000 THEN '30s-2m'
    ELSE '2m+'
  END AS lifetime_bucket,
  COUNT(*) AS orders,
  COUNT(*) FILTER (WHERE status='FILLED') AS filled,
  ROUND(100.0 * COUNT(*) FILTER (WHERE status='FILLED') / COUNT(*), 2) AS fill_pct
FROM mm_orders
WHERE status IN ('FILLED','CANCELLED')
  AND placed_time > extract(epoch from NOW() - INTERVAL '24 hours')*1000
  AND order_age_ms IS NOT NULL
GROUP BY lifetime_bucket
ORDER BY MIN(order_age_ms);

\echo '════════════════════════════════════════════════════════════'
\echo '  LAYER 6: CANCEL REASONS (where time is spent)'
\echo '════════════════════════════════════════════════════════════'

SELECT
  cancel_reason,
  COUNT(*) AS count,
  ROUND(100.0 * COUNT(*) / (SELECT COUNT(*) FROM mm_orders
         WHERE status='CANCELLED'
         AND placed_time > extract(epoch from NOW() - INTERVAL '24 hours')*1000), 1) AS pct
FROM mm_orders
WHERE status='CANCELLED'
  AND placed_time > extract(epoch from NOW() - INTERVAL '24 hours')*1000
GROUP BY cancel_reason
ORDER BY count DESC;