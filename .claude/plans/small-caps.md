# Small-Caps Mini-Phase — Halt/SSR Badge + Auto PMH/PML/PDH/PDL + Daily Loss Limit

**Status:** Draft (not yet started).
**Goal:** Three additions specifically aimed at small-cap day trading — the segment where halts, pre-market levels, and account-drawdown discipline matter most. Each task is independently shippable; they bundle into one phase because the same trader workflow uses all three together (spot a halt resume → confirm move respects PMH/PDH → submit within the daily risk budget).

This file is the source of truth across sessions — update **Status** and per-task checkboxes as work progresses.

---

## 1. Approved scope

| # | Feature | Default | Notes |
|---|---|---|---|
| A | **Halt / SSR / RegSHO status badge** | always-on when applicable | Surfaces IB's per-symbol halt status (`tickGeneric` field 49) and short-sale restriction (`tickString` field 46) as a coloured chip in ChartWindow + TradingWindow. Read-only. |
| B | **Auto PMH / PML / PDH / PDL levels** on chart | ON by default | Adds four deterministic levels to the auto-analysis layer: prior-day high/low (from yesterday's session) and pre-market high/low (from today's 04:00–09:30 ET window). Rendered with distinct labels so they don't blend with auto S/R. |
| C | **Daily loss limit + reduce-only mode** | OFF (user sets a $ floor) | App-wide gate. When realised+unrealised PnL crosses the user's daily floor, new *opening* orders are blocked; *closing* orders (reduce/flat) still go through. Reduce-only auto-clears at midnight ET. |

Defaults reflect the philosophy from prior phases — visualisation (A/B) is opt-in or always-on where unambiguous, but the safety rail (C) requires explicit user activation since "what counts as daily loss" is a personal choice.

## 2. Resolved decisions

- **Halt detection is read-only** — we *display* the halt; we never auto-cancel or auto-modify orders based on it. Halt-aware order routing is a future task.
- **SSR / RegSHO** is grouped into Task A's chip because the data flows through the same `tickString` plumbing. The chip shows whichever applies; if both, halt takes priority (halt is more urgent).
- **PMH/PML/PDH/PDL are deterministic, not statistical** — no clustering, no tolerance window. Compute once per chart per session, recompute when the session rolls (midnight ET) or the user switches symbols.
- **Daily loss limit is per-IB-account**, not per-window. One number per account, persisted to disk so a crash doesn't reset it mid-session.
- **Reduce-only mode is enforced at submit time in `main.cpp`**, not in window code — gating in one place prevents a per-window forget-to-check bug.
- **No new IB subscriptions for Task A** — halt/SSR ticks ride on the existing market-data subscription per chart/trading window.

## 3. Architecture

### Task A — Halt / SSR plumbing

IB EWrapper callbacks currently missing from `IBKRClient`:
- `tickGeneric(tickerId, field, value)` — field 49 = halted status. Values: `0 = not halted`, `1 = halted`, `2 = LULD halted (volatility pause)`.
- `tickString(tickerId, field, value)` — field 46 = ShortableShares (numeric string), field 47 = Fundamentals (ignore for now). The simpler signal is **field 32** = BID exchange and **field 46** which IB uses for SSR via the generic tick `"236"` (Shortable). The cleanest path:
  - Add `genericTickList = "165,236,233"` to `ReqMarketData` calls (165 = misc, 236 = shortable, 233 = RTVolume). Field 46 then carries shortable status; values:
    - `0` = not available to short / hard-to-borrow with no shares
    - `1` = available with conditions (typically SSR / RegSHO active)
    - `2` = available, easy-to-borrow
  - When `field 46 == 1`, the symbol is on **Reg SHO threshold / SSR list** — shorting requires uptick.

New `IBMessage` variants in `IBKRClient.h`:
```cpp
struct MsgTickGeneric { int tickerId; int field; double value; };
struct MsgTickString  { int tickerId; int field; std::string value; };
```

`IBKRClient` gains `tickGeneric` and `tickString` EWrapper overrides that `Push` the new variants. `ProcessMessages` dispatches via new public callbacks `onTickGeneric` / `onTickString`.

`main.cpp` adds tick routing per existing mktId range:
- Chart mktIds (10000–10999 rotated + 100–109 initial) → `ChartWindow::OnTickGeneric/OnTickString`
- Trading mktIds (110–119) → `TradingWindow::OnTickGeneric/OnTickString`
- Reuses the existing `g_tickerSymbols` validation pattern that already gates `tickPrice` / `tickSize`.

ChartWindow + TradingWindow gain:
```cpp
enum class HaltStatus { None, Halted, LULDPause };
enum class ShortStatus { Unknown, NotShortable, RegSHO, ShortableEasy };
HaltStatus  m_haltStatus  = HaltStatus::None;
ShortStatus m_shortStatus = ShortStatus::Unknown;
std::time_t m_haltSince   = 0;        // when halt was first observed; for elapsed-time chip
void OnTickGeneric(int field, double value);
void OnTickString (int field, const std::string& value);
void DrawStatusChips();               // renders halt/SSR chips inline in toolbar
```

`DrawStatusChips()` renders in the toolbar (or `DrawInfoBar()` on ChartWindow) using the same `BeginChild`+`AddRectFilled` pattern as `DrawUnguardedStrip`:
- **Halted**: red `IM_COL32(180, 30, 30, 230)` background, white `HALTED 14:23 (LULD)` text with elapsed-time counter
- **Reg SHO / SSR active**: amber `IM_COL32(220, 160, 30, 220)` `SSR — uptick only`
- **Hard-to-borrow / not shortable**: dim red `HTB — short rejected` (only shown when user hovers SELL button — too noisy otherwise)

Tooltip on the chip explains the IB regulatory meaning ("Limit Up / Limit Down pause — trading resumes at exchange discretion. New orders may be queued; market-on-resume orders fill at the auction print.").

**Notification hook**: when `m_haltStatus` transitions `None → Halted/LULDPause`, fire `NotificationService::Notify(Warning, OrderRouting?, SymbolHalted, "AAPL halted (LULD)")`. Add new `NotificationEvent::SymbolHalted` after `OrderHeld`.

### Task B — Auto PMH / PML / PDH / PDL

`core::services::ChartAnalysis.h` gains:
```cpp
struct PriorDayLevels {
    bool   valid = false;
    double pdh   = 0.0;   // prior day high  (Regular session only)
    double pdl   = 0.0;   // prior day low
    double pmh   = 0.0;   // pre-market high (today's 04:00–09:30 ET window)
    double pml   = 0.0;   // pre-market low
    int    pdFirstIdx = -1, pdLastIdx = -1;   // span in m_xs for rendering
    int    pmFirstIdx = -1, pmLastIdx = -1;
};

// Pure: caller passes the chart's flat-array snapshot. Walks backwards from the
// tail using localtime ET-day boundary detection + BarSession classification.
PriorDayLevels ComputePriorDayLevels(const std::vector<double>& xs,
                                      const std::vector<double>& highs,
                                      const std::vector<double>& lows,
                                      bool isIntraday);
```

Algorithm:
1. If `!isIntraday`, return `valid=false` (PMH/PML only meaningful intraday; PDH/PDL on a daily chart is just the prior bar, which the user already sees).
2. Walk `xs` backwards using `localtime` to identify:
   - `todayStart` = first index of today's bars
   - `prevDayStart`, `prevDayEnd` = first/last index of yesterday's bars
   - `pmStart`, `pmEnd` = first/last index of today's pre-market bars (04:00 ≤ ET < 09:30, using `BarSession::PreMarket`)
3. PDH/PDL: max/min over `prevDay[Regular session bars only]` — skip pre/after-hours of yesterday so PDH matches what Bloomberg, Finviz, etc. show.
4. PMH/PML: max/min over `pm[start..end]`. If no pre-market bars (weekends, holidays, or chart only loaded RTH), set `valid=false` for those two levels but keep PDH/PDL.

`ChartWindow.h` adds to `AutoAnalysisSettings`:
```cpp
bool priorDayLevels  = true;     // master toggle
bool showPDH = true, showPDL = true;
bool showPMH = true, showPML = true;
```

`m_priorLevels` member populated by `DetectStructure()` (alongside existing pivots / fib / breakouts). Computed once per `DetectStructure()` pass — cheap, just two min/max loops over yesterday's bars and today's pre-market range.

`DrawPriorDayLevels()` invoked from `DrawOverlays()` after `DrawAutoPivots()` (so user pivots + prior-day levels coexist). Each level is a dashed h-line spanning `[firstIdx, m_xs.size()-1]` (PDH/PDL extend from yesterday's range through today; PMH/PML extend from pre-market start through current bar) with right-edge labels:
- **PDH** in `IM_COL32(220, 100, 220, 200)` magenta, label `PDH 187.42`
- **PDL** in `IM_COL32(220, 100, 220, 200)` magenta, label `PDL 184.15`
- **PMH** in `IM_COL32(140, 200, 240, 200)` light blue, label `PMH 188.95`
- **PML** in `IM_COL32(140, 200, 240, 200)` light blue, label `PML 185.20`

These four are intentionally on different hues from auto-S/R (red/green), zones (translucent red/green), trend (green/red/grey), pivots (red/green/grey), Fib (gradient) — so they read as a distinct "deterministic levels" family.

`DrawAnalysisToolbar()` Auto: row gains a `PD/PM` checkbox after `Pivots`. `DrawAutoSettingsPopup()` gains four sub-checkboxes (Show PDH / PDL / PMH / PML) so the user can suppress individual lines.

**StylePreset integration** (`core::services::TradingStyle.h`): `priorDayLevels` field added to `StylePreset`:
- Scalping: `true` (every level matters on M1)
- Day Trading: `true` (these levels drive the whole day's plan)
- Swing: `false` (D1+ chart, PMH/PML resolution doesn't fit)
- Investment: `false`
- Free: `true` (default baseline)

`ApplyPreset` stamps `priorDayLevels` and `showPDH/PDL/PMH/PML` onto `AutoAnalysisSettings`.

### Task C — Daily loss limit + reduce-only mode

Per-account state in `main.cpp`:
```cpp
struct DailyRiskState {
    bool        enabled       = false;
    double      lossLimitUSD  = 0.0;   // positive value; PnL ≤ -lossLimitUSD triggers reduce-only
    bool        reduceOnly    = false;
    std::time_t lastSessionRollover = 0;   // unix-ts of last midnight-ET reset
};
static std::unordered_map<std::string, DailyRiskState> g_dailyRisk;   // keyed by IB account
```

Persisted to `~/.config/ibkr-trading-app/daily-risk.cfg` (atomic `.tmp`+`rename`, same pattern as `watchlists.cfg`). Format:
```
ACCOUNT:DU1234567
ENABLED:1
LOSS_LIMIT:500.00
REDUCE_ONLY:0
ROLLOVER:1747094400
```

Loaded once on `FinishConnect(false)` after `CreateTradingWindows()`. Saved on every state change (enable/disable, limit edit, reduce-only flip) and on `DestroyTradingWindows()`.

**Session rollover**: at every frame, check if `std::time(nullptr)` has crossed the next midnight-ET boundary since `lastSessionRollover`. If so:
- Reset `reduceOnly = false`
- Update `lastSessionRollover` to today's midnight ET
- Save file
- Fire `NotificationService::Notify(Info, AccountState, NewSessionStart, "Daily risk reset for <account>")`

(New `NotificationEvent::NewSessionStart` and `NotificationEvent::DailyLossLimitHit` appended after `SymbolHalted` from Task A. Same convention as the existing Append-after-Test rule.)

**PnL evaluation**: existing `g_PnLData` (from PortfolioWindow's `OnPnLData`) carries `daily`, `unrealized`, `realized` per account. Each frame in `RenderTradingUI()`, before drawing windows:
```cpp
double totalPnL = g_PnLData[account].daily;   // already includes unreal + real
if (g_dailyRisk[account].enabled && !g_dailyRisk[account].reduceOnly
        && totalPnL <= -g_dailyRisk[account].lossLimitUSD) {
    g_dailyRisk[account].reduceOnly = true;
    SaveDailyRiskFile();
    if (g_NotificationService) g_NotificationService->Notify(
        Critical, AccountState, DailyLossLimitHit,
        "Daily loss limit hit", "Reduce-only mode active until midnight ET.");
}
```

**Order gating** in `main.cpp` — every order submission is funneled through a thin gate:
```cpp
// Returns true if the order is allowed; false (with toast) if blocked.
static bool GateOrderForDailyRisk(const core::Order& o) {
    auto& risk = g_dailyRisk[g_selectedAccount];
    if (!risk.enabled || !risk.reduceOnly) return true;

    // Allowed: closing-side orders for an existing position
    auto posIt = g_positions.find(o.symbol);
    double posQty = (posIt != g_positions.end()) ? posIt->second.quantity : 0.0;
    bool isClosing =
        (posQty > 0 && o.side == core::OrderSide::Sell && std::abs(o.quantity) <= posQty + 1e-9) ||
        (posQty < 0 && o.side == core::OrderSide::Buy  && std::abs(o.quantity) <= -posQty + 1e-9);
    if (isClosing) return true;

    // Blocked: opening / adding / flipping orders
    if (g_NotificationService) g_NotificationService->Notify(
        Warning, OrderRouting, OrderBlocked,
        "Order blocked",
        "Reduce-only mode active — only closing orders allowed for the rest of the session.");
    return false;
}
```

All `PlaceOrder(o)` call sites in `main.cpp` (the chart `OnOrderSubmit` lambda, the trading `OnOrderSubmit` lambda, the bracket STP/TP fan-out in `onFillReceived`, the DOM click-to-trade path) prepend a `if (!GateOrderForDailyRisk(o)) return;` guard.

**UI surface** — Settings panel gains a *Daily Risk* section:
- Per-account combo (defaults to `g_selectedAccount`)
- "Enable daily loss limit" checkbox
- `$ Loss limit` numeric input (USD, ≥ 0)
- Read-only chip showing current daily PnL with color (green/red)
- "Currently: ACTIVE / REDUCE-ONLY" badge
- "Reset reduce-only now (manual override)" button — clears flag immediately without waiting for midnight ET. Fires confirmation modal.

**Menu-bar warning**: when `reduceOnly` is active for the selected account, an amber `REDUCE-ONLY` chip appears in the top menu bar (right of the DISCONNECTED badge area), with the same elapsed-since-trigger counter pattern as the halt chip.

**Order forms**: TradingWindow's submit button and ChartWindow's BUY/SELL buttons render an amber outline (not a full disable — the user can still try, the gate provides the block + toast) when `reduceOnly` is active.

New `NotificationEvent::OrderBlocked` appended after `DailyLossLimitHit`.

## 4. Plumbing dependencies

| Need | Where it comes from |
|---|---|
| `g_PnLData[account].daily` | Already populated by `PortfolioWindow::OnPnLData` (Task #40) |
| `g_positions[symbol]` | Already populated by `onPositionData` / `onPortfolioUpdate` (Task #58) |
| `g_selectedAccount` | Already maintained by `RenderAccountSelectorUI` (Task #45) |
| `genericTickList` parameter on `ReqMarketData` | Already passes `"165,233"` — extend to `"165,233,236"` |
| Tone + voice WAVs for new events | Add 3 entries to `tools/gen_tones.cpp` + `tools/voice_phrases.txt`, run `./tools/gen_voice.sh` |

## 5. Tasks

- [ ] **Task A1** — IBKRClient halt/SSR plumbing. Add `MsgTickGeneric` + `MsgTickString` variants, override `tickGeneric` / `tickString` in `IBKRClient`, expose `onTickGeneric` / `onTickString` callbacks, extend genericTickList to include `"236"` on all `ReqMarketData` paths. Dispatch tests in `test_ibkr_queue.cpp` (2 new cases / ~8 assertions).

- [ ] **Task A2** — ChartWindow + TradingWindow halt/SSR chips. `m_haltStatus` / `m_shortStatus` / `m_haltSince` members, `OnTickGeneric` / `OnTickString` handlers, `DrawStatusChips()` rendered in toolbar / InfoBar. Wire `main.cpp` tick routing for chart and trading mktIds. New `NotificationEvent::SymbolHalted` + asset entries.

- [ ] **Task B** — `ComputePriorDayLevels` + tests + ChartWindow integration. Pure helper in `ChartAnalysis.h`; `[prior-day]` Catch2 tag with ~10 cases (single-day input, weekend-skip, no-prior-day, no-pre-market, mixed Regular+PreMarket+AfterHours bars). `AutoAnalysisSettings::priorDayLevels` + per-side toggles, `m_priorLevels`, `DetectStructure()` call, `DrawPriorDayLevels()` invocation from `DrawOverlays()`. `StylePreset.priorDayLevels` + `ApplyPreset` propagation; `[style]` test updates for all 5 presets.

- [ ] **Task C1** — `DailyRiskState` + persistence. New struct, `~/.config/ibkr-trading-app/daily-risk.cfg`, `LoadDailyRiskFile()` / `SaveDailyRiskFile()`, hook into `FinishConnect(false)` and `DestroyTradingWindows()`. Per-frame midnight-ET rollover check in `RenderTradingUI()`. New `NotificationEvent::NewSessionStart` / `DailyLossLimitHit` / `OrderBlocked` + asset entries.

- [ ] **Task C2** — Order gating + UI surface. `GateOrderForDailyRisk(o)` helper, guards prepended to all 4 `PlaceOrder` call sites (chart submit, trading submit, bracket fan-out, DOM click-to-trade). Settings panel *Daily Risk* section, menu-bar `REDUCE-ONLY` chip, amber outline on submit buttons.

## 6. Edge cases & gotchas

### Task A
- IB sends `tickGeneric` for halt status even when the symbol isn't halted (value=0) — only update `m_haltSince` on the `0 → 1|2` transition, not on every tick.
- LULD pause is normally 5 minutes; the chip should clear automatically when IB sends `value=0`. No timeout fallback — trust IB's state.
- Field 46 (shortable) updates only on subscription start and material changes — don't expect a tick on every order. Cache the latest value; show `Unknown` until first arrival.
- TWS vs Gateway: `tickGeneric` field 49 exists in both, but some Gateway versions delay halt ticks by up to 2 seconds. Add this to the tooltip ("Up to 2-second delay from exchange").

### Task B
- DST transitions: `localtime`'s day-boundary detection handles DST correctly because the kernel returns the local time including the DST shift. Test cases must include a Sunday-spring-forward and Sunday-fall-back boundary explicitly.
- Holidays / shortened sessions: a 13:00 ET early-close day means yesterday's `Regular` session ended at 13:00, not 16:00. `BarSession` already classifies these correctly because it uses the bar's actual timestamp; the algorithm doesn't need to know about half-days.
- Symbols that don't trade pre-market (illiquid small caps, some ETFs): `PMH`/`PML` will land on `valid=false` for those two levels. Skip the line render; keep PDH/PDL.
- Chart open during pre-market: PMH/PML grow live as bars arrive. Re-run on every `DetectStructure()` so the level extends with each new pre-market high/low.
- Pre-market window definition: 04:00 ≤ ET < 09:30. After 09:30, PMH/PML *freeze* — they don't continue updating from RTH bars. The line still renders, just at the locked value.
- Sub-minute timeframes (when we eventually add them): unchanged. Min/max over whatever bars fall in the pre-market window.

### Task C
- Account switch (multi-account live sessions): when the user switches accounts via the menu-bar combo, swap to that account's `DailyRiskState`. The previous account's reduce-only state persists (still in the map) but doesn't affect the new account's order gate.
- Negative `lossLimitUSD` is rejected at input (`InputDouble` clamp to ≥ 0).
- `g_PnLData[account].daily` may be stale by a few seconds — that's fine, the limit is approximate. We toast once on first crossing, not continuously.
- Existing brackets at the moment reduce-only triggers: leave them alive. The TP and STP legs are *closing* orders and are allowed to fill. New brackets would be blocked at the entry leg (opening).
- Closing-leg detection in `GateOrderForDailyRisk` uses `g_positions[symbol]` — if a fresh-fill position hasn't yet reached `g_positions` (IB's `onPosition` callback lags `onExecDetails` by a tick), an immediate close-on-fill could be misclassified as opening. Mitigation: also consult `g_pendingLocalAccept` and the most recent fills; if there's an *opposing* fill within the last 2 seconds, treat the order as closing. Document this as the v1 known limitation; revisit if it bites.
- App restart with `reduceOnly=true` mid-session: persistence preserves it. On next connect, `LoadDailyRiskFile()` reads the flag, midnight-ET rollover check fires, and if we're still in the same trading day, the flag stays. Only midnight clears it (or the manual override button).

## 7. Out of scope (deferred)

- **Halt-aware order routing** — auto-cancel pending opens during a halt, queue them for resume. Bigger UX decision, separate task.
- **Float / shares-outstanding display** — IB's data is incomplete; better sourced externally (Finviz scrape or paid feed).
- **Hard-to-borrow rate** — `reqMarketRule` doesn't carry borrow rate; would need a separate IB subscription or external data.
- **Premarket VWAP** — already covered by Phase 13 VWAP if user toggles `useRTH=false`.
- **Weekly / monthly equivalents** (WK-high, WK-low, etc.) — useful but lower-priority than the daily levels.
- **Daily profit *target*** (mirror of loss limit) — same plumbing if/when wanted, but emotionally distinct (profit targets encourage premature flatten; small-cap traders typically prefer pure stop discipline).

## 8. Acceptance per task

**Task A:**
- AAPL during normal trading: no chip rendered.
- AAPL during a LULD pause (or manually triggered halt on a paper symbol): red `HALTED 14:23 (LULD)` chip with live elapsed-time counter; clears on resume.
- A symbol on Reg SHO list (e.g. recently-volatile small cap): amber `SSR — uptick only` chip.
- New `Symbol halted` notification fires on transition; tone + voice WAVs play.

**Task B:**
- Open AAPL M5 in pre-market: PMH/PML/PDH/PDL all visible with right-edge labels.
- After 09:30 ET: PMH/PML lines freeze at their pre-market peak/trough.
- Switch to /ES front-month: PDH/PDL recompute from yesterday's Regular bars; PMH/PML show overnight high/low if `useRTH=false` (PMH/PML are still labelled even though /ES has no formal pre-market).
- D1 timeframe: levels suppressed (`isIntraday=false` early-return).
- Tests-core pass with the new `[prior-day]` tag (~10 cases / ~30 assertions).
- `[style]` test cases pass with new `priorDayLevels` defaults across all 5 presets.

**Task C:**
- Set $500 limit on paper account, take losing trades until daily PnL ≤ -$500 → reduce-only triggers, toast fires, menu-bar chip appears, BUY/SELL buttons gain amber outline.
- Try to place a new opening order → blocked with `Order blocked` toast.
- Try to place a closing order against an existing position → allowed.
- Manual override → reduce-only clears immediately.
- Kill and restart the app same session: persisted limit + reduce-only flag survive.
- After midnight ET (or manual local clock advance for testing): reduce-only clears, new session toast fires.

## 9. Open questions

1. **Reduce-only scope for brackets**: when a user submits a *bracket* in reduce-only mode against a position they're trying to manage (entry leg is opening, STP+TP are closing), the entry leg is blocked → the bracket never fires → STP+TP never submitted. Acceptable v1 behaviour, but worth flagging.
2. **Halt notification spam**: a basket of small caps all halting at the same time (sector-wide LULD on a news event) could fire 5+ toasts in seconds. Consider rate-limiting `SymbolHalted` to one toast per minute per symbol. Or accept it — these are real events the trader needs to know about.
3. **PMH/PML on chart open after RTH start**: if the chart loads at 10:00 ET, IB's historical bars include pre-market by default — PMH/PML are correct. But if `useRTH=true` on initial load, pre-market bars are filtered out → PMH/PML show `valid=false`. Solution: the algorithm explicitly walks `m_xs` even when `useRTH=true` because session-filter dropping happens later in `RebuildFlatArrays`. We compute against `m_series.bars` (raw) for PMH/PML, then render. Verify this works correctly.
