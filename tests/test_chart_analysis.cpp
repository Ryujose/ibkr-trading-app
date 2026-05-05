#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "core/services/ChartAnalysis.h"
#include "core/services/TradingStyle.h"

using core::services::ATR;
using core::services::AutoFibSpan;
using core::services::AvoidRoundNumber;
using core::services::BollingerBands;
using core::services::BreakoutMark;
using core::services::ComputeBollinger;
using core::services::EMA;
using core::services::RSI;
using core::services::SMA;
using core::services::ClassicPivots;
using core::services::ClusterLevels;
using core::services::DailyPivot;
using core::services::DonchianBands;
using core::services::DonchianResult;
using core::services::FindBreakouts;
using core::services::FindSwings;
using core::services::KeepTopN;
using core::services::LargestSwingSpan;
using core::services::Level;
using core::services::LevelSide;
using core::services::LinearRegression;
using core::services::PositionSizeShares;
using core::services::PositionStop;
using core::services::RoundToTick;
using core::services::SetupPlan;
using core::services::ApplyPreset;
using core::services::GetPreset;
using core::services::SessionVwap;
using core::services::StylePreset;
using core::services::SuggestSetup;
using core::services::SuggestStopForPosition;
using core::services::Swing;
using core::services::TradingStyle;
using core::services::TradingStyleLabel;
using core::services::TradingStyleShort;
using core::services::TrendFit;
using core::services::VwapResult;
using core::services::ComputeOrderImpact;
using core::services::OrderImpact;
using core::services::OrderImpactKind;
using core::services::PreviewStopTarget;
using core::services::StopTargetPreview;

// ── FindSwings ───────────────────────────────────────────────────────────────

TEST_CASE("FindSwings: returns nothing when input is too small", "[analysis][swings]") {
    std::vector<double> highs = {1, 2, 3};
    std::vector<double> lows  = {0, 0, 0};
    auto r = FindSwings(highs, lows, 3);
    REQUIRE(r.highs.empty());
    REQUIRE(r.lows.empty());
}

TEST_CASE("FindSwings: simple peak at center is found as swing high", "[analysis][swings]") {
    //                idx:   0    1    2    3    4    5    6
    std::vector<double> H = { 10, 11,  12, 15,  12,  11,  10 };
    std::vector<double> L = { 10, 11,  12, 15,  12,  11,  10 };
    auto r = FindSwings(H, L, 3);
    REQUIRE(r.highs.size() == 1);
    REQUIRE(r.highs[0].idx   == 3);
    REQUIRE(r.highs[0].price == 15.0);
}

TEST_CASE("FindSwings: simple trough is found as swing low", "[analysis][swings]") {
    std::vector<double> H = { 10,  9,  8,  5,  8,  9, 10 };
    std::vector<double> L = { 10,  9,  8,  5,  8,  9, 10 };
    auto r = FindSwings(H, L, 3);
    REQUIRE(r.lows.size() == 1);
    REQUIRE(r.lows[0].idx   == 3);
    REQUIRE(r.lows[0].price == 5.0);
}

TEST_CASE("FindSwings: W-shape produces two lows and a high in the middle", "[analysis][swings]") {
    //                idx:   0  1  2  3  4  5  6  7  8  9 10
    std::vector<double> H = { 5, 4, 3, 2, 3, 6, 3, 2, 3, 4, 5 };
    std::vector<double> L = { 5, 4, 3, 2, 3, 6, 3, 2, 3, 4, 5 };
    auto r = FindSwings(H, L, 2);
    REQUIRE(r.lows.size()  >= 2);
    REQUIRE(r.highs.size() >= 1);
    bool sawLeftLow  = false, sawRightLow = false, sawMidHigh = false;
    for (const auto& s : r.lows)  { if (s.idx == 3) sawLeftLow  = true; if (s.idx == 7) sawRightLow = true; }
    for (const auto& s : r.highs) { if (s.idx == 5) sawMidHigh  = true; }
    REQUIRE(sawLeftLow);
    REQUIRE(sawRightLow);
    REQUIRE(sawMidHigh);
}

TEST_CASE("FindSwings: respects scanCap to limit the right-side region", "[analysis][swings]") {
    // Peak at idx=3 (early). With scanCap=3 we should not detect it.
    std::vector<double> H(20, 1.0);
    std::vector<double> L(20, 1.0);
    H[3] = 5.0;
    auto rFull = FindSwings(H, L, 2, 0);
    auto rCap  = FindSwings(H, L, 2, 3);
    REQUIRE(!rFull.highs.empty());
    REQUIRE(rCap.highs.empty());
}

TEST_CASE("FindSwings: flat top counts as a single swing high", "[analysis][swings]") {
    std::vector<double> H = { 1, 2, 3, 5, 5, 3, 2, 1 };
    std::vector<double> L = { 1, 2, 3, 5, 5, 3, 2, 1 };
    auto r = FindSwings(H, L, 2);
    // Only the LEFT-most equal high qualifies as a swing because the strict ">=" on
    // the right side rejects subsequent equal bars: high[i+1] == high[i] is allowed,
    // but the next bar must not be greater. Validate at least one and at most one.
    int hits = 0;
    for (const auto& s : r.highs) if (s.price == 5.0) ++hits;
    REQUIRE(hits == 1);
}

// ── ATR ─────────────────────────────────────────────────────────────────────

TEST_CASE("ATR: returns zeros when input is too small", "[analysis][atr]") {
    std::vector<double> H(10, 1.0), L(10, 0.5), C(10, 0.75);
    auto a = ATR(H, L, C, 14);
    REQUIRE(a.size() == 10);
    for (double v : a) REQUIRE(v == 0.0);
}

TEST_CASE("ATR: constant true range yields constant ATR", "[analysis][atr]") {
    int n = 30;
    std::vector<double> H(n), L(n), C(n);
    for (int i = 0; i < n; ++i) {
        H[i] = 101.0;
        L[i] = 100.0;
        C[i] = 100.5;
    }
    auto a = ATR(H, L, C, 14);
    REQUIRE(a[14] == Catch::Approx(1.0));
    REQUIRE(a[29] == Catch::Approx(1.0));
}

// ── ClusterLevels ───────────────────────────────────────────────────────────

TEST_CASE("ClusterLevels: empty input gives empty output", "[analysis][cluster]") {
    auto r = ClusterLevels({}, 1.0);
    REQUIRE(r.empty());
}

TEST_CASE("ClusterLevels: prices within tolerance merge into one cluster", "[analysis][cluster]") {
    std::vector<Swing> in = { {1, 100.00}, {3, 100.05}, {7, 100.10} };
    auto out = ClusterLevels(in, 0.20);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].touches  == 3);
    REQUIRE(out[0].firstIdx == 1);
    REQUIRE(out[0].lastIdx  == 7);
    REQUIRE(out[0].price    == Catch::Approx(100.05));
    REQUIRE(out[0].minPrice == Catch::Approx(100.00));
    REQUIRE(out[0].maxPrice == Catch::Approx(100.10));
}

TEST_CASE("ClusterLevels: prices outside tolerance produce separate clusters", "[analysis][cluster]") {
    std::vector<Swing> in = { {1, 100.0}, {2, 105.0}, {3, 110.0} };
    auto out = ClusterLevels(in, 0.5);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].price == 100.0);
    REQUIRE(out[1].price == 105.0);
    REQUIRE(out[2].price == 110.0);
    for (const auto& c : out) {
        REQUIRE(c.touches  == 1);
        REQUIRE(c.minPrice == c.price);  // single-swing cluster: min == max == price
        REQUIRE(c.maxPrice == c.price);
    }
}

TEST_CASE("ClusterLevels: minPrice / maxPrice track constituent swing prices", "[analysis][cluster]") {
    // Mixed input order — make sure minPrice/maxPrice come from the constituents,
    // not the sort/iteration order.
    std::vector<Swing> in = { {2, 100.10}, {0, 99.95}, {1, 100.05}, {3, 100.20} };
    auto out = ClusterLevels(in, 0.50);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].touches  == 4);
    REQUIRE(out[0].minPrice == Catch::Approx(99.95));
    REQUIRE(out[0].maxPrice == Catch::Approx(100.20));
    REQUIRE(out[0].minPrice <= out[0].price);
    REQUIRE(out[0].maxPrice >= out[0].price);
}

TEST_CASE("ClusterLevels: returns clusters in ascending price order", "[analysis][cluster]") {
    std::vector<Swing> in = { {1, 110.0}, {2, 100.0}, {3, 105.0} };
    auto out = ClusterLevels(in, 0.5);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].price < out[1].price);
    REQUIRE(out[1].price < out[2].price);
}

// ── KeepTopN ────────────────────────────────────────────────────────────────

TEST_CASE("KeepTopN: filters by side and minimum touches", "[analysis][topn]") {
    std::vector<Level> in = {
        { 90.0, 3,  0,  10,  90.0,  90.0 },   // below — keep as support
        { 95.0, 1,  0,  10,  95.0,  95.0 },   // below but only 1 touch — drop
        {105.0, 2,  0,  10, 105.0, 105.0 },   // above — keep as resistance
        {110.0, 1,  0,  10, 110.0, 110.0 },   // above but only 1 touch — drop
    };
    auto sup = KeepTopN(in, 100.0, LevelSide::Below, 2, 5);
    auto res = KeepTopN(in, 100.0, LevelSide::Above, 2, 5);
    REQUIRE(sup.size() == 1);
    REQUIRE(sup[0].price == 90.0);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].price == 105.0);
}

TEST_CASE("KeepTopN: caps at maxLevels, ranking by touches then recency",
          "[analysis][topn]") {
    std::vector<Level> in = {
        { 90.0, 4, 0,  20, 90.0, 90.0 },
        { 92.0, 4, 0,  50, 92.0, 92.0 },   // same touches, later — should rank above 90.0
        { 95.0, 3, 0, 100, 95.0, 95.0 },
        { 80.0, 5, 0,  10, 80.0, 80.0 },   // most touches — should be first
    };
    auto out = KeepTopN(in, 100.0, LevelSide::Below, 2, 3);
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].price == 80.0);   // 5 touches
    REQUIRE(out[1].price == 92.0);   // 4 touches, newer
    REQUIRE(out[2].price == 90.0);   // 4 touches, older
}

TEST_CASE("KeepTopN: maxLevels=0 means no cap", "[analysis][topn]") {
    std::vector<Level> in = {
        { 80.0, 2, 0, 0, 80.0, 80.0 },
        { 85.0, 2, 0, 0, 85.0, 85.0 },
        { 90.0, 2, 0, 0, 90.0, 90.0 },
    };
    auto out = KeepTopN(in, 100.0, LevelSide::Below, 2, 0);
    REQUIRE(out.size() == 3);
}

// ── LinearRegression ────────────────────────────────────────────────────────

TEST_CASE("LinearRegression: too-small input returns valid=false", "[analysis][trend]") {
    std::vector<double> c = { 1.0, 2.0, 3.0 };
    auto r = LinearRegression(c, 10);
    REQUIRE_FALSE(r.valid);
    auto r2 = LinearRegression(c, 1);
    REQUIRE_FALSE(r2.valid);
    auto r3 = LinearRegression({}, 5);
    REQUIRE_FALSE(r3.valid);
}

TEST_CASE("LinearRegression: y = 2x + 5 recovers slope, intercept, sigma=0",
          "[analysis][trend]") {
    int n = 50;
    std::vector<double> c(n);
    for (int i = 0; i < n; ++i) c[i] = 2.0 * i + 5.0;
    auto r = LinearRegression(c, n);
    REQUIRE(r.valid);
    REQUIRE(r.slope     == Catch::Approx(2.0));
    REQUIRE(r.intercept == Catch::Approx(5.0));
    REQUIRE(r.sigma     == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(r.firstIdx  == 0);
    REQUIRE(r.lastIdx   == n - 1);
}

TEST_CASE("LinearRegression: lookback < n fits the trailing window", "[analysis][trend]") {
    // closes[i] = 3*i + 1 for i=0..99; fit only the last 30 points.
    int n = 100;
    std::vector<double> c(n);
    for (int i = 0; i < n; ++i) c[i] = 3.0 * i + 1.0;
    auto r = LinearRegression(c, 30);
    REQUIRE(r.valid);
    REQUIRE(r.firstIdx == n - 30);
    REQUIRE(r.lastIdx  == n - 1);
    REQUIRE(r.slope     == Catch::Approx(3.0));
    REQUIRE(r.intercept == Catch::Approx(1.0));
    REQUIRE(r.sigma     == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("LinearRegression: flat line yields slope approximately 0", "[analysis][trend]") {
    std::vector<double> c(40, 100.0);
    auto r = LinearRegression(c, 40);
    REQUIRE(r.valid);
    REQUIRE(r.slope     == Catch::Approx(0.0).margin(1e-12));
    REQUIRE(r.intercept == Catch::Approx(100.0));
    REQUIRE(r.sigma     == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("LinearRegression: noisy line gives non-zero sigma but recovers slope",
          "[analysis][trend]") {
    // y = x + noise where noise pattern (+1, -1, -1, +1) has period 4 and
    // exactly zero covariance with x in each period — so the OLS slope is
    // unbiased while sigma stays at 1.
    int n = 48; // multiple of 4
    static constexpr double kNoise[4] = { 1.0, -1.0, -1.0, 1.0 };
    std::vector<double> c(n);
    for (int i = 0; i < n; ++i) c[i] = i + kNoise[i % 4];
    auto r = LinearRegression(c, n);
    REQUIRE(r.valid);
    REQUIRE(r.slope == Catch::Approx(1.0).margin(1e-9));
    REQUIRE(r.sigma == Catch::Approx(1.0).margin(1e-9));
}

// ── End-to-end: V-shape series exercises the full pipeline ──────────────────

TEST_CASE("Full pipeline: V-shape yields one support cluster below current price",
          "[analysis][pipeline]") {
    // Synthesise three V-shapes with a shared low at ~100.
    //   bar 0..40 fall to 100, rise to 120
    //   second V around bar 40..80
    //   third V around bar 80..120, ending at 118 (above the 100 lows)
    int n = 121;
    std::vector<double> H(n), L(n), C(n);
    for (int i = 0; i < n; ++i) {
        int phase = i % 40;
        double base;
        if (phase < 20) base = 120.0 - (phase * 1.0);             // 120 → 100
        else            base = 100.0 + ((phase - 20) * 1.0);      // 100 → 120
        H[i] = base + 0.3;
        L[i] = base - 0.3;
        C[i] = base;
    }
    // End the series above the support so KeepTopN(Below) keeps the cluster.
    C[n - 1] = 118.0;

    auto sw       = FindSwings(H, L, 3);
    REQUIRE(sw.lows.size() >= 2);
    auto clusters = ClusterLevels(sw.lows, 1.0);
    auto sup      = KeepTopN(clusters, C.back(), LevelSide::Below, 2, 3);
    REQUIRE(!sup.empty());
    REQUIRE(sup[0].price == Catch::Approx(100.0).margin(1.0));
    REQUIRE(sup[0].touches >= 2);

    // Zone bounds must agree with constituent swings: minPrice and maxPrice
    // should bracket the mean cluster price (Task D in the auto-analysis plan).
    REQUIRE(sup[0].minPrice <= sup[0].price);
    REQUIRE(sup[0].maxPrice >= sup[0].price);

    // Every cluster's min/max must agree with the actual swing constituents that
    // landed inside it.
    for (const auto& c : clusters) {
        double minSeen =  std::numeric_limits<double>::infinity();
        double maxSeen = -std::numeric_limits<double>::infinity();
        int    seen    = 0;
        for (const auto& s : sw.lows) {
            if (std::abs(s.price - c.price) <= 1.0 + 1e-9) {
                minSeen = std::min(minSeen, s.price);
                maxSeen = std::max(maxSeen, s.price);
                ++seen;
            }
        }
        if (seen == c.touches) {  // unambiguous attribution
            REQUIRE(c.minPrice == Catch::Approx(minSeen));
            REQUIRE(c.maxPrice == Catch::Approx(maxSeen));
        }
    }
}

// ── Donchian ────────────────────────────────────────────────────────────────

TEST_CASE("DonchianBands: too-small input returns zeros", "[analysis][donchian]") {
    std::vector<double> H = {1, 2};
    std::vector<double> L = {1, 2};
    auto d = DonchianBands(H, L, 5);
    REQUIRE(d.hi.size() == 2);
    REQUIRE(d.lo.size() == 2);
    for (double v : d.hi) REQUIRE(v == 0.0);
    for (double v : d.lo) REQUIRE(v == 0.0);
}

TEST_CASE("DonchianBands: rolling max/min over a known sequence",
          "[analysis][donchian]") {
    //                     idx:  0  1  2  3  4  5  6  7  8
    std::vector<double> H = {    1, 2, 3, 4, 5, 4, 3, 2, 1 };
    std::vector<double> L = {    1, 2, 3, 4, 5, 4, 3, 2, 1 };
    auto d = DonchianBands(H, L, 3);
    REQUIRE(d.hi.size() == 9);
    // Entries before N-1 should stay at 0.
    REQUIRE(d.hi[0] == 0.0);
    REQUIRE(d.hi[1] == 0.0);
    // From idx N-1 = 2 onwards: rolling max(H[i-2..i])
    REQUIRE(d.hi[2] == 3.0);   // max(1,2,3)
    REQUIRE(d.hi[3] == 4.0);   // max(2,3,4)
    REQUIRE(d.hi[4] == 5.0);   // max(3,4,5)
    REQUIRE(d.hi[5] == 5.0);   // max(4,5,4)
    REQUIRE(d.hi[6] == 5.0);   // max(5,4,3)
    REQUIRE(d.hi[7] == 4.0);   // max(4,3,2)
    REQUIRE(d.hi[8] == 3.0);   // max(3,2,1)

    REQUIRE(d.lo[2] == 1.0);   // min(1,2,3)
    REQUIRE(d.lo[3] == 2.0);
    REQUIRE(d.lo[4] == 3.0);
    REQUIRE(d.lo[8] == 1.0);
}

TEST_CASE("DonchianBands: window equals series length collapses to a single value",
          "[analysis][donchian]") {
    std::vector<double> H = {3, 5, 7, 6, 4};
    std::vector<double> L = {1, 2, 4, 3, 1};
    auto d = DonchianBands(H, L, 5);
    REQUIRE(d.hi.size() == 5);
    REQUIRE(d.hi[4] == 7.0);
    REQUIRE(d.lo[4] == 1.0);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(d.hi[i] == 0.0);
        REQUIRE(d.lo[i] == 0.0);
    }
}

// ── LargestSwingSpan ────────────────────────────────────────────────────────

TEST_CASE("LargestSwingSpan: picks max-span pair across all considered swings",
          "[analysis][autofib]") {
    // Pairs span (high − low):
    //   (100, 95)=5  (100, 90)=10 (100, 98)=2
    //   (110, 95)=15 (110, 90)=20  ← max
    //   (110, 98)=12
    //   (105, 95)=10 (105, 90)=15 (105, 98)=7
    std::vector<Swing> H = { {0, 100.0}, {10, 110.0}, {20, 105.0} };
    std::vector<Swing> L = { {5,  95.0}, {15,  90.0}, {25,  98.0} };
    auto s = LargestSwingSpan(H, L, 30);
    REQUIRE(s.valid);
    REQUIRE(s.hiPrice == 110.0);
    REQUIRE(s.loPrice ==  90.0);
    REQUIRE(s.hiIdx   ==  10);
    REQUIRE(s.loIdx   ==  15);
}

TEST_CASE("LargestSwingSpan: window=1 only considers the most recent swing of each",
          "[analysis][autofib]") {
    std::vector<Swing> H = { {0, 110}, {10, 120}, {20, 100} };
    std::vector<Swing> L = { {5,  90}, {15,  95}, {25,  80} };
    auto s = LargestSwingSpan(H, L, 1);
    // Only H.back()=100 and L.back()=80 → span 20.
    REQUIRE(s.valid);
    REQUIRE(s.hiPrice == 100.0);
    REQUIRE(s.loPrice ==  80.0);
    REQUIRE(s.hiIdx   ==  20);
    REQUIRE(s.loIdx   ==  25);
}

TEST_CASE("LargestSwingSpan: empty list returns invalid", "[analysis][autofib]") {
    REQUIRE_FALSE(LargestSwingSpan({},               {{0, 100.0}}, 5).valid);
    REQUIRE_FALSE(LargestSwingSpan({{0, 100.0}},     {},           5).valid);
    REQUIRE_FALSE(LargestSwingSpan({},               {},           5).valid);
}

TEST_CASE("LargestSwingSpan: zero-span input stays invalid",
          "[analysis][autofib]") {
    std::vector<Swing> H = {{0, 100.0}, {1, 100.0}};
    std::vector<Swing> L = {{2, 100.0}};
    auto s = LargestSwingSpan(H, L, 5);
    REQUIRE_FALSE(s.valid);
}

// ── ClassicPivots ───────────────────────────────────────────────────────────

TEST_CASE("ClassicPivots: H=110 L=90 C=100 produces classic levels",
          "[analysis][pivots]") {
    auto p = ClassicPivots(110.0, 90.0, 100.0);
    REQUIRE(p.valid);
    REQUIRE(p.p  == Catch::Approx(100.0));
    REQUIRE(p.r1 == Catch::Approx(110.0));
    REQUIRE(p.r2 == Catch::Approx(120.0));
    REQUIRE(p.r3 == Catch::Approx(130.0));
    REQUIRE(p.s1 == Catch::Approx( 90.0));
    REQUIRE(p.s2 == Catch::Approx( 80.0));
    REQUIRE(p.s3 == Catch::Approx( 70.0));
}

TEST_CASE("ClassicPivots: invariants S3 < S2 < S1 < P < R1 < R2 < R3",
          "[analysis][pivots]") {
    auto p = ClassicPivots(127.50, 122.00, 125.30);
    REQUIRE(p.valid);
    REQUIRE(p.s3 < p.s2);
    REQUIRE(p.s2 < p.s1);
    REQUIRE(p.s1 < p.p);
    REQUIRE(p.p  < p.r1);
    REQUIRE(p.r1 < p.r2);
    REQUIRE(p.r2 < p.r3);
}

TEST_CASE("ClassicPivots: invalid input returns valid=false", "[analysis][pivots]") {
    REQUIRE_FALSE(ClassicPivots(  0.0,   0.0,   0.0).valid);
    REQUIRE_FALSE(ClassicPivots(-10.0, 100.0, 100.0).valid);
    REQUIRE_FALSE(ClassicPivots(100.0,  -1.0, 100.0).valid);
    REQUIRE_FALSE(ClassicPivots(100.0, 100.0,   0.0).valid);
    REQUIRE_FALSE(ClassicPivots( 80.0, 100.0, 100.0).valid);  // H < L
}

// ── FindBreakouts ───────────────────────────────────────────────────────────

TEST_CASE("FindBreakouts: synthetic up-breakout produces one mark on the right bar",
          "[analysis][breakout]") {
    int n = 30;
    std::vector<double> H(n), L(n), C(n), atr(n, 1.0);
    // Sideways around 100 then a breakout at idx 25.
    for (int i = 0; i < n; ++i) {
        if (i < 25) { H[i] = 101.0; L[i] = 99.0;  C[i] = 100.0; }
        else        { H[i] = 110.0; L[i] = 105.0; C[i] = 108.0; }
    }
    Level resistance{ 102.0, 3, 0, 10, 102.0, 102.0 };
    Level support   {  98.0, 3, 0, 10,  98.0,  98.0 };

    auto marks = FindBreakouts({resistance}, {support}, H, L, C, atr,
                               /*lookback=*/50, /*minTouches=*/2);
    REQUIRE(marks.size() == 1);
    REQUIRE(marks[0].idx == 25);
    REQUIRE(marks[0].up);
    REQUIRE(marks[0].y == Catch::Approx(110.0 + 0.5));  // high + 0.5*atr
}

TEST_CASE("FindBreakouts: minTouches filters out weak (single-touch) levels",
          "[analysis][breakout]") {
    std::vector<double> H = {99,  99,  99, 110, 110};
    std::vector<double> L = {97,  97,  97, 105, 105};
    std::vector<double> C = {98,  98,  98, 108, 108};
    std::vector<double> atr(5, 1.0);
    Level weakResistance{ 102.0, 1, 0, 0, 102.0, 102.0 };  // only 1 touch
    auto marks = FindBreakouts({weakResistance}, {}, H, L, C, atr, 50, 2);
    REQUIRE(marks.empty());
}

TEST_CASE("FindBreakouts: down-breakout below support",
          "[analysis][breakout]") {
    int n = 10;
    std::vector<double> H = {101, 101, 101, 101, 101, 101, 101, 101, 95, 92};
    std::vector<double> L = { 99,  99,  99,  99,  99,  99,  99,  99, 90, 88};
    std::vector<double> C = {100, 100, 100, 100, 100, 100, 100, 100, 92, 90};
    std::vector<double> atr(n, 1.0);
    Level support{ 98.0, 3, 0, 5, 98.0, 98.0 };
    auto marks = FindBreakouts({}, {support}, H, L, C, atr, 50, 2);
    REQUIRE(marks.size() == 1);
    REQUIRE(marks[0].idx == 8);
    REQUIRE_FALSE(marks[0].up);
    REQUIRE(marks[0].y == Catch::Approx(90.0 - 0.5));  // low - 0.5*atr
}

TEST_CASE("FindBreakouts: causality guard skips levels established after the bar",
          "[analysis][breakout]") {
    // Level "established" at firstIdx=20 cannot mark a breakout at idx 5.
    int n = 10;
    std::vector<double> H(n, 101.0), L(n, 99.0), C(n, 100.0), atr(n, 1.0);
    C[4] = 99.0;   // close at i=4
    C[5] = 105.0;  // close at i=5 — would cross 102 if level were causal
    Level futureResistance{ 102.0, 3, 20, 25, 102.0, 102.0 };
    auto marks = FindBreakouts({futureResistance}, {}, H, L, C, atr, 50, 2);
    REQUIRE(marks.empty());
}

TEST_CASE("FindBreakouts: empty inputs and degenerate parameters",
          "[analysis][breakout]") {
    REQUIRE(FindBreakouts({}, {}, {}, {}, {}, {}, 50, 2).empty());
    REQUIRE(FindBreakouts({}, {}, {1.0}, {1.0}, {1.0}, {1.0}, 50, 2).empty());
    std::vector<double> v(5, 1.0);
    REQUIRE(FindBreakouts({}, {}, v, v, v, v, 0, 2).empty());
}

// ── RoundToTick ─────────────────────────────────────────────────────────────

TEST_CASE("RoundToTick: snaps to nearest cent on $0.01 grid",
          "[analysis][setup]") {
    // Already-clean values pass through.
    REQUIRE(RoundToTick(187.42)        == Catch::Approx(187.42));
    REQUIRE(RoundToTick(10.00)         == Catch::Approx(10.00));
    // Sub-cent drift that AvoidRoundNumber leaks (`184 - 0.07 = 183.929999…`)
    // must snap back onto the $0.01 grid before going to IB.
    REQUIRE(RoundToTick(184.0 - 0.07)  == Catch::Approx(183.93));
    REQUIRE(RoundToTick(184.43 - 0.10) == Catch::Approx(184.33));
    // Half-up rounding on exact midpoints.
    REQUIRE(RoundToTick(100.005)       == Catch::Approx(100.01));
}

TEST_CASE("RoundToTick: respects custom tick sizes",
          "[analysis][setup]") {
    // $0.05 ticks (typical option contract).
    REQUIRE(RoundToTick(187.43, 0.05) == Catch::Approx(187.45));
    REQUIRE(RoundToTick(187.41, 0.05) == Catch::Approx(187.40));
    // $0.0001 ticks (sub-dollar stocks).
    REQUIRE(RoundToTick(0.12345, 0.0001) == Catch::Approx(0.1235));
}

TEST_CASE("RoundToTick: tick<=0 is a no-op",
          "[analysis][setup]") {
    REQUIRE(RoundToTick(187.4267, 0.0)  == Catch::Approx(187.4267));
    REQUIRE(RoundToTick(187.4267, -0.1) == Catch::Approx(187.4267));
}

// ── AvoidRoundNumber ────────────────────────────────────────────────────────

TEST_CASE("AvoidRoundNumber: returns input unchanged when already safe",
          "[analysis][setup]") {
    REQUIRE(AvoidRoundNumber(187.42, 0.07, true)  == 187.42);
    REQUIRE(AvoidRoundNumber(187.42, 0.07, false) == 187.42);
    // Comfortably outside pad on every side.
    REQUIRE(AvoidRoundNumber(187.10, 0.07, true)  == 187.10);
    REQUIRE(AvoidRoundNumber(187.40, 0.07, false) == 187.40);
}

TEST_CASE("AvoidRoundNumber: pushes price away from round marks",
          "[analysis][setup]") {
    // $10.00 → push down to $9.93
    REQUIRE(AvoidRoundNumber(10.00, 0.07, true)  == Catch::Approx( 9.93));
    REQUIRE(AvoidRoundNumber(10.00, 0.07, false) == Catch::Approx(10.07));
    // $187.50 (right on .50) → push to either side by pad
    REQUIRE(AvoidRoundNumber(187.50, 0.07, true)  == Catch::Approx(187.43));
    REQUIRE(AvoidRoundNumber(187.50, 0.07, false) == Catch::Approx(187.57));
    // $10.95 close to 1.00 → push up past next integer → 11.07
    REQUIRE(AvoidRoundNumber(10.95, 0.07, false) == Catch::Approx(11.07));
}

TEST_CASE("AvoidRoundNumber: degenerate inputs return unchanged",
          "[analysis][setup]") {
    REQUIRE(AvoidRoundNumber(50.00,  0.0, true) == 50.00);   // pad <= 0
    REQUIRE(AvoidRoundNumber(50.00, -0.1, true) == 50.00);
    REQUIRE(AvoidRoundNumber( 0.05, 0.07, true) ==  0.05);   // price <= pad
    // Sub-cent pad won't trip on a non-round price.
    REQUIRE(AvoidRoundNumber(187.42, 0.001, true) == Catch::Approx(187.42));
}

// ── SuggestSetup ────────────────────────────────────────────────────────────

TEST_CASE("SuggestSetup: long inside the zone uses zone midpoint as entry",
          "[analysis][setup]") {
    auto p = SuggestSetup(/*side=*/1,
                          /*zoneTop=*/187.0, /*zoneBot=*/185.0,
                          /*anchor=*/184.5,
                          /*opposingLevel=*/192.0,
                          /*atr=*/2.0, /*last=*/186.0,
                          /*atrPad=*/0.5, /*roundPad=*/0.07,
                          /*stopOffset=*/0.10, /*rrMin=*/2.0,
                          /*equity=*/0.0, /*riskPct=*/1.0);
    REQUIRE(p.valid);
    REQUIRE(p.side    == 1);
    REQUIRE(p.entry   == Catch::Approx(186.00));
    // raw stop = 184.5 - 0.5*2 = 183.50 (right on .50) → push down to 183.43
    REQUIRE(p.stop    == Catch::Approx(183.43));
    REQUIRE(p.stopLmt == Catch::Approx(183.33));
    REQUIRE(p.target  == Catch::Approx(192.00));
    REQUIRE(p.rr      == Catch::Approx(6.0 / (186.0 - 183.43)));
    REQUIRE(p.shares  == 0);   // equity = 0 → unknown
}

TEST_CASE("SuggestSetup: long with last past zoneTop chases at last price",
          "[analysis][setup]") {
    auto p = SuggestSetup(/*side=*/1,
                          /*zoneTop=*/187.0, /*zoneBot=*/185.0,
                          /*anchor=*/184.5,
                          /*opposingLevel=*/200.0,
                          /*atr=*/2.0, /*last=*/188.0,
                          /*atrPad=*/0.5, /*roundPad=*/0.07,
                          /*stopOffset=*/0.10, /*rrMin=*/2.0,
                          /*equity=*/100000.0, /*riskPct=*/1.0);
    REQUIRE(p.valid);
    REQUIRE(p.entry  == Catch::Approx(188.00));   // = last, not mid
    REQUIRE(p.stop   == Catch::Approx(183.43));
    REQUIRE(p.target == Catch::Approx(200.00));
    REQUIRE(p.rr     >= 2.0);
    // shares = floor(0.01 * 100000 / 4.57) = floor(218.8) = 218
    REQUIRE(p.shares == 218);
}

TEST_CASE("SuggestSetup: rejects when reward/risk falls below rrMin",
          "[analysis][setup]") {
    auto p = SuggestSetup(/*side=*/1,
                          /*zoneTop=*/187.0, /*zoneBot=*/185.0,
                          /*anchor=*/184.5,
                          /*opposingLevel=*/187.5,    // reward only ~1.5
                          /*atr=*/2.0, /*last=*/186.0,
                          /*atrPad=*/0.5, /*roundPad=*/0.07,
                          /*stopOffset=*/0.10, /*rrMin=*/2.0,
                          /*equity=*/0.0, /*riskPct=*/1.0);
    REQUIRE_FALSE(p.valid);
}

TEST_CASE("SuggestSetup: short setup mirrors long path with anchor on zone top",
          "[analysis][setup]") {
    auto p = SuggestSetup(/*side=*/0,
                          /*zoneTop=*/190.0, /*zoneBot=*/188.0,
                          /*anchor=*/190.5,           // = zone.maxPrice for short
                          /*opposingLevel=*/180.0,
                          /*atr=*/2.0, /*last=*/189.0,
                          /*atrPad=*/0.5, /*roundPad=*/0.07,
                          /*stopOffset=*/0.10, /*rrMin=*/2.0,
                          /*equity=*/0.0, /*riskPct=*/1.0);
    REQUIRE(p.valid);
    REQUIRE(p.side    == 0);
    REQUIRE(p.entry   == Catch::Approx(189.00));   // mid (last == mid here)
    // raw stop = 190.5 + 0.5*2 = 191.50 → push up to 191.57
    REQUIRE(p.stop    == Catch::Approx(191.57));
    REQUIRE(p.stopLmt == Catch::Approx(191.67));
    REQUIRE(p.target  == Catch::Approx(180.00));
    REQUIRE(p.rr      == Catch::Approx(9.0 / (191.57 - 189.0)));
}

TEST_CASE("SuggestSetup: degenerate input rejected", "[analysis][setup]") {
    // ATR = 0 → reject
    REQUIRE_FALSE(SuggestSetup(1, 187, 185, 184.5, 192, 0.0, 186,
                               0.5, 0.07, 0.10, 2.0, 0, 1).valid);
    // zoneTop ≤ zoneBot → reject
    REQUIRE_FALSE(SuggestSetup(1, 185, 187, 184.5, 192, 2.0, 186,
                               0.5, 0.07, 0.10, 2.0, 0, 1).valid);
    // Invalid side
    REQUIRE_FALSE(SuggestSetup(2, 187, 185, 184.5, 192, 2.0, 186,
                               0.5, 0.07, 0.10, 2.0, 0, 1).valid);
}

// ── SuggestStopForPosition ──────────────────────────────────────────────────

TEST_CASE("SuggestStopForPosition: long picks closest support below entry",
          "[analysis][setup]") {
    std::vector<Level> levels = {
        { 185.0, 3, 0, 10, 184.6, 185.4 },   // closer to entry
        { 180.0, 2, 0, 20, 179.5, 180.3 },
    };
    auto r = SuggestStopForPosition(/*isLong=*/true,
                                    /*entry=*/187.0, levels,
                                    /*atr=*/2.0,
                                    /*atrPad=*/0.5, /*roundPad=*/0.07,
                                    /*stopOffset=*/0.10);
    REQUIRE(r.valid);
    // raw stop = 184.6 - 0.5*2 = 183.6 → frac=.60, dist to .50 = .10 ≥ pad → unchanged
    REQUIRE(r.stop    == Catch::Approx(183.60));
    REQUIRE(r.stopLmt == Catch::Approx(183.50));
    REQUIRE(r.pctRisk == Catch::Approx((187.0 - 183.6) / 187.0 * 100.0));
}

TEST_CASE("SuggestStopForPosition: returns invalid when no qualifying level",
          "[analysis][setup]") {
    // Single-touch level — touches < 2, skipped.
    std::vector<Level> a = { { 185.0, 1, 0, 0, 185.0, 185.0 } };
    REQUIRE_FALSE(SuggestStopForPosition(true, 187.0, a, 2.0, 0.5, 0.07, 0.10).valid);

    // Empty levels.
    REQUIRE_FALSE(SuggestStopForPosition(true, 187.0, {}, 2.0, 0.5, 0.07, 0.10).valid);

    // Long: every level is at or above entry → no valid support.
    std::vector<Level> b = { { 200.0, 5, 0, 0, 199.5, 200.5 } };
    REQUIRE_FALSE(SuggestStopForPosition(true, 187.0, b, 2.0, 0.5, 0.07, 0.10).valid);
}

TEST_CASE("SuggestStopForPosition: short picks closest resistance above entry",
          "[analysis][setup]") {
    std::vector<Level> levels = {
        { 185.0, 3, 0, 10, 184.6, 185.4 },   // nearest resistance above entry=180
        { 200.0, 2, 0, 20, 199.5, 200.5 },
    };
    auto r = SuggestStopForPosition(/*isLong=*/false,
                                    /*entry=*/180.0, levels,
                                    /*atr=*/2.0,
                                    /*atrPad=*/0.5, /*roundPad=*/0.07,
                                    /*stopOffset=*/0.10);
    REQUIRE(r.valid);
    // raw stop = 185.4 + 0.5*2 = 186.4 → frac=.40, dist to .50 = .10 ≥ pad → unchanged
    REQUIRE(r.stop    == Catch::Approx(186.40));
    REQUIRE(r.stopLmt == Catch::Approx(186.50));
    REQUIRE(r.pctRisk == Catch::Approx((186.4 - 180.0) / 180.0 * 100.0));
}

TEST_CASE("SuggestStopForPosition: outputs are tick-clean (regression for IB error 110)",
          "[analysis][setup]") {
    // AvoidRoundNumber leaks ULP-level drift on inputs like `0.07` because that
    // literal is not exactly representable in IEEE-754. Without RoundToTick
    // the resulting stop/stopLmt arrive at IB as e.g. 182.92999999… and IB
    // rejects them with error 110 ("price does not conform to the minimum
    // price variation for this contract"). Both helpers must clamp output to
    // the $0.01 grid.
    auto onTick = [](double v) {
        return std::abs(v * 100.0 - std::round(v * 100.0)) < 1e-9;
    };

    // Long: rawStop = 184.0 - 0.5*2 = 183.0 (exact, lands ON the .00 mark) →
    // AvoidRoundNumber pushes by 0.07 → 182.929999999… (FP drift) → must round
    // back to 182.93.
    std::vector<Level> longLevels = { { 185.0, 3, 0, 10, 184.0, 185.5 } };
    auto rL = SuggestStopForPosition(true, 200.0, longLevels,
                                     2.0, 0.5, 0.07, 0.10);
    REQUIRE(rL.valid);
    REQUIRE(rL.stop    == Catch::Approx(182.93));
    REQUIRE(rL.stopLmt == Catch::Approx(182.83));
    REQUIRE(onTick(rL.stop));
    REQUIRE(onTick(rL.stopLmt));

    // Short mirror: rawStop = 185.0 + 1.0 = 186.0 → AvoidRoundNumber → 186.07
    // (drift) → round to 186.07.
    std::vector<Level> shortLevels = { { 185.0, 3, 0, 10, 184.5, 185.0 } };
    auto rS = SuggestStopForPosition(false, 180.0, shortLevels,
                                     2.0, 0.5, 0.07, 0.10);
    REQUIRE(rS.valid);
    REQUIRE(rS.stop    == Catch::Approx(186.07));
    REQUIRE(rS.stopLmt == Catch::Approx(186.17));
    REQUIRE(onTick(rS.stop));
    REQUIRE(onTick(rS.stopLmt));

    // SuggestSetup outputs must also be tick-clean for the [Use suggestion]
    // entry leg.
    auto plan = SuggestSetup(/*side=*/1,
                             /*zoneTop=*/188.0, /*zoneBot=*/186.0,
                             /*anchor=*/186.0,                      // demand zone minPrice
                             /*opposingLevel=*/192.0,
                             /*atr=*/2.0, /*last=*/187.0,
                             /*atrPad=*/0.5, /*roundPad=*/0.07,
                             /*stopOffset=*/0.10, /*rrMin=*/2.0,
                             /*equity=*/0, /*riskPct=*/1.0);
    REQUIRE(plan.valid);
    REQUIRE(onTick(plan.entry));
    REQUIRE(onTick(plan.stop));
    REQUIRE(onTick(plan.stopLmt));
    REQUIRE(onTick(plan.target));
}

// ── PositionSizeShares ──────────────────────────────────────────────────────

TEST_CASE("PositionSizeShares: basic risk math", "[analysis][setup]") {
    // 1% of 100k = 1000. Risk per share = 3 → 333.
    REQUIRE(PositionSizeShares(100000.0, 1.0, 187.0, 184.0) == 333);
    // 0.5% of 50k = 250. Risk per share = 2.5 → 100.
    REQUIRE(PositionSizeShares( 50000.0, 0.5, 100.0,  97.5) == 100);
}

TEST_CASE("PositionSizeShares: degenerate inputs return zero", "[analysis][setup]") {
    REQUIRE(PositionSizeShares(    0.0, 1.0, 100.0,  95.0) == 0);  // no equity
    REQUIRE(PositionSizeShares(100000.0, 0.0, 100.0,  95.0) == 0);  // no risk %
    REQUIRE(PositionSizeShares(100000.0, 1.0, 100.0, 100.0) == 0);  // entry == stop
    REQUIRE(PositionSizeShares(-10000.0, 1.0, 100.0,  95.0) == 0);  // negative equity
}

// ── SessionVwap ─────────────────────────────────────────────────────────────

TEST_CASE("SessionVwap: known volume-weighted average across three bars", "[analysis][vwap]") {
    // typical = (h+l+c)/3 — make h=l=c so typical equals close.
    std::vector<double> H = {100.0, 102.0, 101.0};
    std::vector<double> L = {100.0, 102.0, 101.0};
    std::vector<double> C = {100.0, 102.0, 101.0};
    std::vector<double> V = {1000.0, 2000.0, 3000.0};
    std::vector<int> noStarts;
    auto r = SessionVwap(H, L, C, V, noStarts);
    REQUIRE(r.vwap.size()  == 3);
    REQUIRE(r.sd1Up.size() == 3);
    REQUIRE(r.sd2Dn.size() == 3);

    REQUIRE(r.vwap[0] == Catch::Approx(100.0));
    // VWAP[1] = (100·1000 + 102·2000) / 3000 = 304000/3000 ≈ 101.333…
    REQUIRE(r.vwap[1] == Catch::Approx(304000.0 / 3000.0).epsilon(1e-9));
    // VWAP[2] = (100·1000 + 102·2000 + 101·3000) / 6000 = 607000/6000 ≈ 101.166…
    REQUIRE(r.vwap[2] == Catch::Approx(607000.0 / 6000.0).epsilon(1e-9));

    // Band ordering: sd2Dn ≤ sd1Dn ≤ vwap ≤ sd1Up ≤ sd2Up.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(r.sd2Dn[i] <= r.sd1Dn[i] + 1e-12);
        REQUIRE(r.sd1Dn[i] <= r.vwap[i]  + 1e-12);
        REQUIRE(r.vwap[i]  <= r.sd1Up[i] + 1e-12);
        REQUIRE(r.sd1Up[i] <= r.sd2Up[i] + 1e-12);
    }

    // Bar 0 has no spread vs itself → bands collapse to vwap.
    REQUIRE(r.sd1Up[0] == Catch::Approx(r.vwap[0]).epsilon(1e-12));
    REQUIRE(r.sd1Dn[0] == Catch::Approx(r.vwap[0]).epsilon(1e-12));
}

TEST_CASE("SessionVwap: single-bar input returns vwap == typical, bands collapsed", "[analysis][vwap]") {
    std::vector<double> H = {110.0};
    std::vector<double> L = { 90.0};
    std::vector<double> C = {100.0};
    std::vector<double> V = {500.0};
    auto r = SessionVwap(H, L, C, V, {});
    REQUIRE(r.vwap.size()  == 1);
    REQUIRE(r.vwap[0]      == Catch::Approx(100.0));
    REQUIRE(r.sd1Up[0]     == Catch::Approx(100.0));
    REQUIRE(r.sd1Dn[0]     == Catch::Approx(100.0));
    REQUIRE(r.sd2Up[0]     == Catch::Approx(100.0));
    REQUIRE(r.sd2Dn[0]     == Catch::Approx(100.0));
}

TEST_CASE("SessionVwap: session reset bar restarts cumulator", "[analysis][vwap]") {
    // Two days, two bars each. Day 1 typical=100, day 2 typical=200.
    std::vector<double> H = {100.0, 100.0, 200.0, 200.0};
    std::vector<double> L = {100.0, 100.0, 200.0, 200.0};
    std::vector<double> C = {100.0, 100.0, 200.0, 200.0};
    std::vector<double> V = {1000.0, 1000.0, 1000.0, 1000.0};
    std::vector<int> starts = {2};
    auto r = SessionVwap(H, L, C, V, starts);
    REQUIRE(r.vwap[0] == Catch::Approx(100.0));
    REQUIRE(r.vwap[1] == Catch::Approx(100.0));
    // Reset at idx=2 → vwap[2] uses only day-2 data.
    REQUIRE(r.vwap[2] == Catch::Approx(200.0));
    REQUIRE(r.vwap[3] == Catch::Approx(200.0));

    // Without sessionStarts, vwap[3] would be the cumulative average across all 4.
    auto rNoReset = SessionVwap(H, L, C, V, {});
    REQUIRE(rNoReset.vwap[3] == Catch::Approx(150.0));
}

TEST_CASE("SessionVwap: zero-volume bar carries previous values forward", "[analysis][vwap]") {
    std::vector<double> H = {100.0, 105.0, 110.0};
    std::vector<double> L = {100.0, 105.0, 110.0};
    std::vector<double> C = {100.0, 105.0, 110.0};
    std::vector<double> V = {1000.0, 0.0, 1000.0};
    auto r = SessionVwap(H, L, C, V, {});
    REQUIRE(r.vwap[0] == Catch::Approx(100.0));
    // vol=0 → cumulator unchanged → vwap[1] stays 100 (cumTPV/cumVol same as bar 0).
    REQUIRE(r.vwap[1] == Catch::Approx(100.0));
    // bar 2 sees 1000 vol at typical=110 plus prior 1000 vol at typical=100.
    REQUIRE(r.vwap[2] == Catch::Approx((100.0 * 1000.0 + 110.0 * 1000.0) / 2000.0));
}

TEST_CASE("SessionVwap: empty / mismatched-length input returns zero-filled result", "[analysis][vwap]") {
    auto rEmpty = SessionVwap({}, {}, {}, {}, {});
    REQUIRE(rEmpty.vwap.empty());
    REQUIRE(rEmpty.sd1Up.empty());

    std::vector<double> H = {1.0, 2.0};
    std::vector<double> L = {1.0};                    // wrong size
    std::vector<double> C = {1.0, 2.0};
    std::vector<double> V = {100.0, 100.0};
    auto rMismatch = SessionVwap(H, L, C, V, {});
    REQUIRE(rMismatch.vwap.size()  == 2);
    REQUIRE(rMismatch.vwap[0]      == 0.0);
    REQUIRE(rMismatch.vwap[1]      == 0.0);
    REQUIRE(rMismatch.sd1Up[0]     == 0.0);
    REQUIRE(rMismatch.sd2Dn[1]     == 0.0);
}

TEST_CASE("SessionVwap: band ordering invariant on a varying series", "[analysis][vwap]") {
    // 10 bars walking up; volumes vary. Bands must respect sd2Dn ≤ sd1Dn ≤ vwap ≤ sd1Up ≤ sd2Up
    // for every bar.
    std::vector<double> H, L, C, V;
    for (int i = 0; i < 10; ++i) {
        double p = 100.0 + i * 1.5;
        H.push_back(p + 0.5);
        L.push_back(p - 0.5);
        C.push_back(p);
        V.push_back(500.0 + i * 100.0);
    }
    auto r = SessionVwap(H, L, C, V, {});
    REQUIRE(r.vwap.size() == 10);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(r.sd2Dn[i] <= r.sd1Dn[i] + 1e-12);
        REQUIRE(r.sd1Dn[i] <= r.vwap[i]  + 1e-12);
        REQUIRE(r.vwap[i]  <= r.sd1Up[i] + 1e-12);
        REQUIRE(r.sd1Up[i] <= r.sd2Up[i] + 1e-12);
    }
    // After several bars with rising prices, variance is positive → bands non-collapsed.
    REQUIRE(r.sd1Up.back() > r.vwap.back() + 0.01);
    REQUIRE(r.sd2Up.back() > r.sd1Up.back() + 0.01);
}

// ── TradingStyle presets ────────────────────────────────────────────────────

namespace {
// Stub structs that mirror ChartWindow's private nested types — same field names
// so ApplyPreset() instantiates against them. Defaults intentionally diverge from
// every preset so we can detect "field was not written" as a test failure.
struct StubInd {
    bool   vwap       = false;
    bool   vwapBands  = false;
    int    smaPeriod1 = -1;
    int    smaPeriod2 = -1;
};
struct StubAuto {
    bool supports     = false;
    bool resistances  = false;
    bool trend        = false;
    bool donchian     = false;
    bool keltner      = false;
    bool autoFib      = false;
    bool pivotPoints  = false;
    bool breakouts    = false;
    bool zones        = false;
    int  swingK         = -1;
    int  trendLookback  = -1;
    int  donchianLen    = -1;
    int  maxLevels      = -1;
    int  minTouches     = -1;
    int  scanCap        = -1;
    bool trendChannel   = true;   // diverges from every preset (false)
};
struct StubSetup {
    bool   overlay     = true;    // diverges from every preset (false)
    double rrMin       = -1.0;
    double atrPad      = -1.0;
    double roundPad    = -1.0;
    double stopOffset  = -1.0;
    double riskPct     = -1.0;
    bool   useStopLmt  = false;   // diverges from every preset (true)
};
}  // namespace

TEST_CASE("TradingStyleLabel / Short cover all five modes", "[analysis][style]") {
    REQUIRE(std::string(TradingStyleLabel(TradingStyle::Scalping))   == "Scalping");
    REQUIRE(std::string(TradingStyleLabel(TradingStyle::DayTrading)) == "Day Trading");
    REQUIRE(std::string(TradingStyleLabel(TradingStyle::Swing))      == "Swing");
    REQUIRE(std::string(TradingStyleLabel(TradingStyle::Investment)) == "Investment");
    REQUIRE(std::string(TradingStyleLabel(TradingStyle::Free))       == "Free");
    REQUIRE(std::string(TradingStyleShort(TradingStyle::Scalping))   == "SCALP");
    REQUIRE(std::string(TradingStyleShort(TradingStyle::DayTrading)) == "DAY");
    REQUIRE(std::string(TradingStyleShort(TradingStyle::Swing))      == "SWING");
    REQUIRE(std::string(TradingStyleShort(TradingStyle::Investment)) == "INVEST");
    REQUIRE(std::string(TradingStyleShort(TradingStyle::Free))       == "FREE");
}

TEST_CASE("GetPreset(Scalping): expected 1m / 2 D / scalper params", "[analysis][style]") {
    auto p = GetPreset(TradingStyle::Scalping);
    REQUIRE(p.timeframe       == core::Timeframe::M1);
    REQUIRE(std::string(p.historyDuration) == "2 D");
    REQUIRE(p.swingK          == 3);
    REQUIRE(p.trendLookback   == 30);
    REQUIRE(p.maxLevels       == 4);
    REQUIRE(p.minTouches      == 2);
    REQUIRE(p.indVwap         == true);
    REQUIRE(p.indVwapBands    == false);
    REQUIRE(p.smaPeriod1      == 9);
    REQUIRE(p.smaPeriod2      == 20);
    REQUIRE(p.pivotPoints     == true);
    REQUIRE(p.breakouts       == true);
    REQUIRE(p.zones           == true);
    REQUIRE(p.trend           == false);
    REQUIRE(p.autoFib         == false);
    REQUIRE(p.setupOverlay    == false);
    REQUIRE(p.rrMin           == Catch::Approx(1.5));
    REQUIRE(p.atrPad          == Catch::Approx(0.4));
    REQUIRE(p.roundPad        == Catch::Approx(0.03));
    REQUIRE(p.stopOffset      == Catch::Approx(0.05));
    REQUIRE(p.riskPct         == Catch::Approx(0.5));
    REQUIRE(p.useStopLmt      == true);
}

TEST_CASE("GetPreset(DayTrading): expected 15m / 20 D / day-trade params", "[analysis][style]") {
    auto p = GetPreset(TradingStyle::DayTrading);
    REQUIRE(p.timeframe       == core::Timeframe::M15);
    REQUIRE(std::string(p.historyDuration) == "20 D");
    REQUIRE(p.swingK          == 3);
    REQUIRE(p.trendLookback   == 40);
    REQUIRE(p.indVwap         == true);
    REQUIRE(p.indVwapBands    == true);
    REQUIRE(p.smaPeriod1      == 20);
    REQUIRE(p.smaPeriod2      == 50);
    REQUIRE(p.pivotPoints     == true);
    REQUIRE(p.breakouts       == true);
    REQUIRE(p.zones           == true);
    REQUIRE(p.trend           == true);
    REQUIRE(p.rrMin           == Catch::Approx(1.75));
    REQUIRE(p.atrPad          == Catch::Approx(0.5));
    REQUIRE(p.riskPct         == Catch::Approx(0.75));
}

TEST_CASE("GetPreset(Swing): expected 1D / 1 Y / swing params", "[analysis][style]") {
    auto p = GetPreset(TradingStyle::Swing);
    REQUIRE(p.timeframe       == core::Timeframe::D1);
    REQUIRE(std::string(p.historyDuration) == "1 Y");
    REQUIRE(p.swingK          == 4);
    REQUIRE(p.trendLookback   == 50);
    REQUIRE(p.indVwap         == false);
    REQUIRE(p.indVwapBands    == false);
    REQUIRE(p.smaPeriod1      == 20);
    REQUIRE(p.smaPeriod2      == 50);
    REQUIRE(p.pivotPoints     == false);
    REQUIRE(p.breakouts       == true);
    REQUIRE(p.zones           == true);
    REQUIRE(p.trend           == true);
    REQUIRE(p.autoFib         == true);
    REQUIRE(p.rrMin           == Catch::Approx(2.0));
    REQUIRE(p.atrPad          == Catch::Approx(0.5));
    REQUIRE(p.roundPad        == Catch::Approx(0.07));
    REQUIRE(p.stopOffset      == Catch::Approx(0.10));
    REQUIRE(p.riskPct         == Catch::Approx(1.0));
}

TEST_CASE("GetPreset(Investment): expected 1W / 5 Y / investor params", "[analysis][style]") {
    auto p = GetPreset(TradingStyle::Investment);
    REQUIRE(p.timeframe       == core::Timeframe::W1);
    REQUIRE(std::string(p.historyDuration) == "5 Y");
    REQUIRE(p.swingK          == 5);
    REQUIRE(p.trendLookback   == 52);
    REQUIRE(p.minTouches      == 3);
    REQUIRE(p.indVwap         == false);
    REQUIRE(p.indVwapBands    == false);
    REQUIRE(p.smaPeriod1      == 50);
    REQUIRE(p.smaPeriod2      == 200);
    REQUIRE(p.pivotPoints     == false);
    REQUIRE(p.breakouts       == false);
    REQUIRE(p.zones           == false);
    REQUIRE(p.trend           == true);
    REQUIRE(p.autoFib         == true);
    REQUIRE(p.rrMin           == Catch::Approx(3.0));
    REQUIRE(p.atrPad          == Catch::Approx(1.0));
    REQUIRE(p.roundPad        == Catch::Approx(0.20));
    REQUIRE(p.stopOffset      == Catch::Approx(0.25));
    REQUIRE(p.riskPct         == Catch::Approx(1.5));
}

TEST_CASE("GetPreset(Free): construction-default baseline + D1 placeholder TF", "[analysis][style]") {
    auto p = GetPreset(TradingStyle::Free);
    // Free's TF is only the placeholder ChartWindow uses when no prior TF
    // exists; setTradingStyle(Free) special-cases this to preserve the
    // chart's current TF. Locking the baseline here so accidental changes
    // are caught.
    REQUIRE(p.timeframe       == core::Timeframe::D1);
    REQUIRE(std::string(p.historyDuration) == "6 M");
    // Auto-analysis: same as the construction defaults of AutoAnalysisSettings.
    REQUIRE(p.supports        == true);
    REQUIRE(p.resistances     == true);
    REQUIRE(p.trend           == true);
    REQUIRE(p.donchian        == false);
    REQUIRE(p.keltner         == false);
    REQUIRE(p.autoFib         == false);
    REQUIRE(p.pivotPoints     == false);
    REQUIRE(p.breakouts       == false);
    REQUIRE(p.zones           == false);
    REQUIRE(p.swingK          == 3);
    REQUIRE(p.trendLookback   == 50);
    REQUIRE(p.donchianLen     == 20);
    REQUIRE(p.maxLevels       == 3);
    REQUIRE(p.minTouches      == 2);
    REQUIRE(p.scanCap         == 1000);
    REQUIRE(p.trendChannel    == false);
    // Indicators: VWAP on (matches the IndicatorSettings default).
    REQUIRE(p.indVwap         == true);
    REQUIRE(p.indVwapBands    == false);
    REQUIRE(p.smaPeriod1      == 20);
    REQUIRE(p.smaPeriod2      == 50);
    // Setup overlay: matches SetupSettings construction defaults.
    REQUIRE(p.setupOverlay    == false);
    REQUIRE(p.rrMin           == Catch::Approx(2.0));
    REQUIRE(p.atrPad          == Catch::Approx(0.5));
    REQUIRE(p.roundPad        == Catch::Approx(0.07));
    REQUIRE(p.stopOffset      == Catch::Approx(0.10));
    REQUIRE(p.riskPct         == Catch::Approx(1.0));
    REQUIRE(p.useStopLmt      == true);
}

TEST_CASE("ApplyPreset stamps every overridable field onto stub structs", "[analysis][style]") {
    StubInd ind; StubAuto a; StubSetup s;
    core::Timeframe tf = core::Timeframe::M5;
    auto p = GetPreset(TradingStyle::DayTrading);
    ApplyPreset(p, ind, a, s, tf);

    REQUIRE(tf == core::Timeframe::M15);
    REQUIRE(a.supports     == true);
    REQUIRE(a.resistances  == true);
    REQUIRE(a.trend        == true);
    REQUIRE(a.donchian     == false);
    REQUIRE(a.keltner      == false);
    REQUIRE(a.autoFib      == false);
    REQUIRE(a.pivotPoints  == true);
    REQUIRE(a.breakouts    == true);
    REQUIRE(a.zones        == true);
    REQUIRE(a.swingK       == 3);
    REQUIRE(a.trendLookback== 40);
    REQUIRE(a.donchianLen  == 20);
    REQUIRE(a.maxLevels    == 4);
    REQUIRE(a.minTouches   == 2);
    REQUIRE(a.scanCap      == 1000);
    REQUIRE(a.trendChannel == false);

    REQUIRE(ind.vwap       == true);
    REQUIRE(ind.vwapBands  == true);
    REQUIRE(ind.smaPeriod1 == 20);
    REQUIRE(ind.smaPeriod2 == 50);

    REQUIRE(s.overlay      == false);
    REQUIRE(s.rrMin        == Catch::Approx(1.75));
    REQUIRE(s.atrPad       == Catch::Approx(0.5));
    REQUIRE(s.roundPad     == Catch::Approx(0.05));
    REQUIRE(s.stopOffset   == Catch::Approx(0.07));
    REQUIRE(s.riskPct      == Catch::Approx(0.75));
    REQUIRE(s.useStopLmt   == true);
}

TEST_CASE("ApplyPreset round-trip leaves no carry-over from the previous preset", "[analysis][style]") {
    // Apply Scalping (small swingK, VWAP on, bands off, riskPct=0.5) then
    // Investment (swingK=5, VWAP off, bands off, riskPct=1.5) — final state must
    // match Investment exactly.
    StubInd ind; StubAuto a; StubSetup s;
    core::Timeframe tf = core::Timeframe::H1;

    ApplyPreset(GetPreset(TradingStyle::Scalping), ind, a, s, tf);
    REQUIRE(tf == core::Timeframe::M1);
    REQUIRE(a.swingK == 3);
    REQUIRE(ind.vwap == true);
    REQUIRE(ind.smaPeriod1 == 9);
    REQUIRE(ind.smaPeriod2 == 20);

    ApplyPreset(GetPreset(TradingStyle::Investment), ind, a, s, tf);
    REQUIRE(tf == core::Timeframe::W1);
    REQUIRE(a.swingK         == 5);
    REQUIRE(a.trendLookback  == 52);
    REQUIRE(a.minTouches     == 3);
    REQUIRE(a.zones          == false);
    REQUIRE(a.breakouts      == false);
    REQUIRE(a.pivotPoints    == false);
    REQUIRE(ind.vwap         == false);
    REQUIRE(ind.vwapBands    == false);
    REQUIRE(ind.smaPeriod1   == 50);
    REQUIRE(ind.smaPeriod2   == 200);
    REQUIRE(s.riskPct        == Catch::Approx(1.5));
    REQUIRE(s.atrPad         == Catch::Approx(1.0));
}

TEST_CASE("ApplyPreset(Free) stamps construction-default baseline", "[analysis][style]") {
    // Note: ApplyPreset always writes the preset's TF onto the caller's tf
    // ref. The Free-mode "preserve current TF" behaviour lives in
    // ChartWindow::setTradingStyle, NOT in this pure helper. Verifying here
    // that the helper itself stays simple and predictable.
    StubInd ind; StubAuto a; StubSetup s;
    core::Timeframe tf = core::Timeframe::H4;

    // Stamp Investment first to dirty every field, then Free to overwrite.
    ApplyPreset(GetPreset(TradingStyle::Investment), ind, a, s, tf);
    REQUIRE(a.autoFib   == true);    // dirtied
    REQUIRE(s.riskPct   == Catch::Approx(1.5));

    ApplyPreset(GetPreset(TradingStyle::Free), ind, a, s, tf);
    REQUIRE(tf              == core::Timeframe::D1);
    REQUIRE(a.supports      == true);
    REQUIRE(a.resistances   == true);
    REQUIRE(a.trend         == true);
    REQUIRE(a.autoFib       == false);   // overwritten back to baseline
    REQUIRE(a.zones         == false);
    REQUIRE(a.breakouts     == false);
    REQUIRE(a.pivotPoints   == false);
    REQUIRE(a.swingK        == 3);
    REQUIRE(a.trendLookback == 50);
    REQUIRE(a.minTouches    == 2);
    REQUIRE(ind.vwap        == true);
    REQUIRE(ind.vwapBands   == false);
    REQUIRE(ind.smaPeriod1  == 20);
    REQUIRE(ind.smaPeriod2  == 50);
    REQUIRE(s.overlay       == false);
    REQUIRE(s.rrMin         == Catch::Approx(2.0));
    REQUIRE(s.atrPad        == Catch::Approx(0.5));
    REQUIRE(s.riskPct       == Catch::Approx(1.0));
    REQUIRE(s.useStopLmt    == true);
}

// ── OrderImpact — side-intent badge & P&L preview ─────────────────────────────

TEST_CASE("ComputeOrderImpact: no position + BUY -> OpenLong", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(0.0, 0.0, 0.0, true, 100.0, 187.42);
    REQUIRE(r.kind == OrderImpactKind::OpenLong);
    REQUIRE(r.closeQty == Catch::Approx(0.0));
    REQUIRE(r.openQty == Catch::Approx(100.0));
    REQUIRE(r.newPosQty == Catch::Approx(100.0));
    REQUIRE(r.newAvgCost == Catch::Approx(187.42));
    REQUIRE(r.isClosingPath == false);
}

TEST_CASE("ComputeOrderImpact: no position + SELL -> OpenShort", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(0.0, 0.0, 0.0, false, 100.0, 187.42);
    REQUIRE(r.kind == OrderImpactKind::OpenShort);
    REQUIRE(r.closeQty == Catch::Approx(0.0));
    REQUIRE(r.openQty == Catch::Approx(100.0));
    REQUIRE(r.newPosQty == Catch::Approx(-100.0));
    REQUIRE(r.newAvgCost == Catch::Approx(187.42));
    REQUIRE(r.isClosingPath == false);
}

TEST_CASE("ComputeOrderImpact: long 100@$150 + BUY 50@$160 -> AddToLong", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(100.0, 150.0, 0.0, true, 50.0, 160.0);
    REQUIRE(r.kind == OrderImpactKind::AddToLong);
    REQUIRE(r.isClosingPath == false);
    REQUIRE(r.closePnL == Catch::Approx(0.0));
    REQUIRE(r.newPosQty == Catch::Approx(150.0));
    // (100*150 + 50*160) / 150 = 23000/150 = 153.333...
    REQUIRE(r.newAvgCost == Catch::Approx(153.33333));
}

TEST_CASE("ComputeOrderImpact: long 100@$150 + SELL 50@$155 -> ReduceLong", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(100.0, 150.0, 0.0, false, 50.0, 155.0);
    REQUIRE(r.kind == OrderImpactKind::ReduceLong);
    REQUIRE(r.isClosingPath == true);
    REQUIRE(r.closeQty == Catch::Approx(50.0));
    REQUIRE(r.openQty == Catch::Approx(0.0));
    // (155 - 150) * 50 = +250
    REQUIRE(r.closePnL == Catch::Approx(250.0));
    REQUIRE(r.newPosQty == Catch::Approx(50.0));
}

TEST_CASE("ComputeOrderImpact: long 100@$150 + SELL 100@$155 -> CloseLong", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(100.0, 150.0, 0.0, false, 100.0, 155.0);
    REQUIRE(r.kind == OrderImpactKind::CloseLong);
    REQUIRE(r.isClosingPath == true);
    REQUIRE(r.closeQty == Catch::Approx(100.0));
    REQUIRE(r.openQty == Catch::Approx(0.0));
    REQUIRE(r.closePnL == Catch::Approx(500.0));
    REQUIRE(r.newPosQty == Catch::Approx(0.0));
}

TEST_CASE("ComputeOrderImpact: long 100@$150 + SELL 150@$155 -> FlipToShort", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(100.0, 150.0, 0.0, false, 150.0, 155.0);
    REQUIRE(r.kind == OrderImpactKind::FlipToShort);
    REQUIRE(r.isClosingPath == true);
    REQUIRE(r.closeQty == Catch::Approx(100.0));
    REQUIRE(r.openQty == Catch::Approx(50.0));
    REQUIRE(r.closePnL == Catch::Approx(500.0));
    REQUIRE(r.newPosQty == Catch::Approx(-50.0));
    REQUIRE(r.newAvgCost == Catch::Approx(155.0));
}

TEST_CASE("ComputeOrderImpact: short 100@$150 + SELL 50@$155 -> AddToShort", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(-100.0, 150.0, 0.0, false, 50.0, 155.0);
    REQUIRE(r.kind == OrderImpactKind::AddToShort);
    REQUIRE(r.isClosingPath == false);
    REQUIRE(r.closePnL == Catch::Approx(0.0));
    REQUIRE(r.newPosQty == Catch::Approx(-150.0));
    // (100*150 + 50*155) / 150 = 22750/150 = 151.666...
    REQUIRE(r.newAvgCost == Catch::Approx(151.66667));
}

TEST_CASE("ComputeOrderImpact: short 100@$150 + BUY 50@$148 -> ReduceShort", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(-100.0, 150.0, 0.0, true, 50.0, 148.0);
    REQUIRE(r.kind == OrderImpactKind::ReduceShort);
    REQUIRE(r.isClosingPath == true);
    REQUIRE(r.closeQty == Catch::Approx(50.0));
    REQUIRE(r.openQty == Catch::Approx(0.0));
    // (150 - 148) * 50 = +100
    REQUIRE(r.closePnL == Catch::Approx(100.0));
    REQUIRE(r.newPosQty == Catch::Approx(-50.0));
}

TEST_CASE("ComputeOrderImpact: short 100@$150 + BUY 100@$148 -> CloseShort", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(-100.0, 150.0, 0.0, true, 100.0, 148.0);
    REQUIRE(r.kind == OrderImpactKind::CloseShort);
    REQUIRE(r.isClosingPath == true);
    REQUIRE(r.closeQty == Catch::Approx(100.0));
    REQUIRE(r.openQty == Catch::Approx(0.0));
    REQUIRE(r.closePnL == Catch::Approx(200.0));
    REQUIRE(r.newPosQty == Catch::Approx(0.0));
}

TEST_CASE("ComputeOrderImpact: short 100@$150 + BUY 150@$148 -> FlipToLong", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(-100.0, 150.0, 0.0, true, 150.0, 148.0);
    REQUIRE(r.kind == OrderImpactKind::FlipToLong);
    REQUIRE(r.isClosingPath == true);
    REQUIRE(r.closeQty == Catch::Approx(100.0));
    REQUIRE(r.openQty == Catch::Approx(50.0));
    REQUIRE(r.closePnL == Catch::Approx(200.0));
    REQUIRE(r.newPosQty == Catch::Approx(50.0));
    REQUIRE(r.newAvgCost == Catch::Approx(148.0));
}

TEST_CASE("ComputeOrderImpact: long 100@$150 + SELL 50@$140 -> loss path", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(100.0, 150.0, 0.0, false, 50.0, 140.0);
    REQUIRE(r.kind == OrderImpactKind::ReduceLong);
    REQUIRE(r.isClosingPath == true);
    // (140 - 150) * 50 = -500
    REQUIRE(r.closePnL == Catch::Approx(-500.0));
}

TEST_CASE("ComputeOrderImpact: degenerate -- qty <= 0", "[analysis][order-impact]") {
    auto r1 = ComputeOrderImpact(0.0, 0.0, 0.0, true, 0.0, 100.0);
    REQUIRE(r1.kind == OrderImpactKind::Invalid);

    auto r2 = ComputeOrderImpact(0.0, 0.0, 0.0, true, -10.0, 100.0);
    REQUIRE(r2.kind == OrderImpactKind::Invalid);
}

TEST_CASE("ComputeOrderImpact: degenerate -- fillPrice <= 0", "[analysis][order-impact]") {
    auto r = ComputeOrderImpact(0.0, 0.0, 0.0, true, 100.0, 0.0);
    REQUIRE(r.kind == OrderImpactKind::Invalid);
}

TEST_CASE("ComputeOrderImpact: commission propagation", "[analysis][order-impact]") {
    // long 100@$150 + SELL 50@$155 with $0.01/share commission
    // gross = (155-150)*50 = 250, net = 250 - 0.01*50 = 249.50
    auto r = ComputeOrderImpact(100.0, 150.0, 0.01, false, 50.0, 155.0);
    REQUIRE(r.kind == OrderImpactKind::ReduceLong);
    REQUIRE(r.closePnL == Catch::Approx(249.50));
}

TEST_CASE("PreviewStopTarget: long entry $150, target=$155 stop=$148", "[analysis][order-impact]") {
    // Target leg: long 100@150 + SELL 100@155 → closePnL = +500
    auto target = ComputeOrderImpact(100.0, 150.0, 0.0, false, 100.0, 155.0);
    // Stop leg:   long 100@150 + SELL 100@148 → closePnL = -200
    auto stop   = ComputeOrderImpact(100.0, 150.0, 0.0, false, 100.0, 148.0);

    auto p = PreviewStopTarget(target, stop);
    REQUIRE(p.valid == true);
    REQUIRE(p.targetPnL == Catch::Approx(500.0));
    REQUIRE(p.stopPnL == Catch::Approx(-200.0));
    REQUIRE(p.rrRatio == Catch::Approx(2.5));   // 500/200
}

TEST_CASE("PreviewStopTarget: invalid -- non-closing leg", "[analysis][order-impact]") {
    // OpenLong is not a closing path
    auto open  = ComputeOrderImpact(0.0, 0.0, 0.0, true, 100.0, 150.0);
    auto close = ComputeOrderImpact(100.0, 150.0, 0.0, false, 100.0, 155.0);
    auto p = PreviewStopTarget(open, close);
    REQUIRE(p.valid == false);
}

TEST_CASE("PreviewStopTarget: invalid -- zero stop risk", "[analysis][order-impact]") {
    // Close at entry price → zero P&L, risk=0 → invalid R:R
    auto target = ComputeOrderImpact(100.0, 150.0, 0.0, false, 100.0, 155.0);
    auto stop   = ComputeOrderImpact(100.0, 150.0, 0.0, false, 100.0, 150.0);
    auto p = PreviewStopTarget(target, stop);
    REQUIRE(p.valid == false);   // stopPnL=0 → risk=0
}

// ============================================================================
// [chart-indicators] — extracted single-series indicator helpers
// (was ChartWindow::CalcSMA / CalcEMA / CalcBollingerBands / CalcRSI before
// the refactor; now lives in core::services. Tests are a regression net so a
// future change to the math doesn't silently break ChartWindow / ReplayWindow.)
// ============================================================================

TEST_CASE("SMA: rolling 3-period on {1..6} matches expected", "[analysis][chart-indicators]") {
    std::vector<double> in = {1, 2, 3, 4, 5, 6};
    auto out = SMA(in, 3);
    REQUIRE(out.size() == 6);
    REQUIRE(out[0] == 0.0);
    REQUIRE(out[1] == 0.0);
    REQUIRE(out[2] == Catch::Approx(2.0));   // (1+2+3)/3
    REQUIRE(out[3] == Catch::Approx(3.0));   // (2+3+4)/3
    REQUIRE(out[4] == Catch::Approx(4.0));   // (3+4+5)/3
    REQUIRE(out[5] == Catch::Approx(5.0));   // (4+5+6)/3
}

TEST_CASE("SMA: degenerate input returns all zeros", "[analysis][chart-indicators]") {
    REQUIRE(SMA({1.0, 2.0}, 0).size() == 2);
    REQUIRE(SMA({1.0, 2.0}, 0)[0] == 0.0);
    REQUIRE(SMA({1.0, 2.0}, 0)[1] == 0.0);
    auto small = SMA({1.0, 2.0}, 5);   // n < period
    REQUIRE(small.size() == 2);
    REQUIRE(small[0] == 0.0);
    REQUIRE(small[1] == 0.0);
    REQUIRE(SMA({}, 3).empty());
}

TEST_CASE("EMA: 3-period on monotone series, seed equals SMA, smoothing applied",
          "[analysis][chart-indicators]") {
    std::vector<double> in = {10, 20, 30, 40, 50};
    auto out = EMA(in, 3);
    REQUIRE(out.size() == 5);
    REQUIRE(out[0] == 0.0);
    REQUIRE(out[1] == 0.0);
    REQUIRE(out[2] == Catch::Approx(20.0));   // SMA seed: (10+20+30)/3
    // k = 2/(3+1) = 0.5
    // out[3] = 40 * 0.5 + 20 * 0.5 = 30
    // out[4] = 50 * 0.5 + 30 * 0.5 = 40
    REQUIRE(out[3] == Catch::Approx(30.0));
    REQUIRE(out[4] == Catch::Approx(40.0));
}

TEST_CASE("EMA: degenerate input", "[analysis][chart-indicators]") {
    auto z = EMA({1.0}, 3);    // n < period
    REQUIRE(z.size() == 1);
    REQUIRE(z[0] == 0.0);
    REQUIRE(EMA({}, 3).empty());
    auto z2 = EMA({1.0, 2.0}, 0);   // period <= 0
    REQUIRE(z2.size() == 2);
    REQUIRE(z2[0] == 0.0);
}

TEST_CASE("ComputeBollinger: mid equals SMA, bands symmetric around mid",
          "[analysis][chart-indicators]") {
    std::vector<double> in = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto bb = ComputeBollinger(in, 3, 2.0);
    auto sma = SMA(in, 3);
    REQUIRE(bb.mid.size() == 10);
    REQUIRE(bb.upper.size() == 10);
    REQUIRE(bb.lower.size() == 10);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(bb.mid[i] == Catch::Approx(sma[i]));
        if (i >= 2) {   // warm-up boundary
            // Symmetric: |upper-mid| == |mid-lower|
            REQUIRE((bb.upper[i] - bb.mid[i]) == Catch::Approx(bb.mid[i] - bb.lower[i]));
            REQUIRE(bb.upper[i] > bb.mid[i]);
            REQUIRE(bb.lower[i] < bb.mid[i]);
        }
    }
    // Stdev of {1,2,3} = sqrt(((1-2)^2 + 0 + (3-2)^2)/3) = sqrt(2/3) ≈ 0.8165
    // upper[2] = 2 + 2*0.8165 ≈ 3.633
    REQUIRE(bb.upper[2] == Catch::Approx(2.0 + 2.0 * std::sqrt(2.0 / 3.0)));
}

TEST_CASE("ComputeBollinger: degenerate", "[analysis][chart-indicators]") {
    auto bb = ComputeBollinger({1.0, 2.0}, 5, 2.0);   // n < period
    REQUIRE(bb.mid.size() == 2);
    REQUIRE(bb.mid[0] == 0.0);
    REQUIRE(bb.upper[1] == 0.0);
    auto bb2 = ComputeBollinger({}, 3, 2.0);
    REQUIRE(bb2.mid.empty());
    REQUIRE(bb2.upper.empty());
    REQUIRE(bb2.lower.empty());
}

TEST_CASE("RSI: strictly rising series → 100", "[analysis][chart-indicators]") {
    std::vector<double> in = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto out = RSI(in, 2);
    REQUIRE(out.size() == 10);
    REQUIRE(out[0] == 0.0);
    REQUIRE(out[1] == 0.0);   // before period
    REQUIRE(out[2] == Catch::Approx(100.0));    // al == 0
    REQUIRE(out[9] == Catch::Approx(100.0));
}

TEST_CASE("RSI: strictly falling series → 0", "[analysis][chart-indicators]") {
    std::vector<double> in = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    auto out = RSI(in, 2);
    REQUIRE(out[2] == Catch::Approx(0.0));     // ag == 0 → 100 - 100/(1+0) = 0
    REQUIRE(out[9] == Catch::Approx(0.0));
}

TEST_CASE("RSI: mixed series, known midpoint value", "[analysis][chart-indicators]") {
    // 4-period RSI with equal up/down moves should hover near 50.
    std::vector<double> in = {10, 11, 10, 11, 10, 11, 10};
    auto out = RSI(in, 2);
    REQUIRE(out.size() == 7);
    REQUIRE(out[2] == Catch::Approx(50.0));   // ag=0.5 al=0.5 → 50
}

TEST_CASE("RSI: degenerate input", "[analysis][chart-indicators]") {
    auto z = RSI({1.0, 2.0}, 5);    // n <= period
    REQUIRE(z.size() == 2);
    REQUIRE(z[0] == 0.0);
    REQUIRE(z[1] == 0.0);
    REQUIRE(RSI({}, 14).empty());
    auto z2 = RSI({1.0, 2.0, 3.0}, 0);   // period <= 0
    REQUIRE(z2.size() == 3);
    REQUIRE(z2[0] == 0.0);
}
