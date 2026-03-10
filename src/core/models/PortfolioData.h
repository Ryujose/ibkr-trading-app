#pragma once

#include <string>
#include <vector>
#include <ctime>

namespace core {

// ---- Account-level values ---------------------------------------------------

struct AccountValues {
    // Equity / cash
    double netLiquidation   = 0.0;   // total account value
    double totalCashValue   = 0.0;
    double settledCash      = 0.0;

    // Margin
    double buyingPower      = 0.0;
    double initMarginReq    = 0.0;
    double maintMarginReq   = 0.0;
    double excessLiquidity  = 0.0;

    // P&L
    double unrealizedPnL    = 0.0;
    double realizedPnL      = 0.0;
    double dayPnL           = 0.0;
    double dayPnLPct        = 0.0;

    // Rates
    double grossPosValue    = 0.0;   // sum of abs(market value) of all positions
    double leverage         = 0.0;   // grossPosValue / netLiquidation

    // Account metadata
    std::string accountId;
    std::string accountType;         // INDIVIDUAL, ADVISOR, …
    std::string baseCurrency;
    std::time_t updatedAt   = 0;
};

// ---- A single open position -------------------------------------------------

struct Position {
    std::string symbol;
    std::string description;
    std::string assetClass;          // STK, OPT, FUT, CASH, …
    std::string exchange;
    std::string currency;

    double quantity      = 0.0;      // positive = long, negative = short
    double avgCost       = 0.0;      // average fill price (cost basis per share)
    double marketPrice   = 0.0;      // current bid/ask mid
    double marketValue   = 0.0;      // quantity * marketPrice
    double costBasis     = 0.0;      // quantity * avgCost

    double unrealizedPnL = 0.0;      // marketValue - costBasis
    double unrealizedPct = 0.0;      // unrealizedPnL / |costBasis|
    double realizedPnL   = 0.0;      // cumulative realized for this symbol

    double dayChange     = 0.0;      // price change today
    double dayChangePct  = 0.0;

    // Weight in portfolio (computed after all positions are loaded)
    double portfolioWeight = 0.0;    // |marketValue| / netLiquidation

    std::time_t updatedAt = 0;
};

// ---- Closed trade record (trade history) ------------------------------------

struct TradeRecord {
    int         tradeId     = 0;
    std::string symbol;
    std::string side;                // "BUY" / "SELL"
    double      quantity    = 0.0;
    double      price       = 0.0;
    double      commission  = 0.0;
    double      realizedPnL = 0.0;
    std::time_t executedAt  = 0;
};

// ---- Daily equity snapshot (for equity curve) --------------------------------

struct EquityPoint {
    std::time_t date       = 0;
    double      equity     = 0.0;
    double      cash       = 0.0;
    double      positions  = 0.0;    // mark-to-market of positions
};

// ---- Performance metrics ----------------------------------------------------

struct PerformanceMetrics {
    double totalReturn      = 0.0;   // % from inception
    double ytdReturn        = 0.0;
    double mtdReturn        = 0.0;
    double dayReturn        = 0.0;

    double sharpeRatio      = 0.0;   // annualized, risk-free = 5%
    double maxDrawdown      = 0.0;   // worst peak-to-trough %
    double winRate          = 0.0;   // % of closed trades that were profitable
    double avgWin           = 0.0;   // avg $ profit on winning trades
    double avgLoss          = 0.0;   // avg $ loss on losing trades
    double profitFactor     = 0.0;   // grossWin / |grossLoss|
    double beta             = 0.0;   // vs S&P 500
    double alpha            = 0.0;   // annualized
    double volatility       = 0.0;   // annualized daily return std-dev
};

// ---- Sort columns for position table ----------------------------------------

enum class PositionColumn {
    Symbol,
    Description,
    Quantity,
    AvgCost,
    Price,
    MarketValue,
    CostBasis,
    UnrealizedPnL,
    UnrealizedPct,
    RealizedPnL,
    DayChange,
    DayChangePct,
    Weight,
};

}  // namespace core
