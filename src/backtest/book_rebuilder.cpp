#include "book_rebuilder.h"
#include <sstream>
#include <iomanip>

namespace bt {

    bool BookRebuilder::apply(OrderBook& book,
                              const BookSnapshot& update,
                              std::string* out_err) {
        if (update.bids.empty() || update.asks.empty()) {
            if (out_err) *out_err = "empty sides";
            return false;
        }

        // Строим snapshot JSON с BBO
        std::stringstream ss;
        ss << "{\"u\":" << update.update_id << ",\"b\":[[\""
           << std::fixed << std::setprecision(8) << update.bids[0].price
           << "\",\"" << update.bids[0].size << "\"]],\"a\":[[\""
           << update.asks[0].price << "\",\"" << update.asks[0].size << "\"]]}";

        simdjson::dom::parser parser;
        simdjson::dom::element doc;
        if (parser.parse(ss.str()).get(doc) != simdjson::SUCCESS) {
            if (out_err) *out_err = "parse failed";
            return false;
        }

        // Всегда snapshot → book пересобирается заново
        book.update(doc, "snapshot");
        book.set_last_update_ms(update.timestamp_ms);

        // Валидация
        double bb = book.get_best_bid();
        double ba = book.get_best_ask();
        if (bb <= 0.0 || ba <= 0.0 || bb >= ba) {
            if (out_err) *out_err = "invalid book";
            return false;
        }
        return true;
    }

    bool BookRebuilder::is_valid(const OrderBook& book) {
        double bb = book.get_best_bid();
        double ba = book.get_best_ask();
        return bb > 0.0 && ba > 0.0 && ba > bb;
    }

    std::string BookRebuilder::describe(const OrderBook& book) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(6);
        ss << "bid=" << book.get_best_bid() << "×" << book.get_best_bid_size()
           << " ask=" << book.get_best_ask() << "×" << book.get_best_ask_size();
        return ss.str();
    }

}  // namespace bt