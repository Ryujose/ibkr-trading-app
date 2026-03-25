#include <catch2/catch_test_macros.hpp>
#include <ctime>
#include <cstring>

#include "core/models/MarketData.h"

// Helper: build a UTC time_t from calendar components without local-timezone
// influence (avoids mktime() DST guessing).
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

// ── TimeframeLabel ────────────────────────────────────────────────────────────

TEST_CASE("TimeframeLabel returns correct display strings", "[timeframe][label]") {
    using namespace core;
    REQUIRE(std::string(TimeframeLabel(Timeframe::M1))  == "1m");
    REQUIRE(std::string(TimeframeLabel(Timeframe::M5))  == "5m");
    REQUIRE(std::string(TimeframeLabel(Timeframe::M15)) == "15m");
    REQUIRE(std::string(TimeframeLabel(Timeframe::M30)) == "30m");
    REQUIRE(std::string(TimeframeLabel(Timeframe::H1))  == "1h");
    REQUIRE(std::string(TimeframeLabel(Timeframe::H4))  == "4h");
    REQUIRE(std::string(TimeframeLabel(Timeframe::D1))  == "1D");
    REQUIRE(std::string(TimeframeLabel(Timeframe::W1))  == "1W");
    REQUIRE(std::string(TimeframeLabel(Timeframe::MN))  == "1M");
}

// ── TimeframeSeconds ──────────────────────────────────────────────────────────

TEST_CASE("TimeframeSeconds returns correct bar durations in seconds", "[timeframe][seconds]") {
    using namespace core;
    REQUIRE(TimeframeSeconds(Timeframe::M1)  ==       60);
    REQUIRE(TimeframeSeconds(Timeframe::M5)  ==      300);
    REQUIRE(TimeframeSeconds(Timeframe::M15) ==      900);
    REQUIRE(TimeframeSeconds(Timeframe::M30) ==     1800);
    REQUIRE(TimeframeSeconds(Timeframe::H1)  ==     3600);
    REQUIRE(TimeframeSeconds(Timeframe::H4)  ==    14400);
    REQUIRE(TimeframeSeconds(Timeframe::D1)  ==    86400);
    REQUIRE(TimeframeSeconds(Timeframe::W1)  ==   604800);
    REQUIRE(TimeframeSeconds(Timeframe::MN)  ==  2592000);
}

TEST_CASE("TimeframeSeconds: each value is a positive multiple of the previous", "[timeframe][seconds]") {
    using namespace core;
    // M5 is exactly 5× M1, M15 is 3× M5, etc. — sanity-check the relationships.
    REQUIRE(TimeframeSeconds(Timeframe::M5)  == TimeframeSeconds(Timeframe::M1)  * 5);
    REQUIRE(TimeframeSeconds(Timeframe::M15) == TimeframeSeconds(Timeframe::M1)  * 15);
    REQUIRE(TimeframeSeconds(Timeframe::M30) == TimeframeSeconds(Timeframe::M1)  * 30);
    REQUIRE(TimeframeSeconds(Timeframe::H1)  == TimeframeSeconds(Timeframe::M1)  * 60);
    REQUIRE(TimeframeSeconds(Timeframe::H4)  == TimeframeSeconds(Timeframe::H1)  * 4);
    REQUIRE(TimeframeSeconds(Timeframe::D1)  == TimeframeSeconds(Timeframe::H1)  * 24);
    REQUIRE(TimeframeSeconds(Timeframe::W1)  == TimeframeSeconds(Timeframe::D1)  * 7);
}

// ── TimeframeIBBarSize ────────────────────────────────────────────────────────

TEST_CASE("TimeframeIBBarSize returns correct IB API strings", "[timeframe][ibbarsize]") {
    using namespace core;
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::M1))  == "1 min");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::M5))  == "5 mins");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::M15)) == "15 mins");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::M30)) == "30 mins");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::H1))  == "1 hour");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::H4))  == "4 hours");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::D1))  == "1 day");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::W1))  == "1 week");
    REQUIRE(std::string(TimeframeIBBarSize(Timeframe::MN))  == "1 month");
}

// ── TimeframeIBDuration ───────────────────────────────────────────────────────

TEST_CASE("TimeframeIBDuration returns correct IB API duration strings", "[timeframe][ibduration]") {
    using namespace core;
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::M1))  == "1 D");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::M5))  == "5 D");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::M15)) == "10 D");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::M30)) == "20 D");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::H1))  == "30 D");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::H4))  == "60 D");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::D1))  == "6 M");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::W1))  == "2 Y");
    REQUIRE(std::string(TimeframeIBDuration(Timeframe::MN))  == "5 Y");
}

// ── BarSeries helpers ─────────────────────────────────────────────────────────

TEST_CASE("BarSeries reports empty/size correctly", "[barseries]") {
    core::BarSeries s;
    REQUIRE(s.empty());
    REQUIRE(s.size() == 0);
    s.bars.push_back({});
    REQUIRE_FALSE(s.empty());
    REQUIRE(s.size() == 1);
}

TEST_CASE("BarSeries default timeframe is D1", "[barseries]") {
    core::BarSeries s;
    REQUIRE(s.timeframe == core::Timeframe::D1);
}

// ── IsUSDST ───────────────────────────────────────────────────────────────────
// Reference year: 2024
//   DST starts: Sunday March 10, 2024 (2nd Sunday of March)
//   DST ends:   Sunday November 3, 2024 (1st Sunday of November)

TEST_CASE("IsUSDST — January (no DST)", "[dst]") {
    REQUIRE_FALSE(core::IsUSDST(utc(2024, 1, 15, 12)));
}

TEST_CASE("IsUSDST — July (DST active)", "[dst]") {
    REQUIRE(core::IsUSDST(utc(2024, 7, 4, 12)));
}

TEST_CASE("IsUSDST — March: day before DST start is not DST", "[dst]") {
    // March 6 is 4 days before DST starts on March 10.
    REQUIRE_FALSE(core::IsUSDST(utc(2024, 3, 6, 12)));
}

TEST_CASE("IsUSDST — March: day after DST start is DST", "[dst]") {
    // March 11 is the day after DST starts.
    REQUIRE(core::IsUSDST(utc(2024, 3, 11, 12)));
}

TEST_CASE("IsUSDST — November: day before DST end is still DST", "[dst]") {
    // Nov 2 (Saturday) — DST ends on Nov 3 (Sunday).
    REQUIRE(core::IsUSDST(utc(2024, 11, 2, 12)));
}

TEST_CASE("IsUSDST — November: day after DST end is not DST", "[dst]") {
    // Nov 4 (Monday) — DST ended yesterday.
    REQUIRE_FALSE(core::IsUSDST(utc(2024, 11, 4, 12)));
}

TEST_CASE("IsUSDST — December (no DST)", "[dst]") {
    REQUIRE_FALSE(core::IsUSDST(utc(2024, 12, 25, 12)));
}

// ── BarSession ────────────────────────────────────────────────────────────────
// Summer (July — EDT, UTC-4): market open 13:30–20:00 UTC
// Winter (January — EST, UTC-5): market open 14:30–21:00 UTC

TEST_CASE("BarSession — Regular session in summer (EDT)", "[session]") {
    // 14:00 UTC = 10:00 AM EDT — squarely in regular session
    REQUIRE(core::BarSession(utc(2024, 7, 15, 14, 0)) == core::Session::Regular);
    // 19:59 UTC = 3:59 PM EDT — one minute before close
    REQUIRE(core::BarSession(utc(2024, 7, 15, 19, 59)) == core::Session::Regular);
}

TEST_CASE("BarSession — Regular session in winter (EST)", "[session]") {
    // 15:00 UTC = 10:00 AM EST
    REQUIRE(core::BarSession(utc(2024, 1, 15, 15, 0)) == core::Session::Regular);
}

TEST_CASE("BarSession — Pre-market in summer (EDT)", "[session]") {
    // 08:30 UTC = 4:30 AM EDT — pre-market starts at 4:00 AM ET (08:00 UTC)
    REQUIRE(core::BarSession(utc(2024, 7, 15, 8, 30)) == core::Session::PreMarket);
    // 13:00 UTC = 9:00 AM EDT — 30 min before open
    REQUIRE(core::BarSession(utc(2024, 7, 15, 13, 0)) == core::Session::PreMarket);
}

TEST_CASE("BarSession — After-hours in summer (EDT)", "[session]") {
    // 21:00 UTC = 5:00 PM EDT — one hour after close
    REQUIRE(core::BarSession(utc(2024, 7, 15, 21, 0)) == core::Session::AfterHours);
    // 23:30 UTC = 7:30 PM EDT — still in after-hours (ends at midnight ET = 04:00 UTC)
    REQUIRE(core::BarSession(utc(2024, 7, 15, 23, 30)) == core::Session::AfterHours);
}

TEST_CASE("BarSession — Overnight in summer (EDT)", "[session]") {
    // 02:00 UTC = 10:00 PM EDT previous day — overnight
    REQUIRE(core::BarSession(utc(2024, 7, 16, 2, 0)) == core::Session::Overnight);
    // 07:59 UTC = 3:59 AM EDT — still overnight (pre-market starts at 08:00 UTC)
    REQUIRE(core::BarSession(utc(2024, 7, 15, 7, 59)) == core::Session::Overnight);
}

TEST_CASE("BarSession — Pre-market boundary (exactly 04:00 AM ET = 08:00 UTC in summer)", "[session]") {
    // 08:00 UTC = 4:00 AM EDT — first second of pre-market (hhmm == 400)
    REQUIRE(core::BarSession(utc(2024, 7, 15, 8, 0)) == core::Session::PreMarket);
}

TEST_CASE("BarSession — Open boundary (exactly 09:30 AM ET in summer)", "[session]") {
    // 13:30 UTC = 9:30 AM EDT — market open (hhmm == 930)
    REQUIRE(core::BarSession(utc(2024, 7, 15, 13, 30)) == core::Session::Regular);
    // 13:29 UTC = 9:29 AM EDT — still pre-market
    REQUIRE(core::BarSession(utc(2024, 7, 15, 13, 29)) == core::Session::PreMarket);
}

TEST_CASE("BarSession — Close boundary (exactly 04:00 PM ET in summer)", "[session]") {
    // 20:00 UTC = 4:00 PM EDT — after-hours begins (hhmm == 1600)
    REQUIRE(core::BarSession(utc(2024, 7, 15, 20, 0)) == core::Session::AfterHours);
    // 19:59 UTC = 3:59 PM EDT — still regular
    REQUIRE(core::BarSession(utc(2024, 7, 15, 19, 59)) == core::Session::Regular);
}
