# Plan: ReplayWindow Indicators + Date-Range Loading

**Status:** DRAFT — awaiting user sign-off before any code lands.
**Owner:** Jose
**Phase:** 14.x (incremental on top of Phase 14 Replay Window).
**Goal:** Bring the ReplayWindow to feature-parity with ChartWindow's indicator stack
(SMA20, SMA50, EMA20, Bollinger Bands, VWAP + ±σ bands, Volume sub-plot, RSI sub-plot)
and let the user load **a date range** (e.g. 2026-04-01 → 2026-04-15) instead of a
single day, so longer-period indicators (SMA50, BB20, RSI14) and multi-day swing/trend
analysis become meaningful inside Replay.

This file is the source of truth across sessions — update the **Status** line and
the per-task checkboxes as work progresses.

---

## 1. Why now / what changes for the user

ReplayWindow currently:
- Loads a single trading day via `core::HistoricalDay { symbol, date, bars, ticks, userFills }`.
- Renders candles + cursor line + fill markers via `ui::ChartRender.h` shared helpers.
- Has **no** SMA / EMA / BB / VWAP / Volume / RSI overlays.
- The session combo (PreMkt / Intraday / PostMkt / All) selects a window inside the loaded day.

After this plan:
- Single day OR a contiguous range of days. Date toolbar becomes `From: 2026-04-01  To: 2026-04-15` (with the existing `DatePicker.h` popup, two of them).
- All seven Chart-style indicators selectable from a toolbar `IndicatorSettings` group, defaults matching whatever TradingStyle the user picks (or just sensible defaults for replay's M1).
- Volume bar plot + RSI line plot stack below the price plot, identical to ChartWindow.
- Indicators compute over the **full loaded range**, but the **cursor / playback** still respects the session filter inside the currently-visible day (so a user replaying intraday on AAPL still gets to play 09:30–16:00 ET, but the SMA50 line at the open is not garbage because it had 50+ bars from the previous days to warm up).

---

## 2. Scope

### 2a. In v1 of this plan
- Extract `CalcSMA` / `CalcEMA` / `CalcBollingerBands` / `CalcRSI` from `ChartWindow.cpp`
  into `core::services::ChartAnalysis.h` as pure inline functions (matches the
  pattern of `SessionVwap` already living there).
- ChartWindow becomes a thin wrapper that calls the new free functions (no
  behaviour change; existing `[analysis]`/`[vwap]` test coverage continues to apply).
- ReplayWindow gains `IndicatorSettings` + indicator vectors + `ComputeIndicators()`
  + an indicators toolbar row + a volume sub-plot + an RSI sub-plot.
- ReplayWindow gains a date-range UI (`From` / `To`) and a `core::HistoricalRange`
  data model (or an extended `HistoricalDay` — see §3a for the tradeoff).
- `~/.config/ibkr-trading-app/replay-windows.cfg` extended with `DATE_FROM:` /
  `DATE_TO:` (back-compat with the existing single `DATE:` line — old configs load
  with both fields equal to the saved date).
- New test cases in `[chart-indicators]` Catch2 tag for the extracted helpers
  (purely a regression net — the math is unchanged).
- New test cases in `[replay-range]` Catch2 tag for the date-range helpers
  (parsing, validation, session-range computation across day boundaries).

### 2b. Deferred (captured here so we don't lose them)
- **Multi-day tick replay.** `reqHistoricalTicks` returns ≤ 1000 ticks/call paced
  at ≤ 60 req / 10 min / contract. A liquid-stock day already takes 60–90 minutes
  to fetch on first run; a week's worth would be 5–10× that. v1 keeps tick fills
  **single-day only** — the engine's tick-evaluation path falls back to bar-mode
  for bars outside the tick-loaded day. The "Tick fills" checkbox toggles tick
  loading for the **last day** of the loaded range only (the cursor-day at the
  moment of toggle).
- **Indicator-aware setup overlay / unguarded-stop guard inside Replay.** The
  Phase 12 setup overlay and the Phase 12 unguarded-stop guard already work on
  a per-`AutoLevelSnapshot` basis from chart instances. v2 of this plan can wire
  them into ReplayWindow via the same path. Captured separately so this plan
  stays small.
- **Per-day session shading inside multi-day ranges.** Plot a faint grey vertical
  band between trading days when the loaded range spans more than one date. v2.
- **Per-style indicator defaults inside ReplayWindow.** ChartWindow's
  `setTradingStyle` stamps `IndicatorSettings` from the preset. ReplayWindow
  doesn't have a TradingStyle combo (it has its own `TF` combo) — v1 just
  defaults to a sensible "Day Trading" preset (VWAP+bands ON, BB ON, SMA20 ON).
  v2 can add a TradingStyle combo to ReplayWindow if useful.

### 2c. Open questions for the user (default values are my pick — override anything)
1. **Single-day backwards compat.** The existing `DATE:` line in
   `replay-windows.cfg` should keep working, mapping to `DATE_FROM=DATE_TO=DATE`.
   **Default: yes, back-compat.**
2. **Maximum range length.** IB allows up to 365 D for `1 min` bars per request,
   though the practical pacing limit (60 req / 10 min / contract) means very long
   ranges hit throttle. **Default: cap UI at 30 days for M1 / M5; 1 year for M15
   and above.** (Defensive cap; user can extend in code if needed.)
3. **Range that spans non-trading days.** A range like
   `From: 2026-04-10 (Friday) → To: 2026-04-13 (Monday)` will return only the
   trading days IB has data for. **Default: silently honour whatever IB returns;
   show "Loaded 2 trading days (2026-04-10, 2026-04-13)" in the status bar.**
4. **Cursor-bar session classification with multi-day ranges.** When the cursor is
   inside day-1 of a 5-day range, the "Intraday" filter should let the cursor
   play through 09:30–16:00 of *that day*, then the user can scrub forward to
   day-2. **Default: yes — `BarRangeForSession` per-day-aware (re-detect session
   bounds when the cursor crosses a day boundary).** Alternative: treat the
   entire range as a single concatenated session — rejected, breaks scrubber
   semantics for users who want to compare same-time-of-day across days.
5. **Indicator state during scrubbing.** Computing on the full range means the
   SMA/EMA/BB/RSI vectors are sized once and stable across the scrub. VWAP
   resets per ET-day boundary (already handled by `SessionVwap`'s
   `sessionStarts`). **Default: this is the right behaviour, no toggle needed.**
6. **Volume / RSI sub-plot heights.** ChartWindow uses 90px reserved space when
   each is on. **Default: same in Replay.**

---

## 3. Architecture

### 3a. Data model — `HistoricalDay` extended vs. new `HistoricalRange`?

Two options. Both are 1–2 lines of code; the cost is downstream API noise.

**Option A: extend `HistoricalDay` to hold a date *range* under the same name.**
- Rename `date` → `dateFrom`, add `dateTo`. (Or just leave `date` and reinterpret
  it as the start.)
- `bars` already a `std::vector<Bar>` — works for any range.
- Fewest changes. Misleading type name (`HistoricalDay` no longer holds a day).

**Option B: introduce `HistoricalRange { dateFrom, dateTo, bars, ticks, userFills }`
and keep `HistoricalDay` as a thin alias / wrapper for the single-day case.**
- Cleaner naming. Forces every call site (engine, window, cache, persistence) to
  decide which type they want.
- More files touched.

**Default: option B with the alias path.** Concretely:
```cpp
namespace core {
struct HistoricalRange {
    std::string                       symbol;
    std::string                       dateFrom;      // "YYYY-MM-DD"
    std::string                       dateTo;        // "YYYY-MM-DD" (==dateFrom for single day)
    std::vector<core::Bar>            bars;
    std::vector<core::HistoricalTick> ticks;         // single-day-bound; only populated for the cursor's day
    std::string                       ticksDate;     // which day the ticks cover (empty = none)
    std::vector<core::Fill>           userFills;     // filtered to (symbol, [from..to])
    std::time_t                       fetchedAt = 0;
};
// HistoricalDay kept as an alias for the one-day case (call sites that pass
// dateFrom==dateTo continue to compile unchanged).
using HistoricalDay = HistoricalRange;
}
```
Rationale: zero churn at most call sites (engine doesn't care, ReplayEngine just
walks `bars`), but the field name is honest. Tests that build a single-day fixture
just leave `dateTo` equal to `dateFrom`.

### 3b. Indicator extraction — `CalcSMA` / `CalcEMA` / `CalcBollingerBands` / `CalcRSI`

Currently lives as static methods on `ChartWindow` (`ChartWindow.cpp:4098–4156`).
Move to `src/core/services/ChartAnalysis.h` as inline free functions in
`core::services` (matches `SessionVwap`, `FindSwings`, etc.):

```cpp
namespace core::services {
inline std::vector<double> SMA(const std::vector<double>& close, int period);
inline std::vector<double> EMA(const std::vector<double>& close, int period);
struct BollingerBands { std::vector<double> mid, upper, lower; };
inline BollingerBands ComputeBollinger(const std::vector<double>& close, int period, double sigma);
inline std::vector<double> RSI(const std::vector<double>& close, int period);
}
```

`ChartWindow::ComputeIndicators` becomes:
```cpp
m_sma1 = core::services::SMA(m_closes, m_ind.smaPeriod1);
m_sma2 = core::services::SMA(m_closes, m_ind.smaPeriod2);
m_ema  = core::services::EMA(m_closes, m_ind.emaPeriod);
auto bb = core::services::ComputeBollinger(m_closes, m_ind.bbPeriod, m_ind.bbSigma);
m_bbMid = std::move(bb.mid); m_bbUpper = std::move(bb.upper); m_bbLower = std::move(bb.lower);
m_rsi  = core::services::RSI(m_closes, m_ind.rsiPeriod);
// VWAP path unchanged — already calls core::services::SessionVwap.
```

The `ChartWindow::CalcSMA` etc. static method declarations are removed from
`ChartWindow.h`. Callers (just `ComputeIndicators` itself plus a couple of
auto-analysis edges) switch to the new free functions. No behaviour change.

**Naming choice.** The plan uses `SMA` / `EMA` / `RSI` (all-caps, matching the
acronym) instead of preserving the verb-prefixed `CalcSMA`. Consistent with
`SessionVwap`, `LinearRegression`, `DonchianBands`, `ClassicPivots`,
`LargestSwingSpan`, `RoundToTick`, `AvoidRoundNumber`, `SuggestSetup`,
`SuggestStopForPosition`, `PositionSizeShares`, `ComputeOrderImpact`,
`PreviewStopTarget`, `FindSwings`, `FindBreakouts`, `KeepTopN`, `ClusterLevels`,
`ATR` already in `ChartAnalysis.h` — no `Calc` prefix on any of them. Bollinger
keeps the `Compute` verb because the noun alone (`Bollinger`/`BB`) reads as a
type, not a function.

### 3c. ReplayWindow indicator integration

`src/ui/windows/ReplayWindow.h` gains:

```cpp
struct IndicatorSettings {
    bool sma20 = false;   bool sma50 = false;   bool ema20 = false;
    bool bbands = true;   bool vwap = true;     bool vwapBands = true;
    bool volume = true;   bool rsi = false;
    int  smaPeriod1 = 20; int smaPeriod2 = 50;  int emaPeriod = 20;
    int  bbPeriod   = 20; double bbSigma  = 2.0;
    int  rsiPeriod  = 14;
};
// Defaults match a "Day Trading" feel — VWAP+bands and BB on, SMA50 off.
// Persisted per-window in replay-windows.cfg (one byte per toggle, periods as ints).

IndicatorSettings    m_ind;
std::vector<double>  m_sma1, m_sma2, m_ema, m_bbMid, m_bbUpper, m_bbLower, m_rsi;
std::vector<double>  m_vwap, m_vwapSd1Up, m_vwapSd1Dn, m_vwapSd2Up, m_vwapSd2Dn;

void ComputeIndicators();
void DrawVolumeChart();
void DrawRsiChart();
```

`SetDay(const HistoricalRange&)` (renamed from the current `SetDay(const HistoricalDay&)`
— actually a no-op rename since `HistoricalDay` is an alias for `HistoricalRange`)
calls `ComputeIndicators()` after rebuilding the flat OHLCV arrays. The cursor
auto-tick path (`ApplyEngineTick`) does **not** recompute indicators per frame —
they're stable across the loaded range and only need recomputation when:
- the user toggles an indicator on/off (cheap — recompute everything)
- the user changes a period in the indicators settings popup
- a new range is loaded

`m_idxs` / `m_xs` / `m_opens` / `m_highs` / `m_lows` / `m_closes` / `m_volumes`
already exist. `RebuildFlatArrays()` already populates them in `SetDay`. Just add
the `ComputeIndicators()` call at the tail.

### 3d. ReplayWindow chart layout (price + volume + RSI sub-plots)

`DrawChart()` currently calls `BeginPlot` once for the price plot. Mirror
`ChartWindow::DrawCandleChart`'s sub-plot stack — three vertically-stacked
ImPlot frames sharing the same X axis:

```
┌─────────────────────────────────────────────────┐
│ Price chart (candles + indicator overlays)      │   ← takes most of the height
│                                                 │
│                                                 │
├─────────────────────────────────────────────────┤
│ Volume bar chart (only if m_ind.volume)         │   ← 90px reserved
├─────────────────────────────────────────────────┤
│ RSI line chart (only if m_ind.rsi)              │   ← 90px reserved
└─────────────────────────────────────────────────┘
```

ImPlot sub-plot height calc lifted from ChartWindow.cpp:3255–3263 verbatim.
Cursor line (yellow vertical) renders on **all three** plots so the user can
correlate price action with volume and RSI at the cursor's bar.

### 3e. Toolbar — new indicators row

ReplayWindow's toolbar currently has: `[G1] [Symbol] [Date] [Session] [TF] [Pause] [⏪] [▶/⏸] [⏩] [Speed] [Mode] [Equity] [TickFills] [Reset] [PAPER]`.

After this plan:

**Row 1 (existing) — date picker becomes a *range*:**
```
[G1] [Symbol] [From: 2026-04-01] [To: 2026-04-15] [Session] [TF] [Pause] [⏪] [▶/⏸] [⏩] [Speed] [Mode] [Equity] [TickFills] [Reset] [PAPER]
```

`From` / `To` are two compact buttons that open `DrawDatePicker` popups (already
shared from `WshCalendarWindow`). When `From == To`, the range is a single day
(matches current behaviour). When `To < From`, validate-and-revert (don't fire
`OnDataRequest`). `Load` is implicit on either pick (or kept as an explicit
button to control IB calls per §2c.2 30-day cap).

**Row 2 (new) — indicators row, FlexRow-wrapped:**
```
[SMA20] [SMA50] [EMA20] [BB] [VWAP] [±σ] [Vol] [RSI]   [Indicators...]
```
`[Indicators...]` opens a popup with `InputInt` for SMA20/SMA50/EMA20/BB/RSI
periods + `InputDouble` for BB sigma. Mirror `ChartWindow::DrawAnalysisToolbar`'s
indicator section verbatim (cf. `ChartWindow.cpp:747–765`).

### 3f. Date-range loading — `OnDataRequest` signature change

Current callback:
```cpp
std::function<void(const std::string& sym, const std::string& date,
                   core::services::ReplaySession session,
                   core::Timeframe tf)> OnDataRequest;
```

New callback:
```cpp
std::function<void(const std::string& sym,
                   const std::string& dateFrom,
                   const std::string& dateTo,
                   core::services::ReplaySession session,
                   core::Timeframe tf)> OnDataRequest;
```

`main.cpp` `SpawnReplayWindow`'s lambda updates: compute the IB `durationStr`
from the range span (`(dateTo - dateFrom + 1) days`), pick the bar size from
`tf`, fire `ReqHistoricalData`. The end-date for IB's request is `dateTo
23:59:59 ET`. ReqId routing unchanged (still `11000 + idx*100 + 0`).

### 3g. Persistence — `replay-windows.cfg` schema bump

Current format (excerpt):
```
INSTANCE:0
SYMBOL:AAPL
DATE:2026-04-15
SESSION:1
TF:0
SPEED:1.0
MODE:0
CURSOR:132
EQUITY:100000.0
TICKFILLS:0
```

New format adds:
```
DATE_FROM:2026-04-01
DATE_TO:2026-04-15
IND_FLAGS:181        # bit-packed: SMA20|SMA50|EMA20|BB|VWAP|VWAP_SD|VOL|RSI
IND_SMA1:20
IND_SMA2:50
IND_EMA:20
IND_BB_PERIOD:20
IND_BB_SIGMA:2.0
IND_RSI:14
```

**Back-compat parse rule:** if the file has only `DATE:` (no `DATE_FROM:` / `DATE_TO:`),
load both `dateFrom = dateTo = DATE`. If the file has no `IND_*` lines, use
`IndicatorSettings{}` defaults (the `= true` defaults on bbands/vwap/vwapBands/volume).

`SaveReplayWindowsFile()` always writes the new keys; the legacy `DATE:` key is
**dropped** on save (single-day windows just have `DATE_FROM == DATE_TO`).

---

## 4. Tasks

### Task A — Indicator helper extraction + tests
**Files:** `src/core/services/ChartAnalysis.h` (edit), `src/ui/windows/ChartWindow.h` (edit), `src/ui/windows/ChartWindow.cpp` (edit), `tests/test_chart_analysis.cpp` (edit).

1. Add `core::services::SMA`, `EMA`, `BollingerBands` struct + `ComputeBollinger`, `RSI` to `ChartAnalysis.h` as inline free functions. Code is a near-verbatim copy of the existing `ChartWindow::CalcSMA/CalcEMA/CalcBollingerBands/CalcRSI` (only namespace + return-type change for BB).
2. Remove the four static method declarations from `ChartWindow.h` (lines 473–478) and the four definitions from `ChartWindow.cpp` (lines 4098–4156).
3. Update `ChartWindow::ComputeIndicators()` to call the new free functions.
4. Add `[chart-indicators]` Catch2 tag to `tests/test_chart_analysis.cpp` with regression cases:
   - `SMA(period=3)` on `{1,2,3,4,5,6}` → `{0,0,2,3,4,5}`.
   - `SMA(period=0)` and `n<period` → all-zeros.
   - `EMA(period=2)` exact recovery of expected smoothed sequence.
   - `ComputeBollinger(period=3, sigma=2)` on a known sequence — assert `mid` equals `SMA`, `upper-mid == mid-lower`, value at known index.
   - `RSI(period=2)` on a strictly-rising sequence → 100, on strictly-falling → 0, on a mixed sequence → known value at the warm-up index.
   - Edge: `n <= period` → all-zeros (no crash).

   Target: ≥ 6 cases / ≥ 30 assertions. Behaviour-preserving — existing ChartWindow tests continue to pass.

### Task B — `HistoricalRange` model
**Files:** `src/core/models/ReplayData.h` (edit).

1. Rename `HistoricalDay` → `HistoricalRange`, add `dateFrom` / `dateTo` / `ticksDate`.
2. Add `using HistoricalDay = HistoricalRange;` alias so existing call sites compile unchanged.
3. Update the comment block to reflect the multi-day semantics.

No test changes needed (the struct is data-only; consuming tests in `[replay]` already use the alias). One-line readability win for callers that read `dateFrom`/`dateTo`.

### Task C — ReplayWindow date-range UI
**Files:** `src/ui/windows/ReplayWindow.h` (edit), `src/ui/windows/ReplayWindow.cpp` (edit), `src/main.cpp` (edit).

1. Replace `m_dateBuf[16]` with `m_dateFromBuf[16]` / `m_dateToBuf[16]` (both default to `"2026-04-15"` so a fresh window opens as a single-day range).
2. Replace the single date `DrawDatePicker` button with two buttons (`From: …` / `To: …`).
3. Validate `dateTo >= dateFrom` on pick; revert + show a tooltip if invalid.
4. Cap the range per §2c.2 (30 days for M1/M5, 365 days otherwise).
5. Update `OnDataRequest` signature to take both dates. Update `main.cpp` `SpawnReplayWindow` lambda to compute the IB `durationStr` from the span and call `ReqHistoricalData`.
6. Status-bar update: include "Loaded N trading days (YYYY-MM-DD … YYYY-MM-DD)" when `dateFrom != dateTo`.
7. Persisted state: write `DATE_FROM` / `DATE_TO` instead of `DATE`. Back-compat-load `DATE` if found.

### Task D — ReplayWindow indicator integration
**Files:** `src/ui/windows/ReplayWindow.h` (edit), `src/ui/windows/ReplayWindow.cpp` (edit).

1. Add `IndicatorSettings m_ind` + the seven indicator vectors + `m_vwap*` quintet from §3c.
2. Add `void ComputeIndicators()` — copy `ChartWindow::ComputeIndicators` body verbatim (it's already a near-trivial chain of `core::services::*` calls after Task A).
3. Call `ComputeIndicators()` from the tail of `SetDay()` (where `RebuildFlatArrays()` is called).
4. Add `DrawVolumeChart()` + `DrawRsiChart()` — copy the implementations from `ChartWindow.cpp` (verbatim, modulo namespace and member references).
5. Update `DrawChart()` to allocate sub-plot heights for volume + RSI when `m_ind.volume` / `m_ind.rsi` is on. Use the same height-share formula as `ChartWindow::DrawCandleChart`.
6. Add the new indicators-row to `DrawToolbar()`. FlexRow-wrap. Mirror `ChartWindow::DrawAnalysisToolbar`'s indicator section (with the `±σ` checkbox gated on `m_ind.vwap`).
7. Add an `[Indicators...]` settings popup with period inputs + BB sigma. On any change, call `ComputeIndicators()` immediately.
8. Render BB/SMA/EMA/VWAP/VWAP-bands overlays inside the price plot (copy lines `ChartWindow.cpp:3302–3328` — the `PlotLine` block).
9. Cursor line (`RenderCursorLine` from `ChartRender.h`) renders on **all three** sub-plots, not just price.

### Task E — `replay-windows.cfg` schema bump
**Files:** `src/main.cpp` (edit — `SaveReplayWindowsFile` + `LoadReplayWindowsFromFile`).

1. Write `DATE_FROM:` / `DATE_TO:` instead of `DATE:`. Write the `IND_*` keys from §3g.
2. Loader: parse `DATE_FROM` / `DATE_TO` if present; fall back to `DATE` for legacy files. Parse `IND_FLAGS` as a single uint8 bitfield (`SMA20=1, SMA50=2, EMA20=4, BB=8, VWAP=16, VWAP_SD=32, VOL=64, RSI=128`) plus the period inputs. Apply via setters on the window instance (add `setIndicators(const IndicatorSettings&)` if needed).
3. After `setIndicators` call, the window itself will call `ComputeIndicators()` on the next `SetDay`, so no special wiring needed for restore.

### Task F — Tests for date-range + multi-day session detection
**Files:** `tests/test_replay.cpp` (edit).

Add `[replay-range]` Catch2 cases:
1. `BarRangeForSession` on a synthetic 5-day intraday bar array (8 bars per day at H1 = 40 bars total): `Session::Intraday` with cursor in day-3 returns `{first, last}` for *that day's* intraday range, not the whole 40-bar span.
2. Cross-day cursor advance: `SeekToTime(t)` where `t` falls in day-2 places the cursor at the correct global index and `BarRangeForSession` re-detects that day's bounds.
3. Single-day range (`dateFrom == dateTo`) behaves identically to the existing single-day path (regression net for existing `[replay]` cases).
4. Invalid range (`dateTo < dateFrom`) helper rejects with `valid=false`.

Target: ≥ 4 new cases / ≥ 20 assertions under `[replay-range]`. Existing 207 `[replay]` cases must continue to pass.

### Task G — Documentation
**Files:** `.claude/rules/architecture.md` (edit), `.claude/rules/testing.md` (edit), `.claude/rules/task-history.md` (edit).

- Architecture: update the "Replay Window" section to mention indicator parity and the multi-day range. Update the data-model line to list `HistoricalRange` (not `HistoricalDay`).
- Testing: add the `[chart-indicators]` and `[replay-range]` tags to the coverage list. Note that `tests-core` now exercises the four indicator helpers directly (instead of only via the ChartWindow integration path).
- Task history: append a new task line under Phase 14.

---

## 5. Sequencing & estimates

Suggested merge order (each task is one PR):
1. Task A — indicator helper extraction + tests (~0.5 day; pure refactor + tests)
2. Task B — `HistoricalRange` model (~0.25 day; tiny rename)
3. Task C — ReplayWindow date-range UI (~1 day)
4. Task D — ReplayWindow indicator integration (~1 day; mostly copying from ChartWindow)
5. Task E — persistence schema bump (~0.5 day)
6. Task F — date-range tests (~0.5 day)
7. Task G — docs (~0.25 day)

**Total: ~4 dev-days for v1, plus live-IB smoke test (manual) at the end.**

---

## 6. Acceptance checklist (for marking this plan done)

- [ ] All `tests-core` tests pass; new `[chart-indicators]` tag has ≥ 6 cases; new `[replay-range]` tag has ≥ 4 cases.
- [ ] Existing `[analysis]`, `[vwap]`, `[replay]`, `[setup]`, `[order-impact]`, `[style]` tags continue to pass with no changes (Task A is a pure refactor).
- [ ] `grep -rn "ChartWindow::Calc" src/` returns zero matches (the static methods are gone).
- [ ] ReplayWindow opens, lets the user set a `From` / `To` range up to 30 days at M1, fetches the bars, and renders candles + SMA/EMA/BB/VWAP overlays.
- [ ] Toggling SMA50 on a 5-day M1 range shows a continuous line (warm-up only at the very start of the range, not at every day boundary).
- [ ] Volume bar plot renders below the price plot when `Vol` is on; RSI line plot renders below it when `RSI` is on.
- [ ] Cursor line renders on all three sub-plots and tracks correctly.
- [ ] Scrubber spans the full multi-day range; playing through 5 days at 60× completes in < 7 minutes without UI stutter (frame > 16 ms < 5% of frames).
- [ ] Persistence: close the app with a multi-day Replay window open → reopen → range, indicators, periods all restored.
- [ ] Persistence: open an app that has a legacy single-day `replay-windows.cfg` (with `DATE:`, no `DATE_FROM:`) → window restores as a single-day range with indicator defaults.
- [ ] **Safety unchanged:** grep `src/ui/windows/ReplayWindow.cpp` for `PlaceOrder` / `cancelOrder` / `modifyOrder` returns zero matches.
- [ ] Live-IB smoke test: load AAPL for the past 5 trading days at M5; verify SMA50 + BB + VWAP all render correctly across the range.

---

## 7. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Indicator-helper refactor changes `ChartWindow` behaviour subtly (e.g. due to a copy/paste typo in the function body). | Task A's tests assert the *same outputs* on the *same inputs* the old code computed. Pre-merge: run `tests-ibkr` + `tests-core` with the refactor; manually compare a known chart screenshot before/after. |
| 30-day M1 range is ~7800 bars × full indicator stack — `ComputeIndicators` perf concern. | Indicators are O(n) (SMA/EMA/RSI rolling, BB is O(n×period)). 7800 × 20 = 156k ops for BB — sub-millisecond. Verified via a perf assertion in `[chart-indicators]` if we hit > 5 ms on a 10k-bar input. |
| User picks a 365-day M1 range and slams IB pacing. | UI cap from §2c.2: 30 days for M1/M5. Hard limit, not a soft warning. Status bar shows the cap when the user picks `To` > `From + 30d`. |
| Multi-day session-range detection (`BarRangeForSession` per-day-aware) introduces an off-by-one at day boundaries. | Task F's tests cover exactly this — synthetic 5-day fixture with cursor in day-3 must return day-3's session bounds. |
| Persistence back-compat breaks: an old config with `DATE:` causes a parse error and the user loses all replay-window restore state. | Loader treats unknown keys as ignored, missing keys as defaults, and `DATE:` as "use for both `dateFrom` and `dateTo`". Tested by hand-crafting an old-format config and asserting the loaded state matches expectation. |
| Tick-fills toggle confusion in multi-day range: ticks only cover one day, but the user can scrub across all days. | Status bar shows "Tick fills active for: 2026-04-15" when ticks are loaded but cursor is on a different day. Engine's tick path automatically falls back to bar-mode for bars outside `m_day.ticksDate`. |

---

## 8. References

- `architecture.md` — Replay Window section, Auto-analysis primitives in `ChartAnalysis.h`
- `testing.md` — `tests-core` source-list pattern, `[analysis]` umbrella tag style
- `.claude/plans/replay.md` — Phase 14 base plan (this is an incremental on top)
- `src/ui/windows/ChartWindow.{h,cpp}` — source of truth for the indicator stack we're cloning
- `src/ui/ChartRender.h` — shared candlestick + cursor rendering
- `src/ui/DatePicker.h` — date-picker popup we reuse for the `From` / `To` controls
