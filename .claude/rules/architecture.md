# Architecture Rules

## Directory Structure

```
├── src/
│   ├── core/
│   │   ├── services/         # IB Gateway integration (IBKRClient.h/.cpp, IBKRUtils.h)
│   │   └── models/           # Data models: MarketData.h, NewsData.h, ScannerData.h,
│   │                         #   PortfolioData.h, OrderData.h, WindowGroup.h
│   ├── ui/
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

Chart, Order Book (Trading), and Scanner support up to 4 simultaneous instances each.
Singleton windows (News, Portfolio, Orders) have one instance.

Entry structs in `main.cpp` hold per-instance state:
```cpp
struct ChartEntry   { ui::ChartWindow* win; int histId, extId, mktId; core::BarSeries pendingBars, pendingExtBars; };
struct TradingEntry { ui::TradingWindow* win; int depthId, mktId; double nbboBid, nbboAsk, ...; };
struct ScannerEntry { ui::ScannerWindow* win; int scanBase, activeScanId, mktBase; ... };
```

Vectors: `g_chartEntries`, `g_tradingEntries`, `g_scannerEntries` (max 4 each).

Spawn helpers: `SpawnChartWindow(idx)`, `SpawnTradingWindow(idx)`, `SpawnScannerWindow(idx)` — create the window, wire all callbacks with `idx` capture, push to the vector.

**ReqId layout (no overlaps):**
- Chart hist: 1,3,5,7 · ext: 2,4,6,8 · mkt: 100-103
- Trading mkt: 110-113 · depth: 120-123
- Scanner scan: 1000,1100,1200,1300 (+99 each) · mkt: 800,812,824,836 (+12 each)
- News: 201(RT), 400-420, 500-520, 600-699, 700-759 · Account: 900

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
- For News window: calls `win->SetSymbol(sym)` → switches to Stock tab

Default group assignment: instance N → group N (e.g. Chart 1 / Order Book 1 / Scanner 1 all start in G1).
Group picker button (`G1`–`G4` / `G-`) is the leftmost item in every window's toolbar.
