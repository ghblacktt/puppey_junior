#pragma once
#include "backtest_types.h"
#include <vector>
#include <string>
#include <memory>

namespace bt {

    class DataLoader {
    public:
        struct Config {
            std::string host = "localhost";
            int         port = 9000;
            std::string user = "robot";
            std::string password = "robot_pass";
            std::string database = "market_data";
        };

        explicit DataLoader(const Config& cfg);
        ~DataLoader();

        // Загрузка book updates для символа за период
        std::vector<BookSnapshot> load_book_updates(
                const std::string& symbol,
                uint64_t from_ts_ms,
                uint64_t to_ts_ms);

        // Загрузка trades для символа за период
        std::vector<TradeEvent> load_trades(
                const std::string& symbol,
                uint64_t from_ts_ms,
                uint64_t to_ts_ms);

        // Загрузка merged stream (book + trades) в хронологическом порядке
        // (с сортировкой по timestamp, при равенстве — trade раньше book,
        //  чтобы fill'ы были доступны до обновления книги)
        std::vector<TimedEvent> load_merged(
                const std::string& symbol,
                uint64_t from_ts_ms,
                uint64_t to_ts_ms);

        // Список символов, для которых есть данные
        std::vector<std::string> available_symbols(
                uint64_t from_ts_ms,
                uint64_t to_ts_ms);

        // Проверка соединения
        bool is_connected() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace bt