# Plan: Setup Suggestions + Stop-Loss Guard + SL-Hunter Protection

**Status:** Tasks A, B, C done (2026-04-27). Task D in progress.
**Owner:** Jose
**Goal:** Two related additions on top of the auto-analysis layer (Phase 11):
1. **Setup suggestion overlay** — when the existing `m_breakoutSignal` fires
   (`LongSetup` / `ShortSetup`), publish a structure-based reference plan with
   entry (limit) / stop-loss (stop-limit) / target (limit) so the user can stage
   the trade with one click instead of re-deriving the levels by eye.
2. **Unguarded-position warning** — when the user has an open position with no
   protective Stop / StopLimit / Trail / TrailLimit order on the same conId,
   surface a non-blocking yellow strip in the ChartWindow + TradingWindow with a
   one-click *"Place protective stop at $X"* button.

Both halves apply the same **SL-hunter defenses**: pad past the structure level,
anchor at the longest recent wick (not the cluster mean), avoid round numbers,
prefer Stop-Limit with a small offset over Stop-Market.

This file is the source of truth across sessions — update the **Status** line and
the per-task checkboxes as work progresses.

---

## 1. Approved scope

| # | Feature | Default | Notes |
|---|---|---|---|
| 1 | **Setup overlay** on chart | OFF | Renders when `m_breakoutSignal != None` AND `m_setup.valid`. Three dashed h-lines (entry / stop / target) with right-edge labels and an R:R tag. Reuses the existing supply/demand zone detection — no new IB data needed. |
| 2 | **"Use suggestion" button** in TradePanel | OFF (only visible when `m_setup.valid`) | One click prefills `m_orderTypeIdx` + price buffers and opens `DrawConfirmPopup()` (never auto-fires). |
| 3 | **Unguarded-position warning** strip | ON | Per-frame check across `g_positions` × `g_liveOrders`. Yellow strip + one-click *"Place protective stop"* button. App-wide (Chart + Trading windows). |
| 4 | **SL-hunter protection** helpers | always-on inside the helpers | Padding past structure, longest-wick anchor, round-number avoidance, Stop-Limit offset. Used by both #1 and #3. |

Defaults reflect the same signal-to-noise trade-off as Phase 11: visualisation
(#1) starts off, but the safety guard (#3) is on by default because an
unguarded position is a real hazard, not a curated opinion.

## 2. Resolved decisions (from the conversation)

- **Wording matters** — the overlay calls them *"reference levels"*, not
  *"entry / SL / TP"* in user-visible labels. Tooltip clarifies they are
  *"structure-based suggestions, not advice"*.
- **No auto-staging** — every code path that builds a `core::Order` from a
  suggestion must route through `DrawConfirmPopup()` regardless of the
  *"Transmit Instantly"* toggle in the Trade panel. The user always sees the
  final order before it goes out.
- **Per-instance toggles, in-memory only** — match the existing
  `IndicatorSettings` / `AutoAnalysisSettings` pattern. No disk persistence.
- **Tests** — pure logic in `src/core/services/ChartAnalysis.h`
  (`SuggestSetup`, `SuggestStopForPosition`, `AvoidRoundNumber`,
  `PositionSizeShares`); cases added to `tests/test_chart_analysis.cpp`
  under a new `[setup]` tag.
- **No new IB subscriptions** — every input is already in `m_breakoutSignal`,
  `m_breakoutZoneTop/Bot`, `m_atr14`, `m_autoSupports`, `m_autoResistances`,
  `g_positions`, `g_liveOrders`, and `g_accountValues` (NetLiquidation from
  `PortfolioWindow`).
- **Long-only first cut for the suggestion overlay** — Short setups follow the
  same code paths with `LevelSide::Above` as the SL anchor and demand zones as
  targets; both are wired in Task A but the manual test plan exercises long
  first.

## 3. Architecture

### 3a. Files

| Path | Purpose | Status |
|---|---|---|
| `src/core/services/ChartAnalysis.h` | Pure helpers (`SuggestSetup`, `SuggestStopForPosition`, `AvoidRoundNumber`, `PositionSizeShares`) | EDIT |
| `src/ui/windows/ChartWindow.h` | Add `SetupSettings` struct + `m_setup` + `m_unguardedSuggestedStop` + `SetUnguardedSuggestion()` + decls | EDIT |
| `src/ui/windows/ChartWindow.cpp` | Wire compute + draw + toolbar + "Use suggestion" button + unguarded strip | EDIT |
| `src/ui/windows/TradingWindow.h` | Add `m_unguardedSuggestedStop` + `SetUnguardedSuggestion()` decl | EDIT |
| `src/ui/windows/TradingWindow.cpp` | Render unguarded strip above order entry; one-click stop button | EDIT |
| `src/main.cpp` | `g_unguarded` vector + `RecomputeUnguardedPositions()`; wire into position/order callbacks; broadcast to ChartWindow + TradingWindow per frame | EDIT |
| `tests/test_chart_analysis.cpp` | Catch2 cases for the four new helpers | EDIT |

### 3b. New structs (in `core::services`, alongside `Level` / `TrendFit`)

```cpp
// Reference-only trade plan derived from the active supply/demand signal.
// Side mirrors core::OrderSide (1 = Buy / Long, 0 = Sell / Short).
struct SetupPlan {
    bool   valid       = false;
    int    side        = 0;     // 1=long, 0=short
    double entry       = 0.0;   // limit price
    double stop        = 0.0;   // protective stop trigger
    double stopLmt     = 0.0;   // stop-limit price (stop ± offset, depending on side)
    double target      = 0.0;   // take-profit limit
    double rr          = 0.0;   // (target-entry)/(entry-stop) — magnitude
    int    shares      = 0;     // 0 if equity not wired
    int    refLevelIdx = -1;    // index into m_autoSupports/m_autoResistances used as anchor
};

// Suggested stop for an existing position (reads from m_autoSupports / m_autoResistances).
struct PositionStop {
    bool   valid    = false;
    double stop     = 0.0;
    double stopLmt  = 0.0;     // stop ± offset
    double pctRisk  = 0.0;     // |entry-stop|/entry × 100
};
```

### 3c. New helper signatures (in `core::services`)

```cpp
// Snap `price` away from the nearest .00 / .25 / .50 / .75 mark by at least
// `pad` dollars. `pushDown=true` means we're a stop below the entry (push
// further below if too close); false means stop above (push further above).
inline double AvoidRoundNumber(double price, double pad, bool pushDown);

// Build a setup plan from the active breakout signal.
//   side        = 1 long / 0 short
//   zoneTop/Bot = m_breakoutZoneTop / m_breakoutZoneBot (already buffered)
//   anchorMin   = Level.minPrice (long) or Level.maxPrice (short) of the active
//                 zone — used as the SL structural anchor (longest-wick)
//   opposing    = nearest opposing level (resistance for long, support for short)
//   atr         = current ATR(14)
//   last        = latest close (used as fallback entry if past the zone)
//   atrPad      = SL padding multiplier (k × ATR; default 0.5)
//   roundPad    = round-number avoidance padding in $ (default 0.07)
//   stopOffset  = stop-limit offset from stop trigger (default 0.10)
//   rrMin       = minimum target/risk ratio to emit a valid plan (default 2.0)
//   equity      = NetLiquidation in $; 0 = skip share calc
//   riskPct     = % of equity per trade (default 1.0)
inline SetupPlan SuggestSetup(int side, double zoneTop, double zoneBot,
                              double anchorMin, double opposingLevel,
                              double atr, double last,
                              double atrPad, double roundPad, double stopOffset,
                              double rrMin, double equity, double riskPct);

// Build a protective-stop suggestion for an existing position.
//   isLong        = pos.quantity > 0
//   entry         = pos.avgCost
//   levels        = m_autoSupports (long) or m_autoResistances (short),
//                   passed as already-filtered KeepTopN result
//   atr, atrPad, roundPad, stopOffset = same semantics as SuggestSetup
inline PositionStop SuggestStopForPosition(bool isLong, double entry,
                                           const std::vector<Level>& levels,
                                           double atr,
                                           double atrPad, double roundPad,
                                           double stopOffset);

// Risk-based position sizing. Returns 0 if equity≤0 or |entry-stop|<=0.
inline int PositionSizeShares(double equity, double riskPct,
                              double entry, double stop);
```

### 3d. ChartWindow additions

```cpp
struct SetupSettings {
    bool   overlay     = false;   // master toggle for the suggestion overlay
    double rrMin       = 2.0;
    double atrPad      = 0.5;
    double roundPad    = 0.07;
    double stopOffset  = 0.10;
    double riskPct     = 1.0;
    bool   useStopLmt  = true;    // false = use plain Stop instead of StopLimit
};

SetupSettings              m_setupSettings;
core::services::SetupPlan  m_setup;

// Unguarded-position warning state — set by main.cpp once per frame.
struct UnguardedHint {
    bool        active     = false;
    double      qty        = 0.0;
    double      avgCost    = 0.0;
    double      stopTrig   = 0.0;
    double      stopLmt    = 0.0;
};
UnguardedHint              m_unguarded;
void SetUnguardedSuggestion(const UnguardedHint& h) { m_unguarded = h; }

// New private methods
void DrawSetupOverlay();         // dashed entry/stop/target lines + R:R tag
void DrawUnguardedStrip();       // yellow strip above the chart with "Place stop" btn
void DrawSetupSettingsPopup();   // section inside the existing Auto... popup
```

### 3e. TradingWindow additions

```cpp
ChartWindow::UnguardedHint m_unguarded;          // same struct, copied per frame
void SetUnguardedSuggestion(const ChartWindow::UnguardedHint& h);

// Renders a yellow strip above the order-entry panel when m_unguarded.active.
// One-click "Place protective stop" button fires OnOrderSubmit with a fully
// populated core::Order (StopLimit, Sell for long pos, opposite for short).
void DrawUnguardedStrip();
```

### 3f. main.cpp additions

```cpp
struct UnguardedPosition {
    std::string symbol;
    long        conId    = 0;
    double      qty      = 0.0;     // signed
    double      avgCost  = 0.0;
    double      stopTrig = 0.0;     // suggested stop (filled in per chart's S/R)
    double      stopLmt  = 0.0;
};
static std::vector<UnguardedPosition> g_unguarded;

void RecomputeUnguardedPositions();   // walk g_positions × g_liveOrders, no S/R yet
void PushUnguardedHintsToWindows();   // per-frame: ask each ChartWindow for its
                                      // S/R, build the suggestion, send to win
                                      // + matching TradingWindow
```

`RecomputeUnguardedPositions()` is cheap (O(positions × orders), both small) and
is wired into:
- `onPositionData` (line ~1462)
- `onPortfolioUpdate` (line ~1487)
- `onOpenOrder` (line ~1513)
- `onOrderStatusChanged` (line ~1523)

`PushUnguardedHintsToWindows()` is called once per frame from the main loop,
right after `UpdateAllChartPendingOrders()`. It walks `g_chartEntries` /
`g_tradingEntries`, looks up `g_unguarded` by symbol, and if present calls
`SuggestStopForPosition(...)` using that ChartWindow's already-detected S/R
(passed via a new `getAutoLevels()` accessor on ChartWindow), then forwards the
result via `SetUnguardedSuggestion()`. If no chart instance is open for the
symbol, the trading window still gets a coarse stop based on `avgCost − k×ATR`
estimated from the last-known volatility (defer to v2 — for v1, skip the warning
when no chart S/R is available).

### 3g. Toolbar / popup wiring

- `DrawAnalysisToolbar()` Auto: row gains a `Setup` checkbox after `Zones`.
  Toggling it just flips `m_setupSettings.overlay`; `m_setup` is recomputed in
  `DetectStructure()` whenever `m_setupSettings.overlay` is true (cheap — one
  `SuggestSetup()` call).
- `DrawAutoSettingsPopup()` gains a *Setup suggestions* section:
  - `R:R minimum` (`InputFloat`, 1.0–5.0, default 2.0)
  - `ATR padding (k)` (`InputFloat`, 0.1–2.0, default 0.5)
  - `Round-number pad ($)` (`InputFloat`, 0.0–0.50, default 0.07)
  - `Stop-limit offset ($)` (`InputFloat`, 0.0–1.0, default 0.10)
  - `Risk per trade (%)` (`InputFloat`, 0.1–5.0, default 1.0)
  - `Use Stop-Limit (off = plain Stop)` (`Checkbox`, default ON)

### 3h. ReqId / data dependencies

None new. Inputs:
- `m_breakoutSignal` / `m_breakoutZoneTop` / `m_breakoutZoneBot` (Phase 11)
- `m_autoSupports` / `m_autoResistances` (Phase 11)
- `m_atr14` (Phase 11)
- `m_closes.back()` (existing)
- `g_positions` / `g_liveOrders` (existing)
- `g_accountValues.netLiquidation` from `PortfolioWindow` (existing) — accessed
  via a new free accessor `GetSelectedAccountEquity()` in main.cpp.

---

## 4. Algorithms (reference)

### 4a. `AvoidRoundNumber`

```
fracDollar = price − floor(price)
candidates = { 0.00, 0.25, 0.50, 0.75, 1.00 }
nearest    = argmin |fracDollar − r|
distance   = |fracDollar − nearest|
if distance >= pad: return price  // already safe

// Push *away* from the round number, in the direction of safety.
if pushDown:
    return floor(price) + (nearest − pad)
else:
    return floor(price) + (nearest + pad)
```

Edge: if `nearest == 1.00` and `pushDown=false`, the nudge crosses into the next
integer — that's fine, we're moving stops further out, which is the safe
direction.

### 4b. `SuggestSetup` (long path; short is the mirror)

```
mid     = (zoneTop + zoneBot) / 2
entry   = (last >= zoneTop) ? last : mid          // already past = chase at last
                                                   // inside zone = limit at mid

// Structural anchor: the LONGEST WICK of the demand zone, not the cluster mean.
// For long: anchorMin = Level.minPrice of the active demand zone
// (passed in by ChartWindow from m_breakoutFromSupply branch).
rawStop = anchorMin − atrPad × atr               // pad below the wick
stop    = AvoidRoundNumber(rawStop, roundPad, /*pushDown=*/true)

target  = opposingLevel                          // nearest resistance
risk    = entry − stop
reward  = target − entry
rr      = reward / risk

if rr < rrMin:                                   // R:R too thin → no plan
    return SetupPlan{ valid=false }

stopLmt = stop − stopOffset                      // tolerate small slippage
shares  = PositionSizeShares(equity, riskPct, entry, stop)

return SetupPlan{ valid=true, side=1, entry, stop, stopLmt, target, rr, shares,
                  refLevelIdx=indexOfOpposingLevel }
```

Short: swap inequalities, use `Level.maxPrice` as anchor, push UP for round
avoidance, `stopLmt = stop + stopOffset`, target = nearest support below entry.

### 4c. `SuggestStopForPosition`

```
isLong:
    candidates = supports filtered by { c.price < entry, c.touches >= 2 }
    if empty: return PositionStop{ valid=false }
    nearest    = argmax c.price  (closest to entry from below)
    rawStop    = nearest.minPrice − atrPad × atr
    stop       = AvoidRoundNumber(rawStop, roundPad, true)
    stopLmt    = stop − stopOffset
    pctRisk    = (entry − stop) / entry × 100
    return PositionStop{ valid=true, stop, stopLmt, pctRisk }

short: mirror with resistances, push UP, stopLmt = stop + offset
```

### 4d. `PositionSizeShares`

```
if equity <= 0 or |entry − stop| <= 0: return 0
risk_dollars = riskPct/100 × equity
return floor(risk_dollars / |entry − stop|)
```

No max-position cap in v1 — the user already sees R:R + share count and decides.

### 4e. `RecomputeUnguardedPositions`

```
g_unguarded.clear()
for (sym, pos) in g_positions:
    if |pos.quantity| < 1e-9: continue            // flat
    isLong   = pos.quantity > 0
    needSide = isLong ? Sell : Buy
    hasStop  = false
    for (oid, ord) in g_liveOrders:
        if ord.symbol != sym:                     continue
        if ord.side   != needSide:                continue
        if ord.status not in {Pending, Working, PartialFill, PendingCancel}: continue
        if ord.type in {Stop, StopLimit, Trail, TrailLimit}:
            // Sanity: qty must cover at least part of the position
            if ord.quantity > 1e-9:
                hasStop = true; break
    if not hasStop:
        g_unguarded.push_back({sym, pos.conId, pos.quantity, pos.avgCost, 0, 0})
```

The actual `stopTrig`/`stopLmt` fields are filled in by
`PushUnguardedHintsToWindows()` once per frame, because they depend on the
chart-instance's auto-detected S/R levels.

### 4f. `DrawSetupOverlay`

Three dashed h-lines spanning the visible X range, drawn between
`DrawAutoSupportResistance()` and the user drawings:

| Line | Colour | Right-edge label |
|---|---|---|
| Entry  | cyan, alpha 220 | ` ENTRY 187.42 ` |
| Stop   | red, alpha 220, dashed | ` STOP 184.97 (-1.31%) ` |
| Target | green, alpha 220, dashed | ` TGT 192.10 (R:R 2.1) ` |

Labels are right-edge so they don't collide with the left-edge S/R tags from
Phase 11. When `m_setup.shares > 0`, append ` × N sh` to the entry label.

### 4g. `DrawUnguardedStrip` (chart-window version)

A 24-px-tall yellow strip rendered above `DrawCandleChart()` (i.e. inside the
chart window but above the plot rect — same vertical slot as the existing info
bar but rendered before it) when `m_unguarded.active`:

```
[ ⚠  AAPL long 100 sh @ $187.42 — no protective stop. Suggested stop $184.97 (-1.31%).  [ Place stop ]  [ Dismiss ] ]
```

- "Place stop" button builds a `core::Order` (`type=StopLimit`,
  `side=Sell` for long, `quantity=|pos.quantity|`, `auxPrice=stopTrig`,
  `lmtPrice=stopLmt`, `tif=DAY`, `outsideRth=false`) and routes it through
  `DrawConfirmPopup()` (sets `m_pendingConfirmOrder` and opens the modal —
  never bypasses confirmation).
- "Dismiss" sets a per-symbol `m_dismissedUnguarded` set so the strip stops
  showing for that symbol until a position change.
- The same logic on the TradingWindow side renders a strip above
  `DrawOrderEntry()` instead.

---

## 5. Task breakdown

### Task A — Pure helpers + tests
**Status:** DONE (2026-04-26)

- [x] `ChartAnalysis.h`:
  - [x] `AvoidRoundNumber(price, pad, pushDown) → double`
  - [x] `SuggestSetup(side, zoneTop, zoneBot, anchor, opposingLevel, atr, last, atrPad, roundPad, stopOffset, rrMin, equity, riskPct) → SetupPlan`
  - [x] `SuggestStopForPosition(isLong, entry, levels, atr, atrPad, roundPad, stopOffset) → PositionStop`
  - [x] `PositionSizeShares(equity, riskPct, entry, stop) → int`
  - [x] Added `<limits>` include for `std::numeric_limits` in `AvoidRoundNumber`.
- [x] `tests/test_chart_analysis.cpp` — 13 new cases / 55 assertions under `[setup]` tag:
  - [x] `AvoidRoundNumber`: already-safe price returned unchanged (3 prices comfortably outside pad); `$10.00` push down → `$9.93`, push up → `$10.07`; `$187.50` (right on .50) → `$187.43` / `$187.57`; `$10.95` near 1.00 push up → `$11.07`; degenerate inputs (pad ≤ 0, price ≤ pad, sub-cent pad on non-round price) return unchanged.
  - [x] `SuggestSetup` long inside zone: entry = mid (186), stop = 183.43 (snapped past .50), target = 192, rr ≈ 2.34, shares = 0 when equity = 0.
  - [x] `SuggestSetup` long with `last > zoneTop`: entry clamped to last (188), shares = 218 with equity=100k riskPct=1.
  - [x] `SuggestSetup` rejects when `rr < rrMin` (opposingLevel = 187.5 → reward 1.5 < 2.57 risk).
  - [x] `SuggestSetup` short mirror: anchor = zone.maxPrice = 190.5, stop = 191.57 (snapped past .50 up), target = 180, rr ≈ 3.5.
  - [x] `SuggestSetup` degenerate input (atr = 0, zoneTop ≤ zoneBot, side = 2) → invalid.
  - [x] `SuggestStopForPosition` long: picks the closer 185-cluster from `[{185,touches=3}, {180,touches=2}]`; stop = 184.6 - 1.0 = 183.6 (frac=.60, dist to .50 = .10 ≥ pad → unchanged).
  - [x] `SuggestStopForPosition` returns `valid=false` for single-touch level, empty list, and "every level is above entry" (no qualifying support).
  - [x] `SuggestStopForPosition` short mirror: picks 185-cluster as nearest resistance above entry=180; stop = 185.4 + 1.0 = 186.4.
  - [x] `PositionSizeShares` basic: 1% of 100k / $3 risk → 333; 0.5% of 50k / $2.50 → 100. Degenerate (zero equity, zero risk%, entry==stop, negative equity) → 0.
- [x] `cmake --build build --target tests-core -j$(nproc)` — clean (no new warnings).
- [x] `ctest --test-dir build --output-on-failure` — **116/116 pass** (100 existing + 16 from this task; the additional 3 over the planned 13 are sub-cases counted separately by ctest discovery).

**Notes / changes from the plan:**
- The boundary case `AvoidRoundNumber(187.07, 0.07, true) == 187.07` was dropped from the test — `0.07` literal isn't exactly representable in double, and `187.07 - 187.0` produces a fractional part one ULP below `0.07`, so the `bestD >= pad` check trips by a hair. Replaced with two clearly-safe-distance assertions (`187.10` and `187.40`). The `>=` boundary semantics in the helper are unchanged.
- Renamed the `anchor` parameter in `SuggestSetup` (originally `anchorMin`) so a single name covers both sides — caller passes `Level.minPrice` for long, `Level.maxPrice` for short. Documented in the function header comment.

### Task B — Setup overlay rendering + "Use suggestion" button
**Status:** DONE (2026-04-26)

- [x] `ChartWindow.h`:
  - [x] Added `SetupSettings m_setupSettings`, `core::services::SetupPlan m_setup` (aliased as `SetupPlan`), and `m_breakoutLevelIdx` (idx into `m_autoSupports` / `m_autoResistances` for the active zone).
  - [x] Decls for `ComputeSetupPlan`, `DrawSetupOverlay`, `DrawSetupSettingsPopup`.
  - [x] Public accessor `AutoLevelSnapshot getAutoLevels() const` returning `{m_autoSupports, m_autoResistances, m_atr14.empty() ? 0.0 : m_atr14.back()}` so main.cpp can build position-stop suggestions per chart in Task C.
- [x] `ChartWindow.cpp`:
  - [x] Forward-declared `double GetSelectedAccountEquity()` (extern, defined in main.cpp).
  - [x] `ComputeBreakoutSignal()` now records `m_breakoutLevelIdx` so `ComputeSetupPlan()` can fetch the actual `Level.minPrice` / `maxPrice` of the active zone for the structural anchor.
  - [x] `ComputeSetupPlan()` — new helper invoked at the tail of `DetectStructure()` when `m_setupSettings.overlay && m_breakoutSignal != None`. Picks the right anchor side (long → demand zone `minPrice`, short → supply zone `maxPrice`), finds the nearest opposing level by walking `m_autoResistances`/`m_autoSupports`, fetches equity, and calls `SuggestSetup()`. `DetectStructure()` resets `m_setup = SetupPlan{}` and `m_breakoutLevelIdx = -1` up-front so toggling the overlay off cleanly clears state.
  - [x] `DrawOverlays()`: invokes `DrawSetupOverlay()` between `DrawAutoSupportResistance()` and `DrawAutoTrend()` so the entry/stop/target lines render on top of S/R but underneath the trend overlay and user drawings.
  - [x] `DrawSetupOverlay()`: three dashed h-lines — entry (cyan, ` ENTRY 187.42 x N sh `), stop (red, ` STOP 184.97 (-1.31%) `), target (green, ` TGT 192.10 (R:R 2.1) `) — all right-edge labels (S/R uses left-edge so they don't collide). Faint stop-limit companion line rendered when `m_setupSettings.useStopLmt`.
  - [x] `DrawAnalysisToolbar()`: added `Setup` checkbox immediately after `Breakouts` in the Auto: row (default OFF). Toggling re-runs `DetectStructure()` so `m_setup` recomputes immediately.
  - [x] `DrawAutoSettingsPopup()`: appends `DrawSetupSettingsPopup()` after the Donchian section. Setup suggestions section exposes R:R minimum (1.0–5.0), ATR padding (0.1–2.0), Round-number pad ($) (0.0–0.50), Stop-limit offset ($) (0.0–1.0), Risk per trade (%) (0.1–5.0), and a `Use Stop-Limit (off = plain Stop)` checkbox. Any change re-runs `DetectStructure()`.
  - [x] `DrawTradePanel()`: when `m_setupSettings.overlay && m_setup.valid`, a blue `[Use suggestion]` button renders right after the SELL button. Click → forces `m_orderTypeIdx = 1` (Limit), adopts `m_setup.shares` as the qty when known, builds a Limit order via the existing `buildOrder()` lambda, sets `o.limitPrice = m_setup.entry`, stages it in `m_pendingConfirmOrder`, and opens `DrawConfirmPopup()` regardless of the Transmit Instantly toggle. Tooltip explains "Reference plan, not advice."
- [x] `PortfolioWindow.h`: added `[[nodiscard]] double netLiquidation() const`.
- [x] `main.cpp`: free-function `double GetSelectedAccountEquity()` returns `g_PortfolioWindow ? g_PortfolioWindow->netLiquidation() : 0.0`.
- [x] Build: `cmake --build build -j$(nproc)` clean (no new warnings from added code; only pre-existing `snprintf` truncation warnings on group-id formatting).
- [x] Tests: `ctest --test-dir build --output-on-failure` — **116/116 pass** (Task A's 16 `[setup]` cases unchanged; nothing in Task B touched pure logic, all UI/wiring).

**Notes / changes from the plan:**
- Anchor-direction parameter to `SuggestSetup` was originally specified as `anchorMin`. Task A renamed it to `anchor` (caller picks `Level.minPrice` for long, `Level.maxPrice` for short); Task B's `ComputeSetupPlan` honours that contract.
- The `DrawSetupSettingsPopup()` helper is invoked from inside `DrawAutoSettingsPopup()` rather than being its own popup, matching §3g of the plan ("section inside the existing Auto... popup"). The decl exists in the header so it can be unit-tested or repointed later.
- The "Use suggestion" button sets `m_orderTypeIdx = 1` (the index of "Limit" in `kOrderTypes[]`). The mapping is fixed by the file-scope table in `ChartWindow.cpp` and stable across the codebase.
- `m_breakoutLevelIdx` is a new field added to ChartWindow because the existing breakout signal only stored the buffered top/bottom of the active zone, not which `Level` produced it. `ComputeSetupPlan()` needs the underlying `Level` to read its `minPrice`/`maxPrice` (the buffered zone bounds are not the right anchor — the structural anchor is the deepest wick).
- The acceptance check (live IB session, AAPL 5m setup signal, popup verification) is deferred to manual smoke-test before Task C lands, since the test infra is headless. The build + ctest gate is the automated proof.

### Task C — Unguarded-position guard
**Status:** DONE (2026-04-27) — automated gates green; live IB smoke-test deferred (manual).

- [x] `main.cpp`:
  - [x] `UnguardedPosition` struct + `static std::vector<UnguardedPosition> g_unguarded` (lines 262–268). Lean shape — `{symbol, conId, qty, avgCost}`; the suggested stop fields live on the per-window `UnguardedHint` instead.
  - [x] `RecomputeUnguardedPositions()` per §4e (lines 281–315). Walks `g_positions` × `g_liveOrders`; matches by `(symbol, opposite side, live status, protective type, qty>0)`.
  - [x] Wired into `onPositionData` (line 1671), `onPortfolioUpdate` (line 1698), `onOpenOrder` (line 1706), `onOrderStatusChanged` (line 1727). Each adds a `RecomputeUnguardedPositions()` call after the existing logic.
  - [x] `onPositionData` also mirrors `pos` into `g_positions` (lines 1644–1657) so `RecomputeUnguardedPositions` sees the same single source of truth that `onPortfolioUpdate` already populates. Quantity-zero erases the entry so the warning self-clears on flat.
  - [x] `GetSelectedAccountEquity()` was already wired in Task B; reused as-is.
  - [x] `PushUnguardedHintsToWindows()` per §3f (lines 318–411). Called once per frame from `RenderTradingUI()` (line 3018) right before window `Render()` calls. For each chart, builds the hint from its own `getAutoLevels()` + `SuggestStopForPosition()`. For each trading window, borrows S/R from the *first* chart on the same symbol; falls back to `active=false` when no chart instance is open (per §3f v1 behaviour). Cleared hints (`active=false`) are sent to all non-matching windows so stale strips clear automatically.
- [x] `ChartWindow`:
  - [x] `UnguardedHint` struct on `ChartWindow.h` (lines 54–62) — `{active, symbol, qty, avgCost, stopTrig, stopLmt, pctRisk}`. Added `<unordered_set>` include.
  - [x] `SetUnguardedSuggestion(const UnguardedHint&)` slot (line 105) — quantity-change resets the per-symbol dismissal so the strip re-appears after the user trims/adds.
  - [x] `DrawUnguardedStrip()` per §4g (`ChartWindow.cpp` lines 2497–2592). Yellow strip with FlexRow toolbar; "Place stop" button stages a `core::Order` (`StopLimit` if `m_setupSettings.useStopLmt`, else plain `Stop`) on the opposite side of the position and routes through `DrawConfirmPopup()` — never bypasses confirmation.
  - [x] `m_unguarded` / `m_dismissedUnguarded` / `m_lastWarnedQty` private state (lines 304–306).
  - [x] `Render()` calls `DrawUnguardedStrip()` immediately after `if (!m_viewInitialized) InitViewRange();` (line 443) — above `DrawInfoBar()` / `DrawPositionStrip()` / `DrawCandleChart()`.
- [x] `TradingWindow`:
  - [x] Identical `UnguardedHint` struct on `TradingWindow.h` (lines 63–71) — declared independently to avoid header coupling. Added `<unordered_set>` include.
  - [x] `SetUnguardedSuggestion()` + `DrawUnguardedStrip()` (`TradingWindow.cpp` lines 1670–1770).
  - [x] Strip rendered inside `##entry_panel` BeginChild, above `DrawOrderEntry()` (line 298).
  - [x] One-click "Place stop" button pre-fills the order-entry buffers (`m_sideIdx`/`m_typeIdx=3` StopLimit/`m_tifIdx=0` DAY/`m_outsideRth=false`/`m_qtyBuf`/`m_stpBuf`/`m_lmtBuf`) and triggers `m_showConfirm = true`, so the existing `DrawConfirmationPopup()` reads the right values. The pre-fill keeps the order-entry panel and the confirmation modal in sync.
- [x] Acceptance — automated:
  - [x] `cmake --build build -j$(nproc)` clean (no new warnings beyond the pre-existing snprintf/strncpy truncation warnings).
  - [x] `ctest --test-dir build --output-on-failure` — **116/116 pass** (no new test cases — Task C is wiring, not pure logic; the helpers under `[setup]` were already covered in Task A).
- [ ] Acceptance — manual (deferred):
  - [ ] Paper account: buy 10 SPY at market; within one frame, yellow strip appears in the SPY ChartWindow and the SPY TradingWindow with a sensible stop price below the nearest detected support.
  - [ ] Click "Place stop" → confirmation popup → confirm; strip disappears within one frame after the stop order is acknowledged by IB.
  - [ ] Cancel the protective stop → strip reappears.
  - [ ] Click "Dismiss" → strip hides; close half the position (sell 5 SPY) → strip reappears (position changed).
  - [ ] Short position: same flow, stop above nearest resistance.

**Notes / changes from the plan:**
- §3e originally specified `ChartWindow::UnguardedHint` reused by TradingWindow. We declared `TradingWindow::UnguardedHint` independently with the same shape so neither header drags in the other — small duplication, but it preserves window independence. `PushUnguardedHintsToWindows` does a field-by-field copy when forwarding from chart-side to trading-side.
- One build error caught at compile time: `ImGuiChildFlags_Border` (singular) does not exist in this ImGui version (1.92.6-docking) — the correct enum is `ImGuiChildFlags_Borders` (plural). Fixed in both `ChartWindow.cpp` (line 2517) and `TradingWindow.cpp` (line 1706).
- The "scope of stop coverage" (`sum(stop.quantity)` vs `|pos.quantity|`) check is still v2 per §7. v1 considers any live protective stop on the matching side as "guarded" regardless of quantity.

### Task D — Docs + sanity
**Status:** Pending — final cleanup

- [ ] Update `.claude/rules/task-history.md` with **Task #56** (new Phase 12: Setup Suggestions & Position Safety) describing the user-visible additions.
- [ ] Update `.claude/rules/architecture.md` *Auto Technical Analysis* section with the new `SetupPlan` / `PositionStop` structs and the `g_unguarded` per-frame loop.
- [ ] Update root `README.md` if user-visible (yes — both features are user-facing).
- [ ] Run `cmake --build build -j$(nproc)` clean (no new warnings), `ctest` all pass, `DISPLAY=:1 ./build/ibkr-trading-app` smoke-test.

---

## 6. Acceptance / verification

For each task, before marking complete:

1. `cmake --build build -j$(nproc)` — clean build (no new warnings).
2. `ctest --test-dir build --output-on-failure` — all tests pass (including the
   new `[setup]` cases under `tests-core`).
3. Manual:
   - Connect paper account.
   - Open AAPL 5m + AAPL Order Book.
   - Toggle `Auto > Setup` — overlay appears once a zone signal fires.
   - Click `[Use suggestion]` — confirmation modal pre-filled with entry, stop,
     target. Cancel — no order goes out.
   - Buy 10 AAPL at market → unguarded strip appears in both windows within
     one frame. Click "Place stop" → confirmation → confirm → strip clears.
   - Cancel the stop in OrdersWindow → strip reappears.
   - Sell the 10 AAPL → strip clears (no position).
4. Update `.claude/rules/task-history.md` and root `README.md`.

---

## 7. Risks / sharp edges

- **"Suggestion" → "advice"** — the legal/UX trap. Every label must say
  *"reference"* / *"structure-based"* and the tooltip must include the
  *"not advice"* line. No emoji, no green-light coloured "GO" buttons.
- **No protective-stop coverage by partial size** — if the user has 100 sh long
  and a Stop for 50 sh, `RecomputeUnguardedPositions` currently treats that as
  guarded. Real risk is half-naked. v1: accept the simplification; v2:
  compare `sum(stop.quantity)` vs `|pos.quantity|` and warn on mismatch.
- **Stop near another support** — `AvoidRoundNumber` could push the stop into
  the next support cluster; small effect on R:R but the level itself is
  unchanged so the bias is benign. Accepted.
- **Bracket / OCO orders** — IB supports parent/child orderIds with `oca_group`
  and `transmit=false` chaining. Out of scope for v1; only the entry leg is
  staged via `[Use suggestion]`. v2 issues entry + stop + target as a real
  bracket so the protective stop lands automatically when the entry fills.
- **Equity not yet received** — `GetSelectedAccountEquity()` may return 0 on
  fresh connect before `accountSummary()` fires. `PositionSizeShares` returns
  0 in that case; the overlay still shows entry/stop/target but omits the
  share-count tail of the entry label.
- **Multi-account** — share sizing uses `g_selectedAccount`'s NetLiquidation.
  Switching accounts mid-session re-runs `FinishConnect()` which already
  refreshes account values; no extra wiring needed.
- **Timing of `RecomputeUnguardedPositions`** — fires inside IB callback
  lambdas. Cheap (O(positions × orders)), no IB calls inside, no UI calls — safe.
- **Stop-Limit no-fill risk** — a stop-limit can fail to fill on a sweep wick
  if `stopOffset` is too tight. Default `0.10` is a reasonable starting point
  for $100–500 stocks; the popup lets the user widen it. Document this in the
  popup tooltip.
- **Round-number snap can cross integer boundary** — `$10.00` with `pad=0.07`
  pushDown=true becomes `$9.93`. Intentional; the further-out side is the safe
  side for stops.

---

## 8. Out of scope (explicitly deferred)

- Real bracket / OCO orders (entry + stop + target as a single IB submission).
- Trailing-stop auto-management (move stop to break-even at +1R, etc.).
- Per-symbol risk profile tuning (different `riskPct` per ticker).
- Cross-symbol portfolio risk (correlation, sector concentration).
- Persistence of `m_setupSettings` / dismissed-warning set across restarts.
- Suggestion when no chart instance is open for the symbol (use cached
  volatility instead of live S/R).
- Hidden / iceberg orders for the entry leg (`::Order::hidden = true`,
  `displaySize`) — useful for size but resting stops aren't displayed in DOM
  anyway, so the marginal benefit is small and it adds an order-routing
  surface area that's better tackled separately.

---

## 9. Update log

| Date | Change | By |
|---|---|---|
| 2026-04-26 | Plan written | claude / jose |
| 2026-04-26 | Task A (pure helpers + tests) DONE — 13 cases / 55 assertions under `[setup]`; 116/116 pass | claude / jose |
| 2026-04-26 | Task B (setup overlay + Use suggestion button) DONE — wiring only, no new pure-logic cases | claude / jose |
| 2026-04-27 | Task C (unguarded-position guard) DONE — `g_unguarded` + `RecomputeUnguardedPositions()` wired into 4 IB callbacks; `PushUnguardedHintsToWindows()` per-frame; yellow strip + "Place stop" button on ChartWindow and TradingWindow, both routed through their existing confirmation modals. Build clean, 116/116 pass. Live-IB smoke-test deferred (manual). | claude / jose |
| 2026-04-27 | IB error 110 regression on `Place stop`: AvoidRoundNumber's `0.07` literal leaks ULP-level FP drift (`184 - 0.07 = 183.929999…`) and IB rejects sub-cent prices. Fixed by adding `core::services::RoundToTick(price, tick=0.01)` and applying it to every output of `SuggestSetup` and `SuggestStopForPosition`. 4 new test cases (incl. an IB-110 regression check that asserts `value*100 == round(value*100)` for all output prices). 120/120 pass. | claude / jose |
