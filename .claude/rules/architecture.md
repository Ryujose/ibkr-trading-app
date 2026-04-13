# Architecture Rules

## Directory Structure

```
├── src/
│   ├── core/
│   │   ├── services/         # IB Gateway integration (IBKRClient.h/.cpp, IBKRUtils.h)
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
- `ParseStatus(const std::string&) → core::OrderStatus` — maps IB order-status strings to enum
- `ParseIBTime(const std::string&) → std::time_t` — parses IB date/timestamp formats (YYYYMMDD, Unix string, formatted datetime)

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

`TradingWindow::OnOrderSubmit` callback: `std::function<void(const core::Order&)>`  
`ChartWindow::OnOrderSubmit` callback: unchanged (string-based, basic types only — main.cpp lambda builds a `core::Order` and calls `PlaceOrder`).

## Connection State Machine

`ConnectionState` enum in `main.cpp`:

| State | Meaning |
|---|---|
| `Disconnected` | Not connected — login screen shown |
| `Connecting` | Initial connect in progress |
| `Connected` | Live session — trading UI shown |
| `LostConnection` | Unexpected drop — trading UI stays alive, DISCONNECTED badge shown, auto-reconnect polling |
| `Error` | Initial connect failed — login screen with error message |

**Auto-reconnect** (`StartSilentReconnect()`):
- Polled every frame from main loop when `LostConnection && !g_IBClient`
- Timer: `g_reconnectNextAttempt` (5s interval via `kReconnectIntervalSec`)
- On success (`onConnectionChanged(true)` with `isReconnect=true`): skips `DestroyTradingWindows`/`CreateTradingWindows`, re-subscribes each chart/trading window using `getSymbol()`/`getTimeframe()` accessors
- On failure: schedules next retry in 5s, stays `LostConnection`

**DISCONNECTED badge**: orange background rect + yellow text, left of `[LIVE]`/`[PAPER]` in menu bar.

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
