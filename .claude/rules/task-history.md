# Task History

All planned tasks are complete through Phase 7.

## Phase 1: Infrastructure
- [x] **Task #1** — CMake configuration with dependency validation
- [x] **Task #2** — ImGui base with dockable window system

## Phase 2: Core Features
- [x] **Task #3** — Login window (paper and live accounts) → `RenderLoginWindow()` in `main.cpp`
- [x] **Task #4** — Candlestick chart window with indicators → `src/ui/windows/ChartWindow.{h,cpp}`

## Phase 3: Data Feeds
- [x] **Task #5** — News window (Market / Portfolio / Stock tabs) → `src/ui/windows/NewsWindow.{h,cpp}`
- [x] **Task #6** — Trading window (order book + execution) → `src/ui/windows/TradingWindow.{h,cpp}`

## Phase 4: Advanced Features
- [x] **Task #7** — Market scanner → `src/ui/windows/ScannerWindow.{h,cpp}` + `src/core/models/ScannerData.h`
- [x] **Task #8** — Portfolio dashboard → `src/ui/windows/PortfolioWindow.{h,cpp}` + `src/core/models/PortfolioData.h`

## Phase 5: Multi-Instance & Group Sync
- [x] **Task #9** — Window reopen from menu — all 6 windows togglable via Windows menu (`bool& open()` on every window)
- [x] **Task #10** — Window groups (`src/core/models/WindowGroup.h`) — `GroupState`, `WindowPreset`, `DrawGroupPicker`; `BroadcastGroupSymbol` + `g_groupSyncInProgress` guard in `main.cpp`; presets menu (Trading Focus, Research, Full Desk)
- [x] **Task #11** — Multi-instance windows — up to 4 simultaneous Chart, Order Book, and Scanner windows; `ChartEntry`/`TradingEntry`/`ScannerEntry` structs with per-entry reqId allocation; `SpawnChartWindow`/`SpawnTradingWindow`/`SpawnScannerWindow`; "+ New" buttons in Windows menu
- [x] **Task #12** — Group sync fix — `ApplyTradingSymbol` helper so group broadcast re-subscribes IB market data + depth (not just display); reqId collision audit (TradingDepth moved to 120+, ScannerBase moved to 1000+)
- [x] **Task #13** — Group picker visibility fix — replaced Unicode glyphs with ASCII `G1`/`G-` labels, moved picker to leftmost toolbar position in all windows, raised button alpha
- [x] **Task #14** — Default group assignment — instance N auto-assigned to group N on spawn; NewsWindow defaults to group 1

## Phase 6: UX Polish & Resilience
- [x] **Task #15** — Current price line — dashed grey horizontal line at last close price; right-aligned price tag inside the chart clip rect (`DrawOverlays()` in `ChartWindow.cpp`); uses `pMax.x` (actual pixel boundary) for correct placement
- [x] **Task #16** — Disconnection badge — `LostConnection` added to `ConnectionState` enum; on unexpected disconnect keeps trading UI alive (nulls client, doesn't destroy windows); orange `DISCONNECTED` badge with background rect rendered left of `[LIVE]`/`[PAPER]` in menu bar
- [x] **Task #17** — Auto-reconnect — `StartSilentReconnect()` in `main.cpp`; `g_reconnectNextAttempt` timer (5s interval) polled each frame when `LostConnection && !g_IBClient`; on reconnect success re-subscribes each chart/trading window with its current symbol+timeframe instead of recreating windows; `getTimeframe()` added to `ChartWindow`, `getSymbol()` added to `TradingWindow`

## Phase 7: Testing Infrastructure
- [x] **Task #18** — Extract `ParseStatus` / `ParseIBTime` to `src/core/services/IBKRUtils.h` as inline free functions; move `IBKRClient::Push()` from `private` to `protected`; remove private static declarations from `IBKRClient.h`
- [x] **Task #19** — `tests/CMakeLists.txt` — Catch2 v3.7.1 via FetchContent; `tests-core` (no IB API dep) and `tests-ibkr` (links ibapi-lib) targets; `IBKR_SANITIZE` option applies ASan+UBSan to `tests-core`
- [x] **Task #20** — `tests/test_market_data.cpp` — 20+ cases: all Timeframe label/seconds/barsize/duration helpers; `IsUSDST` DST boundary cases (March/November 2024); `BarSession` session classification at all transition boundaries (EDT and EST)
- [x] **Task #21** — `tests/test_models.cpp` — enum string helpers (`OrderSideStr`, `OrderTypeStr`, `TIFStr`, `OrderStatusStr`, `ScanPresetLabel`, `AssetClassLabel`); struct default-value assertions for `Order`, `Fill`, `DepthLevel`, `ScanResult`, `ScanFilter`, `Position`, `AccountValues`, `Bar`
- [x] **Task #22** — `tests/test_ibkr_utils.cpp` — `ParseStatus` all IB string variants including case-sensitivity; `ParseIBTime` for 8-digit date (noon UTC mapping), Unix timestamp round-trips, calendar correctness across multiple dates
- [x] **Task #23** — `tests/test_ibkr_queue.cpp` — `TestableIBKRClient` subclass; dispatch tests for all major `IBMessage` variants; null-callback safety; ordering; queue-drain idempotency
- [x] **Task #24** — CI: all three platform jobs (`build-linux`, `build-macos`, `build-windows`) now configure with `-DIBKR_BUILD_TESTS=ON` and run `ctest`; FetchContent dep cache added; new `sanitize-linux` job runs `tests-core` under ASan + UBSan

## Phase 8: UX Redesign & Responsiveness
- [x] **Task #25** — MSVC NOMINMAX fix — `$<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>` added to `target_compile_definitions` in `CMakeLists.txt`; resolves C2589 `min`/`max` macro conflict on Windows CI
- [x] **Task #26** — Windows menu restructure — `Windows → IBKR → ...` submenu hierarchy; `ImGuiItemFlags_AutoClosePopups = false` keeps submenu open while toggling windows; `kMaxMultiWin` raised from 4 to 10 for Chart, Order Book, Scanner, and News
- [x] **Task #27** — Dynamic window title labels — all windows display `"<Type> <Symbol> <Group>###stable_id"` (e.g. "Chart AAPL G1") in both the IBKR submenu and the window title bar; uses `###id` triple-hash for stable ImGui window identity; `ImGuiWindowFlags_NoFocusOnAppearing` prevents newly opened windows from stealing focus and closing the menu
- [x] **Task #28** — News feed multi-instance — `NewsEntry` struct with per-instance reqId ranges (2000–3699); `SpawnNewsWindow(idx)` wires per-instance callbacks; market news broadcasts to all instances; per-instance stock/portfolio/article routing by reqId range; default group = instance index
- [x] **Task #29** — Settings panel — floating `RenderSettingsWindow()` with Small / Medium / Large font size radio buttons; `io.FontGlobalScale` (0.85 / 1.0 / 1.5) drives scaling; `g_baseStyle` saved once after style setup and restored before each `ScaleAllSizes()` call to prevent compounding
- [x] **Task #30** — `src/ui/UiScale.h` — `em(px)` converts design-time pixel values (authored at 13 px base) to font-scale-aware sizes; `FlexRow` struct implements CSS `flex-wrap: wrap` for ImGui toolbars — calls `SameLine()` only when the next item fits on the current line; static helpers `buttonW`, `checkboxW`, `textW` estimate item widths
- [x] **Task #31** — FlexRow applied to all toolbars — `ChartWindow::DrawToolbar()` (group picker, symbol input, quick symbols, timeframe, zoom, intraday toggles, 7 indicator checkboxes), `DrawAnalysisToolbar()`, `TradingWindow` order-book header, `NewsWindow::DrawToolbar()`, `ScannerWindow::DrawToolbar()`; all `SetNextItemWidth(N)` calls converted to `SetNextItemWidth(em(N))`
- [x] **Task #32** — Chart window inner rows responsive — `DrawTradePanel()` (Trade label, Qty, order type, trail, session, TIF, BUY/SELL, armed hint), `DrawInfoBar()` (symbol/date, O/H/L/C, change, VWAP, PnL), `DrawPositionStrip()` (Position, Entry, Last, Unreal P&L, Comm, Net) all use `FlexRow`; `DrawPositionStrip` child window height is now dynamic (2 × lineHeight) to accommodate wrapped content
- [x] **Task #33** — TradingWindow resizable panels — `m_topHeightRatio` (default 0.65) and `m_bookWidthRatio` (default 0.54) stored as member variables; draggable 4px splitter bars between left/right top panels (`ImGuiMouseCursor_ResizeEW`) and top/bottom panels (`ImGuiMouseCursor_ResizeNS`); splitters highlight on hover/drag; NBBO bar uses `FlexRow`; Order Entry symbol row uses `FlexRow`; form label column uses `SameLine(em(75))` to scale with font; BUY/SELL and submit buttons scale with `em()`
