#include <catch2/catch_test_macros.hpp>
#include <ctime>
#include <cstring>
#include <string>

#include "core/services/IBKRUtils.h"

using core::services::ParseStatus;
using core::services::ParseIBTime;

// Helper: build a UTC time_t (same helper as in test_market_data.cpp).
static std::time_t utc(int year, int mon, int day,
                       int hour = 0, int min = 0, int sec = 0)
{
    struct tm t = {};
    t.tm_year  = year - 1900;
    t.tm_mon   = mon  - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_sec   = sec;
    return core::services::Timegm(&t);
}

// ── ParseStatus ───────────────────────────────────────────────────────────────

TEST_CASE("ParseStatus - Filled", "[parsestatus]") {
    REQUIRE(ParseStatus("Filled") == core::OrderStatus::Filled);
}

TEST_CASE("ParseStatus - Cancelled variants", "[parsestatus]") {
    REQUIRE(ParseStatus("Cancelled")    == core::OrderStatus::Cancelled);
    REQUIRE(ParseStatus("ApiCancelled") == core::OrderStatus::Cancelled);
    REQUIRE(ParseStatus("Inactive")     == core::OrderStatus::Cancelled);
}

TEST_CASE("ParseStatus - Working variants", "[parsestatus]") {
    REQUIRE(ParseStatus("Submitted")    == core::OrderStatus::Working);
    REQUIRE(ParseStatus("PreSubmitted") == core::OrderStatus::Working);
    REQUIRE(ParseStatus("ApiPending")   == core::OrderStatus::Working);
}

TEST_CASE("ParseStatus - PartialFill", "[parsestatus]") {
    REQUIRE(ParseStatus("PartiallyFilled") == core::OrderStatus::PartialFill);
}

TEST_CASE("ParseStatus - Pending variants", "[parsestatus]") {
    REQUIRE(ParseStatus("Pending")       == core::OrderStatus::Pending);
    REQUIRE(ParseStatus("PendingSubmit") == core::OrderStatus::Pending);
}

TEST_CASE("ParseStatus - PendingCancel maps to PendingCancel", "[parsestatus]") {
    REQUIRE(ParseStatus("PendingCancel") == core::OrderStatus::PendingCancel);
}

TEST_CASE("ParseStatus - empty string maps to Pending", "[parsestatus]") {
    REQUIRE(ParseStatus("") == core::OrderStatus::Pending);
}

TEST_CASE("ParseStatus - unknown string maps to Rejected", "[parsestatus]") {
    REQUIRE(ParseStatus("SomeUnknownStatus") == core::OrderStatus::Rejected);
    REQUIRE(ParseStatus("filled")            == core::OrderStatus::Rejected); // case-sensitive
    REQUIRE(ParseStatus("CANCELLED")         == core::OrderStatus::Rejected); // case-sensitive
}

// ── ParseIBTime ───────────────────────────────────────────────────────────────

TEST_CASE("ParseIBTime - empty string returns 0", "[parseibtime]") {
    REQUIRE(ParseIBTime("") == 0);
}

TEST_CASE("ParseIBTime - 8-digit date string parsed as noon UTC", "[parseibtime]") {
    // IB returns "YYYYMMDD" for daily/weekly/monthly bars. We map it to noon UTC
    // so that any reasonable timezone still maps back to the correct date.
    std::time_t result = ParseIBTime("20240115");
    REQUIRE(result != 0);

    // Verify the UTC calendar date (not affected by local timezone).
    struct tm* gm = std::gmtime(&result);
    REQUIRE(gm != nullptr);
    REQUIRE(gm->tm_year + 1900 == 2024);
    REQUIRE(gm->tm_mon  + 1    == 1);
    REQUIRE(gm->tm_mday        == 15);
    REQUIRE(gm->tm_hour        == 12);  // noon UTC
}

TEST_CASE("ParseIBTime - Unix timestamp string (intraday formatDate=2)", "[parseibtime]") {
    // IB sends the Unix timestamp as a decimal string for intraday bars.
    std::time_t known = utc(2024, 7, 15, 14, 30, 0);  // 2024-07-15 14:30:00 UTC
    std::string ts    = std::to_string(static_cast<long long>(known));

    std::time_t result = ParseIBTime(ts);
    REQUIRE(result == known);
}

TEST_CASE("ParseIBTime - Unix timestamp string round-trips for various dates", "[parseibtime]") {
    const std::time_t samples[] = {
        utc(2020, 1,  1,  0,  0,  0),
        utc(2023, 6, 15, 13, 30,  0),
        utc(2024, 12, 31, 23, 59, 59),
    };
    for (auto t : samples) {
        std::string s = std::to_string(static_cast<long long>(t));
        REQUIRE(ParseIBTime(s) == t);
    }
}

TEST_CASE("ParseIBTime - different dates produce different timestamps", "[parseibtime]") {
    std::time_t d1 = ParseIBTime("20240101");
    std::time_t d2 = ParseIBTime("20240102");
    REQUIRE(d2 > d1);
    REQUIRE(d2 - d1 == 86400);  // exactly one calendar day apart (both at noon UTC)
}

TEST_CASE("ParseIBTime - 8-digit date: year/month/day boundaries are correct", "[parseibtime]") {
    // Spot-check a few specific dates by verifying the UTC year/month/day.
    const struct { const char* str; int y, m, d; } cases[] = {
        {"20240101", 2024, 1,  1},
        {"20241231", 2024, 12, 31},
        {"20230704", 2023, 7,  4},
    };
    for (auto& c : cases) {
        std::time_t t = ParseIBTime(c.str);
        struct tm* gm = std::gmtime(&t);
        REQUIRE(gm != nullptr);
        REQUIRE(gm->tm_year + 1900 == c.y);
        REQUIRE(gm->tm_mon  + 1    == c.m);
        REQUIRE(gm->tm_mday        == c.d);
    }
}

// ── Futures symbol helpers ───────────────────────────────────────────────────

#include <string>
#include <cctype>

using core::services::IsFuturesBaseSymbol;
using core::services::IsFuturesSymbol;
using core::services::ParseFuturesSymbol;
using core::services::StripFuturesPrefix;
using core::services::FuturesFrontMonth;

TEST_CASE("IsFuturesBaseSymbol: known symbols", "[futures]") {
    REQUIRE(IsFuturesBaseSymbol("ES"));
    REQUIRE(IsFuturesBaseSymbol("NQ"));
    REQUIRE(IsFuturesBaseSymbol("YM"));
    REQUIRE(IsFuturesBaseSymbol("RTY"));
    REQUIRE(IsFuturesBaseSymbol("GC"));
    REQUIRE(IsFuturesBaseSymbol("CL"));
    REQUIRE(IsFuturesBaseSymbol("NG"));
    REQUIRE(IsFuturesBaseSymbol("ZN"));
    REQUIRE(IsFuturesBaseSymbol("ZB"));
    REQUIRE(IsFuturesBaseSymbol("6E"));
    REQUIRE(IsFuturesBaseSymbol("6J"));
    REQUIRE(IsFuturesBaseSymbol("ZC"));
    REQUIRE(IsFuturesBaseSymbol("ZS"));
    REQUIRE(IsFuturesBaseSymbol("HE"));
    REQUIRE(IsFuturesBaseSymbol("LE"));
}

TEST_CASE("IsFuturesBaseSymbol: unknown symbols", "[futures]") {
    REQUIRE_FALSE(IsFuturesBaseSymbol("AAPL"));
    REQUIRE_FALSE(IsFuturesBaseSymbol("MSFT"));
    REQUIRE_FALSE(IsFuturesBaseSymbol("SPY"));
    REQUIRE_FALSE(IsFuturesBaseSymbol(""));
    REQUIRE_FALSE(IsFuturesBaseSymbol("XYZ"));
}

TEST_CASE("IsFuturesSymbol: /-prefix", "[futures]") {
    REQUIRE(IsFuturesSymbol("/ES"));
    REQUIRE(IsFuturesSymbol("/NQ"));
    REQUIRE(IsFuturesSymbol("/GC"));
}

TEST_CASE("IsFuturesSymbol: /-prefix requires at least 2 chars", "[futures]") {
    REQUIRE_FALSE(IsFuturesSymbol("/"));
}

TEST_CASE("IsFuturesSymbol: plain base symbol", "[futures]") {
    REQUIRE(IsFuturesSymbol("ES"));
    REQUIRE(IsFuturesSymbol("NQ"));
    REQUIRE(IsFuturesSymbol("CL"));
}

TEST_CASE("IsFuturesSymbol: base symbol with contract month (space)", "[futures]") {
    REQUIRE(IsFuturesSymbol("NQ 202612"));
    REQUIRE(IsFuturesSymbol("ES 202606"));
    REQUIRE(IsFuturesSymbol("CL 202701"));
}

TEST_CASE("IsFuturesSymbol: base symbol with contract month (colon)", "[futures]") {
    REQUIRE(IsFuturesSymbol("NQ:202612"));
    REQUIRE(IsFuturesSymbol("ES:202606"));
}

TEST_CASE("IsFuturesSymbol: /-prefixed with contract month", "[futures]") {
    REQUIRE(IsFuturesSymbol("/NQ 202612"));
    REQUIRE(IsFuturesSymbol("/ES:202606"));
}

TEST_CASE("IsFuturesSymbol: unknown base with contract month is not futures", "[futures]") {
    REQUIRE_FALSE(IsFuturesSymbol("AAPL 202612"));
    REQUIRE_FALSE(IsFuturesSymbol("XYZ:202606"));
}

TEST_CASE("ParseFuturesSymbol: plain symbol", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("ES", base, month);
    REQUIRE(base == "ES");
    REQUIRE(month.empty());
}

TEST_CASE("ParseFuturesSymbol: /-prefixed", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("/NQ", base, month);
    REQUIRE(base == "NQ");
    REQUIRE(month.empty());
}

TEST_CASE("ParseFuturesSymbol: with space-separated contract month", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("NQ 202612", base, month);
    REQUIRE(base == "NQ");
    REQUIRE(month == "202612");
}

TEST_CASE("ParseFuturesSymbol: with colon-separated contract month", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("ES:202606", base, month);
    REQUIRE(base == "ES");
    REQUIRE(month == "202606");
}

TEST_CASE("ParseFuturesSymbol: /-prefixed with space-separated contract month", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("/NQ 202612", base, month);
    REQUIRE(base == "NQ");
    REQUIRE(month == "202612");
}

TEST_CASE("ParseFuturesSymbol: /-prefixed with colon-separated contract month", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("/ES:202606", base, month);
    REQUIRE(base == "ES");
    REQUIRE(month == "202606");
}

TEST_CASE("ParseFuturesSymbol: non-futures symbol passes through", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("AAPL", base, month);
    REQUIRE(base == "AAPL");
    REQUIRE(month.empty());
}

TEST_CASE("ParseFuturesSymbol: non-digit suffix treated as part of base", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("ES JUN26", base, month);  // not YYYYMM
    REQUIRE(base == "ES JUN26");
    REQUIRE(month.empty());
}

TEST_CASE("ParseFuturesSymbol: short suffix not treated as contract month", "[futures]") {
    std::string base, month;
    ParseFuturesSymbol("ES 20261", base, month);  // 5 digits, not 6
    REQUIRE(base == "ES 20261");
    REQUIRE(month.empty());
}

TEST_CASE("StripFuturesPrefix: strips leading slash", "[futures]") {
    REQUIRE(StripFuturesPrefix("/ES") == "ES");
    REQUIRE(StripFuturesPrefix("/NQ 202612") == "NQ 202612");
}

TEST_CASE("StripFuturesPrefix: no slash returns unchanged", "[futures]") {
    REQUIRE(StripFuturesPrefix("ES") == "ES");
    REQUIRE(StripFuturesPrefix("NQ 202612") == "NQ 202612");
    REQUIRE(StripFuturesPrefix("") == "");
}

TEST_CASE("FuturesFrontMonth: returns 6-char YYYYMM all-digit string", "[futures]") {
    std::string fm = FuturesFrontMonth();
    REQUIRE(fm.size() == 6);
    REQUIRE(fm[0] == '2');   // year starts with 2 (this century)
    REQUIRE(fm[4] >= '0'); REQUIRE(fm[4] <= '1');  // month first digit 0 or 1
    REQUIRE(fm[5] >= '0'); REQUIRE(fm[5] <= '9');   // month second digit
    for (char c : fm) REQUIRE(std::isdigit(static_cast<unsigned char>(c)));
}

TEST_CASE("FuturesFrontMonth: month is between 01 and 12", "[futures]") {
    std::string fm = FuturesFrontMonth();
    int month = std::stoi(fm.substr(4, 2));
    REQUIRE(month >= 1);
    REQUIRE(month <= 12);
}
