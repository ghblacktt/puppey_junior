#include "local_orderbook.h"

namespace scanner {

    void LocalBook::reset() {
        bids_.clear();
        asks_.clear();
        // update_id НЕ сбрасываем — caller сам решит
    }

    void LocalBook::apply_bid(double px, double sz) {
        if (sz <= 1e-12) bids_.erase(px);
        else             bids_[px] = sz;
    }

    void LocalBook::apply_ask(double px, double sz) {
        if (sz <= 1e-12) asks_.erase(px);
        else             asks_[px] = sz;
    }

    double LocalBook::best_bid() const {
        return bids_.empty() ? 0.0 : bids_.begin()->first;
    }
    double LocalBook::best_ask() const {
        return asks_.empty() ? 0.0 : asks_.begin()->first;
    }
    double LocalBook::bid_size() const {
        return bids_.empty() ? 0.0 : bids_.begin()->second;
    }
    double LocalBook::ask_size() const {
        return asks_.empty() ? 0.0 : asks_.begin()->second;
    }

    bool LocalBook::is_valid_bbo() const {
        const double bb = best_bid();
        const double ba = best_ask();
        return bb > 0.0 && ba > 0.0 && ba > bb;
    }

} // namespace scanner