#!/bin/bash
# analyze_week1.sh

LOG=week1.log

echo "════════════════════════════════════════════════════════════"
echo "  WARMUP"
echo "════════════════════════════════════════════════════════════"
grep -a "warmup done" $LOG
grep -a "Scanner filter" $LOG
grep -a "Strategies initialized" $LOG

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  DISCOVERY — promote/demote events"
echo "════════════════════════════════════════════════════════════"
echo "Promotes (unique symbols):"
grep -a "\[SYNC\] + NEW" $LOG | awk '{print $4}' | sort -u | wc -l
grep -a "\[SYNC\] + NEW" $LOG | awk '{print $4}' | sort -u

echo ""
echo "Demotes:"
grep -a "\[SYNC\] - DISABLED" $LOG | awk '{print $4}' | sort -u

echo ""
echo "Screener progression:"
grep -a "\[Screener\] stats=" $LOG | tail -20

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  FILL MODEL — hit_through vs hit_touch ratio"
echo "════════════════════════════════════════════════════════════"
grep -a "\[FILL-TYPE\]" $LOG | tail -5

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  METRICS — PnL, markout, balance"
echo "════════════════════════════════════════════════════════════"
grep -a "\[MM-METRICS\]" $LOG | tail -3
grep -a "\[MM-MARKOUT\]" $LOG | tail -3
grep -a "\[PAPER\]" $LOG | grep -v "orphan" | tail -10

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  ADAPTIVE — triggers"
echo "════════════════════════════════════════════════════════════"
echo "TOXIC_KILL:"
grep -ac "TOXIC_KILL" $LOG
echo "MM-POST-LOSS:"
grep -ac "MM-POST-LOSS" $LOG
echo "MM-BLACKLIST:"
grep -ac "MM-BLACKLIST" $LOG
echo "MM-TOXIC (fills):"
grep -ac "MM-TOXIC" $LOG
echo "CRITICAL:"
grep -ac "\[CRITICAL\]" $LOG || echo 0
echo "SIGSEGV:"
grep -ac "SIGSEGV" $LOG || echo 0
echo "pqxx errors:"
grep -ac "pqxx::sql_error" $LOG || echo 0

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  WS-HEALTH — gaps, bad data"
echo "════════════════════════════════════════════════════════════"
echo "WS-BAD (crossed books):"
grep -ac "\[WS-BAD\]" $LOG
echo "Scanner OB gaps:"
grep -ac "OB gap" $LOG
echo "Reconnects:"
grep -ac "\[RECONNECT\]" $LOG

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  SIZE"
echo "════════════════════════════════════════════════════════════"
du -sh $LOG
wc -l $LOG
echo "SCREENER-DBG lines (spam):"
grep -ac "SCREENER-DBG" $LOG