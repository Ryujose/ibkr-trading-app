# Plan: TradingStyle Modes on ChartWindow (Phase 13)

**Status:** All tasks landed (Tasks A/B/C/D as #61–#64; Task E = docs in `task-history.md` / `architecture.md` / `README.md` / `testing.md`). Live IB smoke-test deferred to manual session.
**Owner:** Jose
**Goal:** Replace today's "the analysis basis shifts whenever the user changes
timeframe or pulls more history" behaviour with four curated **trading-style
presets** (Scalping / Day Trading / Swing / Investment). Each preset hard-binds
{timeframe + history horizon + auto-analysis params + setup-overlay params +
default-overlay toggles} so the auto S/R, the breakout signal, the setup
overlay, and the unguarded-stop guard all stay coherent within a session.

This file is the source of truth across sessions — update the **Status** line
and the per-task checkboxes as work progresses.

---

## 1. Approved scope

| # | Style | Bars | History | Single IB request? |
|---|---|---|---|---|
| 1 | **Scalping** | 1m | 2 D (~780 bars) | yes (`"2 D"` × `"1 min"` is within IB STK limit) |
| 2 | **Day Trading** | 15m | 20 D (~520 bars) | yes |
| 3 | **Swing** | 1D | 1 Y (~252 bars) | yes |
| 4 | **Investment** | 1W | 5 Y (~260 bars) | yes |

All four modes fit in a single `reqHistoricalData` call — no chaining, no
pacing-limit gymnastics. Default for a fresh `ChartWindow` = **Swing** (matches
today's behaviour: 1D bars, ~6 months of history).

## 2. Resolved decisions (from the conversation)

- **Hard-bind, not soft-bind** — switching modes overwrites the chart's
  timeframe. If the user wants a different TF for the same symbol, they spawn
  another chart in a different mode. Rationale: the whole point is that the
  suggestion stays coherent across panning/loading; soft-binding keeps the
  drift problem.
- **Per-instance, persisted** — each `ChartWindow` instance owns its own mode.
  Persisted to `~/.config/ibkr-trading-app/chart-modes.cfg` keyed by
  `(instanceIdx, symbol)` so it survives reconnect and app-exit. Cleared
  automatically when the chart's symbol changes (because mode is part of the
  per-symbol view).
- **Mode-switch atomicity** — must not show a half-populated overlay or stale
  S/R from the previous mode. Sequence: cancel current historical sub →
  clear all auto-detection state → issue prefetch → on `historicalDataEnd`,
  `DetectStructure()` re-runs → overlays + setup + unguarded hint repopulate
  in a single frame.
- **Throttle for mass-switch** — if the user batch-switches modes across many
  chart windows (e.g. opens 10 charts then picks Scalping for all), queue the
  historical requests with **1s spacing** to stay well under IB's
  60-requests-per-10-min-per-contract pacing limit. A simple FIFO with a
  `glfwGetTime()`-based ready check is enough; no need for IB's pacing API.
- **Defaults override the existing `IndicatorSettings` + `AutoAnalysisSettings`
  + `SetupSettings` defaults at *mode-apply time only*** — once applied, the
  user can still tune individual knobs via the existing popups. Switching
  modes again resets to the new mode's defaults (the previous tuning is lost
  for that chart). This matches the spirit of presets and avoids "I tweaked
  one knob, why did the others reset" confusion.
- **`Watchlists.cfg`-style hand-rolled persistence** — same atomic-write
  pattern as the existing `WatchlistsFilePath()` (`.tmp` + `rename`). No
  external dep.
- **Tests** — pure-logic mode-application helper extracted to
  `src/core/services/TradingStyle.h` (no ImGui / IB API / file I/O). Catch2
  cases under a new `[style]` tag in `tests/test_chart_analysis.cpp` (or a new
  `tests/test_trading_style.cpp` — pick the existing file to keep the tests-core
  source list short).

## 3. Architecture

### 3a. Files

| Path | Purpose | Status |
|---|---|---|
| `src/core/services/TradingStyle.h` | `enum class TradingStyle`, `StylePreset` struct, `GetPreset(style) → StylePreset`, `ApplyPreset(StylePreset, IndicatorSettings&, AutoAnalysisSettings&, SetupSettings&, Timeframe&)` | NEW |
| `src/ui/windows/ChartWindow.h` | Add `m_tradingStyle` member, `setTradingStyle()` / `tradingStyle()` accessors, `OnStyleChange` callback | EDIT |
| `src/ui/windows/ChartWindow.cpp` | Wire `setTradingStyle()` → call `ApplyPreset()` → emit `OnStyleChange()` (host triggers prefetch); add Style combo to `DrawToolbar()` | EDIT |
| `src/main.cpp` | Per-chart `ChartEntry::tradingStyle`, `OnStyleChange` lambda → cancel current sub → push to mode-switch FIFO → throttle → `ReqChartData()` with the new TF + duration; `chart-modes.cfg` save/load wired into the existing connect/disconnect lifecycle | EDIT |
| `src/core/models/MarketData.h` | Optionally add `TimeframeIBDurationFor(Timeframe, TradingStyle)` overload — but cleaner to drop a `historyDuration` field on `StylePreset` and have `ReqChartData` accept a duration override instead | EDIT (small) |
| `tests/test_chart_analysis.cpp` (or new `tests/test_trading_style.cpp`) | Catch2 cases for `GetPreset`, `ApplyPreset` field-by-field assertions, `style → timeframe` mapping invariants | EDIT/NEW |
| `tests/CMakeLists.txt` | If new test file — add to `tests-core` sources | EDIT (maybe) |

### 3b. New types (`core::services::TradingStyle.h`)

```cpp
namespace core::services {

enum class TradingStyle : int {
    Scalping    = 0,
    DayTrading  = 1,
    Swing       = 2,
    Investment  = 3,
};

inline const char* TradingStyleLabel(TradingStyle s) {
    switch (s) {
        case TradingStyle::Scalping:   return "Scalping";
        case TradingStyle::DayTrading: return "Day Trading";
        case TradingStyle::Swing:      return "Swing";
        case TradingStyle::Investment: return "Investment";
    }
    return "?";
}

inline const char* TradingStyleShort(TradingStyle s) {
    switch (s) {
        case TradingStyle::Scalping:   return "SCALP";
        case TradingStyle::DayTrading: return "DAY";
        case TradingStyle::Swing:      return "SWING";
        case TradingStyle::Investment: return "INVEST";
    }
    return "?";
}

// Bundle of every per-style override. POD on purpose — no business logic here.
// ChartWindow's `setTradingStyle()` calls `ApplyPreset(GetPreset(style), ...)`.
struct StylePreset {
    core::Timeframe timeframe;
    const char*     historyDuration;  // IB duration string, e.g. "2 D" / "20 D" / "1 Y" / "5 Y"

    // AutoAnalysisSettings overrides (only the fields that differ across modes;
    // leaving a sentinel here would be over-engineering — just write all of them).
    bool supports, resistances, trend, donchian, keltner, autoFib, pivotPoints, breakouts, zones;
    int  swingK, trendLookback, donchianLen, maxLevels, minTouches, scanCap;
    bool trendChannel;

    // IndicatorSettings overrides (only VWAP-relevant for now — everything else
    // is the same across modes).
    bool indVwap;
    bool indVwapBands;   // ±σ bands sub-toggle (used by the VWAP plan)

    // SetupSettings overrides.
    bool   setupOverlay;
    double rrMin, atrPad, roundPad, stopOffset, riskPct;
    bool   useStopLmt;
};

// The four canonical presets. Pure data — no allocation, no I/O.
inline StylePreset GetPreset(TradingStyle s) {
    switch (s) {
        case TradingStyle::Scalping:
            return StylePreset{
                .timeframe       = core::Timeframe::M1,
                .historyDuration = "2 D",
                .supports = true, .resistances = true, .trend = false,
                .donchian = false, .keltner = false, .autoFib = false,
                .pivotPoints = true, .breakouts = true, .zones = true,
                .swingK = 3, .trendLookback = 30, .donchianLen = 20,
                .maxLevels = 4, .minTouches = 2, .scanCap = 1500,
                .trendChannel = false,
                .indVwap = true, .indVwapBands = false,
                .setupOverlay = false,
                .rrMin = 1.5, .atrPad = 0.4, .roundPad = 0.03,
                .stopOffset = 0.05, .riskPct = 0.5, .useStopLmt = true,
            };
        case TradingStyle::DayTrading:
            return StylePreset{
                .timeframe       = core::Timeframe::M15,
                .historyDuration = "20 D",
                .supports = true, .resistances = true, .trend = true,
                .donchian = false, .keltner = false, .autoFib = false,
                .pivotPoints = true, .breakouts = true, .zones = true,
                .swingK = 3, .trendLookback = 40, .donchianLen = 20,
                .maxLevels = 4, .minTouches = 2, .scanCap = 1000,
                .trendChannel = false,
                .indVwap = true, .indVwapBands = true,
                .setupOverlay = false,
                .rrMin = 1.75, .atrPad = 0.5, .roundPad = 0.05,
                .stopOffset = 0.07, .riskPct = 0.75, .useStopLmt = true,
            };
        case TradingStyle::Swing:
            return StylePreset{
                .timeframe       = core::Timeframe::D1,
                .historyDuration = "1 Y",
                .supports = true, .resistances = true, .trend = true,
                .donchian = false, .keltner = false, .autoFib = true,
                .pivotPoints = false, .breakouts = true, .zones = true,
                .swingK = 4, .trendLookback = 50, .donchianLen = 20,
                .maxLevels = 3, .minTouches = 2, .scanCap = 1000,
                .trendChannel = false,
                .indVwap = false, .indVwapBands = false,
                .setupOverlay = false,
                .rrMin = 2.0, .atrPad = 0.5, .roundPad = 0.07,
                .stopOffset = 0.10, .riskPct = 1.0, .useStopLmt = true,
            };
        case TradingStyle::Investment:
            return StylePreset{
                .timeframe       = core::Timeframe::W1,
                .historyDuration = "5 Y",
                .supports = true, .resistances = true, .trend = true,
                .donchian = false, .keltner = false, .autoFib = true,
                .pivotPoints = false, .breakouts = false, .zones = false,
                .swingK = 5, .trendLookback = 52, .donchianLen = 20,
                .maxLevels = 3, .minTouches = 3, .scanCap = 1000,
                .trendChannel = false,
                .indVwap = false, .indVwapBands = false,
                .setupOverlay = false,
                .rrMin = 3.0, .atrPad = 1.0, .roundPad = 0.20,
                .stopOffset = 0.25, .riskPct = 1.5, .useStopLmt = true,
            };
    }
    return GetPreset(TradingStyle::Swing);  // unreachable
}

// Stamp every overridable field onto the existing settings structs.
// Caller is responsible for re-running DetectStructure() after this returns.
template <typename Ind, typename Auto, typename Setup>
inline void ApplyPreset(const StylePreset& p,
                        Ind& ind, Auto& a, Setup& s,
                        core::Timeframe& tf) {
    tf = p.timeframe;

    a.supports     = p.supports;
    a.resistances  = p.resistances;
    a.trend        = p.trend;
    a.donchian     = p.donchian;
    a.keltner      = p.keltner;
    a.autoFib      = p.autoFib;
    a.pivotPoints  = p.pivotPoints;
    a.breakouts    = p.breakouts;
    a.zones        = p.zones;
    a.swingK       = p.swingK;
    a.trendLookback= p.trendLookback;
    a.donchianLen  = p.donchianLen;
    a.maxLevels    = p.maxLevels;
    a.minTouches   = p.minTouches;
    a.scanCap      = p.scanCap;
    a.trendChannel = p.trendChannel;

    ind.vwap       = p.indVwap;
    // ind.vwapBands is added by the VWAP plan; if absent at compile time the
    // template instantiation just won't reference it — keep the assignment
    // guarded by the VWAP-plan member.
    // (Concretely: the member exists once both plans land; keep both rolling
    // out together.)

    s.overlay     = p.setupOverlay;
    s.rrMin       = p.rrMin;
    s.atrPad      = p.atrPad;
    s.roundPad    = p.roundPad;
    s.stopOffset  = p.stopOffset;
    s.riskPct     = p.riskPct;
    s.useStopLmt  = p.useStopLmt;
}

}  // namespace core::services
```

Templated on `Ind` / `Auto` / `Setup` so the helper doesn't have to include
`ChartWindow.h` (the structs are private nested types of `ChartWindow`). The
test code instantiates with hand-rolled stub structs.

### 3c. ChartWindow additions

```cpp
// Header
public:
    void                       setTradingStyle(core::services::TradingStyle s);
    core::services::TradingStyle tradingStyle() const { return m_tradingStyle; }

    // Fired whenever the user picks a new style from the toolbar combo (NOT on
    // initial set / restore). Host wires this to:
    //   1. Cancel current historical sub
    //   2. Enqueue mode-switch fetch (1s throttle)
    //   3. Re-issue ReqHistoricalData with the preset's TF + historyDuration
    std::function<void(core::services::TradingStyle s,
                       const std::string& historyDuration)> OnStyleChange;

private:
    core::services::TradingStyle m_tradingStyle = core::services::TradingStyle::Swing;
```

`setTradingStyle()` — pure UI side:
1. Apply preset to `m_ind` / `m_auto` / `m_setupSettings` / `m_timeframe`.
2. Clear all auto-detection state (`m_autoSupports`, `m_autoResistances`,
   `m_autoTrend`, `m_donchHi/Lo`, `m_keltUpper/Lower`, `m_autoFib`, `m_pivots`,
   `m_breakouts`, `m_breakoutSignal`, `m_setup`, `m_unguarded`).
3. Clear the bar arrays (`m_xs`, `m_idxs`, `m_opens`, …) so the next data load
   starts from scratch — prevents 1m bars sticking around in a Swing chart.
4. Set `m_loading = true`, `m_hasRealData = false`, `m_viewInitialized = false`.
5. Fire `OnStyleChange(s, preset.historyDuration)` so the host can prefetch.

The `setTradingStyle()` shape is intentionally close to the existing TF-change
path inside `DrawToolbar()` (around line 526) — that path already does
"set m_timeframe; m_needsRefresh = true; OnDataRequest(...)" — but we need to
also reset the per-style settings, hence a dedicated entry point.

### 3d. main.cpp wiring

```cpp
// New mode-switch queue (FIFO with 1s throttle per chart)
struct PendingStyleSwitch {
    int             chartIdx;
    core::services::TradingStyle style;
    std::string     historyDuration;
};
static std::deque<PendingStyleSwitch> g_pendingStyleSwitches;
static double                         g_nextStyleSwitchAllowed = 0.0;
static constexpr double kStyleSwitchThrottleSec = 1.0;

static void DrainStyleSwitchQueue() {
    if (g_pendingStyleSwitches.empty() || !g_IBClient) return;
    double now = glfwGetTime();
    if (now < g_nextStyleSwitchAllowed) return;
    auto p = g_pendingStyleSwitches.front();
    g_pendingStyleSwitches.pop_front();
    auto& ce = g_chartEntries[p.chartIdx];
    if (!ce.win) return;
    g_IBClient->CancelHistoricalData(ce.histId);
    ce.pendingBars        = core::BarSeries{};
    ce.pendingBars.symbol    = ce.win->getSymbol();
    ce.pendingBars.timeframe = p.style /* TF derived */;
    g_IBClient->ReqHistoricalData(ce.histId, ce.win->getSymbol(),
                                  p.historyDuration.c_str(),
                                  core::TimeframeIBBarSize(ce.win->getTimeframe()),
                                  ce.win->/* useRTH from m_useRTH */);
    g_nextStyleSwitchAllowed = now + kStyleSwitchThrottleSec;
}
```

Called once per frame from `RenderTradingUI()` next to
`PushUnguardedHintsToWindows()`. Empty-list early-out keeps the cost at one
load + compare per frame when idle.

`SpawnChartWindow()` wires `OnStyleChange`:

```cpp
e.win->OnStyleChange = [idx](core::services::TradingStyle s,
                             const std::string& historyDuration) {
    auto& ce = g_chartEntries[idx];
    PendingStyleSwitch p{ idx, s, historyDuration };
    g_pendingStyleSwitches.push_back(p);
    // chart-modes.cfg is rewritten lazily on next save trigger (see 3e).
};
```

`ReqChartData()` already takes a duration via `core::TimeframeIBDuration(tf)`.
We need an overload that accepts a custom duration string so style switches
can pass their preset's `historyDuration` instead. Same logic, one extra
parameter.

### 3e. Persistence (`chart-modes.cfg`)

Same pattern as `WatchlistsFilePath()` — atomic write via `.tmp` + `rename`.

Format:
```
INSTANCE:0
SYMBOL:AAPL
STYLE:Swing
INSTANCE:1
SYMBOL:NVDA
STYLE:Scalping
…
```

Persistence triggers (mirrors `SaveWatchlistsFile()`):
- `setTradingStyle()` → enqueue dirty flag (don't write inside the UI tick).
- Once-per-second flush from the main loop (`SaveChartModesIfDirty()`).
- `DestroyTradingWindows()` flushes synchronously before tearing down.

Load:
- `FinishConnect(false)` (initial connect): after `CreateTradingWindows()`,
  walk file blocks and apply via `setTradingStyle()` per chart that exists at
  the matching `INSTANCE:` index. Symbols not in the file → leave at default
  (`Swing`). The symbol-match check (`SYMBOL:`) is a sanity guard so a saved
  "AAPL = Scalping" doesn't apply if the user had since changed that chart's
  symbol to something else — fall back to default in that case.
- `FinishConnect(true)` (reconnect): no-op for chart-modes; the chart already
  has a style in memory and we just re-issue its current preset's history.

`ApplyChartModesFromFile()` is called at the same point in
`FinishConnect(false)` where `LoadWatchlistsFromFile()` is currently called.

### 3f. Toolbar UI

`DrawToolbar()` — between the timeframe combo (around line 522) and the
`useRTH` checkbox, add a Style combo:

```cpp
const char* styleLabels[] = { "Scalping", "Day Trading", "Swing", "Investment" };
ImGui::SetNextItemWidth(em(110));
int curStyleIdx = static_cast<int>(m_tradingStyle);
if (ImGui::Combo("##style", &curStyleIdx, styleLabels, IM_ARRAYSIZE(styleLabels))) {
    setTradingStyle(static_cast<core::services::TradingStyle>(curStyleIdx));
}
if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Trading style preset.\n"
                      "Sets timeframe + history horizon + analysis params.\n"
                      "Switching modes cancels current bars and prefetches the new horizon.");
}
```

The TF combo stays in the toolbar but becomes **read-only display** — the user
can no longer pick a different TF from it once a style is active. Replace the
existing `BeginCombo("##tf", ...)` block with a `Text(TimeframeLabel(...))`
followed by a small note that says "(via Style)". Rationale: the whole point
is hard-binding; leaving the combo enabled invites silent drift.

(Alternative: keep the TF combo enabled but treat any user-change as
"switch to Custom" — same as the watchlist preset combo. v2 polish; v1 = lock.)

Title bar continues to show `"Chart AAPL G1###chart0"`. The style is implicit
in the timeframe label (1m → Scalping, 15m → Day Trading, etc.); no need for
a `[SCALP]` badge in v1.

### 3g. ReqId / data dependencies

None new. Existing chart hist/ext/mkt reqIds are reused — a style switch is a
re-issue on the same `histId`, not a new subscription.

The mode-switch queue plus the existing `kReconnectIntervalSec` 5-second
reconnect cadence stay independent — no shared timer.

---

## 4. Algorithms (reference)

### 4a. Mode-switch sequence (single chart)

```
user picks new style in combo
  → ChartWindow::setTradingStyle(new):
        ApplyPreset(GetPreset(new), m_ind, m_auto, m_setupSettings, m_timeframe)
        clear all m_auto* / m_donch* / m_kelt* / m_autoFib / m_pivots / m_breakouts /
              m_breakoutSignal / m_setup / m_unguarded
        clear m_xs/m_idxs/m_opens/m_highs/m_lows/m_closes/m_volumes
        m_loading = true; m_hasRealData = false; m_viewInitialized = false
        m_tradingStyle = new
        OnStyleChange(new, preset.historyDuration)
  → main.cpp lambda:
        push {idx, style, duration} to g_pendingStyleSwitches
  → next frame DrainStyleSwitchQueue():
        if now < g_nextStyleSwitchAllowed → wait
        cancelHistoricalData(ce.histId)
        reqHistoricalData(ce.histId, sym, duration, barSize(tf), useRTH)
        g_nextStyleSwitchAllowed = now + 1.0
  → IBKR fires historicalData callbacks → SetHistoricalData(series)
  → ComputeIndicators() → DetectStructure() → overlays + setup + unguarded
     repopulate atomically.
```

### 4b. Throttle

```
constexpr kStyleSwitchThrottleSec = 1.0;  // 1 request per second app-wide
DrainStyleSwitchQueue: pop 1 request, fire it, set g_nextStyleSwitchAllowed.
```

10 charts batch-switching = 10 seconds of staggered firing. IB's
60-requests-per-10-min limit is 60/600s = 0.1/s, so 1/s leaves 10× headroom.

### 4c. Persistence flush cadence

```
g_chartModesDirty = false
on setTradingStyle():       g_chartModesDirty = true
on RenderTradingUI() tail:
  if g_chartModesDirty && (now - g_lastChartModesSave) > 1.0:
      SaveChartModesFile()
      g_lastChartModesSave = now
      g_chartModesDirty = false
on DestroyTradingWindows():
  if g_chartModesDirty: SaveChartModesFile()  // synchronous
```

---

## 5. Task breakdown

### Task A — `TradingStyle.h` + `ApplyPreset()` + tests
**Status:** TODO

- [ ] Create `src/core/services/TradingStyle.h` with `TradingStyle` enum,
      `TradingStyleLabel` / `TradingStyleShort`, `StylePreset` struct,
      `GetPreset(style)` returning the four canonical presets, and the
      templated `ApplyPreset(preset, ind, a, s, tf)` helper.
- [ ] `tests/test_chart_analysis.cpp` (or new `tests/test_trading_style.cpp`)
      add `[style]` cases:
  - [ ] `GetPreset(Scalping).timeframe == M1`, `historyDuration == "2 D"`,
        `swingK == 3`, `riskPct == 0.5`, `setupOverlay == false`.
  - [ ] `GetPreset(DayTrading).timeframe == M15`, etc. for all four modes.
  - [ ] `ApplyPreset` against hand-rolled stub `Ind`/`Auto`/`Setup` structs:
        every overridable field changed; non-listed fields unchanged.
  - [ ] Round-trip: apply Scalping then Investment to the same stubs — final
        state matches `GetPreset(Investment)` field-for-field (no carry-over
        from the previous mode).
- [ ] If new test file: append to `tests-core` source list in
      `tests/CMakeLists.txt`.
- [ ] `cmake --build build --target tests-core -j$(nproc)` clean; `ctest` green.

### Task B — ChartWindow integration
**Status:** TODO

- [ ] `ChartWindow.h`:
  - [ ] `core::services::TradingStyle m_tradingStyle = ...::Swing;`
  - [ ] `void setTradingStyle(...)`, `tradingStyle() const`.
  - [ ] `OnStyleChange` callback decl.
  - [ ] `#include "core/services/TradingStyle.h"`.
- [ ] `ChartWindow.cpp`:
  - [ ] `setTradingStyle()` body per §3c.
  - [ ] `DrawToolbar()` Style combo per §3f. Lock the existing TF combo
        (replace `BeginCombo("##tf", …)` with a `Text` showing the bound TF
        followed by a tooltip "Set via Style").
- [ ] No change to `DetectStructure()` / `RebuildFlatArrays()` /
      `ComputeIndicators()` — those already trigger off `m_timeframe` and
      `m_closes` content. Setting `m_loading = true` in `setTradingStyle()` is
      enough to suppress half-rendered overlays during the gap.
- [ ] Build `ibkr-trading-app` clean (no new warnings).

### Task C — main.cpp wiring (mode-switch queue + ReqChartData duration override)
**Status:** TODO

- [ ] `g_pendingStyleSwitches` deque + `g_nextStyleSwitchAllowed` time
      gate + `kStyleSwitchThrottleSec` constant.
- [ ] `DrainStyleSwitchQueue()` per §3d. Called once per frame from
      `RenderTradingUI()` near `PushUnguardedHintsToWindows()`.
- [ ] Add `ReqChartData(...)` overload (or extra param) accepting a duration
      string override; the existing call sites continue to use
      `TimeframeIBDuration(tf)`.
- [ ] `SpawnChartWindow()` wires `OnStyleChange` lambda per §3d.
- [ ] Manual smoke-test gate (deferred from automated CI):
  - [ ] Single chart, switch Scalping → Day Trading → Swing → Investment.
        Each switch shows blank-chart-with-spinner for ~1 frame, then loaded
        bars at the new TF with the correct overlay defaults.
  - [ ] Open 6 charts, set them all to Scalping in one click each (rapid
        consecutive changes). Console shows 6 historical requests fired with
        ~1s spacing; no IB pacing-violation messages.

### Task D — Persistence (`chart-modes.cfg`)
**Status:** TODO

- [ ] `ChartModesFilePath()` returns
      `~/.config/ibkr-trading-app/chart-modes.cfg` (mirror
      `WatchlistsFilePath`).
- [ ] `SaveChartModesFile()` — atomic write per §3e format.
- [ ] `LoadChartModesFromFile()` returns `vector<{instance, symbol, style}>`.
- [ ] `g_chartModesDirty` flag set in the `OnStyleChange` lambda.
- [ ] Once-per-second flush in `RenderTradingUI()` tail, plus synchronous
      flush in `DestroyTradingWindows()`.
- [ ] `FinishConnect(false)` calls `LoadChartModesFromFile()` after
      `CreateTradingWindows()`, walks blocks and applies via
      `setTradingStyle()` (only when the saved symbol matches the chart's
      current symbol — otherwise leave at default `Swing`).
- [ ] Manual: kill the app while charts are on `Scalping NVDA` and
      `Investment AAPL`; restart, reconnect, verify both charts come back in
      their saved styles with the correct timeframe + horizon.

### Task E — Docs + sanity
**Status:** TODO

- [ ] Update `.claude/rules/task-history.md` with **Phase 13 — Trading-Style
      Modes** and individual task entries.
- [ ] Update `.claude/rules/architecture.md` with a new section "Trading
      Styles" documenting `TradingStyle.h`, the per-chart `m_tradingStyle`
      member, the mode-switch queue / throttle, and the persistence file path.
- [ ] Update root `README.md` with a one-paragraph user-facing description of
      the four modes.
- [ ] Final acceptance: `cmake --build build -j$(nproc)` clean, `ctest` green,
      `DISPLAY=:1 ./build/ibkr-trading-app` smoke test of all four modes plus
      a kill-and-restart to verify persistence.

---

## 6. Acceptance / verification

For each task, before marking complete:

1. `cmake --build build -j$(nproc)` — clean (no new warnings).
2. `ctest --test-dir build --output-on-failure` — all tests pass (including
   the new `[style]` cases under `tests-core`).
3. Manual (Tasks B onward):
   - Connect paper account.
   - Open AAPL chart → default mode = Swing → 1D bars, ~1Y of history, S/R +
     trend + auto-fib visible.
   - Switch combo to Scalping → blank-chart spinner ~1s → 1m bars, 2 trading
     days of history, S/R + zones + pivots + breakouts visible (no auto-fib,
     no trend), VWAP line on, no ±σ bands.
   - Switch to Day Trading → 15m, 20D, S/R + zones + trend + pivots +
     breakouts + VWAP + bands visible.
   - Switch to Investment → 1W, 5Y, S/R + trend + auto-fib visible (no zones,
     no breakouts, no VWAP).
   - For each mode, take an arbitrary unrelated action (toggle an indicator,
     pan, zoom). The auto-detected S/R / setup / unguarded-stop suggestion
     must NOT change as a result.
   - Kill + restart: each chart returns to its last-set mode for the same
     symbol.
4. Update `.claude/rules/task-history.md` and root `README.md`.

---

## 7. Risks / sharp edges

- **Mode-switch race with extend-history**: if the user pans left to load
  older bars, the `extId` request is in flight. Switching mode in that window
  should cancel the extend-request too (`g_IBClient->CancelHistoricalData(ce.extId)`),
  otherwise older bars from the *previous* TF could land after the new TF's
  bars arrive and corrupt the series. Add the cancel to `setTradingStyle()` /
  `DrainStyleSwitchQueue()`.
- **Symbol change during mode switch**: if the user types a new symbol while
  the mode-switch fetch is enqueued, the queue entry's `historyDuration` is
  fine but the IB call needs the *new* symbol. Resolve by reading
  `ce.win->getSymbol()` at drain time, not at enqueue time — already what the
  draft pseudocode does. Document so future-me doesn't switch to capturing
  the symbol at enqueue.
- **Default chart at first connect**: a fresh chart with no `chart-modes.cfg`
  entry stays at Swing → 1D / 6 M (existing behaviour). Only after the user
  picks a mode does the file gain an entry. No silent migration.
- **`indVwapBands` member doesn't exist yet**: `ApplyPreset` references it,
  but the field is added by the VWAP plan. Land both plans together (or land
  VWAP first and gate the `ind.vwapBands = …` assignment behind an
  `#ifdef IBKR_HAVE_VWAP_BANDS`-style preprocessor — but cleaner to just
  schedule them as Phase 13 paired plans).
- **TF combo lock surprises users who currently change TF freely**: the
  release notes need a one-line "Trading Style now controls timeframe; switch
  modes to change TF". The Style tooltip already says it; the locked TF combo
  has a tooltip "Set via Style".
- **Mode-switch queue starvation**: if the user spams the combo, only the
  most-recent click matters. Implementation: when enqueuing, drop any prior
  pending entries for the same `chartIdx` so the queue only carries the
  latest switch per chart. (If we don't, the user could see chart bars hop
  through every intermediate mode before settling.)
- **History duration vs IB STK limits**: every preset is well within IB's
  per-bar-size historical caps. If a future style exceeds the cap (e.g. a
  hypothetical "1m × 30 D"), `reqHistoricalData` will silently truncate or
  return error 162 — would be caught by the existing IB error-handler
  pipeline.

---

## 8. Out of scope (explicitly deferred)

- **Custom mode** — user-defined preset with their own TF + params. v2; for
  now the four canonical modes cover the surveyed use cases.
- **Per-symbol mode** — different mode per (instance, symbol) pair beyond
  what the persistence file already tracks. v1 keys on `(instance, symbol)`
  but doesn't migrate the mode when the user types a new symbol — they
  inherit the current chart's mode until they explicitly pick another.
- **Mode badge in title bar** — `Chart AAPL G1 [SCALP]` for at-a-glance
  identification. Skipped to avoid title-bar clutter; the TF in the combo
  is enough.
- **Auto-mode by symbol class** — futures default to Scalping, ETFs to
  Swing, etc. v3 polish.
- **Cross-chart mode sync** — when Group G1 broadcasts a symbol, every chart
  in G1 keeps its own mode (intentional — same symbol on different
  timeframes is the point). Not deferred — explicitly the right behaviour.

---

## 9. Update log

| Date | Change | By |
|---|---|---|
| 2026-04-29 | Plan written (draft) | claude / jose |
