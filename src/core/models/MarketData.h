#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <ctime>

namespace core {

// One OHLCV bar (any timeframe)
struct Bar {
    double timestamp;  // UNIX timestamp (seconds) — used as x-axis value
    double open;
    double high;
    double low;
    double close;
    double volume;
};

enum class Timeframe : int {
    M1  = 0,
    M5  = 1,
    M15 = 2,
    M30 = 3,
    H1  = 4,
    H4  = 5,
    D1  = 6,
};

inline const char* TimeframeLabel(Timeframe tf) {
    switch (tf) {
        case Timeframe::M1:  return "1m";
        case Timeframe::M5:  return "5m";
        case Timeframe::M15: return "15m";
        case Timeframe::M30: return "30m";
        case Timeframe::H1:  return "1h";
        case Timeframe::H4:  return "4h";
        case Timeframe::D1:  return "1D";
        default:             return "?";
    }
}

// Seconds per bar for each timeframe
inline int64_t TimeframeSeconds(Timeframe tf) {
    switch (tf) {
        case Timeframe::M1:  return 60;
        case Timeframe::M5:  return 300;
        case Timeframe::M15: return 900;
        case Timeframe::M30: return 1800;
        case Timeframe::H1:  return 3600;
        case Timeframe::H4:  return 14400;
        case Timeframe::D1:  return 86400;
        default:             return 86400;
    }
}

struct BarSeries {
    std::string   symbol;
    Timeframe     timeframe  = Timeframe::D1;
    std::vector<Bar> bars;

    bool empty() const { return bars.empty(); }
    int  size()  const { return (int)bars.size(); }
};

}  // namespace core
