#include "bybit/bybit_ws.h"
#include "bybit/bybit_api_client.h"
#include "paper_exchange.h"
#include "mm_symbol_registry.h"
#include "screener.h"
#include "scanner_client.h"
#include "slow_loop_supervisor.h"
#include "cross_venue_signal.h"
#include "bitunix/bitunix_api_client.h"
#include "bitunix/bitunix_private_ws.h"
#include "bitunix/bitunix_public_ws.h"
#include "binance/binance_public_ws.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include "util/console.h"
#include "util/env.h"
#include "util/dotenv.h"


#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <streambuf>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <ctime>
#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_set>
#include <execinfo.h>
#include <unistd.h>

namespace net = boost::asio;
namespace ssl = net::ssl;
using namespace std::chrono_literals;
using arb::IExchange;
using arb::PaperExchange;
using arb::ExchangeExecution;

static constexpr bool DEBUG_MM = false;    // false в проде
static constexpr bool DEBUG_WS = false;    // WS-stats, on_resolve

static constexpr bool kEnableBitunixFollower = true;

static std::string curl_get(const std::string& url, int timeout_sec = 15) {
    std::string cmd = "curl -sS -L --compressed --max-time "
                      + std::to_string(timeout_sec) + " '" + url + "' 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::array<char, 8192> buf;
    std::string out;
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) out += buf.data();
    pclose(pipe);
    return out;
}

static std::vector<std::string> fetch_binance_overlap(
        const std::vector<std::string>& candidates) {
    std::vector<std::string> out;
    std::cout << "[MAIN] fetching Binance exchangeInfo...\n";
    std::string resp = curl_get("https://fapi.binance.com/fapi/v1/exchangeInfo", 15);
    if (resp.empty()) {
        std::cerr << "[MAIN] Binance exchangeInfo: empty\n";
        return out;
    }
    simdjson::dom::parser parser;
    auto r = parser.parse(resp);
    if (r.error()) {
        std::cerr << "[MAIN] Binance exchangeInfo: parse error\n";
        return out;
    }
    simdjson::dom::array arr;
    if (r.value()["symbols"].get_array().get(arr) != simdjson::SUCCESS) {
        std::cerr << "[MAIN] Binance exchangeInfo: no symbols array\n";
        return out;
    }
    std::unordered_set<std::string> bnset;
    bnset.reserve(500);
    for (auto item : arr) {
        std::string_view sym, ct, qa, st;
        if (item["symbol"].get_string().get(sym) != simdjson::SUCCESS) continue;
        if (item["contractType"].get_string().get(ct) != simdjson::SUCCESS) continue;
        if (item["quoteAsset"].get_string().get(qa) != simdjson::SUCCESS) continue;
        if (item["status"].get_string().get(st) != simdjson::SUCCESS) continue;
        if (ct != "PERPETUAL" || qa != "USDT" || st != "TRADING") continue;
        bnset.emplace(sym);
    }
    std::cout << "[MAIN] Binance: " << bnset.size() << " PERPETUAL USDT trading\n";
    for (const auto& s : candidates) {
        if (bnset.count(s)) out.push_back(s);
    }
    std::cout << "[MAIN] Binance overlap: " << out.size()
              << " of " << candidates.size() << "\n";
    return out;
}

// ============================================================
// Tee-буфер (без изменений)
// ============================================================
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* a, std::streambuf* b) : a_(a), b_(b) {}
protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        if (a_->sputc((char)c) == traits_type::eof()) return traits_type::eof();
        if (b_->sputc((char)c) == traits_type::eof()) return traits_type::eof();
        if ((char)c == '\n') { a_->pubsync(); b_->pubsync(); }
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        a_->sputn(s, n); b_->sputn(s, n);
        if (std::memchr(s, '\n', (size_t)n)) { a_->pubsync(); b_->pubsync(); }
        return n;
    }
    int sync() override { a_->pubsync(); b_->pubsync(); return 0; }
private:
    std::streambuf* a_;
    std::streambuf* b_;
};

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

// ── helper: прокинуть сигнал об остановке в ioc ──
template <class IoCtx>
static void run_ioc_until_stopped(IoCtx& ioc, const char* tag) {
    while (true) {
        try { ioc.run(); break; }
        catch (const std::exception& e) {
            std::cerr << "[" << tag << "-IOC] " << e.what() << "\n";
            ioc.restart();
        }
    }
    std::cout << "[" << tag << "-IOC] stopped\n";
}

int main() {
    load_dotenv();
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);   // чтобы запись в закрытый сокет не убивала процесс
    signal(SIGSEGV, [](int sig) {
        std::cerr << "\n[SIGSEGV] caught\n";
        void* array[20];
        size_t size = backtrace(array, 20);
        backtrace_symbols_fd(array, size, 2);
        _exit(139);
    });

    std::string BybitKey, BybitSecret, BitunixKey, BitunixSecret, ConnStr;
    try {
        BybitKey      = env_required("BYBIT_API_KEY");
        BybitSecret   = env_required("BYBIT_API_SECRET");
        BitunixKey    = env_required("BITUNIX_API_KEY");
        BitunixSecret = env_required("BITUNIX_API_SECRET");
        ConnStr       = env_required("PG_CONN_STR");
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] config error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    ::mkdir("data", 0755);
    ::mkdir("logs", 0755);

    static std::ofstream log_file("logs/mm.log", std::ios::app);
    if (!log_file.is_open()) {
        std::cerr << "[MAIN] cannot open logs/mm.log\n";
        return EXIT_FAILURE;
    }
    static TeeBuf tee_out(std::cout.rdbuf(), log_file.rdbuf());
    static TeeBuf tee_err(std::cerr.rdbuf(), log_file.rdbuf());
    std::cout.rdbuf(&tee_out);
    std::cerr.rdbuf(&tee_err);

    auto t0 = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::cout << "\n" << clr::bold << clr::cyan
              << "╔══════════════════════════════════════════════════════════╗\n"
              << "║  " << clr::bright_cyan << "HFT MM Bot"
              << std::string(50, ' ') << clr::cyan << "║\n"
              << "╚══════════════════════════════════════════════════════════╝\n"
              << clr::reset;
    std::cout << clr::gray << "  start: " << std::ctime(&t0) << clr::reset;
    std::cout << clr::bold << "  MODE: " << clr::yellow << "PAPER"
              << clr::reset << clr::dim << " (mainnet data + local sim)" << clr::reset << "\n";
    std::cout << "  " << clr::cyan << "Executor:" << clr::reset << " Bybit public WS\n";
    std::cout << "  " << clr::cyan << "Signal:  " << clr::reset << "Bybit flow (cross-venue: off)\n";
    std::cout << "  " << clr::cyan << "Telemetry:" << clr::reset << " Bybit + Binance + Bitunix\n\n";

    // ═══════════════════════════════════════════════════════════
    // 1) REST API — Bybit mainnet + PaperExchange
    // ═══════════════════════════════════════════════════════════
    auto api = std::make_shared<BybitApiClient>(BybitKey, BybitSecret,
            /*use_testnet=*/false);    if (!api->is_connected()) {

        std::cerr << "[MAIN] API not connected\n";
        return EXIT_FAILURE;
    }

    PaperExchange::Config cfg;
    cfg.initial_balance_usd   = 10000.0;
    cfg.maker_fee_bps         = 2.0;
    cfg.taker_fee_bps         = 5.5;
    cfg.taker_slippage_bps    = 3.0;
    cfg.touch_fill_volume_usd = 2000.0;
    cfg.max_position_usd      = 60.0;

    net::io_context exec_ioc;
    auto paper = std::make_unique<PaperExchange>(*api, exec_ioc, cfg);
    PaperExchange* paper_ptr = paper.get();

    std::string run_tag;
    {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&t));
        run_tag = buf;
    }
    paper_ptr->set_run_tag(run_tag);
    std::cout << "[MAIN] run_tag: " << run_tag << "\n";

    // ═══════════════════════════════════════════════════════════
    // 2) Registry
    // ═══════════════════════════════════════════════════════════
    auto registry = MmSymbolRegistry::create(ConnStr);
    registry->snapshot_config("run_tag=" + run_tag);

    std::unique_ptr<IExchange> exchange = std::move(paper);
    std::cout << "[MAIN] PaperExchange: balance=$" << cfg.initial_balance_usd << "\n";

    try {
        pqxx::connection conn(ConnStr);
        pqxx::work txn(conn);
        txn.exec(
                "INSERT INTO mm_runs (run_tag, note) VALUES ($1, $2) "
                "ON CONFLICT (run_tag) DO NOTHING",
                pqxx::params{run_tag, "paper run, momentum 200ms + bitunix follower"});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] run registration error: " << e.what() << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // 3) Executor symbols (Bybit)
    // ═══════════════════════════════════════════════════════════
    static const std::set<std::string> kStableBases = {
            "USDC", "USDT", "DAI", "TUSD", "USDP", "PYUSD",
            "FDUSD", "USDE", "USD1", "RLUSD", "USDD", "USDB",
            "BUSD", "GUSD", "FRAX", "LUSD"
    };
    auto is_stable_base = [](const std::string& sym) -> bool {
        if (sym.size() < 4) return false;
        return kStableBases.count(sym.substr(0, sym.size() - 4)) > 0;
    };

    constexpr int kTopNSymbols = 500;
    auto liquid_syms = api->get_top_liquid_symbols(kTopNSymbols);
    auto all_tickers = api->get_all_tickers_linear();

    std::vector<std::string> exec_symbols;
    exec_symbols.reserve(liquid_syms.size());

    for (const auto& sym : liquid_syms) {
        if (!registry->is_enabled(sym)) continue;
        if (is_stable_base(sym)) continue;
        auto tit = all_tickers.find(sym);
        if (tit == all_tickers.end() || tit->second.last_price <= 0.0) continue;

        auto inst = api->get_instrument_info(sym, "linear");
        if (inst.step_size <= 0.0) continue;
        double step_value = inst.step_size * tit->second.last_price;
        if (step_value > 15.0 || inst.min_order_amt > 10.0) continue;

        exec_symbols.push_back(sym);
    }

    for (const auto& sym : registry->enabled_symbols()) {
        if (std::find(exec_symbols.begin(), exec_symbols.end(), sym) != exec_symbols.end())
            continue;
        if (is_stable_base(sym)) continue;
        auto tit = all_tickers.find(sym);
        if (tit == all_tickers.end() || tit->second.last_price <= 0.0) continue;
        auto inst = api->get_instrument_info(sym, "linear");
        if (inst.step_size <= 0.0) continue;
        double step_value = inst.step_size * tit->second.last_price;
        if (step_value > 15.0 || inst.min_order_amt > 10.0) continue;

        exec_symbols.push_back(sym);
        std::cout << "[MAIN] + " << sym << " (registry-enabled)\n";
    }

    if (exec_symbols.empty()) {
        std::cerr << "[MAIN] FATAL: no symbols\n";
        return EXIT_FAILURE;
    }

    std::cout << "[MAIN] Executor trading " << exec_symbols.size()
              << " symbols (from " << liquid_syms.size() << " liquid)\n";

    // ═══════════════════════════════════════════════════════════
    // 4) Bitunix: REST + private WS + public WS
    //    ВАЖНО: bitunix_symbols и signal_engine — в main-scope,
    //    чтобы пробросить их в BybitWSClient ниже.
    // ═══════════════════════════════════════════════════════════
    std::unique_ptr<BitunixApiClient> bitunix_api;
    std::shared_ptr<BitunixPrivateWS> bitunix_priv_ws;
    std::shared_ptr<BitunixPublicWS>  bitunix_pub_ws;
    net::io_context bitunix_priv_ioc;
    net::io_context bitunix_pub_ioc;
    std::thread bitunix_priv_thread;
    std::thread bitunix_pub_thread;
    ssl::context bitunix_pub_ssl(ssl::context::tlsv12_client);
    // signal_engine создаём ДО любых WS, чтобы можно было безопасно
    // захватывать в лямбды по значению shared_ptr.
    auto signal_engine = std::make_shared<CrossVenueSignalEngine>();
    signal_engine->set_symbols(exec_symbols);

    // Пересечение Bybit × Bitunix — заполняется ниже, но объявлено здесь.
    std::vector<std::string> bitunix_symbols;

    bitunix_api = std::make_unique<BitunixApiClient>(BitunixKey, BitunixSecret);
    auto fut = std::async(std::launch::async, [&]() {
        return bitunix_api->get_available_symbols(exec_symbols);
    });
    if (fut.wait_for(std::chrono::seconds(15)) == std::future_status::ready) {
        bitunix_symbols = fut.get();
    } else {
        std::cerr << "[MAIN] Bitunix tickers timeout (15s)\n";
    }

    // ── FALLBACK: если REST throttled / timeout — используем exec_symbols ──
    if (bitunix_symbols.empty() && !exec_symbols.empty()) {
        std::cerr << "[MAIN] Bitunix REST failed — FALLBACK to exec_symbols\n";
        bitunix_symbols = exec_symbols;
    }

    std::cout << "[MAIN] Bitunix overlap: " << bitunix_symbols.size()
              << " of " << exec_symbols.size() << " executor symbols\n";
    for (size_t i = 0; i < bitunix_symbols.size(); ++i) {
        std::cout << "  [BNX] " << (i + 1) << ". " << bitunix_symbols[i] << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // 5) BybitWSClient (Executor)
    // ═══════════════════════════════════════════════════════════
    ssl::context exec_ssl_ctx(ssl::context::tlsv12_client);
    ssl::context bitunix_priv_ssl(ssl::context::tlsv12_client);
    bitunix_priv_ssl.set_default_verify_paths();
    bitunix_priv_ws = std::make_shared<BitunixPrivateWS>(
            bitunix_priv_ioc, bitunix_priv_ssl,
            BitunixKey, BitunixSecret,
            [](const std::string &order_id, const std::string &symbol,
               const std::string &side, const std::string &status,
               double price, double qty, double filled_qty) {
                std::cout << "[BITUNIX-ORDER] " << symbol
                          << " " << side
                          << " status=" << status
                          << " price=" << price
                          << " qty=" << qty
                          << " filled=" << filled_qty
                          << " order=" << order_id << "\n";
            },
            [](const std::string &symbol, const std::string &side,
               double size, double avg_price, double unrealized_pnl) {
                std::cout << "[BITUNIX-POS] " << symbol
                          << " side=" << side
                          << " size=" << size
                          << " avg=" << avg_price
                          << " upnl=" << unrealized_pnl << "\n";
            },
            []() {
                std::cout << "[BITUNIX-PRIVWS] subscribed order/position\n";
            });
    bitunix_priv_ws->run("fapi.bitunix.com");
    bitunix_priv_thread = std::thread(
            [&bitunix_priv_ioc]() {
                run_ioc_until_stopped(bitunix_priv_ioc, "BITUNIX-PRIV");
            });// ── Public WS: depth_book1 → CrossVenueSignalEngine ──
    exec_ssl_ctx.set_default_verify_paths();
    if (!bitunix_symbols.empty()) {
        bitunix_pub_ssl.set_default_verify_paths();

        bitunix_pub_ws = std::make_shared<BitunixPublicWS>(
                bitunix_pub_ioc, bitunix_pub_ssl, bitunix_symbols);

        bitunix_pub_ws->set_book_callback(
                [signal_engine](const std::string &symbol,
                                double bid, double ask,
                                double bid_size, double ask_size) {
                    signal_engine->on_bitunix_book(symbol, bid, ask,
                                                   bid_size, ask_size);
                });

         bitunix_pub_ws->set_trade_callback(
                 [signal_engine](const std::string& symbol,
                                 double px, double qty, bool is_buyer_maker) {
                     signal_engine->on_bitunix_trade(symbol, px, qty, is_buyer_maker);
                 });

        bitunix_pub_ws->run("fapi.bitunix.com");
        bitunix_pub_thread = std::thread(
                [&bitunix_pub_ioc]() { run_ioc_until_stopped(bitunix_pub_ioc, "BITUNIX-PUB"); });
        std::cout << "[MAIN] Bitunix public WS started with "
                  << bitunix_symbols.size() << " symbols\n";
    } else {
        std::cout << "[MAIN] WARNING: no overlap between Bybit and Bitunix symbols\n";
    }

    auto ws_client = std::make_shared<BybitWSClient>(
            exec_ioc, exec_ssl_ctx, exec_symbols,
            nullptr, *api,
            "/v5/public/linear",
            /*auxiliary=*/false,
            ConnStr);

    ws_client->set_paper_exchange(paper_ptr);
    ws_client->set_initial_balance(cfg.initial_balance_usd);
    ws_client->set_mm_registry(registry);
    ws_client->set_signal_engine(signal_engine);
// ── CH Writer для Фазы 1 (data collection) ──
// ── CH Writer для Фазы 1 (data collection) ──
// ── CH Writer для Фазы 1 (data collection) ──
    ch::Credentials ch_cred;
    ch_cred.host     = env_or("CH_HOST", "localhost");
    ch_cred.port     = std::stoi(env_or("CH_PORT", "9000"));
    ch_cred.user     = env_or("CH_USER", "robot");
    ch_cred.password = env_required("CH_PASSWORD");

// ── main-scope: всё, что нужно в shutdown и через async ──
    std::shared_ptr<ch::Writer>       ch_writer;
    std::shared_ptr<BinancePublicWS>  binance_pub_ws;
    net::io_context                    binance_pub_ioc;
    std::thread                        binance_pub_thread;
    ssl::context                       binance_ssl(ssl::context::tlsv12_client);

    try {
        ch_writer = std::make_shared<ch::Writer>(ch_cred, exec_symbols);
        ws_client->set_ch_writer(ch_writer);
        if (bitunix_pub_ws) bitunix_pub_ws->set_ch_writer(ch_writer);
        std::cout << "[MAIN] CH-Writer attached\n";

        // ── Binance (public WS, no API key) ──
        auto binance_symbols = fetch_binance_overlap(exec_symbols);
        if (!binance_symbols.empty()) {
            binance_ssl.set_default_verify_paths();

            binance_pub_ws = std::make_shared<BinancePublicWS>(
                    binance_pub_ioc, binance_ssl, binance_symbols);
            binance_pub_ws->set_ch_writer(ch_writer);
            binance_pub_ws->run();

            binance_pub_thread = std::thread([&binance_pub_ioc]() {
                run_ioc_until_stopped(binance_pub_ioc, "BINANCE");
            });
            std::cout << "[MAIN] Binance public WS started with "
                      << binance_symbols.size() << " symbols\n";
        } else {
            std::cout << "[MAIN] Binance: no overlap, skipping\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] CH-Writer init failed: " << e.what()
                  << " — continuing without CH\n";
    }
    // Прокидываем set Bitunix-символов ДО init_mm_strategies.
    // is_bitunix_symbol() будет использоваться в create_mm_for_symbol.
    ws_client->set_bitunix_symbols(
            std::set<std::string>(bitunix_symbols.begin(), bitunix_symbols.end()));
    std::cout << "[MAIN] ws_client bitunix_symbols: "
              << bitunix_symbols.size() << "\n";

    paper_ptr->set_fill_callback(
            [ws_client_weak = std::weak_ptr<BybitWSClient>(ws_client)]
                    (const ExchangeExecution& e) {
                auto ws = ws_client_weak.lock();
                if (!ws) return;
                ws->on_paper_fill(e);
            });

    // ═══════════════════════════════════════════════════════════
    // 6) Scanner
    // ═══════════════════════════════════════════════════════════
    net::io_context screener_ioc;
    auto screener = std::make_shared<Screener>(screener_ioc, ConnStr);
    screener->set_run_tag(run_tag);
    screener->load(*api);

    std::vector<std::string> scan_symbols;
    {
        auto top = api->get_top_liquid_symbols(screener->config().scan_top_n);
        for (const auto& s : top) {
            if (!registry->is_enabled(s)) continue;
            scan_symbols.push_back(s);
        }
    }
    std::cout << "[MAIN] Scanner: " << scan_symbols.size() << " symbols\n";

    ssl::context screener_ssl_ctx(ssl::context::tlsv12_client);
    screener_ssl_ctx.set_default_verify_paths();

    auto scanner = std::make_shared<ScannerClient>(
            screener_ioc, screener_ssl_ctx, scan_symbols, *screener);

    std::thread screener_thread([&screener_ioc, scanner, screener]() {
        scanner->run("stream.bybit.com");
        screener->start();
        run_ioc_until_stopped(screener_ioc, "SCREENER");
    });

    // ═══════════════════════════════════════════════════════════
    // 7) Cleanup stale OPEN orders
    // ═══════════════════════════════════════════════════════════
    try {
        auto cleanup_conn = std::make_shared<pqxx::connection>(ConnStr);
        pqxx::work txn(*cleanup_conn);
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        auto res = txn.exec(
                "UPDATE mm_orders SET status='CANCELLED', "
                "cancel_reason='STALE_ON_START', cancel_time=$1 "
                "WHERE status IN ('OPEN','PARTIALLY_FILLED')",
                pqxx::params{now_ms});
        txn.commit();
        std::cout << "[MAIN] cleaned up " << res.affected_rows()
                  << " stale OPEN orders\n";
    } catch (const std::exception& e) {
        std::cerr << "[MAIN] cleanup error: " << e.what() << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // 8) Запуск Executor
    // ═══════════════════════════════════════════════════════════
    ws_client->run("stream.bybit.com", "443", "/v5/public/linear");

    std::thread exec_thread([&exec_ioc]() { run_ioc_until_stopped(exec_ioc, "EXEC"); });

    // ═══════════════════════════════════════════════════════════
    // 9) Warmup
    // ═══════════════════════════════════════════════════════════
    std::cout << "[MAIN] warming up WS books...\n";
    auto warmup_start    = std::chrono::steady_clock::now();
    auto warmup_deadline = warmup_start + std::chrono::seconds(30);

    std::cout << clr::cyan << "Warming up WS books" << clr::reset << "\n";
    int last_ready = -1;
    while (std::chrono::steady_clock::now() < warmup_deadline) {
        int ready = ws_client->count_symbols_with_book();
        int total = static_cast<int>(exec_symbols.size());
        if (ready != last_ready) {
            // ── Progress bar ──
            int pct = total > 0 ? (ready * 100 / total) : 0;
            int filled = ready * 30 / std::max(1, total);
            std::string bar;
            bar.reserve(30 * 3);
            for (int i = 0; i < filled; ++i) bar += "█";
            for (int i = 0; i < 30 - filled; ++i) bar += "░";
            const char* color = pct == 100 ? clr::green : clr::yellow;
            std::cout << "\r  " << color << "[" << bar << "]" << clr::reset
                      << "  " << pad_l(std::to_string(ready) + "/" + std::to_string(total), 7)
                      << "  " << pct << "%"
                      << std::flush;
            last_ready = ready;
        }
        if (ready >= total) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "\n\n";
    int final_ready = ws_client->count_symbols_with_book();
    int final_total = static_cast<int>(exec_symbols.size());
    if (final_ready < final_total) {
        std::cout << "[MAIN] warmup partial: " << final_ready << "/" << final_total
                  << " — " << (final_total - final_ready)
                  << " symbols skipped\n";
    }

    if (paper_ptr != nullptr) {
        std::set<std::string> active_set(exec_symbols.begin(), exec_symbols.end());
        int closed = paper_ptr->close_orphan_positions(active_set);
        if (closed > 0) {
            std::cout << "[MAIN] closed " << closed << " orphan positions\n";
        } else {
            std::cout << "[MAIN] no orphan positions to clean\n";
        }
    }

    // ═══════════════════════════════════════════════════════════
    // 10) Init MM
    // ═══════════════════════════════════════════════════════════
    std::cout << "[MAIN] init_mm_strategies START (timeout 90s)\n";
    std::atomic<bool> mm_done{false};
    std::atomic<bool> mm_abort{false};
    std::thread mm_thread([&]() {
        try {
            ws_client->init_mm_strategies(*exchange);
            mm_done = true;
        } catch (const std::exception& e) {
            std::cerr << "[MAIN] init_mm_strategies EXCEPTION: "
                      << e.what() << "\n";
            mm_done = true;
        }
    });

    auto mm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
    while (!mm_done && std::chrono::steady_clock::now() < mm_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (mm_done) {
        mm_thread.join();
        std::cout << "[MAIN] MM initialized with run_tag=" << run_tag << "\n";
        std::cout << "\n" << clr::bold << clr::green
                  << "══════════════════════════════════════════════════════════\n"
                  << "  BOT READY  (log: logs/mm.log)\n"
                  << "══════════════════════════════════════════════════════════\n"
                  << clr::reset;
        std::cout << "  " << clr::cyan << "Trading symbols: " << clr::reset
                  << exec_symbols.size() << "\n";
        std::cout << "  " << clr::cyan << "Bitunix overlap: " << clr::reset
                  << bitunix_symbols.size() << "\n";
        std::cout << "  " << clr::cyan << "Binance overlap: " << clr::reset
                  << /* binance_symbols.size() */ 0 << "\n";
        std::cout << "  " << clr::cyan << "CH-Writer:       " << clr::reset;
        if (ch_writer) std::cout << clr::green << "connected" << clr::reset;
        else           std::cout << clr::red << "disabled" << clr::reset;
        std::cout << "\n\n";
    } else {
        std::cerr << "[MAIN] init_mm_strategies TIMEOUT 90s — "
                     "running WITHOUT MM (strategies="
                  << ws_client->currently_trading_symbols().size() << ")\n";
        mm_thread.detach();   // висит в фоне, но main идёт дальше
        // MM-tick НЕ запускаем — иначе segfault
    }
    std::cout << "[MAIN] running. Ctrl+C to stop. log: logs/mm.log\n";

    // ═══════════════════════════════════════════════════════════
    // 11) SlowLoop Supervisor
    // ═══════════════════════════════════════════════════════════
    net::io_context supervisor_ioc;
    SlowLoopSupervisor::Config supervisor_cfg;
    supervisor_cfg.check_interval_sec = 30;
    supervisor_cfg.enabled            = true;

    auto supervisor = std::make_shared<SlowLoopSupervisor>(
            exec_ioc, supervisor_ioc, ConnStr, registry,
            std::weak_ptr<BybitWSClient>(ws_client),
            supervisor_cfg);

    supervisor->start();
    std::thread supervisor_thread(
            [&supervisor_ioc]() { run_ioc_until_stopped(supervisor_ioc, "SUPERVISOR"); });
    std::cout << "[MAIN] SlowLoop supervisor started\n";

    // ═══════════════════════════════════════════════════════════
    // 12) Main loop
    // ═══════════════════════════════════════════════════════════
    while (!g_stop) {
        std::this_thread::sleep_for(500ms);
    }

    // ═══════════════════════════════════════════════════════════
    // 13) Shutdown (порядок важен: WS → ioc → threads)
    // ═══════════════════════════════════════════════════════════
    std::cout << "\n[MAIN] stopping...\n";
    if (binance_pub_ws) binance_pub_ws->stop();
    binance_pub_ioc.stop();
    if (binance_pub_thread.joinable()) binance_pub_thread.join();
    supervisor->stop();
    supervisor_ioc.stop();
    if (supervisor_thread.joinable()) supervisor_thread.join();

    if (bitunix_pub_ws)  bitunix_pub_ws->stop();
    bitunix_pub_ioc.stop();
    if (bitunix_pub_thread.joinable()) bitunix_pub_thread.join();

    if (bitunix_priv_ws) bitunix_priv_ws->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bitunix_priv_ioc.stop();
    if (bitunix_priv_thread.joinable()) bitunix_priv_thread.join();

    screener_ioc.stop();
    if (screener_thread.joinable()) screener_thread.join();

    exec_ioc.stop();
    if (exec_thread.joinable()) exec_thread.join();

    for (const auto& sym : ws_client->currently_trading_symbols()) {
        try { ws_client->disable_symbol(sym); } catch (...) {}
    }

    if (ch_writer) ch_writer->flush_now();
    exec_ioc.stop();
    if (exec_thread.joinable()) exec_thread.join();

    try {
        pqxx::connection conn(ConnStr);
        pqxx::work txn(conn);
        txn.exec(
                "UPDATE mm_runs SET stopped_at = NOW() WHERE run_tag = $1",
                pqxx::params{run_tag});
        txn.commit();
    } catch (...) {}

    std::cout << "[MAIN] done\n";
    return EXIT_SUCCESS;
}