# IBKR Trading App

A C++20 desktop trading terminal for Interactive Brokers, built with Dear ImGui (Vulkan backend) and the official IBKR TWS API.

Designed as a high-performance alternative front-end for IB Gateway / TWS, focusing on execution speed, multi-window workflows, and real-time market visualization.

Distributed for research and personal trading use via IBKR accounts. No guarantees of correctness, uptime, or suitability for financial decision-making.

The “Interactive Brokers API Usage Notice” section must be reviewed prior to use. By using this software, users acknowledge and agree to its terms.

## Demonstrated Capabilities

- **Candlestick Charts** — Multi-timeframe OHLCV with SMA, EMA, Bollinger Bands, VWAP, RSI, and volume
- **DOM / Level II** — Live order book ladder with click-to-trade
- **Order Management** — Place, track, and cancel Market, Limit, Stop, Stop-Limit, Trailing, MOC/LOC, MTL, MIT/LIT, Midprice, and Relative orders
- **Market Scanner** — Scan for Top Gainers/Losers, Volume Leaders, 52W Highs/Lows, RSI extremes, and more
- **News Feed** — Real-time and historical news across Market, Portfolio, and per-Stock tabs with sentiment indicators
- **Portfolio Dashboard** — Account summary, positions, equity curve, allocation donut, and performance metrics (Sharpe, Max Drawdown, Alpha, Beta, Win Rate)
- **Orders Blotter** — Live open orders and full execution history with commissions and realized P&L
- **Paper & Live accounts** — Toggle between paper and live trading from the login screen
- **Multi-Instance Windows** — Open up to 10 simultaneous Chart, Order Book, Scanner, and News windows to monitor multiple assets at once
- **Window Groups** — Link any windows into a color-coded group (G1–G4); changing the asset in one window instantly syncs all others in the same group
- **Layout Presets** — One-click workspace layouts: Trading Focus, Research, Full Desk
- **Responsive UI** — All toolbars and info bars wrap gracefully when windows are resized small; font size adjustable (Small / Medium / Large) via the Settings menu
- **Resizable Panels** — Drag the splitter bars inside the Order Book window to resize the DOM ladder, order entry form, and bottom tabs independently

---

## Requirements

### System

| Dependency | Version | Notes |
|---|---|---|
| C++ Compiler | C++20 | GCC 11+, Clang 13+, MSVC 2022+ |
| CMake | 3.20+ | |
| Vulkan SDK | 1.3+ | Must include validation layers and ICD loaders |
| GLFW3 | 3.3+ | System-installed |
| Protobuf | 3.21.x | System-installed (`libprotobuf-dev`) |

### Linux (Debian/Ubuntu)

```bash
sudo apt install libvulkan-dev vulkan-validationlayers \
                 libglfw3-dev libprotobuf-dev protobuf-compiler \
                 cmake build-essential
```

### macOS (Homebrew)

```bash
brew install vulkan-headers molten-vk glfw protobuf cmake
```

### Windows

Install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) and ensure CMake and a C++20 compiler (MSVC or MinGW) are on your PATH. GLFW and Protobuf can be installed via vcpkg.

---

## Install Interactive Brokers API

Download the official API:

https://interactivebrokers.github.io/

Extract it into the project root so that this directory exists:

```
twsapi_macunix.1037.02/IBJts/source/cppclient/client
```

IMPORTANT!!! Don't version this into the repository. Read License agreement.

> The Protobuf-generated files inside the API package were originally generated with Protobuf 3.12 and are incompatible with system Protobuf 3.21. The CMake build regenerates them automatically using `protoc` if the system version is detected.

---

## Build

```bash
# Configure (Release by default)
cmake -B build -S .

# Build (parallel)
cmake --build build -j$(nproc)

# Run
./build/ibkr-trading-app

# --- Variants ---

# Debug build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Clean build
rm -rf build && cmake -B build -S . && cmake --build build
```

### Linux headless / virtual display

If running on a machine without a physical display (e.g., a server or WSL), set a virtual display before launching:

```bash
DISPLAY=:1 ./build/ibkr-trading-app
```

### Platform notes

- **Windows** — MSVC or MinGW; CMake handles Vulkan/ImGui linking automatically
- **Linux** — Ensure ICD loaders are configured (`/etc/vulkan/icd.d/`) and `DISPLAY` is set
- **macOS** — Requires Xcode command line tools; MoltenVK provides Vulkan over Metal

---

## Testing

The test suite uses [Catch2 v3](https://github.com/catchorg/Catch2) and is fetched automatically by CMake. No extra install step needed.

### Run tests locally

```bash
# Configure with tests enabled
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DIBKR_BUILD_TESTS=ON

# Build
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure
```

### Test targets

| Target | What it covers |
|---|---|
| `tests-core` | Pure logic: Timeframe helpers, DST/session classification, model struct defaults, enum string helpers, `ParseStatus`, `ParseIBTime` |
| `tests-ibkr` | IBKRClient message dispatch: inject `IBMessage` variants into the queue, assert callbacks fire correctly — no live IB connection required |

### Sanitizers (Linux)

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DIBKR_BUILD_TESTS=ON -DIBKR_SANITIZE=ON
cmake --build build --target tests-core -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
  ./build/tests/tests-core
```

### CI

All three platform jobs (Linux, macOS, Windows) build and run the full test suite on every push and pull request. A dedicated `sanitize-linux` job runs `tests-core` under AddressSanitizer + UBSanitizer after the main Linux build passes.

> UI rendering (ImGui/Vulkan) and live IB Gateway connectivity are not covered by automated tests — these require a real display and a running IB session.

---

## IB Gateway / TWS Setup

The app connects to either **IB Gateway** or **Trader Workstation (TWS)**. You must enable API access before connecting.

### Enable API in TWS

1. Open TWS → **Edit → Global Configuration → API → Settings**
2. Check **Enable ActiveX and Socket Clients**
3. Set **Socket port** (see table below)
4. Uncheck **Read-Only API** if you want to place orders
5. Add `127.0.0.1` to **Trusted IP Addresses** (or your machine's IP)

### Enable API in IB Gateway

1. Open IB Gateway → **Configure → Settings → API → Settings**
2. Same steps as TWS above

### Port Reference

| Account Type | Application | Port |
|---|---|---|
| Live | TWS | 7496 |
| Paper | TWS | 7497 |
| Live | IB Gateway | 4001 |
| Paper | IB Gateway | 4002 |

### Client ID

Each connection to IB requires a unique **Client ID** (integer). If you connect multiple programs simultaneously, use different Client IDs to avoid conflicts. The app defaults to `1`.

---

## Login Window

On launch, a login dialog appears before the trading UI loads.

| Field | Description |
|---|---|
| **Host** | Hostname or IP of TWS/Gateway (default: `127.0.0.1`) |
| **Port** | Auto-populated when you toggle Account Type and API Type |
| **Client ID** | Unique integer per connection (default: `1`) |
| **API Type** | Toggle between **TWS** and **IB Gateway** (updates default port) |
| **Account** | Toggle between **Live** and **Paper** (updates default port) |

Click **Connect**. The app waits for `nextValidId` from IB (which signals the connection is ready) before showing the trading UI. If connection fails, an error message is displayed with the IB error code.

---

## Windows

The UI uses ImGui's docking system. All windows are dockable and can be rearranged freely.

### Multi-Instance Windows

Chart, Order Book, Scanner, and News windows support up to **10 simultaneous windows instances** each. Open additional instances from **Windows → IBKR → + New Chart / + New Order Book / + New Scanner / + New News**. Each instance has an independent symbol subscription and its own IB reqId range, so they never interfere with each other.

### Window Groups & Symbol Sync

Every window has a **group button** (`G1` / `G2` / `G3` / `G4` / `G-`) at the leftmost position of its toolbar.

- Click the button to assign the window to a group (or clear it with `G-`).
- When you change the asset in any grouped window — by typing a symbol in the chart search box, changing the symbol in the Order Book, or double-clicking a row in the Scanner — **all other windows in the same group immediately switch to that asset** and re-subscribe to live market data.
- Groups are color-coded: G1 = blue, G2 = green, G3 = orange, G4 = purple.
- By default, instance N starts in group N (e.g. Chart 1, Order Book 1, Scanner 1, News 1 all start in G1).

### Layout Presets

The **Presets** menu applies one-click workspace configurations:

| Preset | Windows shown |
|---|---|
| Trading Focus | Chart 1, Order Book 1, Orders |
| Research | Chart 1, Scanner 1, News |
| Full Desk | Chart 1, Order Book 1, News (G2), Scanner (G2), Portfolio, Orders |

### Chart Window

Real-time candlestick charting with technical analysis overlays.

**Timeframes:** 1m, 5m, 15m, 30m, 1h, 4h, 1D, 1W, 1M

**Indicators (toggleable):**
- SMA 20, SMA 50 (periods configurable)
- EMA 20 (period configurable)
- Bollinger Bands (period and sigma configurable)
- VWAP (resets intraday)
- RSI (separate subplot, period configurable)
- Volume subplot with up/down coloring

**Drawing Tools:** Horizontal lines, trendlines, Fibonacci retracements, eraser

**Trading:**
- Place orders directly from the chart (MKT, LMT, STP, STP LMT, TRAIL, TRAIL LIMIT, MOC, LOC, MTL, MIT, LIT, MIDPRICE, REL)
- Working orders displayed as horizontal lines on the price axis
- Current position shown with entry price, current price, and unrealized P&L strip
- **Current price line** — dashed horizontal line tracking the latest price, with a right-aligned price tag inside the chart
- RTH toggle to include or exclude pre/post-market bars

**Session bands:** Chart shades premarket, regular hours, after-hours, and overnight regions.

**Symbol history:** Last 10 symbols are remembered for quick switching.

---

### Trading Window (DOM)

Professional Depth of Market ladder for market microstructure analysis and fast order entry.

**Layout** — Three resizable panels separated by draggable splitter bars:
- **Left**: DOM ladder (drag the vertical splitter to resize)
- **Right**: Order entry form
- **Bottom**: Tabbed panel (drag the horizontal splitter to resize)

**Order Book:**
- Up to 50 bid/ask price levels (Level II)
- Cumulative size and number of orders per level
- Volume-at-price overlay from executed trades

**Interactive order placement (via IBKR API):**
- Click any price level to pre-fill an order at that price
- Select order type: MKT, LMT, STP, STP LMT, TRAIL, TRAIL LIMIT, MOC, LOC, MTL, MIT, LIT, MIDPRICE, REL
- Select time-in-force: DAY, GTC, IOC, FOK
- BUY / SELL buttons confirm submission

**Tabs:**
- **Open Orders** — Working and partially-filled orders with cancel button
- **Execution Log** — Filled orders with commission and realized P&L
- **Time & Sales** — Live tape of last 80 executed trades with uptick/downtick coloring

---

### Settings

Open via **Settings** in the menu bar. A floating panel lets you change the font size:

| Option | Scale |
|---|---|
| Small | 0.85× |
| Medium | 1.0× (default) |
| Large | 1.5× |

All UI elements — text, widgets, padding, and spacing — scale uniformly. The setting takes effect immediately without restarting.

---

### News Window

Multi-source financial news with three tabs. Supports up to **10 simultaneous windows instances**, each independently grouped.

**Market Tab** — Real-time news ticks for major market symbols. Auto-updates as headlines arrive. Highlights breaking news.

**Portfolio Tab** — Historical news filtered to your current positions. Populated automatically when positions load after connection.

**Stock Tab** — Enter any symbol to search historical news archives. Click a headline to load the full article body on demand.

**Features:**
- Sentiment indicator per article (Positive / Negative / Neutral)
- Source attribution (Dow Jones, Briefing.com, etc.)
- Time-ago formatting ("5 min ago", "2 hrs ago")
- Filter by headline text

**Free news providers included:** `BRFUPDN`, `BRFG`, `DJ-N`, `DJNL`, `DJ-RTA`, `DJ-RTE`, `DJ-RTG`, `DJ-RTPRO`

> A market data subscription from IB may be required for some providers. Delayed/free tier still works for many sources.

---

### Scanner Window

Market scanning across stocks, indexes, ETFs, and futures.

**Preset scans:**

| Preset | Description |
|---|---|
| Top Gainers | Largest % gain today |
| Top Losers | Largest % loss today |
| Volume Leaders | Highest share volume |
| New 52W Highs | Stocks at 52-week high |
| New 52W Lows | Stocks at 52-week low |
| RSI Overbought | RSI >= 70 |
| RSI Oversold | RSI <= 30 |
| Near Earnings | Upcoming earnings reports |
| Most Active | Dollar volume leaders |
| Custom | User-defined scan code |

**Filters:** Price range, % change, volume, market cap, RSI range, sector, exchange

**Results table:** 25+ sortable columns including symbol, price, change, volume, PE, EPS, ATR, MACD, 52W distance. Gainers highlighted green, losers red. Portfolio holdings are marked. Mini sparkline chart per row.

Auto-refreshes every 30 seconds (configurable). Falls back to simulated data when IB is not connected (useful for UI testing).

---

### Portfolio Window

Full account and position dashboard.

**Summary cards (top row):**
- Net Liquidation Value
- Cash Balance
- Day P&L ($ and %)
- Total P&L (unrealized + realized)
- Buying Power

**Positions table:** Symbol, quantity, avg cost, current price, market value, cost basis, unrealized P&L ($ and %), realized P&L, day change, portfolio weight. All columns sortable and toggleable.

**Charts:**
- 90-day equity curve (line chart)
- Portfolio allocation donut (by market value, top holdings labeled)

**Bottom tabs:**

- **Trade History** — Closed trades with side, qty, price, commission, realized P&L, and timestamp. Searchable.
- **Performance** — Key metrics: Total Return, YTD, MTD, Day, Sharpe Ratio, Max Drawdown, Win Rate, Avg Win/Loss, Profit Factor, Beta, Alpha, Volatility
- **Risk** — Advanced drawdown analysis and risk metrics

---

### Orders Window

Live order blotter with two tabs.

**Open Tab** — All submitted, working, and partially-filled orders. Shows order type, quantity, limit/stop prices, current fill amount, avg fill price, commission, and a color-coded status badge. Cancel button per order.

**History Tab** — Filled and cancelled orders sorted by execution time (newest first).

---

## Connection Resilience

If IB Gateway or TWS closes unexpectedly while the app is running:

- The trading UI **stays open** with last-known chart data and positions still visible.
- An orange **DISCONNECTED** badge appears to the left of the `[LIVE]` / `[PAPER]` label in the menu bar.
- The app **automatically retries** the connection every 5 seconds in the background.
- When Gateway comes back up, the app reconnects silently and re-subscribes all open chart and order book windows to live data — no need to restart or re-enter credentials.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      main.cpp                           │
│  Vulkan/GLFW init · Login state machine · UI dispatch   │
│  Entry structs (ChartEntry, TradingEntry, ScannerEntry) │
│  BroadcastGroupSymbol · SpawnXxxWindow · WireIBCallbacks│
└────────────────────────┬────────────────────────────────┘
                         │
         ┌───────────────▼───────────────┐
         │         IBKRClient            │
         │  EWrapper + EClientSocket      │
         │                               │
         │  EReader thread               │
         │    └─ IB callbacks            │
         │         └─ push to queue      │
         │                               │
         │  Send thread                  │
         │    └─ PlaceOrder/CancelOrder  │
         │                               │
         │  UI thread (ProcessMessages)  │
         │    └─ drain queue (5ms budget)│
         │         └─ invoke callbacks   │
         └──┬────────────────────────────┘
            │  Callbacks routed by reqId to entry vectors
   ┌────────┼──────────────────────────────────────────────┐
   │        │                  │              │             │
   ▼        ▼         ▼        ▼           ▼             ▼
Chart×10 Trading×10 News×10 Scanner×10 Portfolio     Orders
 (G1-G4)  (G1-G4)  (G1-G4)  (G1-G4)  (singleton) (singleton)
```

**Threading model:**
- The IB EReader runs on its own thread and pushes typed messages (`std::variant`) into a lock-free queue.
- A dedicated send thread handles socket writes for order submission.
- The UI thread drains the queue during `ProcessMessages()` (called once per frame, 5ms budget) and invokes the corresponding `std::function` callbacks that update window state.
- This prevents any IB socket I/O from blocking the render loop.

---

## License

This project is licensed under the MIT License - see the LICENSE file for details.

### Interactive Brokers API Usage Notice

- This application uses the Interactive Brokers (IBKR) Trader Workstation (TWS) API under IBKR’s Non-Commercial License Agreement.

- The software is provided for personal, educational, and research purposes, and for use with the user’s own IBKR account.

- It is not a brokerage service, investment advisory tool, or financial institution, and does not provide investment advice or recommendations.

- Users are solely responsible for any trading activity executed through their IBKR account.

- The application requires a locally running IBKR Trader Workstation (TWS) or IB Gateway instance.

- This project does not redistribute or modify any proprietary IBKR API components.

- This software is not certified for production or mission-critical trading environments. Users should evaluate suitability before live use.

- Use of the IBKR API is subject to IBKR’s own license terms and policies.
