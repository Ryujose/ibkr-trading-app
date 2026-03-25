#pragma once

#include <string>
#include <ctime>
#include <cctype>
#include <sstream>
#include <iomanip>

#include "core/models/OrderData.h"

namespace core::services {

// Maps an IB order-status string to our OrderStatus enum.
inline ::core::OrderStatus ParseStatus(const std::string& s) {
    if (s == "Filled")                                                    return ::core::OrderStatus::Filled;
    if (s == "Cancelled" || s == "ApiCancelled" || s == "Inactive")      return ::core::OrderStatus::Cancelled;
    if (s == "Submitted"  || s == "PreSubmitted" || s == "ApiPending")   return ::core::OrderStatus::Working;
    if (s == "PartiallyFilled")                                           return ::core::OrderStatus::PartialFill;
    if (s == "Pending" || s == "PendingSubmit" || s == "PendingCancel")  return ::core::OrderStatus::Pending;
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
            t.tm_isdst = 0;
#ifdef _WIN32
            return _mkgmtime(&t);
#else
            return timegm(&t);
#endif
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

} // namespace core::services
