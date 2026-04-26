# Architecture Rules

## Directory Structure

```
├── src/
│   ├── core/
│   │   ├── services/         # IB Gateway integration (IBKRClient.h/.cpp, IBKRUtils.h, ChartAnalysis.h)
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

`src/core/services/ChartAnalysis.h` — standalone header (no IB API / ImGui / ImPlot deps) for the auto technical-analysis layer in `ChartWindow`. Inline free functions in `core::services`:
- `FindSwings(highs, lows, k, scanCap=0) → SwingResult` — pivot detection with left/right window `k`. Strict `>` on the left and `>=` on the right (mirror for lows) so flat tops/bottoms register as a single swing. `scanCap` limits the scan to the last N bars.
- `ATR(highs, lows, closes, period) → vector<double>` — Wilder's true-range smoothing.
- `ClusterLevels(swings, tol) → vector<Level>` — sweeps swings sorted by price, merging consecutive ones within `tol` into a single `Level{price, touches, firstIdx, lastIdx, minPrice, maxPrice}`. Cluster price = mean of constituents; `minPrice`/`maxPrice` track the lowest/highest constituent so ChartWindow can render thickness-aware supply/demand zones.
- `KeepTopN(clusters, currentPrice, side, minTouches, maxLevels) → vector<Level>` — filters by side (`Above`/`Below`), min touches, then ranks by `(touches desc, lastIdx desc)` and caps at `maxLevels` (`maxLevels=0` = no cap).
- `LinearRegression(closes, lookback) → TrendFit{valid, slope, intercept, sigma, firstIdx, lastIdx}` — closed-form OLS using bar idx as x over the trailing `lookback` closes. `sigma` is population residual stdev (used by the optional ±2σ channel). Returns `valid=false` on too-small input or zero-variance x.
- `DonchianBands(highs, lows, N) → DonchianResult{hi, lo}` — rolling N-bar max/min envelope. Vectors are sized to `highs.size()` with `0.0` for indices `< N-1` (matching the convention used by `CalcSMA`/`CalcEMA`/`CalcBollingerBands`).
- `LargestSwingSpan(swingHighs, swingLows, window) → AutoFibSpan{valid, hiIdx, hiPrice, loIdx, loPrice}` — picks the (high, low) pair with the largest absolute price difference among the last `window` swings of each list. Used to anchor the auto-Fibonacci overlay. `valid=false` on empty input or zero-span.
- `ClassicPivots(prevH, prevL, prevC) → DailyPivot{valid, p, r1-r3, s1-s3}` — pure formula for classic pivot points (`P=(H+L+C)/3`, `R1=2P-L`, `R2=P+(H-L)`, `R3=H+2(P-L)`, mirror for S). Caller is responsible for picking the previous trading day's OHLC (ChartWindow does this in `ComputeDailyPivots()` by walking `m_xs` backwards using `localtime`). Returns `valid=false` on non-positive prices or `H<L`.
- `FindBreakouts(resistances, supports, highs, lows, closes, atr, lookback, minTouches) → vector<BreakoutMark{idx, y, up}>` — walks the last `lookback` bars and emits a mark whenever `close[i-1] <= R.price < close[i]` (up) or the support mirror (down). Uses `cluster.firstIdx` as a causality guard so a level cannot mark bars before it was established. `minTouches` filters trivially-weak clusters. Mark `y` = `highs[i] + 0.5·atr[i]` (up) or `lows[i] - 0.5·atr[i]` (down). Caller passes the raw cluster output (not `KeepTopN`-filtered) so already-broken levels still produce historical marks.

`ChartWindow.h` re-exports `Level` as `ChartWindow::AutoLevel` and `TrendFit` as `ChartWindow::AutoTrend`. Auto-detection results live in `m_autoSupports`/`m_autoResistances`/`m_atr14`/`m_autoTrend` and are populated by `DetectStructure()`, which runs at the tail of `ComputeIndicators()` so every existing recompute trigger refreshes auto-analysis. Toolbar toggles in `DrawAnalysisToolbar()` call `DetectStructure()` directly. Rendering: `DrawAutoSupportResistance()` is invoked from `DrawOverlays()` between the current-price line and the user drawings — red dashed h-lines for resistance, green for support, alpha proportional to touch count, left-edge price tag formatted ` R 187.42 (3×) `.

**Auto trend line** (`AutoAnalysisSettings::trend`, default ON; `trendLookback` default 50): `m_autoTrend` is populated whenever `trend` is on (using `min(trendLookback, n)` so short series still fit). `DrawAutoTrend()` is invoked from `DrawOverlays()` after S/R rendering: solid segment from `(firstIdx, y(firstIdx))` to `(lastIdx, y(lastIdx))` plus a faded (60% alpha) `L/4`-bar forward projection. Colour reflects slope direction (green if `slope > +ε`, red if `< −ε`, grey otherwise) with `ε = 0.05·sigma/L`. When `m_auto.trendChannel` is on, ±2σ parallel lines render on both segments at 50% alpha (fainter on the projection). A right-edge ` trend +0.123/bar ` label tags the projection endpoint.

**Supply/demand zones + imminent-breakout signal** (`AutoAnalysisSettings::zones`, default OFF): `ComputeBreakoutSignal()` runs at the tail of `DetectStructure()` when `zones` is on. It walks supply zones first then demand zones, finds the one whose `[minPrice − 0.5·ATR, maxPrice + 0.5·ATR]` interval contains the latest close, and only emits a `LongSetup`/`ShortSetup` (`m_breakoutSignal`) when (a) `bbWidth[n-1] < 0.7 × avg(bbWidth[n-50..n-1])` (BB compression, ≥20 valid samples required) and (b) `closes[n-1] − mean(closes[n-6..n-2])` exceeds ±0.1·ATR. `DrawAutoZones()` is invoked from `DrawOverlays()` immediately before `DrawAutoSupportResistance()` so the dashed S/R lines render on top of the rectangles: translucent red fill + thin red border for supply, translucent green for demand. When a signal is active, the right edge of the active zone gets a coloured triangle (▲ green / ▼ red) at the zone midpoint with a ` LONG SETUP ` / ` SHORT SETUP ` label tag to its left. `DetectStructure()` now populates the S/R clusters whenever `zones` is on, even if the individual `supports`/`resistances` toggles are off, since zones reuse the same cluster output.

**Donchian + Keltner channels** (`AutoAnalysisSettings::donchian`/`keltner`, default OFF): Donchian uses `core::services::DonchianBands(m_highs, m_lows, donchianLen)` (default `donchianLen=20`); Keltner reuses `m_ema` (always computed in `ComputeIndicators`, regardless of the EMA20 indicator toggle) and `m_atr14` to build `m_keltUpper = m_ema + 2·m_atr14` / `m_keltLower = m_ema - 2·m_atr14`. Both render via `ImPlot::PlotLine` from `DrawCandleChart()` (faded yellow for `Donch Hi`/`Donch Lo`, faded cyan for `Kelt Hi`/`Kelt Lo`) so they participate in the legend alongside SMA/BB/EMA.

**Auto-Fibonacci** (`AutoAnalysisSettings::autoFib`, default OFF): `m_autoFib = LargestSwingSpan(sw.highs, sw.lows, /*window=*/30)` picks the largest-span pair among the last 30 swings of each list. `DrawAutoFib()` (called from `DrawOverlays()` after S/R/trend) renders six dashed h-lines at `kFibLevels[]` between `loPrice` and `hiPrice`, in faded `kFibColors` with **left-edge** ` F 38.2% 187.42 ` labels (manual fibs use right-edge labels — staying on opposite edges avoids collisions). Anchor markers (small filled circles) at the swing-high (orange) and swing-low (blue) make the source pair visible.

**Daily pivot points** (`AutoAnalysisSettings::pivotPoints`, default OFF; intraday only): `ComputeDailyPivots()` walks `m_xs` backwards (using `localtime` to match how intraday timestamps are interpreted elsewhere) to identify the previous trading day's first/last bar and the current day's first bar. Computes `m_pivots = ClassicPivots(prevH, prevL, prevC)` and pins `m_pivotsTodayStart`/`m_pivotsTodayEnd`. `DrawAutoPivots()` (called from `DrawOverlays()` when `pivotPoints && IsIntraday(m_timeframe)`) renders 7 dashed h-lines (S3/S2/S1 in greens, P in light grey, R1/R2/R3 in reds) spanning `[todayFirstIdx, todayLastIdx]` only, with right-aligned label tags inside today's range. Toolbar checkbox is `BeginDisabled()`-wrapped on D1/W1/MN with an `AllowWhenDisabled` tooltip explaining "Intraday only".

**Breakout markers** (`AutoAnalysisSettings::breakouts`, default OFF): `m_breakouts = FindBreakouts(highClusters, lowClusters, m_highs, m_lows, m_closes, m_atr14, /*lookback=*/50, m_auto.minTouches)` is computed in `DetectStructure()` after clustering. The unfiltered cluster output is passed (not `KeepTopN`-filtered) so already-broken levels still produce historical marks. `DrawBreakoutMarks()` (called from `DrawCandleChart()` after candlesticks/overlays) emits two `ImPlot::PlotScatter` calls — `Breakout Up` with `ImPlotMarker_Up` (green) and `Breakout Dn` with `ImPlotMarker_Down` (red).

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
