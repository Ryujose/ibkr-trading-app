# Plan: VWAP Primitive + ±σ Bands (Phase 13 companion)

**Status:** Both tasks landed (Tasks A/B as #59–#60). Tests-core 117/117 pass. Live IB smoke-test deferred to manual session.
**Owner:** Jose
**Goal:** VWAP already renders today (`ChartWindow::CalcVWAP` in
`ChartWindow.cpp:3566`, drawn via `ImPlot::PlotLine` at line 2761,
toolbar checkbox `m_ind.vwap`). Two gaps: (1) the math lives inside
ChartWindow as a static method so it can't be unit-tested without linking
ImGui/ImPlot; (2) ±1σ / ±2σ volume-weighted bands — a standard intraday
trader request — don't exist. This plan extracts the VWAP computation into
`core::services::ChartAnalysis.h` and adds the bands.

Companion to `.claude/plans/trading-styles.md` — the trading-style presets
toggle the VWAP and bands defaults per mode (Scalping ON / no bands; Day
Trading ON / bands ON; Swing/Investment OFF). Land both together so the
preset fields line up.

This file is the source of truth across sessions — update the **Status** line
and the per-task checkboxes as work progresses.

---

## 1. Approved scope

| # | Feature | Default | Notes |
|---|---|---|---|
| 1 | **`SessionVwap` primitive** in `core::services` | n/a | Pure free function; takes highs/lows/closes/volumes/sessionStarts and returns `VwapResult{vwap, sd1Up, sd1Dn, sd2Up, sd2Dn}`. Session-anchored — resets per session boundary. |
| 2 | **±1σ / ±2σ bands** rendering | OFF | Volume-weighted standard deviation around the running VWAP. Toggleable via a new `IndicatorSettings::vwapBands` field. |
| 3 | **VWAP toggle** (existing) | unchanged | The existing `m_ind.vwap` checkbox in `DrawToolbar()` keeps its current spot. The ±σ bands sub-toggle lives next to it (or in a small hover popup off the VWAP checkbox — see §3f). |
| 4 | **Mode integration** | per mode | Scalping: VWAP ON, bands OFF. Day Trading: VWAP ON, bands ON. Swing / Investment: VWAP OFF (daily/weekly bars don't have intraday session boundaries that make session-VWAP meaningful). |

## 2. Resolved decisions (from the conversation)

- **Session-anchored, not "anchored VWAP" (AVWAP)** — v1 resets at session
  open per the existing `CalcVWAP(intradayReset=true)` behaviour. Anchored
  VWAP from a major event (earnings, breakout) is a v2 polish.
- **Typical price `(H+L+C)/3`** as the per-bar price input — same as the
  existing implementation, same as TradingView / IB's default.
- **Bands derived from volume-weighted variance**, not naive stdev:
  ```
  varVw = (Σ vol × (typical - vwap)²) / Σ vol
  sd    = sqrt(varVw)
  ```
  Resets at the same session boundary as VWAP itself.
- **Zero-volume bar** carries the previous VWAP and bands forward (no
  division-by-zero, no flicker on illiquid bars).
- **Tests** under a new `[vwap]` tag in `tests/test_chart_analysis.cpp` —
  same file as the rest of the analysis primitives.
- **No new IB subscriptions** — bar volume is already in
  `m_volumes`; session boundaries are already detectable via `BarSession`.
- **Render style** matches the existing five overlay-line plotting
  conventions (faded `RGBA`, thin line, no markers).
- **Backward-compat** — the existing `ChartWindow::CalcVWAP` static is
  removed once the new path is wired; no parallel implementations.

## 3. Architecture

### 3a. Files

| Path | Purpose | Status |
|---|---|---|
| `src/core/services/ChartAnalysis.h` | Add `VwapResult` struct + `SessionVwap` inline free function | EDIT |
| `src/ui/windows/ChartWindow.h` | Add `bool vwapBands = false` to `IndicatorSettings`; remove `CalcVWAP` static decl; add `m_vwapSd1Up/Dn`, `m_vwapSd2Up/Dn` member vectors | EDIT |
| `src/ui/windows/ChartWindow.cpp` | Replace `CalcVWAP` call site with `SessionVwap`; render bands in `DrawCandleChart`; add bands sub-toggle in `DrawToolbar` | EDIT |
| `src/core/services/TradingStyle.h` | Already references `indVwap` and `indVwapBands` per-mode (see trading-styles plan); ensure both fields exist before applying presets | EDIT (cross-cutting) |
| `tests/test_chart_analysis.cpp` | Add `[vwap]` cases | EDIT |

No CMake change — both files are already in the right targets.

### 3b. New types (`core::services::ChartAnalysis.h`)

```cpp
// ─── VWAP + volume-weighted ±σ bands ────────────────────────────────────────
// Session-anchored: every entry in `sessionStarts` is interpreted as the bar
// index where a new session begins (cumulative TPV / vol reset). For intraday
// timeframes, sessionStarts is typically derived from `BarSession`; for
// non-intraday, pass an empty vector to disable resets (single cumulative
// run from index 0).
//
// Variance is the *volume-weighted* variance:
//   varVw = (Σ vol × (typical - vwap)²) / Σ vol
//   sd    = sqrt(varVw)
//
// Zero-volume bars carry the previous VWAP and bands forward — no
// division-by-zero, no NaN.
struct VwapResult {
    std::vector<double> vwap;
    std::vector<double> sd1Up;
    std::vector<double> sd1Dn;
    std::vector<double> sd2Up;
    std::vector<double> sd2Dn;
};

inline VwapResult SessionVwap(const std::vector<double>& highs,
                              const std::vector<double>& lows,
                              const std::vector<double>& closes,
                              const std::vector<double>& volumes,
                              const std::vector<int>&    sessionStarts) {
    VwapResult r;
    int n = static_cast<int>(closes.size());
    r.vwap.assign(n, 0.0);
    r.sd1Up.assign(n, 0.0);
    r.sd1Dn.assign(n, 0.0);
    r.sd2Up.assign(n, 0.0);
    r.sd2Dn.assign(n, 0.0);
    if (n == 0 ||
        static_cast<int>(highs.size())   != n ||
        static_cast<int>(lows.size())    != n ||
        static_cast<int>(volumes.size()) != n) return r;

    auto isStart = [&](int i) {
        for (int s : sessionStarts) if (s == i) return true;
        return false;
    };

    double cumTPV  = 0.0;
    double cumVol  = 0.0;
    double cumTPV2 = 0.0;     // Σ vol × typical² (running, used to derive variance)

    for (int i = 0; i < n; ++i) {
        if (i == 0 || isStart(i)) {
            cumTPV  = 0.0;
            cumVol  = 0.0;
            cumTPV2 = 0.0;
        }
        double typ = (highs[i] + lows[i] + closes[i]) / 3.0;
        double vol = volumes[i];

        if (vol > 0.0) {
            cumTPV  += typ * vol;
            cumVol  += vol;
            cumTPV2 += typ * typ * vol;
        }

        if (cumVol > 0.0) {
            double vwap = cumTPV / cumVol;
            // E[X²] - E[X]²  on the volume-weighted distribution.
            double meanSq = cumTPV2 / cumVol;
            double varVw  = meanSq - vwap * vwap;
            if (varVw < 0.0) varVw = 0.0;            // numerical clamp
            double sd     = std::sqrt(varVw);
            r.vwap[i]  = vwap;
            r.sd1Up[i] = vwap + sd;
            r.sd1Dn[i] = vwap - sd;
            r.sd2Up[i] = vwap + 2.0 * sd;
            r.sd2Dn[i] = vwap - 2.0 * sd;
        } else if (i > 0) {
            r.vwap[i]  = r.vwap[i - 1];
            r.sd1Up[i] = r.sd1Up[i - 1];
            r.sd1Dn[i] = r.sd1Dn[i - 1];
            r.sd2Up[i] = r.sd2Up[i - 1];
            r.sd2Dn[i] = r.sd2Dn[i - 1];
        } else {
            r.vwap[i] = closes[i];   // first bar with zero volume — fall back to close
        }
    }
    return r;
}
```

The closed-form `varVw = E[X²] - E[X]²` formulation lets us run a single
pass without rebuilding the deviation sum from scratch each bar — O(n) total.
Numerical drift is bounded for the typical session length (≤ 390 bars for 1m
US RTH); the `varVw < 0` clamp guards the rare case where `meanSq` and
`vwap²` are equal up to ULPs and subtract to slightly negative.

### 3c. ChartWindow changes

```cpp
// Header — IndicatorSettings
bool vwap      = true;
bool vwapBands = false;   // NEW — ±1σ / ±2σ bands sub-toggle

// Header — member vectors (alongside m_vwap)
std::vector<double> m_vwapSd1Up;
std::vector<double> m_vwapSd1Dn;
std::vector<double> m_vwapSd2Up;
std::vector<double> m_vwapSd2Dn;

// Header — remove static decl
// static std::vector<double> CalcVWAP(...);     // DELETE
```

```cpp
// ChartWindow.cpp ComputeIndicators() — replace the existing
// CalcVWAP call (around line 3199) with the new free function.
{
    std::vector<int> sessionStarts;
    if (IsIntraday(m_timeframe)) {
        for (int i = 1; i < (int)m_xs.size(); ++i) {
            // Session "start" = the bar after a Regular→AfterHours→PreMarket→Regular
            // transition wraps. Cheaper detection: ET day boundary.
            std::time_t a = (std::time_t)m_xs[i - 1];
            std::time_t b = (std::time_t)m_xs[i];
            int dayA = std::gmtime(&a)->tm_yday;
            int dayB = std::gmtime(&b)->tm_yday;
            if (dayA != dayB) sessionStarts.push_back(i);
        }
    }
    auto vw = core::services::SessionVwap(m_highs, m_lows, m_closes, m_volumes, sessionStarts);
    m_vwap      = std::move(vw.vwap);
    m_vwapSd1Up = std::move(vw.sd1Up);
    m_vwapSd1Dn = std::move(vw.sd1Dn);
    m_vwapSd2Up = std::move(vw.sd2Up);
    m_vwapSd2Dn = std::move(vw.sd2Dn);
}
```

(Note the day-boundary detection lives in ChartWindow because it depends on
`m_xs` semantics; the pure helper takes pre-computed `sessionStarts`.
Keeping `BarSession`/timezone logic out of `ChartAnalysis.h` matches how
`ComputeDailyPivots()` did it in Phase 11.)

```cpp
// DrawCandleChart() — VWAP + bands rendering, replacing the existing
// PlotLine("VWAP", ...) call near line 2761.
if (m_ind.vwap && (int)m_vwap.size() == n) {
    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), 1.5f);
    ImPlot::PlotLine("VWAP", m_idxs.data(), m_vwap.data(), n);

    if (m_ind.vwapBands && (int)m_vwapSd2Up.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.0f, 0.30f), 1.0f);
        ImPlot::PlotLine("VWAP+1σ", m_idxs.data(), m_vwapSd1Up.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.0f, 0.30f), 1.0f);
        ImPlot::PlotLine("VWAP-1σ", m_idxs.data(), m_vwapSd1Dn.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.0f, 0.18f), 1.0f);
        ImPlot::PlotLine("VWAP+2σ", m_idxs.data(), m_vwapSd2Up.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.85f, 0.0f, 0.18f), 1.0f);
        ImPlot::PlotLine("VWAP-2σ", m_idxs.data(), m_vwapSd2Dn.data(), n);
    }
}
```

The `ImPlot::SetNextLineStyle` calls give each band a distinct alpha so the
inner band reads as "warmer" than the outer. Names "VWAP+1σ" / "VWAP-1σ" /
"VWAP+2σ" / "VWAP-2σ" appear in the chart legend — easy to toggle off there
if the user wants just the centre line without going to the toolbar.

### 3d. Toolbar UI

`DrawToolbar()` — currently around line 591:
```cpp
row.item(FlexRow::checkboxW("VWAP"));
ImGui::Checkbox("VWAP", &m_ind.vwap);
```
Append a sub-toggle that's only visible when VWAP is on:
```cpp
if (m_ind.vwap) {
    row.item(FlexRow::checkboxW("±σ"));
    ImGui::Checkbox("±σ", &m_ind.vwapBands);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show ±1σ and ±2σ volume-weighted bands around VWAP.");
}
```

Compact label `±σ` keeps the toolbar dense; tooltip explains the math.

### 3e. ReqId / data dependencies

None new. Inputs are already in `m_highs`, `m_lows`, `m_closes`, `m_volumes`,
`m_xs`. No new IB calls.

### 3f. Cross-cutting with `trading-styles.md`

`StylePreset.indVwap` and `StylePreset.indVwapBands` feed the per-mode
defaults. Land this plan first (so the fields exist), then the trading-styles
plan can wire `ApplyPreset()` to set them. Or land both in the same PR — the
test/build matrix is simpler that way.

---

## 4. Algorithms (reference)

### 4a. Volume-weighted variance, single-pass

```
For each bar i in session:
    typ = (high[i] + low[i] + close[i]) / 3
    vol = volume[i]

    if vol > 0:
        cumTPV  += typ * vol
        cumVol  += vol
        cumTPV2 += typ * typ * vol

    if cumVol > 0:
        vwap   = cumTPV / cumVol
        meanSq = cumTPV2 / cumVol
        varVw  = max(0, meanSq - vwap * vwap)
        sd     = sqrt(varVw)
        emit (vwap, vwap±sd, vwap±2sd)
    else:
        carry forward previous values (or use close[i] for the first bar)
```

This is the closed-form `Var[X] = E[X²] - E[X]²` on the volume-weighted
distribution. O(n) total, no nested loops, no rebuilding the deviation sum.

Numerically: for typical session lengths (390 bars for 1m × 6.5h), the catastrophic-cancellation risk between `meanSq` and `vwap²` is small (~ULP scale). The `varVw < 0` clamp is the safety net for the degenerate "all bars at the same typical price" case where the difference is purely noise.

### 4b. Session boundary detection

ChartWindow side, before calling `SessionVwap`:

```
sessionStarts = []
for i in 1..n-1:
    dayA = ET-day(m_xs[i-1])
    dayB = ET-day(m_xs[i])
    if dayA != dayB:
        sessionStarts.append(i)
```

ET-day = `localtime(ts)->tm_yday` if we're running on a host pinned to ET, or
explicit ET-offset arithmetic otherwise (the existing `BarSession` already
handles DST — we could reuse that for a more robust day-boundary check, but
`tm_yday` is sufficient when the host is in ET, which is how this app is
typically run).

For non-intraday timeframes, pass an empty `sessionStarts` and the function
runs as a single cumulative VWAP from index 0 (matching today's behaviour for
D1/W1/MN bars — the existing `intradayReset=false` branch).

---

## 5. Task breakdown

### Task A — `SessionVwap` primitive + tests
**Status:** TODO

- [ ] `core::services::ChartAnalysis.h`: add `VwapResult` struct +
      `SessionVwap` inline free function per §3b.
- [ ] `tests/test_chart_analysis.cpp` add `[vwap]` cases:
  - [ ] **Known volume-weighted average** — 3-bar input
        `typical = {100, 102, 101}`, `vol = {1000, 2000, 3000}`. Expected
        VWAP[2] = `(100·1000 + 102·2000 + 101·3000)/(6000) = 101.166...`.
        Bands at ±sqrt(varVw) where the math is hand-checked.
  - [ ] **Single-bar input** — `vwap[0] == typical[0]` (and bands collapse
        to vwap[0] since variance = 0).
  - [ ] **Session reset** — synthetic two-day series, e.g. day 1 typical
        `{100, 100}`, day 2 typical `{200, 200}`. With `sessionStarts = {2}`,
        `vwap[1] == 100`, `vwap[2] == 200` (no carry-over from day 1).
        Without sessionStarts, `vwap[2]` is the cumulative average of all 4
        bars.
  - [ ] **Zero-volume bar** — series with vol[1] = 0; assert vwap[1] ==
        vwap[0] and bands carry forward.
  - [ ] **Empty / mismatched-length input** — empty series → empty result;
        unequal sizes → all-zero result, no crash.
  - [ ] **±2σ band ordering** — for any non-degenerate input, `sd2Dn ≤ sd1Dn
        ≤ vwap ≤ sd1Up ≤ sd2Up` element-wise.
- [ ] `cmake --build build --target tests-core -j$(nproc)` clean.
- [ ] `ctest --test-dir build -R "^tests-core" --output-on-failure` —
      all `[vwap]` cases pass.

### Task B — ChartWindow integration
**Status:** TODO

- [ ] `ChartWindow.h`:
  - [ ] `IndicatorSettings::vwapBands = false`.
  - [ ] `m_vwapSd1Up`, `m_vwapSd1Dn`, `m_vwapSd2Up`, `m_vwapSd2Dn` member
        vectors.
  - [ ] Remove `CalcVWAP` static decl.
- [ ] `ChartWindow.cpp`:
  - [ ] `ComputeIndicators()`: build `sessionStarts` from `m_xs`, call
        `core::services::SessionVwap(...)`, move-assign into the four band
        members + `m_vwap`.
  - [ ] Remove the `ChartWindow::CalcVWAP` static implementation (around
        line 3566).
  - [ ] `DrawCandleChart()`: render four band lines (alpha 0.30 / 0.18) when
        `m_ind.vwap && m_ind.vwapBands`. Centre-line VWAP rendering
        unchanged.
  - [ ] `DrawToolbar()`: append `±σ` checkbox right after the existing VWAP
        checkbox, gated on `m_ind.vwap`.
  - [ ] `DrawHoverTooltip()` / `DrawInfoBar()`: optional polish — append
        `(±0.42)` to the VWAP value when bands are on (1σ value at the
        hovered bar). Skip in v1 if it's noisy.
- [ ] Build clean (no new warnings).
- [ ] Manual smoke-test:
  - [ ] AAPL 1m intraday: VWAP centre line resets at the day open; bands
        widen as variance grows through the day.
  - [ ] Toggle `±σ` off → bands disappear; on → bands reappear without
        re-fetch.
  - [ ] Switch to Day Trading mode (after trading-styles plan lands) →
        bands ON by default; switch to Scalping → centre on, bands off.

---

## 6. Acceptance / verification

For Task A:
1. `cmake --build build --target tests-core -j$(nproc)` clean.
2. `ctest --test-dir build -R "^tests-core" --output-on-failure` — all
   `[vwap]` cases plus all existing tests pass.

For Task B:
1. `cmake --build build -j$(nproc)` clean (no new warnings).
2. `ctest --test-dir build --output-on-failure` — all targets pass.
3. Manual:
   - Connect paper account, open AAPL 1m chart.
   - Verify VWAP line is unchanged from current behaviour (same colour,
     same reset at day open).
   - Toggle `±σ` checkbox on → 1σ + 2σ bands appear in faded gold; legend
     gains "VWAP+1σ", "VWAP-1σ", "VWAP+2σ", "VWAP-2σ".
   - Pan, zoom — bands stay aligned with VWAP centre line, no flicker.
   - Switch to D1 → VWAP toggle is OFF by default per the trading-styles
     mode (or unchanged if the user previously had it on); bands toggle
     hides when VWAP is off.
4. Update `.claude/rules/task-history.md` and root `README.md` (one-line
   user-visible mention of bands).

---

## 7. Risks / sharp edges

- **Numerical drift on long sessions** — for very long single-session
  cumulative computation (e.g. a non-intraday chart with no resets and 10k
  bars), `meanSq - vwap²` accumulates ULP-level subtraction error. Acceptable
  for the use case (intraday sessions are ≤ 1k bars; the bands are visual,
  not used for order pricing). v2 polish: switch to Welford's algorithm if
  precision becomes an issue.
- **Session-boundary detection mismatch** — if the host machine is not in ET,
  `tm_yday` from `gmtime` and `localtime` may differ by one day across the
  midnight UTC boundary. Symptom: a single "phantom" reset right at midnight
  UTC. Fix: use the same DST-aware ET offset arithmetic as `BarSession()` in
  `MarketData.h`. v1 acceptable for ET-pinned hosts, document in §7.
- **First bar of a session has zero variance** → `sd = 0` → bands collapse
  to the centre line. Intentional (bands need at least 2 bars of data
  within the session to be meaningful). Renders cleanly without special
  casing.
- **Pre-market / after-hours sessions reset** — current `CalcVWAP` resets on
  *date* change, not on RTH boundary. Same here — pre-market 4am bars share
  a session with the 9:30 RTH bars on the same day. That's the standard
  "session VWAP" definition for stocks; if a user wants a 9:30-only VWAP
  they can toggle `useRTH` on. Don't change the reset rule.
- **Legend pollution** — five entries (VWAP, ±1σ ×2, ±2σ ×2) in the legend
  is a lot when the bands are on. Acceptable; the user can drag the legend
  out or hide individual series via the legend itself.
- **`indVwapBands` field added before trading-styles uses it** — fine; the
  trading-styles plan references it but only after this plan lands. If the
  two land together, watch the `ApplyPreset` template assignment so the
  field reference doesn't compile-fail.

---

## 8. Out of scope (explicitly deferred)

- **Anchored VWAP** — user-placed anchor (drag-to-set) for VWAP from a
  specific bar (earnings date, breakout). Different UX (drawing-tool style)
  and a different cumulative reset rule. v2.
- **Multi-session anchored VWAP** — VWAP from a date range, week-to-date,
  month-to-date. v2.
- **Volume profile** — horizontal histogram on the right edge showing
  volume-at-price. Different kind of overlay. Already in
  `auto-analysis.md` §8 (out of scope).
- **Session-VWAP on D1+** — daily/weekly bars don't have intraday session
  semantics that make session-VWAP useful. The `intradayReset=false` branch
  just runs cumulative from index 0, which is the "anchored to first bar"
  meaning — not particularly informative for swing/investment styles. The
  trading-styles plan defaults VWAP OFF for those modes.

---

## 9. Update log

| Date | Change | By |
|---|---|---|
| 2026-04-29 | Plan written (draft) | claude / jose |
