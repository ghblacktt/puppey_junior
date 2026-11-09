#include <iostream>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <random>
#include <simdjson.h>
#include <curl/curl.h>
#include <cstdio>
#include <array>
#include <unordered_set>
#include "bitunix_api_client.h"
// ============================================================
// SHA256
// ============================================================
std::string BitunixApiClient::sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()),
           input.length(), digest);
    std::stringstream ss;
    for (unsigned char c : digest) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    }
    return ss.str();
}

std::string BitunixApiClient::generate_nonce() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::string nonce(32, '0');
    for (auto& c : nonce) c = hex[dis(gen)];
    return nonce;
}

std::string BitunixApiClient::now_ms_str() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    return std::to_string(ms);
}

std::string BitunixApiClient::now_sec_str() {
    auto now = std::chrono::system_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
    return std::to_string(sec);
}

// ============================================================
// Подпись: sign = SHA256(SHA256(nonce + timestamp + apiKey + queryParams + body) + secretKey)
// ============================================================
std::string BitunixApiClient::generate_signature(
        const std::string& nonce,
        const std::string& timestamp,
        const std::string& query_params,
        const std::string& body) {
    std::string digest_input = nonce + timestamp + api_key_ + query_params + body;
    std::string digest = sha256_hex(digest_input);
    return sha256_hex(digest + api_secret_);
}

// ============================================================
// Конструктор
// ============================================================
BitunixApiClient::BitunixApiClient(const std::string& api_key,
                                   const std::string& api_secret)
        : api_key_(api_key), api_secret_(api_secret),
          ctx_(ssl::context::tlsv12_client) {
    ctx_.set_default_verify_paths();
}

BitunixApiClient::~BitunixApiClient() {
    close_connection();
}

// ============================================================
// HTTP connection
// ============================================================
bool BitunixApiClient::ensure_connection() {
    if (http_stream_) {
        auto& sock = beast::get_lowest_layer(*http_stream_).socket();
        if (sock.is_open()) return true;
    }

    try {
        http_stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc_, ctx_);

        if (!SSL_set_tlsext_host_name(http_stream_->native_handle(), host_.c_str())) {
            std::cerr << "[BitunixAPI] SNI error\n";
            http_stream_.reset();
            return false;
        }

// ── Force HTTP/1.1 (Cloudflare иначе хочет h2) ──
        static const unsigned char kAlpnHttp11[] = {
                0x08, 'h','t','t','p','/','1','.','1'
        };
        if (SSL_set_alpn_protos(http_stream_->native_handle(),
                                kAlpnHttp11, sizeof(kAlpnHttp11)) != 0) {
            std::cerr << "[BitunixAPI] ALPN set failed\n";
            http_stream_.reset();
            return false;
        }

        const unsigned char* alpn_sel = nullptr;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(http_stream_->native_handle(), &alpn_sel, &alpn_len);
        if (alpn_sel && alpn_len > 0) {
            std::cout << "[BitunixAPI] ALPN selected: "
                      << std::string(reinterpret_cast<const char*>(alpn_sel), alpn_len) << "\n";
        } else {
            std::cout << "[BitunixAPI] ALPN: none\n";
        }

        if (cached_endpoints_.empty()) {
            std::cout << "[BitunixAPI] resolve " << host_ << ":" << port_ << " ...\n";
            tcp::resolver resolver{ioc_};
            boost::system::error_code rec;
            auto results = resolver.resolve(host_, port_, rec);
            if (rec) {
                std::cerr << "[BitunixAPI] resolve failed: " << rec.message() << "\n";
                http_stream_.reset();
                return false;
            }
            for (const auto& r : results) cached_endpoints_.push_back(r.endpoint());
            std::cout << "[BitunixAPI] resolved OK, n=" << cached_endpoints_.size() << "\n";
        }

        std::cout << "[BitunixAPI] connect ...\n";
        beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
        beast::get_lowest_layer(*http_stream_).connect(cached_endpoints_);

        std::cout << "[BitunixAPI] SSL handshake ...\n";
        beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
        http_stream_->handshake(ssl::stream_base::client);

        std::cout << "[BitunixAPI] handshake OK\n";

        beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
        beast::get_lowest_layer(*http_stream_).connect(cached_endpoints_);

        beast::get_lowest_layer(*http_stream_).expires_after(std::chrono::seconds(5));
        http_stream_->handshake(ssl::stream_base::client);

        beast::get_lowest_layer(*http_stream_).socket().set_option(tcp::no_delay(true));
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BitunixAPI] Connect failed: " << e.what() << "\n";
        http_stream_.reset();
        cached_endpoints_.clear();
        return false;
    }
}

void BitunixApiClient::close_connection() {
    if (http_stream_) {
        beast::error_code ec;
        http_stream_->shutdown(ec);
        http_stream_.reset();
    }
}

// ============================================================
// Универсальный HTTP-запрос
// ============================================================

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string BitunixApiClient::send_request(
        const std::string& method,
        const std::string& endpoint,
        const std::string& query_params,
        const std::string& body,
        bool authenticated) {

    std::lock_guard<std::mutex> lock(io_mutex_);

    CURL* curl = curl_easy_init();
    if (!curl) return "";
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

// ── форсим IPv4 (у Bitunix нет IPv6-адресов в DNS) ──
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    std::string url = "https://" + host_ + endpoint;
    if (!query_params.empty()) url += "?" + query_params;

    std::string response;
    struct curl_slist* headers = nullptr;

    if (!body.empty()) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    if (authenticated) {
        std::string nonce     = generate_nonce();
        std::string timestamp = now_ms_str();
        std::string sign      = generate_signature(nonce, timestamp, query_params, body);

        headers = curl_slist_append(headers, ("api-key: "   + api_key_).c_str());
        headers = curl_slist_append(headers, ("nonce: "     + nonce).c_str());
        headers = curl_slist_append(headers, ("timestamp: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("sign: "      + sign).c_str());
        headers = curl_slist_append(headers, "language: en-US");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1); // форсим HTTP/1.1
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "[BitunixAPI] curl error: " << curl_easy_strerror(res) << "\n";
        response.clear();
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        std::cout << "[BitunixAPI] HTTP " << http_code
                  << " bytes=" << response.size() << "\n";
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

// ============================================================
// Place limit order
// ============================================================
std::string BitunixApiClient::place_limit_order(
        const std::string& symbol,
        const std::string& side,
        double price, double qty,
        const std::string& effect) {

    std::ostringstream body_ss;
    body_ss << R"({"symbol":")" << symbol
            << R"(","side":")" << side
            << R"(","orderType":"LIMIT","qty":")" << std::fixed << std::setprecision(8) << qty
            << R"(","price":")" << std::fixed << std::setprecision(8) << price
            << R"(","effect":")" << effect << R"("})";

    std::string body = body_ss.str();

    std::string response = send_request(
            "POST", "/api/v1/futures/trade/place_order", "", body, true);

    if (response.empty()) {
        last_ret_code_ = -1;
        last_ret_msg_ = "empty response";
        return "";
    }

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) {
        last_ret_code_ = -2;
        last_ret_msg_ = "parse error";
        return "";
    }

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    if (doc["code"].get_int64().get(code) != simdjson::SUCCESS || code != 0) {
        last_ret_code_ = (int)code;
        std::string_view msg;
        if (doc["msg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg_ = std::string(msg);
        return "";
    }

    std::string_view order_id;
    if (doc["data"]["orderId"].get_string().get(order_id) == simdjson::SUCCESS)
        return std::string(order_id);

    return "";
}

// ============================================================
// Place market order
// ============================================================
std::string BitunixApiClient::place_market_order(
        const std::string& symbol,
        const std::string& side,
        double qty) {

    std::ostringstream body_ss;
    body_ss << R"({"symbol":")" << symbol
            << R"(","side":")" << side
            << R"(","orderType":"MARKET","qty":")" << std::fixed << std::setprecision(8) << qty
            << R"(","tradeSide":"OPEN"})";

    std::string body = body_ss.str();

    std::string response = send_request(
            "POST", "/api/v1/futures/trade/place_order", "", body, true);

    if (response.empty()) {
        last_ret_code_ = -1;
        last_ret_msg_ = "empty response";
        return "";
    }

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) {
        last_ret_code_ = -2;
        last_ret_msg_ = "parse error";
        return "";
    }

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    if (doc["code"].get_int64().get(code) != simdjson::SUCCESS || code != 0) {
        last_ret_code_ = (int)code;
        std::string_view msg;
        if (doc["msg"].get_string().get(msg) == simdjson::SUCCESS)
            last_ret_msg_ = std::string(msg);
        return "";
    }

    std::string_view order_id;
    if (doc["data"]["orderId"].get_string().get(order_id) == simdjson::SUCCESS)
        return std::string(order_id);

    return "";
}

// ============================================================
// Cancel order
// ============================================================
bool BitunixApiClient::cancel_order(const std::string& symbol,
                                    const std::string& order_id) {
    std::ostringstream body_ss;
    body_ss << R"({"symbol":")" << symbol
            << R"(","orderId":")" << order_id << R"("})";

    std::string body = body_ss.str();
    std::string response = send_request(
            "POST", "/api/v1/futures/trade/cancel_orders", "", body, true);

    if (response.empty()) return false;

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) return false;

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    return doc["code"].get_int64().get(code) == simdjson::SUCCESS && code == 0;
}

bool BitunixApiClient::cancel_all_orders(const std::string& symbol) {
    std::ostringstream body_ss;
    body_ss << R"({"symbol":")" << symbol << R"("})";

    std::string body = body_ss.str();
    std::string response = send_request(
            "POST", "/api/v1/futures/trade/cancel_all_orders", "", body, true);

    if (response.empty()) return false;

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) return false;

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    return doc["code"].get_int64().get(code) == simdjson::SUCCESS && code == 0;
}

// ============================================================
// Position info
// ============================================================
BitunixApiClient::PositionInfo BitunixApiClient::get_position_info(
        const std::string& symbol) {
    PositionInfo info;
    info.symbol = symbol;

    std::string query = "symbol=" + symbol;
    std::string response = send_request(
            "GET", "/api/v1/futures/position/get_pending_positions", query, "", true);

    if (response.empty()) return info;

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) return info;

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    if (doc["code"].get_int64().get(code) != simdjson::SUCCESS || code != 0) return info;

    simdjson::dom::array arr;
    if (doc["data"].get_array().get(arr) != simdjson::SUCCESS || arr.size() == 0) return info;

    simdjson::dom::element pos = arr.at(0);

    std::string_view side, size_str, avg_str, pnl_str;
    if (pos["side"].get_string().get(side) == simdjson::SUCCESS) info.side = std::string(side);
    if (pos["qty"].get_string().get(size_str) == simdjson::SUCCESS)
        info.size = std::stod(std::string(size_str));
    if (pos["avgPrice"].get_string().get(avg_str) == simdjson::SUCCESS)
        info.avg_price = std::stod(std::string(avg_str));
    if (pos["unrealizedPNL"].get_string().get(pnl_str) == simdjson::SUCCESS)
        info.unrealized_pnl = std::stod(std::string(pnl_str));

    info.is_valid = (info.size > 0 && !info.side.empty());
    return info;
}


static const std::string& curl_bin() {
    static const std::string path = []() {
        const char* candidates[] = {
                "/opt/homebrew/bin/curl",
                "/usr/local/bin/curl",
                "/usr/bin/curl",
        };
        for (auto p : candidates) {
            if (access(p, X_OK) == 0) return std::string(p);
        }
        return std::string("curl");   // fallback на PATH
    }();
    return path;
}

static std::string popen_curl(const std::string& url, int timeout_sec = 30) {
    std::string cmd = curl_bin()
                      + " -sS -L --compressed"
                      + " -H 'Accept: application/json'"
                      + " -A 'HFT-Cpp-Client/1.0'"
                      + " --max-time " + std::to_string(timeout_sec)
                      + " '" + url + "'";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::array<char, 8192> buf;
    std::string out;
    while (fgets(buf.data(), buf.size(), pipe) != nullptr) out += buf.data();
    int rc = pclose(pipe);
    int exit_code = WEXITSTATUS(rc);   // ← декодируем
    if (exit_code != 0) {
        std::cerr << "[BitunixAPI] curl exit code=" << exit_code
                  << " (28=timeout, 6=DNS, 7=conn, 56=recv)\n";
        return "";
    }
    return out;
}

std::vector<std::string> BitunixApiClient::get_trading_pairs() {
    std::vector<std::string> out;

    std::cout << "[BitunixAPI] curl binary: " << curl_bin() << "\n";
    std::string response = popen_curl(
            "https://fapi.bitunix.com/api/v1/futures/market/trading_pairs", 15);

    if (response.empty()) {
        std::cerr << "[BitunixAPI] popen: empty response\n";
        return out;
    }
    std::cout << "[BitunixAPI] popen: " << response.size() << " bytes\n";

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) {
        std::cerr << "[BitunixAPI] popen parse error\n";
        return out;
    }

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    if (doc["code"].get_int64().get(code) != simdjson::SUCCESS || code != 0) {
        std::cerr << "[BitunixAPI] popen code=" << code << "\n";
        return out;
    }

    simdjson::dom::element data_el;
    if (doc["data"].get(data_el) != simdjson::SUCCESS) return out;

    simdjson::dom::array arr;
    if (data_el.get_array().get(arr) != simdjson::SUCCESS) {
        if (data_el["list"].get_array().get(arr) != simdjson::SUCCESS) {
            std::cerr << "[BitunixAPI] popen: unexpected data format\n";
            return out;
        }
    }

    for (auto item : arr) {
        std::string_view symbol;
        if (item["symbol"].get_string().get(symbol) == simdjson::SUCCESS) {
            out.emplace_back(symbol);
        } else if (item["symbolName"].get_string().get(symbol) == simdjson::SUCCESS) {
            out.emplace_back(symbol);
        }
    }

    std::cout << "[BitunixAPI] loaded " << out.size()
              << " trading pairs (via popen curl)\n";
    return out;
}

std::vector<std::string> BitunixApiClient::get_available_symbols(
        const std::vector<std::string>& candidates) {
    std::vector<std::string> out;
    if (candidates.empty()) return out;

    const std::string url =
            "https://fapi.bitunix.com/api/v1/futures/market/tickers";

    std::cout << "[BitunixAPI] tickers: fetching ALL (no filter)...\n";
    std::string response = popen_curl(url, 120);

    if (response.empty()) {
        std::cerr << "[BitunixAPI] tickers: empty response\n";
        return out;
    }
    std::cout << "[BitunixAPI] tickers: " << response.size() << " bytes\n";

    simdjson::dom::parser parser;
    auto r = parser.parse(response);
    if (r.error()) {
        std::cerr << "[BitunixAPI] tickers: parse error\n";
        return out;
    }

    simdjson::dom::element doc = r.value();
    int64_t code = 0;
    if (doc["code"].get_int64().get(code) != simdjson::SUCCESS || code != 0) {
        std::string_view msg;
        if (doc["msg"].get_string().get(msg) == simdjson::SUCCESS)
            std::cerr << "[BitunixAPI] tickers code=" << code
                      << " msg=" << msg << "\n";
        return out;
    }

    simdjson::dom::array arr;
    if (doc["data"].get_array().get(arr) != simdjson::SUCCESS) {
        std::cerr << "[BitunixAPI] tickers: no data array\n";
        return out;
    }

    std::unordered_set<std::string> available;
    available.reserve(arr.size());
    for (auto item : arr) {
        std::string_view symbol;
        if (item["symbol"].get_string().get(symbol) == simdjson::SUCCESS)
            available.emplace(symbol);
    }
    std::cout << "[BitunixAPI] tickers: " << available.size()
              << " available on Bitunix\n";

    for (const auto& sym : candidates) {
        if (available.count(sym)) out.push_back(sym);
    }

    std::cout << "[BitunixAPI] tickers: overlap " << out.size()
              << "/" << candidates.size() << "\n";
    return out;
}