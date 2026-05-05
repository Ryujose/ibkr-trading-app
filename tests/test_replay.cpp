#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "core/models/MarketData.h"
#include "core/models/OrderData.h"
#include "core/models/ReplayData.h"
#include "core/services/IBKRUtils.h"
#include "core/services/ReplayEngine.h"

using core::services::ReplaySession;
using core::services::ReplaySessionLabel;
using core::services::ReplaySessionShort;
using core::services::ReplayClock;
using core::services::Tick;
using core::services::StepBars;
using core::services::SeekToBar;
using core::services::SeekToTime;
using core::services::SnapCursorToNearestBar;
using core::services::BarRange;
using core::services::BarRangeForSession;
using core::services::SimulatedAccount;
using core::services::SimulatedFill;
using core::services::ApplyFill;
using core::services::Equity;
using core::services::UnrealizedPnL;
using core::services::Reset;
using core::services::WorkingOrder;
using core::services::SimulatedOrderBook;
using core::services::EvaluateBar;
using core::services::EvaluateTick;
using core::services::kDefaultCommissionPerShare;

namespace {

std::time_t utc(int y, int mo, int d, int h, int mi, int s = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = s;
    tm.tm_isdst = 0;
    return core::services::Timegm(&tm);
}

// April 15, 2026 EDT (UTC-4): 09:30 ET = 13:30 UTC
std::time_t rthTime(int hourEt, int minEt = 0) {
    return utc(2026, 4, 15, hourEt + 4, minEt);
}

core::Bar makeBar(std::time_t ts, double o, double h, double l, double c, double v = 1000.0) {
    return {static_cast<double>(ts), o, h, l, c, v};
}

core::Bar rthBar(int hourEt, int minEt, double o, double h, double l, double c) {
    return makeBar(rthTime(hourEt, minEt), o, h, l, c);
}

core::Order makeOrder(core::OrderType type, core::OrderSide side, double qty,
                      double limitPx = 0.0, double stopPx = 0.0,
                      double auxPx = 0.0) {
    core::Order o;
    o.type       = type;
    o.side       = side;
    o.quantity   = qty;
    o.limitPrice = limitPx;
    o.stopPrice  = stopPx;
    o.auxPrice   = auxPx;
    o.tif        = core::TimeInForce::Day;
    o.outsideRth = true;
    o.symbol     = "AAPL";
    return o;
}

WorkingOrder makeWO(int localId, core::OrderType type, core::OrderSide side,
                    double qty, double limitPx = 0.0, double stopPx = 0.0,
                    double auxPx = 0.0, std::time_t placedAt = 0) {
    WorkingOrder wo;
    wo.localId  = localId;
    wo.order    = makeOrder(type, side, qty, limitPx, stopPx, auxPx);
    wo.placedAt = placedAt;
    return wo;
}

}  // anonymous namespace

// ============================================================================
// ReplaySession labels
// ============================================================================

TEST_CASE("ReplaySessionLabel covers all four values", "[replay][session]") {
    CHECK(std::string(ReplaySessionLabel(ReplaySession::PreMarket)) == "Pre-Market");
    CHECK(std::string(ReplaySessionLabel(ReplaySession::Intraday))  == "Intraday");
    CHECK(std::string(ReplaySessionLabel(ReplaySession::PostMarket))== "Post-Market");
    CHECK(std::string(ReplaySessionLabel(ReplaySession::All))       == "All");
}

TEST_CASE("ReplaySessionShort covers all four values", "[replay][session]") {
    CHECK(std::string(ReplaySessionShort(ReplaySession::PreMarket)) == "PRE");
    CHECK(std::string(ReplaySessionShort(ReplaySession::Intraday))  == "RTH");
    CHECK(std::string(ReplaySessionShort(ReplaySession::PostMarket))== "POST");
    CHECK(std::string(ReplaySessionShort(ReplaySession::All))       == "ALL");
}

// ============================================================================
// ReplayClock — Tick / StepBars / SeekToBar / SeekToTime
// ============================================================================

TEST_CASE("Tick: paused clock does not advance", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 10;
    c.sessionLastIdx = 100;
    c.paused        = true;
    c.speed         = 1.0;
    Tick(c, 60.0, 60.0);
    CHECK(c.cursorBarIdx == 10);
    CHECK(c.cursorSeconds == 0.0);
}

TEST_CASE("Tick: speed=0 does not advance", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 10;
    c.sessionLastIdx = 100;
    c.paused        = false;
    c.speed         = 0.0;
    Tick(c, 60.0, 60.0);
    CHECK(c.cursorBarIdx == 10);
}

TEST_CASE("Tick: scrubbing flag prevents advance", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 10;
    c.sessionLastIdx = 100;
    c.paused        = false;
    c.speed         = 1.0;
    c.scrubbing     = true;
    Tick(c, 60.0, 60.0);
    CHECK(c.cursorBarIdx == 10);
}

TEST_CASE("Tick: speed=1 advances one bar per barSec of wall time", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 10;
    c.sessionLastIdx = 100;
    c.paused        = false;
    c.speed         = 1.0;
    Tick(c, 60.0, 60.0);
    CHECK(c.cursorBarIdx == 11);
}

TEST_CASE("Tick: speed=5 advances five bars", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 10;
    c.sessionLastIdx = 100;
    c.paused        = false;
    c.speed         = 5.0;
    Tick(c, 60.0, 60.0);
    CHECK(c.cursorBarIdx == 15);
}

TEST_CASE("Tick: clamps at sessionLastIdx", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 98;
    c.sessionLastIdx = 100;
    c.paused        = false;
    c.speed         = 10.0;
    Tick(c, 60.0, 60.0);
    CHECK(c.cursorBarIdx == 100);
}

TEST_CASE("Tick: fractional advance across multiple ticks", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx  = 10;
    c.sessionLastIdx = 100;
    c.paused        = false;
    c.speed         = 1.0;
    Tick(c, 30.0, 60.0);
    CHECK(c.cursorBarIdx == 10);
    Tick(c, 30.0, 60.0);
    CHECK(c.cursorBarIdx == 11);
}

TEST_CASE("StepBars: positive and negative with clamp", "[replay][clock]") {
    ReplayClock c;
    c.cursorBarIdx   = 50;
    c.sessionFirstIdx= 10;
    c.sessionLastIdx = 90;
    StepBars(c, 5);
    CHECK(c.cursorBarIdx == 55);
    StepBars(c, -3);
    CHECK(c.cursorBarIdx == 52);
    StepBars(c, -100);
    CHECK(c.cursorBarIdx == 10);
    StepBars(c, 200);
    CHECK(c.cursorBarIdx == 90);
}

TEST_CASE("SeekToBar: within range and clamped", "[replay][clock]") {
    ReplayClock c;
    c.sessionFirstIdx = 5;
    c.sessionLastIdx  = 95;
    SeekToBar(c, 42);
    CHECK(c.cursorBarIdx == 42);
    CHECK(c.cursorSeconds == 0.0);
    SeekToBar(c, -5);
    CHECK(c.cursorBarIdx == 5);
    SeekToBar(c, 500);
    CHECK(c.cursorBarIdx == 95);
}

TEST_CASE("SnapCursorToNearestBar: exact match", "[replay][clock]") {
    std::vector<core::Bar> bars;
    for (int i = 0; i < 10; ++i)
        bars.push_back(makeBar(utc(2026, 4, 15, 14, i), 100.0, 101.0, 99.0, 100.5));
    int idx = SnapCursorToNearestBar(utc(2026, 4, 15, 14, 5), bars);
    CHECK(idx == 5);
}

TEST_CASE("SnapCursorToNearestBar: between bars picks closest", "[replay][clock]") {
    std::vector<core::Bar> bars;
    for (int i = 0; i < 10; ++i)
        bars.push_back(makeBar(utc(2026, 4, 15, 14, i), 100.0, 101.0, 99.0, 100.5));
    // bar[3]=14:03, bar[4]=14:04. Target=14:03:40→ closer to bar[4]
    int idx = SnapCursorToNearestBar(utc(2026, 4, 15, 14, 3, 40), bars);
    CHECK(idx == 4);
}

TEST_CASE("SnapCursorToNearestBar: tie-break to earlier bar", "[replay][clock]") {
    std::vector<core::Bar> bars;
    bars.push_back(makeBar(utc(2026, 4, 15, 14, 0), 100.0, 101.0, 99.0, 100.5));
    bars.push_back(makeBar(utc(2026, 4, 15, 14, 2), 100.0, 101.0, 99.0, 100.5));
    // Target=14:01 — exactly halfway → earlier bar
    int idx = SnapCursorToNearestBar(utc(2026, 4, 15, 14, 1), bars);
    CHECK(idx == 0);
}

TEST_CASE("SnapCursorToNearestBar: clamps low and high", "[replay][clock]") {
    std::vector<core::Bar> bars;
    for (int i = 5; i < 10; ++i)
        bars.push_back(makeBar(utc(2026, 4, 15, 14, i), 100.0, 101.0, 99.0, 100.5));
    CHECK(SnapCursorToNearestBar(utc(2026, 4, 15, 13, 0), bars) == 0);
    CHECK(SnapCursorToNearestBar(utc(2026, 4, 15, 15, 0), bars) == 4);
}

TEST_CASE("SnapCursorToNearestBar: empty vector returns 0", "[replay][clock]") {
    CHECK(SnapCursorToNearestBar(utc(2026, 4, 15, 14, 0), {}) == 0);
}

// ============================================================================
// BarRangeForSession
// ============================================================================

static std::vector<core::Bar> buildDayBars() {
    std::vector<core::Bar> bars;
    for (int h = 0; h < 24; ++h)
        bars.push_back(makeBar(utc(2026, 4, 15, h, 0), 100.0, 101.0, 99.0, 100.5));
    bars.push_back(makeBar(utc(2026, 4, 15, 13, 30), 100.0, 101.0, 99.0, 100.5)); // 09:30 ET
    return bars;
}

TEST_CASE("BarRangeForSession: Intraday picks 09:30-16:00 ET bars", "[replay][session]") {
    auto bars = buildDayBars();
    auto r = BarRangeForSession(bars, ReplaySession::Intraday);
    CHECK(r.firstIdx > 0);
    CHECK(r.lastIdx > r.firstIdx);
    // 13:30 UTC = 09:30 ET → should be in range
    bool found = false;
    for (int i = r.firstIdx; i <= r.lastIdx && !found; ++i) {
        std::time_t t = static_cast<std::time_t>(bars[i].timestamp);
        std::tm* u = std::gmtime(&t);
        if (u && u->tm_hour == 13 && u->tm_min == 30) found = true;
    }
    CHECK(found);
}

TEST_CASE("BarRangeForSession: PreMarket picks 04:00-09:30 ET", "[replay][session]") {
    auto bars = buildDayBars();
    auto r = BarRangeForSession(bars, ReplaySession::PreMarket);
    // 08:00 UTC = 04:00 ET
    bool found = false;
    for (int i = r.firstIdx; i <= r.lastIdx && !found; ++i) {
        std::time_t t = static_cast<std::time_t>(bars[i].timestamp);
        std::tm* u = std::gmtime(&t);
        if (u && u->tm_hour == 8) found = true;
    }
    CHECK(found);
}

TEST_CASE("BarRangeForSession: PostMarket picks 16:00-20:00 ET", "[replay][session]") {
    auto bars = buildDayBars();
    auto r = BarRangeForSession(bars, ReplaySession::PostMarket);
    // 20:00 UTC = 16:00 ET
    bool found = false;
    for (int i = r.firstIdx; i <= r.lastIdx && !found; ++i) {
        std::time_t t = static_cast<std::time_t>(bars[i].timestamp);
        std::tm* u = std::gmtime(&t);
        if (u && u->tm_hour == 20) found = true;
    }
    CHECK(found);
}

TEST_CASE("BarRangeForSession: All covers Pre+Intra+Post", "[replay][session]") {
    auto bars = buildDayBars();
    auto r = BarRangeForSession(bars, ReplaySession::All);
    CHECK(r.lastIdx > r.firstIdx);
    bool pre = false, post = false;
    for (int i = r.firstIdx; i <= r.lastIdx; ++i) {
        std::time_t t = static_cast<std::time_t>(bars[i].timestamp);
        std::tm* u = std::gmtime(&t);
        if (u && u->tm_hour == 8)  pre  = true;
        if (u && u->tm_hour == 20) post = true;
    }
    CHECK(pre);
    CHECK(post);
}

TEST_CASE("BarRangeForSession: empty input returns {0,0}", "[replay][session]") {
    auto r = BarRangeForSession({}, ReplaySession::Intraday);
    CHECK(r.firstIdx == 0);
    CHECK(r.lastIdx == 0);
}

// ============================================================================
// EvaluateBar — Market
// ============================================================================

TEST_CASE("EvaluateBar: Market BUY fills at bar.open after placement", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].qty == 100.0);
    CHECK(r.fills[0].price == 150.0);
    CHECK(r.fills[0].side == core::OrderSide::Buy);
    CHECK(r.fills[0].intentNote == "market fill");
    CHECK(r.filledIds == std::vector<int>{1});
}

TEST_CASE("EvaluateBar: Market does NOT fill on placement bar", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto bar = makeBar(placedAt, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    CHECK(r.fills.empty());
}

TEST_CASE("EvaluateBar: Market SELL fills at bar.open", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Sell,
                          50.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].qty == 50.0);
    CHECK(r.fills[0].price == 150.0);
    CHECK(r.fills[0].side == core::OrderSide::Sell);
}

// ============================================================================
// EvaluateBar — Limit
// ============================================================================

TEST_CASE("EvaluateBar: Limit BUY fills when bar.low <= limitPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Limit, core::OrderSide::Buy,
                          100.0, 149.0, 0, 0, placedAt));
    // bar.open=150, bar.low=148 → fill at min(150,149)=149
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 149.0);
    CHECK(r.fills[0].intentNote == "limit fill");
}

TEST_CASE("EvaluateBar: Limit BUY no fill when bar.low > limitPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Limit, core::OrderSide::Buy,
                          100.0, 145.0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 149.0, 151.0);
    auto r = EvaluateBar(book, bar);
    CHECK(r.fills.empty());
}

TEST_CASE("EvaluateBar: Limit SELL fills when bar.high >= limitPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Limit, core::OrderSide::Sell,
                          100.0, 151.0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 151.0);  // max(150,151)=151
}

// ============================================================================
// EvaluateBar — Stop
// ============================================================================

TEST_CASE("EvaluateBar: Stop BUY fills when bar.high >= stopPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Stop, core::OrderSide::Buy,
                          100.0, 0, 151.0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 151.0);  // max(150,151)=151
    CHECK(r.fills[0].intentNote == "stop trigger");
}

TEST_CASE("EvaluateBar: Stop SELL fills when bar.low <= stopPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Stop, core::OrderSide::Sell,
                          100.0, 0, 149.0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 149.0);  // min(150,149)=149
}

TEST_CASE("EvaluateBar: Stop BUY no trigger when bar.high < stopPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Stop, core::OrderSide::Buy,
                          100.0, 0, 153.0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    CHECK(r.fills.empty());
}

// ============================================================================
// EvaluateBar — StopLimit
// ============================================================================

TEST_CASE("EvaluateBar: StopLimit BUY stop+limit same bar", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::StopLimit, core::OrderSide::Buy,
                          100.0, 152.0, 151.0, 0, placedAt));
    // bar.high=152 triggers stop at 151, bar.low=148 meets limit at 152 → fill at min(150,152)=150
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "stop-limit fill");
}

TEST_CASE("EvaluateBar: StopLimit stop triggers but limit not met this bar", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::StopLimit, core::OrderSide::Buy,
                          100.0, 149.0, 151.0, 0, placedAt));
    // stopPrice=151 triggered by bar.high=154. limitPrice=149, bar.low=150 → 150 NOT <= 149 → no fill
    auto bar = rthBar(10, 1, 152.0, 154.0, 150.0, 153.0);
    auto r = EvaluateBar(book, bar);
    CHECK(r.fills.empty());
    CHECK(book[0].stopTriggered == true);
}

TEST_CASE("EvaluateBar: StopLimit SELL stop+limit same bar", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::StopLimit, core::OrderSide::Sell,
                          100.0, 149.0, 148.0, 0, placedAt));
    // stopPrice=148 triggered by bar.low=147. limitPrice=149, bar.high=153 → fill at max(151,149)=151
    auto bar = rthBar(10, 1, 151.0, 153.0, 147.0, 150.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "stop-limit fill");
}

// ============================================================================
// EvaluateBar — MIT / LIT
// ============================================================================

TEST_CASE("EvaluateBar: MIT BUY triggers when bar.high >= auxPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::MIT, core::OrderSide::Buy,
                          100.0, 0, 0, 151.0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 150.0);  // bar.open
    CHECK(r.fills[0].intentNote == "MIT trigger");
}

TEST_CASE("EvaluateBar: MIT SELL triggers when bar.low <= auxPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::MIT, core::OrderSide::Sell,
                          100.0, 0, 0, 148.0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 147.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 150.0);
}

TEST_CASE("EvaluateBar: LIT BUY trigger+limit same bar", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::LIT, core::OrderSide::Buy,
                          100.0, 150.0, 0, 151.0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "LIT fill");
}

// ============================================================================
// EvaluateBar — Trail / TrailLimit
// ============================================================================

TEST_CASE("EvaluateBar: Trail SELL triggers after price drops below trailing stop", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    // Trail SELL $2. Entry bar.open=151, init trailStop=149
    // bar.high=152 → trailRef=152, trailStop=150. bar.low=147 → 147<=150 → fill at bar.open=151
    book.push_back(makeWO(1, core::OrderType::Trail, core::OrderSide::Sell,
                          100.0, 0, 0, 2.0, placedAt));
    auto bar = rthBar(10, 1, 151.0, 152.0, 147.0, 150.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "trail stop trigger");
}

TEST_CASE("EvaluateBar: Trail BUY triggers after price rises above trailing stop", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    // Trail BUY $3. Entry bar.open=150, init trailStop=153
    // bar.low=146 → trailRef=146, trailStop=149. bar.high=153 → 153>=149 → fill
    book.push_back(makeWO(1, core::OrderType::Trail, core::OrderSide::Buy,
                          100.0, 0, 0, 3.0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 153.0, 146.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "trail stop trigger");
}

TEST_CASE("EvaluateBar: Trail SELL does not trigger when price stays above stop", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    // Trail SELL $5. Entry bar.open=150, init trailStop=145
    // bar.high=153 → trailRef=153, trailStop=148. bar.low=149 → 149 > 148 → no trigger
    book.push_back(makeWO(1, core::OrderType::Trail, core::OrderSide::Sell,
                          100.0, 0, 0, 5.0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 153.0, 149.0, 151.0);
    auto r = EvaluateBar(book, bar);
    CHECK(r.fills.empty());
}

TEST_CASE("EvaluateBar: Trail Limit SELL stop triggers then limit fills", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    // TrailLimit SELL $1 trail, limit=150. Entry=151, trailStop init=150.
    // bar.high=153 → trailRef=153, trailStop=152. bar.low=147 → 147<=152 → stop triggered
    // limitPrice=150, bar.high=153 → 153>=150 → fill at max(151,150)=151
    book.push_back(makeWO(1, core::OrderType::TrailLimit, core::OrderSide::Sell,
                          100.0, 150.0, 0, 1.0, placedAt));
    auto bar = rthBar(10, 1, 151.0, 153.0, 147.0, 150.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "trail-limit fill");
}

// ============================================================================
// EvaluateBar — MOC / LOC / MTL
// ============================================================================

TEST_CASE("EvaluateBar: MOC fills only on session-close bar at bar.close", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::MOC, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    // Not session close
    auto r1 = EvaluateBar(book, bar);
    CHECK(r1.fills.empty());
    // Session close
    auto r2 = EvaluateBar(book, bar, kDefaultCommissionPerShare, true);
    REQUIRE(r2.fills.size() == 1);
    CHECK(r2.fills[0].price == 151.0);  // bar.close
    CHECK(r2.fills[0].intentNote == "MOC fill");
}

TEST_CASE("EvaluateBar: LOC fills at bar.close when limit met on session-close", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::LOC, core::OrderSide::Buy,
                          100.0, 149.0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 151.0, 153.0, 147.0, 151.0);
    auto r = EvaluateBar(book, bar, kDefaultCommissionPerShare, true);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 151.0);
    CHECK(r.fills[0].intentNote == "LOC fill");
}

TEST_CASE("EvaluateBar: MTL fills at bar.close", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::MTL, core::OrderSide::Sell,
                          50.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 151.0);
    CHECK(r.fills[0].intentNote == "MTL fill");
}

// ============================================================================
// EvaluateBar — Midprice / Relative
// ============================================================================

TEST_CASE("EvaluateBar: Midprice fills at bar mid", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Midprice, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 154.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 151.0);  // (154+148)/2 = 151
    CHECK(r.fills[0].intentNote == "midprice fill");
}

TEST_CASE("EvaluateBar: Midprice caps at limitPrice", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Midprice, core::OrderSide::Buy,
                          100.0, 150.0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 156.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    // mid=(156+148)/2=152, limit=150 → capped at 150
    CHECK(r.fills[0].price == 150.0);
}

TEST_CASE("EvaluateBar: Relative BUY pegs below bar.open", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Relative, core::OrderSide::Buy,
                          100.0, 0, 0, 0.05, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 149.95);  // 150.0 - 0.05
    CHECK(r.fills[0].intentNote == "relative fill");
}

TEST_CASE("EvaluateBar: Relative SELL pegs above bar.open", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Relative, core::OrderSide::Sell,
                          100.0, 0, 0, 0.10, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 150.10);  // 150.0 + 0.10
}

// ============================================================================
// EvaluateBar — commission, ordering, outsideRth, TIF DAY
// ============================================================================

TEST_CASE("EvaluateBar: commission is quantity x rate", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Buy,
                          200.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar, 0.01);
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].commission == 2.0);  // 200 × 0.01
}

TEST_CASE("EvaluateBar: multiple orders same bar produce sorted filledIds", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(9, 0);
    book.push_back(makeWO(3, core::OrderType::Market, core::OrderSide::Buy, 10.0, 0, 0, 0, placedAt));
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Buy, 20.0, 0, 0, 0, placedAt));
    book.push_back(makeWO(2, core::OrderType::Market, core::OrderSide::Buy, 30.0, 0, 0, 0, placedAt));
    auto bar = rthBar(10, 1, 150.0, 152.0, 148.0, 151.0);
    auto r = EvaluateBar(book, bar);
    REQUIRE(r.fills.size() == 3);
    // filledIds sorted ascending
    CHECK(r.filledIds[0] == 1);
    CHECK(r.filledIds[1] == 2);
    CHECK(r.filledIds[2] == 3);
}

TEST_CASE("EvaluateBar: outsideRth=false only fills on Intraday bars", "[replay][evalbar]") {
    // Pre-market bar: 08:00 UTC = 04:00 ET
    auto preBar = makeBar(utc(2026, 4, 15, 8, 0), 100.0, 101.0, 99.0, 100.5);
    // RTH bar: 13:30 UTC = 09:30 ET
    auto rthBar2 = makeBar(utc(2026, 4, 15, 13, 30), 150.0, 152.0, 148.0, 151.0);

    auto placedAt = utc(2026, 4, 15, 7, 0);
    auto wo = makeWO(1, core::OrderType::Market, core::OrderSide::Buy, 100.0,
                     0, 0, 0, placedAt);
    wo.order.outsideRth = false;

    SimulatedOrderBook book1{wo};
    CHECK(EvaluateBar(book1, preBar).fills.empty());

    SimulatedOrderBook book2{wo};
    CHECK(EvaluateBar(book2, rthBar2).fills.size() == 1);
}

TEST_CASE("EvaluateBar: TIF DAY orders auto-cancel at session close", "[replay][evalbar]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    // Limit at 100.0 with bar.low=149 — never triggers, so the TIF DAY cancel is the only removal
    book.push_back(makeWO(1, core::OrderType::Limit, core::OrderSide::Buy,
                          100.0, 100.0, 0, 0, placedAt));
    book[0].order.tif = core::TimeInForce::Day;
    auto bar = rthBar(10, 1, 150.0, 152.0, 149.0, 151.0);
    auto r = EvaluateBar(book, bar, kDefaultCommissionPerShare, true);
    // No fill (limit not met) but cancelled
    CHECK(r.fills.empty());
    CHECK(r.filledIds == std::vector<int>{1});
}

// ============================================================================
// EvaluateTick — per-tick resolution
// ============================================================================

static core::HistoricalTick tradeTick(std::time_t t, double price, double size = 100.0) {
    core::HistoricalTick tk;
    tk.type  = core::TickType::Trades;
    tk.time  = t;
    tk.price = price;
    tk.size  = size;
    return tk;
}

static core::HistoricalTick bidAskTick(std::time_t t, double bid, double ask) {
    core::HistoricalTick tk;
    tk.type     = core::TickType::BidAsk;
    tk.time     = t;
    tk.bidPrice = bid;
    tk.askPrice = ask;
    return tk;
}

static core::HistoricalTick midTick(std::time_t t, double price) {
    core::HistoricalTick tk;
    tk.type  = core::TickType::Midpoint;
    tk.time  = t;
    tk.price = price;
    return tk;
}

TEST_CASE("EvaluateTick: Market fills on TRADES tick", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto r = EvaluateTick(book, tradeTick(rthTime(10, 1), 150.25));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 150.25);
    CHECK(r.fills[0].intentNote == "market fill");
}

TEST_CASE("EvaluateTick: Limit BUY fills on BID_ASK when ask <= limit", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Limit, core::OrderSide::Buy,
                          100.0, 150.00, 0, 0, placedAt));
    auto r = EvaluateTick(book, bidAskTick(rthTime(10, 1), 149.50, 149.90));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 149.90);
    CHECK(r.fills[0].intentNote == "limit fill");
}

TEST_CASE("EvaluateTick: Limit SELL fills on BID_ASK when bid >= limit", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Limit, core::OrderSide::Sell,
                          100.0, 149.50, 0, 0, placedAt));
    auto r = EvaluateTick(book, bidAskTick(rthTime(10, 1), 149.90, 150.10));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 149.90);  // fills at bid
}

TEST_CASE("EvaluateTick: Stop triggers on TRADES tick", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Stop, core::OrderSide::Buy,
                          100.0, 0, 151.0, 0, placedAt));
    auto r = EvaluateTick(book, tradeTick(rthTime(10, 1), 151.50));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 151.50);
    CHECK(r.fills[0].intentNote == "stop trigger");
}

TEST_CASE("EvaluateTick: StopLimit stop on TRADES then limit on BID_ASK", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::StopLimit, core::OrderSide::Buy,
                          100.0, 152.0, 151.0, 0, placedAt));
    // Stop leg triggers on TRADES
    auto r1 = EvaluateTick(book, tradeTick(rthTime(10, 1), 151.50));
    CHECK(r1.fills.empty());
    CHECK(book[0].stopTriggered == true);
    // Limit leg fills on BID_ASK
    auto r2 = EvaluateTick(book, bidAskTick(rthTime(10, 2), 151.50, 151.80));
    REQUIRE(r2.fills.size() == 1);
    CHECK(r2.fills[0].price == 151.80);
    CHECK(r2.fills[0].intentNote == "stop-limit fill");
}

TEST_CASE("EvaluateTick: MIT triggers on TRADES tick", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::MIT, core::OrderSide::Buy,
                          100.0, 0, 0, 151.0, placedAt));
    auto r = EvaluateTick(book, tradeTick(rthTime(10, 1), 151.25));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].intentNote == "MIT trigger");
}

TEST_CASE("EvaluateTick: LIT MIT leg then limit leg", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::LIT, core::OrderSide::Buy,
                          100.0, 151.0, 0, 150.0, placedAt));
    // MIT leg triggers on TRADES
    auto r1 = EvaluateTick(book, tradeTick(rthTime(10, 1), 150.50));
    CHECK(r1.fills.empty());
    CHECK(book[0].stopTriggered == true);
    // Limit leg fills on BID_ASK
    auto r2 = EvaluateTick(book, bidAskTick(rthTime(10, 2), 150.20, 150.80));
    REQUIRE(r2.fills.size() == 1);
    CHECK(r2.fills[0].intentNote == "LIT fill");
}

TEST_CASE("EvaluateTick: Midprice fills on MIDPOINT tick", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Midprice, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto r = EvaluateTick(book, midTick(rthTime(10, 1), 150.50));
    REQUIRE(r.fills.size() == 1);
    CHECK(r.fills[0].price == 150.50);
    CHECK(r.fills[0].intentNote == "midprice fill");
}

TEST_CASE("EvaluateTick: single call does not double-fill same order", "[replay][evaluatetick]") {
    SimulatedOrderBook book;
    auto placedAt = rthTime(10, 0);
    book.push_back(makeWO(1, core::OrderType::Market, core::OrderSide::Buy,
                          100.0, 0, 0, 0, placedAt));
    auto r = EvaluateTick(book, tradeTick(rthTime(10, 1), 150.25));
    REQUIRE(r.fills.size() == 1);
    REQUIRE(r.filledIds.size() == 1);
}

// ============================================================================
// SimulatedAccount — ApplyFill
// ============================================================================

TEST_CASE("ApplyFill: BUY opens new long position", "[replay][account]") {
    SimulatedAccount acct;
    SimulatedFill f{0,"AAPL", core::OrderSide::Buy, 100.0, 150.0, 1.0, 1, ""};
    ApplyFill(acct, f);
    // cash: 100000 - 15000 (cost) - 1 (comm) = 84999
    CHECK(acct.cash == 84999.0);
    CHECK(acct.commissionPaid == 1.0);
    REQUIRE(acct.positions.count("AAPL") == 1);
    CHECK(acct.positions["AAPL"].qty == 100.0);
    CHECK(acct.positions["AAPL"].avgCost == 150.0);
    CHECK(acct.realizedPnL == 0.0);
}

TEST_CASE("ApplyFill: BUY adds to existing long position", "[replay][account]") {
    SimulatedAccount acct;
    acct.positions["AAPL"] = {0, "AAPL", 100.0, 150.0};
    acct.cash = 84999.0;  // 100000 - 15000 - 1
    SimulatedFill f{0,"AAPL", core::OrderSide::Buy, 50.0, 160.0, 0.5, 2, ""};
    ApplyFill(acct, f);
    CHECK(acct.positions["AAPL"].qty == 150.0);
    CHECK(acct.positions["AAPL"].avgCost == Catch::Approx(153.333).margin(0.001));
    CHECK(acct.realizedPnL == 0.0);
}

TEST_CASE("ApplyFill: SELL closes long position fully", "[replay][account]") {
    SimulatedAccount acct;
    acct.positions["AAPL"] = {0, "AAPL", 100.0, 150.0};
    acct.cash = 84999.0;  // 100000 - 15000 - 1 comm from buy
    SimulatedFill f{0,"AAPL", core::OrderSide::Sell, 100.0, 155.0, 1.0, 2, ""};
    ApplyFill(acct, f);
    // cash: 84999 + 15500 (proceeds) - 1 (comm) = 100498
    // realizedPnL: 100 * (155 - 150) = 500
    CHECK(acct.cash == 100498.0);
    CHECK(acct.realizedPnL == 500.0);
    CHECK(acct.positions.count("AAPL") == 0);
}

TEST_CASE("ApplyFill: SELL partially closes long position", "[replay][account]") {
    SimulatedAccount acct;
    acct.positions["AAPL"] = {0, "AAPL", 100.0, 150.0};
    acct.cash = 84999.0;
    SimulatedFill f{0,"AAPL", core::OrderSide::Sell, 40.0, 155.0, 0.4, 2, ""};
    ApplyFill(acct, f);
    // proceeds: 40*155=6200. cash: 84999+6200-0.4=91198.6
    // pnl: 40*(155-150)=200
    CHECK(acct.cash == Catch::Approx(91198.6));
    CHECK(acct.realizedPnL == 200.0);
    CHECK(acct.positions["AAPL"].qty == 60.0);
    CHECK(acct.positions["AAPL"].avgCost == 150.0);  // unchanged for partial close
}

TEST_CASE("ApplyFill: SELL flips long to short", "[replay][account]") {
    SimulatedAccount acct;
    acct.positions["AAPL"] = {0, "AAPL", 100.0, 150.0};
    acct.cash = 84999.0;
    // Sell 150 shares: close 100 at profit + open 50 short
    SimulatedFill f{0,"AAPL", core::OrderSide::Sell, 150.0, 155.0, 1.5, 2, ""};
    ApplyFill(acct, f);
    // proceeds: 150*155=23250. cash: 84999+23250-1.5=108247.5
    // close pnl: 100*(155-150)=500
    CHECK(acct.realizedPnL == 500.0);
    REQUIRE(acct.positions.count("AAPL") == 1);
    CHECK(acct.positions["AAPL"].qty == -50.0);
    CHECK(acct.positions["AAPL"].avgCost == 155.0);
}

TEST_CASE("ApplyFill: BUY closes short position fully", "[replay][account]") {
    SimulatedAccount acct;
    // Short 100 shares at 155: cash += 15500, position = -100
    acct.positions["AAPL"] = {0, "AAPL", -100.0, 155.0};
    acct.cash = 115499.0;  // 100000 + 15500 - 1
    acct.realizedPnL = 0.0;
    // Buy 100 shares at 145 to cover
    SimulatedFill f{0,"AAPL", core::OrderSide::Buy, 100.0, 145.0, 1.0, 2, ""};
    ApplyFill(acct, f);
    // cost: 14500. cash: 115499 - 14500 - 1 = 100998
    // pnl: 100*(155-145)=1000
    CHECK(acct.cash == 100998.0);
    CHECK(acct.realizedPnL == 1000.0);
    CHECK(acct.positions.count("AAPL") == 0);
}

// ============================================================================
// SimulatedAccount — Equity / UnrealizedPnL / Reset
// ============================================================================

TEST_CASE("Equity: cash plus position mark-to-market", "[replay][account]") {
    SimulatedAccount acct;
    acct.cash = 80000.0;
    acct.positions["AAPL"] = {0, "AAPL", 100.0, 150.0};
    CHECK(Equity(acct, 155.0) == 80000.0 + 15500.0);
}

TEST_CASE("Equity: short position reduces equity when price rises", "[replay][account]") {
    SimulatedAccount acct;
    acct.cash = 115000.0;
    acct.positions["AAPL"] = {0, "AAPL", -100.0, 150.0};
    // Position value = -100 * 155 = -15500. Equity = 115000 - 15500 = 99500
    CHECK(Equity(acct, 155.0) == 99500.0);
}

TEST_CASE("UnrealizedPnL: long position", "[replay][account]") {
    SimulatedAccount acct;
    acct.positions["AAPL"] = {0, "AAPL", 100.0, 150.0};
    CHECK(UnrealizedPnL(acct, 155.0) == 500.0);   // 100*(155-150)
    CHECK(UnrealizedPnL(acct, 145.0) == -500.0);
}

TEST_CASE("UnrealizedPnL: short position", "[replay][account]") {
    SimulatedAccount acct;
    acct.positions["AAPL"] = {0, "AAPL", -100.0, 150.0};
    CHECK(UnrealizedPnL(acct, 145.0) == 500.0);   // -100*(145-150)=500
    CHECK(UnrealizedPnL(acct, 155.0) == -500.0);
}

TEST_CASE("Reset: clears everything and restores startingCash", "[replay][account]") {
    SimulatedAccount acct;
    acct.startingCash = 50000.0;
    acct.cash = 42300.0;
    acct.positions["AAPL"] = {0, "AAPL", 50.0, 148.0};
    acct.fills.push_back({});
    acct.realizedPnL = 1200.0;
    acct.commissionPaid = 15.0;
    Reset(acct, 75000.0);
    CHECK(acct.startingCash == 75000.0);
    CHECK(acct.cash == 75000.0);
    CHECK(acct.positions.empty());
    CHECK(acct.fills.empty());
    CHECK(acct.realizedPnL == 0.0);
    CHECK(acct.commissionPaid == 0.0);
}

// ============================================================================
// [replay-range] — multi-day date range tests for the per-day session detection
// + back-compat single-day path. See .claude/plans/replay-indicators.md §2c.4.
// ============================================================================

namespace {

// Build a synthetic 5-trading-day intraday-only bar series. Each "day" gets
// one bar at 09:30 ET, one at 12:00 ET, one at 15:59 ET. (3 bars/day × 5 = 15.)
// Day 1 is 2026-04-13 (Mon), through day 5 = 2026-04-17 (Fri) — all weekdays
// EDT, so 09:30 ET == 13:30 UTC, 12:00 ET == 16:00 UTC, 15:59 ET == 19:59 UTC.
std::vector<core::Bar> buildFiveDayIntradayBars() {
    std::vector<core::Bar> bars;
    static const int kDays[] = {13, 14, 15, 16, 17};
    for (int d : kDays) {
        bars.push_back(makeBar(utc(2026, 4, d, 13, 30), 100, 101, 99, 100.5));
        bars.push_back(makeBar(utc(2026, 4, d, 16,  0), 100, 101, 99, 100.5));
        bars.push_back(makeBar(utc(2026, 4, d, 19, 59), 100, 101, 99, 100.5));
    }
    return bars;
}

}  // namespace

TEST_CASE("BarRangeForSession spans 5 days end-to-end (Intraday)",
          "[replay][replay-range]") {
    auto bars = buildFiveDayIntradayBars();
    REQUIRE(bars.size() == 15);
    auto r = BarRangeForSession(bars, ReplaySession::Intraday);
    // First bar of day 1 (idx 0) … last bar of day 5 (idx 14).
    CHECK(r.firstIdx == 0);
    CHECK(r.lastIdx == 14);
}

TEST_CASE("SeekToTime crosses day boundary correctly", "[replay][replay-range]") {
    auto bars = buildFiveDayIntradayBars();
    ReplayClock clock;
    clock.sessionFirstIdx = 0;
    clock.sessionLastIdx  = (int)bars.size() - 1;
    // Target = 12:00 ET on day 3 (April 15) → idx 7 (5th-from-start: day3 09:30=6, 12:00=7).
    SeekToTime(clock, utc(2026, 4, 15, 16, 0), bars);
    CHECK(clock.cursorBarIdx == 7);
    // Snap helper agrees.
    int idx = SnapCursorToNearestBar(utc(2026, 4, 15, 16, 0), bars);
    CHECK(idx == 7);
}

TEST_CASE("SnapCursorToNearestBar picks correct day across multi-day range",
          "[replay][replay-range]") {
    auto bars = buildFiveDayIntradayBars();
    // Day-2 09:30 ET → idx 3 (after day1's 3 bars).
    int day2_open = SnapCursorToNearestBar(utc(2026, 4, 14, 13, 30), bars);
    CHECK(day2_open == 3);
    // Day-5 15:59 ET → idx 14 (last).
    int day5_close = SnapCursorToNearestBar(utc(2026, 4, 17, 19, 59), bars);
    CHECK(day5_close == 14);
    // 14:00 UTC on day-3 (between 13:30 and 16:00 of that day) — closer to 13:30.
    // |14:00 - 13:30| = 30min vs |14:00 - 16:00| = 120min → snap to 13:30 (idx 6).
    int day3_mid = SnapCursorToNearestBar(utc(2026, 4, 15, 14, 0), bars);
    CHECK(day3_mid == 6);
}

TEST_CASE("Single-day range matches existing single-day behaviour",
          "[replay][replay-range]") {
    // Just day 1 of the 5-day series — same shape as legacy single-day load.
    std::vector<core::Bar> bars;
    bars.push_back(makeBar(utc(2026, 4, 13, 13, 30), 100, 101, 99, 100.5));
    bars.push_back(makeBar(utc(2026, 4, 13, 16,  0), 100, 101, 99, 100.5));
    bars.push_back(makeBar(utc(2026, 4, 13, 19, 59), 100, 101, 99, 100.5));
    auto r = BarRangeForSession(bars, ReplaySession::Intraday);
    CHECK(r.firstIdx == 0);
    CHECK(r.lastIdx == 2);
    // Snap to the open of that day.
    CHECK(SnapCursorToNearestBar(utc(2026, 4, 13, 13, 30), bars) == 0);
    CHECK(SnapCursorToNearestBar(utc(2026, 4, 13, 19, 59), bars) == 2);
}

TEST_CASE("HistoricalRange / HistoricalDay alias both compile",
          "[replay][replay-range]") {
    core::HistoricalRange r;
    r.symbol   = "AAPL";
    r.dateFrom = "2026-04-13";
    r.dateTo   = "2026-04-17";
    REQUIRE(r.dateFrom == "2026-04-13");
    REQUIRE(r.dateTo   == "2026-04-17");

    // Legacy alias still works.
    core::HistoricalDay d;
    d.dateFrom = "2026-04-15";
    d.dateTo   = "2026-04-15";
    REQUIRE(d.dateFrom == d.dateTo);   // single-day case
    REQUIRE(d.ticksDate.empty());      // default — no ticks loaded
}
