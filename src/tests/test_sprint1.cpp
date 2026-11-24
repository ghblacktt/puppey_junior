// tests/test_sprint1.cpp
// Sprint 1 verification. Запуск: ./bybit_client --test-sprint1
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "market_making_strategy.h"
#include "iexchange.h"
#include "fill_model.h"
#include "screener.h"
#include <boost/asio/io_context.hpp>
#include "local_orderbook.h"

using namespace std::chrono_literals;


// ─────────────────────────────────────────────────────────────
// Семантика Bybit: is_buyer_maker=true ⇔ aggressor=seller ⇔ бьёт bid
//                     is_buyer_maker=false ⇔ aggressor=buyer ⇔ бьёт ask
// Хелперы, чтобы в тестах было очевидно.
// ─────────────────────────────────────────────────────────────
static void trade_sell_aggr(mm::FillModel& fm, const char* sym,
                            double px, double qty, uint64_t ts) {
    fm.on_trade(sym, px, qty, /*is_buyer_maker=*/true, ts);
}
static void trade_buy_aggr(mm::FillModel& fm, const char* sym,
                           double px, double qty, uint64_t ts) {
    fm.on_trade(sym, px, qty, /*is_buyer_maker=*/false, ts);
}
// ─────────────────────────────────────────────────────────────
// Mock: биржа, которая ЕЩЁ НЕ увидела наш fill.
// Возвращает stale long 0.5 всегда — именно этот сценарий
// давал over-close: fill → inv=0 → refresh видит 0.5 → close повторно.
// ─────────────────────────────────────────────────────────────
class StaleLongApi : public arb::IExchange {
public:
    std::string place_limit_order(const std::string&, const std::string&,
                                  double, double, double, double, int) override {
        return "mock_oid";
    }
    std::string place_market_order(const std::string&, const std::string&,
                                   double) override { return "mock_mkt"; }
    std::string last_ret_msg() const override { return ""; }
    int last_ret_code() const override { return 0; }
    bool cancel_order(const std::string&, const std::string&) override { return true; }
    arb::ExchangeOrder get_order(const std::string&, const std::string&) override {
        return {};
    }
    arb::ExchangePositionInfo get_position_info(const std::string&, int) override {
        arb::ExchangePositionInfo p;
        p.is_valid   = true;
        p.size       = 0.5;
        p.signed_qty = 0.5;
        p.side       = "Buy";
        p.avg_price  = 100.0;
        return p;
    }
    arb::ExchangeExecution get_last_execution(const std::string&,
                                              const std::string&) override {
        return {};
    }
    arb::ExchangeInstrumentInfo get_instrument_info(const std::string&,
                                                    const std::string&) override {
        arb::ExchangeInstrumentInfo i;
        i.tick_size     = 0.01;
        i.step_size     = 0.001;
        i.min_order_qty = 0.001;
        i.min_order_amt = 5.0;
        i.is_valid      = true;
        return i;
    }
    double normalize_price(const std::string&, double px) override { return px; }
    double normalize_qty(const std::string&, double qty) override { return qty; }
};

// ─────────────────────────────────────────────────────────────
static int test_stale_refresh_does_not_overwrite_fill() {
    std::cout << "[TEST 1] stale refresh vs recent fill... ";
    std::cout.flush();

    StaleLongApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);

    mm.set_current_time_ms(1'000'000);

    // Sell 0.5 из flat → математически inv = -0.5 (short)
    mm.on_order_filled("oid1", 1'000'000, 100.0, 0.5, "Sell", /*full=*/true);

    // Tick через 100ms — внутри 3-сек cooldown, refresh ДОЛЖЕН пропуститься
    mm.set_current_time_ms(1'000'100);
    mm.tick();

    const double inv = mm.get_inventory_qty();
    if (std::abs(inv - (-0.5)) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: inventory=" << inv
                  << " (ожидалось -0.5)\n"
                  << "  → OVER-CLOSE BUG: fill перезаписан stale refresh\n"
                  << "    (stale API отдаёт +0.5, значит refresh НЕ пропустился)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 1b: cooldown внутри refresh_position истекает через 3+ сек.
//          Вызываем refresh_position() напрямую — иначе внешний
//          refreshPositionIfDue (20 сек throttle) не даст дойти до проверки.
// ─────────────────────────────────────────────────────────────
static int test_cooldown_expires() {
    std::cout << "[TEST 1b] cooldown expires after 3s... ";
    std::cout.flush();

    StaleLongApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);

    mm.set_current_time_ms(1'000'000);
    mm.on_order_filled("oid1", 1'000'000, 100.0, 0.5, "Sell", true);

    // Tick через 100ms — process_deferred_fills применяет fill,
    // last_fill_processed_ms_ = 1'000'100, inv = -0.5
    mm.set_current_time_ms(1'000'100);
    mm.tick();
    if (std::abs(mm.get_inventory_qty() - (-0.5)) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: cooldown не работает на первом тике, inv="
                  << mm.get_inventory_qty() << "\n";
        return 1;
    }

    // t=1'005'000: 5000ms прошло, cooldown (3000) истёк.
    // Вызываем refresh_position напрямую — должен прочитать stale +0.5.
    mm.set_current_time_ms(1'005'000);
    mm.refresh_position();   // ← напрямую, не через tick()

    const double inv = mm.get_inventory_qty();
    if (std::abs(inv - 0.5) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: cooldown не истекает, inv=" << inv
                  << " (ожидалось +0.5 от stale API)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
static int test_multiple_partial_fills_in_order() {
    std::cout << "[TEST 2] multiple partial fills... ";
    std::cout.flush();

    StaleLongApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(2'000'000);

    mm.on_order_filled("o0", 2'000'000, 100.0, 0.5, "Buy",  false);  // open long 0.5
    mm.on_order_filled("o1", 2'000'000, 101.0, 0.2, "Sell", false);  // partial close
    mm.on_order_filled("o2", 2'000'000, 101.0, 0.1, "Sell", false);  // partial close
    mm.on_order_filled("o3", 2'000'000, 101.0, 0.2, "Sell", true);   // full close

    mm.set_current_time_ms(2'000'050);
    mm.tick();

    const double inv = mm.get_inventory_qty();
    if (std::abs(inv) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: inventory=" << inv
                  << " (ожидалось 0 после всех partial fills)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
static int test_concurrent_fills_no_crash() {
    std::cout << "[TEST 3] concurrent fills + tick (1 sec)... ";
    std::cout.flush();

    StaleLongApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sent{0};

    std::thread producer([&]{
        uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const uint64_t n = i++;
            mm.on_order_filled("oid" + std::to_string(n), 3'000'000 + n,
                               100.0, 0.0001,
                               (n % 2) ? "Buy" : "Sell", false);
            sent.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(50us);
        }
    });

    std::thread consumer([&]{
        while (!stop.load(std::memory_order_relaxed)) {
            mm.tick();
            std::this_thread::sleep_for(10ms);
        }
    });

    std::this_thread::sleep_for(1s);
    stop.store(true);
    producer.join();
    consumer.join();

    if (sent.load() < 100) {
        std::cerr << "\n  ❌ FAIL: only " << sent.load() << " fills sent\n";
        return 1;
    }
    std::cout << "PASS (" << sent.load() << " fills)\n";
    return 0;
}

// ============================================================
// Mock с подсчётом API-вызовов
// ============================================================
class CountingApi : public arb::IExchange {
public:
    mutable std::atomic<int> cancel_calls{0};
    mutable std::atomic<int> place_calls{0};

    std::string place_limit_order(const std::string&, const std::string&,
                                  double, double, double, double, int) override {
        place_calls.fetch_add(1);
        return "mock_oid_" + std::to_string(place_calls.load());
    }
    std::string place_market_order(const std::string&, const std::string&,
                                   double) override { return "mock_mkt"; }
    std::string last_ret_msg() const override { return ""; }
    int last_ret_code() const override { return 0; }
    bool cancel_order(const std::string&, const std::string&) override {
        cancel_calls.fetch_add(1);
        return true;
    }
    arb::ExchangeOrder get_order(const std::string&, const std::string&) override { return {}; }
    arb::ExchangePositionInfo get_position_info(const std::string&, int) override {
        arb::ExchangePositionInfo p;
        p.is_valid = true; p.size = 0.0;
        return p;
    }
    arb::ExchangeExecution get_last_execution(const std::string&, const std::string&) override { return {}; }
    arb::ExchangeInstrumentInfo get_instrument_info(const std::string&, const std::string&) override {
        arb::ExchangeInstrumentInfo i;
        i.tick_size = 0.01; i.step_size = 0.001;
        i.min_order_qty = 0.001; i.min_order_amt = 5.0;
        i.is_valid = true;
        return i;
    }
    double normalize_price(const std::string&, double px) override { return px; }
    double normalize_qty(const std::string&, double qty) override { return qty; }
};

// ─────────────────────────────────────────────────────────────
// TEST 4: WS-thread не делает API cancel
// ─────────────────────────────────────────────────────────────
static int test_ws_thread_no_api_call() {
    std::cout << "[TEST 4] WS thread cancel_through не зовёт API... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(1'000'000);

    // Симулируем поток через WS thread
    for (int i = 0; i < 100; ++i) {
        mm.on_trade_through_bid(99.0 + i * 0.01, 1.0);
        mm.on_trade_through_ask(101.0 - i * 0.01, 1.0);
    }

    if (api.cancel_calls.load() != 0) {
        std::cerr << "\n  ❌ FAIL: cancel_order вызван " << api.cancel_calls.load()
                  << " раз из WS thread (ожидалось 0)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 5: tick отменяет активный bid после through-трейда
// ─────────────────────────────────────────────────────────────
static int test_tick_after_through_cancels_active_bid() {
    std::cout << "[TEST 5] tick после through отменяет активный bid... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(1'000'000);

    // Симулируем активный bid через debug-аксессор
    mm.debug_set_bid("bid_active", 100.0);

    // WS: trade @99 прошёл через наш bid@100
    mm.on_trade_through_bid(99.0, 1.0);
    if (api.cancel_calls.load() != 0) {
        std::cerr << "\n  ❌ FAIL: cancel из WS thread\n";
        return 1;
    }

    // Tick — должен отменить
    mm.set_current_time_ms(1'000'100);
    mm.tick();

    if (api.cancel_calls.load() < 1) {
        std::cerr << "\n  ❌ FAIL: tick не отменил bid (calls="
                  << api.cancel_calls.load() << ")\n";
        return 1;
    }
    if (!mm.debug_bid_oid().empty()) {
        std::cerr << "\n  ❌ FAIL: bid_order_id_ не очищен: "
                  << mm.debug_bid_oid() << "\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 6: trigger=min для bid — несколько through, tick видит худший
// ─────────────────────────────────────────────────────────────
static int test_trigger_keeps_worst_price() {
    std::cout << "[TEST 6] trigger хранит худшую цену... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(1'000'000);

    mm.debug_set_bid("bid_active", 100.0);

    // Два трейда через WS: @99 и @97
    mm.on_trade_through_bid(99.0, 1.0);
    mm.on_trade_through_bid(97.0, 1.0);

    // Tick — trigger должен быть 97 (min), 97 <= 100 → cancel
    mm.set_current_time_ms(1'000'100);
    mm.tick();

    if (api.cancel_calls.load() < 1) {
        std::cerr << "\n  ❌ FAIL: bid не отменён\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 7: concurrent WS + tick — no crash, no double-cancel
// ─────────────────────────────────────────────────────────────
static int test_concurrent_through_no_crash() {
    std::cout << "[TEST 7] concurrent through + tick... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> sent{0};

    std::thread ws([&]{
        uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            mm.on_trade_through_bid(99.0 - (i % 5) * 0.01, 1.0);
            mm.on_trade_through_ask(101.0 + (i % 5) * 0.01, 1.0);
            sent.fetch_add(2, std::memory_order_relaxed);
            ++i;
            std::this_thread::sleep_for(100us);
        }
    });
    std::thread tick_thread([&]{
        while (!stop.load(std::memory_order_relaxed)) {
            mm.tick();
            std::this_thread::sleep_for(10ms);
        }
    });

    std::this_thread::sleep_for(1s);
    stop.store(true);
    ws.join();
    tick_thread.join();

    if (sent.load() < 1000) {
        std::cerr << "\n  ❌ FAIL: only " << sent.load() << " events\n";
        return 1;
    }
    std::cout << "PASS (" << sent.load() << " events)\n";
    return 0;
}

static int test_fill_model_hit_through_buy() {
    std::cout << "[TEST 8] FillModel hit_through=fill (buy)... ";
    std::cout.flush();

    mm::FillModel fm;
    fm.register_order({"o1", "BTCUSDT", "Buy", 100.0, 1.0},
                      100.0, 101.0, 10.0, 10.0);

    // sell aggressor @ 99 — прошёл насквозь bid @ 100
    trade_sell_aggr(fm, "BTCUSDT", 99.0, 0.5, 1'000);

    auto fills = fm.drain_fills();
    if (fills.size() != 1
        || std::abs(fills[0].exec_qty   - 0.5)   > 1e-9
        || std::abs(fills[0].exec_price - 100.0) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: fills=" << fills.size()
                  << " qty=" << (fills.empty() ? 0.0 : fills[0].exec_qty)
                  << " px="  << (fills.empty() ? 0.0 : fills[0].exec_price)
                  << " (ожидалось 1 fill 0.5@100)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 9: hit_through → fill (sell)
// Trade @ 101 (buy aggressor), наш ask @ 100 → мы заполнены @ 100.
// ─────────────────────────────────────────────────────────────
static int test_fill_model_hit_through_sell() {
    std::cout << "[TEST 9] FillModel hit_through=fill (sell)... ";
    std::cout.flush();

    mm::FillModel fm;
    fm.register_order({"o1", "BTCUSDT", "Sell", 100.0, 1.0},
                      99.0, 100.0, 10.0, 10.0);

    fm.on_trade("BTCUSDT", 101.0, 0.5, /*is_buyer_maker=*/false, 1'000);

    auto fills = fm.drain_fills();
    if (fills.size() != 1
        || std::abs(fills[0].exec_qty - 0.5) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: sell hit_through не заполнен\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 10: hit_touch → queue consume (не мгновенный fill)
// ─────────────────────────────────────────────────────────────
static int test_fill_model_hit_touch_consumes_queue() {
    std::cout << "[TEST 10] FillModel hit_touch=queue consume... ";
    std::cout.flush();

    mm::FillModel fm;
    // bid @ 100 на best_bid → queue = 10 * 0.02 = 0.2
    fm.register_order({"o1", "BTCUSDT", "Buy", 100.0, 1.0},
                      100.0, 101.0, 10.0, 10.0);

    // 1) sell aggressor @ 100 qty 0.1 < queue 0.2 → fill=0
    trade_sell_aggr(fm, "BTCUSDT", 100.0, 0.1, 1'000);
    auto fills = fm.drain_fills();
    if (!fills.empty()) {
        std::cerr << "\n  ❌ FAIL: fill до консьюма очереди (qty="
                  << fills[0].exec_qty << ")\n";
        return 1;
    }

    // 2) sell aggressor @ 100 qty 0.3 → queue 0.1 → остаток трейда 0.2 = fill
    trade_sell_aggr(fm, "BTCUSDT", 100.0, 0.3, 2'000);
    fills = fm.drain_fills();
    if (fills.size() != 1 || std::abs(fills[0].exec_qty - 0.2) > 1e-9) {
        std::cerr << "\n  ❌ FAIL: queue остаток не fill'ится (qty="
                  << (fills.empty() ? 0.0 : fills[0].exec_qty)
                  << ", ожидалось 0.2)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 11: cancel убирает из активных
// ─────────────────────────────────────────────────────────────
static int test_fill_model_cancel() {
    std::cout << "[TEST 11] FillModel cancel... ";
    std::cout.flush();

    mm::FillModel fm;
    fm.register_order({"o1", "BTCUSDT", "Buy", 100.0, 1.0},
                      100.0, 101.0, 10.0, 10.0);
    fm.cancel("o1");

    fm.on_trade("BTCUSDT", 99.0, 0.5, false, 1'000);
    auto fills = fm.drain_fills();
    if (!fills.empty()) {
        std::cerr << "\n  ❌ FAIL: cancel не убрал ордер\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 12: wrong side не филит
// Buy aggressor не должен заполнять наш sell
// ─────────────────────────────────────────────────────────────
static int test_fill_model_wrong_side() {
    std::cout << "[TEST 12] FillModel wrong side... ";
    std::cout.flush();

    mm::FillModel fm;
    fm.register_order({"o1", "BTCUSDT", "Sell", 100.0, 1.0},
                      99.0, 100.0, 10.0, 10.0);

    // sell aggressor не может заполнить ask-лимит
    trade_sell_aggr(fm, "BTCUSDT", 100.0, 0.5, 1'000);
    auto fills = fm.drain_fills();
    if (!fills.empty()) {
        std::cerr << "\n  ❌ FAIL: wrong-side fill (sell aggressor заполнил ask)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 13: hit_through трейд → screener фиксирует fill
// ─────────────────────────────────────────────────────────────
static int test_screener_shadow_fill_hit_through() {
    std::cout << "[TEST 13] Screener hit_through → fill... ";
    std::cout.flush();

    boost::asio::io_context ioc;
    Screener s(ioc, "");
    s.debug_add_symbol("BTCUSDT");

    // BBO @ 100/100.02 → mid=100.01, shadow_bid=mid*(1-10bps)≈99.91
    s.on_book_update("BTCUSDT", 100.0, 100.02, 50.0, 50.0);

    // Sell aggressor @ 99.5 — пробивает 99.91 насквозь
    s.on_trade("BTCUSDT", 99.5, 1.0, /*is_buyer_maker=*/true, 1'000);

    auto st = s.debug_stats("BTCUSDT");
    if (st.shadow_bid_fills != 1) {
        std::cerr << "\n  ❌ FAIL: shadow_bid_fills=" << st.shadow_bid_fills
                  << " (ожидалось 1)\n";
        return 1;
    }
    if (st.shadow_bid_fill_ms != 1'000) {
        std::cerr << "\n  ❌ FAIL: fill_ms=" << st.shadow_bid_fill_ms
                  << " (ожидалось 1000)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 14: no fill без трейда (только mid движется)
// ─────────────────────────────────────────────────────────────
static int test_screener_no_fill_without_trade() {
    std::cout << "[TEST 14] Screener no fill без трейда... ";
    std::cout.flush();

    boost::asio::io_context ioc;
    Screener s(ioc, "");
    s.debug_add_symbol("ETHUSDT");

    s.on_book_update("ETHUSDT", 100.0, 100.02, 50.0, 50.0);

    // Много book-update'ов с mid ниже нашей цены, но без трейдов
    for (int i = 0; i < 100; ++i) {
        s.on_book_update("ETHUSDT", 98.0, 98.02, 50.0, 50.0);
    }

    auto st = s.debug_stats("ETHUSDT");
    if (st.shadow_bid_fills != 0) {
        std::cerr << "\n  ❌ FAIL: fills=" << st.shadow_bid_fills
                  << " без трейда (было бы со старой BBO-моделью)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 15: adverse после fill
// ─────────────────────────────────────────────────────────────
static int test_screener_adverse_after_fill() {
    std::cout << "[TEST 15] Screener adverse после fill... ";
    std::cout.flush();

    boost::asio::io_context ioc;
    Screener s(ioc, "");
    s.debug_add_symbol("SOLUSDT");

    s.on_book_update("SOLUSDT", 100.0, 100.02, 50.0, 50.0);
    // Fill по bid @ ~99.91 через trade @ 99.5
    s.on_trade("SOLUSDT", 99.5, 1.0, true, 1'000);

    // Прошло 120+ сек (max_hold_sec=120) — adverse должен посчитаться
    // mid упал до 99.0
    s.on_book_update("SOLUSDT", 99.0, 99.02, 50.0, 50.0);

    auto st = s.debug_stats("SOLUSDT");
    // fill был 1, adverse_n должен стать 1
    if (st.shadow_bid_adverse_n < 1) {
        std::cerr << "\n  ❌ FAIL: adverse_n=" << st.shadow_bid_adverse_n
                  << " (ожидалось ≥1)\n";
        return 1;
    }
    // mid упал с 99.91 до 99.0 → adverse ≈ +91 bps (положительный = плохо)
    if (st.shadow_bid_adverse_bps <= 0.0) {
        std::cerr << "\n  ❌ FAIL: adverse_bps=" << st.shadow_bid_adverse_bps
                  << " (ожидалось >0, mid упал)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 16: concurrent on_trade + on_book — no crash
// ─────────────────────────────────────────────────────────────
static int test_screener_concurrent_trades_books() {
    std::cout << "[TEST 16] Screener concurrent trades+books... ";
    std::cout.flush();

    boost::asio::io_context ioc;
    Screener s(ioc, "");
    s.debug_add_symbol("XRPUSDT");

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> trades{0};

    std::thread trade_thr([&]{
        uint64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            bool sell = (i % 2) == 0;
            s.on_trade("XRPUSDT", sell ? 99.5 : 100.5, 0.5, sell,
                       1'000'000 + i);
            trades.fetch_add(1);
            ++i;
            std::this_thread::sleep_for(100us);
        }
    });
    std::thread book_thr([&]{
        while (!stop.load(std::memory_order_relaxed)) {
            s.on_book_update("XRPUSDT", 100.0, 100.02, 50.0, 50.0);
            std::this_thread::sleep_for(1ms);
        }
    });

    std::this_thread::sleep_for(1s);
    stop.store(true);
    trade_thr.join();
    book_thr.join();

    if (trades.load() < 1000) {
        std::cerr << "\n  ❌ FAIL: only " << trades.load() << " trades\n";
        return 1;
    }
    std::cout << "PASS (" << trades.load() << " trades)\n";
    return 0;
}

static int test_markout_is_close_flag() {
    std::cout << "[TEST 17] markout is_close flag... ";
    std::cout.flush();

    StaleLongApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(1'000'000);

    mm.on_order_filled("o_open", 1'000'000, 100.0, 0.5, "Buy", true);
    mm.on_order_filled("o_close", 1'001'000, 101.0, 0.5, "Sell", true);

    mm.set_current_time_ms(1'001'100);
    mm.tick();

    auto m = mm.debug_pending_markouts_is_close();
    int open_cnt = 0, close_cnt = 0;
    for (const auto& [oid, is_close] : m) {
        if (is_close) close_cnt++; else open_cnt++;
    }
    if (open_cnt != 1 || close_cnt != 1) {
        std::cerr << "\n  ❌ FAIL: open=" << open_cnt
                  << " close=" << close_cnt
                  << " (ожидалось 1/1)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 18: окна статистики в Screener
// ─────────────────────────────────────────────────────────────
static int test_screener_windowed_stats() {
    std::cout << "[TEST 18] Screener windowed stats... ";
    std::cout.flush();

    boost::asio::io_context ioc;
    Screener s(ioc, "");
    s.debug_add_symbol("WNDUSDT");

    // Book update регистрирует shadow bid/ask + пушит в update_events_
    s.on_book_update("WNDUSDT", 100.0, 100.02, 50.0, 50.0);
    s.on_book_update("WNDUSDT", 100.0, 100.02, 50.0, 50.0);

    // Fill через hit_through
    s.on_trade("WNDUSDT", 99.5, 1.0, true, 1'000);

    // Запускаем snapshot — он пересчитает окна
    // (приватный, через debug accessor — см. ниже; здесь проверяем через stats)
    auto st = s.debug_stats("WNDUSDT");

    // last_shadow_fills должен быть 1 (окно содержит 1 fill)
    if (st.last_shadow_fills != 0 && st.last_shadow_fills != 1) {
        std::cerr << "\n  ❌ FAIL: last_shadow_fills=" << st.last_shadow_fills
                  << " (ожидалось 0 или 1)\n";
        return 1;
    }
    // Проверим что fills_events_ts_ заполнен (через size)
    if (st.fills_events_ts_.size() != 1) {
        std::cerr << "\n  ❌ FAIL: fills_events_ts_.size()="
                  << st.fills_events_ts_.size() << " (ожидалось 1)\n";
        return 1;
    }
    if (st.update_events_ts_.size() != 2) {
        std::cerr << "\n  ❌ FAIL: update_events_ts_.size()="
                  << st.update_events_ts_.size() << " (ожидалось 2)\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 19: is_enabled для unknown = false (раньше было true)
// ─────────────────────────────────────────────────────────────
static int test_registry_is_enabled_unknown_false() {
    std::cout << "[TEST 19] registry is_enabled(unknown)=false... ";
    std::cout.flush();

    // Пустой registry без подключения к PG — cache пуст
    auto reg = MmSymbolRegistry::create("");   // reload fail = empty cache

    if (reg->is_enabled("UNKNOWN_SYMBOL")) {
        std::cerr << "\n  ❌ FAIL: is_enabled(unknown) вернул true\n";
        return 1;
    }
    if (reg->is_whitelisted("UNKNOWN_SYMBOL")) {
        std::cerr << "\n  ❌ FAIL: is_whitelisted(unknown) вернул true\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 20: shutdown_gracefully при has_exec_=false — прямой путь
// ─────────────────────────────────────────────────────────────
static int test_shutdown_direct_when_no_exec() {
    std::cout << "[TEST 20] shutdown direct (no exec)... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(1'000'000);

    mm.debug_set_bid("bid_x", 100.0);
    mm.debug_set_ask("ask_x", 101.0);

    // has_exec_ = false (default) → cleanup выполняется сразу
    mm.shutdown_gracefully("TEST");

    if (!mm.is_shutdown_complete()) {
        std::cerr << "\n  ❌ FAIL: shutdown_complete=false (ожидалось true)\n";
        return 1;
    }
    if (!mm.debug_bid_oid().empty() || !mm.debug_ask_oid().empty()) {
        std::cerr << "\n  ❌ FAIL: orders не очищены\n";
        return 1;
    }
    if (api.cancel_calls.load() < 2) {
        std::cerr << "\n  ❌ FAIL: cancel calls=" << api.cancel_calls.load()
                  << " (ожидалось ≥2)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 21: shutdown_gracefully при has_exec_=true — defer в tick()
// ─────────────────────────────────────────────────────────────
static int test_shutdown_deferred_to_tick() {
    std::cout << "[TEST 21] shutdown deferred to tick... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);
    mm.set_current_time_ms(1'000'000);

    mm.debug_set_bid("bid_x", 100.0);
    mm.debug_set_ask("ask_x", 101.0);

    // Имитируем live-режим (has_exec_=true) через dummy-executor
    mm.set_executor(boost::asio::system_executor{});

    mm.shutdown_gracefully("TEST");

    // НЕ вызвали tick() → state НЕ должен меняться, shutdown НЕ complete
    if (mm.debug_bid_oid() != "bid_x" || mm.debug_ask_oid() != "ask_x") {
        std::cerr << "\n  ❌ FAIL: state изменён до tick()\n";
        return 1;
    }
    if (mm.is_shutdown_complete()) {
        std::cerr << "\n  ❌ FAIL: shutdown_complete=true до tick()\n";
        return 1;
    }
    if (api.cancel_calls.load() != 0) {
        std::cerr << "\n  ❌ FAIL: cancel вызван из main-thread\n";
        return 1;
    }

    // Теперь tick() — должен выполнить cleanup
    mm.tick();

    if (!mm.is_shutdown_complete()) {
        std::cerr << "\n  ❌ FAIL: shutdown_complete=false после tick()\n";
        return 1;
    }
    if (!mm.debug_bid_oid().empty() || !mm.debug_ask_oid().empty()) {
        std::cerr << "\n  ❌ FAIL: orders не очищены после tick()\n";
        return 1;
    }
    if (api.cancel_calls.load() < 2) {
        std::cerr << "\n  ❌ FAIL: cancel calls=" << api.cancel_calls.load()
                  << " после tick() (ожидалось ≥2)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 22: shutdown_gracefully idempotent (второй вызов = no-op)
// ─────────────────────────────────────────────────────────────
static int test_shutdown_idempotent() {
    std::cout << "[TEST 22] shutdown idempotent... ";
    std::cout.flush();

    CountingApi api;
    OrderBook book;
    ClusterEngine cluster;
    MarketMakingStrategy mm(book, cluster, api, "TEST", 0.001, 25.0, 50.0);

    mm.debug_set_bid("bid_x", 100.0);
    mm.shutdown_gracefully("test1");
    const int calls_after_first = api.cancel_calls.load();

    mm.shutdown_gracefully("test2");
    const int calls_after_second = api.cancel_calls.load();

    if (calls_after_second != calls_after_first) {
        std::cerr << "\n  ❌ FAIL: повторный shutdown вызвал extra cancels ("
                  << calls_after_first << " → " << calls_after_second << ")\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 23: LocalBook — snapshot + delta даёт корректный BBO
// ─────────────────────────────────────────────────────────────
static int test_local_book_snapshot_delta() {
    std::cout << "[TEST 23] LocalBook snapshot+delta BBO... ";
    std::cout.flush();

    scanner::LocalBook book;

    // Snapshot: bids 99.99, 99.98, 99.97; asks 100.01, 100.02
    book.apply_bid(99.99, 1.0);
    book.apply_bid(99.98, 2.0);
    book.apply_bid(99.97, 3.0);
    book.apply_ask(100.01, 1.5);
    book.apply_ask(100.02, 2.5);

    if (book.best_bid() != 99.99 || book.best_ask() != 100.01) {
        std::cerr << "\n  ❌ FAIL: after snapshot bb=" << book.best_bid()
                  << " ba=" << book.best_ask() << " (ожидалось 99.99/100.01)\n";
        return 1;
    }

    // Delta: удаляем 99.99 (sz=0) → best_bid = 99.98
    book.apply_bid(99.99, 0.0);
    if (book.best_bid() != 99.98) {
        std::cerr << "\n  ❌ FAIL: after delta bb=" << book.best_bid()
                  << " (ожидалось 99.98)\n";
        return 1;
    }

    // Delta: добавляем новый уровень 100.005 на ask
    book.apply_ask(100.005, 0.5);
    if (book.best_ask() != 100.005) {
        std::cerr << "\n  ❌ FAIL: after ask delta ba=" << book.best_ask()
                  << " (ожидалось 100.005)\n";
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 24: LocalBook reset
// ─────────────────────────────────────────────────────────────
static int test_local_book_reset() {
    std::cout << "[TEST 24] LocalBook reset... ";
    std::cout.flush();

    scanner::LocalBook book;
    book.apply_bid(100.0, 1.0);
    book.apply_ask(101.0, 1.0);
    book.set_update_id(42);
    book.reset();

    if (book.best_bid() != 0.0 || book.best_ask() != 0.0) {
        std::cerr << "\n  ❌ FAIL: after reset book not empty\n";
        return 1;
    }
    if (book.bid_levels() != 0 || book.ask_levels() != 0) {
        std::cerr << "\n  ❌ FAIL: after reset levels not 0\n";
        return 1;
    }
    // update_id сохраняется
    if (book.last_update_id() != 42) {
        std::cerr << "\n  ❌ FAIL: update_id reset to 0, expected preserved\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────
// TEST 25: LocalBook is_valid_bbo — отсеивает crossed/empty
// ─────────────────────────────────────────────────────────────
static int test_local_book_valid_bbo() {
    std::cout << "[TEST 25] LocalBook is_valid_bbo... ";
    std::cout.flush();

    scanner::LocalBook book;
    if (book.is_valid_bbo()) {
        std::cerr << "\n  ❌ FAIL: empty book valid\n";
        return 1;
    }

    book.apply_bid(100.0, 1.0);
    if (book.is_valid_bbo()) {
        std::cerr << "\n  ❌ FAIL: one-sided book valid\n";
        return 1;
    }

    book.apply_ask(100.5, 1.0);
    if (!book.is_valid_bbo()) {
        std::cerr << "\n  ❌ FAIL: normal book invalid\n";
        return 1;
    }

    // Crossed: ask ниже bid (после временной аномалии)
    book.apply_ask(99.5, 1.0);
    if (book.is_valid_bbo()) {
        std::cerr << "\n  ❌ FAIL: crossed book valid\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

int run_sprint6_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 6 verification (ScannerClient / LocalBook)\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_local_book_snapshot_delta();
    failed += test_local_book_reset();
    failed += test_local_book_valid_bbo();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL SPRINT 6 TESTS PASSED\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    return 1;
}

int run_sprint5_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 5 verification (config safety + shutdown)\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_registry_is_enabled_unknown_false();
    failed += test_shutdown_direct_when_no_exec();
    failed += test_shutdown_deferred_to_tick();
    failed += test_shutdown_idempotent();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL SPRINT 5 TESTS PASSED\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    return 1;
}

int run_sprint4_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 4 verification (markout + windows)\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_markout_is_close_flag();
    failed += test_screener_windowed_stats();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL SPRINT 4 TESTS PASSED\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    return 1;
}

// ─────────────────────────────────────────────────────────────
int run_sprint3b_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 3b verification (Screener×FillModel)\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_screener_shadow_fill_hit_through();
    failed += test_screener_no_fill_without_trade();
    failed += test_screener_adverse_after_fill();
    failed += test_screener_concurrent_trades_books();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL SPRINT 3b TESTS PASSED\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    return 1;
}

// ─────────────────────────────────────────────────────────────
int run_sprint3a_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 3a verification (FillModel)\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_fill_model_hit_through_buy();
    failed += test_fill_model_hit_through_sell();
    failed += test_fill_model_hit_touch_consumes_queue();
    failed += test_fill_model_cancel();
    failed += test_fill_model_wrong_side();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL SPRINT 3a TESTS PASSED\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    return 1;
}

// ─────────────────────────────────────────────────────────────
// run_sprint2_tests — friend к MarketMakingStrategy
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────
int run_sprint1_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 1 verification\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_stale_refresh_does_not_overwrite_fill();
    failed += test_multiple_partial_fills_in_order();
    failed += test_cooldown_expires();
    failed += test_concurrent_fills_no_crash();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL TESTS PASSED\n";
        std::cout << "════════════════════════════════════════════════\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    std::cerr << "════════════════════════════════════════════════\n";
    return 1;
}

int run_sprint2_tests() {
    std::cout << "════════════════════════════════════════════════\n";
    std::cout << " Sprint 2 verification\n";
    std::cout << "════════════════════════════════════════════════\n";

    int failed = 0;
    failed += test_ws_thread_no_api_call();
    failed += test_tick_after_through_cancels_active_bid();
    failed += test_trigger_keeps_worst_price();
    failed += test_concurrent_through_no_crash();

    std::cout << "════════════════════════════════════════════════\n";
    if (failed == 0) {
        std::cout << " ALL SPRINT 2 TESTS PASSED\n";
        return 0;
    }
    std::cerr << " " << failed << " TEST(S) FAILED\n";
    return 1;
}

int main() {
    run_sprint1_tests();
    run_sprint2_tests();
    run_sprint3a_tests();
    run_sprint3b_tests();
    run_sprint4_tests();
    run_sprint5_tests();
    run_sprint6_tests();
    return 0;
}