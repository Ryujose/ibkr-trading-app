#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace core::services {

// ============================================================================
// Pure technical-analysis helpers used by ChartWindow's auto-detection layer.
// No IB API / ImGui / ImPlot dependency — testable from tests-core.
// ============================================================================

struct Swing {
    int    idx;
    double price;
};

struct SwingResult {
    std::vector<Swing> highs;
    std::vector<Swing> lows;
};

// Find pivot highs and pivot lows using a left/right window of size k.
//   pivot high: high[i] > max(high[i-k..i-1])  AND  high[i] >= max(high[i+1..i+k])
//   pivot low : low[i]  < min(low[i-k..i-1])   AND  low[i]  <= min(low[i+1..i+k])
// Strict on the left, ≥ on the right — flat tops register as a single swing.
// scanCap (>0): scan only the last `scanCap` bars.
inline SwingResult FindSwings(const std::vector<double>& highs,
                              const std::vector<double>& lows,
                              int                         k,
                              int                         scanCap = 0) {
    SwingResult out;
    int n = static_cast<int>(highs.size());
    if (k <= 0 || n < 2 * k + 1 || static_cast<int>(lows.size()) != n) return out;

    int lo = k;
    if (scanCap > 0 && n - k > scanCap) lo = n - scanCap;
    if (lo < k) lo = k;
    int hi = n - k;

    for (int i = lo; i < hi; ++i) {
        bool isHigh = true;
        for (int j = i - k; j < i && isHigh; ++j)
            if (highs[j] >= highs[i]) isHigh = false;
        for (int j = i + 1; j <= i + k && isHigh; ++j)
            if (highs[j] >  highs[i]) isHigh = false;
        if (isHigh) out.highs.push_back({i, highs[i]});

        bool isLow = true;
        for (int j = i - k; j < i && isLow; ++j)
            if (lows[j] <= lows[i]) isLow = false;
        for (int j = i + 1; j <= i + k && isLow; ++j)
            if (lows[j] <  lows[i]) isLow = false;
        if (isLow) out.lows.push_back({i, lows[i]});
    }
    return out;
}

// Wilder's ATR. Returns a vector of length n; values before `period` are 0.
inline std::vector<double> ATR(const std::vector<double>& highs,
                               const std::vector<double>& lows,
                               const std::vector<double>& closes,
                               int                         period) {
    int n = static_cast<int>(highs.size());
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n < period + 1 ||
        static_cast<int>(lows.size())  != n ||
        static_cast<int>(closes.size()) != n) return out;

    double seed = 0.0;
    for (int i = 1; i <= period; ++i) {
        double tr = std::max({ highs[i] - lows[i],
                               std::abs(highs[i] - closes[i - 1]),
                               std::abs(lows[i]  - closes[i - 1]) });
        seed += tr;
    }
    out[period] = seed / period;
    for (int i = period + 1; i < n; ++i) {
        double tr = std::max({ highs[i] - lows[i],
                               std::abs(highs[i] - closes[i - 1]),
                               std::abs(lows[i]  - closes[i - 1]) });
        out[i] = (out[i - 1] * (period - 1) + tr) / period;
    }
    return out;
}

struct Level {
    double price;     // cluster price (mean of constituents)
    int    touches;   // number of swings in the cluster
    int    firstIdx;  // earliest constituent idx
    int    lastIdx;   // latest constituent idx
    double minPrice;  // lowest constituent price (zone bottom before buffer)
    double maxPrice;  // highest constituent price (zone top before buffer)
};

// Cluster swings by price proximity. Two swings join the same cluster when
// their prices are within `tol`. Returns clusters in ascending price order.
// `minPrice` / `maxPrice` track the range of constituent swing prices, used by
// ChartWindow to render thickness-aware supply/demand zones.
inline std::vector<Level> ClusterLevels(std::vector<Swing> swings, double tol) {
    std::vector<Level> out;
    if (swings.empty() || tol <= 0.0) return out;

    std::sort(swings.begin(), swings.end(),
              [](const Swing& a, const Swing& b) { return a.price < b.price; });

    Level  cur{ swings[0].price, 1, swings[0].idx, swings[0].idx,
                swings[0].price, swings[0].price };
    double sum = swings[0].price;
    for (std::size_t i = 1; i < swings.size(); ++i) {
        if (swings[i].price - cur.price <= tol) {
            cur.touches++;
            sum     += swings[i].price;
            cur.price    = sum / cur.touches;
            cur.firstIdx = std::min(cur.firstIdx, swings[i].idx);
            cur.lastIdx  = std::max(cur.lastIdx,  swings[i].idx);
            cur.minPrice = std::min(cur.minPrice, swings[i].price);
            cur.maxPrice = std::max(cur.maxPrice, swings[i].price);
        } else {
            out.push_back(cur);
            cur = { swings[i].price, 1, swings[i].idx, swings[i].idx,
                    swings[i].price, swings[i].price };
            sum = swings[i].price;
        }
    }
    out.push_back(cur);
    return out;
}

// Linear-regression result for the trend overlay. (x, y) = (bar idx, close).
//   slope, intercept give y = slope*x + intercept fit over the last `lookback`
//   closes. firstIdx / lastIdx pin the actual bar range used. sigma is the
//   residual stdev (population), used by the optional ±2σ channel.
struct TrendFit {
    bool   valid     = false;
    double slope     = 0.0;
    double intercept = 0.0;
    double sigma     = 0.0;
    int    firstIdx  = 0;
    int    lastIdx   = 0;
};

inline TrendFit LinearRegression(const std::vector<double>& closes, int lookback) {
    TrendFit r;
    int n = static_cast<int>(closes.size());
    if (lookback < 2 || n < lookback) return r;

    int    first = n - lookback;
    int    last  = n - 1;
    double L     = static_cast<double>(lookback);
    double sumX = 0.0, sumY = 0.0;
    for (int i = first; i <= last; ++i) { sumX += i; sumY += closes[i]; }
    double meanX = sumX / L;
    double meanY = sumY / L;

    double num = 0.0, den = 0.0;
    for (int i = first; i <= last; ++i) {
        double dx = i - meanX;
        num += dx * (closes[i] - meanY);
        den += dx * dx;
    }
    if (den <= 0.0) return r;

    double slope = num / den;
    double intercept = meanY - slope * meanX;

    double ssRes = 0.0;
    for (int i = first; i <= last; ++i) {
        double pred = slope * i + intercept;
        double e    = closes[i] - pred;
        ssRes += e * e;
    }
    double sigma = std::sqrt(ssRes / L);

    r.valid     = true;
    r.slope     = slope;
    r.intercept = intercept;
    r.sigma     = sigma;
    r.firstIdx  = first;
    r.lastIdx   = last;
    return r;
}

enum class LevelSide { Above, Below };

// Filter clusters by side relative to currentPrice and minimum touch count,
// then keep top N sorted by (touches desc, recency desc).
inline std::vector<Level> KeepTopN(const std::vector<Level>& clusters,
                                   double                    currentPrice,
                                   LevelSide                 side,
                                   int                       minTouches,
                                   int                       maxLevels) {
    std::vector<Level> filtered;
    filtered.reserve(clusters.size());
    for (const auto& c : clusters) {
        if (c.touches < minTouches) continue;
        if (side == LevelSide::Above && c.price <= currentPrice) continue;
        if (side == LevelSide::Below && c.price >= currentPrice) continue;
        filtered.push_back(c);
    }
    std::sort(filtered.begin(), filtered.end(),
              [](const Level& a, const Level& b) {
                  if (a.touches != b.touches) return a.touches > b.touches;
                  return a.lastIdx > b.lastIdx;
              });
    if (maxLevels > 0 && static_cast<int>(filtered.size()) > maxLevels)
        filtered.resize(maxLevels);
    return filtered;
}

// ─── Donchian channels ──────────────────────────────────────────────────────
// hi[i] = max(highs[i-N+1..i]), lo[i] = min(lows[i-N+1..i]) for i >= N-1.
// Entries before N-1 are filled with 0.0 (matching the convention used by
// ChartWindow's CalcSMA / CalcEMA / CalcBollingerBands).
struct DonchianResult {
    std::vector<double> hi;
    std::vector<double> lo;
};

inline DonchianResult DonchianBands(const std::vector<double>& highs,
                                    const std::vector<double>& lows,
                                    int                        N) {
    DonchianResult r;
    int n = static_cast<int>(highs.size());
    r.hi.assign(n, 0.0);
    r.lo.assign(n, 0.0);
    if (N <= 0 || n < N || static_cast<int>(lows.size()) != n) return r;
    for (int i = N - 1; i < n; ++i) {
        double h = highs[i - N + 1];
        double l = lows[i  - N + 1];
        for (int j = i - N + 2; j <= i; ++j) {
            if (highs[j] > h) h = highs[j];
            if (lows[j]  < l) l = lows[j];
        }
        r.hi[i] = h;
        r.lo[i] = l;
    }
    return r;
}

// ─── Auto-Fibonacci span ────────────────────────────────────────────────────
// Pick the (swing-high, swing-low) pair with the largest absolute price
// difference among the last `window` swings of each list. valid=false if
// either list is empty or every pair has zero span.
struct AutoFibSpan {
    bool   valid   = false;
    int    hiIdx   = 0;
    double hiPrice = 0.0;
    int    loIdx   = 0;
    double loPrice = 0.0;
};

inline AutoFibSpan LargestSwingSpan(const std::vector<Swing>& highs,
                                    const std::vector<Swing>& lows,
                                    int                       window) {
    AutoFibSpan out;
    if (highs.empty() || lows.empty() || window < 1) return out;
    int hStart = std::max(0, static_cast<int>(highs.size()) - window);
    int lStart = std::max(0, static_cast<int>(lows.size())  - window);
    double bestSpan = 0.0;
    for (int i = hStart; i < static_cast<int>(highs.size()); ++i) {
        for (int j = lStart; j < static_cast<int>(lows.size()); ++j) {
            double span = std::abs(highs[i].price - lows[j].price);
            if (span > bestSpan) {
                bestSpan    = span;
                out.valid   = true;
                out.hiIdx   = highs[i].idx;
                out.hiPrice = highs[i].price;
                out.loIdx   = lows[j].idx;
                out.loPrice = lows[j].price;
            }
        }
    }
    return out;
}

// ─── Classic daily pivot points ─────────────────────────────────────────────
// Pure formula — caller is responsible for picking the previous trading day's
// OHLC. Returns valid=false on degenerate input (non-positive prices or H<L).
//   P  = (H+L+C)/3
//   R1 = 2P - L,  R2 = P + (H-L), R3 = H + 2(P-L)
//   S1 = 2P - H,  S2 = P - (H-L), S3 = L - 2(H-P)
struct DailyPivot {
    bool   valid = false;
    double p  = 0.0;
    double r1 = 0.0, r2 = 0.0, r3 = 0.0;
    double s1 = 0.0, s2 = 0.0, s3 = 0.0;
};

inline DailyPivot ClassicPivots(double prevH, double prevL, double prevC) {
    DailyPivot d;
    if (prevH <= 0.0 || prevL <= 0.0 || prevC <= 0.0 || prevH < prevL) return d;
    d.valid = true;
    d.p  = (prevH + prevL + prevC) / 3.0;
    d.r1 = 2.0 * d.p - prevL;
    d.r2 = d.p + (prevH - prevL);
    d.r3 = prevH + 2.0 * (d.p - prevL);
    d.s1 = 2.0 * d.p - prevH;
    d.s2 = d.p - (prevH - prevL);
    d.s3 = prevL - 2.0 * (prevH - d.p);
    return d;
}

// ─── Breakout markers ──────────────────────────────────────────────────────
// Walk the last `lookback` bars and emit a mark whenever close[i] crosses a
// resistance cluster (up) or support cluster (down). Cluster.firstIdx is used
// as a causality guard: a cluster can only mark bars at i-1 or later.
// minTouches filters out trivially-weak clusters.
//   up   mark y = highs[i] + 0.5·atr[i]
//   down mark y = lows[i]  - 0.5·atr[i]
struct BreakoutMark {
    int    idx;
    double y;
    bool   up;     // true = above resistance, false = below support
};

inline std::vector<BreakoutMark> FindBreakouts(
        const std::vector<Level>&  resistances,
        const std::vector<Level>&  supports,
        const std::vector<double>& highs,
        const std::vector<double>& lows,
        const std::vector<double>& closes,
        const std::vector<double>& atr,
        int                        lookback,
        int                        minTouches) {
    std::vector<BreakoutMark> out;
    int n = static_cast<int>(closes.size());
    if (n < 2 || lookback < 1) return out;
    if (static_cast<int>(highs.size()) != n ||
        static_cast<int>(lows.size())  != n) return out;
    int start = std::max(1, n - lookback);
    for (int i = start; i < n; ++i) {
        double prev = closes[i - 1];
        double cur  = closes[i];
        double off  = (i < static_cast<int>(atr.size())) ? 0.5 * atr[i] : 0.0;
        bool marked = false;
        for (const auto& r : resistances) {
            if (r.touches < minTouches) continue;
            if (r.firstIdx > i - 1)     continue;
            if (prev <= r.price && cur > r.price) {
                out.push_back({i, highs[i] + off, true});
                marked = true;
                break;
            }
        }
        if (marked) continue;
        for (const auto& s : supports) {
            if (s.touches < minTouches) continue;
            if (s.firstIdx > i - 1)     continue;
            if (prev >= s.price && cur < s.price) {
                out.push_back({i, lows[i] - off, false});
                break;
            }
        }
    }
    return out;
}

} // namespace core::services
