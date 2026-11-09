-- Создание базы данных (если ещё не существует)
CREATE DATABASE trading_signals;

-- Подключаемся к базе trading_signals
\c trading_signals

-- Удаляем старые таблицы (если они есть)
DROP TABLE IF EXISTS trade_tracking CASCADE;
DROP TABLE IF EXISTS signal_analysis CASCADE;
DROP TABLE IF EXISTS signals CASCADE;

-- ============================================================
-- Таблица signals: хранение всех сгенерированных сигналов
-- ============================================================
CREATE TABLE signals (
    id SERIAL PRIMARY KEY,
    symbol VARCHAR(20) NOT NULL,
    signal_type VARCHAR(10) NOT NULL CHECK (signal_type IN ('LONG', 'SHORT')),
    entry_price NUMERIC(20, 8) NOT NULL,
    stop_loss NUMERIC(20, 8) NOT NULL,
    take_profit_1 NUMERIC(20, 8) NOT NULL,
    take_profit_2 NUMERIC(20, 8),
    confidence NUMERIC(5, 4) NOT NULL,
    volatility NUMERIC(20, 8),
    absorption_volume NUMERIC(20, 8),
    at_extreme BOOLEAN DEFAULT FALSE,
    absorption_present BOOLEAN DEFAULT FALSE,
    delta_weakening BOOLEAN DEFAULT FALSE,
    in_range BOOLEAN DEFAULT FALSE,
    created_at TIMESTAMPTZ DEFAULT NOW(),
    reason TEXT
);

-- ============================================================
-- Таблица trade_tracking: отслеживание открытых и закрытых сделок
-- ============================================================
CREATE TABLE trade_tracking (
    id INTEGER PRIMARY KEY,  -- управляется вручную из TradeTracker
    signal_id INTEGER REFERENCES signals(id),
    symbol VARCHAR(20) NOT NULL,
    signal_type VARCHAR(10) NOT NULL CHECK (signal_type IN ('LONG', 'SHORT')),
    entry_price NUMERIC(20, 8) NOT NULL,
    stop_loss NUMERIC(20, 8) NOT NULL,
    take_profit_1 NUMERIC(20, 8) NOT NULL,
    take_profit_2 NUMERIC(20, 8),
    entry_time BIGINT NOT NULL,               -- timestamp в миллисекундах
    confidence NUMERIC(5, 4),
    volatility NUMERIC(20, 8),
    absorption_volume NUMERIC(20, 8),
    status VARCHAR(20) NOT NULL DEFAULT 'OPEN',  -- OPEN, CLOSED, ORPHANED
    entry_order_id VARCHAR(64),
    position_size_usdt NUMERIC(20, 4),
    liquidity_depth NUMERIC(20, 4),
    liquidity_volume_60s NUMERIC(20, 4),
    liquidity_spread NUMERIC(10, 4),
    liquidity_avg_trade_size NUMERIC(20, 4),
    initial_sl_distance NUMERIC(10, 6),
    exit_price NUMERIC(20, 8),
    exit_time BIGINT,
    exit_reason VARCHAR(50),
    pnl_percent NUMERIC(10, 4),
    max_favorable_excursion NUMERIC(10, 4),
    max_adverse_excursion NUMERIC(10, 4),
    actual_entry_price NUMERIC(20, 8),
    actual_exit_price NUMERIC(20, 8),
    entry_commission NUMERIC(20, 8) DEFAULT 0,
    exit_commission NUMERIC(20, 8) DEFAULT 0,
    net_pnl NUMERIC(20, 8),
    trailing_stop_updates INTEGER DEFAULT 0,
    final_stop_loss NUMERIC(20, 8),
    exit_order_id VARCHAR(64)
);

-- ============================================================
-- Таблица signal_analysis: анализ принятых/отклонённых сигналов
-- ============================================================
CREATE TABLE signal_analysis (
    id SERIAL PRIMARY KEY,
    signal_id INTEGER,                     -- может быть NULL, если сигнал не записан в signals
    symbol VARCHAR(20) NOT NULL,
    signal_type VARCHAR(10),
    confidence NUMERIC(5, 4),
    signal_time BIGINT,
    was_executed BOOLEAN DEFAULT FALSE,
    not_executed_reason VARCHAR(50),
    filtered_by_confidence BOOLEAN DEFAULT FALSE,
    filtered_by_rr BOOLEAN DEFAULT FALSE,
    filtered_by_liquidity BOOLEAN DEFAULT FALSE,
    filtered_by_max_positions BOOLEAN DEFAULT FALSE,
    filtered_by_duplicate BOOLEAN DEFAULT FALSE,
    trade_id INTEGER,
    created_at TIMESTAMPTZ DEFAULT NOW()
);

-- Индексы для ускорения запросов
CREATE INDEX idx_signals_symbol_time ON signals(symbol, created_at);
CREATE INDEX idx_trade_tracking_status_time ON trade_tracking(status, exit_time);
CREATE INDEX idx_trade_tracking_symbol_time ON trade_tracking(symbol, entry_time);
CREATE INDEX idx_signal_analysis_symbol_time ON signal_analysis(symbol, created_at);