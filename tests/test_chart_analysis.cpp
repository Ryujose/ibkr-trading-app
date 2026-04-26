#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include "core/services/ChartAnalysis.h"

using core::services::ATR;
using core::services::AutoFibSpan;
using core::services::BreakoutMark;
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
using core::services::Swing;
using core::services::TrendFit;

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

TEST_CASE("LinearRegression: flat line yields slope ≈ 0", "[analysis][trend]") {
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

TEST_CASE("LargestSwingSpan: empty list → invalid", "[analysis][autofib]") {
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

TEST_CASE("ClassicPivots: invalid input → valid=false", "[analysis][pivots]") {
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
