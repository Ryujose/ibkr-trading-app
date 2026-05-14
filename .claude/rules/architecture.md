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
- Chart hist: 1,3,5,7,9,11,13,15,17,19 (initial slot; rotates through 12000-12999 on every cancel→reissue cycle) · ext: 2,4,6,8,10,12,14,16,18,20 (initial slot; rotates through 13000-13999) · mkt: 100-109 (initial slot; rotates through 10000-10999 on each symbol change — see "Chart mktId/histId/extId rotation" below)
- Trading mkt: 110-119 (initial slot; rotates through 14000-14999) · depth: 120-129 (initial slot; rotates through 15000-15999) · tick-by-tick: 130-139 (initial slot; rotates through 16000-16999). Every `ApplyTradingSymbol`, `OnDepthModeChanged` (L1↔L2), and `OnDepthRowsChanged` cycle rotates the relevant id so stale ticks from the just-cancelled subscription (IB streams for a few ms after cancel) land on ids no entry owns and are silently dropped at the dispatcher — prevents crossed/intercalated L2 books on symbol switch.
- Futures /ES,/NQ (front-month): 140-141 · /ES,/NQ (Dec): 142-143 (market health)
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

## State Persistence (state-io.h)

`src/core/services/state-io.h` — header-only inline helpers shared by every per-feature `.cfg` file under `~/.config/ibkr-trading-app/`. No IB API / ImGui dependency so tests-core links it cleanly. Used by `watchlists.cfg`, `chart-modes.cfg`, `replay-windows.cfg`, plus the new chart/trading/scanner/news/singleton/app-prefs files added in `state-persistence.md`.

Public API:
- `EnsureConfigDir()` — returns `~/.config/ibkr-trading-app` creating it on demand; empty string on failure (no HOME, mkdir failed). Cross-platform (`USERPROFILE` fallback on Windows).
- `ConfigFilePath(filename)` — convenience: returns `<config-dir>/<filename>`.
- `AtomicWriteText(path, contents)` — writes to `<path>.tmp` then renames over `<path>`. Returns false on failure, leaves the original untouched. Falls back to copy+remove on cross-device rename failure.
- `ReadTextFile(path, &exists)` — reads whole file; `*exists=false` for missing files (not an error — first launch).
- `ParseStateBlocks(contents) → vector<StateBlock>` — line-based parser. Each block is `INSTANCE:<idx>` followed by `KEY:value` lines. Comments (`#…`) and blank lines skipped; malformed lines (no colon) skipped; bad `INSTANCE:` values default the instance to -1. Values containing colons are preserved (split on first colon only — URL-style values round-trip).
- `FormatStateBlocks(blocks) → string` — reverse builder, keys sorted alphabetically per block for deterministic diffs.
- `GetBool/GetInt/GetDouble/GetString(block, key, dflt, [lo, hi])` — typed accessors with range clamping; missing keys / parse failures / NaN/Inf return the default.
- `SetBool/SetInt/SetDouble/SetString(block, key, value)` — insertion-side helpers used by `Serialize*` methods on window classes.

`StateBlock` POD: `{ int instance; unordered_map<string, string> fields; }`.

**ImGui layout persistence**: `main.cpp` sets `io.IniFilename = ConfigFilePath("imgui.ini").c_str()` immediately after `ImGui::CreateContext()`. Pointer must outlive ImGui — backed by a `static const std::string`. Restores window positions, sizes, dock arrangement, splitter regions ImGui owns, and table column widths across launches regardless of CWD.

**Per-chart UI settings** (`chart-settings.cfg`): one `INSTANCE:N` block per `ChartWindow` covering every user-tunable preference: `IndicatorSettings` (10 toggles + 7 numeric params), `AutoAnalysisSettings` (9 toggles + 7 params), `SetupSettings` (1 master + 6 numeric + 7 confluence + 2 advanced), and the splitter-level `USE_RTH` / `SHOW_OVERNIGHT` / `SHOW_LEGEND` / `VOL_RATIO` / `RSI_RATIO` fields. `ChartWindow::SerializeSettings(StateBlock&) const` and `ChartWindow::ApplySettings(const StateBlock&)` are the public hooks — pure methods (no IB calls, no rendering side effects). All numeric fields are clamped on apply via `GetInt`/`GetDouble` so a corrupted file with `SMA1_PERIOD:9999999` rounds to a sane bound. main.cpp's `SaveChartSettingsFile()` builds a `vector<StateBlock>` by calling `SerializeSettings` per chart, formats via `FormatStateBlocks` (sorted keys → deterministic diffs), hashes the output, and only writes when the hash differs from `g_lastChartSettingsHash` — so the per-second flush in `RenderTradingUI` is a no-op when nothing changed, no dirty-flag plumbing required across the 40+ toolbar/popup mutation sites in `ChartWindow.cpp`. `LoadChartSettingsFromFile()` runs in `FinishConnect(false)` immediately after the existing `chart-modes.cfg` restore block — chart-modes restores style+symbol (which stamps preset defaults onto `m_ind`/`m_auto`/`m_setupSettings` via `setTradingStyle`), then chart-settings layers the user's customisations on top. `m_useRTH` is loaded AFTER the chart-modes `ReqChartData` was already issued with `!isIntraday` — the (rare) mismatch just means a few extra bars get filtered at render time, not a correctness bug. Sync flush from `DestroyTradingWindows` covers disconnect / app-exit.

**Per-trading-window UI settings** (`trading-settings.cfg`): one `INSTANCE:N` block per `TradingWindow` covering `USE_L2`, `EXCH_FILTER` (saved by **name**, not index — survives the dynamic per-symbol smart-component list refresh), `NUM_DEPTH_ROWS`, `LADDER_ROWS_IDX`, `TOP_HEIGHT_RATIO`, `BOOK_WIDTH_RATIO`, `CLICK_TO_TRADE`, `EXPAND_SPREAD`, and order-entry defaults `DEFAULT_QTY` (string buffer), `DEFAULT_SIDE`, `DEFAULT_TYPE` (clamped to the 13 OrderType variants), `DEFAULT_TIF`, `DEFAULT_OUTSIDE_RTH`. `TradingWindow::SerializeSettings(StateBlock&) const` / `ApplySettings(const StateBlock&)` are pure — `ApplySettings` sets fields directly (no setter calls) so loading before any depth subscription exists doesn't fire `OnDepthRowsChanged` / `OnDepthModeChanged` spuriously. On apply, the saved `EXCH_FILTER` name is looked up in the current `m_exchangeList` (just `{"All"}` on initial load — actual exchanges are populated as ticks arrive); when the name isn't present the filter falls back to index 0 ("All"). `LADDER_ROWS_IDX` apply also re-derives `m_ladderRows` from `kLadderOptions[]` to keep the two in sync. main.cpp's `SaveTradingSettingsFile` / `LoadTradingSettingsFromFile` use the same hash-diff persistence pattern as chart-settings. Load fires in `FinishConnect(false)` immediately after `LoadChartSettingsFromFile`; no subscription side effects because `TradingWindow` doesn't auto-subscribe — the user-typed-symbol path (`ApplyTradingSymbol`) later reads `numDepthRows()` / `useL2()` and issues the depth subscription with the restored values.

**Per-scanner UI settings** (`scanner-settings.cfg`): one `INSTANCE:N` block per `ScannerWindow` covering `ASSET_CLASS` (0=Stocks..3=Futures), `PRESET_IDX` (index into `kPresets[10]`), `SHOW_FILTERS` (filter bar expanded flag), all `ScanFilter` numeric ranges (`FLT_MIN_PRICE` / `FLT_MAX_PRICE` / `FLT_MIN_CHGPCT` / `FLT_MAX_CHGPCT` / `FLT_MIN_VOLUME` / `FLT_MAX_VOLUME` / `FLT_MIN_MKTCAP` / `FLT_MAX_MKTCAP` / `FLT_MIN_RSI` / `FLT_MAX_RSI`), string filters (`FLT_SECTOR`, `FLT_EXCHANGE`), filter-input char buffers (`FLT_BUF_MIN_PRICE` etc — persisted alongside the parsed ranges so the input fields look identical across restart), 14 column-visibility toggles (`COL_COMPANY` / `COL_CHANGE` / `COL_CHANGE_PCT` / `COL_VOLUME` / `COL_RELVOL` / `COL_MKTCAP` / `COL_PE` / `COL_HIGH52` / `COL_LOW52` / `COL_PCT_H52` / `COL_RSI` / `COL_MACD` / `COL_ATR` / `COL_SPARKLINE`), sort column + direction (`SORT_COL` clamped to the 16-value `ScanColumn` enum, `SORT_ASC`), and `AUTO_REFRESH` + `AUTO_REFRESH_SEC` (5–3600s clamp). `ScannerWindow::SerializeSettings/ApplySettings` are pure; main.cpp's `SaveScannerSettingsFile/LoadScannerSettingsFromFile` use the same hash-diff pattern. Load fires in `FinishConnect(false)` after `LoadTradingSettingsFromFile`. No scan side effects on apply — `RunScan` is user-initiated; the user clicking "Scan" picks up the restored asset-class + preset + filters.

**Singleton-window settings** (`singleton-settings.cfg`): three `WINDOW:<name>` blocks (no `INSTANCE:` lines — these are singletons) for Portfolio, Orders, and WshCalendar. The `StateBlock` struct in `state-io.h` carries a `windowName` field; `ParseStateBlocks` treats `WINDOW:` lines as block delimiters alongside `INSTANCE:`, and `FormatStateBlocks` emits `WINDOW:<name>` for blocks whose `windowName` is non-empty. Portfolio block: `PORT_SORT_COL` (clamped 0–12, `PositionColumn` enum), `PORT_SORT_ASC`, 7 `PORT_COL_*` column-visibility toggles, `PORT_FILTER_SYMBOL` (trade-history filter buffer). Orders block: `ORD_FILTER_SYMBOL` / `ORD_FILTER_SIDE` (0–2) / `ORD_FILTER_DATE` (history filter state). WshCalendar block: `WSH_FILTER_SYMBOL` / `WSH_FILTER_FROM` / `WSH_FILTER_TO` (date strings), `WSH_FILTER_TYPE` (0–4) / `WSH_FILTER_IMPORTANCE` (0–3), `WSH_SORT_COL` (0–4) / `WSH_SORT_ASC`. Active-tab state on Portfolio and Orders is NOT included — both use ImGui `TabItem` which ImGui's `imgui.ini` already persists via the stable `io.IniFilename` path set in Task 2 (`Task #79`). All three windows' `SerializeSettings`/`ApplySettings` are pure (no IB calls, no side effects). main.cpp's `SaveSingletonSettingsFile` / `LoadSingletonSettingsFromFile` use the same hash-diff pattern. Sync flush in `DestroyTradingWindows`, per-second flush in `RenderTradingUI`, load in `FinishConnect(false)` immediately after `LoadScannerSettingsFromFile`.

**Save/load invariants (closed-entry guard + instance spawn)**:
- All per-instance save builders (`SaveChartModesFile`, `BuildChartSettingsText`, `BuildTradingSettingsText`, `BuildScannerSettingsText`, replay save) skip entries where `!win || !win->open()` — closed windows (`m_open=false`) never reach disk. On restart they don't reappear.
- All per-instance loaders (`LoadChartSettingsFromFile`, `LoadTradingSettingsFromFile`, `LoadScannerSettingsFromFile`, and the chart-modes restore block) first compute `maxInstanceIdx` from the saved blocks and call the appropriate `Spawn*Window()` in a `while` loop to fill the vector up to that index — so settings for G2/G3 (beyond instance 0) land on existing windows.
- `CreateTradingWindows()` spawns only instance 0; the restore path fills higher indices from disk.

**App-wide UI preferences** (`app-prefs.cfg`): singleton-block file (no `INSTANCE:` lines) covering `FONT_SIZE` (0=Small / 1=Medium / 2=Large, drives `io.FontGlobalScale` via `kFontScales[]` + `ScaleAllSizes`), `DEFAULT_TRADING_STYLE` (int enum value applied to every newly-spawned `ChartWindow` in `SpawnChartWindow`; existing charts keep their per-chart style from `chart-modes.cfg`), and `SYNC_TWS_DISPLAY_GROUPS` (the Settings toggle wired to `SetTwsGroupSync`). `LoadAppPrefsFromFile()` runs in `main()` immediately after `g_baseStyle = ImGui::GetStyle()`; `ApplyAppPrefsToStyle()` then sets `FontGlobalScale` + re-scales the saved baseline so the login window already renders at the user's preferred size on launch. The TWS sync global stages there too — the actual `SubscribeToGroupEvents` fan-out fires from the existing post-connect block in `FinishConnect` which already inspects `g_twsGroupSync`. `SaveAppPrefsFile()` fires immediately on every Settings UI change (font radio click, default-style combo selection, TWS toggle) — small writes, frequent enough not to need a debounce.

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

**Confluence gates (Phase 15b)**: Five optional boolean filters run inside `ComputeBreakoutSignal()` after the BB-compression + momentum checks and before the final `LongSetup`/`ShortSetup` assignment. Each gate returns `true` if disabled in `m_setupSettings`, or if the condition is met. The gates are:

1. **Trend align** (`trendAlign`, default OFF) — auto-trend slope must agree with trade direction (`TrendSupportsSide`).
2. **VWAP context** (`vwapContext`, default OFF) — long only above VWAP, short only below (`VwapSupportsSide`).
3. **Market health** (`marketHealth`, default OFF) — /ES and /NQ (front-month + Dec) must not move strongly against the setup (`FuturesSupportDirection`). Configured per trading-style preset; Dec futures are subscribed on reqIds 142/143 and fanned out to all charts via `OnFuturesTick`.
4. **RSI filter** (`rsiFilter`, default OFF) — long RSI ≤ 70, short RSI ≥ 30 (`RsiSupportsSide`).
5. **Volume confluence** (`volumeConfluence`, default OFF) — the active supply/demand zone's buffered range must overlap at least one high-volume bin (≥30% of POC volume) in `m_vp`.

The four pure helpers (`TrendSupportsSide`, `VwapSupportsSide`, `RsiSupportsSide`, `FuturesSupportDirection`) live in `core::services::ChartAnalysis.h` with full Catch2 coverage under the `[setup]` tag. Gate controls appear in `DrawSetupSettingsPopup()` with per-style defaults in `TradingStyle.h`.

**Multi-target exits**: When `m_setupSettings.multiTarget` is on, `ComputeSetupPlan` walks the remaining KeepTopN levels beyond T1 to find a second take-profit level (T2), rendered as a magenta dashed h-line in `DrawSetupOverlay()`. `SetupPlan` gains `t2Target` / `t2SplitPct` fields.

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

## Volume Profile (Phase 15 — CW-1)

`core::services::ChartAnalysis.h` gains `VolumeBin` / `VolumeProfile` structs and `ComputeVolumeProfile(highs, lows, volumes, priceLo, priceHi, numBins=50) → VolumeProfile{bins, pocIdx, maxVolume, totalVol, valueAreaLoIdx, valueAreaHiIdx}` as a pure inline free function. Each bar's volume is distributed *uniformly across the bar's [low, high] range*, normalised against the bar's *full* range — so a partially-clipped bar contributes only `visibleRng / fullRng` of its volume. Matches TradingView / IB convention; avoids the "POC sits at one big bar's close" artefact of pure-typical-price profiles. POC = `argmax(bins.volume)`. Value Area (~70% of total volume) computed by expanding outward from POC, picking whichever side has the larger neighbouring bin (right-bias on ties). `numBins` clamped to `[10, 200]`; degenerate inputs (`priceHi <= priceLo`, mismatched lengths, empty bars) yield empty profile / `pocIdx = -1`.

`ChartWindow::IndicatorSettings::volumeProfile` (default false; per-mode defaults driven by `StylePreset.indVolumeProfile` — Day Trading and Swing ON, others OFF) and `::vpBins` (default 50) drive the histogram. `m_vp` is recomputed every frame inside `DrawVolumeProfile()` from `ImPlot::GetPlotLimits()` Y-range, so panning / zooming re-buckets to the visible price band. `DrawVolumeProfile()` is invoked from `DrawCandleChart()` between `DrawCandlesticks()` and `DrawOverlays()`; renders translucent right-anchored horizontal bars via the plot drawlist (not `PlotBarsH`, so it doesn't interact with the X-axis or auto-fit). Histogram occupies the rightmost ~25% of the plot at the POC; bins scale proportionally against `m_vp.maxVolume`. Three alphas: ~18% normal, ~29% value-area bins, ~43% POC + thin gold POC border. Toolbar `VP` checkbox lives in the Auto: row right after `Setup`; `DrawAutoSettingsPopup()` exposes `VP bins` `InputInt` (10–200). Tests under `[volume-profile]` tag in `tests/test_chart_analysis.cpp` (12 cases / 72 assertions) cover single-bar uniform distribution, POC concentration, out-of-range bars (above + below), partial overlap proportional contribution, empty / mismatched / degenerate input, `numBins` clamping, zero-volume bars skipped, and value-area expansion (synthetic profile [10,20,50,20,10] → POC=4, VA=[3,5]).

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
- `parentId` — non-zero links an order as an IB-native bracket child of the parent order
- `ocaGroup` / `ocaType` — One-Cancels-All linkage. Siblings sharing the same `ocaGroup` are mutually-OCA; `ocaType=1` is cancel-with-block (most common). Used by ChartWindow's Bracket order to OCA-pair the STP stop-loss and TP take-profit legs submitted on entry fill.
- `holdReason` — informational IB warning when an order is accepted but held (e.g. pre/post-market submission held until RTH open, "Outside RTH attribute ignored"). Distinct from `rejectReason`: the order is still live. Populated in `main.cpp`'s `onError` lambda for a curated warning-code allowlist (399, 404, 2109, 2148, 10311); cleared implicitly once the order transitions to Filled/Cancelled/Rejected. The OrdersWindow status column appends a yellow `HELD` chip; the ChartWindow pending-order overlay renders the leg's dashed line in amber and appends `⚠ HELD` to the row label.

## Bracket Orders (ChartWindow)

ChartWindow's "Bracket" order type (entry LMT + STP stop-loss + optional TP take-profit) is **synthesized client-side**, not submitted as an IB-native bracket-with-`parentId`. The click order is **Entry → STP → TP** (not the more-conventional STP first) so that once the entry leg is locked, the cursor bubbles for the STP and TP placements can show projected $ loss / gain, % move from entry, and live R:R against the locked entry. The flow:

1. User picks `Bracket` from the order-type combo, clicks BUY/SELL → enters chart-click arming.
2. **Click 1** — entry LMT price (stored in `m_firstPrice`; `m_firstPricePlaced=true`). The cursor bubble for this phase shows only estimated cost (`~ $N`) since no risk leg exists yet.
3. **Click 2** — STP stop-loss price (stored in `m_secondPrice`; `m_secondPricePlaced=true`). Side-validated: BUY rejects STP at-or-above entry; SELL rejects STP at-or-below entry. While positioning, the cursor bubble shows ` STOP $X  -$Y (-Z%) ` — projected loss in dollars and percent vs the locked entry.
4. **Click 3** — TP take-profit price; fires the entry LMT and stuffs `PendingBracketStop{symbol, stopSide=opposite, qty, stopPrice, tpPrice}` into `g_pendingBracketStops` keyed by the entry order id. Side-validated: BUY rejects TP at-or-below entry; SELL rejects TP at-or-above entry. The cursor bubble shows ` TP $X  +$Y (+Z%)  R:R W ` — projected gain plus risk:reward computed from the locked entry/STP and the live TP cursor. The locked STP line additionally shows its frozen ` STOP $X  -$Y (-Z%) ` label.
5. On entry fill (`onFillReceived` in main.cpp): the map is consulted. STP and TP are submitted as a pair with shared `ocaGroup = "BRK_<entryId>"` and `ocaType = 1`. IB auto-cancels the survivor when one fills.

`drawArmedLine` accepts optional `pct` and `extra` (free-form trailing string) parameters used to compose the bracket-leg labels: `pct` formats as ` (+%.2f%%)` after the dollar P&L, and `extra` appends a trailing token like `R:R 2.34`. Bubble and right-edge labels share the same composer so they stay consistent.

The `DrawOrderImpactBadge` uses `m_firstPrice` (locked entry) as the fill-price proxy from phase 2 onward, so the position-impact strip below the trade panel reflects the open-position impact at the entry — not a future closing leg's price.

`tpPrice == 0.0` in the pending struct is treated as a legacy stop-only bracket — only the STP leg is submitted on fill, with no `ocaGroup`. Reserved for backward compatibility; new bracket orders always include TP.

The confirmation popup (when `Transmit Instantly` is off or Ctrl+click) renders all three legs with computed R:R; the Confirm button fires the entry and the on-fill handler then dispatches STP+TP. Cancel/Escape clears `m_isBracketConfirm` / `m_bracketStopPrice` / `m_bracketTpPrice` and the dual-price phase state.

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
- `kNumGroups = 10` — number of sync groups (matches `kMaxMultiWin`)
- `GroupState { int id; std::string symbol; }` — active symbol per group (10 slots)
- `WindowPreset` — visibility + group snapshot for all windows
- `DrawGroupPicker(int& groupId, const char* popupId)` — renders the `G1`/`G-` button + popup with G1–G10 options
- Group ids are clamped to `[1, kNumGroups]` in all `Spawn*` functions via `(idx % kNumGroups) + 1`

`BroadcastGroupSymbol(int groupId, const std::string& sym)` in `main.cpp`:
- Guard: `groupId > kNumGroups` upper-bound check prevents out-of-bounds access to `g_groups[]`
- Guard: `g_groupSyncInProgress` prevents re-entrant loops
- For chart entries: calls `win->SetSymbol(sym)` → fires `OnDataRequest` → `ReqChartData`
- For trading entries: calls `ApplyTradingSymbol(te, sym)` — updates display AND re-subscribes IB mkt data + depth
- For news entries: calls `win->SetSymbol(sym)` → switches to Stock tab
- TWS display group sync only covers G1–G4 (IB's fixed limit); G5–G10 are app-local only

Default group assignment: instance N → group `(N % 10) + 1`.
Group picker button (`G1`–`G10` / `G-`) is the leftmost item in every window's toolbar.
`OnDataRequest` callback is suppressed during `FinishConnect` chart-modes restore to prevent cascading group broadcasts during initial load.

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

## Bracket After-Hours Guard

When a bracket order (entry LMT + STP stop-loss + TP take-profit) is placed outside regular trading hours (09:30–16:00 ET), IB only evaluates the stop trigger during extended hours when the stop leg itself is marked `outsideRth = true`. Originally we hardcoded the stop leg to `outsideRth = false` on the assumption that "stop conditions only trigger during RTH regardless" — that turned out to be wrong; IB *does* evaluate stop triggers in pre/after-market for stocks that allow ext-hours stops, but only when the flag is set. With it false the order was just parked until the next RTH open, leaving the position effectively unguarded.

**Detection**: `core::BarSession(std::time(nullptr))` checks current session at bracket fire time (both confirmation popup and transmit-instantly paths).

**Behavior outside RTH**:
1. Confirmation popup shows an orange warning: *"IB does not trigger stop orders outside regular trading hours..."*
2. Stop type auto-switches from `STP` to `STP LMT` (`useStopLmt = true` in `core::PendingBracketStop`)
3. Stop limit offset = `stopPrice * 0.001` rounded via `core::services::RoundToTick(raw, 0.01)` to the penny grid (prevents IB error 110)
4. **`outsideRth = true` on the stop leg** — propagated from `PendingBracketStop.outsideRth` in `main.cpp`'s `onFillReceived` bracket handler. Makes the stop eligible to fire pre/post-market for instruments that support it.
5. `outsideRth = true` on the TP leg (limit orders can fill outside RTH)
6. During regular hours — `PendingBracketStop.outsideRth = false`, propagates to both legs unchanged (plain STP / RTH-only TP).

**Caveat**: not every instrument honours `outsideRth = true` on stop orders — futures and some ETFs behave differently from common stocks. When IB still parks the order until RTH open, it sends an informational warning (`error()` code 399/404/2109/2148/10311) that the `holdReason` plumbing surfaces as an amber `HELD` chip in the OrdersWindow and on the chart pending-order overlay (amber dashed line + `⚠ HELD` row tag).

`core::PendingBracketStop` struct moved from `ChartWindow.h` to `core/models/OrderData.h` with 7 fields including `outsideRth` and `useStopLmt` (both default `false`). Unit-tested in `test_models.cpp`.

## Historical Data Safety Net

`core::services::ShouldReplaceHistoricalBars(existingCount, existingSymbol, newSymbol, newCount) → bool` in `ChartAnalysis.h` — pure function used by `ChartWindow::SetHistoricalData` to reject three classes of broken replacement:

1. **Empty completion on existing data** — keepUpToDate reset sends `done=true` with no bars
2. **Symbol mismatch** — stale response from a previous subscription (backstops histId rotation)
3. **Data-loss ratchet** — existing ≥ 50 bars, new ≤ 5 bars → rejected (catches keepUpToDate mid-session resets delivering a 1-bar stub)

Full Catch2 coverage in `test_chart_analysis.cpp` (6 cases / 16 assertions).

## Chart Sub-Plot Splitters

Two draggable splitter bars (same style as TradingWindow's panel splitters) between:
- **Price chart ↔ Volume** — adjusts `m_volumeHeightRatio` (clamped 5%–50%)
- **Volume ↔ RSI** — adjusts `m_rsiHeightRatio` (clamped 5%–40%)

5px `InvisibleButton` with `ImGuiButtonFlags_MouseButtonLeft`, background rect (grey → white-grey on hover/drag), `ImGuiMouseCursor_ResizeNS` cursor. Heights are pre-computed in `DrawCandleChart()` and stored in `m_cachedVolumeH` / `m_cachedRsiH` for consistent sub-plot sizing.

**Legend toggle**: `m_showLegend` checkbox in toolbar (next to "Sessions") — hides the ImPlot legend box without removing indicators. Uses `ImPlotFlags_NoLegend`.

## TradingWindow DOM — Click-to-Trade, Last-Price Highlight, Legend

**Click-to-trade**: Moved from full-row `MouseClicked[0]` hack to a proper `InvisibleButton` overlaid on the price text in column 2. `PushID(rowSeq++)` / `PopID()` per row prevents ID conflicts with multi-venue L2 data. Clicking an ask price places a BUY limit, clicking a bid price places a SELL limit.

**Last-price highlight**: `m_lastPrice` updated from `OnTick()` and `OnTickByTick()`, cleared on `SetSymbol()`. Any DOM row within ±$0.005 of the last traded price renders in gold with a ` *` suffix.

**Ladder legend**: Compact color-coded legend below the submit button showing:
- Green = Bid depth (resting buy orders)
- Red = Ask depth (resting sell orders)
- Blue = Executed volume (actual trades today)

**Futures health fix**: `OnFuturesTick` now only accepts LAST (field 4) for the displayed price — BID (1) and ASK (2) no longer contaminate the value. `DrawFuturesItem` shows price-only when prevClose hasn't arrived yet, avoiding bogus `+29158.75 (0.00%)` reads.
