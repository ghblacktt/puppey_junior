#include "book_rebuilder.h"
#include <sstream>
#include <iomanip>

namespace bt {

    bool BookRebuilder::apply(OrderBook& book,
                              const BookSnapshot& update,
                              std::string* out_err) {
        if (update.bids.empty() && update.asks.empty()) {
            if (out_err) *out_err = "empty update";
            return false;
        }

        // Формируем JSON для OrderBook::update
        // (переиспользуем существующую логику, чтобы не дублировать apply)
        std::stringstream ss;
        ss << "{";
        ss << "\"u\":" << update.update_id << ",";
        if (!update.bids.empty()) {
            ss << "\"b\":[";
            for (size_t i = 0; i < update.bids.size(); ++i) {
                if (i > 0) ss << ",";
                ss << "[\"" << std::fixed << std::setprecision(8)
                   << update.bids[i].price << "\",\""
                   << update.bids[i].size << "\"]";
            }
            ss << "],";
        }
        if (!update.asks.empty()) {
            ss << "\"a\":[";
            for (size_t i = 0; i < update.asks.size(); ++i) {
                if (i > 0) ss << ",";
                ss << "[\"" << std::fixed << std::setprecision(8)
                   << update.asks[i].price << "\",\""
                   << update.asks[i].size << "\"]";
            }
            ss << "],";
        }
        // Убираем trailing comma
        std::string json = ss.str();
        if (json.back() == ',') json.pop_back();
        json += "}";

        // Парсим через simdjson
        simdjson::dom::parser parser;
        simdjson::dom::element doc;
        if (parser.parse(json).get(doc) != simdjson::SUCCESS) {
            if (out_err) *out_err = "parse failed";
            return false;
        }

        book.update(doc, update.update_type);
        book.set_last_update_ms(update.timestamp_ms);   // ← переопределяем real → backtest
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