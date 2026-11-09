#include "bybit/bybit_api_client.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <set>
#include <unordered_map>

std::mutex BybitApiClient::request_mutex_;

double BybitApiClient::safe_stod(std::string_view sv) {
    if (sv.empty()) return 0.0;
    try {
        return std::stod(std::string(sv));
    } catch (...) {
        return 0.0;
    }
}

std::unordered_map<std::string, BybitApiClient::TickerData>
BybitApiClient::get_all_tickers_linear() {
    std::unordered_map<std::string, TickerData> result;
    std::map<std::string, std::string> params = {{"category", "linear"}};
    std::string response = send_request("GET", "/v5/market/tickers", params, false);
    if (response.empty()) return result;

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) return result;

    simdjson::dom::element doc = r.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return result;

    auto list = doc["result"]["list"].get_array();
    if (list.error()) return result;

    for (auto item : list.value()) {
        std::string_view sym, fr, turn, last;
        if (item["symbol"].get_string().get(sym) != simdjson::SUCCESS) continue;

        TickerData t;
        if (item["fundingRate"].get_string().get(fr) == simdjson::SUCCESS)
            t.funding_rate = safe_stod(fr);
        if (item["turnover24h"].get_string().get(turn) == simdjson::SUCCESS)
            t.turnover_24h = safe_stod(turn);
        if (item["lastPrice"].get_string().get(last) == simdjson::SUCCESS)
            t.last_price = safe_stod(last);

        result[std::string(sym)] = t;
    }
    return result;
}

std::string BybitApiClient::place_market_order_open(const std::string& symbol,
                                                    const std::string& side,
                                                    double qty) {
    double norm_qty = normalize_qty(symbol, qty);
    std::map<std::string, std::string> params = {
            {"category",   "linear"},
            {"symbol",     symbol},
            {"side",       side},
            {"orderType",  "Market"},
            {"qty",        format_qty(symbol, norm_qty)},
            {"positionIdx","0"}
    };

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) { last_ret_code = -1; last_ret_msg = "empty"; return ""; }

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) { last_ret_code = -2; last_ret_msg = "parse"; return ""; }

    simdjson::dom::element doc = r.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) {
        last_ret_code = (int)rc;
        std::string_view msg;
        if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg = std::string(msg);
        return "";
    }
    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

BybitApiClient::OrderBookSnapshot
BybitApiClient::get_orderbook_snapshot_spot(const std::string& symbol, int depth) {
    OrderBookSnapshot snap;
    std::map<std::string, std::string> params = {
            {"category", "spot"},
            {"symbol",   symbol},
            {"limit",    std::to_string(depth)}
    };
    std::string response = send_request("GET", "/v5/market/orderbook", params, false);
    if (response.empty()) return snap;

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) return snap;

    simdjson::dom::element doc = r.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return snap;

    auto bids = doc["result"]["b"].get_array();
    if (!bids.error()) {
        for (auto entry : bids.value()) {
            auto pair = entry.get_array().value();
            std::string_view p, q;
            if (pair.size() >= 2 &&
                pair.at(0).get_string().get(p) == simdjson::SUCCESS &&
                pair.at(1).get_string().get(q) == simdjson::SUCCESS) {
                snap.bids.push_back({safe_stod(p), safe_stod(q)});
            }
        }
    }
    auto asks = doc["result"]["a"].get_array();
    if (!asks.error()) {
        for (auto entry : asks.value()) {
            auto pair = entry.get_array().value();
            std::string_view p, q;
            if (pair.size() >= 2 &&
                pair.at(0).get_string().get(p) == simdjson::SUCCESS &&
                pair.at(1).get_string().get(q) == simdjson::SUCCESS) {
                snap.asks.push_back({safe_stod(p), safe_stod(q)});
            }
        }
    }
    return snap;
}

double BybitApiClient::get_funding_rate(const std::string& symbol) {
    std::map<std::string, std::string> params = {
            {"category", "linear"}, {"symbol", symbol}
    };
    std::string response = send_request("GET", "/v5/market/tickers", params, false);
    if (response.empty()) return 0.0;

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) return 0.0;

    simdjson::dom::element doc = r.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return 0.0;

    auto list = doc["result"]["list"].get_array();
    if (list.error() || list.value().size() == 0) return 0.0;

    simdjson::dom::element item;
    if (list.value().at(0).get(item) != simdjson::SUCCESS) return 0.0;

    std::string_view fr;
    if (item["fundingRate"].get_string().get(fr) == simdjson::SUCCESS)
        return safe_stod(fr);
    return 0.0;
}

// ============================================================
// Конструктор — ✅ ИСПРАВЛЕН: use_testnet теперь учитывается
// ============================================================
BybitApiClient::BybitApiClient(const std::string& api_key,
                               const std::string& api_secret,
                               bool use_testnet)
        : api_key_(api_key), api_secret_(api_secret), ctx_(ssl::context::tlsv12_client) {

    if (use_testnet) {
        host_ = "api-demo.bybit.com";
        std::cout << "[BybitAPI] Using TESTNET\n" << std::flush;
    } else {
        host_ = "api.bybit.com";
        std::cout << "[BybitAPI] Using MAINNET\n" << std::flush;
    }
    port_ = "443";
    is_connected_ = true;
}

BybitApiClient::~BybitApiClient() {
    is_connected_ = false;
}

std::string BybitApiClient::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return std::to_string(ms.count());
}

// ============================================================
// Market data
// ============================================================
double BybitApiClient::get_last_price(const std::string& symbol) {
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol}
    };

    std::string response = send_request("GET", "/v5/market/tickers", params, false);
    if (response.empty()) return 0.0;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return 0.0;

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) {
        return 0.0;
    }

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error() || list_result.value().size() == 0) return 0.0;

    simdjson::dom::element item;
    if (list_result.value().at(0).get(item) != simdjson::SUCCESS) return 0.0;

    std::string_view last_price_str;
    if (item["lastPrice"].get_string().get(last_price_str) == simdjson::SUCCESS) {
        return safe_stod(last_price_str);
    }

    return 0.0;
}

std::vector<BybitApiClient::Kline> BybitApiClient::get_kline(
        const std::string& symbol, const std::string& interval, int limit) {
    std::vector<Kline> klines;
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"interval", interval},
            {"limit", std::to_string(limit)}
    };
    std::string response = send_request("GET", "/v5/market/kline", params, false);
    if (response.empty()) return klines;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return klines;

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) return klines;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error()) return klines;

    for (auto item : list_result.value()) {
        auto arr_result = item.get_array();
        if (arr_result.error()) continue;
        auto arr = arr_result.value();
        if (arr.size() < 6) continue;

        Kline k;
        std::string_view ts_str, o_str, h_str, l_str, c_str, v_str;
        if (arr.at(0).get_string().get(ts_str) == simdjson::SUCCESS)
            k.open_time = std::stoull(std::string(ts_str));
        if (arr.at(1).get_string().get(o_str) == simdjson::SUCCESS)
            k.open = safe_stod(o_str);
        if (arr.at(2).get_string().get(h_str) == simdjson::SUCCESS)
            k.high = safe_stod(h_str);
        if (arr.at(3).get_string().get(l_str) == simdjson::SUCCESS)
            k.low = safe_stod(l_str);
        if (arr.at(4).get_string().get(c_str) == simdjson::SUCCESS)
            k.close = safe_stod(c_str);
        if (arr.at(5).get_string().get(v_str) == simdjson::SUCCESS)
            k.volume = safe_stod(v_str);

        klines.push_back(k);
    }
    std::reverse(klines.begin(), klines.end());
    return klines;
}

std::vector<BybitApiClient::Trade> BybitApiClient::get_public_trades(
        const std::string& symbol, int limit) {
    std::vector<Trade> trades;
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"limit", std::to_string(limit)}
    };
    std::string response = send_request("GET", "/v5/market/recent-trade", params, false);
    if (response.empty()) return trades;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return trades;

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) return trades;

    auto list = doc["result"]["list"].get_array();
    if (list.error()) return trades;

    for (auto item : list.value()) {
        Trade t;
        std::string_view price_str, size_str, side_str, time_str;
        if (item["price"].get_string().get(price_str) == simdjson::SUCCESS)
            t.price = safe_stod(price_str);
        if (item["size"].get_string().get(size_str) == simdjson::SUCCESS)
            t.volume = safe_stod(size_str);
        if (item["side"].get_string().get(side_str) == simdjson::SUCCESS)
            t.is_buyer_maker = (side_str == "Sell");
        if (item["time"].get_string().get(time_str) == simdjson::SUCCESS)
            t.timestamp = std::stoull(std::string(time_str));
        trades.push_back(t);
    }
    std::reverse(trades.begin(), trades.end());
    return trades;
}

BybitApiClient::OrderBookSnapshot BybitApiClient::get_orderbook_snapshot(
        const std::string& symbol, int depth) {
    OrderBookSnapshot snap;
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"limit", std::to_string(depth)}
    };
    std::string response = send_request("GET", "/v5/market/orderbook", params, false);
    if (response.empty()) return snap;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return snap;
    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) return snap;

    auto bids_arr = doc["result"]["b"].get_array();
    if (!bids_arr.error()) {
        for (auto item : bids_arr.value()) {
            auto pair = item.get_array().value();
            std::string_view price, qty;
            if (pair.size() >= 2 && pair.at(0).get_string().get(price) == simdjson::SUCCESS &&
                pair.at(1).get_string().get(qty) == simdjson::SUCCESS) {
                snap.bids.push_back({safe_stod(price), safe_stod(qty)});
            }
        }
    }
    auto asks_arr = doc["result"]["a"].get_array();
    if (!asks_arr.error()) {
        for (auto item : asks_arr.value()) {
            auto pair = item.get_array().value();
            std::string_view price, qty;
            if (pair.size() >= 2 && pair.at(0).get_string().get(price) == simdjson::SUCCESS &&
                pair.at(1).get_string().get(qty) == simdjson::SUCCESS) {
                snap.asks.push_back({safe_stod(price), safe_stod(qty)});
            }
        }
    }
    return snap;
}

// ============================================================
// Position / balance
// ============================================================
BybitApiClient::PositionInfo BybitApiClient::get_position_info(
        const std::string& symbol, int positionIdx) {
    PositionInfo info;
    info.symbol       = symbol;
    info.is_valid     = false;
    info.rate_limited = false;
    if (!try_acquire_pos_info_token()) {
        info.rate_limited = true;
        return info;
    }
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol}
    };

    std::string response = send_request("GET", "/v5/position/list", params, true);
    if (response.empty()) return info;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return info;

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) return info;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error() || list_result.value().size() == 0) return info;

    for (auto item : list_result.value()) {
        int64_t idx = 0;
        auto idx_result = item["positionIdx"].get_int64().get(idx);
        if (idx_result == simdjson::SUCCESS && idx == positionIdx) {
            std::string_view side_str, size_str, avg_str, pnl_str;
            if (item["side"].get_string().get(side_str) == simdjson::SUCCESS)
                info.side = std::string(side_str);
            if (item["size"].get_string().get(size_str) == simdjson::SUCCESS)
                info.size = safe_stod(size_str);
            if (item["avgPrice"].get_string().get(avg_str) == simdjson::SUCCESS)
                info.avg_price = safe_stod(avg_str);
            if (item["unrealisedPnl"].get_string().get(pnl_str) == simdjson::SUCCESS)
                info.unrealized_pnl = safe_stod(pnl_str);

            info.is_valid = (info.size > 0 && !info.side.empty());
            break;
        }
    }
    return info;
}

double BybitApiClient::get_balance(const std::string& coin) {
    std::map<std::string, std::string> params = {
            {"accountType", "UNIFIED"}
    };
    std::string response = send_request("GET", "/v5/account/wallet-balance", params, true);
    if (response.empty()) return 0.0;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return 0.0;

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) return 0.0;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error()) return 0.0;

    for (auto account : list_result.value()) {
        auto coins_result = account["coin"].get_array();
        if (coins_result.error()) continue;
        for (auto coin_elem : coins_result.value()) {
            std::string_view coin_name;
            if (coin_elem["coin"].get_string().get(coin_name) == simdjson::SUCCESS &&
                coin_name == coin) {
                std::string_view balance_str;
                if (coin_elem["walletBalance"].get_string().get(balance_str) == simdjson::SUCCESS)
                    return safe_stod(balance_str);
            }
        }
    }
    return 0.0;
}

// ============================================================
// Auth / connection
// ============================================================
std::string BybitApiClient::generate_signature_post(
        const std::string& timestamp, const std::string& body) {
    return hmac_sha256(timestamp + api_key_ + "10000" + body);
}

std::string BybitApiClient::generate_signature_get(
        const std::string& timestamp, const std::string& query_string) {
    return hmac_sha256(timestamp + api_key_ + "10000" + query_string);
}

std::string BybitApiClient::hmac_sha256(const std::string& sign_str) const {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(), api_secret_.c_str(), api_secret_.length(),
         (const unsigned char*)sign_str.c_str(), sign_str.length(), digest, &len);

    std::stringstream ss;
    for (unsigned int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return ss.str();
}

bool BybitApiClient::ensure_connection() {
    if (http_stream_) {
        auto& sock = beast::get_lowest_layer(*http_stream_).socket();
        if (sock.is_open()) return true;
    }

    try {
        http_stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc_, ctx_);

        if (!SSL_set_tlsext_host_name(http_stream_->native_handle(), host_.c_str())) {
            std::cerr << "[BybitAPI] SNI error\n";
            http_stream_.reset();
            return false;
        }

        if (cached_endpoints_.empty()) {
            tcp::resolver resolver{ioc_};
            auto results = resolver.resolve(host_, port_);
            for (const auto& r : results) cached_endpoints_.push_back(r.endpoint());
        }

        beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
        beast::get_lowest_layer(*http_stream_).connect(cached_endpoints_);

        beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
        http_stream_->handshake(ssl::stream_base::client);

        beast::get_lowest_layer(*http_stream_).socket().set_option(tcp::no_delay(true));
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BybitAPI] Connect failed: " << e.what() << "\n";
        http_stream_.reset();
        cached_endpoints_.clear();
        return false;
    }
}

void BybitApiClient::close_connection() {
    if (http_stream_) {
        beast::error_code ec;
        http_stream_->shutdown(ec);
        http_stream_.reset();
    }
}

std::string BybitApiClient::send_request(const std::string& method, const std::string& endpoint,
                                         const std::map<std::string, std::string>& params,
                                         bool authenticated) {
    std::lock_guard<std::mutex> lock(io_mutex_);

    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (!ensure_connection()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            http::verb verb = (method == "GET") ? http::verb::get : http::verb::post;
            std::string target = endpoint;
            std::string body_or_query;

            if (!params.empty()) {
                std::string query_string;
                bool first = true;
                for (const auto& [key, value] : params) {
                    if (!first) query_string += "&";
                    query_string += key + "=" + value;
                    first = false;
                }
                if (verb == http::verb::get) {
                    target = endpoint + "?" + query_string;
                    body_or_query = query_string;
                } else {
                    body_or_query = "{";
                    first = true;
                    for (const auto& [key, value] : params) {
                        if (!first) body_or_query += ",";
                        body_or_query += "\"" + key + "\":\"" + value + "\"";
                        first = false;
                    }
                    body_or_query += "}";
                }
            }

            http::request<http::string_body> req{verb, target, 11};
            req.set(http::field::host, host_);
            req.set(http::field::user_agent, "HFT-Cpp-Client");
            req.keep_alive(true);

            if (authenticated) {
                std::string timestamp = get_timestamp();
                std::string signature;
                if (verb == http::verb::get) {
                    signature = generate_signature_get(timestamp, body_or_query);
                } else {
                    signature = generate_signature_post(timestamp, body_or_query);
                    req.set(http::field::content_type, "application/json");
                    req.body() = body_or_query;
                    req.prepare_payload();
                }
                req.set("X-BAPI-API-KEY", api_key_);
                req.set("X-BAPI-SIGN", signature);
                req.set("X-BAPI-TIMESTAMP", timestamp);
                req.set("X-BAPI-RECV-WINDOW", "10000");
            }

            beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
            beast::error_code write_ec;
            http::write(*http_stream_, req, write_ec);
            if (write_ec) {
                std::cerr << "[BybitAPI] Write error: " << write_ec.message() << "\n";
                close_connection();
                continue;
            }

            beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            beast::error_code read_ec;
            http::read(*http_stream_, buffer, res, read_ec);
            if (read_ec) {
                std::cerr << "[BybitAPI] Read error: " << read_ec.message() << "\n";
                close_connection();
                continue;
            }

            return res.body();
        } catch (const std::exception& e) {
            std::cerr << "[BybitAPI] Exception: " << e.what() << "\n";
            close_connection();
        }
    }
    return "";
}

// ============================================================
// Instruments / normalization
// ============================================================
InstrumentInfo BybitApiClient::get_instrument_info(const std::string& symbol,
                                                   const std::string& category) {
    std::string cache_key = category + ":" + symbol;
    {
        std::lock_guard<std::mutex> lock(instruments_mutex_);
        auto it = instruments_cache_.find(cache_key);
        if (it != instruments_cache_.end()) return it->second;
    }

    InstrumentInfo info;
    info.symbol = symbol;

    std::map<std::string, std::string> params = {
            {"category", category},
            {"symbol", symbol}
    };

    std::string response = send_request("GET", "/v5/market/instruments-info", params, false);
    if (response.empty()) {
        std::cerr << "[BybitAPI] Empty instrument info for " << cache_key << "\n";
        return info;
    }

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) {
        std::cerr << "[BybitAPI] Parse error: " << result.error() << "\n";
        return info;
    }

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) {
        std::string_view ret_msg;
        if (doc["retMsg"].get_string().get(ret_msg) == simdjson::SUCCESS)
            std::cerr << "[BybitAPI] Instrument info error (" << cache_key << "): " << ret_msg << "\n";
        return info;
    }

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error() || list_result.value().size() == 0) {
        std::cerr << "[BybitAPI] Empty list for " << cache_key << "\n";
        return info;
    }

    auto item = list_result.value().at(0);
    std::string_view tick_str, step_str, minq_str, mina_str;

    auto pf = item["priceFilter"];
    if (!pf.error()) {
        if (pf["tickSize"].get_string().get(tick_str) == simdjson::SUCCESS)
            info.tick_size = safe_stod(tick_str);
    }

    auto lf = item["lotSizeFilter"];
    if (!lf.error()) {
        if (lf["qtyStep"].get_string().get(step_str) == simdjson::SUCCESS)
            info.step_size = safe_stod(step_str);
        if (lf["minOrderQty"].get_string().get(minq_str) == simdjson::SUCCESS)
            info.min_order_qty = safe_stod(minq_str);
        if (lf["minOrderAmt"].get_string().get(mina_str) == simdjson::SUCCESS)
            info.min_order_amt = safe_stod(mina_str);
    }

    {
        std::lock_guard<std::mutex> lock(instruments_mutex_);
        instruments_cache_[cache_key] = info;
    }
    return info;
}

static bool is_stablecoin_base(const std::string& base) {
    static const std::set<std::string> stables = {
            "USDC", "USDT", "DAI", "TUSD", "USDP", "PYUSD",
            "FDUSD", "USDE", "USD1", "RLUSD", "USDD", "USDB",
            "BUSD", "GUSD", "FRAX", "LUSD", "UST", "USDE"
    };
    return stables.count(base) > 0;
}

std::vector<std::string> BybitApiClient::get_top_liquid_spot_symbols(int top_n) {
    std::vector<std::pair<std::string, double>> syms_with_turnover;
    std::map<std::string, std::string> params = {{"category", "spot"}};
    std::string response = send_request("GET", "/v5/market/tickers", params, false);
    if (response.empty()) return {};

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return {};

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return {};

    auto list = doc["result"]["list"].get_array();
    if (list.error()) return {};

    for (auto item : list.value()) {
        std::string_view sym, turn_str;
        if (item["symbol"].get_string().get(sym) == simdjson::SUCCESS &&
            item["turnover24h"].get_string().get(turn_str) == simdjson::SUCCESS) {
            std::string s = std::string(sym);
            if (s.size() < 4 || s.substr(s.size() - 4) != "USDT") continue;
            if (s.find('-') != std::string::npos) continue;
            if (s.find("PERP") != std::string::npos) continue;
            if (is_stablecoin_base(s.substr(0, s.size() - 4))) continue;
            double turn = safe_stod(turn_str);
            if (turn > 100000.0) {
                syms_with_turnover.emplace_back(s, turn);
            }
        }
    }

    std::sort(syms_with_turnover.begin(), syms_with_turnover.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::string> result_syms;
    for (int i = 0; i < std::min(top_n, (int)syms_with_turnover.size()); ++i)
        result_syms.push_back(syms_with_turnover[i].first);
    return result_syms;
}

// ---- Linear normalize/format ----
double BybitApiClient::normalize_price(const std::string& symbol, double price) {
    auto info = get_instrument_info(symbol, "linear");
    if (info.tick_size <= 0) return price;
    double n = std::round(price / info.tick_size) * info.tick_size;
    int dec = 0; double t = info.tick_size;
    while (t < 1.0 && dec < 10) { t *= 10; dec++; }
    std::stringstream ss; ss << std::fixed << std::setprecision(dec) << n;
    return safe_stod(ss.str());
}

double BybitApiClient::normalize_qty(const std::string& symbol, double qty) {
    auto info = get_instrument_info(symbol, "linear");
    if (info.step_size <= 0) {
        std::cerr << "[BybitAPI] Invalid step_size for " << symbol << ", using floor\n";
        return std::floor(qty);
    }
    double n = std::floor(qty / info.step_size) * info.step_size;
    if (n < info.min_order_qty) n = info.min_order_qty;
    return n;
}

std::string BybitApiClient::format_price(const std::string& symbol, double price) {
    auto info = get_instrument_info(symbol, "linear");
    int dec = 0; double t = info.tick_size;
    while (t < 1.0 && dec < 10) { t *= 10; dec++; }
    std::stringstream ss; ss << std::fixed << std::setprecision(dec) << price;
    return ss.str();
}

std::string BybitApiClient::format_qty(const std::string& symbol, double qty) {
    auto info = get_instrument_info(symbol, "linear");
    int dec = 0; double t = info.step_size;
    if (t >= 1.0) dec = 0;
    else { while (t < 1.0 && dec < 10) { t *= 10; dec++; } }
    std::stringstream ss; ss << std::fixed << std::setprecision(dec) << qty;
    return ss.str();
}

// ---- Spot normalize/format ----
double BybitApiClient::normalize_price_spot(const std::string& symbol, double price) {
    auto info = get_instrument_info(symbol, "spot");
    if (info.tick_size <= 0) return price;
    double n = std::round(price / info.tick_size) * info.tick_size;
    int dec = 0; double t = info.tick_size;
    while (t < 1.0 && dec < 10) { t *= 10; dec++; }
    std::stringstream ss; ss << std::fixed << std::setprecision(dec) << n;
    return safe_stod(ss.str());
}

double BybitApiClient::normalize_qty_spot(const std::string& symbol, double qty) {
    auto info = get_instrument_info(symbol, "spot");
    if (info.step_size <= 0) return std::floor(qty);
    double n = std::floor(qty / info.step_size) * info.step_size;
    if (n < info.min_order_qty) n = info.min_order_qty;
    return n;
}

std::string BybitApiClient::format_price_spot(const std::string& symbol, double price) {
    auto info = get_instrument_info(symbol, "spot");
    int dec = 0; double t = info.tick_size;
    while (t < 1.0 && dec < 10) { t *= 10; dec++; }
    std::stringstream ss; ss << std::fixed << std::setprecision(dec) << price;
    return ss.str();
}

std::string BybitApiClient::format_qty_spot(const std::string& symbol, double qty) {
    auto info = get_instrument_info(symbol, "spot");
    int dec = 0; double t = info.step_size;
    if (t >= 1.0) dec = 0;
    else { while (t < 1.0 && dec < 10) { t *= 10; dec++; } }
    std::stringstream ss; ss << std::fixed << std::setprecision(dec) << qty;
    return ss.str();
}

// ============================================================
// Order placement
// ============================================================
std::string BybitApiClient::place_limit_order(const std::string& symbol, const std::string& side,
                                              double price, double qty,
                                              double take_profit, double stop_loss,
                                              int positionIdx) {
    double norm_price = normalize_price(symbol, price);
    double norm_qty   = normalize_qty(symbol, qty);
    std::string price_str = format_price(symbol, norm_price);
    std::string qty_str   = format_qty(symbol, norm_qty);

    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"side", side},
            {"orderType", "Limit"},
            {"qty", qty_str},
            {"price", price_str},
            {"timeInForce", "PostOnly"},
            {"positionIdx", std::to_string(positionIdx)}
    };

    if (take_profit > 0 || stop_loss > 0) params["tpslMode"] = "Partial";
    if (take_profit > 0) {
        std::string tp = format_price(symbol, take_profit);
        params["takeProfit"] = tp;
        params["tpTriggerBy"] = "LastPrice";
        params["tpOrderType"] = "Limit";
        params["tpLimitPrice"] = tp;
    }
    if (stop_loss > 0) {
        std::string sl = format_price(symbol, stop_loss);
        params["stopLoss"] = sl;
        params["slTriggerBy"] = "LastPrice";
        params["slOrderType"] = "Limit";
        params["slLimitPrice"] = sl;
    }

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) return "";

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return "";

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) {
        last_ret_code = (int)ret_code;
        std::string_view msg;
        if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg = std::string(msg);
        std::cerr << "[BybitAPI] place_limit_order failed: code="
                  << last_ret_code << " msg=" << last_ret_msg << "\n";
        return "";
    }

    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

std::string BybitApiClient::place_market_order(const std::string& symbol,
                                               const std::string& side,
                                               double qty) {
    double norm_qty = normalize_qty(symbol, qty);
    std::string qty_str = format_qty(symbol, norm_qty);

    std::map<std::string, std::string> params = {
            {"category",    "linear"},
            {"symbol",      symbol},
            {"side",        side},
            {"orderType",   "Market"},
            {"qty",         qty_str},
            {"reduceOnly",  "true"},
            {"positionIdx", "0"}
    };

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) { last_ret_code = -1; last_ret_msg = "empty response"; return ""; }

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) { last_ret_code = -2; last_ret_msg = "parse error"; return ""; }

    simdjson::dom::element doc = result.value();
    int64_t ret_code = 0;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS || ret_code != 0) {
        last_ret_code = (int)ret_code;
        std::string_view msg;
        if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg = std::string(msg);
        std::cerr << "[BybitAPI] place_market_order failed: code="
                  << last_ret_code << " msg=" << last_ret_msg << "\n";
        return "";
    }
    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

std::string BybitApiClient::place_ioc_limit_order(const std::string& symbol,
                                                  const std::string& side,
                                                  double price, double qty) {
    double np = normalize_price(symbol, price);
    double nq = normalize_qty(symbol, qty);

    std::map<std::string, std::string> params = {
            {"category",    "linear"},
            {"symbol",      symbol},
            {"side",        side},
            {"orderType",   "Limit"},
            {"qty",         format_qty(symbol, nq)},
            {"price",       format_price(symbol, np)},
            {"timeInForce", "IOC"},
            {"positionIdx", "0"}
    };

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) { last_ret_code = -1; last_ret_msg = "empty"; return ""; }

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) { last_ret_code = -2; last_ret_msg = "parse"; return ""; }

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) {
        last_ret_code = (int)rc;
        std::string_view msg;
        if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg = std::string(msg);
        return "";
    }
    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

std::string BybitApiClient::place_ioc_limit_order_spot(const std::string& symbol,
                                                       const std::string& side,
                                                       double price, double qty) {
    double np = normalize_price_spot(symbol, price);
    double nq = normalize_qty_spot(symbol, qty);
    if (np <= 0 || nq <= 0) { last_ret_code = -3; last_ret_msg = "normalize failed"; return ""; }

    std::map<std::string, std::string> params = {
            {"category",    "spot"},
            {"symbol",      symbol},
            {"side",        side},
            {"orderType",   "Limit"},
            {"qty",         format_qty_spot(symbol, nq)},
            {"price",       format_price_spot(symbol, np)},
            {"timeInForce", "IOC"}
    };

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) { last_ret_code = -1; last_ret_msg = "empty"; return ""; }

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) { last_ret_code = -2; last_ret_msg = "parse"; return ""; }

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) {
        last_ret_code = (int)rc;
        std::string_view msg;
        if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg = std::string(msg);
        std::cerr << "[BybitAPI] place_ioc_limit_order_spot failed: code="
                  << last_ret_code << " msg=" << last_ret_msg << "\n";
        return "";
    }
    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

std::string BybitApiClient::place_market_order_spot(const std::string& symbol,
                                                    const std::string& side,
                                                    double qty) {
    double nq = normalize_qty_spot(symbol, qty);
    if (nq <= 0) { last_ret_code = -3; last_ret_msg = "normalize failed"; return ""; }

    std::map<std::string, std::string> params = {
            {"category",  "spot"},
            {"symbol",    symbol},
            {"side",      side},
            {"orderType", "Market"},
            {"qty",       format_qty_spot(symbol, nq)},
            {"marketUnit",  "base"}
    };

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) { last_ret_code = -1; last_ret_msg = "empty"; return ""; }

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) { last_ret_code = -2; last_ret_msg = "parse"; return ""; }

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) {
        last_ret_code = (int)rc;
        std::string_view msg;
        if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg = std::string(msg);
        return "";
    }
    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

bool BybitApiClient::cancel_order(const std::string& symbol, const std::string& order_id) {
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"orderId", order_id}
    };
    std::string response = send_request("POST", "/v5/order/cancel", params, true);
    if (response.empty()) return false;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return false;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    return doc["retCode"].get_int64().get(rc) == simdjson::SUCCESS && rc == 0;
}

bool BybitApiClient::cancel_all_orders(const std::string& symbol) {
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol}
    };
    std::string response = send_request("POST", "/v5/order/cancel-all", params, true);
    if (response.empty()) return false;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return false;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    return doc["retCode"].get_int64().get(rc) == simdjson::SUCCESS && rc == 0;
}

// ============================================================
// Orders / executions
// ============================================================
BybitOrder BybitApiClient::get_order(const std::string& symbol, const std::string& order_id) {
    BybitOrder order;
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"orderId", order_id}
    };

    std::string response = send_request("GET", "/v5/order/realtime", params, true);
    if (!response.empty()) {
        simdjson::dom::parser parser;
        auto result = parser.parse(response);
        if (!result.error()) {
            simdjson::dom::element doc = result.value();
            int64_t rc = 0;
            if (doc["retCode"].get_int64().get(rc) == simdjson::SUCCESS && rc == 0) {
                auto list_result = doc["result"]["list"].get_array();
                if (!list_result.error() && list_result.value().size() > 0) {
                    simdjson::dom::element order_elem;
                    if (list_result.value().at(0).get(order_elem) == simdjson::SUCCESS) {
                        parse_order(order_elem, order);
                        return order;
                    }
                }
            }
        }
    }

    params["limit"] = "1";
    response = send_request("GET", "/v5/order/history", params, true);
    if (!response.empty()) {
        simdjson::dom::parser parser;
        auto result = parser.parse(response);
        if (!result.error()) {
            simdjson::dom::element doc = result.value();
            int64_t rc = 0;
            if (doc["retCode"].get_int64().get(rc) == simdjson::SUCCESS && rc == 0) {
                auto list_result = doc["result"]["list"].get_array();
                if (!list_result.error() && list_result.value().size() > 0) {
                    simdjson::dom::element order_elem;
                    if (list_result.value().at(0).get(order_elem) == simdjson::SUCCESS) {
                        parse_order(order_elem, order);
                        return order;
                    }
                }
            }
        }
    }
    return order;
}

void BybitApiClient::parse_order(const simdjson::dom::element& order_elem, BybitOrder& order) {
    std::string_view oid, sym, side, otype, status;
    std::string_view price_str, qty_str, created_str, updated_str;

    if (order_elem["orderId"].get_string().get(oid) == simdjson::SUCCESS) order.order_id = std::string(oid);
    if (order_elem["symbol"].get_string().get(sym) == simdjson::SUCCESS) order.symbol = std::string(sym);
    if (order_elem["side"].get_string().get(side) == simdjson::SUCCESS) order.side = std::string(side);
    if (order_elem["orderType"].get_string().get(otype) == simdjson::SUCCESS) order.order_type = std::string(otype);
    if (order_elem["orderStatus"].get_string().get(status) == simdjson::SUCCESS) order.order_status = std::string(status);
    if (order_elem["price"].get_string().get(price_str) == simdjson::SUCCESS) order.price = safe_stod(price_str);
    if (order_elem["qty"].get_string().get(qty_str) == simdjson::SUCCESS) order.qty = safe_stod(qty_str);
    if (order_elem["createdTime"].get_string().get(created_str) == simdjson::SUCCESS) order.created_time = std::stoull(std::string(created_str));
    if (order_elem["updatedTime"].get_string().get(updated_str) == simdjson::SUCCESS) order.updated_time = std::stoull(std::string(updated_str));
}

std::vector<BybitOrder> BybitApiClient::get_open_orders(const std::string& symbol) {
    std::vector<BybitOrder> orders;
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol}
    };
    std::string response = send_request("GET", "/v5/order/realtime", params, true);
    if (response.empty()) return orders;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return orders;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return orders;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error()) return orders;

    for (auto order_elem : list_result.value()) {
        BybitOrder order;
        parse_order(order_elem, order);
        orders.push_back(order);
    }
    return orders;
}

ExecutionInfo BybitApiClient::get_last_execution(const std::string& symbol, const std::string& order_id) {
    ExecutionInfo info;
    info.is_valid = false;

    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"orderId", order_id},
            {"limit", "1"}
    };
    std::string response = send_request("GET", "/v5/execution/list", params, true);
    if (response.empty()) return info;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return info;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return info;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error() || list_result.value().size() == 0) return info;

    simdjson::dom::element item;
    if (list_result.value().at(0).get(item) != simdjson::SUCCESS) return info;

    std::string_view price_str, qty_str, fee_str, fee_cur, time_str, side_str;
    if (item["execPrice"].get_string().get(price_str) == simdjson::SUCCESS) info.exec_price = safe_stod(price_str);
    if (item["execQty"].get_string().get(qty_str) == simdjson::SUCCESS) info.exec_qty = safe_stod(qty_str);
    if (item["execFee"].get_string().get(fee_str) == simdjson::SUCCESS) info.exec_fee = safe_stod(fee_str);
    if (item["feeCurrency"].get_string().get(fee_cur) == simdjson::SUCCESS) info.fee_currency = std::string(fee_cur);
    if (item["execTime"].get_string().get(time_str) == simdjson::SUCCESS) info.exec_time = std::stoull(std::string(time_str));
    if (item["side"].get_string().get(side_str) == simdjson::SUCCESS) info.side = std::string(side_str);

    info.order_id = order_id;
    info.symbol = symbol;
    info.is_valid = (info.exec_price > 0 && info.exec_qty > 0);
    return info;
}

std::vector<BybitApiClient::Execution> BybitApiClient::get_executions(
        const std::string& symbol, uint64_t start_time_ms, int limit) {
    std::vector<Execution> execs;
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"limit", std::to_string(limit)}
    };
    if (start_time_ms > 0) params["startTime"] = std::to_string(start_time_ms);

    std::string response = send_request("GET", "/v5/execution/list", params, true);
    if (response.empty()) return execs;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return execs;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return execs;

    auto list = doc["result"]["list"].get_array();
    if (list.error()) return execs;

    for (auto item : list.value()) {
        Execution e;
        std::string_view sym, side, oid, price, qty, fee, fee_cur, time;
        if (item["symbol"].get_string().get(sym) != simdjson::SUCCESS) continue;
        if (item["side"].get_string().get(side) != simdjson::SUCCESS) continue;
        if (item["orderId"].get_string().get(oid) != simdjson::SUCCESS) continue;
        if (item["execPrice"].get_string().get(price) != simdjson::SUCCESS) continue;
        if (item["execQty"].get_string().get(qty) != simdjson::SUCCESS) continue;
        if (item["execFee"].get_string().get(fee) != simdjson::SUCCESS) continue;
        if (item["feeCurrency"].get_string().get(fee_cur) != simdjson::SUCCESS) continue;
        if (item["execTime"].get_string().get(time) != simdjson::SUCCESS) continue;

        e.symbol       = std::string(sym);
        e.side         = std::string(side);
        e.order_id     = std::string(oid);
        e.exec_price   = safe_stod(price);
        e.exec_qty     = safe_stod(qty);
        e.exec_fee     = safe_stod(fee);
        e.fee_currency = std::string(fee_cur);
        e.exec_time    = std::stoull(std::string(time));

        std::string_view eid;
        if (item["execId"].get_string().get(eid) == simdjson::SUCCESS) e.exec_id = std::string(eid);
        if (e.exec_id.empty()) continue;

        bool is_maker = false;
        if (item["isMaker"].get_bool().get(is_maker) == simdjson::SUCCESS)
            e.exec_type = is_maker ? "Maker" : "Taker";
        else if (e.exec_fee != 0)
            e.exec_type = (e.exec_fee < 0) ? "Maker" : "Taker";
        else
            e.exec_type = "Unknown";

        e.is_valid = true;
        execs.push_back(e);
    }
    return execs;
}

// ============================================================
// Symbols
// ============================================================
std::vector<std::string> BybitApiClient::get_all_linear_symbols() {
    std::vector<std::string> symbols;
    std::map<std::string, std::string> params = {{"category", "linear"}, {"limit", "1000"}};
    std::string response = send_request("GET", "/v5/market/instruments-info", params, false);
    if (response.empty()) return symbols;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return symbols;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return symbols;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error()) return symbols;

    for (auto item : list_result.value()) {
        std::string_view sym, status;
        if (item["symbol"].get_string().get(sym) == simdjson::SUCCESS &&
            item["status"].get_string().get(status) == simdjson::SUCCESS &&
            status == "Trading") {
            symbols.push_back(std::string(sym));
        }
    }
    return symbols;
}

std::vector<std::string> BybitApiClient::get_all_spot_symbols() {
    std::vector<std::string> symbols;
    std::map<std::string, std::string> params = {{"category", "spot"}, {"limit", "1000"}};
    std::string response = send_request("GET", "/v5/market/instruments-info", params, false);
    if (response.empty()) return symbols;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return symbols;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return symbols;

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error()) return symbols;

    for (auto item : list_result.value()) {
        std::string_view sym, status;
        if (item["symbol"].get_string().get(sym) == simdjson::SUCCESS &&
            item["status"].get_string().get(status) == simdjson::SUCCESS &&
            status == "Trading") {
            symbols.push_back(std::string(sym));
        }
    }
    return symbols;
}

std::vector<std::string> BybitApiClient::get_top_liquid_symbols(int top_n) {
    std::vector<std::pair<std::string, double>> syms_with_turnover;
    std::map<std::string, std::string> params = {{"category", "linear"}, {"limit", "1000"}};
    std::string response = send_request("GET", "/v5/market/tickers", params, false);
    if (response.empty()) return {};

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return {};

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return {};

    auto list_result = doc["result"]["list"].get_array();
    if (list_result.error()) return {};

    for (auto item : list_result.value()) {
        std::string_view sym, turn_str;
        if (item["symbol"].get_string().get(sym) == simdjson::SUCCESS &&
            item["turnover24h"].get_string().get(turn_str) == simdjson::SUCCESS) {
            double turn = safe_stod(turn_str);
            if (turn > 1000000.0) syms_with_turnover.emplace_back(std::string(sym), turn);
        }
    }

    std::sort(syms_with_turnover.begin(), syms_with_turnover.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::string> result_syms;
    for (int i = 0; i < std::min(top_n, (int)syms_with_turnover.size()); ++i)
        result_syms.push_back(syms_with_turnover[i].first);
    return result_syms;
}

// ============================================================
// Misc
// ============================================================
std::string BybitApiClient::place_close_order(const std::string& symbol, const std::string& side,
                                              double price, double qty) {
    double np = normalize_price(symbol, price);
    double nq = normalize_qty(symbol, qty);

    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"side", side},
            {"orderType", "Limit"},
            {"qty", format_qty(symbol, nq)},
            {"price", format_price(symbol, np)},
            {"timeInForce", "IOC"},
            {"positionIdx", "0"},
            {"reduceOnly", "true"}
    };

    std::string response = send_request("POST", "/v5/order/create", params, true);
    if (response.empty()) return "";

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return "";

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return "";

    std::string_view oid;
    if (doc["result"]["orderId"].get_string().get(oid) == simdjson::SUCCESS)
        return std::string(oid);
    return "";
}

bool BybitApiClient::request_demo_funds(const std::string& coin) {
    std::map<std::string, std::string> params = {{"adjustType", "0"}};
    std::string response = send_request("POST", "/v5/account/wallet-balance", params, true);
    if (response.empty()) return false;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return false;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    if (doc["retCode"].get_int64().get(rc) != simdjson::SUCCESS || rc != 0) return false;
    return true;
}

bool BybitApiClient::set_trading_stop(const std::string& symbol, const std::string& side,
                                      double take_profit, double stop_loss, double position_size) {
    std::map<std::string, std::string> params = {
            {"category", "linear"},
            {"symbol", symbol},
            {"positionIdx", "0"},
            {"tpslMode", "Partial"}
    };

    if (take_profit > 0 && position_size > 0) {
        std::string tp = format_price(symbol, take_profit);
        params["takeProfit"] = tp;
        params["tpTriggerBy"] = "LastPrice";
        params["tpOrderType"] = "Limit";
        params["tpLimitPrice"] = tp;
        params["tpSize"] = format_qty(symbol, position_size);
    }
    if (stop_loss > 0 && position_size > 0) {
        std::string sl = format_price(symbol, stop_loss);
        params["stopLoss"] = sl;
        params["slTriggerBy"] = "LastPrice";
        params["slOrderType"] = "Limit";
        params["slLimitPrice"] = sl;
        params["slSize"] = format_qty(symbol, position_size);
    }

    std::string response = send_request("POST", "/v5/position/trading-stop", params, true);
    if (response.empty()) return false;

    simdjson::dom::parser parser;
    auto result = parser.parse(response);
    if (result.error()) return false;

    simdjson::dom::element doc = result.value();
    int64_t rc = 0;
    return doc["retCode"].get_int64().get(rc) == simdjson::SUCCESS && rc == 0;
}

// ============================================================
// Token bucket для /v5/position/list
// ============================================================
bool BybitApiClient::try_acquire_pos_info_token() {
    std::lock_guard<std::mutex> lk(rate_mtx_);
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - rate_last_refill_).count();

    rate_tokens_ = std::min(POS_INFO_RATE_BURST,
                            rate_tokens_ + elapsed * POS_INFO_RATE_PER_SEC);
    rate_last_refill_ = now;

    if (rate_tokens_ >= 1.0) {
        rate_tokens_ -= 1.0;
        return true;
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    if (now_ms - last_rate_warn_ms_ > 5'000) {
        last_rate_warn_ms_ = now_ms;
        std::cerr << "[RATE] get_position_info bucket empty — call skipped\n";
    }
    return false;
}