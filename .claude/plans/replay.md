# Plan: Replay Window — Pre / Intra / Post-Market Playback (Phase 14)

**Status:** DRAFT — open questions §6.1, §6.3, §6.6, §6.7, §6.9 RESOLVED (2026-05-03 user pass). §6.2 (tick replay) refined to hybrid proposal — awaiting one more pass. Other open questions still using defaults.
**Owner:** Jose
**Goal:** A new `ReplayWindow` (chart-shaped, multi-instance, group-syncable) that
replays a chosen trading day so the user can (1) **review** what they did and
how the day unfolded, or (2) **paper-trade the same day** with simulated fills
against the historical tape — never touching the live IB market — and then
compare actual vs. simulated results, optionally with an AI-generated post-mortem.

This file is the source of truth across sessions — update the **Status** line
and the per-task checkboxes as work progresses.

---

## 1. Critical safety constraint — read first

> **No order placed in any ReplayWindow ever reaches IB.**
> The Replay code path **must not** call `IBKRClient::PlaceOrder`,
> `cancelOrder`, `modifyOrder`, or any other live-mutating API.
> Every paper order goes through `core::services::ReplayEngine` and stays in
> RAM (or a per-day cache file). All on-screen badges, modals, and toolbar
> labels say "PAPER" / "REPLAY" / "SIMULATED" so the user cannot confuse a
> replay session with a live trading session.

A unit test will assert at construction time that `ReplayWindow` holds **no**
pointer or callback to `IBKRClient::PlaceOrder`. The only IB calls it makes
are read-only historical fetches (Task B).

---

## 2. Approved scope (subject to user sign-off)

### 2a. v1 (this phase)

| Capability | v1 |
|---|---|
| Replay sources | Pre-market (04:00–09:30 ET), Intraday (09:30–16:00 ET), Post-market (16:00–20:00 ET), or **All** combined |
| Tape granularity | M1 bars by default; configurable via TF combo (M1/M5/M15/H1) |
| Speed presets | Pause, 0.25×, 1×, 2×, 5×, 20×, 60×, **MAX** (skip per-frame to a target idx) |
| Manual nav | Step forward/back 1 bar; jump to time; scrubber drag |
| Mode | **Analysis** (read-only) or **Operate** (paper trading) toggle |
| Order types | All 13 `core::OrderType` values via simulated execution |
| Trade overlay | Actual fills from that day plotted as triangle markers on the chart, hover tooltip with order details |
| Compare | Actual PnL vs. simulated PnL side-by-side panel (Operate mode) |
| Group sync | Symbol AND time-cursor sync within a window group (CONFIRMED in scope per §6.3) |
| Persistence | Per-window state (date, symbol, session, speed, last cursor) restored on reconnect / app-restart |
| Multi-instance | Up to 10 (matches Chart / Trading / Scanner / News / Watchlist limit) |
| AI button | Placeholder text panel + button stub; real Anthropic API call deferred to Phase 15 |
| Cache | On-disk per-symbol per-day binary cache; one IB historical fetch per (symbol, date, granularity) |

### 2b. Deferred (not in v1, captured here so we don't lose them)

- **True tick-by-tick replay** (`reqHistoricalTicks`) — heavier on data, IB rate limits, and rendering. v1 uses M1 bars + the TF combo for resolution. v2 can add a "Tick" mode.
- **Multi-day backtesting** — replay a week / month / arbitrary range. v1 is single-day to keep the scrubber UX tractable.
- **Anthropic API integration** — the *button* lands in v1 (Task G), the actual API call lands in Phase 15 once we settle on key handling, the HTTP client (libcurl?), and the prompt template.
- **Decision journal export** — emit replay sessions as Markdown / JSON for a separate trading-journal tool. v1 stores in RAM + per-window cache file.
- **Equity curve plot** — small ImPlot overlay showing simulated equity over the replay window. v1 shows scalar PnL only.
- **News / WSH event markers on the chart** — replay-time-correct overlay of news headlines and WSH events. Captured here, see open question §6.5.

### 2c. Possibly-cut (open questions §6 will decide)

- ~~Time-cursor group sync~~ — CONFIRMED in scope (§6.3 resolved).
- Bookmarks / annotations.
- ~~Slippage model beyond zero~~ — CONFIRMED zero in v1 (§6.7 resolved); knob for fixed cents kept as a hidden settings popup option.
- Hybrid bar/tick fill mode — see §6.2.

---

## 3. Architecture

### 3a. Files

| Path | Purpose | Status |
|---|---|---|
| `src/core/services/ReplayEngine.h` | Pure POD/free-function module: `ReplayClock`, `ReplaySession` enum, `SimulatedAccount`, `SimulatedOrderBook`, `EvaluateBar(workingOrders, bar) → fills`, `EvaluateTick(workingOrders, tick) → fills`, `BarRangeForSession(bars, session) → {first, last}` | NEW |
| `src/core/services/ReplayEngine.cpp` | Heavier impls (slippage table, fill-price derivation per OrderType) — header-only is also fine if it stays inline | NEW (maybe) |
| `src/core/services/IBKRClient.h/.cpp` | Add `reqHistoricalTicks`, `reqHistoricalNews`, `cancelHistoricalTicks` wrappers; wire `historicalTicks*()` and `historicalNews*()` EWrapper callbacks → `IBMessage` variants | EDIT |
| `src/core/models/ReplayData.h` | `HistoricalDay { Bar[]; HistoricalTick[]; HistoricalNews[]; vector<core::Fill>; }`; `SimulatedFill { time, side, qty, price, commission, intent }`; `ReplayCursor { idx, t }` | NEW |
| `src/ui/windows/ReplayWindow.h/.cpp` | The window itself — toolbar, chart, scrubber, panels, modes | NEW |
| `src/main.cpp` | `ReplayEntry`, `g_replayEntries`, `SpawnReplayWindow(idx)`, IB callback routing for new reqId range, `replay-windows.cfg` save/load wired into connect/disconnect lifecycle | EDIT |
| `tests/test_replay.cpp` | Catch2 cases for `EvaluateBar`, `EvaluateTick`, `BarRangeForSession`, all OrderType trigger paths, slippage, simulated-account math | NEW |
| `tests/CMakeLists.txt` | Add `test_replay.cpp` to `tests-core` (no IB API dep) | EDIT |
| `~/.config/ibkr-trading-app/replay-cache/<SYMBOL>/<YYYY-MM-DD>/{bars-1m.bin,ticks.bin,news.json,user-fills.json}` | On-disk cache | NEW (created at runtime) |
| `~/.config/ibkr-trading-app/replay-windows.cfg` | Per-window state for restore | NEW |

### 3b. Layered diagram

```
┌─────────────────────────────────────────────────────────────────┐
│  ReplayWindow (UI, ImGui/ImPlot)                                │
│   - Toolbar: date / session / TF / speed / play / scrubber      │
│   - Chart: reuses ChartWindow render helpers                    │
│   - Order entry panel (Operate mode only)                       │
│   - Bottom tabs: Trades / Sim Orders / T&S / News / AI          │
└────────────────────────┬────────────────────────────────────────┘
                         │ no IB calls beyond historical reads
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│  ReplayEngine (pure logic, in core::services)                   │
│   - ReplayClock: paused?, speed, currentBarIdx, scrub state     │
│   - SimulatedOrderBook: vector<core::Order> working orders      │
│   - SimulatedAccount: cash, positions, fills                    │
│   - EvaluateBar/Tick: fills from price-action, no I/O           │
└─────────┬───────────────────────────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────────────┐
│  HistoricalDay (data model)                                     │
│   - bars (M1)  - ticks (optional, deferred)                     │
│   - news       - WSH events  - user's actual fills              │
└─────────┬───────────────────────────────────────────────────────┘
          │ filled by:
          ▼
┌─────────────────────────────────────────────────────────────────┐
│  IBKRClient (READ-ONLY for replay)                              │
│   - reqHistoricalData (already exists)                          │
│   - reqHistoricalTicks (NEW)                                    │
│   - reqHistoricalNews  (NEW, gated on news-provider sub)        │
│   + on-disk cache hit short-circuits the IB call                │
└─────────────────────────────────────────────────────────────────┘
```

### 3c. ReqId layout for replay windows

Existing allocations per `architecture.md`:
- P&L account-wide: 9000; P&L single per-position: 9001–9999

Replay needs ranges per instance for: bars (initial fetch), bar-extend, ticks, news. 10 instances × ~10 reqIds each = 100 reqIds.

**Proposed: 11000–11999.**
- Per instance N (0–9): base = 11000 + N × 100
  - bars initial: base + 0
  - bars extend (rare; once per window per session): base + 1
  - ticks: base + 2
  - news: base + 3
  - user fills (queryable from `reqExecutions` already on 8001 — no new id needed; replay just filters the existing g_executions cache by date + symbol)
  - WSH events: base + 4 (re-uses existing `reqWshEventData` plumbing)
  - Reserved: base + 5 .. base + 99

No overlaps with existing layout (P&L is 9000–9999, watchlist mkt data is 7000–7999, smart components 8050–8059, display group 8060–8064, WSH 8010/8020–8029/8070–8199).

### 3d. Lifecycle

```
SpawnReplayWindow(idx):
  - allocate ReplayEntry { window, baseReqId = 11000 + idx*100, day, engine }
  - wire OnDataRequest(symbol, date, session, tf) → main.cpp
                                                  → cache-check → IB fetch if miss
                                                  → on bars-end: push into entry.day.bars → window.SetDay(day)
  - wire OnPlaceOrder(order) → engine.workingOrders.push_back(order)  (NOT IBKRClient!)
  - wire OnCancelOrder(id) → engine.workingOrders.erase(id)
  - default group = idx + 1

per frame in RenderTradingUI:
  - if entry.day loaded and not paused:
      advance entry.engine.cursor by (deltaTime × speed × secondsPerBar)
      while cursor crosses next bar: engine.EvaluateBar(...) → fills appended → window pushes redraw
  - window.Render()
```

---

## 4. Data model

### 4a. `core::services::ReplaySession`

```cpp
enum class ReplaySession : int {
    PreMarket = 0,    // 04:00–09:30 ET
    Intraday  = 1,    // 09:30–16:00 ET
    PostMarket= 2,    // 16:00–20:00 ET
    All       = 3     // 04:00–20:00 ET
};
const char* ReplaySessionLabel(ReplaySession);
const char* ReplaySessionShort(ReplaySession);
```

Open-question §6.10 covers whether to use ET-fixed boundaries or pull session times from the contract details (some exchanges run different hours).

### 4b. `core::services::ReplayClock`

```cpp
struct ReplayClock {
    int    cursorBarIdx = 0;     // index into HistoricalDay::bars
    double cursorSeconds = 0.0;  // fractional seconds within the current bar (for sub-bar interpolation)
    double speed = 1.0;          // 0.0 = paused; 0.25 / 1 / 2 / 5 / 20 / 60; INFINITY = MAX
    bool   paused = true;        // play/pause toggle
    bool   scrubbing = false;    // user is dragging the scrubber — skip auto-advance this frame
    int    sessionFirstIdx = 0;  // start of replayable range (PreMarket/Intraday/PostMarket/All)
    int    sessionLastIdx  = 0;  // end of replayable range
};
void Tick(ReplayClock&, double deltaSec, int barSec);  // advances cursorBarIdx by speed*deltaSec/barSec (no-op if paused or scrubbing)
void StepBars(ReplayClock&, int delta);                 // jump ±N bars, clamped to session range
void SeekToBar(ReplayClock&, int idx);                  // absolute scrub
void SeekToTime(ReplayClock&, std::time_t t, const std::vector<Bar>& bars);  // binary search
```

### 4c. `core::services::SimulatedAccount`

```cpp
struct SimulatedPosition { long conId; std::string symbol; double qty, avgCost; };
struct SimulatedFill {
    std::time_t time;
    std::string symbol;
    core::OrderSide side;
    double qty, price, commission;
    int    intentOrderId;       // local id of the order that produced this fill
    std::string intentNote;     // "stop trigger", "limit fill", "market open", etc.
};
struct SimulatedAccount {
    double startingCash = 100000.0;  // user-editable per window via toolbar input (§6.6); persisted in replay-windows.cfg
    double cash = 100000.0;          // initialized to startingCash on Reset
    std::unordered_map<std::string, SimulatedPosition> positions;
    std::vector<SimulatedFill> fills;
    double realizedPnL  = 0.0;
    double commissionPaid = 0.0;
};
double Equity(const SimulatedAccount&, double lastPrice);  // cash + sum(qty * lastPrice for positions)
double UnrealizedPnL(const SimulatedAccount&, double lastPrice);
void   ApplyFill(SimulatedAccount&, const SimulatedFill&);  // updates cash, positions, realizedPnL
```

### 4d. `core::services::SimulatedOrderBook`

```cpp
struct WorkingOrder {
    int          localId;        // 1, 2, 3, ... per replay session
    core::Order  order;          // full intent (type, qty, prices, TIF, outsideRth)
    std::time_t  placedAt;       // simulated time when the order was placed (cursor's bar time)
};
using SimulatedOrderBook = std::vector<WorkingOrder>;

// Per-bar evaluator: walks working orders, checks each for trigger/fill against bar OHLC, returns fills + the orders that filled (which the caller removes)
struct EvaluateResult {
    std::vector<SimulatedFill> fills;
    std::vector<int> filledIds;   // localIds to erase from book
};
EvaluateResult EvaluateBar(const SimulatedOrderBook&, const Bar&, double commissionPerShare = 0.005);

// Per-tick evaluator (deferred; signature shown for forward compat)
EvaluateResult EvaluateTick(const SimulatedOrderBook&, const HistoricalTick&, double commissionPerShare = 0.005);
```

### 4e. Fill-price derivation per OrderType (per-bar resolution)

| OrderType | Trigger condition | Fill price |
|---|---|---|
| Market | always (bar after placement) | `bar.open` |
| Limit BUY | `bar.low <= limitPrice` | `min(bar.open, limitPrice)` |
| Limit SELL | `bar.high >= limitPrice` | `max(bar.open, limitPrice)` |
| Stop BUY | `bar.high >= stopPrice` | `max(bar.open, stopPrice)` (worst-case) |
| Stop SELL | `bar.low <= stopPrice` | `min(bar.open, stopPrice)` |
| StopLimit BUY | stop hit AND `bar.low <= limitPrice` after | as Limit BUY using limitPrice |
| StopLimit SELL | stop hit AND `bar.high >= limitPrice` after | as Limit SELL using limitPrice |
| Trail (BUY) | high-water of `bar.low - trailAmount` triggers when `bar.high >= trail` | `bar.open` |
| Trail (SELL) | mirror with low-water of `bar.high + trailAmount` | `bar.open` |
| TrailLimit | as Trail then as Limit using `lmtPriceOffset` | per-leg |
| MOC | always at session-close bar | `bar.close` of last bar in Intraday |
| LOC | as Limit but only on session-close bar | `bar.close` if condition met |
| MTL | always (treats as market with last-price snap) | `bar.close` of placement-bar's next bar |
| MIT BUY | `bar.high >= auxPrice` | `bar.open` (then Market behaviour) |
| MIT SELL | `bar.low <= auxPrice` | `bar.open` |
| LIT BUY | trigger as MIT then as Limit using `limitPrice` | per-leg |
| LIT SELL | mirror | per-leg |
| Midprice | always (replay treats mid as `(bar.high + bar.low)/2` capped at `limitPrice`) | derived |
| Relative | always (replay treats peg as `bar.open ± auxPrice`) | derived |

**Caveats baked into v1:**
- Single-bar resolution means a bar that pierces the limit and reverts in the same bar is counted as filled (optimistic). Open question §6.7 covers a "pessimistic mode" toggle that requires the close to confirm.
- TIF DAY orders auto-cancel at the end of Intraday (last bar of `09:30–16:00`).
- `outsideRth=false` orders only fill on Intraday bars.
- Slippage = 0 in v1 (open question §6.7).
- Commission = `core::services::ReplayEngine::kDefaultCommissionPerShare = 0.005`, configurable per window.

### 4f. `core::models::ReplayData::HistoricalDay`

```cpp
struct HistoricalDay {
    std::string symbol;
    std::string date;            // "YYYY-MM-DD"
    std::vector<core::Bar>           bars;     // M1 by default
    std::vector<HistoricalTick>      ticks;    // empty in v1
    std::vector<core::NewsItem>      news;     // empty if no news sub
    std::vector<core::WshEvent>      wsh;      // events that day
    std::vector<core::Fill>          userFills; // from g_executions filtered by date+symbol
    std::time_t fetchedAt = 0;   // for cache invalidation
};
```

### 4g. On-disk cache format

`~/.config/ibkr-trading-app/replay-cache/<SYMBOL>/<YYYY-MM-DD>/`:
- `bars-1m.bin` — packed `[count: int32][Bar × count]`. `Bar` is POD, fixed-size. Atomic write.
- `ticks.bin` — same packed pattern. Empty file if no ticks fetched.
- `news.json` — JSON array of news items (text-friendly for debugging, low volume).
- `wsh.json` — JSON array of WSH events that day.
- `user-fills.json` — JSON array of `core::Fill` from `g_executions` snapshotted at fetch time.

Cache hit policy: any file's `mtime` within the last 24h after market close → use it; otherwise re-fetch. (Today's date is always re-fetched until the close + 1h to absorb late prints.)

---

## 5. UI layout

### 5a. Window shape

```
┌──────────────────────────────────────────────────────────────────────┐
│ [G1] [Symbol▼] [Date▼] [PreMkt|Intra|PostMkt|All] [TF▼]              │
│ [Pause] [⏪ Step] [▶/⏸ Play] [Step ⏩] [Speed▼] [PAPER · ANALYSIS▼]   │
│ [────────────────────────────●────────────────────────] 11:32:00 ET  │ ← scrubber
├───────────────────────────────────────────────┬──────────────────────┤
│ Chart area (reuses ChartWindow render helpers │ Order Entry          │
│  with auto-analysis re-running per bar):      │  (only in            │
│   - Candlesticks                              │   OPERATE mode)      │
│   - VWAP / ±σ                                 │  All 13 OrderTypes   │
│   - Auto S/R (recomputed at cursor)           │  Submit → engine     │
│   - Setup overlay (live)                      │  PAPER badge         │
│   - User fill markers (▲ green BUY,           │                      │
│     ▼ red SELL) at their original time        │ "Use suggestion"     │
│   - Sim fill markers (◇ blue) at their        │  hooks the same      │
│     simulated time                            │  setup overlay logic │
│                                               │                      │
├───────────────────────────────────────────────┴──────────────────────┤
│ Tabs: [Actual Trades] [Simulated Orders] [T&S] [News & Events]       │
│       [AI Analysis]                                                  │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ chronological list, columns vary per tab; double-click → scrub │  │
│  │ to that timestamp                                              │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

### 5b. Toolbar widgets (FlexRow)

| Widget | Width hint | Notes |
|---|---|---|
| Group picker | em(28) | Existing `DrawGroupPicker` |
| Symbol input | em(85) | `SymbolSearch.h` `DrawSymbolInput` for autocomplete |
| Date picker | em(105) | New compact YYYY-MM-DD popup; reuse `DrawDatePicker` from `WshCalendarWindow` |
| Session combo | em(80) | PreMkt / Intra / PostMkt / All |
| TF combo | em(60) | M1 / M5 / M15 / H1 — replay's own TF, independent of TradingStyle |
| Pause btn | em(28) | shortcut: Space |
| Step back | em(28) | shortcut: ← |
| Play/Pause | em(28) | shortcut: Space |
| Step fwd | em(28) | shortcut: → |
| Speed combo | em(70) | 0.25× / 1× / 2× / 5× / 20× / 60× / MAX |
| Mode combo | em(110) | ANALYSIS / OPERATE — colored badge |
| Equity input | em(75) | `InputDouble` for `startingCash` (§6.6 resolved); shown only in OPERATE mode; default 100000, persisted per window |
| Tick-fills toggle | em(75) | "Tick fills" checkbox per §6.2 hybrid; OFF default; first-toggle-on triggers background tick fetch with progress bar |
| Reset btn | em(40) | clears engine, rewinds cursor to session first idx, restores cash to startingCash |
| `[PAPER]` badge | em(50) | always visible, orange background, never hideable |

### 5c. Scrubber

Full-width `ImGui::SliderInt(##scrub, &cursorIdx, sessionFirst, sessionLast)` with a custom format showing `HH:MM:SS ET` (derived from `bars[cursorIdx].time`). Drag → `clock.scrubbing = true`, on release → `false`. Tick-marks at session boundaries (Pre/Intra/Post). Bookmarks (open question §6.4) would render as colored notches on this slider.

### 5d. Mode badge

- ANALYSIS: blue background `(0.10, 0.30, 0.55, 0.95)`, white text. Order entry panel hidden. Submit buttons inside any popup are disabled.
- OPERATE: orange background `(0.55, 0.30, 0.05, 0.95)`, white text. Order entry panel visible. PAPER label appears on every confirm modal.

Switching ANALYSIS → OPERATE rewinds the engine and clears simulated state (with a confirm popup if any sim fills exist). Switching OPERATE → ANALYSIS keeps the simulated state visible but hides the order entry.

### 5e. Bottom-tab panels

| Tab | Content |
|---|---|
| **Actual Trades** | Table of `userFills` for this date+symbol; columns: Time, Side, Type, Qty, Price, Comm, Total. Click row → scrub to that bar. Right-click → "Mark as missed" (open question §6.4). |
| **Simulated Orders** | Working + filled simulated orders, with cancel button on working ones. Columns: LocalId, Time, Side, Type, Qty, Price/Trigger, Status, Sim Fill Px. |
| **T&S** | If ticks loaded, the Time & Sales tape from Task #43 reused. Else placeholder. |
| **News & Events** | News items + WSH events sorted by time; click → scrub. Open question §6.5 covers also rendering them as vertical markers on the chart. |
| **AI Analysis** | Big read-only text area + `[Analyze with AI]` button. v1: button shows a hardcoded "Phase 15: configure ANTHROPIC_API_KEY in settings.json to enable." message. |

### 5f. Status bar (single line above the bottom tabs)

```
Cursor: 11:32:00  |  Bar 132/390  |  Sim Equity: $100,425.50 (+0.43%)  |  Real Equity (this day): +$612.20  |  Δ: -$186.70
```

---

## 6. Open questions for the user

These need a yes/no/decide before any code lands. Default values are my pick, override anything you don't like.

1. **Multi-day backtesting?** — **RESOLVED 2026-05-03: single-day only in v1. Multi-day deferred to Phase 15+.**

2. **True tick-by-tick replay (`reqHistoricalTicks`)?** — **REFINED to hybrid proposal, awaiting final pass.**

   User feedback: "Tick by tick replay would be more realistic for all datetime frames doesn't it?" — Yes, with a cost: IB's `reqHistoricalTicks` returns 1000 ticks/call, paced at ≤60 req/10min/contract. A liquid stock day = 50–500k ticks → first-time fetch can take **60–90 minutes**. Subsequent opens are cache-hit (instant). Cache size ~50–100MB per liquid-stock day.

   **Hybrid proposal (recommended for v1):**
   - **Chart rendering** stays at M1 (or whatever TF is selected) — cheap, fast, no rate-limit hit.
   - **Fill evaluator** uses ticks *when available*. The `EvaluateBar` and `EvaluateTick` paths both exist in the engine (Task A); the window picks which based on a per-session toggle.
   - **Per-session toggle**: "Tick-resolution fills" checkbox in the toolbar, default OFF. When ON for the first time on a `(symbol, date)`, fetch ticks in background with a progress bar; replay auto-pauses until done. After cache hit, near-instant.
   - **Why hybrid wins:** Correct fills (which limit hit first, stop spike-through, trail stops accurate, MIT/LIT trigger ambiguity resolved) without paying the 60+min fetch cost on every session. Bar mode stays the snappy default for review-only / swing replay where tick fidelity is overkill. Scalpers and day-traders flip the toggle for the days they actually want to dissect.
   - **Trade-off accepted:** Engine complexity ~2× (two evaluator code paths). Mitigated by sharing the trigger-detection logic (per-OrderType "what triggers this?") between bar and tick paths; only the fill-price derivation differs.

   **Awaiting user confirmation:** ship hybrid in v1, OR commit to bar-only and add tick mode in Phase 15? My pick = hybrid in v1.

3. **Time-cursor sync within a window group?** — **RESOLVED 2026-05-03: YES. Symbol sync + time-cursor sync across ReplayWindows in the same group. Snap-to-nearest-bar when symbols have different bar counts/gaps. Chart and Trading windows NOT affected by replay cursor.**

4. **Bookmarks / annotations / per-bar notes?**
   Right-click a bar → "Add note" → small text input → renders a flag marker on the chart and on the scrubber. Persists in the replay-windows.cfg. **Default: yes for v1, low-effort lift, big workflow win.**

5. **News / WSH overlay on the chart?**
   Vertical dashed markers at the timestamp, hover → headline. We already do WSH event markers in Task #44. **Default: yes for WSH (free, already built), news markers gated on whether `historicalNews` is populated.**

6. **Equity start: real account NetLiq, or fixed virtual?** — **RESOLVED 2026-05-03: $100,000 default, user-editable via input next to mode combo. The "Reset" button restores to whatever value the user last entered (per-window persisted, not hardcoded $100k after first edit).**

7. **Slippage model?** — **RESOLVED 2026-05-03: zero in v1.** The fixed-cents knob and pessimistic close-confirm checkbox stay accessible behind a "Realism" settings popup but default OFF.

8. **Compare view: inline panel or separate window?**
   Inline = always on in OPERATE mode, takes ~30% of bottom tab area. Separate = `[Open Compare]` button spawns a side-by-side window. **Default: inline panel as a tab (`[Compare]`) added in OPERATE mode only — same window, no extra dockable surface.**

9. **AI button — placeholder only, or wire the API now?** — **RESOLVED 2026-05-03: placeholder in v1, full integration as Phase 15. Multi-provider scope locked in: Anthropic, OpenAI, Deepseek.**

   Implications for v1 placeholder (Task F) so Phase 15 doesn't have to refactor:
   - Button label `[Analyze with AI ▼]` opens a provider picker dropdown (Anthropic / OpenAI / Deepseek). v1: all three options are present but greyed-out with "Configure API key in settings — Phase 15"; the visual surface is the same as v2 will use.
   - The `[Copy day summary to clipboard]` button (the actually-useful v1 placeholder) emits provider-neutral Markdown that works as a prompt for any of the three.
   - Settings panel needs three new password-masked input fields for the three API keys, persisted to `~/.config/ibkr-trading-app/api-keys.cfg` (mode 0600, gitignored). The fields render in v1 but no provider is called yet.
   - Phase 15 plan must cover: HTTP client choice (libcurl is the standard fit; would become a new dep used by all three providers), per-provider request/response schemas, streaming vs batch, token-cost surfacing in the UI, error/rate-limit handling.

10. **Session boundary times — ET fixed or contract-driven?**
    Current candidate: fixed ET (PreMkt 04:00–09:30, Intra 09:30–16:00, PostMkt 16:00–20:00). Contract-driven would query `reqContractDetails` for `tradingHours` and parse. **Default: ET fixed in v1. Futures and FX traders can override per-window in a "custom session" popup later.**

11. **Operate mode: single-symbol or multi-symbol simulated portfolio?**
    Single = each ReplayWindow has its own SimulatedAccount, no cross-symbol portfolio view. Multi = one shared SimulatedAccount across all OPERATE-mode replay windows in the same group. **Default: per-window in v1 (simpler, no shared state). v2 can add a `g_replayPortfolio` aggregate if useful.**

12. **What counts as "today's trades" for the userFills overlay?**
    Source = `g_executions` (already populated by `ReqExecutions` on connect). Filter = `(symbol == replay.symbol) && (date(fill.time) == replay.date)`. Edge case: what about orders placed today on a symbol the user *also* paper-traded yesterday in replay — should the chart show both? **Default: show only `g_executions` (real fills) in Analysis mode; in Operate mode, show real fills as triangles AND simulated fills as diamonds, color-distinguished.**

13. **What if the user tries to replay a date with no fills and no historical bars?** 
    e.g. weekends, holidays. **Default: show "No data for YYYY-MM-DD (market closed)" placeholder in the chart area; date picker greys out non-trading days using the `core::services::IsUSDST` helper + a hardcoded US holiday calendar (we don't have one yet — open question 13a).**
    - **13a.** Should we ship a hardcoded US holiday list in `core::services` (covers ~10 dates/year), or let the user click and discover? **Default: ship the list, low effort, hides bad dates from the picker.**

14. **Persistence: how much state restores after reconnect?**
    Cheap to restore: symbol, date, session, TF, speed, last cursor, mode. Expensive to restore: simulated working orders + sim fills (would need to serialize SimulatedAccount). **Default: only the cheap state persists in v1. Reconnect re-fetches the day from cache (or IB) and starts the engine fresh. Sim state is in-RAM only.**

---

## 7. Tasks

### Task A — `core::services::ReplayEngine` pure-logic core + tests

**Files:** `src/core/services/ReplayEngine.h` (new), `tests/test_replay.cpp` (new), `tests/CMakeLists.txt` (edit).

Implement the structs, enums, and free functions in §4 as inline code in `ReplayEngine.h` (header-only, mirroring `ChartAnalysis.h` / `TradingStyle.h` style). Catch2 cases under a new `[replay]` tag:

- `ReplaySessionLabel` / `ReplaySessionShort` cover all four values.
- `Tick(clock, dt, barSec)`: speed=0 / paused → no advance; speed=1 + dt=60 + barSec=60 → +1 bar; speed=5 + dt=60 + barSec=60 → +5 bars; scrubbing flag respected; clamp at sessionLastIdx.
- `StepBars` / `SeekToBar`: clamp respected, no underflow.
- `SeekToTime`: binary search on a synthetic bar array, exact match and between-bars cases.
- `BarRangeForSession`: synthetic 24h bar array, each session enum picks the correct first/last idx; All=full range; PreMkt with no pre-market bars → `{first==last}` empty range.
- `EvaluateBar`: one case per OrderType from §4e covering BUY and SELL paths and the "no trigger" no-op case; assert each fill's price matches the per-row formula; assert the localId of the filled order is in `filledIds`.
- `EvaluateTick`: per §6.2 hybrid, the tick path must exist in v1 (even if Task C wires it behind the toggle later). One case per OrderType + tick-type combination that matters: TRADES tick triggers Stop/Market/MIT; BID_ASK tick triggers Limit (limit BUY checks ask; limit SELL checks bid); MIDPOINT for Midprice. Assert no double-fill if the same order already filled on a prior tick this session.
- `EvaluateBar` ordering: when multiple orders trigger on the same bar, the result is deterministic (sorted by localId). Same invariant for `EvaluateTick`.
- `SimulatedAccount` math: `ApplyFill` for BUY (cash decreases, position grows, avgCost recomputed), SELL closing (cash increases, position shrinks, realizedPnL credited), SELL flipping (position flips sign, avgCost reset).
- `Equity` / `UnrealizedPnL` on a populated account.
- `Reset(account, startingCash)`: cash → startingCash, positions cleared, fills cleared, realizedPnL=0, commissionPaid=0. Verifies §6.6 user-editable starting equity flows through.
- TIF DAY auto-cancel at session-close logic (`EvaluateBar` and `EvaluateTick` on the last bar/tick drop outstanding DAY orders).
- Group-time-sync helper `SnapCursorToNearestBar(targetTime, bars) → int idx` (per §6.3): binary search returns the bar whose start time is closest to `targetTime`; tie-break to the earlier bar; out-of-range clamps to first/last.

Coverage target: ≥55 cases / ≥320 assertions under `[replay]` (bumped from initial 40/250 to cover the tick path and the snap helper).

### Task B — Historical data ingest in IBKRClient

**Files:** `src/core/services/IBKRClient.h/.cpp` (edit), `tests/test_ibkr_queue.cpp` (edit — dispatch tests for the new IBMessage variants).

Add IBMessage variants:
- `MsgHistoricalTick { int reqId; std::vector<HistoricalTick> ticks; bool done; }`
- `MsgHistoricalNews { int reqId; std::string time, providerCode, articleId, headline; }`
- `MsgHistoricalNewsEnd { int reqId; bool hasMore; }`

Wire EWrapper callbacks `historicalTicks`, `historicalTicksBidAsk`, `historicalTicksLast`, `historicalNews`, `historicalNewsEnd`. Public methods:
- `void reqHistoricalTicks(int reqId, const Contract&, const std::string& start, const std::string& end, int count, const std::string& whatToShow, bool useRTH, bool ignoreSize)`
- `void reqHistoricalNews(int reqId, int conId, const std::string& providerCodes, const std::string& start, const std::string& end, int totalResults)`

Cache layer in `main.cpp`:
- `std::filesystem::path ReplayCachePath(symbol, date)` returns `~/.config/ibkr-trading-app/replay-cache/<SYMBOL>/<YYYY-MM-DD>/`.
- `bool LoadCachedDay(symbol, date, HistoricalDay&)` returns true if cache hit + populates day.
- `void SaveCachedDay(const HistoricalDay&)` atomic-writes each component file.
- `void FetchHistoricalDay(replayEntryIdx, symbol, date, ReplaySession, Timeframe, std::function<void(HistoricalDay&&)> done)` — checks cache first, otherwise issues `reqHistoricalData` (and `reqHistoricalNews` if news provider is subscribed and §6.5 says yes), aggregates results in the entry's pending buffers, fires `done()` on `historicalDataEnd`.

Tests for the new IBMessage variants in `test_ibkr_queue.cpp`: inject a synthetic message, verify the registered callback fires with the correct payload (mirroring existing dispatch tests for other variants).

### Task C — `ReplayWindow` shell

**Files:** `src/ui/windows/ReplayWindow.h` (new), `src/ui/windows/ReplayWindow.cpp` (new), `src/main.cpp` (edit).

Window class:
- Constructor takes the same callbacks Chart/TradingWindow use (`OnDataRequest`, group-picker hookup, etc.) plus `OnPaperOrderSubmit(const core::Order&) → int localId`, `OnPaperOrderCancel(int localId)`.
- Members: `m_symbol`, `m_date`, `m_session`, `m_tf`, `m_clock`, `m_engine` (SimulatedOrderBook + SimulatedAccount), `m_day` (HistoricalDay), `m_mode` (Analysis / Operate), `m_groupId`, `m_loading`, `m_lastError`.
- `Render()` orchestrates: `DrawToolbar()` → `DrawScrubber()` → `DrawChart()` → `DrawOrderEntry()` (Operate only) → `DrawBottomTabs()` → `DrawStatusBar()`.
- `DrawChart()`: candlestick rendering reuses `ChartWindow` helpers — refactor needed (open question: pull ChartWindow's chart-drawing helpers into a free namespace `ui::charts` so both windows share them, OR have ReplayWindow embed a minimal subset). **Default: pull a `RenderCandlestickPlot(bars, idxRange, indicators, overlays)` free function out of ChartWindow.cpp and call it from both. This is a refactor task on its own — see Task C.5.**
- `DrawScrubber()`: full-width `SliderInt`, custom format showing time, drag toggles `clock.scrubbing`.
- `DrawBottomTabs()`: `BeginTabBar` with the five tabs from §5e.

main.cpp:
- `struct ReplayEntry { ui::ReplayWindow* win; int baseReqId; HistoricalDay day; std::vector<int> pendingReqIds; };`
- `static std::vector<ReplayEntry> g_replayEntries;` (capacity 10).
- `void SpawnReplayWindow(int idx)` mirrors `SpawnChartWindow`, allocates baseReqId = 11000 + idx*100, wires `OnDataRequest` to `FetchHistoricalDay`, wires `OnPaperOrderSubmit` to `g_replayEntries[idx].engine.workingOrders.push_back(...)`.
- IB callback router in `onHistoricalDataEnd` etc. checks if `reqId` falls in 11000–11999, routes to the matching replay entry's pending buffer.
- Add to `Windows → IBKR → Replay` submenu with "+ New" button (multi-instance pattern).
- `RenderTradingUI()` iterates `g_replayEntries` to advance clocks, run `EvaluateBar` on cursor crossings, then call `Render()`.

#### Task C.5 — Chart-render extraction (sub-task that gates Task C)

Pull `ChartWindow::DrawCandleChart` (and its helpers `DrawOverlays`, `DrawAutoSupportResistance`, etc.) into either:
- (a) static free functions in a new `ui/ChartRender.h` taking everything they need by reference, OR
- (b) a shared base class `ChartViewBase` that both `ChartWindow` and `ReplayWindow` derive from.

**Default: option (a)** — header-only, no inheritance, simpler test surface. Refactor lands as a no-op ChartWindow change (it just becomes a thin wrapper over the new free functions), then ReplayWindow uses the same functions.

### Task D — Simulated execution engine wiring

**Files:** `src/ui/windows/ReplayWindow.cpp` (edit — order entry + per-frame engine tick), `src/main.cpp` (edit — group-time-sync broadcast).

- `DrawOrderEntry()` is a literal copy of `TradingWindow::DrawOrderEntry()` minus the IB-call branches. All 13 OrderTypes, conditional fields, validation, confirmation modal — but the modal's "Submit" branch calls `OnPaperOrderSubmit(order)` instead of `IBKRClient::PlaceOrder`.
- `Render()` hooks the per-frame tick: at the top of `Render()` (before drawing anything), if `!m_clock.paused && !m_clock.scrubbing`, advance the clock; then call `EvaluateBar` (or `EvaluateTick` if `m_tickFills` is on per §6.2) for each newly-crossed bar/tick; apply each fill via `ApplyFill` and append to `m_engine.account.fills`.
- Status-bar PnL: derived from `Equity(m_engine.account, lastPrice) - m_engine.account.startingCash` (uses §6.6 user-edited starting equity, not hardcoded 100k).
- DOM ladder NOT shown in v1 (no L2 in historical data); the order entry panel just has the form.
- **Group-time-sync (§6.3 resolved)**: `main.cpp` adds `void BroadcastReplayCursor(int groupId, std::time_t cursorTime, int sourceIdx)` guarded by `g_replayCursorSyncInProgress` (mirrors `g_groupSyncInProgress`). Called whenever any ReplayWindow advances its cursor (auto-tick or manual scrub). For each *other* ReplayWindow in the same group, calls `win->SeekToTime(cursorTime)` which uses `SnapCursorToNearestBar` (Task A). Throttle: at most once per 100ms per group to avoid CPU spike during MAX-speed playback.
- **Tick-fills toggle (§6.2 hybrid)**: when user flips the "Tick fills" checkbox ON for the first time on a `(symbol, date)`, fire `FetchHistoricalDayTicks(replayEntryIdx, ...)` from main.cpp; window enters a "fetching ticks…" overlay state with progress bar; auto-pauses the clock; on completion, ticks land in `m_day.ticks` and the engine evaluator switches to the tick path.

Acceptance:
- Drop a Limit BUY @ 184.50 on AAPL replay for 2026-04-15, hit Play; when a bar's low ≤ 184.50, a fill appears in the Simulated Orders tab and the position shows in the status bar.
- Open two ReplayWindows in G1 (AAPL + SPY, same date), scrub one — the other's cursor follows to the nearest matching bar.
- Toggle "Tick fills" ON for AAPL day → progress bar shows tick fetch → after fetch, place a tight-spread Limit and verify it fills at the bid/ask tick that touched it (not the bar's open as in bar mode).

### Task E — Analysis-mode features

**Files:** `src/ui/windows/ReplayWindow.cpp` (edit).

- User-fill overlay: `DrawChart()` plots triangle markers (BUY = green up at fill price + bar idx, SELL = red down) for each entry in `m_day.userFills`. Hover tooltip shows full order details.
- Sim-fill overlay: same but with diamond markers (◇ blue) for `m_engine.account.fills`.
- "Actual Trades" tab populates from `m_day.userFills`; double-click row → `SeekToTime` on the clock so the chart jumps to that moment.
- "Missed setups" detection: walk `m_day.bars`, run the auto-analysis pipeline at each bar (using existing `core::services::ChartAnalysis` helpers), record bars where `ComputeBreakoutSignal` would have emitted a signal AND no userFill exists within ±5 bars of that signal. Display in the Analysis panel as a count + list with timestamps.
- Stats: real PnL (sum of userFills' realized), sim PnL (Engine), win rate, R-multiple distribution.

### Task F — AI analysis panel (placeholder, multi-provider-shaped)

**Files:** `src/ui/windows/ReplayWindow.cpp` (edit), `src/main.cpp` (edit — settings panel for API keys).

Provider scope confirmed in §6.9: Anthropic, OpenAI, Deepseek. Phase 15 lands the actual API calls; v1 ships the visual surface so Phase 15 is plug-in only.

- New tab "AI Analysis" with a multi-line read-only `InputTextMultiline` (resizable).
- `[Analyze with AI ▼]` button — clicking opens a popup with three provider rows (Anthropic / OpenAI / Deepseek). v1: all three rows are greyed-out with "Configure API key in Settings — Phase 15".
- `[Copy day summary to clipboard]` button — actually useful in v1. Emits provider-neutral Markdown summary (date, symbol, bar count, OHLC range, VWAP/±σ stats, real fills with PnL breakdown, sim fills with PnL breakdown, missed setups list with timestamps, news headlines for the day, WSH events for the day, current auto-analysis snapshot at cursor). The Markdown is structured so it works as-is as a prompt for any of the three providers in Phase 15.
- Settings panel (in `RenderSettingsWindow()` in `main.cpp`) gains an "AI Providers" section with three password-masked `InputText` fields (Anthropic / OpenAI / Deepseek), persisted to `~/.config/ibkr-trading-app/api-keys.cfg` with file mode `0600`. Path added to `.gitignore` if not already covered. v1 just stores them; v2 (Phase 15) reads them when the user picks a provider.
- The text area is read-only (so v1 displays the Phase 15 placeholder + the copy-to-clipboard hint) and selectable so the user can paste their own AI's response back if they want a permanent record.

### Task G — Persistence & multi-instance restore

**Files:** `src/main.cpp` (edit).

`~/.config/ibkr-trading-app/replay-windows.cfg`:
```
INSTANCE:0
GROUP:1
SYMBOL:AAPL
DATE:2026-04-15
SESSION:1            # 0=PreMkt 1=Intra 2=PostMkt 3=All
TF:0                 # Timeframe enum value
SPEED:1.0
MODE:0               # 0=Analysis 1=Operate
CURSOR:132           # last cursorBarIdx
EQUITY:100000.0      # user-edited startingCash (§6.6)
TICKFILLS:0          # 0=bar-resolution fills, 1=tick-resolution (§6.2 hybrid)
```

Same atomic-write pattern as `chart-modes.cfg`. Save on dirty + once per second from `RenderTradingUI`; sync save in `DestroyTradingWindows`. Load in `FinishConnect(false)` after `CreateTradingWindows`, after watchlist + chart-modes load.

Sim-account state and working orders are NOT persisted in v1 (open question §6.14). Re-fetching the day from cache is fast, so the user can reconstruct state in ~1s.

### Task H — Documentation

**Files:** `.claude/rules/architecture.md`, `.claude/rules/task-history.md`, `.claude/rules/testing.md`, `README.md` (if present).

- Add a "Replay Window" section to `architecture.md` covering the layered diagram, ReqId layout, lifecycle, mode badge.
- Add the `[replay]` Catch2 tag to `testing.md` with full coverage list.
- Mark Phase 14 and individual Tasks A–H in `task-history.md`.

---

## 8. Future work (Phase 15+)

Items already captured in §2b for clarity, plus things that emerged while drafting:

- **Anthropic API integration** — full Phase 15 plan, separate file.
- **Multi-day backtesting + replay-from-csv** for testing strategies on data not in IB.
- **True tick-by-tick replay** with the Time & Sales tape from Task #43 wired to the engine.
- **Saved replay scenarios** — name a `(symbol, date, session)` and re-open it from a menu.
- **Replay competitions** — generate a deterministic seed for slippage so two users can replay the same day with the same fill model and compare their PnL fairly.
- **Equity curve & drawdown plot** — small ImPlot panel.
- **Pattern library** — flag a pattern (head-and-shoulders, double bottom) on a replay day, save the bar-range and indicator state, build a searchable library of patterns the user can revisit.
- **Trading journal export** — emit the replay session as Markdown + chart screenshot for a separate journal app.

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| ReplayWindow's order entry calls `IBKRClient::PlaceOrder` by mistake (severe) | Compile-time: ReplayWindow doesn't take an IBKRClient pointer at all. Only the `OnPaperOrderSubmit` callback. main.cpp guarantees that callback is wired to the ReplayEngine, never to IBKRClient. + a unit test asserts no PlaceOrder symbol resolves in ReplayWindow.cpp. |
| Cache files corrupt the next session | Each cache file has a magic header (`'IBRC'` 4 bytes + uint32 version) checked on load; corrupt files are deleted and re-fetched. |
| IB historical fetch hits pacing limits when the user opens 10 ReplayWindows at once | Mode-switch FIFO from Task #63 gets reused — replay fetches share the same 1s/req throttle queue. |
| Replay clock drifts vs. wall-clock at high speeds | At MAX speed we don't tie to wall-clock at all — we just advance N bars per frame until cursor reaches sessionLast or user pauses. |
| User confuses replay account with live account | `[PAPER]` badge on toolbar (always); orange OPERATE mode badge; modal title prefix "PAPER ORDER —"; sim-fill markers visually distinct (◇ not ▲). |
| Auto-analysis re-running per bar at high replay speed kills frame rate | Cache the auto-analysis result keyed by `(cursorBarIdx, indicator-settings-hash)`; recompute only when crossing into a new bar at <60×; at MAX speed, recompute only every Kth bar. |
| User trades a symbol in replay that they're also actively trading live, gets confused which fills are which | Visual distinction (markers + tab labels) + the `[PAPER]` badge. ReplayWindow titles include the date in the title bar (e.g. `Replay AAPL 2026-04-15 G1###replay0`). |

---

## 10. Sequencing & estimates

Suggested merge order (each task is one PR):

1. Task A — pure logic + tests (~2 days)
2. Task C.5 — chart-render extraction (~1 day)
3. Task B — IBKRClient historical hooks + cache (~2 days)
4. Task C — ReplayWindow shell (~3 days)
5. Task D — paper trading wiring (~2 days)
6. Task E — analysis features (~2 days)
7. Task F — AI placeholder + summary clipboard (~0.5 day)
8. Task G — persistence (~1 day)
9. Task H — docs (~0.5 day)

Total: ~14 dev-days for v1, plus live-IB smoke test (manual) at the end.

---

## 11. Acceptance checklist (for marking Phase 14 done)

- [ ] All Catch2 tests pass (`tests-core` + `tests-ibkr`); `[replay]` tag has ≥40 cases.
- [ ] ReplayWindow opens, fetches a known historical day from IB, plays at 1× and shows the cursor advancing.
- [ ] Speed combo at 60× completes a 6.5-hour intraday replay in <7 minutes without UI stutter (ImGui frame > 16ms <5% of frames).
- [ ] Manual scrub (drag) repositions the cursor instantly with no ghost fills.
- [ ] Step ⏪ / ⏩ buttons move exactly one bar at a time.
- [ ] All four sessions (PreMkt / Intra / PostMkt / All) load the correct bar range.
- [ ] Operate mode: place a Limit BUY at a price the day's low touches → fill appears in Simulated Orders tab; position shows in status bar; equity updates.
- [ ] Operate mode: place a Stop SELL above current price; advance bars until price spikes above the stop → fill appears.
- [ ] Analysis mode: real fills from `g_executions` for that symbol+date appear as triangle markers with hover tooltips.
- [ ] Compare panel shows real PnL vs. sim PnL.
- [ ] AI placeholder panel renders; `[Copy day summary]` button puts a coherent Markdown blob on the clipboard.
- [ ] Persistence: close the app with two ReplayWindows open in different states → reopen → both restore to the same date / symbol / cursor / mode.
- [ ] **Safety:** grep `src/ui/windows/ReplayWindow.cpp` for `PlaceOrder` / `cancelOrder` / `modifyOrder` returns zero matches.
- [ ] Live-IB smoke test on at least one historical day each for PreMkt, Intra, PostMkt sessions.

---

## 12. References

- `architecture.md` — multi-instance window pattern, ReqId layout, ChartAnalysis primitives, SymbolSearch widget, Window groups
- `testing.md` — Catch2 + tests-core source-list pattern
- `ibkr-api.md` — `reqHistoricalData` / `reqHistoricalTicks` / `reqHistoricalNews` quirks
- `.claude/plans/setup-suggestions.md` — Phase 12 §3f for the "borrow S/R from chart" pattern Replay reuses
- `.claude/plans/trading-styles.md` — §3 for the per-instance throttle queue we reuse for replay fetches
- `.claude/plans/auto-analysis.md` — `ChartAnalysis.h` API surface that Replay re-runs per bar
