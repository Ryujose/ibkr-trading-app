#pragma once

#include <string>
#include <vector>
#include <ctime>

namespace core {

// ---- Scanner preset scans ---------------------------------------------------

enum class ScanPreset {
    TopGainers,       // highest % gain today
    TopLosers,        // highest % loss today
    VolumeLeaders,    // highest volume today
    NewHighs,         // stocks at 52-week high
    NewLows,          // stocks at 52-week low
    RSIOverbought,    // RSI >= 70
    RSIOversold,      // RSI <= 30
    NearEarnings,     // earnings within 5 trading days
    MostActive,       // highest dollar volume
    Custom            // user-defined filter set
};

// ---- Asset class tabs -------------------------------------------------------

enum class AssetClass {
    Stocks,
    Indexes,
    ETFs,
    Futures
};

// ---- Filter criteria --------------------------------------------------------

struct ScanFilter {
    // Price range
    double minPrice   =   0.0;
    double maxPrice   = 1e9;

    // % change range
    double minChangePct = -100.0;
    double maxChangePct =  100.0;

    // Volume
    double minVolume  = 0.0;        // in shares
    double maxVolume  = 1e12;

    // Market cap (millions USD)
    double minMktCapM = 0.0;
    double maxMktCapM = 1e9;

    // RSI
    double minRsi = 0.0;
    double maxRsi = 100.0;

    // Sector  (empty = all)
    std::string sector;

    // Exchange (empty = all)
    std::string exchange;          // NYSE, NASDAQ, AMEX, …
};

// ---- A single row returned from a scan --------------------------------------

struct ScanResult {
    // Identity
    std::string symbol;
    std::string company;
    std::string sector;
    std::string exchange;

    // Quote
    double price       = 0.0;
    double change      = 0.0;      // absolute $ change from prev close
    double changePct   = 0.0;      // % change
    double open        = 0.0;
    double high        = 0.0;
    double low         = 0.0;
    double prevClose   = 0.0;

    // Volume
    double volume      = 0.0;      // today's volume (shares)
    double avgVolume   = 0.0;      // 90-day average volume
    double relVolume   = 0.0;      // volume / avgVolume  (relative volume)

    // Market cap / fundamentals
    double mktCapM     = 0.0;      // market cap in millions USD
    double pe          = 0.0;      // trailing P/E  (0 if N/A)
    double eps         = 0.0;

    // 52-week range
    double high52      = 0.0;
    double low52       = 0.0;
    double pctFrom52H  = 0.0;      // % below 52-week high
    double pctFrom52L  = 0.0;      // % above 52-week low

    // Technicals
    double rsi         = 50.0;
    double macdLine    = 0.0;
    double macdSignal  = 0.0;
    double atr         = 0.0;      // average true range

    // Sparkline (last N closes for mini-chart, normalised to 0-1)
    std::vector<float> sparkline;  // typically 20 values

    // Meta
    std::time_t updatedAt = 0;
    bool        isIndex   = false; // draw differently
};

// ---- Column identifiers (used for sort state) --------------------------------

enum class ScanColumn {
    Symbol,
    Company,
    Price,
    Change,
    ChangePct,
    Volume,
    RelVolume,
    MktCap,
    PE,
    High52,
    Low52,
    PctFrom52H,
    RSI,
    MACD,
    ATR,
    Sparkline
};

// ---- String helpers ---------------------------------------------------------

inline const char* ScanPresetLabel(ScanPreset p) {
    switch (p) {
        case ScanPreset::TopGainers:    return "Top Gainers";
        case ScanPreset::TopLosers:     return "Top Losers";
        case ScanPreset::VolumeLeaders: return "Volume Leaders";
        case ScanPreset::NewHighs:      return "New 52-Wk Highs";
        case ScanPreset::NewLows:       return "New 52-Wk Lows";
        case ScanPreset::RSIOverbought: return "RSI Overbought (>=70)";
        case ScanPreset::RSIOversold:   return "RSI Oversold (<=30)";
        case ScanPreset::NearEarnings:  return "Near Earnings";
        case ScanPreset::MostActive:    return "Most Active";
        case ScanPreset::Custom:        return "Custom Scan";
        default:                        return "?";
    }
}

inline const char* AssetClassLabel(AssetClass a) {
    switch (a) {
        case AssetClass::Stocks:  return "Stocks";
        case AssetClass::Indexes: return "Indexes";
        case AssetClass::ETFs:    return "ETFs";
        case AssetClass::Futures: return "Futures";
        default:                  return "?";
    }
}

}  // namespace core
