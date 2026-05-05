#pragma once

#include <string>
#include <ctime>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <vector>

#include "core/models/OrderData.h"

namespace core::services {

// Portable timegm — MSVC has _mkgmtime, POSIX has timegm.
inline std::time_t Timegm(struct tm* t) {
    t->tm_isdst = 0;
#ifdef _WIN32
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}

// Maps an IB order-status string to our OrderStatus enum.
inline ::core::OrderStatus ParseStatus(const std::string& s) {
    if (s == "Filled")                                                    return ::core::OrderStatus::Filled;
    if (s == "Cancelled" || s == "ApiCancelled" || s == "Inactive")      return ::core::OrderStatus::Cancelled;
    if (s == "Submitted"  || s == "PreSubmitted" || s == "ApiPending")   return ::core::OrderStatus::Working;
    if (s == "PartiallyFilled")                                           return ::core::OrderStatus::PartialFill;
    if (s == "Pending" || s == "PendingSubmit")                           return ::core::OrderStatus::Pending;
    if (s == "PendingCancel")                                             return ::core::OrderStatus::PendingCancel;
    if (s.empty())                                                        return ::core::OrderStatus::Pending;
    return ::core::OrderStatus::Rejected;
}

// Parses an IB timestamp string to a UNIX time_t.
// Accepts three formats:
//   "YYYYMMDD"             — daily/weekly/monthly bars; mapped to noon UTC so
//                            gmtime() always returns the correct calendar date.
//   "<unix_timestamp>"     — intraday bars with formatDate=2 (all digits, > 8 chars)
//   "YYYYMMDD HH:MM:SS"   — intraday bars with formatDate=1
inline std::time_t ParseIBTime(const std::string& ts) {
    if (ts.empty()) return 0;

    bool allDigits = true;
    for (char c : ts)
        if (!isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }

    if (allDigits) {
        if (ts.size() == 8) {
            // "YYYYMMDD" — daily/weekly/monthly bar. IB ignores formatDate=2 for
            // these and returns a date string, so we parse it ourselves.
            // We set the time to noon UTC so that any timezone ±12h offset still
            // maps back to the correct calendar date.
            struct tm t = {};
            std::istringstream(ts) >> std::get_time(&t, "%Y%m%d");
            t.tm_hour = 12; t.tm_min = t.tm_sec = 0;
            return Timegm(&t);
        }
        // Unix timestamp string (intraday bars with formatDate=2).
        return static_cast<std::time_t>(std::stoll(ts));
    }

    // "YYYYMMDD HH:MM:SS [TZ]" — intraday bars with formatDate=1.
    struct tm t = {};
    std::istringstream ss(ts);
    ss >> std::get_time(&t, "%Y%m%d %H:%M:%S");
    if (ss.fail()) {
        ss.clear();
        std::istringstream(ts) >> std::get_time(&t, "%Y%m%d");
    }
    t.tm_isdst = -1;
    return mktime(&t);
}

// ── Futures symbol detection ────────────────────────────────────────────────
// IB's reqMatchingSymbols only returns stocks, so the symbol-search layer
// needs a separate path for futures.  IsFuturesSymbol recognises common
// index / commodity / currency futures symbols, the "/" prefix convention
// (e.g. "/ES", "/NQ"), and contract-month-qualified forms
// ("NQ 202612", "/NQ:202612").

// Known CME / CBOT / NYMEX / COMEX futures base symbols.
inline bool IsFuturesBaseSymbol(const std::string& base) {
    static constexpr const char* kFuturesSyms[] = {
        "ES", "NQ", "YM", "RTY",   // equity indices
        "GC", "SI", "PL", "PA",    // precious metals
        "CL", "NG", "RB", "HO",    // energy
        "ZN", "ZB", "ZF", "ZT", "ZQ",  // treasuries
        "ZC", "ZW", "ZS", "ZM", "ZL",  // grains
        "HE", "LE",                // livestock
        "6E", "6J", "6B", "6A", "6C", "6S", "6M", "6N",  // FX
    };
    for (const auto* s : kFuturesSyms)
        if (base == s) return true;
    return false;
}

// Splits a futures symbol string into (base, contractMonth).
//   "/ES"          → base="ES",  contractMonth=""        (auto front-month)
//   "/NQ 202612"   → base="NQ",  contractMonth="202612"
//   "NQ:202612"    → base="NQ",  contractMonth="202612"
//   "ES 202606"    → base="ES",  contractMonth="202606"
//   "GC"           → base="GC",  contractMonth=""
inline void ParseFuturesSymbol(const std::string& sym,
                               std::string& base, std::string& contractMonth) {
    // Strip leading "/" if present.
    std::string s = (!sym.empty() && sym[0] == '/') ? sym.substr(1) : sym;
    // Split on trailing " YYYYMM" or ":YYYYMM".
    auto pos = s.find_last_of(" :");
    if (pos != std::string::npos && pos > 0 && pos + 7 == s.size()) {
        std::string suffix = s.substr(pos + 1);
        bool allDigits = suffix.size() == 6;
        for (char c : suffix)
            if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
        if (allDigits) {
            base          = s.substr(0, pos);
            contractMonth = suffix;
            return;
        }
    }
    base = s;
    contractMonth.clear();
}

inline bool IsFuturesSymbol(const std::string& sym) {
    if (sym.empty()) return false;
    // "/" prefix signals futures intent — require at least 2 chars so
    // a lone "/" doesn't match before the user has typed a symbol letter.
    if (sym[0] == '/' && sym.size() >= 2) return true;
    // Check if it's a plain base symbol or a qualified form like "NQ 202612".
    std::string base, contractMonth;
    ParseFuturesSymbol(sym, base, contractMonth);
    return IsFuturesBaseSymbol(base);
}

// Strips a leading "/" from a futures symbol (e.g. "/ES" → "ES").
// Returns the original string unchanged when there is no prefix.
inline std::string StripFuturesPrefix(const std::string& sym) {
    return (!sym.empty() && sym[0] == '/') ? sym.substr(1) : sym;
}

// Returns the front-month contract month ("YYYYMM") for standard quarterly
// futures (Mar / Jun / Sep / Dec cycle).  Uses a conservative 8-day-before-
// month-end roll assumption so we never pick a contract that has already
// expired according to IB's reference calendar.
inline std::string FuturesFrontMonth() {
    auto now = std::time(nullptr);
    std::tm* utc = std::gmtime(&now);
    int y = utc->tm_year + 1900;
    int m = utc->tm_mon + 1;

    // Months remaining in the current year's quarterly cycle.
    // Expiration is treated as the 22nd of the month (worst-case 3rd-Friday
    // is at most day 21, so by the 22nd even the latest-possible expiration
    // has passed).  This was picked to be safe across all quarterly futures
    // without needing per-product calendars.
    const int qmonths[] = {3, 6, 9, 12};
    for (int qm : qmonths) {
        if (m < qm || (m == qm && utc->tm_mday < 22)) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04d%02d", y, qm);
            return buf;
        }
    }
    // All four quarters of this year have expired → first quarter of next year.
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d%02d", y + 1, 3);
    return buf;
}

} // namespace core::services
