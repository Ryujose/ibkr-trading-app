#include "core/services/NotificationService.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

// miniaudio implementation lives in this TU. Header-only library — exactly one
// translation unit must define the impl macro.
#ifndef IBKR_NOTIFICATIONS_NO_AUDIO
    #define MINIAUDIO_IMPLEMENTATION
    // Disable backends we don't need to keep build times reasonable.
    #define MA_NO_DECODING_FLAC
    #define MA_NO_DECODING_VORBIS
    #define MA_NO_DECODING_MP3
    #include "miniaudio.h"
#endif

namespace core::services {

// ── canonical event metadata ────────────────────────────────────────────────
namespace {

struct EventMeta { NotificationEvent ev; const char* name; };

constexpr std::array<EventMeta, static_cast<size_t>(NotificationEvent::_Count)> kEventMeta = {{
    { NotificationEvent::OrderFilled,         "order_filled"        },
    { NotificationEvent::OrderPartialFill,    "order_partial_fill"  },
    { NotificationEvent::OrderRejected,       "order_rejected"      },
    { NotificationEvent::OrderCancelled,      "order_cancelled"     },
    { NotificationEvent::IbError,             "ib_error"            },
    { NotificationEvent::ConnectionLost,      "connection_lost"     },
    { NotificationEvent::ConnectionRestored,  "connection_restored" },
    { NotificationEvent::LongSetup,           "long_setup"          },
    { NotificationEvent::ShortSetup,          "short_setup"         },
    { NotificationEvent::UnguardedPosition,   "unguarded_position"  },
    { NotificationEvent::Test,                "test"                },
}};

const char* eventNameInternal(NotificationEvent e) {
    const auto idx = static_cast<size_t>(e);
    if (idx >= kEventMeta.size()) return "unknown";
    return kEventMeta[idx].name;
}

}   // namespace

const char* NotificationService::EventName(NotificationEvent e) {
    return eventNameInternal(e);
}

std::string NotificationService::ToneAssetRelPath(NotificationEvent e) {
    return std::string("tones/") + eventNameInternal(e) + ".wav";
}

std::string NotificationService::VoiceAssetRelPath(NotificationEvent e) {
    return std::string("voice/") + eventNameInternal(e) + ".wav";
}

// ── audio backend (miniaudio-backed; opaque to header) ──────────────────────
struct NotificationService::AudioBackend {
#ifndef IBKR_NOTIFICATIONS_NO_AUDIO
    bool       initialised = false;
    ma_engine  engine{};

    bool Init() {
        ma_result r = ma_engine_init(nullptr, &engine);
        if (r != MA_SUCCESS) {
            std::fprintf(stderr, "[notify] ma_engine_init failed (%d) — audio disabled\n",
                         (int)r);
            return false;
        }
        initialised = true;
        return true;
    }

    void Shutdown() {
        if (initialised) {
            ma_engine_uninit(&engine);
            initialised = false;
        }
    }

    void SetVolume01(float v) {
        if (initialised) ma_engine_set_volume(&engine, v);
    }

    void Play(const std::string& path) {
        if (!initialised) {
            std::fprintf(stderr, "[notify] audio not initialised — drop %s\n",
                         path.c_str());
            return;
        }
        // Fire-and-forget; miniaudio mixes on its own audio thread.
        ma_result r = ma_engine_play_sound(&engine, path.c_str(), nullptr);
        if (r != MA_SUCCESS) {
            std::fprintf(stderr, "[notify] play failed (%d): %s\n",
                         (int)r, path.c_str());
        }
    }
#else
    bool Init()                            { return false; }
    void Shutdown()                        {}
    void SetVolume01(float)                {}
    void Play(const std::string& path)     {
        std::fprintf(stderr, "[notify] audio disabled — drop %s\n", path.c_str());
    }
#endif
};

// ── lifecycle ───────────────────────────────────────────────────────────────
NotificationService::NotificationService()
    : m_audio(std::make_unique<AudioBackend>())
{
    NotificationSettings loaded;
    if (LoadSettingsFrom(SettingsFilePath(), loaded))
        m_settings = loaded;

    // Default clock = monotonic seconds since service construction.
    m_clock = []() {
        using clock = std::chrono::steady_clock;
        static const auto t0 = clock::now();
        const auto now = clock::now();
        return std::chrono::duration<double>(now - t0).count();
    };

    if (!m_audio->Init()) {
        // Audio init failure (no audio device, headless CI, etc.) is non-fatal.
        // Toasts still queue; PlayNow becomes a no-op when m_audio is unin'd.
    } else {
        m_audio->SetVolume01(static_cast<float>(m_settings.masterVolume) / 100.0f);
    }
}

NotificationService::~NotificationService() {
    if (m_audio) m_audio->Shutdown();
}

// ── producers ───────────────────────────────────────────────────────────────
namespace {

bool categoryEnabled(const NotificationSettings& s, NotificationCategory c) {
    switch (c) {
        case NotificationCategory::Orders:     return s.enableOrders;
        case NotificationCategory::Connection: return s.enableConnection;
        case NotificationCategory::Signals:    return s.enableSignals;
        case NotificationCategory::System:     return true;   // ignores per-category toggles
    }
    return true;
}

}   // namespace

void NotificationService::Notify(NotificationSeverity sev,
                                 NotificationCategory cat,
                                 NotificationEvent    ev,
                                 std::string          title,
                                 std::string          body)
{
    bool playTone, playVoice, showToast;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_settings.masterEnable)             return;
        if (!categoryEnabled(m_settings, cat))    return;
        playTone  = m_settings.enableTones;
        playVoice = m_settings.enableVoice;
        showToast = m_settings.enableToasts;
    }
    NotifyForce(sev, cat, ev, std::move(title), std::move(body),
                playTone, playVoice, showToast);
}

void NotificationService::NotifyForce(NotificationSeverity sev,
                                      NotificationCategory cat,
                                      NotificationEvent    ev,
                                      std::string          title,
                                      std::string          body,
                                      bool                 playTone,
                                      bool                 playVoice,
                                      bool                 showToast)
{
    bool   masterOn = false;
    double tNow     = 0.0;
    int    id       = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        masterOn = m_settings.masterEnable;
        if (!masterOn) return;
        id   = m_nextId++;
        tNow = m_clock ? m_clock() : 0.0;

        // Build the notification once; copy into both the toast queue (if
        // showToast) and the history ring buffer (always, when masterEnable
        // is on — the user can re-read what fired even if toasts were off).
        Notification n;
        n.severity = sev;
        n.category = cat;
        n.event    = ev;
        n.title    = std::move(title);
        n.body     = std::move(body);
        n.ts       = std::time(nullptr);
        n.id       = id;

        m_history.push_back(n);
        while ((int)m_history.size() > kMaxHistory) m_history.pop_front();

        if (showToast) {
            m_queue.push_back(std::move(n));
            // Soft-cap: drop oldest when we exceed kMaxQueued.
            while ((int)m_queue.size() > kMaxQueued) m_queue.pop_front();
        }

        // Schedule audio plays under the same lock so masterEnable / Tick race
        // with consistent ordering.
        if (playTone) {
            m_pendingPlays.push_back({ tNow, ev, /*isVoice=*/false });
        }
        if (playVoice) {
            // Delay voice only if a tone precedes it; voice-alone fires now.
            const double dueAt = tNow + (playTone ? kVoiceDelayMs / 1000.0 : 0.0);
            m_pendingPlays.push_back({ dueAt, ev, /*isVoice=*/true });
        }
    }
    // Tick will pick up due plays; explicitly drain the immediate ones now so
    // the producer doesn't have to wait for the next UI frame.
    Tick();
}

// ── history accessors ───────────────────────────────────────────────────────
std::vector<Notification> NotificationService::history(int max) const {
    std::vector<Notification> out;
    if (max <= 0) return out;
    std::lock_guard<std::mutex> lk(m_mutex);
    const int n = std::min(max, (int)m_history.size());
    out.reserve(n);
    // Newest-first: walk the deque from the back.
    for (auto it = m_history.rbegin();
         it != m_history.rend() && (int)out.size() < n; ++it)
        out.push_back(*it);
    return out;
}

void NotificationService::clearHistory() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_history.clear();
}

// ── consumer ────────────────────────────────────────────────────────────────
std::vector<Notification> NotificationService::Drain(int max) {
    std::vector<Notification> out;
    if (max <= 0) return out;
    std::lock_guard<std::mutex> lk(m_mutex);
    out.reserve(std::min((int)m_queue.size(), max));
    while (!m_queue.empty() && (int)out.size() < max) {
        out.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
    }
    return out;
}

void NotificationService::Tick() {
    std::vector<PendingPlay> due;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const double tNow = m_clock ? m_clock() : 0.0;
        // Drain due-time-elapsed plays in FIFO order. We don't sort by dueAt:
        // the queue is already monotonic by insertion time + voice-delay
        // semantics so callers see the same order they enqueued.
        while (!m_pendingPlays.empty() && m_pendingPlays.front().dueAt <= tNow) {
            due.push_back(m_pendingPlays.front());
            m_pendingPlays.pop_front();
        }
    }
    for (const auto& p : due) PlayNow(p.event, p.isVoice);
}

void NotificationService::PlayNow(NotificationEvent ev, bool isVoice) {
    PlayHook hook;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        hook = m_playHook;
    }
    if (hook) {
        hook({ ev, isVoice });
        return;
    }
    if (!m_audio) return;
    const std::string base = m_assetDir.empty() ? "assets/sounds" : m_assetDir;
    const std::string rel  = isVoice ? VoiceAssetRelPath(ev) : ToneAssetRelPath(ev);
    m_audio->Play(base + "/" + rel);
}

// ── settings ────────────────────────────────────────────────────────────────
NotificationSettings NotificationService::settings() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_settings;
}

void NotificationService::setSettings(const NotificationSettings& s) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_settings = s;
    }
    if (m_audio) {
        m_audio->SetVolume01(static_cast<float>(s.masterVolume) / 100.0f);
    }
    SaveSettingsTo(SettingsFilePath(), s);
}

void NotificationService::SetPlayHook(PlayHook hook) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_playHook = std::move(hook);
}

void NotificationService::SetClock(ClockFn fn) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_clock = std::move(fn);
}

double NotificationService::now() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_clock ? m_clock() : 0.0;
}

void NotificationService::SetAssetDir(std::string dir) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_assetDir = std::move(dir);
}

std::string NotificationService::assetDir() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_assetDir;
}

// ── settings persistence (mirror of chart-modes.cfg pattern) ────────────────
namespace {

std::string getEnvOrEmpty(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

std::string configHome() {
    auto xdg = getEnvOrEmpty("XDG_CONFIG_HOME");
    if (!xdg.empty()) return xdg;
    auto home = getEnvOrEmpty("HOME");
    if (!home.empty()) return home + "/.config";
    return std::string(".");
}

void mkDirP(const std::string& path) {
    // Lazy: just ensure the immediate parent exists. No-op on EEXIST.
    ::mkdir(path.c_str(), 0755);
}

bool parseBool(const std::string& v) {
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes") return true;
    return false;
}

bool parseInt(const std::string& v, int& out) {
    if (v.empty()) return false;
    try { out = std::stoi(v); } catch (...) { return false; }
    return true;
}

}   // namespace

std::string NotificationService::SettingsFilePath() {
    const std::string dir = configHome() + "/ibkr-trading-app";
    return dir + "/notifications.cfg";
}

bool NotificationService::SaveSettingsTo(const std::string& path,
                                         const NotificationSettings& s)
{
    // Atomic write: tmp + rename.
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) mkDirP(path.substr(0, slash));

    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "masterEnable="     << (s.masterEnable     ? 1 : 0) << '\n';
    f << "masterVolume="     << s.masterVolume                << '\n';
    f << "enableTones="      << (s.enableTones      ? 1 : 0) << '\n';
    f << "enableVoice="      << (s.enableVoice      ? 1 : 0) << '\n';
    f << "enableToasts="     << (s.enableToasts     ? 1 : 0) << '\n';
    f << "enableOrders="     << (s.enableOrders     ? 1 : 0) << '\n';
    f << "enableConnection=" << (s.enableConnection ? 1 : 0) << '\n';
    f << "enableSignals="    << (s.enableSignals    ? 1 : 0) << '\n';
    f.flush();
    if (!f.good()) { f.close(); std::remove(tmp.c_str()); return false; }
    f.close();
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool NotificationService::LoadSettingsFrom(const std::string& path,
                                           NotificationSettings& out)
{
    std::ifstream f(path);
    if (!f) return false;
    NotificationSettings s;
    bool sawAny    = false;
    bool malformed = false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) { malformed = true; break; }
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        sawAny = true;
        if      (key == "masterEnable")     s.masterEnable     = parseBool(val);
        else if (key == "masterVolume") {
            int n;
            if (!parseInt(val, n)) { malformed = true; break; }
            s.masterVolume = std::max(0, std::min(100, n));
        }
        else if (key == "enableTones")      s.enableTones      = parseBool(val);
        else if (key == "enableVoice")      s.enableVoice      = parseBool(val);
        else if (key == "enableToasts")     s.enableToasts     = parseBool(val);
        else if (key == "enableOrders")     s.enableOrders     = parseBool(val);
        else if (key == "enableConnection") s.enableConnection = parseBool(val);
        else if (key == "enableSignals")    s.enableSignals    = parseBool(val);
        // unknown keys are tolerated for forward-compat.
    }
    if (malformed || !sawAny) return false;
    out = s;
    return true;
}

}   // namespace core::services
