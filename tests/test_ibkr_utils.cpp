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
    t.tm_isdst = 0;
#ifdef _WIN32
    return _mkgmtime(&t);
#else
    return timegm(&t);
#endif
}

// ── ParseStatus ───────────────────────────────────────────────────────────────

TEST_CASE("ParseStatus — Filled", "[parsestatus]") {
    REQUIRE(ParseStatus("Filled") == core::OrderStatus::Filled);
}

TEST_CASE("ParseStatus — Cancelled variants", "[parsestatus]") {
    REQUIRE(ParseStatus("Cancelled")    == core::OrderStatus::Cancelled);
    REQUIRE(ParseStatus("ApiCancelled") == core::OrderStatus::Cancelled);
    REQUIRE(ParseStatus("Inactive")     == core::OrderStatus::Cancelled);
}

TEST_CASE("ParseStatus — Working variants", "[parsestatus]") {
    REQUIRE(ParseStatus("Submitted")    == core::OrderStatus::Working);
    REQUIRE(ParseStatus("PreSubmitted") == core::OrderStatus::Working);
    REQUIRE(ParseStatus("ApiPending")   == core::OrderStatus::Working);
}

TEST_CASE("ParseStatus — PartialFill", "[parsestatus]") {
    REQUIRE(ParseStatus("PartiallyFilled") == core::OrderStatus::PartialFill);
}

TEST_CASE("ParseStatus — Pending variants", "[parsestatus]") {
    REQUIRE(ParseStatus("Pending")       == core::OrderStatus::Pending);
    REQUIRE(ParseStatus("PendingSubmit") == core::OrderStatus::Pending);
    REQUIRE(ParseStatus("PendingCancel") == core::OrderStatus::Pending);
}

TEST_CASE("ParseStatus — empty string maps to Pending", "[parsestatus]") {
    REQUIRE(ParseStatus("") == core::OrderStatus::Pending);
}

TEST_CASE("ParseStatus — unknown string maps to Rejected", "[parsestatus]") {
    REQUIRE(ParseStatus("SomeUnknownStatus") == core::OrderStatus::Rejected);
    REQUIRE(ParseStatus("filled")            == core::OrderStatus::Rejected); // case-sensitive
    REQUIRE(ParseStatus("CANCELLED")         == core::OrderStatus::Rejected); // case-sensitive
}

// ── ParseIBTime ───────────────────────────────────────────────────────────────

TEST_CASE("ParseIBTime — empty string returns 0", "[parseibtime]") {
    REQUIRE(ParseIBTime("") == 0);
}

TEST_CASE("ParseIBTime — 8-digit date string parsed as noon UTC", "[parseibtime]") {
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

TEST_CASE("ParseIBTime — Unix timestamp string (intraday formatDate=2)", "[parseibtime]") {
    // IB sends the Unix timestamp as a decimal string for intraday bars.
    std::time_t known = utc(2024, 7, 15, 14, 30, 0);  // 2024-07-15 14:30:00 UTC
    std::string ts    = std::to_string(static_cast<long long>(known));

    std::time_t result = ParseIBTime(ts);
    REQUIRE(result == known);
}

TEST_CASE("ParseIBTime — Unix timestamp string round-trips for various dates", "[parseibtime]") {
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

TEST_CASE("ParseIBTime — different dates produce different timestamps", "[parseibtime]") {
    std::time_t d1 = ParseIBTime("20240101");
    std::time_t d2 = ParseIBTime("20240102");
    REQUIRE(d2 > d1);
    REQUIRE(d2 - d1 == 86400);  // exactly one calendar day apart (both at noon UTC)
}

TEST_CASE("ParseIBTime — 8-digit date: year/month/day boundaries are correct", "[parseibtime]") {
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
