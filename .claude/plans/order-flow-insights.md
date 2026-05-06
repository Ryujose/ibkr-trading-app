# Plan: Order Flow Insights on ChartWindow (Phase 15)

**Status:** Task A landed (Volume Profile, 2026-05-06). Tasks B/C TODO. Tests: 287 cases / 1322 assertions in tests-core; 275/275 ctest pass.
**Owner:** Jose
**Goal:** Help the trader read **why** price is moving — beyond the indicator overlay layer the chart already has. Three additions on `ChartWindow`, in order of dependency:

1. **CW-1 — Volume Profile** (horizontal histogram of volume-by-price). Standalone. Identifies the prices where real money changed hands → those act as supports/resistances *based on transacted volume*, not on swing geometry.
2. **CW-3 — CVD (Cumulative Volume Delta)** rendered as a sub-pane. Per-bar net aggressor delta + running total. Sourced from a tick-by-tick stream **borrowed from a `TradingWindow` open on the same symbol** (same fan-out pattern as the unguarded-position guard). Falls back gracefully when no TradingWindow is open.
3. **CW-4 — Price/CVD divergence detector**. Auto-flags when price makes a higher-high but CVD makes a lower-high (and the bullish mirror). Emits a tag in the chart overlay similar to `m_breakoutSignal`.

Out of scope for this phase (deferred): TradingWindow imbalance widget, large-print markers in the tape, bar-delta approximation from close-position, anchored CVD, footprint chart, volume-by-price as a TPO chart. See §8.

This file is the source of truth across sessions — update the **Status** line and per-task checkboxes as work progresses.

---

## 1. Approved scope

| # | Feature | Default | Toolbar label | Notes |
|---|---|---|---|---|
| 1 | **Volume Profile (CW-1)** | OFF | `VP` checkbox in the Auto: row | Right-edge horizontal histogram. POC highlighted. Optional HVN/LVN markers. Pure bar-data computation — no new IB subscriptions. |
| 2 | **CVD sub-pane (CW-3)** | OFF | `CVD` checkbox in the Auto: row | Sub-plot below RSI showing per-bar delta as bars + cumulative as line. Tick stream borrowed from a `TradingWindow` on the same symbol. Sub-pane shows a "no tape" placeholder when no `TradingWindow` is open on the same symbol. |
| 3 | **Divergence detector (CW-4)** | OFF | `Div` checkbox in the Auto: row | Requires CVD data to be present. Emits `m_cvdDivergence` (BullishReg / BearishReg / None) similar to `m_breakoutSignal`. Renders a label tag on the chart overlay. |

All three honour the existing trading-style preset model (`StylePreset` in `core::services::TradingStyle.h`). Defaults per mode: see §3g.

## 2. Resolved decisions

- **Volume Profile algorithm** — distribute each bar's volume *uniformly across the bar's [low, high] range*, not concentrated at the typical price. This matches TradingView / IB / NinjaTrader behaviour and avoids the "POC sits awkwardly at the close of one big bar" artefact that pure-typical-price profiles produce. Implementation: for each bar, find which bins overlap `[low, high]` and add `volume × overlap_fraction` to each bin.
- **Volume Profile range** — bins span the **visible chart Y-range** (re-bucketed every frame as the user pans/zooms). Not the full data range. Reason: a profile of "all bars ever loaded" turns into a useless smear at extreme zoom-out; the visible range is what the user is reasoning about. Re-bucket cost is O(n_bars × n_bins) per frame, capped at 50 bins, fine for 5–10k bars.
- **CVD source: tick-by-tick borrowed from `TradingWindow`** — not a new subscription per chart. Reason: IB has a streaming-tick budget (typically 100), and adding 10 charts × per-symbol ticks would burn it. Reuses the unguarded-guard fan-out pattern: `main.cpp` builds a `symToTrading` map from `g_tradingEntries` and pushes `TickBatch`/`BarDelta` updates into the matching `ChartWindow`. When no `TradingWindow` is open for the symbol, CVD shows "no tape — open a TradingWindow on this symbol" placeholder text in the sub-pane.
- **Tick aggregation site: ChartWindow** — main.cpp forwards raw ticks (already converted to `core::HistoricalTick`-compatible records by `IBKRClient`); ChartWindow buckets them into bars itself. This keeps the per-bar boundary logic (`m_xs[i] ≤ tick.time < m_xs[i+1]`) co-located with the bar timestamps — main.cpp doesn't need to know about timeframe semantics.
- **Aggressor classification** — `tick.price >= ask → buy aggressor`; `tick.price <= bid → sell aggressor`; in-between → neutral. Standard rule. Requires the tick stream type to be `BID_ASK` *or* the per-tick to carry quote context. We use the **`AllLast` (TRADES) stream** that `TradingWindow::Time & Sales` already subscribes to plus the latest **NBBO from the chart's existing market-data subscription** (`OnTick` already feeds `m_lastBid`/`m_lastAsk` for the current-price line). Bid/ask context = NBBO-at-bar-close, which is "good enough" for backfill; live ticks use the rolling NBBO from `OnTickPrice` updates.
- **Per-bar delta + cumulative** — sub-pane plots two series: per-bar delta as up/down bars (green for net buy, red for net sell), and the cumulative running sum as a single overlay line. Two-axis ImPlot chart (delta on Y1, CVD on Y2) so the tiny per-bar deltas don't get crushed by the cumulative magnitude.
- **No retro tick fetch on bar-load** — CVD only exists for bars that streamed live from the TradingWindow tick borrow. Older bars in the chart get zero delta. We **don't** issue `reqHistoricalTicks` to backfill — that's expensive (hundreds of requests across timeframes) and out of scope. The sub-pane shows a clear "live-only" tag when partial data is present.
- **Divergence detection: regular only** — bearish regular (price HH + CVD LH) and bullish regular (price LL + CVD HL). Hidden divergences deferred to v2. Detection runs at the tail of `DetectStructure()` like `ComputeBreakoutSignal()`, looks at the last 50 bars, picks the two most-recent matching swing pairs.
- **Divergence visual** — single label tag near the right edge of the chart in `DrawOverlays()`, identical pattern to `LONG SETUP` / `SHORT SETUP` from Phase 11 zones. No connecting trendlines (visually noisy on the small swings divergences hit).
- **No new IB subscriptions** for CW-1. CW-3 reuses the existing `TradingWindow` `tickByTickAllLast` subscription. CW-4 is computed from CVD data already in memory.
- **Tests** under three new tags in `tests/test_chart_analysis.cpp`: `[volume-profile]`, `[cvd]`, `[divergence]`. Same file as the rest of the analysis primitives.
- **No persistence** — checkbox states live on `IndicatorSettings` / `AutoAnalysisSettings`. No new fields in `chart-modes.cfg` (the trading-style presets cover the per-mode defaults).

## 3. Architecture

### 3a. Files

| Path | Purpose | Status |
|---|---|---|
| `src/core/services/ChartAnalysis.h` | Add `VolumeProfile` struct + `ComputeVolumeProfile()`; add `CvdResult` struct + `ComputeBarDeltas()` + `ComputeCvd()`; add `CvdDivergence` enum + `DetectCvdDivergence()` | EDIT |
| `src/ui/windows/ChartWindow.h` | New `IndicatorSettings::volumeProfile`, `vpBins`, `cvd`; new `AutoAnalysisSettings::cvdDivergence`; member buffers `m_vp`, `m_barDeltas`, `m_cvd`, `m_cvdDivergence`; new `CvdTickHint` accessor / setter for the borrow path | EDIT |
| `src/ui/windows/ChartWindow.cpp` | `DrawVolumeProfile()` overlay inside `BeginPlot/EndPlot`; `DrawCvdSubpane()` new sub-plot; `DetectStructure()` adds divergence call; `OnTickFromBorrow()` ingestion entrypoint; toolbar checkboxes in `DrawAnalysisToolbar()` | EDIT |
| `src/main.cpp` | New `PushCvdTicksToCharts()` per-frame fan-out (mirrors `PushUnguardedHintsToWindows()`); new `OnTickByTickReceived` routing addendum that fans out to charts as well as TradingWindow | EDIT |
| `tests/test_chart_analysis.cpp` | Add `[volume-profile]`, `[cvd]`, `[divergence]` cases | EDIT |

No CMake change.

### 3b. New types (`core::services::ChartAnalysis.h`)

```cpp
// ─── Volume Profile (CW-1) ─────────────────────────────────────────────────
struct VolumeBin {
    double priceLo;    // bin lower edge
    double priceHi;    // bin upper edge
    double volume;     // total volume that traded inside [priceLo, priceHi]
};

struct VolumeProfile {
    std::vector<VolumeBin> bins;
    int    pocIdx     = -1;     // index of max-volume bin (Point of Control)
    double maxVolume  = 0.0;    // bins[pocIdx].volume — used to normalise widths
    double totalVol   = 0.0;
    int    valueAreaLoIdx = -1; // ~70% volume range, lower bound (optional rendering)
    int    valueAreaHiIdx = -1; // ~70% volume range, upper bound
};

// Bar-data → profile. Visible-range version: caller passes priceLo/priceHi
// (typically from the chart's Y-axis limits). `numBins` is the desired bin
// count (clamped to [10, 200]). Each bar's volume is distributed
// proportionally across the bins it overlaps in [low_i, high_i] — bars whose
// range is entirely outside [priceLo, priceHi] contribute zero, partial
// overlaps contribute pro-rata.
//
// O(n_bars × log(n_bins)) — for 10k bars × 50 bins ≈ 80k ops/frame, fine.
VolumeProfile ComputeVolumeProfile(const std::vector<double>& highs,
                                   const std::vector<double>& lows,
                                   const std::vector<double>& volumes,
                                   double priceLo, double priceHi,
                                   int    numBins = 50);

// ─── CVD (CW-3) ────────────────────────────────────────────────────────────
// One per-bar net delta value: +N for net-buy aggression in the bar window,
// -N for net-sell, 0 for neutral / no-tick bars.
struct CvdResult {
    std::vector<double> barDelta;   // signed per bar
    std::vector<double> cumulative; // running sum
    int    firstLiveBar = -1;       // earliest bar idx with at least one tick
};

// Caller supplies the bar timestamps (`barOpenTimes` — same convention as
// `m_xs`) and a flat list of ticks already classified as Buy/Sell/Neutral
// aggressor. The function bins ticks by `tick.time ∈ [barOpenTimes[i],
// barOpenTimes[i+1])` (last bar gets `tick.time >= barOpenTimes[n-1]`).
//
// Pre-classification keeps this helper pure — `ChartWindow` does the
// `price vs ask/bid` classification at ingestion time using the rolling NBBO,
// then passes the classified stream here.
struct ClassifiedTick {
    std::time_t time;
    double      size;
    int         aggressor;   // +1 buy, -1 sell, 0 neutral
};

CvdResult ComputeCvd(const std::vector<double>& barOpenTimes,
                     const std::vector<ClassifiedTick>& ticks);

// ─── Divergence (CW-4) ─────────────────────────────────────────────────────
enum class CvdDivergence { None = 0, BullishReg = 1, BearishReg = 2 };

// Looks at the last `lookback` bars of `closes` and `cvd`. Picks the two most
// recent local highs (for bearish) / two most recent local lows (for
// bullish). A bearish-regular pattern requires:
//     close[A] < close[B]  (price HH)
//     cvd  [A] > cvd  [B]  (CVD   LH)   — where B is the more-recent swing
// Bullish-regular is the mirror.
//
// Local extrema are detected with a small `swingK=3` pivot window — same
// convention as `FindSwings()` in this file.
CvdDivergence DetectCvdDivergence(const std::vector<double>& closes,
                                  const std::vector<double>& cvd,
                                  int lookback = 50,
                                  int swingK   = 3);
```

The volume-profile bin layout is uniform width across `[priceLo, priceHi]`. Variable-width (TPO-style) is deferred — uniform bins render cleanly and the math is straightforward.

### 3c. ChartWindow changes — header

```cpp
// IndicatorSettings additions
bool volumeProfile = false;     // CW-1
int  vpBins        = 50;        // CW-1 — clamped 10..200 in popup

bool cvd           = false;     // CW-3 sub-pane

// AutoAnalysisSettings additions
bool cvdDivergence = false;     // CW-4 — gated on cvd above

// Member buffers
core::services::VolumeProfile m_vp;            // recomputed every frame from visible Y-range
std::vector<double>           m_barDeltas;     // sized to m_xs.size(), zero where no ticks seen
std::vector<double>           m_cvd;           // cumulative, sized to m_xs.size()
core::services::CvdDivergence m_cvdDivergence = core::services::CvdDivergence::None;

// Tick borrow ingestion
struct CvdTickHint {
    std::vector<core::services::ClassifiedTick> incoming;  // already-classified, pushed by main.cpp
};
void IngestCvdTicks(const CvdTickHint& h);     // appends to internal m_pendingTicks ring buffer

// Render entrypoints
void DrawVolumeProfile();        // called inside BeginPlot/EndPlot of price chart
void DrawCvdSubpane();           // separate ImPlot sub-plot, called from Render() after DrawRsiChart
void DrawCvdDivergenceLabel();   // called from DrawOverlays(), right-edge tag like LONG SETUP
```

### 3d. ChartWindow changes — implementation sketch

**`ComputeIndicators()` — append at the tail (after `DetectStructure()`):**

```cpp
// Drain pending tick ring buffer → classify per current NBBO → bin into bars
if (m_ind.cvd && !m_pendingTicks.empty()) {
    // m_pendingTicks already arrives pre-classified from main.cpp ingestion;
    // here we just merge into the per-bar delta vector.
    for (auto& t : m_pendingTicks) {
        int barIdx = LookupBarIdxByTime(t.time);  // binary search on m_xs
        if (barIdx >= 0 && barIdx < (int)m_barDeltas.size()) {
            m_barDeltas[barIdx] += t.aggressor * t.size;
        }
    }
    m_pendingTicks.clear();

    // Recompute cumulative — cheap, O(n)
    m_cvd.assign(m_barDeltas.size(), 0.0);
    double run = 0.0;
    for (size_t i = 0; i < m_barDeltas.size(); ++i) {
        run += m_barDeltas[i];
        m_cvd[i] = run;
    }
}

// Divergence — only when both CVD and the toggle are on
if (m_ind.cvd && m_auto.cvdDivergence && (int)m_cvd.size() == (int)m_closes.size()) {
    m_cvdDivergence = core::services::DetectCvdDivergence(m_closes, m_cvd);
} else {
    m_cvdDivergence = core::services::CvdDivergence::None;
}
```

`m_pendingTicks` is a small `std::vector<ClassifiedTick>` ring buffer; main.cpp pushes into it via `IngestCvdTicks()` per frame.

**`DrawVolumeProfile()` — inside the price chart's `BeginPlot/EndPlot`:**

```cpp
if (!m_ind.volumeProfile) return;
ImPlotRect lim = ImPlot::GetPlotLimits();
double priceLo = lim.Y.Min, priceHi = lim.Y.Max;
m_vp = core::services::ComputeVolumeProfile(m_highs, m_lows, m_volumes,
                                            priceLo, priceHi, m_ind.vpBins);
// Render as horizontal bars anchored at the right edge of the plot:
//   for each bin: bar from (xRight - normalised_width) to xRight at y = bin centre
// Use 18% alpha so it doesn't drown out candles. POC bin gets 35% alpha + thin border.
```

ImPlot has `PlotBarsH` for horizontal bars. Width is normalised against `m_vp.maxVolume` so the POC always extends ~25% across the plot width. The bars render *behind* candles via `ImPlotBarsFlags_Stacked` is unnecessary — we want them as a translucent backdrop on the right side of the chart only. Implementation uses a manual drawlist (similar to how `DrawAutoZones` paints rectangles) anchored to the right plot rect, **not** `PlotBarsH`, so it doesn't interact with the X-axis or affect zoom-fit calculations.

**`DrawCvdSubpane()` — new sub-plot, alongside Volume / RSI:**

```cpp
if (!m_ind.cvd) return;
float cvdAvail = std::max(60.0f, ImGui::GetContentRegionAvail().y);
if (!ImPlot::BeginPlot("##cvd", ImVec2(-1, cvdAvail), ImPlotFlags_NoLegend)) return;

ImPlot::SetupAxes(nullptr, "CVD",
    ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines,
    ImPlotAxisFlags_AutoFit);

// Per-bar delta as bars (Y1, hidden-axis convention from the volume sub-pane).
// Use the same coloured-bar pattern as the existing volume bars:
//   green when barDelta[i] > 0, red when < 0.
// Cumulative line on Y2.
ImPlot::PlotBars("Δ", m_idxs.data(), m_barDeltas.data(), n, /*width=*/0.7);
ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
ImPlot::PlotLine("CVD", m_idxs.data(), m_cvd.data(), n);

// "live-only" tag if firstLiveBar > 0
if (m_cvdFirstLiveBar > 0) {
    ImGui::TextDisabled("live-only — older bars have no tape data");
}
ImPlot::EndPlot();
```

If no `TradingWindow` is open on this symbol → fan-out from main.cpp leaves `m_pendingTicks` empty, `m_barDeltas` stays all-zero, and the sub-pane shows a flat line + a "no tape" placeholder centred in the plot rect.

**`DrawCvdDivergenceLabel()` — called from `DrawOverlays()` after the setup overlay:**

```cpp
if (m_cvdDivergence == CvdDivergence::None || !m_auto.cvdDivergence) return;
// Right-edge label, similar pattern to the LONG SETUP / SHORT SETUP tags.
const char*  label = (m_cvdDivergence == CvdDivergence::BullishReg)
                       ? " BULL DIV "
                       : " BEAR DIV ";
ImU32 colour = (m_cvdDivergence == CvdDivergence::BullishReg)
                       ? IM_COL32(80, 220, 120, 230)
                       : IM_COL32(230, 90, 90, 230);
// y-anchor: midpoint of the most recent two swings used for the divergence
// (returned alongside the enum if we extend the helper to also return the
// swing indices — TBD in Task C).
```

### 3e. main.cpp tick borrow fan-out

Mirror of `PushUnguardedHintsToWindows()`:

```cpp
static void PushCvdTicksToCharts() {
    // Build symbol → trading window map (already a thing for the unguarded guard,
    // can hoist to a shared helper).
    std::unordered_map<std::string, TradingEntry*> symToTrading;
    for (auto& te : g_tradingEntries) {
        if (te.win) symToTrading[te.win->getSymbol()] = &te;
    }

    for (auto& ce : g_chartEntries) {
        if (!ce.win) continue;
        ChartWindow::CvdTickHint hint;
        auto it = symToTrading.find(ce.win->getSymbol());
        if (it != symToTrading.end() && it->second->win) {
            // Drain the trading-window tick buffer into the hint (after classifying).
            // Trading-window has live NBBO + AllLast tape already streaming.
            it->second->win->DrainClassifiedTicks(hint.incoming);
        }
        ce.win->IngestCvdTicks(hint);
    }
}
```

Called from `RenderTradingUI()` once per frame, alongside `PushUnguardedHintsToWindows()`.

`TradingWindow::DrainClassifiedTicks(std::vector<ClassifiedTick>& out)` is a new method that pops the trading window's *internal* per-frame tick buffer (since the last drain call) and classifies each tick using the trading window's rolling NBBO. The classification logic *lives in TradingWindow* because that window owns the live NBBO (`m_lastBid` / `m_lastAsk`) — chart windows would need their own NBBO subscription otherwise.

> Cross-window pattern note: this is the third instance of the borrow pattern (after unguarded-guard and group-sync). Worth refactoring into a shared `WindowMessageBus` after the third — but defer that until Task B is in and we see how the API actually feels.

### 3f. Toolbar UI

Auto: row in `DrawAnalysisToolbar()` (already populated with `Sup`, `Res`, `AutoTrend`, `Zones`, `Donch`, `Kelt`, `AutoFib`, `Pivots`, `Breakouts`, `Setup`):

```cpp
row.item(FlexRow::checkboxW("VP"));
ImGui::Checkbox("VP", &m_ind.volumeProfile);
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Volume Profile — horizontal histogram of volume-by-price.");

row.item(FlexRow::checkboxW("CVD"));
ImGui::Checkbox("CVD", &m_ind.cvd);
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Cumulative Volume Delta — needs a TradingWindow open on this symbol for tape data.");

if (m_ind.cvd) {
    row.item(FlexRow::checkboxW("Div"));
    ImGui::Checkbox("Div", &m_auto.cvdDivergence);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Detect price/CVD regular divergence.");
}
```

`DrawAutoSettingsPopup()` gains a Volume Profile section: `Bins` (`InputInt`, 10–200, default 50). No popup section for CVD or divergence in v1.

### 3g. Trading-style preset defaults

Update `core::services::GetPreset(...)` cases in `TradingStyle.h`:

| Style | Volume Profile | CVD | Divergence |
|---|---|---|---|
| Scalping | OFF (visual noise on M1) | ON (M1 needs tape badly) | ON |
| Day Trading | ON | ON | ON |
| Swing | ON | OFF (D1 has no live tick context) | OFF |
| Investment | OFF | OFF | OFF |
| Free | construction default (all OFF) | construction default (OFF) | construction default (OFF) |

`StylePreset` gains three new fields (`indVolumeProfile`, `indCvd`, `autoCvdDivergence`) wired through `ApplyPreset(...)`. Tests under `[style]` extend to assert each preset sets these fields.

### 3h. Persistence

None. The trading-style presets cover per-mode defaults; `chart-modes.cfg` keeps its existing format. No new fields.

### 3i. ReqId / data dependencies

| Path | Source | New IB calls |
|---|---|---|
| CW-1 Volume Profile | `m_highs` / `m_lows` / `m_volumes` | None |
| CW-3 CVD | `TradingWindow::DrainClassifiedTicks` | None — reuses `tickByTickAllLast` from `TradingWindow` (reqId 130–139) |
| CW-4 Divergence | `m_closes` / `m_cvd` | None |

---

## 4. Algorithms (reference)

### 4a. Volume Profile bin distribution

```
For each bar i in [0, n):
    if highs[i] < priceLo or lows[i] > priceHi: continue   // bar out of range
    barLo  = max(lows[i],  priceLo)
    barHi  = min(highs[i], priceHi)
    barRng = max(barHi - barLo, ε)        // ε guard for zero-range bars

    binW   = (priceHi - priceLo) / numBins
    firstBin = floor((barLo - priceLo) / binW)
    lastBin  = floor((barHi - priceLo) / binW)

    For b in [firstBin, lastBin]:
        binLo = priceLo + b * binW
        binHi = binLo + binW
        overlap = (min(binHi, barHi) - max(binLo, barLo)) / barRng
        bins[b].volume += volumes[i] * overlap
```

POC = `argmax(bins.volume)`. Value Area (~70% volume) is computed by expanding outward from POC until the cumulative volume hits `0.70 × totalVol` — used optionally as a darker shaded band in the rendering. Tracker fields `valueAreaLoIdx`/`valueAreaHiIdx` get `-1` if `numBins < 5` (not meaningful at low resolution).

### 4b. Per-bar delta from classified ticks

```
For each tick t:
    barIdx = binary_search_le(barOpenTimes, t.time)
    if barIdx in [0, n): barDelta[barIdx] += t.aggressor * t.size

cumulative[0] = barDelta[0]
For i in [1, n): cumulative[i] = cumulative[i-1] + barDelta[i]
```

`firstLiveBar` = smallest `i` with `barDelta[i] != 0`. Used by the sub-pane to render the "live-only" disclaimer.

### 4c. Aggressor classification (TradingWindow side)

```
For each tick t (from tickByTickAllLast):
    bid = m_lastBid, ask = m_lastAsk          // rolling NBBO at tick arrival
    if t.price >= ask: aggressor = +1
    elif t.price <= bid: aggressor = -1
    else: aggressor = 0
```

Lock-free single-producer single-consumer ring buffer (capacity 4096) holds the classified ticks until `DrainClassifiedTicks()` pops them. Capacity covers a worst-case M1 bar with ~4k prints (rare even on liquid names).

### 4d. Regular divergence detection

```
swings = FindSwings(closes, swings, swingK, scanCap=lookback)

# Bearish: most recent two swing-highs in the lookback window
if len(swings.highs) >= 2:
    A, B = last two indices of swings.highs   # A < B (chronological)
    if closes[B] > closes[A] and cvd[B] < cvd[A]:
        return BearishReg

# Bullish mirror on swings.lows:
if len(swings.lows) >= 2:
    A, B = last two indices of swings.lows
    if closes[B] < closes[A] and cvd[B] > cvd[A]:
        return BullishReg

return None
```

Reuses the existing `FindSwings` from CW auto-analysis. No new pivot logic.

---

## 5. Task breakdown

### Task A — Volume Profile (CW-1)
**Status:** DONE (2026-05-06) — landed as Task #74. 12 cases / 72 assertions under `[volume-profile]`, all 275 ctest tests pass.

- [ ] `core::services::ChartAnalysis.h`: add `VolumeBin` / `VolumeProfile` structs + `ComputeVolumeProfile()` per §3b/§4a.
- [ ] `tests/test_chart_analysis.cpp` add `[volume-profile]` cases:
  - [ ] **Single-bar uniform distribution** — one bar `low=100, high=110, vol=1000` over `priceLo=100, priceHi=110, numBins=10` → each bin gets `vol = 100`. POC index = arbitrary tie-break (assert `maxVolume == 100`, count of `volume == 100` bins == 10).
  - [ ] **POC at the close cluster** — 3 bars all `low=100, high=101, vol=500` + 1 bar `low=99, high=100, vol=100`. With `numBins=4` over `[99, 101]`, the [100, 100.5) bin should get the largest share. Assert `pocIdx` falls in the 100-101 range.
  - [ ] **Out-of-range bar** — bar entirely above `priceHi` contributes zero. `totalVol` excludes it.
  - [ ] **Partial overlap** — bar `low=98, high=104` over `priceLo=100, priceHi=104` → only the `[100, 104]` portion contributes, scaled by 4/6.
  - [ ] **Empty input** — empty bars or zero `numBins` → empty profile, `pocIdx = -1`.
  - [ ] **`priceHi <= priceLo`** — degenerate range, returns empty profile.
  - [ ] **Value area expansion** — synthetic profile with bins [10, 20, 50, 20, 10] → POC at index 2; value area expands symmetrically; assert `valueAreaLoIdx`/`HiIdx` boundaries.
- [ ] `src/ui/windows/ChartWindow.h`: add `IndicatorSettings::volumeProfile = false`, `vpBins = 50`, `m_vp` member, `DrawVolumeProfile()` decl.
- [ ] `src/ui/windows/ChartWindow.cpp`:
  - [ ] `DrawVolumeProfile()` body — fetch visible Y-range from `ImPlot::GetPlotLimits()`, call `ComputeVolumeProfile`, render translucent right-anchored horizontal bars via the plot's drawlist (not `PlotBarsH`).
  - [ ] Wire into `DrawCandleChart()` between candlesticks and overlays, gated on `m_ind.volumeProfile`.
  - [ ] `DrawAnalysisToolbar()`: `VP` checkbox in the Auto: row.
  - [ ] `DrawAutoSettingsPopup()`: `Bins` `InputInt` (10–200).
- [ ] `core::services::TradingStyle.h`: add `indVolumeProfile` field to `StylePreset`; update all 5 `GetPreset(...)` cases per §3g; extend `ApplyPreset(...)`.
- [ ] `tests/test_chart_analysis.cpp` `[style]`: extend each preset case with `indVolumeProfile` assertion.
- [ ] `cmake --build build -j$(nproc)` clean.
- [ ] `ctest --test-dir build --output-on-failure` — all targets pass; `[volume-profile]` cases pass.
- [ ] Manual smoke-test: AAPL 1m + AAPL D1, toggle VP on, verify POC visually aligns with high-volume area; pan/zoom — profile rebuckets to the new visible range without flicker.

### Task B — CVD sub-pane (CW-3)
**Status:** TODO

- [ ] `core::services::ChartAnalysis.h`: add `ClassifiedTick` / `CvdResult` structs + `ComputeCvd()` per §3b/§4b.
- [ ] `tests/test_chart_analysis.cpp` add `[cvd]` cases:
  - [ ] **Empty bars / empty ticks** → `CvdResult` with empty vectors, `firstLiveBar = -1`.
  - [ ] **Single-bar single-tick buy** — barOpenTimes={1000}, ticks={t=1100, size=10, agg=+1} → `barDelta[0] = 10`, `cumulative[0] = 10`, `firstLiveBar = 0`.
  - [ ] **Mixed sells/buys within bar** — buy 10 + sell 4 + buy 6 in same bar → `barDelta[0] = 12` (10−4+6).
  - [ ] **Tick before first bar** — discarded silently.
  - [ ] **Tick exactly on bar boundary** — `t.time == barOpenTimes[1]` lands in bar 1 (left-inclusive).
  - [ ] **Cumulative monotonicity** — synthetic series of all +1 aggressors → `cumulative` strictly non-decreasing.
  - [ ] **Sign mirror** — same series with `-1` aggressors → `cumulative` strictly non-increasing.
- [ ] `src/ui/windows/ChartWindow.h`:
  - [ ] `IndicatorSettings::cvd = false`, `m_barDeltas`, `m_cvd`, `m_cvdFirstLiveBar`, `m_pendingTicks` (small ring buffer of `ClassifiedTick`).
  - [ ] `CvdTickHint` struct + `IngestCvdTicks(const CvdTickHint&)` public accessor.
  - [ ] `DrawCvdSubpane()` decl.
- [ ] `src/ui/windows/ChartWindow.cpp`:
  - [ ] `IngestCvdTicks()` body: append into `m_pendingTicks`, cap at 4096 (drop-oldest if overflow).
  - [ ] `ComputeIndicators()` tail: drain `m_pendingTicks`, bin into `m_barDeltas` via `LookupBarIdxByTime` (binary search on `m_xs`), recompute cumulative.
  - [ ] `DrawCvdSubpane()` body — sub-plot pattern matching `DrawRsiChart()`; bar series + line series on Y2; "no tape" placeholder text when `m_cvdFirstLiveBar < 0`.
  - [ ] `Render()`: invoke `DrawCvdSubpane()` after `DrawRsiChart()`, gated on `m_ind.cvd`.
  - [ ] `DrawAnalysisToolbar()`: `CVD` checkbox in the Auto: row.
- [ ] `src/ui/windows/TradingWindow.h`: add `DrainClassifiedTicks(std::vector<ClassifiedTick>&)` decl + private `m_classifiedTickRing`.
- [ ] `src/ui/windows/TradingWindow.cpp`:
  - [ ] In the `tickByTickAllLast` ingestion path (existing tape handler): after pushing into the T&S ring, also classify by `m_lastBid` / `m_lastAsk` and push into `m_classifiedTickRing`.
  - [ ] `DrainClassifiedTicks(out)`: move-pop the ring into `out`; clear the ring.
- [ ] `src/main.cpp`:
  - [ ] `PushCvdTicksToCharts()` per §3e.
  - [ ] Call from `RenderTradingUI()` next to `PushUnguardedHintsToWindows()`.
- [ ] `core::services::TradingStyle.h`: add `indCvd` field to `StylePreset`; update all 5 cases; extend `ApplyPreset(...)`.
- [ ] `tests/test_chart_analysis.cpp` `[style]`: extend each preset with `indCvd` assertion.
- [ ] `cmake --build build -j$(nproc)` clean.
- [ ] `ctest --test-dir build --output-on-failure` — all targets pass; `[cvd]` cases pass.
- [ ] Manual smoke-test:
  - [ ] AAPL 1m chart open + AAPL TradingWindow open + same group → CVD sub-pane fills with live deltas; cumulative line trends with price.
  - [ ] Close the AAPL TradingWindow → sub-pane stops accumulating; tag indicates "no tape".
  - [ ] Switch chart symbol via group sync → CVD resets to zero (older symbol's bars now have no tape data).

### Task C — Divergence detector (CW-4)
**Status:** TODO

- [ ] `core::services::ChartAnalysis.h`: add `CvdDivergence` enum + `DetectCvdDivergence()` per §3b/§4d.
- [ ] `tests/test_chart_analysis.cpp` add `[divergence]` cases:
  - [ ] **Bullish regular** — `closes` makes lower low (e.g. `100, 95, 90` lows), `cvd` makes higher low (e.g. `-50, -30, -10`) → `BullishReg`.
  - [ ] **Bearish regular** — `closes` makes higher high, `cvd` makes lower high → `BearishReg`.
  - [ ] **No divergence** — closes and cvd both rising in lockstep → `None`.
  - [ ] **Insufficient swings** — series with only 1 detectable swing → `None`.
  - [ ] **Empty / mismatched-length** → `None`, no crash.
  - [ ] **Lookback boundary** — divergence between bar 0 and bar 49 with `lookback=50` → detected; with `lookback=30` → not detected.
- [ ] `src/ui/windows/ChartWindow.h`:
  - [ ] `AutoAnalysisSettings::cvdDivergence = false`.
  - [ ] `m_cvdDivergence` member + `DrawCvdDivergenceLabel()` decl.
- [ ] `src/ui/windows/ChartWindow.cpp`:
  - [ ] `ComputeIndicators()` tail (after CVD computation): call `DetectCvdDivergence` when both toggles are on.
  - [ ] `DrawCvdDivergenceLabel()`: right-edge label tag in `DrawOverlays()` (after setup overlay), mirroring the LONG SETUP / SHORT SETUP pattern.
  - [ ] `DrawAnalysisToolbar()`: `Div` checkbox gated on `m_ind.cvd`.
- [ ] `core::services::TradingStyle.h`: add `autoCvdDivergence` field to `StylePreset`; update all 5 cases; extend `ApplyPreset(...)`.
- [ ] `tests/test_chart_analysis.cpp` `[style]`: extend each preset with `autoCvdDivergence` assertion.
- [ ] `cmake --build build -j$(nproc)` clean.
- [ ] `ctest --test-dir build --output-on-failure` — all targets pass; `[divergence]` cases pass.
- [ ] Manual smoke-test: synthetic divergence by manually trimming/replaying a session where price marks a HH but CVD weakens — label appears; toggle Div off → label disappears immediately.

---

## 6. Acceptance / verification

For each task:
1. `cmake --build build -j$(nproc)` clean (no new warnings).
2. `ctest --test-dir build --output-on-failure` — all tests pass, including the new tag's cases.
3. Manual smoke matches the per-task list above.
4. Update `.claude/rules/architecture.md` with one paragraph per feature in the `ChartAnalysis` section.
5. Update `.claude/rules/task-history.md` with a `## Phase 15: Order Flow Insights` block listing the landed tasks.
6. Root `README.md`: one-line user-visible mention per feature.

---

## 7. Risks / sharp edges

- **Volume Profile rebucketing on every frame** — for 10k bars × 50 bins the cost is ~80k float ops per frame, fine. But if the user opens a 50k-bar chart (multi-year D1) the cost climbs to 400k ops; still under 1ms but worth monitoring. If it becomes a problem, cache the result and only recompute when `priceLo`/`priceHi` move beyond a 5% delta.
- **CVD borrow when no TradingWindow is open** — sub-pane is empty by design. But the user may not realise it needs a tape source. Mitigation: clear "no tape" placeholder text + the toolbar tooltip explicitly says "needs a TradingWindow open on this symbol".
- **Symbol switch via group sync** — when the chart's symbol changes, `m_barDeltas` and `m_cvd` need to clear (otherwise old AAPL deltas appear under MSFT bars). Hook into `SetSymbol(...)` like the rest of the buffers do.
- **NBBO classification at tick arrival is imperfect** — the rolling NBBO in TradingWindow is the *latest* bid/ask, not the bid/ask *at the moment the tick fired*. For very fast tape (illiquid names with rapid quote changes) this can misclassify. Acceptable for v1 — same approximation used by every other commercial CVD implementation that doesn't subscribe to BBO ticks separately. v2 polish: subscribe to `tickByTickBidAsk` alongside `AllLast` and pair them on sequence numbers.
- **Aggressor at the bid/ask exactly** — `price == ask` → buy; `price == bid` → sell; `price` between → neutral. Strict inequality risks classifying every executed-at-NBBO tick as neutral; using `<=`/`>=` matches industry convention.
- **Per-bar delta on empty bars** — bars with zero ticks render as zero-height bars (invisible). Fine. But the CVD line stays flat across those gaps, which can look "too smooth" — that's accurate (no tape = no information).
- **Divergence on a very recent swing** — `FindSwings` requires `swingK=3` bars on both sides, so the most recent swing point is at minimum `i = n-4`. On a fast-moving chart the user might want divergences detected sooner. Tradeoff: smaller `swingK` = noisier detection. Default 3 stays; expose in the popup if requests come in.
- **Divergence label collision with setup overlay** — both render right-edge labels. Stack them vertically: setup labels at y=entry/stop/target, divergence label at y=last close − 5%. Document the layering in `DrawOverlays()` ordering.
- **Free trading style** — leaves all three OFF as the construction baseline. Unlike Scalping/Day Trading/Swing, Free preserves the user's customisations on TF change (per Phase 13 Task #65). Confirm `setTradingStyle(Free)` doesn't stomp `m_ind.volumeProfile` / `cvd` / `m_auto.cvdDivergence` when the user has enabled them inside Free.
- **Cross-window borrow pattern is now the third instance** (unguarded-guard, group-sync, CVD-fan-out). Worth refactoring into a shared bus after Task B lands. Don't pre-build it.
- **TradingWindow tick-buffer overflow** — if the chart is throttled (closed/minimised) and not draining, the trading-window ring fills up. Ring capacity 4096 absorbs ~30s of heavy AAPL tape. Drop-oldest on overflow; CVD will simply have a small gap, which is correct behaviour for a "skipped frame".

---

## 8. Out of scope (explicitly deferred)

- **TradingWindow-side imbalance widget / footprint chart** — the previous mid-conversation proposal had an `Order Flow Insights` v1 covering both windows. This phase is ChartWindow-only; TradingWindow widgets deferred to a later phase (or rolled into the unified `WindowMessageBus` refactor).
- **Bar-delta approximation from close-position** — the close-position heuristic (close near high → bullish bar) was considered as a fallback for when no TradingWindow is open. Skipped: the heuristic is misleading on long-wick bars, and the "no tape" placeholder is more honest than fake data.
- **Anchored CVD / session-anchored CVD** — drag-to-set anchor for cumulative reset. Same shape as anchored-VWAP request from Phase 13. v2.
- **Volume Profile across-symbol comparison** — compare two profiles side-by-side. Niche. v2.
- **Hidden divergences** — bullish-hidden (price HL + CVD LL), bearish-hidden (price LH + CVD HH). Useful for trend-continuation; deferred until v1 regular divergences prove out.
- **TPO chart / Market Profile** — full TPO with letters by half-hour brackets. Out of scope; volume profile gives 80% of the trading signal at 5% of the implementation cost.
- **Persistent tick storage / backfill** — `reqHistoricalTicks` to populate older bars' CVD. Expensive (per-symbol per-day pacing limits) and the data is most useful for live decisions anyway.
- **Volume Profile on RTH-only vs full-session** — v1 always uses every loaded bar regardless of session. If users want RTH-only, layer it on top of the existing `m_useRTH` toggle (which already filters bars on load).

---

## 9. Update log

| Date | Change | By |
|---|---|---|
| 2026-05-06 | Plan written (draft) | claude / jose |
| 2026-05-06 | Task A (Volume Profile) landed as Task #74 | claude / jose |
