# Task History

All planned tasks are complete through Phase 5.

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
