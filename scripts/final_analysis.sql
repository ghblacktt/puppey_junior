-- ═══════════════════════════════════════════════════════════════
-- Финальный анализ run_tag 20261102_173431
-- Запускать через 2-3 часа
-- ═══════════════════════════════════════════════════════════════

\echo '════════ 1. ОБЩАЯ КАРТИНА ════════'
SELECT
    COUNT(*) AS n,
    ROUND(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_1s_bps)::numeric, 2)  AS med_1s,
    ROUND(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_5s_bps)::numeric, 2)  AS med_5s,
    ROUND(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_30s_bps)::numeric, 2) AS med_30s,
    ROUND((PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_30s_bps)
         - PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_1s_bps))::numeric, 2) AS recovery,
    ROUND(100.0 * COUNT(*) FILTER (WHERE mk_1s_bps > 0) / COUNT(*), 1) AS hit_pct
FROM mm_markouts
WHERE run_tag = '20261102_173431' AND finalized = TRUE
  AND mk_1s_bps IS NOT NULL AND ABS(mk_1s_bps) < 100;

\echo ''
\echo '════════ 2. PER-SYMBOL (n>=10) ════════'
SELECT symbol, COUNT(*) AS n,
    ROUND(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_1s_bps)::numeric, 2)  AS med_1s,
    ROUND(PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_30s_bps)::numeric, 2) AS med_30s,
    ROUND((PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_30s_bps)
         - PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY mk_1s_bps))::numeric, 2) AS recovery,
    ROUND(100.0 * COUNT(*) FILTER (WHERE mk_1s_bps > 0) / COUNT(*), 1) AS hit_pct
FROM mm_markouts
WHERE run_tag = '20261102_173431' AND finalized = TRUE
  AND mk_1s_bps IS NOT NULL AND ABS(mk_1s_bps) < 100
GROUP BY symbol HAVING COUNT(*) >= 10
ORDER BY med_1s DESC;

\echo ''
\echo '════════ 3. P&L ATTRIBUTION ════════'
SELECT symbol, COUNT(*) AS n,
    ROUND(SUM(spread_income)::numeric, 3) AS spread,
    ROUND(SUM(fee_usd)::numeric, 3) AS fees,
    ROUND((SUM(spread_income) - SUM(fee_usd))::numeric, 3) AS net
FROM mm_pnl_attribution
WHERE run_tag = '20261102_173431'
GROUP BY symbol HAVING COUNT(*) >= 5
ORDER BY net DESC;

\echo ''
\echo '════════ 4. CANCEL REASONS ════════'
SELECT cancel_reason, COUNT(*) AS n
FROM mm_orders
WHERE run_tag = '20261102_173431' AND status = 'CANCELLED'
GROUP BY cancel_reason ORDER BY n DESC;

\echo ''
\echo '════════ 5. MAX_HOLD BY SYMBOL ════════'
SELECT symbol, COUNT(*) AS max_hold_n
FROM mm_orders
WHERE run_tag = '20261102_173431'
  AND status = 'CANCELLED' AND cancel_reason = 'MAX_HOLD'
GROUP BY symbol ORDER BY max_hold_n DESC;