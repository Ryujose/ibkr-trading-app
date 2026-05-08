# Testing Rules

## Framework

**Catch2 v3.7.1** fetched via FetchContent when `IBKR_BUILD_TESTS=ON`.
Registered with CTest via `catch_discover_tests()`.

## Test Targets

| Target | Source files | Links | Purpose |
|---|---|---|---|
| `tests-core` | `test_market_data.cpp`, `test_models.cpp`, `test_ibkr_utils.cpp`, `test_chart_analysis.cpp`, `test_replay.cpp` | Catch2 only | Pure logic — no IB API dependency |
| `tests-ibkr` | `test_ibkr_queue.cpp` + `IBKRClient.cpp` | Catch2 + ibapi-lib | IBKRClient message dispatch (component tests) |

## What Is Tested

### tests-core
- **`MarketData.h`** — `TimeframeLabel`, `TimeframeSeconds`, `TimeframeIBBarSize`, `TimeframeIBDuration` for all 9 timeframes; `IsUSDST` DST boundary cases; `BarSession` session classification at all ET session boundaries (EDT and EST)
- **Model struct defaults** — `Order`, `Fill`, `DepthLevel`, `ScanResult`, `ScanFilter`, `Position`, `AccountValues`, `Bar`, `PendingBracketStop` (7 fields including `outsideRth`/`useStopLmt` → false)
- **Enum string helpers** — `OrderSideStr`, `OrderTypeStr`, `TIFStr`, `OrderStatusStr`, `ScanPresetLabel`, `AssetClassLabel`
- **`IBKRUtils.h`** — `ParseStatus` (all IB status strings, case-sensitivity); `ParseIBTime` (8-digit date → noon UTC, Unix timestamp round-trip, formatted datetime)
- **`ChartAnalysis.h`** — `FindSwings` (peak/trough/W-shape/flat-top/scanCap), `ATR` (constant true-range, too-small input), `ClusterLevels` (within/outside tolerance, ascending order, `minPrice`/`maxPrice` from mixed-input swings, single-swing min==max==price invariant), `KeepTopN` (side+touches filter, ranking, no-cap mode), `LinearRegression` (exact recovery of `y = ax + b`, flat line slope=0, lookback < n trailing-window selection, period-4 unbiased noise pattern, too-small/empty input → `valid=false`), `DonchianBands` (rolling N-bar max/min on a known sequence, too-small input zeros, window==n collapse), `LargestSwingSpan` (max-span pair across all considered swings, window=1 picks only most-recent of each, empty list → invalid, zero-span → invalid), `ClassicPivots` (P/R/S from H=110/L=90/C=100, S3<S2<S1<P<R1<R2<R3 invariants, invalid input → `valid=false`), `FindBreakouts` (synthetic up/down-breakout marks, minTouches filter, causality guard against future-established levels, empty/degenerate inputs), `RoundToTick` ($0.01 default snap, custom $0.05 / $0.0001 ticks for options / sub-dollar, `tick<=0` no-op), `AvoidRoundNumber` (already-safe pass-through, push down/up at $10.00 / $187.50, near-1.00 cross-integer push, degenerate `pad<=0` / `price<=pad` / sub-cent pad), `SuggestSetup` (long inside-zone, `last>zoneTop` clamp, R:R rejection, short mirror, degenerate input), `SuggestStopForPosition` (long picks closest support, single-touch/empty/no-qualifying rejection, short picks closest resistance, **IB error 110 regression**: outputs are tick-clean even when `AvoidRoundNumber` produces sub-cent FP drift), `PositionSizeShares` (basic risk math, degenerate inputs → 0), `ComputeOrderImpact` (all 10 `OrderImpactKind` paths: OpenLong/OpenShort, AddToLong/AddToShort, ReduceLong/ReduceShort, CloseLong/CloseShort, FlipToShort/FlipToLong; commission propagation; degenerate input → Invalid), `PreviewStopTarget` (long $150 entry + target=$155 stop=$148 → R:R=2.5; non-closing/invalid legs), `ShouldReplaceHistoricalBars` (empty completion on existing data → reject; empty on empty → accept; symbol mismatch → reject; symbol match → accept; ratchet blocks ≤5 when existing ≥50; ratchet allows ≥6 and <50 existing; empty existing always accepts; 6 cases / 16 assertions), and an end-to-end V-shape pipeline assertion that also checks every cluster's min/max agrees with its actual swing constituents
- **`ChartAnalysis.h` — VWAP (`[vwap]` tag, 6 cases / 84 assertions)** — `SessionVwap` known volume-weighted average across three bars (typical=`{100,102,101}`, vol=`{1k,2k,3k}` → vwap[2]≈101.166); single-bar input (vwap==typical, bands collapsed); session reset wipes carry-over; zero-volume bar carry-forward; empty / mismatched-length input → zero-filled / no crash; band-ordering invariant (`sd2Dn ≤ sd1Dn ≤ vwap ≤ sd1Up ≤ sd2Up` element-wise on a varying series)
- **`TradingStyle.h` (`[style]` tag)** — `TradingStyleLabel` / `TradingStyleShort` cover all five modes including `Free`/`FREE`; `GetPreset(s)` field-by-field assertions on each of `Scalping` / `DayTrading` / `Swing` / `Investment` / `Free` (timeframe / `historyDuration` / swing/trend/maxLevels/minTouches/scanCap params / setupOverlay/rrMin/atrPad/roundPad/stopOffset/riskPct / useStopLmt / `indVwap` + `indVwapBands` + `indVolumeProfile`); the Free preset asserts construction-default baseline (D1 placeholder TF, "6 M" placeholder duration, indicator/auto/setup defaults match `IndicatorSettings`/`AutoAnalysisSettings`/`SetupSettings` constructors); `ApplyPreset` against hand-rolled `Ind` / `Auto` / `Setup` stub structs proves every overridable field is stamped (including `volumeProfile`); round-trip Scalping → Investment + Investment → Free leaves no carry-over from the previous preset; second leg DayTrading → Free clears `volumeProfile` and `vwapBands` back to baseline (Free-mode TF preservation lives in `ChartWindow::setTradingStyle`, not in the pure helper — `ApplyPreset(Free)` always stamps D1 onto the caller's tf ref)
- **`ChartAnalysis.h` — Order Impact (`[order-impact]` tag, 17 cases / 74 assertions)** — `ComputeOrderImpact` all 10 `OrderImpactKind` paths: no-position BUY→OpenLong / SELL→OpenShort; long 100@$150 paths (BUY 50@$160→AddToLong newAvgCost=$153.33, SELL 50@$155→ReduceLong +$250, SELL 100→CloseLong +$500, SELL 150→FlipToShort close=100+$500 open=50 short); short 100@$150 mirror paths (SELL 50→AddToShort, BUY 50@$148→ReduceShort +$100, BUY 100→CloseShort +$200, BUY 150→FlipToLong); loss-path coverage (SELL 50@$140→-500); commission propagation ($0.01/sh × 50sh = $249.50 net); degenerate (qty≤0, fillPrice≤0→Invalid). `PreviewStopTarget`: long $150 entry + target=$155 stop=$148→targetPnL=+500 stopPnL=-200 R:R=2.5; non-closing leg→invalid; zero stop risk→invalid
- **`ChartAnalysis.h` — Volume Profile (`[volume-profile]` tag, 12 cases / 72 assertions)** — `ComputeVolumeProfile` single-bar uniform distribution (low=100/high=110/vol=1000 over 10 bins → 100 vol/bin); POC concentration (3 bars [100,101] vol=500 + 1 bar [99,100] vol=100 over 10 bins → bins 5..9=300, bins 0..4=20, POC tied in upper half); out-of-range bars (entirely above `priceHi` excluded; entirely below `priceLo` excluded); partial overlap proportional contribution (bar [98,104] vol=600 over [100,104] 10 bins → 400 total, 40 per bin = 600 × visibleRng/fullRng); empty input / mismatched lengths / degenerate range (priceHi ≤ priceLo) → empty profile / pocIdx = -1; `numBins` clamping ([10, 200]); zero-volume bars skipped; value-area expansion (synthetic profile [10,20,50,20,10] → POC=4, VA=[3,5] right-bias on tied neighbours)
- **`ReplayEngine.h` (`[replay]` tag, 207 cases / 987 assertions)** — `ReplaySessionLabel`/`Short`; `Tick`/`StepBars`/`SeekToBar`; `SnapCursorToNearestBar`; `BarRangeForSession` (4 sessions); `EvaluateBar` all 13 `OrderType`s with BUY+SELL, outsideRth, TIF DAY auto-cancel, Trail stateful tracking; `EvaluateTick` (TRADES→Market/Stop/MIT, BID_ASK→Limit, MIDPOINT→Midprice, two-leg StopLimit/LIT); `ApplyFill` (open/add/close/partial/flip long+short); `Equity`/`UnrealizedPnL`/`Reset`

### tests-ibkr
- **IBKRClient message dispatch** — construct a `TestableIBKRClient`, inject `IBMessage` variants via `Push()`, call `ProcessMessages()`, assert callbacks fire with correct values
- **Null-callback safety** — no crash when callbacks are unset
- **Message ordering** — FIFO preserved across a batch
- **Queue idempotency** — second call to `ProcessMessages()` is a no-op when queue is empty

## What Is NOT Tested

- **UI windows** — Dear ImGui rendering requires a Vulkan/display context; not feasible in headless CI
- **Live IB connection** — real end-to-end tests (connect → subscribe → receive data) require a running IB Gateway with credentials; keep those as manual local tests only

## Sanitizer Coverage

`IBKR_SANITIZE=ON` applies `-fsanitize=address,undefined` **only to `tests-core`**, not to `tests-ibkr` or the main app. This avoids false positives from third-party IB API code.

Run sanitized tests directly (bypasses ctest discovery, which requires both binaries built):
```bash
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ./build/tests/tests-core
```

## Key Architectural Decisions That Enable Testing

- `IBKRUtils.h` — `ParseStatus` and `ParseIBTime` live here as free functions so they can be included without pulling in IB API headers
- `ChartAnalysis.h` — auto-analysis primitives (`FindSwings`, `ATR`, `ClusterLevels`, `KeepTopN`), Phase 12 setup helpers (`RoundToTick`, `AvoidRoundNumber`, `SuggestSetup`, `SuggestStopForPosition`, `PositionSizeShares`), and Phase 13 `SessionVwap` are extracted from `ChartWindow` as inline free functions in `core::services` — no ImGui/ImPlot/IB dependency
- `TradingStyle.h` — Phase 13 `StylePreset`, `GetPreset`, and templated `ApplyPreset(...)` live as POD + free functions in `core::services`. Templated on the settings struct types so tests can instantiate with hand-rolled stubs that have the same field names — no ChartWindow.h dependency
- `IBKRClient::Push()` is **protected** (not private) so `TestableIBKRClient : public IBKRClient` can call it
- `IBKRClient` constructor is safe to call without connecting — `EClientSocket` and `EReaderOSSignal` initialise without a network socket

## CI Jobs

| Job | Trigger | What runs |
|---|---|---|
| `build-linux` | Every push/PR | Full build + `ctest` (both targets) |
| `build-macos` | Every push/PR | Full build + `ctest` (both targets) |
| `build-windows` | Every push/PR | Full build + `ctest -C Release` (both targets) |
| `sanitize-linux` | After `build-linux` passes | Debug build of `tests-core` only, with ASan+UBSan |

FetchContent artifacts (`build/_deps`) are cached in GitHub Actions keyed on `CMakeLists.txt` + `tests/CMakeLists.txt` hashes.

## Adding New Tests

- **Pure logic** (no IB API, no ImGui): add `.cpp` to `tests-core` in `tests/CMakeLists.txt`
- **IBKRClient message dispatch**: add cases to `test_ibkr_queue.cpp` using `client.inject(MsgXxx{...})`
- **New `IBMessage` variant**: add a dispatch test for every new message type — the `std::visit` in `ProcessMessages()` is exhaustive so untested variants will silently do nothing if the callback is null
