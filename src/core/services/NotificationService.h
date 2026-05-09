#pragma once

// NotificationService — thread-safe queue + audio dispatch for the trading
// terminal. Header is free of miniaudio so anything in core/ui can include it
// without dragging in the audio backend.
//
// Two surfaces, one service:
//   - in-app toast (drained by the UI thread via Drain())
//   - audio (per-event tone WAV + per-event voice WAV, played via miniaudio
//     when the implementation is built; tests-core stubs this via PlayHook)
//
// Threading: Notify() is callable from any thread (typically the IB EReader
// thread). Drain() and Tick() must run on the UI thread.

#include <ctime>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace core::services {

enum class NotificationSeverity : int {
    Success = 0,   // green border  — fills, reconnect
    Info    = 1,   // cyan  border  — chime, Test
    Warning = 2,   // amber border  — disconnect, signals, unguarded
    Error   = 3,   // red   border  — IB rejections, error codes
};

enum class NotificationCategory : int {
    Orders     = 0,
    Connection = 1,
    Signals    = 2,
    System     = 3,   // Test event lives here; ignores per-category toggles.
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
    std::string          title;          // ≤ 48 chars suggested
    std::string          body;           // ≤ 200 chars suggested
    std::time_t          ts        = 0;
    int                  id        = 0;  // monotonic, used as ImGui id key
};

// Surfaces and categories are independent. Combinations cover every "playback
// style" the user might want:
//   tones-only:    enableTones=1, enableVoice=0
//   voice-only:    enableTones=0, enableVoice=1
//   silent toasts: enableTones=0, enableVoice=0, enableToasts=1
//   total mute:    masterEnable=0
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

constexpr int    kMaxQueued     = 64;     // soft cap; oldest dropped past this
constexpr int    kMaxHistory    = 500;    // history ring buffer cap
constexpr double kVoiceDelayMs  = 150.0;  // delay between tone and voice when both enabled

class NotificationService {
public:
    NotificationService();
    ~NotificationService();

    NotificationService(const NotificationService&)            = delete;
    NotificationService& operator=(const NotificationService&) = delete;

    // ── Producer side (any thread) ───────────────────────────────────────────
    // Honours the current settings: per-category enable, surface flags, master.
    void Notify(NotificationSeverity, NotificationCategory, NotificationEvent,
                std::string title, std::string body);

    // Test-button path: bypass the surface flags so the user can sanity-check
    // each surface independently. Still honours masterEnable + masterVolume.
    void NotifyForce(NotificationSeverity, NotificationCategory, NotificationEvent,
                     std::string title, std::string body,
                     bool playTone, bool playVoice, bool showToast);

    // ── Consumer side (UI thread) ────────────────────────────────────────────
    // Drains visible toasts from the queue (FIFO). Removes returned items.
    std::vector<Notification> Drain(int max = 16);

    // History — every Notify call (when masterEnable is on) appends here,
    // independently of the toast surface flag. Capped at kMaxHistory; oldest
    // dropped past the cap. Returned newest-first so the UI can render a
    // reverse-chronological list without re-sorting.
    std::vector<Notification> history(int max = 200) const;
    void                      clearHistory();

    // Must be called once per frame. Dispatches delayed voice plays whose
    // due-time has passed.
    void Tick();

    // ── Settings ─────────────────────────────────────────────────────────────
    NotificationSettings settings() const;
    void                 setSettings(const NotificationSettings&);

    // ── Test seam ────────────────────────────────────────────────────────────
    // When set, replaces miniaudio dispatch with a callback so tests-core can
    // assert which sounds would have played without linking the audio backend.
    struct PlayedSound { NotificationEvent event; bool isVoice; };
    using PlayHook = std::function<void(PlayedSound)>;
    void SetPlayHook(PlayHook);

    // Inject a deterministic clock for tests. Returns seconds (any monotonic
    // origin). Default is glfwGetTime() in the real app, std::chrono in tests.
    using ClockFn = std::function<double()>;
    void SetClock(ClockFn);

    // ── Asset path resolution (also used by gen tools) ───────────────────────
    static std::string ToneAssetRelPath (NotificationEvent);   // "tones/<event>.wav"
    static std::string VoiceAssetRelPath(NotificationEvent);   // "voice/<event>.wav"
    static const char* EventName        (NotificationEvent);   // canonical filename stem

    // ── Settings persistence ─────────────────────────────────────────────────
    static std::string SettingsFilePath();   // ~/.config/ibkr-trading-app/notifications.cfg
    static bool        SaveSettingsTo (const std::string& path, const NotificationSettings&);
    static bool        LoadSettingsFrom(const std::string& path, NotificationSettings& out);

    // ── Asset directory (set by main.cpp once at startup; tests use a stub) ──
    void        SetAssetDir(std::string dir);
    std::string assetDir() const;

private:
    struct PendingPlay {
        double             dueAt;
        NotificationEvent  event;
        bool               isVoice;
    };

    // Forward-declared backend; impl owns ma_engine + ma_sound cache.
    struct AudioBackend;

    void   PlayNow(NotificationEvent, bool isVoice);   // honours PlayHook
    double now() const;                                // honours ClockFn
    void   schedulePlays(NotificationEvent, bool playTone, bool playVoice);

    mutable std::mutex            m_mutex;
    std::deque<Notification>      m_queue;
    std::deque<Notification>      m_history;     // ring buffer for the history window
    std::deque<PendingPlay>       m_pendingPlays;
    NotificationSettings          m_settings;
    int                           m_nextId = 1;
    PlayHook                      m_playHook;
    ClockFn                       m_clock;
    std::string                   m_assetDir;
    std::unique_ptr<AudioBackend> m_audio;
};

}   // namespace core::services
