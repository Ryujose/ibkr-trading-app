# Plan: Auto Technical Analysis on ChartWindow

**Status:** Complete — Tasks A, B, C, and D all done
**Owner:** Jose
**Goal:** Automated, toggleable overlays in `ChartWindow` so the user no longer has
to draw S/R/trend by hand. Approved scope = supports + resistances + trend + all
five suggested extras.

This file is the source of truth across sessions — update the **Status** line and the
per-task checkboxes as work progresses.

---

## 1. Approved scope

| # | Feature | Default | Notes |
|---|---|---|---|
| 1 | Auto **Resistances** | ON | Top-N pivot-high clusters above current price |
| 2 | Auto **Supports** | ON | Top-N pivot-low clusters below current price |
| 3 | Auto **Trend** line | ON | Linear-regression on last L closes |
| 4 | **Donchian (20)** | OFF | Highest-high / lowest-low envelope |
| 5 | **Keltner channels** | OFF | EMA20 ± 2·ATR(14) |
| 6 | **Auto-Fibonacci** | OFF | Most recent significant swing-high → swing-low |
| 7 | **Daily pivot points** | OFF | Classic P/S1-S3/R1-R3 from prior day OHLC; intraday only |
| 8 | **Breakout markers** | OFF | ▲/▼ on past bars that already closed through a detected S/R |
| 9 | **Supply/demand zones + imminent-breakout signal** | OFF | Filled rectangles around clustered swings; live arrow + "LONG SETUP" / "SHORT SETUP" label when price is inside a zone with BB compression and directional momentum |

All toggleable from `DrawAnalysisToolbar()`. Defaults reflect signal-to-noise
trade-off: the three primary tools on, the rest off so first-time users see the
chart unchanged from today.

## 2. Resolved decisions (from the conversation)

- **Lookback** — detect on the full loaded series, but cap the swing scan at the
  last **1000 bars** so very long histories stay snappy. (Override: per-instance
  setting in the popup.)
- **Style** — thin dashed lines, matching the existing manual H-Line / Fibonacci
  rendering. No filled zones in v1.
- **Settings UI** — inline checkboxes for the toggles, plus a single **"Auto…"**
  small-button that opens a popup with the parameters (swingK, trendLookback,
  maxLevels, minTouches, donchianLen).
- **Persistence** — in-memory only, per-instance, matching the existing
  `IndicatorSettings`. No disk persistence.
- **Tests** — pure-logic helpers extracted to `src/core/services/ChartAnalysis.h`
  (no ImGui / IB API dependency). Unit-tested via `tests-core` —
  `tests/test_chart_analysis.cpp` added to `tests-core` source list in
  `tests/CMakeLists.txt`.

## 3. Architecture

### 3a. Files

| Path | Purpose | Status |
|---|---|---|
| `src/core/services/ChartAnalysis.h` | Pure helpers (`FindSwings`, `ClusterLevels`, `LinearRegression`, `ATR`, `DonchianBands`, `ClassicPivots`) | NEW |
| `src/ui/windows/ChartWindow.h` | Add `AutoAnalysisSettings` struct + result fields + `DetectStructure()` decl | EDIT |
| `src/ui/windows/ChartWindow.cpp` | Wire compute + draw + toolbar | EDIT |
| `tests/test_chart_analysis.cpp` | Catch2 cases for each helper | NEW |
| `tests/CMakeLists.txt` | Add the new test source to `tests-core` | EDIT |

### 3b. New struct (header, alongside `IndicatorSettings`)

```cpp
struct AutoAnalysisSettings {
    bool supports     = true;
    bool resistances  = true;
    bool trend        = true;
    bool donchian     = false;
    bool keltner      = false;
    bool autoFib      = false;
    bool pivotPoints  = false;
    bool breakouts    = false;

    int  swingK         = 3;     // pivot left/right window
    int  trendLookback  = 50;
    int  donchianLen    = 20;
    int  maxLevels      = 3;     // top-N S and R
    int  minTouches     = 2;
    int  scanCap        = 1000;  // cap swing scan to last N bars (0 = unlimited)
    bool trendChannel   = false; // ±2σ regression bands
};
```

### 3c. New result fields (header, private)

```cpp
struct AutoLevel { double price; int touches; double firstX; double lastX; };
struct AutoTrend {
    bool   valid = false;
    double x1, y1, x2, y2;       // line endpoints (in idx, price)
    double slope, intercept, sigma;
};

AutoAnalysisSettings        m_auto;
std::vector<AutoLevel>      m_autoSupports;
std::vector<AutoLevel>      m_autoResistances;
AutoTrend                   m_autoTrend;
std::vector<double>         m_donchHi;       // size = bars; NaN where undef
std::vector<double>         m_donchLo;
std::vector<double>         m_keltUpper;     // EMA20 + 2*ATR
std::vector<double>         m_keltLower;
std::vector<double>         m_atr14;         // also reused by other features
struct AutoFibSpan { bool valid; double xHi, yHi, xLo, yLo; };
AutoFibSpan                 m_autoFib;
struct DailyPivot { double p, r1, r2, r3, s1, s2, s3; bool valid; };
DailyPivot                  m_pivots;
struct BreakoutMark { double x; double price; bool up; }; // up=through resistance
std::vector<BreakoutMark>   m_breakouts;
```

### 3d. New private methods (header)

```cpp
void DetectStructure();          // top-level — populates everything in m_auto*
void DrawAutoSupportResistance(); // overlay rendering (called from DrawOverlays)
void DrawAutoTrend();             // overlay rendering
void DrawAutoFib();               // overlay rendering
void DrawAutoPivots();            // overlay rendering
void DrawDonchian();              // ImPlot lines (called from DrawCandleChart)
void DrawKeltner();               // ImPlot lines
void DrawBreakoutMarks();         // ImPlot scatter
void DrawAutoSettingsPopup();     // popup body for the "Auto…" button
```

### 3e. Wiring points

- `DetectStructure()` is called from every site that currently calls
  `ComputeIndicators()` (lines 157, 170, 200, 225, 243, 258, 339, 553 in the
  current `ChartWindow.cpp`). Add it immediately after each `ComputeIndicators()`
  call.
- `DrawAnalysisToolbar()` (currently around line 586) gets the new checkboxes +
  the "Auto…" button before the existing "Clear All" button.
- `DrawOverlays()` (currently around line 1202) — draw S/R/trend/autoFib/pivots
  *after* the current-price line, *before* the user drawings (so user drawings
  stay on top).
- `DrawCandleChart()` (currently around line 1833) — draw Donchian/Keltner as
  ImPlot lines next to the existing BB plot (so they participate in the legend).
  Breakout markers via `ImPlot::PlotScatter`.

### 3f. ReqId / data dependencies

None. All inputs are already in `m_xs`, `m_idxs`, `m_opens`, `m_highs`, `m_lows`,
`m_closes`, `m_volumes`. No new IB subscriptions needed.

---

## 4. Algorithms (reference)

### 4a. Pivot detection (`FindSwings`)

```
For each i in [k, n-k):
    isHigh = high[i] > max(high[i-k..i-1]) AND high[i] >= max(high[i+1..i+k])
    isLow  = low[i]  < min(low[i-k..i-1])  AND low[i]  <= min(low[i+1..i+k])
```
Strict on the left edge, ≥ on the right edge — so a flat top still registers as
a single swing rather than `k` of them. Returns two vectors of `{idx, price}`.

### 4b. Clustering (`ClusterLevels`)

1. Sort swings by price ascending.
2. Sweep: start a new cluster when next swing's price is more than `tol` away
   from the last swing's price. `tol = max(0.003 * lastClose, 0.5 * ATR14)`.
3. Cluster price = simple average of constituent prices (volume-weighting is a
   v2 polish — keep simple to start).
4. Cluster `firstX` = min idx of constituents, `lastX` = max idx of constituents.
5. `touches = constituent count`.

### 4c. Top-N filter

- `keepN(clusters, currentPrice, side)` →
  - filter by `touches >= minTouches`
  - keep only clusters above (resistance) / below (support) current price
  - sort by `(touches desc, recency desc)` — recency = `lastX`
  - return first `maxLevels`

### 4d. Linear regression (`LinearRegression`)

Standard closed-form on `(idx, close)` pairs over the last L bars:
```
mean_x = ...; mean_y = ...
slope     = Σ((x-mean_x)*(y-mean_y)) / Σ((x-mean_x)²)
intercept = mean_y - slope * mean_x
sigma     = sqrt(Σ((y - (slope*x + intercept))²) / (L - 2))
```
Project line forward `L/4` bars (faded alpha).

### 4e. ATR(14) (`ATR`)

Wilder's true range:
```
TR[i] = max(high[i] - low[i],
            |high[i] - close[i-1]|,
            |low[i]  - close[i-1]|)
ATR[i] = (ATR[i-1] * 13 + TR[i]) / 14   for i >= 14
```
Seed `ATR[14] = average(TR[1..14])`.

### 4f. Donchian (`DonchianBands`)

For each i ≥ N: `hi[i] = max(high[i-N+1..i])`, `lo[i] = min(low[i-N+1..i])`.
Default N=20.

### 4g. Keltner

Reuse existing `m_ema` (EMA20) plus `m_atr14`:
```
upper[i] = ema20[i] + 2 * atr14[i]
lower[i] = ema20[i] - 2 * atr14[i]
```

### 4h. Auto-Fibonacci

Take the most recent significant swing-high and swing-low (largest absolute
price difference among the last ~30 swings). Render as the existing
`Drawing::Fibonacci` style — but kept in `m_autoFib` so it doesn't get cleared
by "Clear All" on user drawings.

### 4i. Daily pivot points (`ClassicPivots`)

Intraday only (`IsIntraday(m_timeframe) == true`). Find the previous trading
day's OHLC by walking `m_xs` backwards from the most recent bar:
```
P  = (H + L + C) / 3
R1 = 2P - L,     R2 = P + (H - L),    R3 = H + 2*(P - L)
S1 = 2P - H,     S2 = P - (H - L),    S3 = L - 2*(H - P)
```
Render seven dashed h-lines spanning today's bars only (use `firstX`/`lastX` of
today's session for the line range).

### 4j. Breakout markers

After S/R detection, walk the last ~50 bars: any bar whose `close` rose above a
prior resistance level (and `close[i-1]` was below) is an `up` breakout; mirror
for support → `down`. Render as ▲/▼ at high+ATR/low-ATR offset.

### 4k. Supply/demand zones + imminent-breakout signal

A **zone** is a thickness-aware version of a `Level`. Each cluster from
`ClusterLevels` already tracks `firstIdx`, `lastIdx`, and now also `minPrice` /
`maxPrice` (extension to the struct — populated from constituent swing prices).
Zone vertical bounds = `(minPrice - buffer)` to `(maxPrice + buffer)`, where
`buffer = 0.5 * ATR(14)`. Resistance clusters become **supply zones** (drawn red)
and supports become **demand zones** (drawn green).

The **imminent-breakout signal** is computed in `DetectStructure()` per recompute:

1. Walk supply zones first, then demand zones; find the zone whose
   `[bot, top]` interval contains `closes[n-1]`. If none, no signal.
2. **Bollinger-Band compression** — using the existing `m_bbUpper`/`m_bbLower`,
   require `bbWidth[n-1] < 0.7 * avg(bbWidth[n-50..n-1])`.
3. **Directional momentum** — let `recent5Avg = mean(closes[n-6..n-2])`. Require
   either `closes[n-1] - recent5Avg > 0.1 * ATR` (bullish) or
   `< -0.1 * ATR` (bearish).
4. **Position within zone** — if `last > midZone` AND bullish → `LongSetup`.
   If `last < midZone` AND bearish → `ShortSetup`. Otherwise no signal.

Render (when the `Zones` toggle is on):
- Each zone as a filled translucent rectangle with a thin border, spanning the
  full visible X range.
- When a signal is active, a coloured triangle on the right edge (▲ green for
  long, ▼ red for short) at the zone midpoint, with a `" LONG SETUP "` /
  `" SHORT SETUP "` label tag to its left.

Single signal at a time — no stacking even if multiple zones overlap.

---

## 5. Task breakdown

### Task A — Supports / Resistances + analysis header + tests
**Status:** DONE (2026-04-25)

- [x] Create `src/core/services/ChartAnalysis.h` with:
  - [x] `Swing { int idx; double price; };`
  - [x] `Level { double price; int touches; int firstIdx; int lastIdx; };`
  - [x] `FindSwings(highs, lows, k, scanCap) -> {highs, lows}`
  - [x] `ATR(highs, lows, closes, period) -> std::vector<double>`
  - [x] `ClusterLevels(swings, tol) -> std::vector<Level>`
  - [x] `KeepTopN(levels, currentPrice, side, minTouches, maxLevels) -> std::vector<Level>`
- [x] `ChartWindow.h` — add `AutoAnalysisSettings`, result fields, method decls.
- [x] `ChartWindow.cpp`:
  - [x] `DetectStructure()` — populate `m_atr14`, `m_autoSupports`, `m_autoResistances`. Lives at the end of `ComputeIndicators()` so every existing recompute site picks it up automatically.
  - [x] `DrawAnalysisToolbar()` — added "Auto:" sub-section with `Sup`, `Res`, `AutoTrend` checkboxes + `Auto...` small button. Toggling Sup/Res calls `DetectStructure()` directly; AutoTrend toggle is wired (no rendering yet — Task B).
  - [x] `DrawAutoSettingsPopup()` — exposes `swingK`, `minTouches`, `maxLevels`, `scanCap`. Each change re-runs `DetectStructure()`.
  - [x] `DrawOverlays()` — `DrawAutoSupportResistance()` called between current-price line and user drawings. Red dashed lines for resistance, green for support, alpha scaled with touch count, left-edge price tag formatted ` R 187.42 (3×) `.
- [x] `tests/test_chart_analysis.cpp` — 16 cases / 58 assertions covering:
  - [x] `FindSwings`: too-small input, simple peak/trough, W-shape, scanCap behaviour, flat-top edge case.
  - [x] `ATR`: too-small input, constant true range.
  - [x] `ClusterLevels`: empty input, within-tolerance merge, outside-tolerance separation, ascending price order.
  - [x] `KeepTopN`: side + min-touches filter, max-levels cap with (touches desc, recency desc) ordering, `maxLevels=0` = no cap.
  - [x] Full pipeline: synthesised V-shape series → swing detection → clustering → KeepTopN identifies the support cluster at the expected price.
- [x] `tests/CMakeLists.txt` — `test_chart_analysis.cpp` appended to `tests-core` sources.
- [x] `cmake --build build -j$(nproc)` clean; `ctest` 82/82 pass.

**Notes / changes from the plan:**
- The S/R *toggle* labels in the toolbar are `Sup`/`Res`/`AutoTrend` (not `Supports`/`Resistances`/`Trend`) — kept short to avoid colliding with the existing `Trend` drawing-tool button on the same row.
- Right-edge price tags would have collided with the current-price tag, so S/R tags render at the **left edge** of the plot rect instead. The format is ` R 187.42 (3×) ` (UTF-8 multiplication sign).
- `DetectStructure()` is invoked from inside `ComputeIndicators()` (single call site) — every existing recompute trigger now also refreshes auto-analysis. Toolbar checkboxes call `DetectStructure()` directly to avoid recomputing the unchanged indicator buffers.

### Task B — Trend (linear regression) + optional channel
**Status:** DONE (2026-04-25)

- [x] `ChartAnalysis.h`: `LinearRegression(closes, lookback) → TrendFit{valid, slope, intercept, sigma, firstIdx, lastIdx}` — closed-form OLS over the trailing `lookback` closes (x = bar idx). Returns `valid=false` on too-small input or zero-variance x.
- [x] `ChartWindow.h`: `using AutoTrend = core::services::TrendFit`; `m_autoTrend` member; `DrawAutoTrend()` decl.
- [x] `DetectStructure()` — populates `m_autoTrend` from `LinearRegression(m_closes, min(trendLookback, n))` when `m_auto.trend` is on; reset to `AutoTrend{}` otherwise.
- [x] `DrawOverlays()` — `DrawAutoTrend()`:
  - [x] line from `(firstIdx, fit(firstIdx))` to `(lastIdx, fit(lastIdx))` solid; projection segment from `(lastIdx, …)` to `(lastIdx + L/4, …)` at 60% alpha.
  - [x] colour: `slope > +ε` green, `slope < -ε` red, else grey; ε = `0.05·sigma/L`.
  - [x] if `m_auto.trendChannel`, draw `±2σ` parallel lines on both segments (50% alpha on the fit, fainter on the projection).
  - [x] right-edge label `" trend +0.123/bar "` colour-matched to the slope sign.
- [x] Toolbar: "AutoTrend" checkbox already added in Task A; toggling it now calls `DetectStructure()`. Auto… popup gains `Trend lookback` (5–500) and `Show ±2σ channel` controls.
- [x] Tests in `tests/test_chart_analysis.cpp`: LR on `y = 2x + 5` recovers slope=2, intercept=5, sigma=0; LR on flat line returns slope=0; lookback < n fits trailing window with correct firstIdx/lastIdx; period-4 noise pattern preserves unbiased slope=1 with sigma=1; too-small / empty input → `valid=false`.

### Task D — Supply/demand zones + imminent-breakout signal
**Status:** DONE (2026-04-25)

- [x] `ChartAnalysis.h`: extended `Level` with `minPrice`, `maxPrice`. `ClusterLevels` populates them from constituent swing prices. Tests cover the new fields (single-swing, multi-swing within tolerance, mixed-input attribution).
- [x] `ChartWindow.h`: added `bool zones = false` to `AutoAnalysisSettings`; added `enum class BreakoutDirection { None, LongSetup, ShortSetup }`; added members `m_breakoutSignal`, `m_breakoutZoneTop`, `m_breakoutZoneBot`, `m_breakoutFromSupply`. Decls: `DrawAutoZones`, `ComputeBreakoutSignal`.
- [x] `ChartWindow.cpp`:
  - [x] `ComputeBreakoutSignal()` per the algorithm in §4k. Called from the tail of `DetectStructure()` when `m_auto.zones` is on, after the S/R clusters are populated. Requires n≥50, valid ATR(14), ≥20 valid BB widths in the last 50 bars.
  - [x] `DrawAutoZones()` invoked from `DrawOverlays()` between the current-price line and `DrawAutoSupportResistance()` so the dashed S/R lines render on top of the rectangles. Translucent red fill + thin red border for supply, translucent green + green border for demand. Right-edge ▲/▼ glyph + ` LONG SETUP ` / ` SHORT SETUP ` label tag when signal is active.
  - [x] `Zones` checkbox in the Auto: section of `DrawAnalysisToolbar` (default OFF). Toggling re-runs `DetectStructure()`.
- [x] `DetectStructure()` updated: still detects S/R when `zones` is on even if the individual `supports`/`resistances` toggles are off, since zones reuse the same cluster output.
- [x] Tests: `tests/test_chart_analysis.cpp` extended with two new cluster cases (`minPrice`/`maxPrice` from mixed input, single-swing min==max==price) and the V-shape end-to-end test now asserts every cluster's min/max agrees with its actual swing constituents.
- [x] Build clean (only pre-existing warnings); ctest 83/83 pass.

**Notes / changes from the plan:**
- The `[bot, top]` containment check uses the already-buffered `(minPrice - 0.5·ATR, maxPrice + 0.5·ATR)` interval; KeepTopN's strict `>`/`<` filter against current price is fine because zone bounds extend beyond the cluster mean — a resistance whose mean is above `last` can still have its lower edge below `last`.
- The BB compression check filters out zero-width bars (BB is undefined before period-1) and requires ≥20 valid samples before trusting the average — keeps short series from triggering false positives.

### Task C — Five extras (one PR or split as desired)
**Status:** DONE (2026-04-26)

- [x] **Donchian**:
  - [x] `ChartAnalysis.h`: `DonchianBands(highs, lows, N) -> DonchianResult{hi, lo}` (rolling N-bar max/min; 0-prefix before period to match `CalcSMA`/`CalcBollingerBands` convention).
  - [x] `DetectStructure()` populates `m_donchHi`/`m_donchLo` when `m_auto.donchian` is on.
  - [x] `DrawCandleChart()` calls `DrawDonchian()` which plots two `ImPlot::PlotLine` calls in faded yellow (`Donch Hi`/`Donch Lo` legend names).
  - [x] Toolbar checkbox "Donch" in the Auto: row.
  - [x] Tests: rolling max/min on a known 9-bar sequence; too-small input returns zeros; window==n collapses to a single value at the last index.
- [x] **Keltner**:
  - [x] Reuses `m_ema` (always computed in `ComputeIndicators`, regardless of the EMA20 indicator toggle) and `m_atr14`. `DetectStructure()` populates `m_keltUpper = m_ema + 2·m_atr14` / `m_keltLower = m_ema - 2·m_atr14` element-wise, skipping bars where either input is 0.
  - [x] `DrawCandleChart()` calls `DrawKeltner()` which plots two `ImPlot::PlotLine` calls in faded cyan (`Kelt Hi`/`Kelt Lo`).
  - [x] Toolbar checkbox "Kelt" in the Auto: row.
- [x] **Auto-Fib**:
  - [x] `ChartAnalysis.h`: `LargestSwingSpan(swingHighs, swingLows, window) -> AutoFibSpan{valid, hiIdx, hiPrice, loIdx, loPrice}` — picks the (high, low) pair with max `|hiPrice - loPrice|` among the last `window` swings of each list.
  - [x] `DetectStructure()` populates `m_autoFib = LargestSwingSpan(sw.highs, sw.lows, 30)` when `m_auto.autoFib` is on.
  - [x] `DrawAutoFib()` invoked from `DrawOverlays()` after S/R/trend; renders six dashed h-lines at `kFibLevels[]` in faded `kFibColors` with **left-edge** ` F 38.2% 187.42 ` labels (manual fibs use right-edge labels — opposite edges avoids collisions). Small anchor circles at the swing-high (orange) and swing-low (blue).
  - [x] Toolbar checkbox "AutoFib" in the Auto: row.
  - [x] Tests: max-span pair across all considered swings; window=1 picks only most-recent of each; empty list / zero-span → invalid.
- [x] **Daily pivots**:
  - [x] `ChartAnalysis.h`: `ClassicPivots(prevH, prevL, prevC) -> DailyPivot{valid, p, r1-r3, s1-s3}` — pure formula; returns `valid=false` on non-positive prices or `H<L`.
  - [x] `DetectStructure()` calls private helper `ComputeDailyPivots()` (walks `m_xs` backwards using `localtime` to identify previous-day OHLC and pins `m_pivotsTodayStart`/`m_pivotsTodayEnd`). Skipped when `!IsIntraday(m_timeframe)`.
  - [x] `DrawAutoPivots()` invoked from `DrawOverlays()` after AutoFib (gated on `pivotPoints && IsIntraday`); renders 7 dashed h-lines spanning `[todayFirstIdx, todayLastIdx]` only, with right-aligned label tags inside today's range. S3/S2/S1 in greens, P in light grey, R1/R2/R3 in reds.
  - [x] Toolbar checkbox "Pivots" in the Auto: row, wrapped in `BeginDisabled()`/`EndDisabled()` on D1/W1/MN with an `AllowWhenDisabled` tooltip ("Intraday only — pivots use the prior day's OHLC.").
  - [x] Tests: classic formula on H=110/L=90/C=100 returns expected levels; S3<S2<S1<P<R1<R2<R3 invariant; invalid input → `valid=false`.
- [x] **Breakout markers**:
  - [x] `ChartAnalysis.h`: `FindBreakouts(resistances, supports, highs, lows, closes, atr, lookback, minTouches) -> vector<BreakoutMark{idx, y, up}>`. Causality guard via `cluster.firstIdx`; minTouches filter rejects weak levels.
  - [x] `DetectStructure()` populates `m_breakouts` after clustering, using the **unfiltered** cluster output (not `KeepTopN`-filtered) so already-broken levels still produce historical marks. lookback=50, minTouches reuses `m_auto.minTouches`.
  - [x] `DrawBreakoutMarks()` invoked from `DrawCandleChart()` after candlesticks/overlays; emits two `ImPlot::PlotScatter` calls — `Breakout Up` with `ImPlotMarker_Up` (green) at `high+0.5·ATR`, `Breakout Dn` with `ImPlotMarker_Down` (red) at `low-0.5·ATR`.
  - [x] Toolbar checkbox "Breakouts" in the Auto: row.
  - [x] Tests: synthetic up-breakout produces one mark on the right bar; minTouches filters single-touch level; down-breakout below support; causality guard skips levels established after the bar; empty/degenerate inputs.

**Notes / changes from the plan:**
- The toolbar labels are abbreviated (`Donch`, `Kelt`, `AutoFib`, `Pivots`, `Breakouts`) to match the existing `Sup`/`Res`/`AutoTrend`/`Zones` row style.
- `DonchianBands` returns a `DonchianResult` struct (not `{hi, lo}` tuple) to avoid name collision with the function and to match the file's `SwingResult`/`TrendFit`/`AutoFibSpan`/`DailyPivot` style.
- The Auto... popup gains a `Donchian length` `InputInt` (2–200, default 20) so users can tune the rolling window without recompiling.
- A `ComputeDailyPivots()` private helper was added on the ChartWindow side so the pure formula in `ChartAnalysis.h` stays free of timestamp/timezone logic. `localtime` is used to match how intraday timestamps are interpreted elsewhere in the file (`std::localtime` in `XTickFormatter` / `DrawHoverTooltip`).

---

## 6. Acceptance / verification

For each task, before marking complete:

1. `cmake --build build -j$(nproc)` — clean build (no new warnings).
2. `ctest --test-dir build --output-on-failure` — all tests pass (including the
   new `test_chart_analysis.cpp` cases under `tests-core`).
3. `DISPLAY=:1 ./build/ibkr-trading-app` — launch, connect to paper, load AAPL
   on 1m and D1 (covers intraday + daily code paths). Toggle every new
   checkbox; verify:
   - Lines appear / disappear cleanly.
   - No flicker on symbol change.
   - No crash on empty series (e.g. before historical data arrives).
   - Auto-Fib regenerates on new data without piling up.
   - Pivots only render on intraday.
4. Update `.claude/rules/task-history.md` (Phase 11 — Auto Technical Analysis,
   tasks #52, #53, #54 or similar) and root `README.md` if user-facing
   features change.

---

## 7. Risks / sharp edges

- **Empty / sparse data**: every helper must guard against `closes.size() < L`
  and return an empty / `valid=false` result. `DetectStructure()` early-out if
  `m_closes.size() < 30`.
- **Daily pivots vs missing prior day**: if the loaded series doesn't cover the
  previous trading day (e.g. user just loaded 1 day of intraday), `m_pivots.valid
  = false`. Render nothing rather than a NaN line.
- **Plot identifier collisions**: ImPlot uses string IDs for legend; pick names
  that don't collide with the existing "BB Mid", "SMA20", etc. Use
  "Donch Hi"/"Donch Lo", "Kelt Hi"/"Kelt Lo", "Breakout Up"/"Breakout Dn".
- **Z-order**: user-drawn lines should stay on top of auto-detection. Render
  auto first, user drawings second inside `DrawOverlays`.
- **Performance**: O(bars) detection + clustering; expected < 1 ms for 5,000
  bars. Cap `scanCap = 1000` is a safety net for pathological cases.

---

## 8. Out of scope (explicitly deferred)

- Volume profile (horizontal histogram on the right edge).
- Chart-pattern recognition (double tops/bottoms, head-and-shoulders).
- Wave counts / Elliott / Gann.
- Persistence of toggles across restarts.
- Per-symbol calibration of `swingK` / `trendLookback`.

---

## 9. Update log

| Date | Change | By |
|---|---|---|
| 2026-04-25 | Plan written | claude / jose |
| 2026-04-26 | Task C complete (Donchian, Keltner, AutoFib, Pivots, Breakouts) | claude / jose |
