# Architecture Rules

## Directory Structure

```
├── src/
│   ├── core/
│   │   ├── services/         # IB Gateway integration (IBKRClient.h/.cpp, IBKRUtils.h, ChartAnalysis.h, TradingStyle.h)
│   │   └── models/           # Data models: MarketData.h, NewsData.h, ScannerData.h,
│   │                         #   PortfolioData.h, OrderData.h, WindowGroup.h
│   ├── ui/
│   │   ├── UiScale.h         # em() font-scale helper + FlexRow CSS-flex-wrap struct
│   │   └── windows/          # One .h/.cpp pair per window
│   ├── bid_stubs/            # bid_stubs.c — Intel BID64 double bit-cast
│   └── main.cpp              # Vulkan/GLFW init, login state machine, top-level UI dispatch
├── tests/                    # Catch2 test suite (see testing.md)
│   ├── CMakeLists.txt
│   ├── test_market_data.cpp  # Timeframe helpers, IsUSDST, BarSession
│   ├── test_models.cpp       # Enum string helpers, struct defaults
│   ├── test_ibkr_utils.cpp   # ParseStatus, ParseIBTime
│   ├── test_chart_analysis.cpp # FindSwings, ATR, ClusterLevels, KeepTopN
│   └── test_ibkr_queue.cpp   # IBKRClient message dispatch (component tests)
├── twsapi_macunix.1037.02/   # IB TWS API sources (in-tree)
├── CMakeLists.txt
└── build/                    # Generated, not committed
```

## Design Principles

1. **Service Layer**: IB Gateway communication lives in `core/services/`. UI never calls IB API directly.
2. **Event-Driven**: `IBKRClient` bridges EWrapper callbacks → UI thread via `std::variant` message queue.
3. **Dependency Injection**: Services injected into window constructors for testability.
4. **One window = one file**: Each UI window gets its own `.h`/`.cpp` in `src/ui/windows/`.
5. **Models are POD-first**: Data models in `core/models/` are plain structs — no business logic.
6. **Utility extraction**: Pure logic extracted from class privates into standalone headers (`IBKRUtils.h`) so it can be unit-tested without IB API linkage.

## Main Entry Point Pattern

`main.cpp` uses the `ImGui_ImplVulkanH_Window` helper pattern:
- `SetupVulkan()` — instance, physical device, logical device, descriptor pool
- `SetupVulkanWindow()` — swapchain, render pass, framebuffers (via ImGui helper)
- `FrameRender()` / `FramePresent()` — render loop
- `RenderMainUI()` — dockspace + all windows dispatched from here

## Multi-Instance Windows

Chart, Order Book (Trading), Scanner, and News support up to **10 simultaneous instances** each.
Singleton windows (Portfolio, Orders) have one instance.

`kMaxMultiWin = 10` in `main.cpp`.

Entry structs in `main.cpp` hold per-instance state:
```cpp
struct ChartEntry   { ui::ChartWindow* win; int histId, extId, mktId; core::BarSeries pendingBars, pendingExtBars; };
struct TradingEntry { ui::TradingWindow* win; int depthId, mktId; double nbboBid, nbboAsk, ...; };
struct ScannerEntry { ui::ScannerWindow* win; int scanBase, activeScanId, mktBase; ... };
struct NewsEntry    { ui::NewsWindow* win; int nextArtReqId, artEnd;
                      std::unordered_map<int,int> artReqToItemId;
                      std::unordered_map<int,bool> newsConIdFired; };
```

Vectors: `g_chartEntries`, `g_tradingEntries`, `g_scannerEntries`, `g_newsEntries` (max 10 each).

Spawn helpers: `SpawnChartWindow(idx)`, `SpawnTradingWindow(idx)`, `SpawnScannerWindow(idx)`, `SpawnNewsWindow(idx)` — create the window, wire all callbacks with `idx` capture, push to the vector.

**ReqId layout (no overlaps, 10 instances each):**
- Chart hist: 1,3,5,7,9,11,13,15,17,19 · ext: 2,4,6,8,10,12,14,16,18,20 · mkt: 100-109
- Trading mkt: 110-119 · depth: 120-129
- Scanner scan: 1000,1100,...,1900 (+99 each) · mkt: 800,812,...,908 (+12 each)
- News stock conId: 2000-2009 · port conId: 2010-2199 · hist stock: 2210-2219 · hist port: 2220-2399
- News articles: 2500-3499 (100 per instance) · hist market: 3500-3599 · market conId: 3600-3699
- Account: 900
- Watchlist contract details: 6900–6909 · market data: 7000–7999 (watchlistIdx×100 + symbolSlot)
- Symbol search: 8000 · Execution filter: 8001
- WSH meta: 8010 · WSH events per chart instance: 8020–8029
- Smart components per TradingWindow: 8050–8059
- Display group query: 8060 · group subscriptions G1–G4: 8061–8064
- WSH Calendar window (aggregate, per-position conId): 8070–8199
- P&L account-wide: 9000 · P&L single per-position: 9001–9999

## UiScale — Responsive Toolbar Helpers

`src/ui/UiScale.h` provides two tools for font-scale-aware responsive layout:

```cpp
// Convert design-time px (authored at 13 px base font) to scaled value
inline float em(float px) { return px * ImGui::GetFontSize() / 13.0f; }
```

Use `em()` everywhere a pixel width or height is hardcoded for a widget:
```cpp
ImGui::SetNextItemWidth(em(80));   // scales with font size
ImGui::Button("OK", ImVec2(em(62), em(22)));
```

```cpp
struct FlexRow { ... };
```

`FlexRow` is a CSS `flex-wrap: wrap` equivalent for ImGui toolbars. Call `row.item(width)` **before** each widget — it calls `SameLine()` only if the item fits on the current line, otherwise wraps to the next line. Static helpers `buttonW`, `checkboxW`, `textW` estimate item widths from `CalcTextSize`.

All window toolbars use `FlexRow` so items wrap rather than overflow when the window is narrow.

## Settings

`main.cpp` globals for font size settings:
```cpp
enum class FontSize { Small = 0, Medium = 1, Large = 2 };
static FontSize       g_fontSize     = FontSize::Medium;
static bool           g_settingsOpen = false;
static constexpr float kFontScales[] = { 0.85f, 1.0f, 1.5f };
static ImGuiStyle     g_baseStyle;   // saved once after style setup
```

`RenderSettingsWindow()` renders a floating panel with Small/Medium/Large radio buttons.
On change: sets `io.FontGlobalScale`, restores `g_baseStyle`, then calls `ScaleAllSizes(scale)`.
`g_baseStyle` must be saved *before* any `ScaleAllSizes` call to prevent compounding.

## Windows Menu

`Windows → IBKR → <instance list>` submenu hierarchy.
`ImGuiItemFlags_AutoClosePopups = false` is pushed inside the IBKR submenu so clicking window toggles or "+ New" buttons does not close the menu.
`ImGuiWindowFlags_NoFocusOnAppearing` on all windows prevents newly shown windows from stealing focus and collapsing the menu.

Window title format: `"<Type> <Symbol> <Group>###<type><id>"` (e.g. `"Chart AAPL G1###chart0"`).
The `###` triple-hash gives ImGui a stable identity while the display label changes every frame.

## IBKRUtils

`src/core/services/IBKRUtils.h` — standalone header with no IB API dependency:
- `ParseStatus(const std::string&) → core::OrderStatus` — maps IB order-status strings to enum; `"PendingCancel"` → `OrderStatus::PendingCancel`
- `ParseIBTime(const std::string&) → std::time_t` — parses IB date/timestamp formats (YYYYMMDD, Unix string, formatted datetime)

## ChartAnalysis

`src/core/services/ChartAnalysis.h` — standalone header (no IB API / ImGui / ImPlot deps) for the auto technical-analysis layer **and** the classic single-series indicator helpers. Both `ChartWindow` and `ReplayWindow` call into it, and tests-core verifies the math without a UI/IB dependency. Inline free functions in `core::services`:
- `SMA(close, period) → vector<double>` — simple moving average; entries before `period-1` are 0.
- `EMA(close, period) → vector<double>` — exponential moving average seeded with the SMA of the first `period` values; entries before `period-1` are 0.
- `ComputeBollinger(close, period, sigma) → BollingerBands{mid, upper, lower}` — middle = SMA(period), upper/lower = mid ± sigma·stdev (population stdev with N=period). All three vectors are sized to `close.size()` with leading zeros for indices `< period-1`.
- `RSI(close, period) → vector<double>` — Wilder-smoothed RSI; `RSI[period]` is the simple-average seed, subsequent values use Wilder smoothing. Entries before `period` are 0.
- `FindSwings(highs, lows, k, scanCap=0) → SwingResult` — pivot detection with left/right window `k`. Strict `>` on the left and `>=` on the right (mirror for lows) so flat tops/bottoms register as a single swing. `scanCap` limits the scan to the last N bars.
- `ATR(highs, lows, closes, period) → vector<double>` — Wilder's true-range smoothing.
- `ClusterLevels(swings, tol) → vector<Level>` — sweeps swings sorted by price, merging consecutive ones within `tol` into a single `Level{price, touches, firstIdx, lastIdx, minPrice, maxPrice}`. Cluster price = mean of constituents; `minPrice`/`maxPrice` track the lowest/highest constituent so ChartWindow can render thickness-aware supply/demand zones.
- `KeepTopN(clusters, currentPrice, side, minTouches, maxLevels) → vector<Level>` — filters by side (`Above`/`Below`), min touches, then ranks by `(touches desc, lastIdx desc)` and caps at `maxLevels` (`maxLevels=0` = no cap).
- `LinearRegression(closes, lookback) → TrendFit{valid, slope, intercept, sigma, firstIdx, lastIdx}` — closed-form OLS using bar idx as x over the trailing `lookback` closes. `sigma` is population residual stdev (used by the optional ±2σ channel). Returns `valid=false` on too-small input or zero-variance x.
- `DonchianBands(highs, lows, N) → DonchianResult{hi, lo}` — rolling N-bar max/min envelope. Vectors are sized to `highs.size()` with `0.0` for indices `< N-1` (matching the convention used by `CalcSMA`/`CalcEMA`/`CalcBollingerBands`).
- `LargestSwingSpan(swingHighs, swingLows, window) → AutoFibSpan{valid, hiIdx, hiPrice, loIdx, loPrice}` — picks the (high, low) pair with the largest absolute price difference among the last `window` swings of each list. Used to anchor the auto-Fibonacci overlay. `valid=false` on empty input or zero-span.
- `ClassicPivots(prevH, prevL, prevC) → DailyPivot{valid, p, r1-r3, s1-s3}` — pure formula for classic pivot points (`P=(H+L+C)/3`, `R1=2P-L`, `R2=P+(H-L)`, `R3=H+2(P-L)`, mirror for S). Caller is responsible for picking the previous trading day's OHLC (ChartWindow does this in `ComputeDailyPivots()` by walking `m_xs` backwards using `localtime`). Returns `valid=false` on non-positive prices or `H<L`.
- `FindBreakouts(resistances, supports, highs, lows, closes, atr, lookback, minTouches) → vector<BreakoutMark{idx, y, up}>` — walks the last `lookback` bars and emits a mark whenever `close[i-1] <= R.price < close[i]` (up) or the support mirror (down). Uses `cluster.firstIdx` as a causality guard so a level cannot mark bars before it was established. `minTouches` filters trivially-weak clusters. Mark `y` = `highs[i] + 0.5·atr[i]` (up) or `lows[i] - 0.5·atr[i]` (down). Caller passes the raw cluster output (not `KeepTopN`-filtered) so already-broken levels still produce historical marks.
- `SessionVwap(highs, lows, closes, volumes, sessionStarts) → VwapResult{vwap, sd1Up, sd1Dn, sd2Up, sd2Dn}` — session-anchored volume-weighted average price plus ±1σ / ±2σ volume-weighted bands. Single-pass closed-form `Var[X] = E[X²] − E[X]²` formulation: tracks running `cumTPV`/`cumVol`/`cumTPV2`, resets at each index in `sessionStarts` (and at `i==0`); zero-volume bars carry the previous values forward; first-bar zero-volume falls back to `closes[0]`. `varVw < 0` clamp guards ULP-level numerical drift. Caller computes `sessionStarts` (ChartWindow uses `localtime` ET-day boundary detection — empty list = single cumulative run). Per-bar typical price = `(H+L+C)/3`.

`ChartWindow.h` re-exports `Level` as `ChartWindow::AutoLevel` and `TrendFit` as `ChartWindow::AutoTrend`. Auto-detection results live in `m_autoSupports`/`m_autoResistances`/`m_atr14`/`m_autoTrend` and are populated by `DetectStructure()`, which runs at the tail of `ComputeIndicators()` so every existing recompute trigger refreshes auto-analysis. Toolbar toggles in `DrawAnalysisToolbar()` call `DetectStructure()` directly. Rendering: `DrawAutoSupportResistance()` is invoked from `DrawOverlays()` between the current-price line and the user drawings — red dashed h-lines for resistance, green for support, alpha proportional to touch count, left-edge price tag formatted ` R 187.42 (3×) `.

**Auto trend line** (`AutoAnalysisSettings::trend`, default ON; `trendLookback` default 50): `m_autoTrend` is populated whenever `trend` is on (using `min(trendLookback, n)` so short series still fit). `DrawAutoTrend()` is invoked from `DrawOverlays()` after S/R rendering: solid segment from `(firstIdx, y(firstIdx))` to `(lastIdx, y(lastIdx))` plus a faded (60% alpha) `L/4`-bar forward projection. Colour reflects slope direction (green if `slope > +ε`, red if `< −ε`, grey otherwise) with `ε = 0.05·sigma/L`. When `m_auto.trendChannel` is on, ±2σ parallel lines render on both segments at 50% alpha (fainter on the projection). A right-edge ` trend +0.123/bar ` label tags the projection endpoint.

**Supply/demand zones + imminent-breakout signal** (`AutoAnalysisSettings::zones`, default OFF): `ComputeBreakoutSignal()` runs at the tail of `DetectStructure()` when `zones` is on. It walks supply zones first then demand zones, finds the one whose `[minPrice − 0.5·ATR, maxPrice + 0.5·ATR]` interval contains the latest close, and only emits a `LongSetup`/`ShortSetup` (`m_breakoutSignal`) when (a) `bbWidth[n-1] < 0.7 × avg(bbWidth[n-50..n-1])` (BB compression, ≥20 valid samples required) and (b) `closes[n-1] − mean(closes[n-6..n-2])` exceeds ±0.1·ATR. `DrawAutoZones()` is invoked from `DrawOverlays()` immediately before `DrawAutoSupportResistance()` so the dashed S/R lines render on top of the rectangles: translucent red fill + thin red border for supply, translucent green for demand. When a signal is active, the right edge of the active zone gets a coloured triangle (▲ green / ▼ red) at the zone midpoint with a ` LONG SETUP ` / ` SHORT SETUP ` label tag to its left. `DetectStructure()` now populates the S/R clusters whenever `zones` is on, even if the individual `supports`/`resistances` toggles are off, since zones reuse the same cluster output.

**Donchian + Keltner channels** (`AutoAnalysisSettings::donchian`/`keltner`, default OFF): Donchian uses `core::services::DonchianBands(m_highs, m_lows, donchianLen)` (default `donchianLen=20`); Keltner reuses `m_ema` (always computed in `ComputeIndicators`, regardless of the EMA20 indicator toggle) and `m_atr14` to build `m_keltUpper = m_ema + 2·m_atr14` / `m_keltLower = m_ema - 2·m_atr14`. Both render via `ImPlot::PlotLine` from `DrawCandleChart()` (faded yellow for `Donch Hi`/`Donch Lo`, faded cyan for `Kelt Hi`/`Kelt Lo`) so they participate in the legend alongside SMA/BB/EMA.

**Auto-Fibonacci** (`AutoAnalysisSettings::autoFib`, default OFF): `m_autoFib = LargestSwingSpan(sw.highs, sw.lows, /*window=*/30)` picks the largest-span pair among the last 30 swings of each list. `DrawAutoFib()` (called from `DrawOverlays()` after S/R/trend) renders six dashed h-lines at `kFibLevels[]` between `loPrice` and `hiPrice`, in faded `kFibColors` with **left-edge** ` F 38.2% 187.42 ` labels (manual fibs use right-edge labels — staying on opposite edges avoids collisions). Anchor markers (small filled circles) at the swing-high (orange) and swing-low (blue) make the source pair visible.

**Daily pivot points** (`AutoAnalysisSettings::pivotPoints`, default OFF; intraday only): `ComputeDailyPivots()` walks `m_xs` backwards (using `localtime` to match how intraday timestamps are interpreted elsewhere) to identify the previous trading day's first/last bar and the current day's first bar. Computes `m_pivots = ClassicPivots(prevH, prevL, prevC)` and pins `m_pivotsTodayStart`/`m_pivotsTodayEnd`. `DrawAutoPivots()` (called from `DrawOverlays()` when `pivotPoints && IsIntraday(m_timeframe)`) renders 7 dashed h-lines (S3/S2/S1 in greens, P in light grey, R1/R2/R3 in reds) spanning `[todayFirstIdx, todayLastIdx]` only, with right-aligned label tags inside today's range. Toolbar checkbox is `BeginDisabled()`-wrapped on D1/W1/MN with an `AllowWhenDisabled` tooltip explaining "Intraday only".

**Breakout markers** (`AutoAnalysisSettings::breakouts`, default OFF): `m_breakouts = FindBreakouts(highClusters, lowClusters, m_highs, m_lows, m_closes, m_atr14, /*lookback=*/50, m_auto.minTouches)` is computed in `DetectStructure()` after clustering. The unfiltered cluster output is passed (not `KeepTopN`-filtered) so already-broken levels still produce historical marks. `DrawBreakoutMarks()` (called from `DrawCandleChart()` after candlesticks/overlays) emits two `ImPlot::PlotScatter` calls — `Breakout Up` with `ImPlotMarker_Up` (green) and `Breakout Dn` with `ImPlotMarker_Down` (red).

## Setup Suggestions (Phase 12 — Task A/B helpers)

`src/core/services/ChartAnalysis.h` gains five pure helpers built on top of the auto-analysis primitives, plus two new structs (`SetupPlan` / `PositionStop`). All are pure logic — no IB / ImGui / ImPlot deps — and have full Catch2 coverage under the `[setup]` tag in `tests/test_chart_analysis.cpp` (17 cases / 80 assertions).

- `RoundToTick(price, tick=0.01) → double` — snap a price onto the contract's `minTick` grid. IB rejects prices finer than `minTick` with error 110 ("price does not conform to the minimum price variation for this contract"); the math in `AvoidRoundNumber` and the `stopLmt = stop ± offset` arithmetic both leak ULP-level drift on inputs like `0.07` / `0.10` (those literals are not exactly representable in IEEE-754), so every output of `SuggestSetup` and `SuggestStopForPosition` runs through `RoundToTick` before reaching the caller. `tick<=0` is treated as a no-op so callers can opt out. Default is $0.01 (regular US stocks above $1); $0.05 covers most options, $0.0001 covers sub-dollar stocks.
- `AvoidRoundNumber(price, pad, pushDown) → double` — snap a price *away* from the nearest .00 / .25 / .50 / .75 / 1.00 mark by at least `pad` dollars. `pushDown=true` → the price is a stop below entry; `false` → stop above. Already-safe prices (distance ≥ pad) returned unchanged. Edge: nudging past 1.00 crosses into the next integer — intentional, the safe direction is further out.
- `SuggestSetup(side, zoneTop, zoneBot, anchor, opposingLevel, atr, last, atrPad, roundPad, stopOffset, rrMin, equity, riskPct) → SetupPlan{valid, side, entry, stop, stopLmt, target, rr, shares, refLevelIdx}` — long path uses `last` as entry when past the supply zone, otherwise the zone midpoint; `rawStop = anchor − atrPad·atr`, snapped via `AvoidRoundNumber(roundPad, pushDown=true)`; target = nearest opposing level; `rr = (target-entry)/(entry-stop)` must clear `rrMin` else `valid=false`. Short mirrors with `anchor=Level.maxPrice` (passed in by ChartWindow), `pushDown=false`, target = nearest support below entry. The `anchor` parameter (originally spec'd as `anchorMin`) was renamed so a single name covers both sides — caller picks `Level.minPrice` for long, `Level.maxPrice` for short.
- `SuggestStopForPosition(isLong, entry, levels, atr, atrPad, roundPad, stopOffset) → PositionStop{valid, stop, stopLmt, pctRisk}` — filters levels by side + `touches >= 2`, picks the one closest to entry (max price for long-supports, min price for short-resistances), anchors at its `minPrice`/`maxPrice`, applies the same padding + round-snap pipeline. Caller passes the already-filtered `KeepTopN` result (or a manually-curated subset). Returns `valid=false` when no qualifying level exists, single-touch only, or empty input.
- `PositionSizeShares(equity, riskPct, entry, stop) → int` — `floor(riskPct/100 × equity / |entry-stop|)`; returns 0 on degenerate input (zero/negative equity, zero risk%, entry==stop). No max-position cap — the user already sees R:R + share count and decides.

`SetupPlan` is the ChartWindow's reference plan struct (entry / stop / stop-limit / target / R:R / suggested share size / index of the opposing level used as target anchor). `PositionStop` is the slimmer struct used by the unguarded-position guard — just the suggested stop trigger / stop-limit / `pctRisk = |entry-stop|/entry × 100`.

**ChartWindow setup overlay** (`SetupSettings { overlay=false, rrMin=2.0, atrPad=0.5, roundPad=0.07, stopOffset=0.10, riskPct=1.0, useStopLmt=true }`): `m_setup` is populated by `ComputeSetupPlan()` at the tail of `DetectStructure()` whenever `m_setupSettings.overlay && m_breakoutSignal != None`. `ComputeBreakoutSignal()` records `m_breakoutLevelIdx` (the index of the active zone within `m_autoSupports`/`m_autoResistances`) so `ComputeSetupPlan` can fetch the underlying `Level` and use its `minPrice`/`maxPrice` (not the buffered zone bounds — the structural anchor is the deepest wick, not the cluster mean). `DrawSetupOverlay()` is invoked from `DrawOverlays()` between `DrawAutoSupportResistance()` and `DrawAutoTrend()` — three dashed h-lines (entry cyan ` ENTRY 187.42 x N sh `, stop red ` STOP 184.97 (-1.31%) `, target green ` TGT 192.10 (R:R 2.1) `, all right-edge labels so they don't collide with left-edge S/R tags). A faint stop-limit companion line renders when `m_setupSettings.useStopLmt`. `DrawAnalysisToolbar()` `Setup` checkbox in the Auto: row toggles `m_setupSettings.overlay`; `DrawSetupSettingsPopup()` (invoked from inside `DrawAutoSettingsPopup()`) exposes R:R minimum (1.0–5.0), ATR padding (0.1–2.0), round-number pad ($) (0.0–0.50), stop-limit offset ($) (0.0–1.0), risk per trade (%) (0.1–5.0), and a `Use Stop-Limit (off = plain Stop)` checkbox; any change re-runs `DetectStructure()`.

**`[Use suggestion]` button** in `DrawTradePanel()`: when `m_setupSettings.overlay && m_setup.valid`, a blue button renders right after the SELL button. Click → forces `m_orderTypeIdx = 1` (Limit), adopts `m_setup.shares` as the qty when known, builds a Limit order via the existing `buildOrder()` lambda, sets `o.limitPrice = m_setup.entry`, stages it in `m_pendingConfirmOrder`, and opens `DrawConfirmPopup()` regardless of the *Transmit Instantly* toggle. Tooltip explicitly says "Reference plan, not advice."

**Equity accessor**: `main.cpp` exposes free function `double GetSelectedAccountEquity()` returning `g_PortfolioWindow ? g_PortfolioWindow->netLiquidation() : 0.0`; `ChartWindow.cpp` forward-declares it (`extern double GetSelectedAccountEquity();`) so `ComputeSetupPlan` can size the suggested share count without including main.cpp internals. Returns 0 before `accountSummary()` fires on fresh connect — `PositionSizeShares` returns 0 and the entry label simply omits the `× N sh` suffix.

## Unguarded-Position Guard (Phase 12 — Task C)

A position is "unguarded" if non-zero quantity exists with **no** active protective `Stop` / `StopLimit` / `Trail` / `TrailLimit` order on the same symbol on the opposite side. When detected, both the matching `ChartWindow` and the matching `TradingWindow` show a yellow warning strip with `Place stop` and `Dismiss` buttons. The check is global (one per app instance), the rendering is per-window.

`main.cpp` adds:
```cpp
struct UnguardedPosition { std::string symbol; long conId; double qty, avgCost; };
static std::vector<UnguardedPosition> g_unguarded;
static void RecomputeUnguardedPositions();   // walks g_positions × g_liveOrders
static void PushUnguardedHintsToWindows();   // per-frame fan-out to chart + trading windows
```

`RecomputeUnguardedPositions()` is cheap (O(positions × orders), both small) and runs after the existing logic in four IB callback hooks — `onPositionData`, `onPortfolioUpdate`, `onOpenOrder`, `onOrderStatusChanged`. Re-entrant-safe: no IB calls, no UI calls. Match criteria for "guarded": opposite side, status in `{Pending, Working, PartialFill, PendingCancel}`, type in `{Stop, StopLimit, Trail, TrailLimit}`, quantity > 0. (v1 does **not** require `sum(stop.quantity) >= |pos.quantity|` — partial-coverage is treated as guarded; v2 deferred per plan §7.) `onPositionData` also mirrors `pos` into `g_positions` so the guard sees the same single source of truth `onPortfolioUpdate` populates (quantity-zero erases the entry so the warning self-clears on flat).

`PushUnguardedHintsToWindows()` is called once per frame from `RenderTradingUI()` right before window `Render()` calls, after `UpdateAllChartPendingOrders()`. For each `g_chartEntries[i]` it builds the hint from that chart's own `getAutoLevels()` + `core::services::SuggestStopForPosition()` (using fixed defaults `atrPad=0.5`, `roundPad=0.07`, `stopOffset=0.10` — the per-chart `SetupSettings` knobs are reserved for the overlay path; the warning uses safe constants so all charts agree). For each `g_tradingEntries[i]` it borrows S/R from the *first* chart on the same symbol via `symToChart[symbol]`; falls back to `active=false` (no warning) when no chart instance is open for the symbol (v1 behaviour per plan §3f). Cleared hints (`active=false`) are sent to all non-matching windows so stale strips clear automatically.

`ChartWindow::getAutoLevels() → AutoLevelSnapshot{supports, resistances, atrLast}` is the public accessor used by `PushUnguardedHintsToWindows` to read each chart's auto-detected S/R without coupling main.cpp to private members.

Both `ChartWindow.h` and `TradingWindow.h` declare an identical `UnguardedHint { active, symbol, qty, avgCost, stopTrig, stopLmt, pctRisk }` struct (independent declarations to avoid header coupling between the two windows; `PushUnguardedHintsToWindows` does a field-by-field copy when forwarding chart-side → trading-side). Each window holds:
- `UnguardedHint m_unguarded;`
- `std::unordered_set<std::string> m_dismissedUnguarded;`
- `double m_lastWarnedQty = 0.0;` — change-detection so dismissal clears when the user trims/adds.
- `void SetUnguardedSuggestion(const UnguardedHint& h);` — entry-point called per frame.
- `void DrawUnguardedStrip();` — renders the yellow strip (or nothing).

`DrawUnguardedStrip()` self-suppresses on `!active`, symbol mismatch, dismissed symbol, or `stopTrig <= 0` (so partially-populated hints — e.g. a chart that hasn't run auto-analysis yet — render nothing rather than a placeholder). Layout: `BeginChild` band with `ImGuiChildFlags_Borders` (note: the enum is *plural* in ImGui 1.92.6-docking — singular `ImGuiChildFlags_Border` does not exist), yellow background (`0.30, 0.24, 0.05, 0.90`), gold border, FlexRow toolbar with `WARNING <SYM> <side> N sh @ $X - no protective stop. Suggested stop $Y (-Z%).` text + `Place stop` button (gold) + `Dismiss` button. Tooltips on both buttons.

`ChartWindow::DrawUnguardedStrip()` "Place stop" stages a `core::Order` (`StopLimit` if `m_setupSettings.useStopLmt`, else plain `Stop`) on the opposite side of the position, full quantity, DAY/RTH-only (`outsideRth=false`, `tif=DAY`, `exchange="SMART"`), then sets `m_pendingConfirmOrder = o; m_showConfirmPopup = true;` — routed through `DrawConfirmPopup()`, never bypasses confirmation.

`TradingWindow::DrawUnguardedStrip()` "Place stop" pre-fills the order-entry buffers (`m_sideIdx`, `m_typeIdx=3 = StopLimit`, `m_tifIdx=0 = DAY`, `m_outsideRth=false`, `m_qtyBuf`, `m_stpBuf`, `m_lmtBuf`) and triggers `m_showConfirm = true` so the existing `DrawConfirmationPopup()` reads the right values and the order-entry panel stays in sync.

Render-order sites:
- `ChartWindow::Render()` calls `DrawUnguardedStrip()` immediately after `if (!m_viewInitialized) InitViewRange();` — above `DrawInfoBar()` / `DrawPositionStrip()` / `DrawCandleChart()`.
- `TradingWindow::Render()` calls it inside the `##entry_panel` `BeginChild`, above `DrawOrderEntry()`.

## Trading Styles (Phase 13)

`src/core/services/TradingStyle.h` — standalone POD header (no IB / ImGui / file I/O deps) defining five chart presets (four canonical + a user-driven Free escape hatch):

```cpp
enum class TradingStyle : int { Scalping=0, DayTrading=1, Swing=2, Investment=3, Free=4 };
struct StylePreset { Timeframe timeframe; const char* historyDuration; <auto fields>; bool indVwap, indVwapBands; <setup fields>; };
StylePreset GetPreset(TradingStyle s);
template<typename Ind, typename Auto, typename Setup>
void ApplyPreset(const StylePreset&, Ind&, Auto&, Setup&, Timeframe&);
```

The preset bundle hard-binds `{timeframe + history horizon + auto-analysis params + indicator/setup defaults}` so the auto S/R, breakout signal, setup overlay, unguarded-stop suggestion, and VWAP all stay coherent within a session. Per the plan in `.claude/plans/trading-styles.md`:

| Style | TF | History | Highlights |
|---|---|---|---|
| Scalping | M1 | 2 D | S/R + zones + pivots + breakouts; VWAP on (no bands); rrMin=1.5, riskPct=0.5% |
| Day Trading | M15 | 20 D | + trend; VWAP + ±σ bands; rrMin=1.75, riskPct=0.75% |
| Swing | D1 | 1 Y | + autoFib; VWAP off; rrMin=2.0, riskPct=1.0% |
| Investment | W1 | 5 Y | trend + autoFib; no zones / no breakouts; rrMin=3.0, atrPad=1.0, riskPct=1.5% |
| Free | (any) | TF default | Construction-default baseline. TF combo is unlocked; all settings remain user-editable across TF changes. |

`ApplyPreset` is **templated on the settings struct types** so the helper can be instantiated by tests with hand-rolled stubs that have the same field names — no ChartWindow.h dependency. The `[style]` Catch2 tag in `tests/test_chart_analysis.cpp` covers field-by-field assertions on each preset (including Free's construction-default baseline), full stub-struct stamping, a Scalping → Investment round-trip proving no carry-over, and a Investment → Free round-trip.

**ChartWindow integration**: `m_tradingStyle` (default `Swing`), `tradingStyle()` accessor, `setTradingStyle(s, silent=false)` mutator. The mutator applies the preset, wipes every derived buffer (`m_xs`/`m_idxs`/all OHLCV/all indicators/all auto-analysis state/`m_setup`/`m_unguarded`/`m_drawings`), sets `m_loading=true`, and fires `OnStyleChange(s, preset.historyDuration, m_useRTH)` unless `silent=true`.

**Free-mode special case**: `setTradingStyle(Free)` preserves the chart's current `m_timeframe` (saves it before `ApplyPreset` and restores it after — the whole point of Free is to let the user pick any TF), and the `OnStyleChange` duration is computed from the preserved TF via `TimeframeIBDuration(currentTF)` rather than from the static preset value. A separate `setTimeframeFree(tf, silent=false)` method lets the toolbar's editable TF combo (rendered only when `m_tradingStyle == Free`) update `m_timeframe` and refetch — it wipes data buffers + derived analysis state but **preserves user settings** (`m_ind` / `m_auto` / `m_setupSettings` / `m_drawings`) so a TF change within Free mode doesn't reset the user's customizations.

Toolbar Style combo lives next to the timeframe display: for non-Free styles the TF is shown as a read-only `[1D]`-style label; for Free the TF widget becomes an editable Combo populated from `kAllTimeframes[]`. `silent=true` is used by the chart-modes.cfg load path so connect doesn't enqueue redundant IB requests.

**Mode-switch queue + throttle** (main.cpp): switching styles re-issues `ReqHistoricalData` with the preset's `historyDuration`. To stay under IB's 60-req-per-10-min-per-contract pacing limit, switches go through `g_pendingStyleSwitches` (deque) drained at most once per `kStyleSwitchThrottleSec` (1.0 s) by `DrainStyleSwitchQueue()`. The drain function:
1. Cancels the chart's in-flight `extId` extend-history request (so older bars from the previous TF can't land into the new series).
2. Reads `symbol` and `useRTH` at drain time (not enqueue time), so a symbol change between click and drain doesn't strand the wrong contract.
3. Calls `ReqChartData(..., durationOverride=p.historyDuration)`.

`OnStyleChange` lambda (in `SpawnChartWindow`) drops any prior queue entry for the same `chartIdx` (latest-wins) before pushing the new entry, so spamming the combo settles on the last pick rather than walking through every intermediate mode. Throttle math: 1 req/sec × 10 charts = 10 s spacing on bulk mode-switch.

`ReqChartData(...)` was extended with an optional `const std::string& durationOverride = ""` parameter — empty falls back to `core::TimeframeIBDuration(tf)` (existing behaviour); non-empty passes through to `ReqHistoricalData`.

**Persistence**: `~/.config/ibkr-trading-app/chart-modes.cfg` (atomic `.tmp` + `rename`, mirroring `WatchlistsFilePath()`). Format:
```
INSTANCE:0
SYMBOL:AAPL
STYLE:2          (integer enum value; 0=Scalping..3=Investment, 4=Free)
TF:6             (optional; only written when STYLE==Free. Integer Timeframe enum value 0..8.)
```

`g_chartModesDirty` is set in the `OnStyleChange` lambda; `RenderTradingUI()` flushes once per second via `SaveChartModesFile()`; `DestroyTradingWindows()` flushes synchronously when dirty (covers both disconnect and app-exit). `FinishConnect(false)` post-`CreateTradingWindows()` and post-watchlist-load calls `LoadChartModesFromFile()`, then for each saved block: `setTradingStyle(style, silent=true)` → (Free only: `setTimeframeFree(savedTF, silent=true)` to apply the persisted TF override) → `SetSymbol(b.symbol)` → `ReqChartData(... duration)` with `duration = TimeframeIBDuration(tf)` for Free or `preset.historyDuration` otherwise, and `useRTH = !isIntraday(tf)`. Tracks restored chart indices in `restoredCharts[]` so the AAPL D1 fallback seed for chart 0 is gated on `!restoredCharts[0]`. `LoadChartModesFromFile()` clamps `STYLE:` to `[0, Free]` and `TF:` to `[M1, MN]` to reject corrupt data; older configs without `TF:` lines load fine since the parser falls through to default `timeframe=-1` (= use preset's TF).

**Drain-queue Free-mode duration recompute**: `DrainStyleSwitchQueue()` recomputes `duration = TimeframeIBDuration(ce.win->getTimeframe())` for Free-mode entries at drain time, bypassing the enqueued static value. Protects against the edge case where the user toggles to Free, the throttle elapses and drains the entry, then they change the TF — without this, the drain would fire `ReqChartData` with the wrong duration. For non-Free modes the enqueued static value is still used.

## VWAP + ±σ Bands

`ChartWindow::IndicatorSettings::vwap` (default true on intraday-flavoured presets, false on Swing/Investment) and `::vwapBands` (default false everywhere except Day Trading) toggle the VWAP centre line and its volume-weighted ±1σ / ±2σ bands. `m_vwap` (centre) and four band vectors `m_vwapSd1Up`/`m_vwapSd1Dn`/`m_vwapSd2Up`/`m_vwapSd2Dn` are populated by `ComputeIndicators()` calling `core::services::SessionVwap(m_highs, m_lows, m_closes, m_volumes, sessionStarts)`. `sessionStarts` is built from `m_xs` using `localtime` ET-day boundary detection on intraday TFs; for D1/W1/MN it stays empty (single cumulative run from index 0). `DrawCandleChart()` plots the centre line in solid gold (`1.0, 0.85, 0.0, 1.0`, width 1.5) and, when `m_ind.vwapBands`, renders four extra `ImPlot::PlotLine` entries: `VWAP+1σ`, `VWAP-1σ` at alpha 0.30; `VWAP+2σ`, `VWAP-2σ` at alpha 0.18 — same gold hue. Toolbar gains a compact `±σ` checkbox right after the existing `VWAP` checkbox, gated on `m_ind.vwap` (only renders when VWAP is on). The previous `ChartWindow::CalcVWAP` static is removed; tests-core now covers the math directly under the `[vwap]` tag (6 cases / 84 assertions).

## Symbol Search

`src/ui/SymbolSearch.h` — reusable autocomplete widget, no IB API dependency (takes a callback):

```cpp
struct SymbolSearchState {
    std::vector<SymbolResult> results;
    double  debounceEnd = 0.0;  int selected = -1;
    char    lastQuery[33]     = {};
    char    lastConfirmed[33] = {};  // last IB-validated symbol — reverted to on bad input
    char    searchedQuery[33] = {};  // query actually sent to IB (race guard)
    bool    popupOpen = false;  ImGuiID ownerID = 0;
    bool    searching = false;  bool searched = false;
};
bool DrawSymbolInput(const char* label, char* buf, int bufSize,
                     std::function<void(const std::string&)> reqMatchFn,
                     SymbolSearchState& state);
```

- Debounces 300 ms before firing `reqMatchFn`
- Opens tooltip-style popup with up to 10 results; arrow-key + Enter to select
- On empty IB results: reverts `buf` to `lastConfirmed` (prevents non-existent symbols)
- On focus-lost without confirm: reverts `buf` to `lastConfirmed`
- All confirm paths (click, Enter) update `lastConfirmed`
- Returns `true` when a confirmed symbol change occurs
- reqId 8000 (cancel-before-reissue pattern); each window has its own `SymbolSearchState`

Both were previously private statics on `IBKRClient`. Extracting them allows unit testing without linking ibapi-lib.

`IBKRClient::Push()` is **protected** (not private) so test subclasses can inject messages directly:
```cpp
class TestableIBKRClient : public IBKRClient {
public:
    void inject(IBMessage msg) { Push(std::move(msg)); }
};
```

## Order Types & PlaceOrder

`IBKRClient::PlaceOrder(const core::Order& order)` — takes a fully-populated `core::Order` struct.  
The old 9-parameter string-based overload is **gone**. All call sites (TradingWindow, ChartWindow, modify-order) build a `core::Order` and call this.

`core::OrderType` enum (13 values):
```
Market, Limit, Stop, StopLimit,   // basic
Trail, TrailLimit,                 // trailing stop
MOC, LOC, MTL,                     // market/limit close/to-limit
MIT, LIT,                          // if-touched
Midprice, Relative                 // smart / pegged-to-primary
```

`core::Order` key fields for advanced types:
- `auxPrice` — trigger for MIT/LIT; peg offset for REL; trailing $ amount for TRAIL*
- `trailingPercent` — trailing % (mutually exclusive with auxPrice trail path)
- `trailStopPrice` — initial stop cap for TRAIL / TRAIL LIMIT (0 = let IB compute)
- `lmtPriceOffset` — limit offset from trail stop price for TRAIL LIMIT
- `outsideRth` — allow pre/after-hours fills (was a separate callback param, now on struct)
- `account` — IB account code; stamped from `g_selectedAccount` at submit time in `main.cpp`

`core::OrderStatus` enum: `Pending, Working, PartialFill, Filled, Cancelled, Rejected, PendingCancel`
- `PendingCancel` maps to IB string `"PendingCancel"` and renders as `"CANCELLING"` in the UI
- `onError` handler skips marking `PendingCancel` orders as Rejected (errors 10148/10149 are informational)

`TradingWindow::OnOrderSubmit` callback: `std::function<void(const core::Order&)>`  
`ChartWindow::OnOrderSubmit` callback: `std::function<void(const core::Order&)>` — main.cpp lambda stamps account and calls `PlaceOrder`.

## Connection State Machine

`ConnectionState` enum in `main.cpp`:

| State | Meaning |
|---|---|
| `Disconnected` | Not connected — login screen shown |
| `Connecting` | Initial connect in progress |
| `SelectingAccount` | IB `managedAccounts()` fired, waiting for user to pick an account (multi-account live sessions only) |
| `Connected` | Live session — trading UI shown |
| `LostConnection` | Unexpected drop — trading UI stays alive, DISCONNECTED badge shown, auto-reconnect polling |
| `Error` | Initial connect failed — login screen with error message |

**`FinishConnect(bool isReconnect)`** — called once account is known (immediately on single-account, or after modal confirm on multi-account). Calls `ReqAccountUpdates`, `ReqPositions`, `ReqAccountSummary`, `ReqOpenOrders`, `ReqAllOpenOrders`, `ReqExecutions(8001)`. On first connect also creates trading windows and subscribes initial symbols. On reconnect re-subscribes all open chart/trading windows.

**Auto-reconnect** (`StartSilentReconnect()`):
- Polled every frame from main loop when `LostConnection && !g_IBClient`
- Timer: `g_reconnectNextAttempt` (5s interval via `kReconnectIntervalSec`)
- On success (`onConnectionChanged(true)` with `isReconnect=true`): skips `DestroyTradingWindows`/`CreateTradingWindows`, re-subscribes each chart/trading window using `getSymbol()`/`getTimeframe()` accessors
- On failure: schedules next retry in 5s, stays `LostConnection`

**DISCONNECTED badge**: orange background rect + yellow text, left of account selector in menu bar.

**Account selector** (menu bar, right of DISCONNECTED badge, left of `[LIVE]`/`[PAPER]`): always visible after connect. Single-account: shows account ID as plain text. Multi-account: shows a clickable combo that opens an inline popup to switch accounts (re-calls `FinishConnect`). Globals: `g_managedAccounts` (all accounts from `managedAccounts()` callback), `g_selectedAccount` (active account), `g_pendingReconnect` (deferred reconnect flag used when account modal is shown during reconnect).

## Window Groups & Symbol Sync

`src/core/models/WindowGroup.h` provides:
- `GroupState { int id; std::string symbol; }` — active symbol per group (4 slots)
- `WindowPreset` — visibility + group snapshot for all windows
- `DrawGroupPicker(int& groupId, const char* popupId)` — renders the `G1`/`G-` button + popup

`BroadcastGroupSymbol(int groupId, const std::string& sym)` in `main.cpp`:
- Guard: `g_groupSyncInProgress` prevents re-entrant loops
- For chart entries: calls `win->SetSymbol(sym)` → fires `OnDataRequest` → `ReqChartData`
- For trading entries: calls `ApplyTradingSymbol(te, sym)` — updates display AND re-subscribes IB mkt data + depth
- For news entries: calls `win->SetSymbol(sym)` → switches to Stock tab

Default group assignment: instance N → group N (e.g. Chart 1 / Order Book 1 / Scanner 1 / News 1 all start in G1).
Group picker button (`G1`–`G4` / `G-`) is the leftmost item in every window's toolbar.

## TradingWindow Layout

Three panels with user-draggable splitters:
- **Top-left**: DOM ladder (`DrawOrderBook`) — width controlled by `m_bookWidthRatio` (default 0.54)
- **Top-right**: Order entry (`DrawOrderEntry`) — takes remaining top width
- **Bottom**: Tabbed panel (Open Orders / Execution Log / Time & Sales) — height controlled by `m_topHeightRatio` (default 0.65)

Splitters are 4px `InvisibleButton` widgets; dragging updates the ratio stored on the window instance. Resize cursors (`ResizeEW` / `ResizeNS`) shown on hover.

**Order entry** supports all 13 `OrderType` values. Conditional fields render per type:
- Trail Stop / Trail Limit: $/% toggle (`m_trailByPct`), trailing amount buffer, optional stop cap, lmt offset (Trail Limit only)
- MIT / LIT: trigger price field (reuses `m_stpBuf`)
- Relative: peg offset (`m_offsetBuf`), optional price cap
- Midprice: optional price cap (reuses `m_lmtBuf`)
- MOC / MTL: no price fields

`SubmitOrder()` uses an explicit `kTypeMap[]` array (not an enum cast) to map `m_typeIdx` → `core::OrderType`.

## Order Impact — Side-Intent Badge + PnL Preview

`src/core/services/ChartAnalysis.h` adds two pure helpers used by both `ChartWindow` and `TradingWindow` to surface, before order submission, what the order would do to the user's position and the projected PnL at the prices currently entered:

- `ComputeOrderImpact(posQty, avgCost, commissionPerShare, isBuy, orderQty, fillPrice) → OrderImpact` — determines the side-intent kind (OpenLong/OpenShort, AddToLong/AddToShort, ReduceLong/ReduceShort, CloseLong/CloseShort, FlipToShort/FlipToLong) and computes closing-leg PnL, new position quantity, and (for Open/AddTo paths) the post-fill average cost. `commissionPerShare` is per-share (caller derives it from `totalCommission / |posQty|`).
- `PreviewStopTarget(targetImpact, stopImpact) → StopTargetPreview` — computes the R:R ratio from two `OrderImpact` legs (target fill + stop-out), both of which must be closing-path impacts.

Both windows render an **inline badge** (a colored `BeginChild` strip with `ImGuiChildFlags_Borders`, same pattern as `DrawUnguardedStrip`) showing:
- Open/AddTo paths: `OPEN LONG · 100 sh @ $187.42 · cost ~ $18,742` (blue)
- Reduce/Close paths: `CLOSE LONG · 100 sh · est. PnL +$425.00 (+2.27%)` (green if profit, red if loss)
- Flip paths: `FLIP TO SHORT · close 100 (+$425) → open 50 short @ $187.42` (orange)

When the Phase 12 setup overlay is active, a second line shows target/stop preview with R:R ratio.

Fill-price derivation is per order type:
- Market / MOC / MTL / Midprice → `last` (current price)
- Limit / LOC → `limitPrice`
- Stop → `stopPrice`
- StopLimit → `stopPrice` for the stop leg, `limitPrice` for the fill leg
- Trail / TrailLimit → `last ± trailAmount`
- MIT → `auxPrice`
- LIT → `auxPrice` for trigger, `limitPrice` for fill leg
- Relative → `last ± pegOffset`

In `ChartWindow` the badge renders below the BUY/SELL row when a side is armed. In `TradingWindow` it renders above the submit button and updates live as the user types prices.

## Replay Window (Phase 14)

Plan at `.claude/plans/replay.md`. Multi-instance (up to 10), group-syncable window for replaying historical trading days.

### Files
| Path | Purpose |
|---|---|
| `src/core/models/ReplayData.h` | `TickType`, `HistoricalTick`, `HistoricalDay` PODs |
| `src/core/services/ReplayEngine.h` | Pure-logic engine: `ReplaySession`, `ReplayClock`, `SimulatedAccount`/`SimulatedOrderBook`, `EvaluateBar`/`EvaluateTick` (all 13 OrderTypes), `BarRangeForSession`, `SnapCursorToNearestBar` |
| `src/ui/ChartRender.h` | Shared candlestick rendering (`RenderCandlestickBodies`, `RenderCursorLine`) — used by both `ChartWindow` and `ReplayWindow` |
| `src/ui/DatePicker.h` | Reusable calendar date-picker popup (`DrawDatePicker`) — shared by `WshCalendarWindow` and `ReplayWindow` |
| `src/ui/windows/ReplayWindow.{h,cpp}` | Window with toolbar (date picker, session/TF/speed/mode controls, Load/Play/Step buttons), ImPlot candlestick chart, scrubber, status bar, order entry (Operate mode, 13 types + confirmation popup), bottom tabs (Actual Trades, Sim Orders, Stats, AI Analysis), unguarded-position strip |
| `tests/test_replay.cpp` | 207 cases / 987 assertions under `[replay]` tag |

### Architecture
```
ReplayWindow (UI)  →  OnDataRequest  →  main.cpp  →  ReqHistoricalData (IB)
                    →  OnPaperOrderSubmit  →  SimulatedOrderBook (engine)
                    →  OnCursorMove  →  BroadcastReplayCursor (group sync)

ReplayEngine (pure logic, no I/O)  →  EvaluateBar/Tick  →  SimulatedAccount
```

### ReqId layout (11000–11999)
Per instance N (0–9): base = 11000 + N×100
- bars initial: base + 0
- bars extend: base + 1

### Key patterns
- **Data flow**: `ReqHistoricalData` → `onBarData` routing (reqIds 11000+) → `pendingBars` → `HistoricalDay` → `SetDay()`
- **Engine tick**: Clock always advances in both modes; bar evaluation + fills only in Operate mode
- **Progressive reveal**: Only candles up to `cursorBarIdx` are rendered via `RenderCandlestickBodies`'s `visibleCount` parameter
- **Group-time-sync**: `BroadcastReplayCursor()` with 100ms throttle per group, `g_replayCursorSyncInProgress` guard
- **Persistence**: `~/.config/ibkr-trading-app/replay-windows.cfg` — atomic `.tmp`+`rename`, per-second flush, restore on `FinishConnect`
- **Safety**: `ReplayWindow` holds no `IBKRClient` pointer; all orders go through `OnPaperOrderSubmit` → engine
