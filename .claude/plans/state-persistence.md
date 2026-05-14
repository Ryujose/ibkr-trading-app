# Comprehensive State Persistence — Window Layout + Per-Window Settings + Global Preferences

**Status:** Complete. Tasks 1–9 landed as #78–#85. **Foundational — lands before `ai-integration.md` and `small-caps.md` Task C so those features inherit the unified persistence pattern.**

**Goal:** When the user shuts down the app and starts it again, **everything they configured comes back**: ImGui layout (window positions, sizes, dock arrangement, splitters, table column widths), per-window settings (indicator toggles, auto-analysis params, setup settings, L2/L1 mode, ladder rows, filters, column visibility, sort), and global UI preferences (font size, default trading style, defaults for new instances). No "set it all up again" friction.

This file is the source of truth across sessions — update **Status** and per-task checkboxes as work progresses.

---

## 1. Current state audit (what persists today vs what doesn't)

### Already persisted

| File | Covers | Pattern |
|---|---|---|
| `~/.config/ibkr-trading-app/imgui.ini` | **WRITTEN BUT TO THE WRONG PATH** — ImGui defaults to CWD because `io.IniFilename` is never set | ImGui internal |
| `~/.config/ibkr-trading-app/watchlists.cfg` | Watchlist content, presets, per-instance group | Atomic `.tmp`+`rename`, per-second flush + on-disconnect flush |
| `~/.config/ibkr-trading-app/chart-modes.cfg` | Per-chart: instance idx, symbol, trading style enum, Free-mode TF override | Same |
| `~/.config/ibkr-trading-app/news-providers.cfg` | Disabled news provider codes | Same |
| `~/.config/ibkr-trading-app/replay-windows.cfg` | Per-replay: state for the replay engine | Same |

### Gaps (what disappears every restart)

**ImGui layout** — because `io.IniFilename` isn't pointed at a stable location, dock arrangement, window positions/sizes, splitter ratios (ImGui-managed), and table column widths come back to defaults every launch unless the user happens to launch from the same CWD twice in a row.

**Per-chart settings**:
- `IndicatorSettings` (10 booleans + 7 numeric params: SMA1/2 periods, EMA period, BB period+sigma, RSI period, VP bins)
- `AutoAnalysisSettings` (9 toggles + 7 params)
- `SetupSettings` (1 master + 6 numeric + 7 confluence toggles + 2 advanced params)
- `m_useRTH`, `m_showOvernight`, `m_showLegend`
- Subplot splitter ratios (`m_volumeHeightRatio`, `m_rsiHeightRatio`)
- Drawings (`m_drawings`) — **flagged for deferral**, see §11

**Per-trading-window settings**:
- `m_useL2`, `m_exchangeFilterIdx`, `m_numDepthRows` / `m_ladderRowsIdx`
- `m_topHeightRatio`, `m_bookWidthRatio` (splitters)
- `m_clickToTrade`, `m_expandSpread`
- Default qty in order entry buffers (so the next session starts with the user's typical size, not 100)
- Default order type / TIF / outside-RTH

**Per-scanner settings**:
- Active scan code + instrument + location
- Active filter values
- Column visibility, sort

**Per-news settings**:
- Active tab (Market / Portfolio / Stock)

**Singleton windows**:
- Portfolio: sort, column visibility
- Orders: filter selections, sort
- WshCalendar: symbol filter, date range, type filter, importance filter, sort
- Notifications: per-event mute toggles (already partial? — check NotificationService)

**Global UI prefs**:
- Font size (`g_fontSize`)
- Default trading style for new chart instances
- "Sync with TWS Display Groups" Settings toggle
- AI provider/model preferences (covered by `ai-integration.md` plan)
- Daily risk state (covered by `small-caps.md` Task C)

**Window visibility** (which instances of Chart / Trading / Scanner / News / Watchlist / Replay are *open*):
- `chart-modes.cfg` and `replay-windows.cfg` partially cover this for those window classes
- Trading / Scanner / News / Watchlist don't have it — restored as the defaults from `CreateTradingWindows`

## 2. Approved scope

| Area | Persist? | Notes |
|---|---|---|
| ImGui layout | ✅ | Point `io.IniFilename` at `~/.config/ibkr-trading-app/imgui.ini` |
| Per-chart indicator/auto/setup settings | ✅ | New `chart-settings.cfg` (per-instance) |
| Per-chart `useRTH`, `showOvernight`, `showLegend`, subplot ratios | ✅ | Merged into `chart-settings.cfg` |
| Per-trading-window UI state | ✅ | New `trading-settings.cfg` |
| Per-scanner UI state | ✅ | New `scanner-settings.cfg` |
| Per-news active tab | ✅ | New `news-settings.cfg` |
| Singleton window UI state (Portfolio / Orders / WshCalendar) | ✅ | New `singleton-settings.cfg` |
| Global UI prefs (font, default style, sync-with-TWS) | ✅ | New `app-prefs.cfg` |
| Live data (positions, orders, ticks, account, P&L) | ❌ | Comes from IB on connect |
| User chart drawings | ⏳ deferred | Requires index→timestamp coordinate refactor; see §11 |

**File strategy**: continue the existing "one file per feature" pattern. Six new `.cfg` files in `~/.config/ibkr-trading-app/` plus the existing ones. Each follows the same conventions:

- Atomic `.tmp` + `rename` write
- `INSTANCE:<idx>` blocks for per-instance state (multi-instance windows)
- `key:value` lines for flat fields
- Per-second-debounced flush from `RenderTradingUI()` when dirty
- Synchronous flush from `DestroyTradingWindows()` (disconnect / app exit)
- Loaded in `FinishConnect(false)` after `CreateTradingWindows()`
- Tolerant parsing: unknown keys ignored, malformed lines skipped, out-of-range values clamped

The repeated pattern is cheap to maintain because every file shares the same parser shape — six near-identical Save/Load function pairs.

## 3. Architecture

### Layer 1 — ImGui ini path (foundation)

In `main()`, immediately after `ImGui::CreateContext()`:

```cpp
// Build the path: ~/.config/ibkr-trading-app/imgui.ini
static std::string g_imguiIniPath = EnsureConfigDir() + "/imgui.ini";
ImGuiIO& io = ImGui::GetIO();
io.IniFilename = g_imguiIniPath.c_str();   // pointer must outlive ImGui (g_imguiIniPath is static)
```

`EnsureConfigDir()` is a new helper that creates `~/.config/ibkr-trading-app/` if missing and returns the path. Today's per-feature `EnsureWatchlistConfigDir` (and similar) calls collapse to call this helper.

That single change restores: window positions, sizes, dock arrangement, splitter regions ImGui owns (the inter-window splitters inside floating panels), and table column widths for tables flagged `ImGuiTableFlags_Sortable | ImGuiTableFlags_Reorderable`.

ImGui handles the file format itself — we don't parse or write it manually.

### Layer 2 — Shared `state-io.h` helpers

New header `src/core/services/state-io.h` (inline free functions, header-only — usable from tests-core):

```cpp
namespace core::services {

// Returns "~/.config/ibkr-trading-app", creating it if missing. Logs once on
// failure (e.g. read-only home dir); returns empty string if unusable.
std::string EnsureConfigDir();

// Atomic write: writes to <path>.tmp then renames. Returns true on success.
bool AtomicWriteText(const std::string& path, const std::string& contents);

// Read entire file as one std::string. Returns empty + sets *exists=false if
// the file doesn't exist (not an error — first launch).
std::string ReadTextFile(const std::string& path, bool* exists = nullptr);

// Minimal line-based key:value parser. Each line is either:
//   - "KEY:value"   (where KEY is uppercase ASCII; value is the rest of the line)
//   - blank or "#..."  (skipped)
// Multiple "INSTANCE:<idx>" lines split the input into per-instance blocks.
struct StateBlock {
    int instance = -1;
    std::unordered_map<std::string, std::string> fields;
};
std::vector<StateBlock> ParseStateBlocks(std::string_view contents);

// Reverse builder. Adds "INSTANCE:<idx>" line then each k:v.
std::string FormatStateBlocks(const std::vector<StateBlock>&);

// Field accessors with type coercion + range clamping. Missing key returns dflt.
bool        GetBool  (const StateBlock&, const std::string& key, bool        dflt);
int         GetInt   (const StateBlock&, const std::string& key, int         dflt, int lo = INT_MIN, int hi = INT_MAX);
double      GetDouble(const StateBlock&, const std::string& key, double      dflt, double lo = -INFINITY, double hi = INFINITY);
std::string GetString(const StateBlock&, const std::string& key, const std::string& dflt);

}
```

Every Save/Load function pair uses these. Cuts each new persistence file to ~50-80 lines from ~200 if it used hand-rolled parsing.

Unit-tested in `tests-core` (no IB / ImGui deps).

### Layer 3 — Per-feature Save/Load functions

In `main.cpp`, alongside the existing `SaveWatchlistsFile()` etc., add:

```cpp
// chart-settings.cfg
static void SaveChartSettingsFile();
static void LoadChartSettingsFromFile();

// trading-settings.cfg
static void SaveTradingSettingsFile();
static void LoadTradingSettingsFromFile();

// scanner-settings.cfg
static void SaveScannerSettingsFile();
static void LoadScannerSettingsFromFile();

// news-settings.cfg
static void SaveNewsSettingsFile();
static void LoadNewsSettingsFromFile();

// singleton-settings.cfg  (Portfolio / Orders / WshCalendar)
static void SaveSingletonSettingsFile();
static void LoadSingletonSettingsFromFile();

// app-prefs.cfg  (font, default-style, sync-with-TWS, etc.)
static void SaveAppPrefsFile();
static void LoadAppPrefsFromFile();
```

Each function follows the existing pattern (e.g. `SaveChartModesFile` at `main.cpp:1457+`). Per-feature dirty flag + per-second flush from `RenderTradingUI` + sync flush from `DestroyTradingWindows`.

### Layer 4 — Window classes expose Save/Load methods

To avoid main.cpp peeking deep into each window's private members, each window class gains:

```cpp
// On the window class (ChartWindow, TradingWindow, ...):
void          SerializeSettings(core::services::StateBlock&) const;   // fill k:v
void          ApplySettings    (const core::services::StateBlock&);   // mutate from k:v
```

main.cpp's Save/Load functions just iterate `g_chartEntries` and call `SerializeSettings` / `ApplySettings` per instance. Keeps the persistence file layout decisions in main.cpp (one place) but the field knowledge in the window class (where it belongs).

This pattern also drops the need for new public getters/setters for every persisted field — the window owns its serialisation.

### Layer 5 — Marking state dirty

Each window class gains a `bool m_settingsDirty = false;` flag, flipped on any user-driven settings change (toggle, slider, splitter drag, column resize). main.cpp's per-second flush walks instances, checks dirty flags, calls Save when any are dirty, clears flags.

For window-level state that lives in main.cpp (group assignments, futures health subscriptions visibility, etc.), the existing `g_chartModesDirty` pattern extends to per-file flags: `g_chartSettingsDirty`, `g_tradingSettingsDirty`, etc.

## 4. File layouts

All files use the same conventions as `chart-modes.cfg`. Example skeletons:

### `chart-settings.cfg`
```
INSTANCE:0
USE_RTH:0
SHOW_OVERNIGHT:1
SHOW_LEGEND:1
VOL_RATIO:0.20
RSI_RATIO:0.18
# IndicatorSettings
IND_SMA20:1
IND_SMA50:1
IND_EMA20:0
IND_BBANDS:1
IND_VWAP:1
IND_VWAP_BANDS:0
IND_VOLUME:1
IND_RSI:1
IND_VOLUME_PROFILE:0
SMA1_PERIOD:20
SMA2_PERIOD:50
EMA_PERIOD:20
BB_PERIOD:20
BB_SIGMA:2.0
RSI_PERIOD:14
VP_BINS:50
# AutoAnalysisSettings
AUTO_SUPPORTS:1
AUTO_RESISTANCES:1
AUTO_TREND:1
AUTO_DONCHIAN:0
AUTO_KELTNER:0
AUTO_FIB:0
AUTO_PIVOTS:0
AUTO_BREAKOUTS:0
AUTO_ZONES:0
AUTO_SWING_K:3
AUTO_TREND_LB:50
AUTO_DONCHIAN_LEN:20
AUTO_MAX_LEVELS:3
AUTO_MIN_TOUCHES:2
AUTO_SCAN_CAP:1000
AUTO_TREND_CHANNEL:0
# SetupSettings
SETUP_OVERLAY:0
SETUP_RR_MIN:2.0
SETUP_ATR_PAD:0.5
SETUP_ROUND_PAD:0.07
SETUP_STOP_OFFSET:0.10
SETUP_RISK_PCT:1.0
SETUP_USE_STOP_LMT:1
SETUP_TREND_ALIGN:0
SETUP_VWAP_CONTEXT:0
SETUP_MARKET_HEALTH:0
SETUP_RSI_FILTER:0
SETUP_VOLUME_CONFLUENCE:0
SETUP_MULTI_TARGET:0
SETUP_MH_MAX_PCT:0.5
SETUP_T2_SPLIT_PCT:50.0

INSTANCE:1
...
```

### `trading-settings.cfg`
```
INSTANCE:0
USE_L2:0
EXCH_FILTER:All
NUM_DEPTH_ROWS:25
TOP_HEIGHT_RATIO:0.65
BOOK_WIDTH_RATIO:0.54
CLICK_TO_TRADE:0
EXPAND_SPREAD:0
DEFAULT_QTY:100
DEFAULT_ORDER_TYPE:1
DEFAULT_TIF:0
DEFAULT_OUTSIDE_RTH:0
```

### `scanner-settings.cfg`
```
INSTANCE:0
SCAN_CODE:TOP_PERC_GAIN
INSTRUMENT:STK
LOCATION:STK.US.MAJOR
FILTER_PRICE_MIN:1.0
FILTER_PRICE_MAX:100.0
FILTER_VOL_MIN:500000
COL_SYMBOL:1
COL_PRICE:1
COL_CHANGE:1
...
```

### `news-settings.cfg`
```
INSTANCE:0
ACTIVE_TAB:1     # 0=Market, 1=Portfolio, 2=Stock
```

### `singleton-settings.cfg`
```
WINDOW:portfolio
SORT_COL:1
SORT_ASC:1
COL_VISIBLE_MASK:0xFFFF

WINDOW:orders
ACTIVE_TAB:0
FILTER_SYMBOL:
FILTER_SIDE:0
SORT_COL:5
SORT_ASC:0

WINDOW:wsh
FILTER_SYMBOL:
DATE_FROM:0
DATE_TO:0
TYPE_FILTER:0
IMPORTANCE_FILTER:0
SORT_COL:0
SORT_ASC:1
```

### `app-prefs.cfg`
```
FONT_SIZE:1                 # 0=Small, 1=Medium, 2=Large
DEFAULT_TRADING_STYLE:2     # int enum value (Scalping=0..Free=4)
SYNC_TWS_DISPLAY_GROUPS:0
```

## 5. Load / Save lifecycle

**Load** — from `FinishConnect(bool isReconnect)`:
```cpp
if (!isReconnect) {
    DestroyTradingWindows();
    CreateTradingWindows();          // creates default instances

    LoadAppPrefsFromFile();          // global UI prefs first (font scale)
    LoadWatchlistsFromFile();        // existing
    LoadChartModesFromFile();        // existing — chart symbol + style
    LoadChartSettingsFromFile();     // NEW — chart indicator/auto/setup settings
    LoadTradingSettingsFromFile();   // NEW
    LoadScannerSettingsFromFile();   // NEW
    LoadNewsSettingsFromFile();      // NEW
    LoadSingletonSettingsFromFile(); // NEW
    LoadReplayWindowsFromFile();     // existing
    ...
}
```

Order matters: `LoadAppPrefsFromFile()` runs first because `FONT_SIZE` affects `io.FontGlobalScale` and `ScaleAllSizes()`, and any subsequent window-creation paths render at the correct scale. `LoadChartModesFromFile()` runs before `LoadChartSettingsFromFile()` because the chart symbol determines which `ApplyPreset(style)` baseline applies — settings then override the preset values for any field the user customised.

**Save** — three triggers:
1. **Per-second debounce** in `RenderTradingUI()`: each dirty flag drives one call. Saves only what changed since the last flush.
2. **Sync flush** in `DestroyTradingWindows()`: guarantees a save on disconnect / app exit.
3. **On-Settings-panel-close**: app-prefs only — saves immediately when the user closes the Settings panel so a crash mid-session doesn't lose font size etc.

**Mark dirty** — every user-driven change site flips the matching dirty flag:
- Chart toolbar checkboxes, settings popup inputs, splitter drags → `m_settingsDirty = true` on the window; main.cpp's `RenderTradingUI` aggregates and sets `g_chartSettingsDirty`.
- TradingWindow L1/L2 toggle, level count combo, splitter drags, click-to-trade toggle → same pattern.
- Settings panel font-size change → `g_appPrefsDirty = true`.

## 6. Plumbing dependencies

| Need | Source |
|---|---|
| `EnsureConfigDir()` | Extract from existing `EnsureWatchlistConfigDir` (already on disk pattern) |
| Atomic `.tmp` + `rename` | Existing pattern in `SaveWatchlistsFile` / `SaveChartModesFile` |
| Per-second debounce flag flush | Existing pattern in `RenderTradingUI` (line ~4443) |
| Sync flush on shutdown | Existing pattern in `DestroyTradingWindows` |
| Dirty-flag pattern | `g_chartModesDirty` (extend to one flag per new file) |

Zero new dependencies. Pure refactor + file format expansion.

## 7. Tasks

- [ ] **Task 1** — `state-io.h` shared helpers + tests. `EnsureConfigDir`, `AtomicWriteText`, `ReadTextFile`, `ParseStateBlocks`, `FormatStateBlocks`, typed `Get*` accessors with clamping. Catch2 `[state-io]` tag (~12 cases: parse blank/comment/malformed lines, multi-block, missing-key default, out-of-range clamping). ~120 lines impl + ~80 lines tests.

- [ ] **Task 2** — ImGui `IniFilename` wired. One-line fix in `main()` after `CreateContext`. Verify across launches: open windows, drag dock arrangement, restart → arrangement comes back. ~5 lines.

- [ ] **Task 3** — `app-prefs.cfg`. Font size + default trading style + sync-with-TWS toggle. Load before `LoadAppPrefsFromFile` runs first in `FinishConnect`. Settings panel save-on-close. ~80 lines.

- [ ] **Task 4** — `ChartWindow::SerializeSettings` / `ApplySettings` + main.cpp `chart-settings.cfg` Save/Load. Round-trips all 47 `IndicatorSettings` + `AutoAnalysisSettings` + `SetupSettings` fields plus `useRTH`/`showOvernight`/`showLegend`/subplot-ratios. Dirty flag wired on every toolbar + popup mutation. ~180 lines.

- [ ] **Task 5** — `TradingWindow::SerializeSettings` / `ApplySettings` + `trading-settings.cfg`. L2 mode, exchange filter (saved as string `"All"` or exchange name — survives across smart-component list changes), num depth rows (drives `setNumDepthRows` on apply → triggers re-subscribe on connect), splitter ratios, click-to-trade, expand-spread, default qty / order type / TIF / outside-RTH. ~140 lines.

- [ ] **Task 6** — `ScannerWindow` + `scanner-settings.cfg`. Active scan code, instrument, location, filter values, column visibility, sort. ~120 lines.

- [x] **Task 7** — `NewsWindow` — **Skipped (no-op).** ImGui's `TabItem` state persistence (via the now-stable `imgui.ini` path wired in Task 2) already persists the active tab per window instance. `news-settings.cfg` is not needed.

- [x] **Task 8** — Singleton windows + `singleton-settings.cfg`. Portfolio sort/columns, Orders filter/sort, WshCalendar filters/sort. ~200 lines (Task #84).

- [x] **Task 9** — Dirty-flag flush wiring. **Done via hash-diff instead.** Each `Save*File()` computes `std::hash<std::string>{}(text)` and skips the write when the hash matches the last-written value — no per-mutation-site dirty flags needed. Per-second `RenderTradingUI` flushes for chart/trading/scanner/singleton/replay + dirty-gated chart-modes/watchlists; sync flush in `DestroyTradingWindows` for all seven files; settings-panel changes (app-prefs) save immediately.

After Tasks 1–9 land, restarting the app produces a session that looks bit-for-bit identical to the prior shutdown except for live IB data — which IB replays on connect anyway via `reqAllOpenOrders` + position fan-out.

## 8. Edge cases & gotchas

- **First launch (no files exist)**: all `Load*` functions early-return on `bool exists=false` from `ReadTextFile`. Window classes keep their construction-default settings. No errors, no warnings.

- **Schema migration when fields are added**: tolerant parsing means new fields silently default if absent in an old file. **Removing** fields is the bigger risk — existing files will carry orphan keys that the parser ignores. Add a `# schema:v2` header line to each file and bump it when changes are breaking; old `v1` files get re-saved as `v2` on first save.

- **Partial corruption**: if a file is corrupted (kill -9 mid-write), the atomic rename pattern means we either have the old version (rename never happened) or the new one (rename completed). Never a half-written file. If somehow we do load garbage, tolerant parsing skips malformed lines and clamps out-of-range values; the worst case is a single window reverts to defaults.

- **Settings panel UX**: a "Reset to defaults" button per window class would be useful so users can recover from a bad customisation without manually editing config files. Defer to v2.

- **TWS Display Group sync conflicts**: if `app-prefs.cfg` says `SYNC_TWS_DISPLAY_GROUPS:1` and the user wasn't connected to IB when the app exited, the toggle restores on the next connect but the IB subscriptions need to fire too. Already handled by the existing TWS-sync wiring — it subscribes on `FinishConnect` based on the toggle's current value, so restoring the toggle from `app-prefs.cfg` before `FinishConnect` runs is sufficient.

- **TradingWindow exchange filter**: `m_exchangeFilterIdx` is an index into `m_exchangeList`, which is populated dynamically from smart-component data per symbol. Persist by name (`"NASDAQ"`) not by index — index would point at the wrong exchange on the next session. On Apply, find the saved name in the rebuilt list (falling back to `"All"` if missing).

- **ScannerWindow column visibility**: column visibility is stored as a bitmask in the file. Bit position corresponds to `kColDefs[]` index. Reordering `kColDefs[]` between versions would shift bits — bump the schema version and migrate.

- **ImGui ini file growth**: ImGui appends per-window state forever, never pruning closed windows. The file will grow over many sessions but stays under ~50KB even with hundreds of distinct window names. Not a concern.

- **Concurrent app instances**: two app processes writing the same config files race. The atomic rename means one wins cleanly, but the loser silently overwrites. v1: document the limitation ("don't run two instances"). v2: file lock via `flock` on Linux/macOS / `LockFileEx` on Windows.

- **Wide-character paths on Windows**: `getenv("HOME")` doesn't work on Windows; need `getenv("USERPROFILE")` or `SHGetKnownFolderPath`. `EnsureConfigDir` handles platform branching with `#ifdef _WIN32`.

- **Disk full / read-only mount**: `AtomicWriteText` returns `false` on failure. Save functions log to stderr once per session (not per failed attempt — would spam) and silently continue. The in-memory state is unaffected; user's session works normally, just doesn't persist.

- **Group sync interaction with restore**: chart symbol restore from `chart-modes.cfg` fires before group sync wiring is fully active (intentional — see Task #64 comment). New settings restore should follow the same pattern: apply settings *before* any IB subscriptions fire so the user's `useRTH` choice etc. applies to the initial market-data request.

## 9. Acceptance per task

**Task 1**: Catch2 `[state-io]` 12 cases / ~40 assertions pass. Round-trip a state block through `Format` → `Parse` and assert all fields recovered.

**Task 2**: Open app, drag a Chart window out into a separate viewport, resize, dock back into a different region. Close app. Reopen → arrangement is identical. Confirm `~/.config/ibkr-trading-app/imgui.ini` exists and is non-empty.

**Task 3**: Set font size to Large, restart app → still Large. Change default trading style in Settings to Investment, restart → next new chart spawns in Investment.

**Task 4**: On Chart 0, toggle off SMA20 + SMA50, toggle on Donchian + Keltner, change BB sigma to 2.5, enable Setup overlay with rrMin=2.5. Close app. Reopen → every toggle and value is restored on Chart 0. Chart 1 (untouched) keeps defaults.

**Task 5**: On TradingWindow 0, switch to L2, filter to NASDAQ, set Levels to 100, drag splitters to 50/50. Close + reopen → all settings restored, depth subscription re-fires with `numRows=100`.

**Task 6**: Configure a scanner with `TOP_PERC_GAIN` + a custom min volume filter. Reopen → scanner restores with same filters.

**Task 7**: NewsWindow 0 on Portfolio tab. Reopen → starts on Portfolio tab.

**Task 8**: Portfolio sorted by `Unreal P&L` descending. Orders filtered to side=SELL. WshCalendar filtered to Earnings + High importance. Reopen → all preserved.

**Task 9**: Make changes across 5 different windows in one session. Verify per-second flushes happen (touch `~/.config/ibkr-trading-app/` mtime timestamps). Kill -9 the app — verify atomic-write guarantee held (no `.tmp` files left, all `.cfg` files valid).

End-to-end: open the app, configure 10 chart windows with different styles + indicator setups, 5 trading windows in different L2 / row-count configs, 2 watchlists with custom presets, change font size, drag dock arrangement to a custom layout. Close. Reopen → entire workspace looks identical except live IB data, which fills back in within ~2-3s of connect.

## 10. Settings panel additions

A new **Workspace** section in the Settings panel:

```
┌─ Workspace ────────────────────────────────────────────┐
│  Config dir: ~/.config/ibkr-trading-app/   [Open]      │
│                                                        │
│  [Reset all settings to defaults]                      │
│  [Reset chart settings only]                           │
│  [Reset trading settings only]                         │
│  [Export workspace to file...]                         │
│  [Import workspace from file...]                       │
└────────────────────────────────────────────────────────┘
```

- "Open" launches the file manager pointed at the config dir (`xdg-open` on Linux, `open` on macOS, `explorer` on Windows).
- "Reset all" prompts for confirmation, then deletes every `.cfg` file (keeps `imgui.ini`) and quits — next start is fresh.
- Per-category reset: deletes just that one `.cfg`.
- Import/Export: tars the whole config dir for cross-machine sync. v2 nice-to-have, not v1.

The reset buttons are escape hatches — important because users *will* paint themselves into corners with custom settings, and "wipe and restart" beats "manually edit a binary-looking file."

## 11. Out of scope (deferred)

- **User chart drawings persistence**: `m_drawings` are stored as index-based coords (`x1`, `x2` are integer bar indices into `m_xs`). After a `RebuildFlatArrays` or session-filter toggle, indices shift — drawings would land on different bars. The clean fix is converting drawings to **timestamp+price coordinates** (resilient to filter changes), then resolving back to indices on render. That's a meaningful refactor (touches `m_drawings`, `DrawUserDrawings`, every click-handler that creates a drawing) and is worth its own task. Flagged here, deferred to a follow-up. **v1 keeps the existing "drawings clear on RebuildFlatArrays" behaviour.**

- **Workspace export/import bundle**: tar the config dir for cross-machine sync. Useful for "moving to a new laptop." Defer.

- **Per-account workspace isolation**: today everything is global. If a user has paper + live accounts and wants different chart setups per account, the config files would need to be keyed by account. Defer — single-workspace is fine for v1.

- **Versioned restore points**: "undo last 10 settings changes." Defer.

- **Cloud sync**: out of scope.

- **Cross-platform path normalisation** beyond `_WIN32` branching: assume POSIX-like `getenv("HOME")` works on Linux + macOS. No XDG_CONFIG_HOME respect (would override `~/.config`). Defer.

- **Reactive UI on file changes**: if the user manually edits a `.cfg` file while the app is running, changes aren't picked up. Defer (would need inotify / FSEvents / ReadDirectoryChangesW).

## 12. Open questions

1. **Should the `imgui.ini` file path be `~/.config/ibkr-trading-app/imgui.ini` (alongside the .cfg files) or `~/.config/ibkr-trading-app/layout/imgui.ini` (in a subdirectory)?** Flat is simpler; subdirectory is cleaner if we later add multiple layout presets. I'd go flat for v1, easy to migrate.

2. **Should we ship a default `app-prefs.cfg` with the install, or rely entirely on construction defaults?** Construction defaults are simpler (no shipping data, no file-existence check needed). Default file gives us a place to document the format. I'd stick with construction defaults.

3. **Migration strategy for the existing `chart-modes.cfg`**: should `chart-settings.cfg` *merge* into `chart-modes.cfg` (extend the existing file) or stay separate? Separate is safer (existing parser doesn't see new keys it doesn't understand; one file per "concern" keeps the format mental model clean). I'd keep them separate.

4. **What about `widget-state` (e.g. the active tab in NewsWindow that already partially persists)?** Some of this state may already be in `imgui.ini` because ImGui persists `TabItem` state by default. Confirm during Task 7 — if so, the `news-settings.cfg` becomes a no-op file and we skip it.

5. **Per-window vs global font scale**: today font is global. If a user wants a smaller font on a packed dock side and larger on a chart, that's per-window. Almost certainly not worth the complexity. Confirm global-only.
