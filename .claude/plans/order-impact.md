# Order Impact — Side Intent Badge + P&L Preview

Surface, before order submission, **(a) what the order does to the user's position** (open / add / reduce / close / flip) and **(b) the projected P&L at the prices currently entered**. Applies to both `ChartWindow` and `TradingWindow`. Read-only — no behaviour change, only visualisation.

## Motivation

User has to mentally compute: "I'm long 100 @ $150, if I SELL 100 LIMIT $155 with a STP $148, what's my P&L?" The math primitives already exist (see "Existing math" below), but they're inlined in render code and not surfaced to the user before they click. This plan extracts them into a pure helper, adds a side-intent badge near BUY/SELL, and a P&L preview line under the price inputs.

## Existing math to reuse

- `ChartWindow.cpp:2314-2323` — closing-leg P&L formula:
  - long close: `(price - avgCost) * closeQty - commission`
  - short close: `(avgCost - price) * closeQty - commission`
- `ChartWindow.cpp:2517-2524` — closing-side detection:
  - `closingLong  = side==SELL && qty >  ε`
  - `closingShort = side==BUY  && qty < -ε`
  - `closeQty    = min(orderQty, |posQty|)`
- `ChartWindow.cpp:2087-2093` — break-even formula (`avgCost + commission/qty`).

These three blocks become the body of `ComputeOrderImpact`.

## Pure helper — `core::services::ChartAnalysis.h`

```cpp
enum class OrderImpactKind {
    OpenLong,            // no position → BUY
    OpenShort,           // no position → SELL
    AddToLong,           // long pos    → BUY
    AddToShort,          // short pos   → SELL
    ReduceLong,          // long pos    → SELL (partial close)
    ReduceShort,         // short pos   → BUY  (partial close)
    CloseLong,           // long pos    → SELL (exact close)
    CloseShort,          // short pos   → BUY  (exact close)
    FlipToShort,         // long pos    → SELL  (qty > posQty)
    FlipToLong,          // short pos   → BUY   (qty > |posQty|)
    Invalid,             // qty<=0, side unset, etc.
};

struct OrderImpact {
    OrderImpactKind kind        = OrderImpactKind::Invalid;
    double          closeQty    = 0.0;   // units that hit avgCost basis
    double          openQty     = 0.0;   // units left over (flip case)
    double          closePnL    = 0.0;   // realised P&L on the closing leg, net of commission share
    double          newAvgCost  = 0.0;   // post-fill avg cost (Add/Open paths)
    double          newPosQty   = 0.0;   // signed position after fill
    bool            isClosingPath = false; // true for Reduce/Close/Flip — closePnL is meaningful
};

// Pure: no I/O, no state. Caller passes existing position + the staged order's
// {side, qty, fillPrice} where fillPrice is whatever price will be hit if the
// order fills (LMT → limitPrice; STP → stopPrice; STP LMT → stopPrice for the
// stop leg + limitPrice for the limit leg, callers compute twice; MKT → last).
OrderImpact ComputeOrderImpact(double posQty, double avgCost, double commissionPerShare,
                                bool isBuy, double orderQty, double fillPrice);

// Risk preview for paired Stop-Limit-style setups. Computes both target P&L
// and stop-out P&L from the same impact base. R:R = |reward| / |risk|.
struct StopTargetPreview {
    double targetPnL = 0.0;
    double stopPnL   = 0.0;   // negative for losses
    double rrRatio   = 0.0;   // 0 if either leg is undefined
    bool   valid     = false;
};
StopTargetPreview PreviewStopTarget(const OrderImpact& target, const OrderImpact& stop);
```

Pre-fill price selection by order type:
- `Market` / `MOC` / `MTL`        → `last` (current price)
- `Limit` / `LOC`                 → `limitPrice`
- `Stop`                          → `stopPrice`
- `StopLimit`                     → `stopPrice` for the stop leg, `limitPrice` for the worst-case-fill leg
- `Trail` / `TrailLimit`          → derive trigger from `last - trailAmount` (long) or `last + trailAmount` (short)
- `MIT`                           → `auxPrice`
- `LIT`                           → `auxPrice` for trigger, `limitPrice` for fill leg
- `Midprice`                      → `last` (best estimate)
- `Relative`                      → `last + pegOffset` (BUY) / `last - pegOffset` (SELL)

For **paired** previews (target leg + protective stop), the user specifies both prices in the order panel — for now we only have a Stop-Limit form on TradingWindow that *is itself* a single order, so the paired case is "the staged order is a closing limit AND there's already a live stop on the book" or "the staged order is a setup overlay where Phase 12 already produces entry/stop/target prices". The Phase 12 `SetupPlan` is the natural feeder for `PreviewStopTarget` — wire `target = SuggestSetup.target`, `stop = SuggestSetup.stop`.

## Tests — `tests/test_chart_analysis.cpp`, new `[order-impact]` tag

1. `ComputeOrderImpact: no position + BUY  → OpenLong, closeQty=0, openQty=N, newPosQty=+N`
2. `ComputeOrderImpact: no position + SELL → OpenShort, closeQty=0, openQty=N, newPosQty=-N`
3. `ComputeOrderImpact: long 100@$150 + BUY 50@$160 → AddToLong, newAvgCost=$153.33, newPosQty=+150, closePnL=0`
4. `ComputeOrderImpact: long 100@$150 + SELL 50@$155 → ReduceLong, closeQty=50, closePnL=+250 (gross), newPosQty=+50`
5. `ComputeOrderImpact: long 100@$150 + SELL 100@$155 → CloseLong, closeQty=100, closePnL=+500, newPosQty=0`
6. `ComputeOrderImpact: long 100@$150 + SELL 150@$155 → FlipToShort, closeQty=100, openQty=50, closePnL=+500, newPosQty=-50, newAvgCost=$155`
7. Mirror cases 3-6 for short positions (symmetric formulas)
8. `ComputeOrderImpact: long 100@$150 + SELL 50@$140 → ReduceLong with closePnL=-500 (loss path)`
9. `ComputeOrderImpact: degenerate (qty<=0, fillPrice<=0) → Invalid`
10. `PreviewStopTarget: long 100@$150 with target=$155 stop=$148 → targetPnL=+500, stopPnL=-200, rrRatio=2.5, valid=true`
11. `PreviewStopTarget: invalid leg → valid=false`
12. Commission propagation: closePnL is net of `commissionPerShare * closeQty` (per-share, not absolute)

Add to `tests-core` (already linked). Expect ~12 cases / ~50 assertions.

## ChartWindow integration — `DrawTradePanel`

Below the BUY/SELL row (after the `[Use suggestion]` button, before any other inline content), render an **inline badge** spanning the row width:

```
Layout: [icon] OPEN LONG  · 100 sh @ $187.42 · cost ≈ $18,742
                                ▲ when posQty=0 and side=BUY
or:     [icon] CLOSE LONG · 100 sh · est. P&L +$425.00 (+2.27%)
                                ▲ when posQty=+100 and side=SELL @ avgCost+price
or:     [icon] FLIP TO SHORT · close 100 (+$425) → open 50 short @ $187.42
                                ▲ when posQty=+100 and SELL 150
```

Colour the badge by `OrderImpactKind`:
- `Open*` / `AddTo*` → blue (informational)
- `Reduce*` / `Close*` with closePnL > 0 → green
- `Reduce*` / `Close*` with closePnL ≤ 0 → red
- `Flip*` → orange (warning — two legs)

Hide the badge when `m_orderQty <= 0`. Hide when no fill price is derivable yet (e.g., LIMIT with no price entered). Use a `BeginChild` with `ImGuiChildFlags_Borders` 30px tall — same pattern as `DrawUnguardedStrip`.

For Stop-Limit and the Phase 12 setup overlay (`m_setup.valid`), append a second line under the badge:

```
Target $192.10 +$X.XX  |  Stop $184.97 -$Y.YY  |  R:R 2.1
```

Use `core::services::PreviewStopTarget(targetImpact, stopImpact)` after computing each leg's `OrderImpact`.

## TradingWindow integration — `DrawOrderEntry`

Same badge, same logic. Insert above the [BUY] [SELL] row (currently around the form's bottom). Use the form's current `m_qtyBuf`/`m_lmtBuf`/`m_stpBuf`/`m_typeIdx`/`m_sideIdx` to derive the fillPrice via the same per-type lookup as ChartWindow. Reuse the helper, no duplication.

## Position source

Both windows already track position state:
- `ChartWindow::m_position` (`PositionInfo` struct populated by `SetPosition`)
- `TradingWindow::m_position` — verify (search). If not present, plumb similarly to the unguarded-strip path: `main.cpp` already has `g_positions` and pushes per-symbol position to chart windows; mirror to trading windows in the same per-frame fan-out (`PushUnguardedHintsToWindows` is the model).

`commissionPerShare` defaults to 0 if the position has no fill history yet (chart window currently stores `m_position.commission` as a *total* — derive per-share as `commission / |qty|` when qty > 0).

## Files to edit

1. `src/core/services/ChartAnalysis.h` — add `OrderImpactKind`, `OrderImpact`, `StopTargetPreview`, `ComputeOrderImpact`, `PreviewStopTarget` (all inline `core::services::` free functions, pure).
2. `src/ui/windows/ChartWindow.h` — declare `DrawOrderImpactBadge()` private method, no new state needed (recomputed each frame).
3. `src/ui/windows/ChartWindow.cpp` — implement `DrawOrderImpactBadge()`, call from `DrawTradePanel` after the BUY/SELL/`[Use suggestion]` row. Refactor the inline `closingLong`/`closingShort`/`closePnL` blocks at lines 2314-2323 and 2517-2524 to call `ComputeOrderImpact` (de-duplicates).
4. `src/ui/windows/TradingWindow.h` — declare `DrawOrderImpactBadge()`. Possibly add `PositionInfo m_position` + `SetPosition()` if not already present.
5. `src/ui/windows/TradingWindow.cpp` — implement `DrawOrderImpactBadge()`, call from `DrawOrderEntry` above [BUY] [SELL] row.
6. `src/main.cpp` — if TradingWindow doesn't already receive position pushes, extend the per-frame fan-out in `RenderTradingUI` next to `PushUnguardedHintsToWindows` (call it `PushPositionInfoToWindows` and route it to both chart + trading windows from `g_positions`).
7. `tests/test_chart_analysis.cpp` — add `[order-impact]` cases.
8. Docs: `.claude/rules/architecture.md` (add an "Order Impact" section under the existing "Setup Suggestions" / "Unguarded-Position Guard" pattern), `.claude/rules/testing.md` (add `[order-impact]` to the tests-core listing), `.claude/rules/task-history.md` (new task entry), README.md if user-facing).

## Acceptance

- Build clean.
- All `[order-impact]` cases pass under `tests-core`.
- Live IB smoke-test deferred (manual): with a paper account, verify badge updates as user types qty / limit; verify FLIP path renders both legs; verify badge clears when qty=0.

## Out of scope (defer)

- Buying-power impact (requires account margin info — not currently subscribed).
- Multi-leg conditional orders (OCO / bracket).
- Tax-lot-aware P&L (uses average cost only).
- Persisting the badge state across reloads (recomputed each frame anyway).
