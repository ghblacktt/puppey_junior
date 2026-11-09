#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ch {

    enum Venue : uint8_t { VenueBybit = 0, VenueBitunix = 1, VenueBinance = 2 };

    struct Credentials {
        std::string host     = "localhost";
        int         port     = 8123;
        std::string user     = "robot";
        std::string password = "robot_pass";           // ← задаётся из main
    };

    struct Event {
        uint64_t ts_local_ms;
        uint64_t ts_event_ms;
        double   a, b, c, d;
        uint64_t trade_id_hash;
        uint32_t symbol_idx;
        uint8_t  kind;         // 0=trade, 1=book
        int8_t   aggressor;
        uint8_t  is_through;
        uint8_t  venue;
    };
    static_assert(sizeof(Event) == 64, "Event must fit cache line");

    inline uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }

    uint64_t fnv1a(const std::string& s);

    class Writer {
    public:
        Writer(const Credentials& cred, const std::vector<std::string>& symbols);
        ~Writer();

        void push_trade(int venue, const std::string& symbol,
                        uint64_t ts_local_ms, uint64_t ts_event_ms,
                        double px, double sz, int8_t aggressor,
                        uint8_t is_through, uint64_t trade_id_hash,
                        double bbo_bid, double bbo_ask);

        void push_book(int venue, const std::string& symbol,
                       uint64_t ts_local_ms, uint64_t ts_event_ms,
                       double bid, double ask, double bid_sz, double ask_sz);

        void flush_now();

    private:
        class SpscQueue {
        public:
            explicit SpscQueue(size_t cap_pow2) : buf_(cap_pow2), mask_(cap_pow2 - 1) {}
            bool push(const Event& e) {
                size_t h = head_.load(std::memory_order_relaxed);
                size_t t = tail_.load(std::memory_order_acquire);
                if (h - t >= buf_.size()) return false;
                buf_[h & mask_] = e;
                head_.store(h + 1, std::memory_order_release);
                return true;
            }
            bool pop(Event& out) {
                size_t t = tail_.load(std::memory_order_relaxed);
                size_t h = head_.load(std::memory_order_acquire);
                if (t == h) return false;
                out = buf_[t & mask_];
                tail_.store(t + 1, std::memory_order_release);
                return true;
            }
        private:
            std::vector<Event> buf_;
            size_t mask_;
            alignas(64) std::atomic<size_t> head_{0};
            alignas(64) std::atomic<size_t> tail_{0};
        };

        SpscQueue binance_q_;

        struct Impl;
        std::unique_ptr<Impl> impl_;

        void worker_loop();
        void flush_batch(std::vector<Event>& batch, bool is_trade);

        SpscQueue bybit_q_;
        SpscQueue bitunix_q_;
        std::unordered_map<std::string, uint32_t> sym_to_idx_;
        std::vector<std::string> symbols_;

        std::thread worker_;
        std::atomic<bool> running_{true};
        std::mutex cv_mtx_;
        std::condition_variable cv_;

        bool is_ch_connected_ = false;   // ← NEW
    };

}