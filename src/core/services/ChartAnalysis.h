#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace core::services {

// ============================================================================
// Pure technical-analysis helpers used by ChartWindow's auto-detection layer.
// No IB API / ImGui / ImPlot dependency — testable from tests-core.
// ============================================================================

// ============================================================================
// Classic single-series indicator helpers — extracted from ChartWindow so both
// ChartWindow and ReplayWindow can call them and tests-core can verify the math
// without an IB API or ImGui dependency.
// ============================================================================

// Simple Moving Average. Returns vector of length n; entries before `period-1`
// are 0 (consistent with CalcEMA / CalcBollingerBands convention).
inline std::vector<double> SMA(const std::vector<double>& close, int period) {
    int n = static_cast<int>(close.size());
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n < period) return out;
    double sum = 0.0;
    for (int i = 0; i < period; ++i) sum += close[i];
    out[period - 1] = sum / period;
    for (int i = period; i < n; ++i) {
        sum += close[i] - close[i - period];
        out[i] = sum / period;
    }
    return out;
}

// Exponential Moving Average. Seeded with the SMA of the first `period` values
// at index `period-1`; entries before that index are 0.
inline std::vector<double> EMA(const std::vector<double>& close, int period) {
    int n = static_cast<int>(close.size());
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n < period) return out;
    double k = 2.0 / (period + 1.0), ema = 0.0;
    for (int i = 0; i < period; ++i) ema += close[i];
    ema /= period;
    out[period - 1] = ema;
    for (int i = period; i < n; ++i) {
        ema = close[i] * k + ema * (1.0 - k);
        out[i] = ema;
    }
    return out;
}

// Bollinger Bands result triplet — same vector length as the input close series.
struct BollingerBands {
    std::vector<double> mid;
    std::vector<double> upper;
    std::vector<double> lower;
};

// Bollinger Bands: middle = SMA(period), upper/lower = mid ± sigma·stdev
// (population stdev with N=period). Entries before `period-1` are 0.
inline BollingerBands ComputeBollinger(const std::vector<double>& close,
                                       int period, double sigma) {
    int n = static_cast<int>(close.size());
    BollingerBands bb;
    bb.mid.assign(n, 0.0); bb.upper.assign(n, 0.0); bb.lower.assign(n, 0.0);
    if (period <= 0 || n < period) return bb;
    for (int i = period - 1; i < n; ++i) {
        double sum = 0.0;
        for (int j = i - period + 1; j <= i; ++j) sum += close[j];
        double m = sum / period, var = 0.0;
        for (int j = i - period + 1; j <= i; ++j) var += (close[j] - m) * (close[j] - m);
        double sd = std::sqrt(var / period);
        bb.mid[i]   = m;
        bb.upper[i] = m + sigma * sd;
        bb.lower[i] = m - sigma * sd;
    }
    return bb;
}

// Wilder-style RSI. Returns vector of length n; entries before `period` are 0.
// RSI[period] uses the simple-average seed; subsequent values use Wilder smoothing.
inline std::vector<double> RSI(const std::vector<double>& close, int period) {
    int n = static_cast<int>(close.size());
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n <= period) return out;
    double ag = 0.0, al = 0.0;
    for (int i = 1; i <= period; ++i) {
        double d = close[i] - close[i - 1];
        if (d > 0) ag += d; else al -= d;
    }
    ag /= period; al /= period;
    auto rsi = [](double g, double l) {
        return l == 0.0 ? 100.0 : 100.0 - 100.0 / (1.0 + g / l);
    };
    out[period] = rsi(ag, al);
    for (int i = period + 1; i < n; ++i) {
        double d = close[i] - close[i - 1];
        ag = (ag * (period - 1) + (d > 0 ?  d : 0.0)) / period;
        al = (al * (period - 1) + (d < 0 ? -d : 0.0)) / period;
        out[i] = rsi(ag, al);
    }
    return out;
}

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

// ─── Round to tick increment ────────────────────────────────────────────────
// IB rejects prices finer than the contract's `minTick` with error 110
// ("price does not conform to the minimum price variation"). Most US stocks
// above $1 use $0.01; below $1 they use $0.0001; options are typically $0.05.
// All arithmetic in `AvoidRoundNumber` and `SuggestSetup` / `SuggestStopForPosition`
// involves non-representable fractions like 0.07 / 0.10 that leak ULP-level
// drift, so the public-facing prices must be snapped back to the tick grid
// before they go anywhere near `placeOrder()`.
//
// `tick = 0.0` is treated as a no-op so callers can opt out (e.g. in tests).
inline double RoundToTick(double price, double tick = 0.01) {
    if (tick <= 0.0) return price;
    return std::round(price / tick) * tick;
}

// ─── Round-number avoidance (SL-hunter defense) ─────────────────────────────
// Stops cluster at .00 / .25 / .50 / .75 marks because retail stops do too.
// Adjust `price` so it is at least `pad` dollars away from the nearest mark.
//   pushDown = true  → push *further down* if too close (use for stops below)
//   pushDown = false → push *further up*   if too close (use for stops above)
// Returns the input unchanged when already safe, when pad ≤ 0, or when the
// price is too small to safely apply the padding (price ≤ pad).
inline double AvoidRoundNumber(double price, double pad, bool pushDown) {
    if (pad <= 0.0 || price <= pad) return price;
    static constexpr double kMarks[] = { 0.00, 0.25, 0.50, 0.75, 1.00 };
    double base    = std::floor(price);
    double frac    = price - base;
    double nearest = 0.0;
    double bestD   = std::numeric_limits<double>::infinity();
    for (double r : kMarks) {
        double d = std::abs(frac - r);
        if (d < bestD) { bestD = d; nearest = r; }
    }
    if (bestD >= pad) return price;
    return pushDown ? base + (nearest - pad)
                    : base + (nearest + pad);
}

// ─── Position sizing ────────────────────────────────────────────────────────
// Risk-based share count: floor((riskPct/100 × equity) / |entry - stop|).
// Returns 0 on degenerate input — caller treats 0 as "size unknown".
inline int PositionSizeShares(double equity, double riskPct,
                              double entry, double stop) {
    if (equity <= 0.0 || riskPct <= 0.0) return 0;
    double dist = std::abs(entry - stop);
    if (dist <= 0.0) return 0;
    double riskDollars = (riskPct / 100.0) * equity;
    return static_cast<int>(std::floor(riskDollars / dist));
}

// ─── Setup suggestion (long or short) ───────────────────────────────────────
// Reference-only trade plan derived from the active supply/demand signal.
// Caller picks the active zone and the nearest opposing level via the existing
// KeepTopN result; this helper just assembles entry / stop / target.
//
//   side             1 = long, 0 = short (other values → invalid)
//   zoneTop/zoneBot  buffered zone bounds (m_breakoutZoneTop / m_breakoutZoneBot)
//   anchor           Level.minPrice of the demand zone (long) or
//                    Level.maxPrice of the supply zone (short) — the longest-wick
//                    edge, used as the structural anchor for the SL
//   opposingLevel    nearest resistance above (long) or support below (short);
//                    becomes the take-profit target
//   atr              ATR(14) at the latest bar
//   last             latest close
//   atrPad           SL padding multiplier (k × ATR)
//   roundPad         round-number avoidance pad in dollars
//   stopOffset       stop-limit offset from stop trigger (subtracted for long,
//                    added for short)
//   rrMin            minimum (target - entry)/(entry - stop) magnitude
//   equity           NetLiquidation in dollars; 0 = skip share calc
//   riskPct          % of equity per trade
struct SetupPlan {
    bool   valid   = false;
    int    side    = 0;        // 1 = long, 0 = short
    double entry   = 0.0;
    double stop    = 0.0;
    double stopLmt = 0.0;
    double target  = 0.0;
    double rr      = 0.0;
    int    shares  = 0;
};

inline SetupPlan SuggestSetup(int side,
                              double zoneTop, double zoneBot,
                              double anchor,
                              double opposingLevel,
                              double atr, double last,
                              double atrPad,
                              double roundPad,
                              double stopOffset,
                              double rrMin,
                              double equity, double riskPct) {
    SetupPlan p;
    if (atr <= 0.0 || zoneTop <= zoneBot) return p;
    if (side != 0 && side != 1)            return p;

    double mid = 0.5 * (zoneTop + zoneBot);

    if (side == 1) {
        double entry  = (last >= zoneTop) ? last : mid;
        double rawSt  = anchor - atrPad * atr;
        double stop   = AvoidRoundNumber(rawSt, roundPad, /*pushDown=*/true);
        double target = opposingLevel;

        double risk   = entry  - stop;
        double reward = target - entry;
        if (risk <= 0.0 || reward <= 0.0) return p;
        double rr = reward / risk;
        if (rr < rrMin) return p;

        p.valid   = true;
        p.side    = 1;
        p.entry   = RoundToTick(entry);
        p.stop    = RoundToTick(stop);
        p.stopLmt = RoundToTick(stop - stopOffset);
        p.target  = RoundToTick(target);
        p.rr      = rr;
        p.shares  = PositionSizeShares(equity, riskPct, p.entry, p.stop);
    } else {
        double entry  = (last <= zoneBot) ? last : mid;
        double rawSt  = anchor + atrPad * atr;
        double stop   = AvoidRoundNumber(rawSt, roundPad, /*pushDown=*/false);
        double target = opposingLevel;

        double risk   = stop  - entry;
        double reward = entry - target;
        if (risk <= 0.0 || reward <= 0.0) return p;
        double rr = reward / risk;
        if (rr < rrMin) return p;

        p.valid   = true;
        p.side    = 0;
        p.entry   = RoundToTick(entry);
        p.stop    = RoundToTick(stop);
        p.stopLmt = RoundToTick(stop + stopOffset);
        p.target  = RoundToTick(target);
        p.rr      = rr;
        p.shares  = PositionSizeShares(equity, riskPct, p.entry, p.stop);
    }
    return p;
}

// ─── Protective stop for an existing position ───────────────────────────────
// Builds a stop suggestion based on auto-detected S/R levels.
//   isLong = true:  pick the closest support cluster BELOW entry (touches ≥ 2),
//                   stop = support.minPrice − atrPad × atr, push DOWN of round
//   isLong = false: pick the closest resistance cluster ABOVE entry (touches ≥ 2),
//                   stop = resistance.maxPrice + atrPad × atr, push UP of round
// `levels` is normally m_autoSupports (long) or m_autoResistances (short) — the
// already-filtered KeepTopN output.
struct PositionStop {
    bool   valid   = false;
    double stop    = 0.0;
    double stopLmt = 0.0;
    double pctRisk = 0.0;     // |entry - stop| / entry × 100
};

inline PositionStop SuggestStopForPosition(bool isLong,
                                           double entry,
                                           const std::vector<Level>& levels,
                                           double atr,
                                           double atrPad,
                                           double roundPad,
                                           double stopOffset) {
    PositionStop r;
    if (entry <= 0.0 || atr <= 0.0 || levels.empty()) return r;

    const Level* best = nullptr;
    if (isLong) {
        for (const auto& L : levels) {
            if (L.touches < 2)    continue;
            if (L.price >= entry) continue;
            if (!best || L.price > best->price) best = &L;
        }
    } else {
        for (const auto& L : levels) {
            if (L.touches < 2)    continue;
            if (L.price <= entry) continue;
            if (!best || L.price < best->price) best = &L;
        }
    }
    if (!best) return r;

    if (isLong) {
        double rawSt = best->minPrice - atrPad * atr;
        double stop  = AvoidRoundNumber(rawSt, roundPad, /*pushDown=*/true);
        r.valid   = true;
        r.stop    = RoundToTick(stop);
        r.stopLmt = RoundToTick(stop - stopOffset);
        r.pctRisk = (entry - r.stop) / entry * 100.0;
    } else {
        double rawSt = best->maxPrice + atrPad * atr;
        double stop  = AvoidRoundNumber(rawSt, roundPad, /*pushDown=*/false);
        r.valid   = true;
        r.stop    = RoundToTick(stop);
        r.stopLmt = RoundToTick(stop + stopOffset);
        r.pctRisk = (r.stop - entry) / entry * 100.0;
    }
    return r;
}

// ─── Session-anchored VWAP + volume-weighted ±σ bands ──────────────────────
// Volume-weighted average price with optional intraday session resets.
// `sessionStarts` lists bar indices where the running cumulator resets (typically
// derived from ET-day boundaries by ChartWindow); pass an empty vector for a
// single cumulative run from index 0 (matches non-intraday behaviour).
//
// Bands use volume-weighted variance: var = E[X²] − E[X]² on the
// running distribution, single-pass O(n). The clamp `var < 0 → 0` guards the
// degenerate "all bars identical" cancellation case.
//
// Zero-volume bars carry the previous values forward (no NaN, no flicker).
// First bar with zero volume falls back to its close so the output isn't 0.
struct VwapResult {
    std::vector<double> vwap;
    std::vector<double> sd1Up;
    std::vector<double> sd1Dn;
    std::vector<double> sd2Up;
    std::vector<double> sd2Dn;
};

inline VwapResult SessionVwap(const std::vector<double>& highs,
                              const std::vector<double>& lows,
                              const std::vector<double>& closes,
                              const std::vector<double>& volumes,
                              const std::vector<int>&    sessionStarts) {
    VwapResult r;
    int n = static_cast<int>(closes.size());
    r.vwap.assign(n, 0.0);
    r.sd1Up.assign(n, 0.0);
    r.sd1Dn.assign(n, 0.0);
    r.sd2Up.assign(n, 0.0);
    r.sd2Dn.assign(n, 0.0);
    if (n == 0 ||
        static_cast<int>(highs.size())   != n ||
        static_cast<int>(lows.size())    != n ||
        static_cast<int>(volumes.size()) != n) return r;

    std::size_t nextStart = 0;
    double cumTPV  = 0.0;
    double cumVol  = 0.0;
    double cumTPV2 = 0.0;     // Σ vol × typical²

    for (int i = 0; i < n; ++i) {
        bool reset = (i == 0);
        while (nextStart < sessionStarts.size() && sessionStarts[nextStart] <= i) {
            if (sessionStarts[nextStart] == i) reset = true;
            ++nextStart;
        }
        if (reset) {
            cumTPV  = 0.0;
            cumVol  = 0.0;
            cumTPV2 = 0.0;
        }

        double typ = (highs[i] + lows[i] + closes[i]) / 3.0;
        double vol = volumes[i];

        if (vol > 0.0) {
            cumTPV  += typ * vol;
            cumVol  += vol;
            cumTPV2 += typ * typ * vol;
        }

        if (cumVol > 0.0) {
            double vwap   = cumTPV / cumVol;
            double meanSq = cumTPV2 / cumVol;
            double varVw  = meanSq - vwap * vwap;
            if (varVw < 0.0) varVw = 0.0;
            double sd     = std::sqrt(varVw);
            r.vwap[i]  = vwap;
            r.sd1Up[i] = vwap + sd;
            r.sd1Dn[i] = vwap - sd;
            r.sd2Up[i] = vwap + 2.0 * sd;
            r.sd2Dn[i] = vwap - 2.0 * sd;
        } else if (i > 0 && !reset) {
            r.vwap[i]  = r.vwap[i - 1];
            r.sd1Up[i] = r.sd1Up[i - 1];
            r.sd1Dn[i] = r.sd1Dn[i - 1];
            r.sd2Up[i] = r.sd2Up[i - 1];
            r.sd2Dn[i] = r.sd2Dn[i - 1];
        } else {
            r.vwap[i]  = closes[i];
            r.sd1Up[i] = closes[i];
            r.sd1Dn[i] = closes[i];
            r.sd2Up[i] = closes[i];
            r.sd2Dn[i] = closes[i];
        }
    }
    return r;
}

// ─── Order impact — side-intent badge + P&amp;L preview ──────────────────────────
// Pure helpers that compute what an order would do to an existing position
// before it is submitted. Used by both ChartWindow and TradingWindow to render
// the side-intent badge (OPEN LONG / ADD TO LONG / REDUCE / CLOSE / FLIP) and
// the projected P&amp;L at the prices currently entered in the order form.
//
// Caller derives `fillPrice` from the staged order type:
//   MKT / MOC / MTL       → last (current price)
//   LMT / LOC             → limitPrice
//   STP                   → stopPrice
//   STP LMT               → stopPrice for the stop leg, limitPrice for the fill leg
//   TRAIL / TRAIL LIMIT   → last ± trailAmount
//   MIT                   → auxPrice (trigger)
//   LIT                   → auxPrice (trigger), limitPrice (fill)
//   MIDPRICE              → last
//   REL                   → last ± pegOffset

enum class OrderImpactKind {
    OpenLong,            // no position → BUY
    OpenShort,           // no position → SELL
    AddToLong,           // long pos    → BUY
    AddToShort,          // short pos   → SELL
    ReduceLong,          // long pos    → SELL (partial close)
    ReduceShort,         // short pos   → BUY  (partial close)
    CloseLong,           // long pos    → SELL (exact close)
    CloseShort,          // short pos   → BUY  (exact close)
    FlipToShort,         // long pos    → SELL (qty > posQty)
    FlipToLong,          // short pos   → BUY  (qty > |posQty|)
    Invalid,             // qty ≤ 0, side unset, etc.
};

struct OrderImpact {
    OrderImpactKind kind          = OrderImpactKind::Invalid;
    double          closeQty      = 0.0;   // units that hit avgCost basis
    double          openQty       = 0.0;   // units left over (flip case only)
    double          closePnL      = 0.0;   // realised P&amp;L on the closing leg, net of commission
    double          newAvgCost    = 0.0;   // post-fill avg cost (Open / AddTo paths)
    double          newPosQty     = 0.0;   // signed position after fill
    bool            isClosingPath = false; // true for Reduce / Close / Flip — closePnL is meaningful
};

// posQty: signed (+ long, - short, 0 flat).  avgCost: always ≥ 0.
// commissionPerShare: per-share commission attributable to this position.
// orderQty: always positive (absolute shares in the order form).
inline OrderImpact ComputeOrderImpact(double posQty, double avgCost,
                                      double commissionPerShare,
                                      bool   isBuy, double orderQty,
                                      double fillPrice) {
    OrderImpact r;
    if (orderQty <= 0.0 || fillPrice <= 0.0) return r;   // Invalid

    double absPos = std::abs(posQty);

    if (absPos <= 0.0) {
        // ── No existing position ──────────────────────────────────────────
        if (isBuy) {
            r.kind      = OrderImpactKind::OpenLong;
            r.newAvgCost = fillPrice;
            r.newPosQty  = orderQty;
        } else {
            r.kind      = OrderImpactKind::OpenShort;
            r.newAvgCost = fillPrice;
            r.newPosQty  = -orderQty;
        }
        r.openQty = orderQty;
        return r;
    }

    if (posQty > 0.0) {
        // ── Existing long position ────────────────────────────────────────
        if (isBuy) {
            r.kind       = OrderImpactKind::AddToLong;
            r.newPosQty  = posQty + orderQty;
            r.newAvgCost = (posQty * avgCost + orderQty * fillPrice) / r.newPosQty;
            r.openQty    = orderQty;
        } else {
            double closeQty = std::min(orderQty, absPos);
            r.closeQty      = closeQty;
            r.isClosingPath = true;
            r.closePnL      = (fillPrice - avgCost) * closeQty
                              - commissionPerShare * closeQty;
            r.newPosQty     = posQty - closeQty;
            r.openQty       = orderQty - closeQty;

            if (closeQty < absPos) {
                r.kind = OrderImpactKind::ReduceLong;
            } else if (orderQty == absPos) {
                r.kind = OrderImpactKind::CloseLong;
            } else {
                r.kind       = OrderImpactKind::FlipToShort;
                r.newAvgCost = fillPrice;
                r.newPosQty  = -r.openQty;
            }
        }
        return r;
    }

    // ── Existing short position (posQty < 0) ─────────────────────────────
    if (!isBuy) {
        r.kind       = OrderImpactKind::AddToShort;
        r.newPosQty  = posQty - orderQty;
        r.newAvgCost = (absPos * avgCost + orderQty * fillPrice) / (absPos + orderQty);
        r.openQty    = orderQty;
    } else {
        double closeQty = std::min(orderQty, absPos);
        r.closeQty      = closeQty;
        r.isClosingPath = true;
        r.closePnL      = (avgCost - fillPrice) * closeQty
                          - commissionPerShare * closeQty;
        r.newPosQty     = posQty + closeQty;
        r.openQty       = orderQty - closeQty;

        if (closeQty < absPos) {
            r.kind = OrderImpactKind::ReduceShort;
        } else if (orderQty == absPos) {
            r.kind = OrderImpactKind::CloseShort;
        } else {
            r.kind       = OrderImpactKind::FlipToLong;
            r.newAvgCost = fillPrice;
            r.newPosQty  = r.openQty;
        }
    }
    return r;
}

// ─── Risk preview for paired Stop-Limit-style setups ─────────────────────────
// Computes both target P&amp;L and stop-out P&amp;L from two OrderImpact legs (the
// closing portion of a trade plan). R:R = |reward| / |risk|.
struct StopTargetPreview {
    double targetPnL = 0.0;
    double stopPnL   = 0.0;   // negative for losses
    double rrRatio   = 0.0;   // 0 if either leg is undefined
    bool   valid     = false;
};

inline StopTargetPreview PreviewStopTarget(const OrderImpact& target,
                                           const OrderImpact& stop) {
    StopTargetPreview p;
    if (!target.isClosingPath || !stop.isClosingPath) return p;
    if (target.closeQty <= 0.0 || stop.closeQty <= 0.0) return p;
    double reward = target.closePnL;
    double risk   = -stop.closePnL;   // stop is a loss → positive risk
    if (risk <= 0.0) return p;
    p.valid     = true;
    p.targetPnL = reward;
    p.stopPnL   = stop.closePnL;
    p.rrRatio   = reward / risk;
    return p;
}

} // namespace core::services
