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

        std::stringstream ss;
        ss << "{\"u\":" << update.update_id << ",\"b\":[";
        size_t nb = std::min<size_t>(update.bids.size(), 50);
        for (size_t i = 0; i < nb; ++i) {
            if (i > 0) ss << ",";
            ss << "[\"" << std::fixed << std::setprecision(8) << update.bids[i].price
               << "\",\"" << update.bids[i].size << "\"]";
        }
        ss << "],\"a\":[";
        size_t na = std::min<size_t>(update.asks.size(), 50);
        for (size_t i = 0; i < na; ++i) {
            if (i > 0) ss << ",";
            ss << "[\"" << update.asks[i].price
               << "\",\"" << update.asks[i].size << "\"]";
        }
        ss << "]}";

        simdjson::dom::parser parser;
        simdjson::dom::element doc;
        if (parser.parse(ss.str()).get(doc) != simdjson::SUCCESS) {
            if (out_err) *out_err = "parse failed";
            return false;
        }

        book.update(doc, "snapshot");
        book.set_last_update_ms(update.timestamp_ms);

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