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
    W1  = 7,
    MN  = 8,
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
        case Timeframe::W1:  return "1W";
        case Timeframe::MN:  return "1M";
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
        case Timeframe::W1:  return 604800;
        case Timeframe::MN:  return 2592000;
        default:             return 86400;
    }
}

// IB API bar size string for each timeframe
inline const char* TimeframeIBBarSize(Timeframe tf) {
    switch (tf) {
        case Timeframe::M1:  return "1 min";
        case Timeframe::M5:  return "5 mins";
        case Timeframe::M15: return "15 mins";
        case Timeframe::M30: return "30 mins";
        case Timeframe::H1:  return "1 hour";
        case Timeframe::H4:  return "4 hours";
        case Timeframe::D1:  return "1 day";
        case Timeframe::W1:  return "1 week";
        case Timeframe::MN:  return "1 month";
        default:             return "1 day";
    }
}

// IB API duration string (how far back to fetch)
inline const char* TimeframeIBDuration(Timeframe tf) {
    switch (tf) {
        case Timeframe::M1:  return "1 D";
        case Timeframe::M5:  return "5 D";
        case Timeframe::M15: return "10 D";
        case Timeframe::M30: return "20 D";
        case Timeframe::H1:  return "30 D";
        case Timeframe::H4:  return "60 D";
        case Timeframe::D1:  return "6 M";
        case Timeframe::W1:  return "2 Y";
        case Timeframe::MN:  return "5 Y";
        default:             return "6 M";
    }
}

// ---- Trading session classification -----------------------------------------
// Classifies a UTC timestamp into US market session using DST-aware ET offset.
// EDT (UTC-4): 2nd Sunday of March → 1st Sunday of November
// EST (UTC-5): otherwise
enum class Session { Regular, PreMarket, AfterHours, Overnight };

inline bool IsUSDST(std::time_t ts) {
    // US DST: 2nd Sunday of March (02:00) → 1st Sunday of November (02:00)
    std::tm* u = std::gmtime(&ts);
    if (!u) return false;
    int month = u->tm_mon + 1;
    if (month < 3 || month > 11) return false;
    if (month > 3 && month < 11) return true;
    int day  = u->tm_mday;
    int wday = u->tm_wday;  // 0 = Sunday
    // Derive weekday of the 1st of the current month
    int wday1 = ((wday - (day - 1)) % 7 + 7) % 7;
    if (month == 3) {
        // DST starts on 2nd Sunday (on or after March 8) at 02:00
        int dstDay = 8 + ((7 - wday1) % 7);
        return (day > dstDay) || (day == dstDay && u->tm_hour >= 2);
    } else {  // month == 11
        // DST ends on 1st Sunday at 02:00
        int dstDay = 1 + ((7 - wday1) % 7);
        return (day < dstDay) || (day == dstDay && u->tm_hour < 2);
    }
}

inline Session BarSession(std::time_t ts) {
    // DST-aware: EDT = UTC-4 (Mar 2nd Sun → Nov 1st Sun), EST = UTC-5 otherwise
    int offsetSec = IsUSDST(ts) ? 4 * 3600 : 5 * 3600;
    std::time_t et = ts - offsetSec;
    std::tm* tm = std::gmtime(&et);
    if (!tm) return Session::Regular;
    int hhmm = tm->tm_hour * 100 + tm->tm_min;
    if      (hhmm <  400) return Session::Overnight;
    else if (hhmm <  930) return Session::PreMarket;
    else if (hhmm < 1600) return Session::Regular;
    else if (hhmm < 2000) return Session::AfterHours;
    else                  return Session::Overnight;
}

struct BarSeries {
    std::string   symbol;
    Timeframe     timeframe  = Timeframe::D1;
    std::vector<Bar> bars;

    bool empty() const { return bars.empty(); }
    int  size()  const { return (int)bars.size(); }
};

}  // namespace core
