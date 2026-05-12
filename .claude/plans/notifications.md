# Notifications — Per-Event Tones + Voice + In-App Toasts

Add a notification layer to the trading terminal. One service, three surfaces:

1. **In-app toast overlay** — top-right ImGui stack, severity-coloured, auto-fade, click-to-dismiss.
2. **Per-event alert tone** — a distinct short WAV per event (fills, cancels, disconnect, reconnect, etc.). Pre-generated procedurally.
3. **Per-event voice line** — a short spoken phrase per event ("Order filled", "Connection lost", etc.). Pre-generated via `piper` (neural TTS) and committed to the repo.

A single thread-safe `core::services::NotificationService` queues notifications, plays sound through one `ma_engine` (miniaudio), and exposes settings (master mute / volume / per-category enable / playback style). UI thread drains the queue every frame.

Triggers wired via one-line `Notify(...)` calls from existing `main.cpp` callback handlers. Edge-triggered for state-based signals (breakout, unguarded-position) so a held condition doesn't spam.

## Motivation

The terminal already surfaces critical state via on-chart strips and menu-bar badges, but those require eyes-on. A trader monitoring multiple charts or away from the screen needs:

- An audible cue per event so they know what just happened without watching the order book.
- A spoken phrase so they don't have to disambiguate beeps under stress.
- A visible toast for connection drops, IB rejections, and edge-triggered setup signals — louder than the existing strip without being modal.
- A single mute toggle when the user steps away from the desk.

Out of scope for v1: OS-native notifications, notification history pane, per-symbol price alerts, threshold alerts on indicators, custom user-recorded voice packs at runtime, snooze.

## Architecture

```
IB callback (onFill, onError, onConnectionChanged, etc.)
        │
        ▼
NotificationService::Notify(severity, category, event, title, body)
        │
        ├──► std::deque<Notification>  (mutex-protected, capped at kMaxQueued=64)
        │
        └──► PlayForEvent(event)
              ├─ tone path  → ma_engine_play_sound (immediate)
              └─ voice path → enqueued in m_pendingPlays  (delay = kVoiceDelayMs = 150 ms)

Service tick (called once per frame from UI thread):
  drains m_pendingPlays whose due-time has passed → ma_engine_play_sound

Main loop, once per frame:
  NotificationOverlay::Render(service)
    ├─ service.Drain() → append to local visible-toast list
    ├─ fade + dismiss expired
    └─ draw stacked toasts via ImGui::GetForegroundDrawList()
```

The service owns `ma_engine` and the cached `ma_sound` pool. The overlay owns the visible-toast list. UI never touches miniaudio.

## File layout

| Path | Purpose |
|---|---|
| `third_party/miniaudio/miniaudio.h` | Vendored single-header. Pinned at v0.11.x. Public domain. |
| `third_party/miniaudio/CMakeLists.txt` | `INTERFACE` target exposing the header path. |
| `src/core/services/NotificationService.h` | API + `Notification` POD + `NotificationEvent` enum + `NotificationSettings` POD. |
| `src/core/services/NotificationService.cpp` | Queue, settings load/save, miniaudio init/teardown, `Notify`/`PlayForEvent`/`Tick` impl. |
| `src/ui/NotificationOverlay.h` | `RenderNotificationOverlay()` free function + per-toast state struct. |
| `src/ui/NotificationOverlay.cpp` | Drain queue, fade/dismiss, draw stacked toasts via `ImGui::GetForegroundDrawList()`. |
| `tools/gen_tones.cpp` | One-shot WAV tone generator (sine + ADSR envelope). Built when `IBKR_BUILD_TOOLS=ON`. |
| `tools/gen_voice.sh` | One-shot shell script that calls `piper` for each phrase, writes voice WAVs. |
| `tools/voice_phrases.txt` | One phrase per line, parallel to event enum. Single source of truth for both `gen_voice.sh` and the test plan. |
| `tools/CMakeLists.txt` | `IBKR_BUILD_TOOLS` gated. Builds `gen_tones`. `gen_voice.sh` is shipped as-is. |
| `tools/README.md` | How to regenerate tones + voice (incl. piper install + model download URL + `--force` semantics). |
| `assets/sounds/tones/<event>.wav` | 11 short tones (see "Event catalogue"). 22.05 kHz 16-bit mono PCM, ≤ 500 ms each. ~5–15 KB each. |
| `assets/sounds/voice/<event>.wav` | 11 short voice phrases. 22.05 kHz 16-bit mono PCM, ≤ 1.2 s each. ~30–60 KB each. |
| `tests/test_notification_service.cpp` | Pure-logic tests against the queue + settings + scheduler; audio mocked via `PlayHook`. |

Repo-size impact: ~22 WAVs × ~40 KB avg = ~900 KB. Acceptable.

## Event catalogue

Single source of truth for the tone generator, the voice generator, and the trigger wiring. Filenames are `<event>` lowercased.

| Event enum | Severity | Category | Tone (gen_tones) | Voice phrase |
|---|---|---|---|---|
| `OrderFilled` | Success | Orders | 880 Hz, 220 ms, soft attack | "Order filled" |
| `OrderPartialFill` | Success | Orders | Same as filled at 60% volume | "Partial fill" |
| `OrderRejected` | Error | Orders | 440→220 Hz sweep, 380 ms + 100 ms tail | "Order rejected" |
| `OrderCancelled` | Warning | Orders | 660 Hz / 440 Hz double-tone, 280 ms | "Order cancelled" |
| `IbError` | Error | Orders | Same sweep as rejected at 80% volume | "Order error" |
| `ConnectionLost` | Warning | Connection | 330 Hz warble, 400 ms | "Connection lost" |
| `ConnectionRestored` | Success | Connection | C5–E5–G5 arpeggio, 320 ms | "Connection restored" |
| `LongSetup` | Warning | Signals | 1320 Hz exp-decay 280 ms | "Long setup detected" |
| `ShortSetup` | Warning | Signals | 660 Hz exp-decay 280 ms | "Short setup detected" |
| `UnguardedPosition` | Warning | Signals | 880 Hz / 660 Hz two-tone, 320 ms | "Position unprotected" |
| `Test` | Info | (none) | 1100 Hz, 200 ms | "Test" |

`Test` is fired only by the Settings panel "Test" buttons.

## NotificationService API

```cpp
namespace core::services {

enum class NotificationSeverity : int {
    Success = 0,   // green border + chime
    Info    = 1,   // cyan border
    Warning = 2,   // amber border
    Error   = 3,   // red border
};

enum class NotificationCategory : int {
    Orders     = 0,
    Connection = 1,
    Signals    = 2,
    System     = 3,   // 'Test' lives here; not user-toggleable
};

enum class NotificationEvent : int {
    OrderFilled = 0,
    OrderPartialFill,
    OrderRejected,
    OrderCancelled,
    IbError,
    ConnectionLost,
    ConnectionRestored,
    LongSetup,
    ShortSetup,
    UnguardedPosition,
    Test,
    _Count
};

struct Notification {
    NotificationSeverity severity = NotificationSeverity::Info;
    NotificationCategory category = NotificationCategory::Orders;
    NotificationEvent    event    = NotificationEvent::Test;
    std::string          title;          // ≤ 48 chars
    std::string          body;           // ≤ 200 chars
    std::time_t          ts = 0;
    int                  id = 0;         // monotonic; ImGui id key
};

// Surfaces and categories are independent. Combining them gives the user the
// same expressiveness as a discrete PlaybackStyle enum but with finer control:
//   "tones only"   = enableTones=1, enableVoice=0
//   "voice only"   = enableTones=0, enableVoice=1
//   "silent toast" = enableTones=0, enableVoice=0, enableToasts=1
//   "all off"      = masterEnable=0 (or all three sub-flags=0)
struct NotificationSettings {
    bool masterEnable     = true;    // master kill switch — false suppresses everything
    int  masterVolume     = 75;      // 0–100 → ma_engine_set_volume; applies to tones AND voice
    bool enableTones      = true;    // alert tone WAVs
    bool enableVoice      = true;    // voice phrase WAVs
    bool enableToasts     = true;    // in-app visual toast overlay
    bool enableOrders     = true;    // category: fills, rejections, cancels, IB errors
    bool enableConnection = true;    // category: disconnect, reconnect
    bool enableSignals    = true;    // category: breakout setup, unguarded-position
};

class NotificationService {
public:
    NotificationService();    // ma_engine_init, loads settings, preloads 22 WAVs
    ~NotificationService();   // ma_engine_uninit; unloads sounds

    // Threadsafe — may be called from any callback / IB reader thread.
    void Notify(NotificationSeverity, NotificationCategory, NotificationEvent,
                std::string title, std::string body);

    // UI thread — drains visible toasts (returns up to `max`, removes from queue).
    std::vector<Notification> Drain(int max = 16);

    // UI thread — must be called once per frame to dispatch delayed voice plays.
    void Tick();

    // Settings — read/write atomic via mutex; persisted on every mutator.
    NotificationSettings settings() const;
    void                 setSettings(const NotificationSettings&);

    // Test hook — replaces real audio with a callback so tests-core can assert
    // exactly what would have played without linking miniaudio.
    struct PlayedSound { NotificationEvent event; bool isVoice; };
    using PlayHook = std::function<void(PlayedSound)>;
    void SetPlayHook(PlayHook);

    // Asset path resolution — public for the gen tools and tests.
    static std::string ToneAssetPath (NotificationEvent);
    static std::string VoiceAssetPath(NotificationEvent);

    // Settings persistence.
    static std::string SettingsFilePath();
    static bool        SaveSettingsTo (const std::string& path, const NotificationSettings&);
    static bool        LoadSettingsFrom(const std::string& path, NotificationSettings& out);

private:
    struct PendingPlay { double dueAt; NotificationEvent event; bool isVoice; };

    void PlayForEvent(NotificationEvent);    // resolves PlaybackStyle + category enable
    void PlayNow     (NotificationEvent, bool isVoice);  // honour PlayHook if set

    mutable std::mutex        m_mutex;
    std::deque<Notification>  m_queue;
    std::deque<PendingPlay>   m_pendingPlays;
    NotificationSettings      m_settings;
    int                       m_nextId = 1;
    PlayHook                  m_playHook;
    // miniaudio internals (ma_engine, ma_sound[Event::_Count][2]) — opaque ptr
    // so the header doesn't include miniaudio.h.
    struct AudioBackend;
    std::unique_ptr<AudioBackend> m_audio;
};

} // namespace core::services
```

### Threading

- `Notify(...)` may be called from the IB EReader thread. `m_mutex` protects everything.
- `ma_engine_play_sound(...)` is documented thread-safe but we still serialise via the mutex to make queue + audio dispatch atomic relative to settings reads.
- `Tick()`, `Drain()`, and overlay rendering only run on the UI thread.

## NotificationOverlay

Anchored top-right of the **main viewport** (not the focused viewport — toasts shouldn't follow a floating chart). Position: `vp->Pos.x + vp->Size.x - kRightMargin - kToastWidth`, `vp->Pos.y + kTitleBarH + kTopMargin`. Stack downward.

Constants:
- `kMaxVisible = 3` — older toasts stay queued and slide in as visible ones expire.
- `kFadeInMs = 150`
- `kHoldMs = 5500` (Success/Info), `8000` (Warning), `12000` (Error). Hover pauses the fade.
- `kFadeOutMs = 350`
- `kToastWidth = em(280)`, `kToastHeight = em(56)` (single-line) or auto-grow to em(76) for two-line body.
- `kSpacing = em(8)`

Visual: left vertical accent bar in severity colour (green / cyan / amber / red), title in regular weight + 1px shadow, body at 88% alpha. Click the toast = dismiss. Small `×` top-right does the same.

Drawn via `ImGui::GetForegroundDrawList()` so toasts sit above modal popups and floating windows. Rendered after `RenderTradingUI` and before `RenderCustomTitleBar` in the main UI dispatch.

```cpp
struct VisibleToast {
    Notification n;
    double       firstShownAt;
    double       hideAt;       // pushed forward while hovered
    bool         dismissed = false;
};

void RenderNotificationOverlay(NotificationService&);
```

`s_visible` is a static inside the function. Each frame: drain new notifications until `s_visible.size() == kMaxVisible`, recompute alpha + hideAt, draw, prune dismissed/faded.

## Settings panel extension

Append a `SeparatorText("Notifications")` section to `RenderSettingsWindow()`. Sub-controls are visually grouped and become disabled (greyed) when their parent is off:

- `[x] Enable notifications` (master kill switch)
  - `Volume  [====|======] 75` (slider; disabled when master off)
  - `Surfaces:`
    - `[x] Alert tones`
    - `[x] Voice phrases`
    - `[x] Visual toasts`
  - `Categories:`
    - `[x] Order events`
    - `[x] Connection events`
    - `[x] Signal events`
- `[ Test tone ]` `[ Test voice ]` `[ Test toast ]`

Each change saves immediately to `~/.config/ibkr-trading-app/notifications.cfg` (atomic `.tmp` + rename). Format:

```
masterEnable=1
masterVolume=75
enableTones=1
enableVoice=1
enableToasts=1
enableOrders=1
enableConnection=1
enableSignals=1
```

The three Test buttons each force exactly one surface so the user can sanity-check each independently:
- `Test tone`  → bypasses the `enableVoice` + `enableToasts` flags, plays the tone WAV for `Test` event regardless. Still respects `masterEnable` and `masterVolume`.
- `Test voice` → bypasses `enableTones` + `enableToasts`, plays the voice WAV.
- `Test toast` → bypasses both audio surfaces, queues a Test toast.

This is implemented as an internal `NotifyForce(...)` API on the service that takes explicit `(playTone, playVoice, showToast)` booleans; the public `Notify(...)` derives those booleans from the current settings.

## Sound generators

### `tools/gen_tones.cpp` — alert tones

Standalone executable, hand-rolled WAV writer (~50 LoC: RIFF header + fmt chunk + data chunk). 22.05 kHz 16-bit mono PCM. No miniaudio dep. Per-event timbres per the catalogue table above.

CLI: `gen_tones [--force] <output_dir>` → writes 11 WAVs. Refuses to overwrite existing files unless `--force` is passed.

ADSR envelope shared helper: linear attack/decay, exp release. Multi-tone events synthesise with cross-faded sine bursts. All tones normalised to -3 dBFS so volumes are consistent.

### `tools/gen_voice.sh` — voice phrases

POSIX shell script. Reads `tools/voice_phrases.txt` (one phrase per line, in event-enum order), pipes each line into `piper`:

```bash
piper \
  --model tools/piper-voices/en_US-lessac-medium.onnx \
  --output_file assets/sounds/voice/<event>.wav \
  <<< "<phrase>"
```

Then runs `sox` (or ffmpeg as fallback) to:
1. Trim leading/trailing silence to ≤ 80 ms.
2. Resample to 22.05 kHz mono 16-bit (matches tones, simpler miniaudio cache).
3. Normalise to -3 dBFS.

`--force` flag mirrors the tones tool.

`tools/voice_phrases.txt` content (committed):
```
order filled
partial fill
order rejected
order cancelled
order error
connection lost
connection restored
long setup detected
short setup detected
position unprotected
test
```

`tools/README.md` documents:
- `apt install piper-tts sox` / `brew install piper sox`.
- Voice model URL: `https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/lessac/medium/en_US-lessac-medium.onnx` (+ `.json` config). Download into `tools/piper-voices/` (`.gitignore`'d — only the generated WAVs are committed).
- Run: `cd <repo>; tools/gen_voice.sh --force` to regenerate.
- Run: `./build/tools/gen_tones --force assets/sounds/tones/` to regenerate tones.

The piper model is **not** committed to the repo (~20 MB) — it's a per-developer download. The committed asset is the *output* WAVs only.

### Why piper over alternatives

- `espeak-ng` — universal install but distinctly robotic; for a trading terminal that handles real money the voice should sound trustworthy.
- macOS `say` — best on Mac but locks regen to that platform.
- Pre-recorded human voice — even better, but couples regen to one specific person and one moment in time. Piper gives reproducible, cross-platform, near-human output.

## Trigger wiring

Each `main.cpp` callback handler gets a single new line. No new state apart from edge-detector statics for state-driven signals.

| Site | Event | Title | Body | Edge? |
|---|---|---|---|---|
| `onFillReceived` (full fill) | `OrderFilled` | "Filled" | `"<SIDE> <qty> <sym> @ $<px>"` | No |
| `onFillReceived` (`fillQty < orderQty`) | `OrderPartialFill` | "Partial fill" | `"<filled>/<total> <sym> @ $<px>"` | No |
| `onOrderStatusChanged` → `Rejected` | `OrderRejected` | "Order rejected" | `"<sym> <orderType> — <reason>"` | No |
| `onOrderStatusChanged` → `Cancelled` (unsolicited; not in `g_userCancelled`) | `OrderCancelled` | "Order cancelled" | `"<sym> <orderType>"` | No |
| `onError` for IB error codes 110, 201, 202, 321, 354, 10148–10149 (curated allowlist) | `IbError` | "IB error <code>" | IB string truncated to 200 ch | Per orderId — once per error per order |
| `onConnectionChanged(false)` (unexpected drop) | `ConnectionLost` | "Disconnected" | `"Lost connection to IB Gateway."` | No (one drop = one toast) |
| `onConnectionChanged(true, isReconnect=true)` | `ConnectionRestored` | "Reconnected" | `"Session restored."` | No |
| `ChartWindow` `m_breakoutSignal` transitions `None → LongSetup` | `LongSetup` | "Long setup" | `"<sym> @ $<px> — R:R <rr>"` | Yes — per chart instance |
| `ChartWindow` `m_breakoutSignal` transitions `None → ShortSetup` | `ShortSetup` | "Short setup" | `"<sym> @ $<px> — R:R <rr>"` | Yes |
| `g_unguarded` set-diff: symbol added | `UnguardedPosition` | "Unguarded position" | `"<sym> <qty> sh @ $<avg> — no protective stop"` | Yes — symbol entry transition |

Edge detection lives in two places:

- **ChartWindow signals** — extend `ChartWindow` with `BreakoutDirection m_lastNotifiedSignal = None;` and a `std::function<void(BreakoutDirection)> OnSignalChange` callback set by `SpawnChartWindow`. In `ComputeBreakoutSignal()`, after the new signal is determined, if `m_lastNotifiedSignal == None && m_breakoutSignal != None` → fire the callback. Update `m_lastNotifiedSignal` regardless. Reset to `None` on `SetSymbol()` so a new symbol starts fresh.
- **Unguarded-position diff** — `RecomputeUnguardedPositions()` already runs in `main.cpp`. Add `static std::vector<std::string> g_lastUnguardedSymbols;` Compare new vs last; for any symbol in new ∖ last, `Notify(...)`. Replace `g_lastUnguardedSymbols` at the end of the function.

## Wiring `Notify` into the rest of the codebase

`main.cpp` owns the service:
```cpp
static std::unique_ptr<core::services::NotificationService> g_NotificationService;
```

Created in `main()` after ImGui setup, destroyed before `ImGui_ImplGlfw_Shutdown`. All `Notify` call sites guard on `if (g_NotificationService)` since service init can fail (audio device unavailable on headless / CI). On init failure the service still queues toasts — only audio is silenced.

`g_NotificationService->Tick()` called once per frame in `RenderTradingUI()` (next to `DrainStyleSwitchQueue()` / `PushUnguardedHintsToWindows()`).

`ChartWindow` doesn't take a service pointer — it calls `OnSignalChange` (set in `SpawnChartWindow`'s lambda). Keeps `ChartWindow` free of `NotificationService.h`.

## Tests — `tests/test_notification_service.cpp`

Catch2, in `tests-core` (no IB API / ImGui dep). New `[notify]` tag. ~14 cases / ~70 assertions. Audio path mocked via `PlayHook`.

- `Notify` enqueues + Drain returns FIFO.
- Drain max cap respected; remaining items stay queued.
- Queue self-caps at `kMaxQueued = 64` (oldest dropped).
- `enableTones=1, enableVoice=0` → exactly one PlayHook call with `isVoice=false`, no voice scheduled.
- `enableTones=0, enableVoice=1` → no immediate tone; one pending play; `Tick()` after 150 ms drains it as `isVoice=true`. Voice plays immediately (no delay) when no tone precedes it.
- `enableTones=1, enableVoice=1` → immediate tone + pending voice 150 ms out.
- `enableTones=0, enableVoice=0, enableToasts=1` → no PlayHook calls; toast still queued.
- `enableToasts=0` → no toast queued; audio still fires per the two surface flags.
- `Tick()` honours due-time — voice dispatched only after `kVoiceDelayMs` (test injects a clock).
- `masterEnable=false` → suppresses **everything**: no audio, no toast, no pending plays scheduled.
- Per-category disable suppresses both audio and toast for that category (e.g. `enableOrders=false` silences fills + rejects + cancels + IB errors).
- Cross-category isolation: connection events still surface when orders disabled.
- `Test` event lives in `Category::System` and ignores per-category toggles, but respects `masterEnable` and the three surface flags.
- `NotifyForce(playTone, playVoice, showToast)` (used by the three Test buttons) bypasses the surface flags but still respects `masterEnable`.
- `SettingsFilePath()` returns expected path under `~/.config/ibkr-trading-app/`.
- `SaveSettingsTo` / `LoadSettingsFrom` round-trip every field.
- `LoadSettingsFrom` on a malformed file returns `false`, leaves `out` untouched.
- `LoadSettingsFrom` on a missing file returns `false`, leaves `out` untouched (caller falls back to defaults).
- `ToneAssetPath` / `VoiceAssetPath` cover all 11 events.

## CMake changes

Top-level:
```cmake
option(IBKR_BUILD_TOOLS "Build helper tools (sound generators)" OFF)
add_subdirectory(third_party/miniaudio)
target_link_libraries(ibkr-trading-app PRIVATE miniaudio)
target_sources(ibkr-trading-app PRIVATE
    src/core/services/NotificationService.cpp
    src/ui/NotificationOverlay.cpp
)
if(IBKR_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
```

Asset deployment: a CMake `configure_file` / install rule copies `assets/` next to the binary so `NotificationService` can resolve relative paths consistently. Path resolution order at runtime:
1. `<exe-dir>/assets/sounds/...` (deployed/installed location)
2. `<repo-root>/assets/sounds/...` (dev build, walking up from CWD)
3. Service silently degrades to "queue toast, skip audio" if no path resolves.

`tests/CMakeLists.txt`: add `test_notification_service.cpp` to `tests-core`. No miniaudio link needed (PlayHook stubs it out). Tests inject a fake clock function so `Tick()` scheduling is deterministic.

## ReqId allocation

No new IB reqIds — this feature doesn't touch the IB API.

## Persistence

- `~/.config/ibkr-trading-app/notifications.cfg` — settings (see format above).
- No history file. Dismissed toasts vanish; the user looks at order history / disconnect badge for after-the-fact context. Notification history is a v2 deferred item.

## Acceptance — manual smoke test

1. Build with `IBKR_BUILD_TOOLS=ON`, install piper + sox, download model, run `tools/gen_tones --force assets/sounds/tones/` and `tools/gen_voice.sh --force`. Verify 22 WAVs present.
2. Build main app, connect to paper IB. Open Settings → Notifications, hit "Test tone" — chime + cyan toast appears top-right. Hit "Test voice" — spoken "Test" + cyan toast.
3. Place a market BUY 1 share — fill toast (green) + rising ping + "Order filled" voice 150 ms later.
4. Place a partial-fill scenario (e.g. limit order that fills against thin book) — green partial-fill toast + half-volume ping + "Partial fill" voice.
5. Place an order with bad auxPrice (e.g. STP $0.001) — IB error 110 → red toast + descending sweep + "Order error" voice.
6. Cancel an order from outside the app (TWS) — orange "Order cancelled" toast + double-tone + "Order cancelled" voice.
7. Stop IB Gateway — orange "Disconnected" toast within 5 s + warble + "Connection lost" voice. Restart Gateway — green "Reconnected" toast on next reconnect tick + arpeggio + "Connection restored" voice.
8. Open AAPL D1 chart, scroll to a known consolidation breakout zone with `Setup` toggle on — orange "Long setup" or "Short setup" fires once + corresponding chime + voice. Hold the signal for 30 s — no re-fire. Switch symbol → state resets, fires again on next match.
9. Open a position without a stop — orange "Unguarded position" fires once + warning two-tone + voice. Place stop → guard clears, no toast on clear.
10. Toggle master mute — Test buttons silent + no toast (toasts are suppressed too — see test note above).

  → Wait, that contradicts the test plan. **Decision**: master mute mutes audio only; toasts still surface. Per-category disable suppresses **both** audio and toast. That matches: "mute all notifications" = "shut up the audio, I'm on a call"; "disable Order events" = "I don't want fill toasts at all". Update test #8 in the spec accordingly. Test plan above already reflects this.

11. Toggle "Order events" off, place a fill — silent + no toast. Disconnect — toast + audio fire (Connection still enabled).
12. Switch playback to Tones Only — Test tone fires the chime, no voice. Switch to Voice Only — Test voice fires, no chime. Switch to Both — both fire 150 ms apart.
13. Drag a chart out into a floating viewport — toasts still anchor to the main app window's top-right (not the floating viewport).
14. Headless / no audio device (CI Linux without `pulseaudio`/`pipewire`) — service init returns gracefully, toasts still appear, no crash.

## Tasks split + progress

All five tasks ship in one PR per earlier alignment, but split by commit so the diff is reviewable in pieces. Progress markers are updated in this file as each sub-step lands so we can resume across sessions.

Legend: `[ ]` pending, `[~]` in progress, `[x]` complete.

- `[x]` **Task A — pure-logic NotificationService + tests + miniaudio vendor.**
  - `[x]` Vendored `third_party/miniaudio/miniaudio.h` v0.11.22 (2025-02-24) via curl from the upstream tag.
  - `[x]` `third_party/miniaudio/CMakeLists.txt` — `INTERFACE` target with platform-specific link deps (Threads + dl + m on Linux, CoreAudio frameworks on macOS, ole32 + winmm on Windows).
  - `[x]` Top-level `CMakeLists.txt` — `add_subdirectory(third_party/miniaudio)`, `IBKR_BUILD_TOOLS` option declared, miniaudio added to `APPLICATION_LIBS`, `NotificationService.cpp` added to `APPLICATION_SOURCES`.
  - `[x]` `src/core/services/NotificationService.h` — full API (header free of miniaudio): events × 11, severity, category, settings POD, queue, `Notify` / `NotifyForce` / `Drain` / `Tick`, `PlayHook` / `ClockFn` test seams.
  - `[x]` `src/core/services/NotificationService.cpp` — queue, settings I/O (atomic .tmp + rename, `~/.config/ibkr-trading-app/notifications.cfg`), `Tick()`, `Notify` / `NotifyForce`, miniaudio-backed `AudioBackend` impl gated on `IBKR_NOTIFICATIONS_NO_AUDIO`.
  - `[x]` `tests/test_notification_service.cpp` — 16 Catch2 cases / 84 assertions under `[notify]` tag.
  - `[x]` `tests/CMakeLists.txt` — added `test_notification_service.cpp` + the service .cpp to `tests-core`, with `IBKR_NOTIFICATIONS_NO_AUDIO` define so tests don't link miniaudio.
  - `[x]` Build clean + `ctest` 311/311 pass (was 295/295 + 16 new).
- `[x]` **Task B — gen_tones tool + gen_voice.sh + WAV assets.**
  - `[x]` `tools/gen_tones.cpp` — hand-rolled WAV writer + ADSR/sweep/LFO/bell synthesisers, `--force` flag, no project deps.
  - `[x]` `tools/CMakeLists.txt` — `gen_tones` exe target; auto-links `stdc++fs` on old GCC.
  - `[x]` Top-level `CMakeLists.txt` — wires `add_subdirectory(tools)` under `IBKR_BUILD_TOOLS`.
  - `[x]` `tools/voice_phrases.txt` — single source of truth (`event_name | phrase` format).
  - `[x]` `tools/gen_voice.sh` — piper + sox pipeline (silence trim, resample to 22.05 kHz mono 16-bit, normalise -3 dBFS), `--force` flag, prerequisite checks fail gracefully if piper/sox/model missing.
  - `[x]` `tools/README.md` — install instructions (apt + brew), Hugging Face model URL, run instructions, voice-swap notes (drop-in custom WAVs supported).
  - `[x]` `assets/sounds/tones/<event>.wav` × 11 — generated and committed (153 KB total, all valid 22.05 kHz 16-bit mono PCM RIFF).
  - `[x]` `assets/sounds/voice/<event>.wav` × 11 — generated and committed (~424 KB total, 22.05 kHz 16-bit mono PCM, durations 0.5–1.2 s). Generated via `tools/gen_voice.sh --force` using piper 2023.11.14-2 standalone Linux x86_64 binary at `~/.local/share/piper/piper/piper` (symlinked into `~/.local/bin/piper`) with the `en_US-lessac-medium` voice model. Note: `apt install piper-tts` is not available on Ubuntu noble — the standalone GitHub release binary is used instead. Service degrades gracefully on missing voice WAVs (Task A `PlayHook` path is a no-op when miniaudio can't open the file).
  - `[x]` `.gitignore` extended for `tools/piper-voices/*.onnx*` (model files not committed).
  - `[x]` `build.md` updated with Tools build/run snippet.
- `[ ]` **Task C — NotificationOverlay + Settings panel.**
  - `[ ]` `src/ui/NotificationOverlay.{h,cpp}`.
  - `[ ]` `g_NotificationService` global + lifecycle in `main()`.
  - `[ ]` Overlay drawn after `RenderTradingUI` and before `RenderCustomTitleBar`.
  - `[ ]` Settings panel section per the layout above (master + volume + 3 surface flags + 3 category flags + 3 test buttons).
  - `[ ]` Asset path resolution (`<exe-dir>/assets` → `<cwd>/assets` → graceful skip on miss).
  - `[ ]` `Tick()` call in `RenderTradingUI`.
- `[ ]` **Task D — Trigger wiring in main.cpp.**
  - `[ ]` `onFillReceived` (full + partial fill paths).
  - `[ ]` `onOrderStatusChanged` (Rejected, unsolicited Cancelled).
  - `[ ]` `onError` curated allowlist (110, 201, 202, 321, 354, 10148–10149).
  - `[ ]` `onConnectionChanged(false)` + reconnect success path.
  - `[ ]` `g_unguarded` set-diff edge detector.
  - `[ ]` `ChartWindow::OnSignalChange` callback + `m_lastNotifiedSignal` state + `SpawnChartWindow` wiring.
- `[ ]` **Task E — Docs.**
  - `[ ]` `CLAUDE.md` (rules pointer if needed).
  - `[ ]` `.claude/rules/architecture.md` — new "Notifications" section.
  - `[ ]` `.claude/rules/testing.md` — `[notify]` tag entry.
  - `[ ]` `.claude/rules/task-history.md` — Phase 16 entries.

## Open questions deferred to v2

- Notification history pane / replay.
- Per-symbol or per-timeframe alert rules (e.g. "alert me when /ES crosses 5500").
- Threshold alerts on indicators (RSI > 70 cross, breakout signal across multiple symbols).
- OS-native notifications when the app is unfocused / minimised.
- Custom user-recorded voice packs at runtime (drop-in folder of WAVs in `~/.config/ibkr-trading-app/voice-packs/<name>/`).
- Per-event volume override.
- Notification snooze ("mute for 10 min").
- Multi-language voice packs (piper supports many locales — single config field would switch the active pack).
