#pragma once
#include "backtest_types.h"
#include "order_book.h"

namespace bt {

    class BookRebuilder {
    public:
        // Применяет snapshot или delta к OrderBook.
        // Возвращает true если успешно, false если gap/ошибка.
        // Если out_err != nullptr — записывает туда описание ошибки.
        static bool apply(OrderBook& book,
                          const BookSnapshot& update,
                          std::string* out_err = nullptr);

        // Проверка целостности: best_bid < best_ask, оба > 0
        static bool is_valid(const OrderBook& book);

        // Форматирует текущее состояние для лога
        static std::string describe(const OrderBook& book);
    };

}  // namespace bt