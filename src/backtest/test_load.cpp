#include "data_loader.h"
#include "book_rebuilder.h"
#include "order_book.h"
#include <iostream>
#include <iomanip>
#include <chrono>

int main(int argc, char** argv) {
    std::cout << "[1] start" << std::endl;
    std::cout.flush();

    std::string symbol = "SOLUSDT";
    uint64_t minutes_back = 60;
    if (argc > 1) symbol = argv[1];
    if (argc > 2) minutes_back = std::stoull(argv[2]);

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t from_ts = now - minutes_back * 60 * 1000;

    std::cout << "[2] from_ts=" << from_ts << " to_ts=" << now 
              << " diff_minutes=" << minutes_back << std::endl;
    std::cout.flush();

    bt::DataLoader::Config cfg;
    std::cout << "[3] creating loader..." << std::endl;
    std::cout.flush();
    bt::DataLoader loader(cfg);
    std::cout << "[4] loader created, is_connected="
              << (loader.is_connected() ? "true" : "false") << std::endl;
    std::cout.flush();

    std::cout << "[5] calling load_book_updates..." << std::endl;
    std::cout.flush();
    auto books = loader.load_book_updates(symbol, 0, now);
    std::cout << "[6] loaded " << books.size() << " book updates" << std::endl;
    std::cout.flush();

    if (books.empty()) {
        std::cout << "[!] no books loaded, abort" << std::endl;
        return 0;
    }

    std::cout << "[7] first book: ts=" << books[0].timestamp_ms
              << " u=" << books[0].update_id
              << " type=" << books[0].update_type
              << " bids=" << books[0].bids.size()
              << " asks=" << books[0].asks.size() << std::endl;
    std::cout.flush();

    std::cout << "[8] calling load_trades..." << std::endl;
    std::cout.flush();
    auto trades = loader.load_trades(symbol, from_ts, now);
    std::cout << "[9] loaded " << trades.size() << " trades" << std::endl;
    std::cout.flush();

    std::cout << "[10] rebuilding book..." << std::endl;
    std::cout.flush();

    OrderBook book;
    size_t book_events = 0, trade_events = 0;
    size_t invalid_after = 0;
    double last_bb = 0, last_ba = 0;

    for (size_t i = 0; i < books.size(); ++i) {
        if (i % 500 == 0) {
            std::cout << "[10." << i << "] book rebuild " << i << "/" << books.size() << std::endl;
            std::cout.flush();
        }
        ++book_events;
        std::string err;
        if (!bt::BookRebuilder::apply(book, books[i], &err)) {
            if (book_events < 5) {
                std::cerr << "Rebuild error at ts=" << books[i].timestamp_ms
                          << ": " << err << "\n";
            }
            continue;
        }
        if (!bt::BookRebuilder::is_valid(book)) ++invalid_after;
        double bb = book.get_best_bid();
        double ba = book.get_best_ask();
        if (bb > 0) { last_bb = bb; last_ba = ba; }
    }

    std::cout << "[11] rebuild done" << std::endl;
    std::cout << "\n=== RESULTS ===\n";
    std::cout << "Book events:   " << book_events << "\n";
    std::cout << "Trade events:  " << trades.size() << "\n";
    std::cout << "Invalid:       " << invalid_after << "\n";
    std::cout << "Last book:     " << std::fixed << std::setprecision(6)
              << last_bb << " / " << last_ba << "\n";
    if (last_bb > 0 && last_ba > 0) {
        double spread_bps = (last_ba - last_bb) / ((last_ba + last_bb) / 2) * 10000;
        std::cout << "Last spread:   " << spread_bps << " bps\n";
    }
    std::cout << "[12] done" << std::endl;
    return 0;
}
