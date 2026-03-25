# Testing Rules

## Framework

**Catch2 v3.7.1** fetched via FetchContent when `IBKR_BUILD_TESTS=ON`.
Registered with CTest via `catch_discover_tests()`.

## Test Targets

| Target | Source files | Links | Purpose |
|---|---|---|---|
| `tests-core` | `test_market_data.cpp`, `test_models.cpp`, `test_ibkr_utils.cpp` | Catch2 only | Pure logic — no IB API dependency |
| `tests-ibkr` | `test_ibkr_queue.cpp` + `IBKRClient.cpp` | Catch2 + ibapi-lib | IBKRClient message dispatch (component tests) |

## What Is Tested

### tests-core
- **`MarketData.h`** — `TimeframeLabel`, `TimeframeSeconds`, `TimeframeIBBarSize`, `TimeframeIBDuration` for all 9 timeframes; `IsUSDST` DST boundary cases; `BarSession` session classification at all ET session boundaries (EDT and EST)
- **Model struct defaults** — `Order`, `Fill`, `DepthLevel`, `ScanResult`, `ScanFilter`, `Position`, `AccountValues`, `Bar`
- **Enum string helpers** — `OrderSideStr`, `OrderTypeStr`, `TIFStr`, `OrderStatusStr`, `ScanPresetLabel`, `AssetClassLabel`
- **`IBKRUtils.h`** — `ParseStatus` (all IB status strings, case-sensitivity); `ParseIBTime` (8-digit date → noon UTC, Unix timestamp round-trip, formatted datetime)

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
