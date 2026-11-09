#include <clickhouse/client.h>
#include <iostream>
#include <string>

int main() {
    std::cout << "[1] start\n";

    clickhouse::ClientOptions opts;
    opts.SetHost(cfg.host);
    opts.SetPort(cfg.port);
    opts.SetUser(cfg.user);
    opts.SetPassword(cfg.password);
    opts.SetDefaultDatabase(cfg.defaul_database);
    clickhouse::Client client(opts);
    std::cout << "[2] client OK\n";

    // ═══════════════════════════════════════════════════════════
    // ТЕСТ 1: простой SELECT (должен работать — уже проверяли)
    // ═══════════════════════════════════════════════════════════
    std::cout << "\n=== TEST 1: simple ===\n";
    client.Select("SELECT count() FROM orderbook_updates WHERE symbol='SOLUSDT'",
        [](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r) {
                std::cout << "  count=" << block[0]->As<clickhouse::ColumnUInt64>()->At(r) << "\n";
            }
        });
    std::cout << "TEST 1 OK\n";

    // ═══════════════════════════════════════════════════════════
    // ТЕСТ 2: SELECT с arrayMap (подозрительный)
    // ═══════════════════════════════════════════════════════════
    std::cout << "\n=== TEST 2: arrayMap ===\n";
    client.Select(
        "SELECT arrayMap(x -> toFloat64(x), bid_prices) AS bp FROM orderbook_updates "
        "WHERE symbol='SOLUSDT' AND length(bid_prices) > 0 LIMIT 1",
        [](const clickhouse::Block& block) {
            std::cout << "  rows=" << block.GetRowCount() << "\n";
            if (block.GetRowCount() == 0) return;
            
            std::cout << "  getting column[0]...\n";
            auto col = block[0];
            std::cout << "  col type=" << col->Type()->GetName() << "\n";
            
            std::cout << "  casting to ColumnArray...\n";
            auto arr = col->As<clickhouse::ColumnArray>();
            std::cout << "  arr=" << (arr ? "OK" : "NULL") << "\n";
            if (!arr) return;
            
            std::cout << "  arr->Size()=" << arr->Size() << "\n";
            
            std::cout << "  GetAsColumn(0)...\n";
            auto inner = arr->GetAsColumn(0);
            std::cout << "  inner=" << (inner ? "OK" : "NULL") << "\n";
            if (!inner) return;
            
            std::cout << "  inner type=" << inner->Type()->GetName() << "\n";
            
            std::cout << "  cast to ColumnFloat64...\n";
            auto f64 = std::dynamic_pointer_cast<clickhouse::ColumnFloat64>(inner);
            std::cout << "  f64=" << (f64 ? "OK" : "NULL") << "\n";
            if (!f64) return;
            
            std::cout << "  f64->Size()=" << f64->Size() << "\n";
            if (f64->Size() > 0) {
                std::cout << "  f64[0]=" << f64->At(0) << "\n";
            }
        });
    std::cout << "TEST 2 OK\n";

    // ═══════════════════════════════════════════════════════════
    // ТЕСТ 3: arrayMap + все колонки из load_book_updates
    // ═══════════════════════════════════════════════════════════
    std::cout << "\n=== TEST 3: full columns ===\n";
    client.Select(
        "SELECT timestamp, update_id, update_type, "
        "       arrayMap(x -> toFloat64(x), bid_prices) AS bp "
        "FROM orderbook_updates "
        "WHERE symbol='SOLUSDT' AND length(bid_prices) > 0 LIMIT 1",
        [](const clickhouse::Block& block) {
            std::cout << "  rows=" << block.GetRowCount() << "\n";
            if (block.GetRowCount() == 0) return;
            
            std::cout << "  col[0] type=" << block[0]->Type()->GetName() << "\n";
            std::cout << "  col[1] type=" << block[1]->Type()->GetName() << "\n";
            std::cout << "  col[2] type=" << block[2]->Type()->GetName() << "\n";
            std::cout << "  col[3] type=" << block[3]->Type()->GetName() << "\n";
        });
    std::cout << "TEST 3 OK\n";

    // ═══════════════════════════════════════════════════════════
    // ТЕСТ 4: Decimal без arrayMap
    // ═══════════════════════════════════════════════════════════
    std::cout << "\n=== TEST 4: Decimal direct ===\n";
    client.Select(
        "SELECT bid_prices FROM orderbook_updates "
        "WHERE symbol='SOLUSDT' AND length(bid_prices) > 0 LIMIT 1",
        [](const clickhouse::Block& block) {
            std::cout << "  rows=" << block.GetRowCount() << "\n";
            if (block.GetRowCount() == 0) return;
            
            auto col = block[0];
            std::cout << "  col type=" << col->Type()->GetName() << "\n";
            
            auto arr = col->As<clickhouse::ColumnArray>();
            std::cout << "  arr=" << (arr ? "OK" : "NULL") << "\n";
            if (!arr) return;
            
            std::cout << "  arr->Size()=" << arr->Size() << "\n";
            
            auto inner = arr->GetAsColumn(0);
            std::cout << "  inner type=" << inner->Type()->GetName() << "\n";
            
            auto dec = std::dynamic_pointer_cast<clickhouse::ColumnDecimal>(inner);
            std::cout << "  dec=" << (dec ? "OK" : "NULL") << "\n";
            if (!dec) return;
            
            std::cout << "  dec->Size()=" << dec->Size() << "\n";
            std::cout << "  dec->GetScale()=" << dec->GetScale() << "\n";
            if (dec->Size() > 0) {
                auto raw = dec->At(0);
                std::cout << "  dec->At(0) raw available\n";
                double v = static_cast<double>(raw);
                for (int i = 0; i < dec->GetScale(); ++i) v /= 10.0;
                std::cout << "  dec[0] as double=" << v << "\n";
            }
        });
    std::cout << "TEST 4 OK\n";

    std::cout << "\n[ALL DONE]\n";
    return 0;
}
