cmake_minimum_required(VERSION 3.16)
project(BybitHFTClient)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
# ── Homebrew curl (SDK libcurl на macOS 26 не работает с CF по HTTP/1.1) ──
set(CURL_HOMEBREW "/opt/homebrew/opt/curl")
if(NOT EXISTS "${CURL_HOMEBREW}/lib/libcurl.dylib")
    message(FATAL_ERROR "brew install curl  (looking for ${CURL_HOMEBREW}/lib/libcurl.dylib)")
endif()
message(STATUS "Using Homebrew curl: ${CURL_HOMEBREW}")
include(FetchContent)

FetchContent_Declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v3.9.3
)
FetchContent_MakeAvailable(simdjson)

FetchContent_Declare(
        clickhouse_cpp
        GIT_REPOSITORY https://github.com/ClickHouse/clickhouse-cpp.git
        GIT_TAG v2.5.1
)
set(WITH_OPENSSL ON CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ABSEIL OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_CITYHASH OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_LZ4 OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(clickhouse_cpp)

set(PQXX_INCLUDE_DIR "/opt/homebrew/include")
set(PQXX_LIB_DIR "/opt/homebrew/lib")

# ═══════════════════════════════════════════════════════════════
# Main trading binary (Executor + Scanner + Bitunix follower)
# ═══════════════════════════════════════════════════════════════
add_executable(bybit_client
        src/main.cpp
        src/bybit/bybit_ws.cpp
        src/order_book.cpp
        src/cluster_engine.cpp
        src/time_window_vwma.cpp
        src/dynamic_bb_engine.cpp
        src/mean_reversion_strategy.cpp
        src/signal_logger.cpp
        src/ticker_state.h
        src/trade_tracker.cpp
        src/bar_aggregator.cpp
        src/regime_filter.cpp
        src/bybit/bybit_api_client.cpp
        src/order_executor.cpp
        src/risk_manager.h
        src/market_making_strategy.h
        src/market_making_strategy.cpp
        src/bybit/bybit_private_ws.cpp
        src/triangle_arbitrage.cpp
        src/triangle_arbitrage.h
        src/funding_arb_strategy.cpp
        src/funding_arb_strategy.h
        src/paper_exchange.cpp
        src/bybit/bybit_trade_ws.cpp
        src/mm_symbol_registry.cpp
        src/screener.cpp
        src/scanner_client.cpp
        src/slow_loop_supervisor.cpp
        src/util/console.h
        # ── Bitunix (follower exchange) ──
        src/bitunix/bitunix_api_client.cpp
        src/bitunix/bitunix_private_ws.cpp
        src/cross_venue_signal.cpp
        src/bitunix/bitunix_public_ws.cpp
        src/ch/ch_writer.cpp

        src/binance/binance_public_ws.cpp
        src/true_price/true_price_engine.h
        src/symbol_regime.cpp
)

target_include_directories(bybit_client PRIVATE
        ${Boost_INCLUDE_DIRS}
        ${PQXX_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/bitunix
        "${CURL_HOMEBREW}/include"
)

target_link_libraries(bybit_client PRIVATE
        Boost::boost
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        simdjson
        pqxx
        curl
        clickhouse-cpp-lib
        /opt/homebrew/opt/libpq/lib/libpq.5.dylib
)

add_executable(tp_shadow
        src/tp_shadow/main.cpp
)

target_include_directories(tp_shadow PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/true_price/true_price
)

target_link_libraries(tp_shadow PRIVATE
        clickhouse-cpp-lib
        Boost::boost
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

target_link_directories(bybit_client PRIVATE
        ${PQXX_LIB_DIR}
        "${CURL_HOMEBREW}/lib"
)

# ═══════════════════════════════════════════════════════════════
# Backtest framework (native clickhouse-cpp)
# ═══════════════════════════════════════════════════════════════

add_library(bt_core
        src/backtest/book_rebuilder.cpp
        src/backtest/data_loader.cpp
        src/backtest/matching_engine.cpp
        src/backtest/backtest_exchange.cpp
        src/backtest/backtest_runner.cpp
        src/order_book.cpp
        src/market_making_strategy.cpp
        src/cluster_engine.cpp
        src/bar_aggregator.cpp
        src/regime_filter.cpp
        src/time_window_vwma.cpp
        src/dynamic_bb_engine.cpp
        src/cross_venue_signal.cpp
        src/symbol_regime.cpp
)

target_include_directories(bt_core PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/backtest
        ${Boost_INCLUDE_DIRS}
        ${PQXX_INCLUDE_DIR}
)

target_link_libraries(bt_core PUBLIC
        clickhouse-cpp-lib
        Boost::boost
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        simdjson
        pqxx
        /opt/homebrew/opt/libpq/lib/libpq.5.dylib
)

target_link_directories(bt_core PUBLIC
        ${PQXX_LIB_DIR}
)

add_executable(bt_backtest
        src/backtest/bt_backtest.cpp
        src/bybit/bybit_api_client.cpp
)
target_link_libraries(bt_backtest PRIVATE bt_core)

if(APPLE)
    target_link_options(bt_backtest PRIVATE
            -Wl,-all_load
            -Wl,-no_warn_duplicate_libraries
    )
endif()

add_executable(bt_test_load src/backtest/test_load.cpp)
target_link_libraries(bt_test_load PRIVATE bt_core)

if(APPLE)
    target_link_options(bt_test_load PRIVATE
            -Wl,-all_load
            -Wl,-no_warn_duplicate_libraries
    )
endif()