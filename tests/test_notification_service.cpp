// NotificationService — pure-logic tests. The audio backend is disabled in
// this TU via IBKR_NOTIFICATIONS_NO_AUDIO (set in tests/CMakeLists.txt) so we
// don't need to link miniaudio. PlayHook captures every dispatch.

#include "core/services/NotificationService.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using core::services::Notification;
using core::services::NotificationCategory;
using core::services::NotificationEvent;
using core::services::NotificationService;
using core::services::NotificationSettings;
using core::services::NotificationSeverity;
using core::services::kMaxQueued;
using PlayedSound = NotificationService::PlayedSound;

namespace {

struct ServiceFixture {
    NotificationService          svc;
    std::vector<PlayedSound>     played;
    double                       fakeNow = 0.0;

    ServiceFixture() {
        // Defaults-from-disk would pollute tests; force known-good defaults.
        NotificationSettings s;
        svc.setSettings(s);
        svc.SetPlayHook([this](PlayedSound p) { played.push_back(p); });
        svc.SetClock([this]() { return fakeNow; });
    }

    void advance(double seconds) {
        fakeNow += seconds;
        svc.Tick();
    }
};

std::string tmpFilePath(const char* name) {
    const char* tmp = std::getenv("TMPDIR");
    std::string base = tmp ? tmp : "/tmp";
    return base + "/ibkr-notify-test-" + name + ".cfg";
}

void deleteIfExists(const std::string& p) { std::remove(p.c_str()); }

}   // namespace

TEST_CASE("Notify enqueues toasts in FIFO order", "[notify]") {
    ServiceFixture f;
    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "AAPL 100 @ 187.42");
    f.svc.Notify(NotificationSeverity::Error, NotificationCategory::Orders,
                 NotificationEvent::OrderRejected, "Rejected", "MSFT");

    auto out = f.svc.Drain();
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].title == "Filled");
    REQUIRE(out[0].event == NotificationEvent::OrderFilled);
    REQUIRE(out[1].title == "Rejected");
    REQUIRE(out[1].event == NotificationEvent::OrderRejected);

    // Subsequent drain returns nothing.
    auto out2 = f.svc.Drain();
    REQUIRE(out2.empty());
}

TEST_CASE("Drain max cap leaves remainder queued", "[notify]") {
    ServiceFixture f;
    for (int i = 0; i < 5; ++i) {
        f.svc.Notify(NotificationSeverity::Info, NotificationCategory::Orders,
                     NotificationEvent::Test, "n" + std::to_string(i), "");
    }
    auto first = f.svc.Drain(2);
    REQUIRE(first.size() == 2);
    REQUIRE(first[0].title == "n0");
    REQUIRE(first[1].title == "n1");

    auto rest = f.svc.Drain();
    REQUIRE(rest.size() == 3);
    REQUIRE(rest[0].title == "n2");
}

TEST_CASE("Queue self-caps at kMaxQueued, drops oldest", "[notify]") {
    ServiceFixture f;
    // Disable audio surfaces so we measure only toast queue behaviour.
    NotificationSettings s = f.svc.settings();
    s.enableTones = s.enableVoice = false;
    f.svc.setSettings(s);

    for (int i = 0; i < kMaxQueued + 5; ++i) {
        f.svc.Notify(NotificationSeverity::Info, NotificationCategory::Orders,
                     NotificationEvent::Test, std::to_string(i), "");
    }
    auto out = f.svc.Drain(kMaxQueued + 10);
    REQUIRE(static_cast<int>(out.size()) == kMaxQueued);
    // Oldest 5 should have been dropped → first surviving title is "5".
    REQUIRE(out.front().title == "5");
    REQUIRE(out.back().title == std::to_string(kMaxQueued + 4));
}

TEST_CASE("Tones-only plays exactly one tone, no voice scheduled", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableTones = true;
    s.enableVoice = false;
    f.svc.setSettings(s);

    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "");
    REQUIRE(f.played.size() == 1);
    REQUIRE(f.played[0].event   == NotificationEvent::OrderFilled);
    REQUIRE(f.played[0].isVoice == false);

    f.advance(1.0);   // no further plays should fire from Tick()
    REQUIRE(f.played.size() == 1);
}

TEST_CASE("Voice-only fires immediately with no leading tone", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableTones = false;
    s.enableVoice = true;
    f.svc.setSettings(s);

    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "");
    REQUIRE(f.played.size() == 1);
    REQUIRE(f.played[0].isVoice == true);
}

TEST_CASE("Both surfaces: tone immediate, voice +150ms", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableTones = true;
    s.enableVoice = true;
    f.svc.setSettings(s);

    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "");
    // Only the tone fires in Notify() (voice scheduled +150ms).
    REQUIRE(f.played.size() == 1);
    REQUIRE(f.played[0].isVoice == false);

    // 100 ms — voice not yet due.
    f.advance(0.10);
    REQUIRE(f.played.size() == 1);

    // Cross 150 ms threshold.
    f.advance(0.06);
    REQUIRE(f.played.size() == 2);
    REQUIRE(f.played[1].isVoice == true);
    REQUIRE(f.played[1].event   == NotificationEvent::OrderFilled);
}

TEST_CASE("Toasts disabled but audio still fires", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableToasts = false;
    f.svc.setSettings(s);

    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "");
    REQUIRE(f.svc.Drain().empty());
    REQUIRE(f.played.size() >= 1);   // tone fired (and voice will at +150ms)
}

TEST_CASE("masterEnable=false suppresses everything", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.masterEnable = false;
    f.svc.setSettings(s);

    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "");
    REQUIRE(f.played.empty());
    REQUIRE(f.svc.Drain().empty());

    // Even after a long Tick, nothing pending was queued.
    f.advance(10.0);
    REQUIRE(f.played.empty());
}

TEST_CASE("Per-category disable suppresses both audio and toast", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableOrders = false;
    f.svc.setSettings(s);

    // Orders silenced.
    f.svc.Notify(NotificationSeverity::Success, NotificationCategory::Orders,
                 NotificationEvent::OrderFilled, "Filled", "");
    REQUIRE(f.played.empty());
    REQUIRE(f.svc.Drain().empty());

    // Connection still surfaces.
    f.svc.Notify(NotificationSeverity::Warning, NotificationCategory::Connection,
                 NotificationEvent::ConnectionLost, "Disconnected", "");
    REQUIRE(f.played.size() == 1);
    REQUIRE(f.svc.Drain().size() == 1);
}

TEST_CASE("System category (Test event) ignores per-category toggles", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableOrders = s.enableConnection = s.enableSignals = false;
    f.svc.setSettings(s);

    f.svc.Notify(NotificationSeverity::Info, NotificationCategory::System,
                 NotificationEvent::Test, "Test", "");
    REQUIRE(f.played.size() >= 1);   // tone fired
    REQUIRE(f.svc.Drain().size() == 1);
}

TEST_CASE("NotifyForce bypasses surface flags but honours masterEnable", "[notify]") {
    ServiceFixture f;
    NotificationSettings s = f.svc.settings();
    s.enableTones = s.enableVoice = s.enableToasts = false;
    f.svc.setSettings(s);

    // Public Notify would suppress everything.
    f.svc.Notify(NotificationSeverity::Info, NotificationCategory::System,
                 NotificationEvent::Test, "T", "");
    REQUIRE(f.played.empty());
    REQUIRE(f.svc.Drain().empty());

    // NotifyForce(playTone=true, playVoice=false, showToast=false) plays only the tone.
    f.svc.NotifyForce(NotificationSeverity::Info, NotificationCategory::System,
                      NotificationEvent::Test, "T", "",
                      /*playTone=*/true, /*playVoice=*/false, /*showToast=*/false);
    REQUIRE(f.played.size() == 1);
    REQUIRE(f.played[0].isVoice == false);
    REQUIRE(f.svc.Drain().empty());

    // NotifyForce(false, false, true) queues a toast with no audio.
    f.svc.NotifyForce(NotificationSeverity::Info, NotificationCategory::System,
                      NotificationEvent::Test, "T", "",
                      false, false, true);
    REQUIRE(f.played.size() == 1);   // unchanged
    REQUIRE(f.svc.Drain().size() == 1);

    // masterEnable=false still wins over NotifyForce.
    s.masterEnable = false;
    f.svc.setSettings(s);
    f.svc.NotifyForce(NotificationSeverity::Info, NotificationCategory::System,
                      NotificationEvent::Test, "T", "", true, true, true);
    REQUIRE(f.played.size() == 1);   // unchanged
    REQUIRE(f.svc.Drain().empty());
}

TEST_CASE("Asset path helpers cover all events", "[notify]") {
    using E = NotificationEvent;
    REQUIRE(NotificationService::ToneAssetRelPath(E::OrderFilled)        == "tones/order_filled.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::OrderPartialFill)   == "tones/order_partial_fill.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::OrderRejected)      == "tones/order_rejected.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::OrderCancelled)     == "tones/order_cancelled.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::IbError)            == "tones/ib_error.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::ConnectionLost)     == "tones/connection_lost.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::ConnectionRestored) == "tones/connection_restored.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::LongSetup)          == "tones/long_setup.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::ShortSetup)         == "tones/short_setup.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::UnguardedPosition)  == "tones/unguarded_position.wav");
    REQUIRE(NotificationService::ToneAssetRelPath(E::Test)               == "tones/test.wav");

    REQUIRE(NotificationService::VoiceAssetRelPath(E::OrderFilled) == "voice/order_filled.wav");
    REQUIRE(NotificationService::VoiceAssetRelPath(E::Test)        == "voice/test.wav");
}

TEST_CASE("Settings round-trip every field", "[notify]") {
    NotificationSettings in;
    in.masterEnable     = false;
    in.masterVolume     = 33;
    in.enableTones      = false;
    in.enableVoice      = true;
    in.enableToasts     = false;
    in.enableOrders     = false;
    in.enableConnection = true;
    in.enableSignals    = false;

    const auto path = tmpFilePath("roundtrip");
    deleteIfExists(path);
    REQUIRE(NotificationService::SaveSettingsTo(path, in));

    NotificationSettings out;
    REQUIRE(NotificationService::LoadSettingsFrom(path, out));
    REQUIRE(out.masterEnable     == in.masterEnable);
    REQUIRE(out.masterVolume     == in.masterVolume);
    REQUIRE(out.enableTones      == in.enableTones);
    REQUIRE(out.enableVoice      == in.enableVoice);
    REQUIRE(out.enableToasts     == in.enableToasts);
    REQUIRE(out.enableOrders     == in.enableOrders);
    REQUIRE(out.enableConnection == in.enableConnection);
    REQUIRE(out.enableSignals    == in.enableSignals);
    deleteIfExists(path);
}

TEST_CASE("Settings volume is clamped 0..100 on load", "[notify]") {
    const auto path = tmpFilePath("clamp");
    deleteIfExists(path);
    {
        std::ofstream f(path);
        f << "masterEnable=1\nmasterVolume=999\nenableTones=1\nenableVoice=1\n"
             "enableToasts=1\nenableOrders=1\nenableConnection=1\nenableSignals=1\n";
    }
    NotificationSettings out;
    REQUIRE(NotificationService::LoadSettingsFrom(path, out));
    REQUIRE(out.masterVolume == 100);
    deleteIfExists(path);

    {
        std::ofstream f(path);
        f << "masterVolume=-5\n";
    }
    NotificationSettings out2;
    REQUIRE(NotificationService::LoadSettingsFrom(path, out2));
    REQUIRE(out2.masterVolume == 0);
    deleteIfExists(path);
}

TEST_CASE("LoadSettingsFrom on missing or malformed file returns false and leaves out untouched", "[notify]") {
    NotificationSettings sentinel;
    sentinel.masterVolume = 42;
    NotificationSettings out = sentinel;

    REQUIRE_FALSE(NotificationService::LoadSettingsFrom("/nonexistent/path/notif.cfg", out));
    REQUIRE(out.masterVolume == 42);   // untouched

    const auto path = tmpFilePath("malformed");
    deleteIfExists(path);
    {
        std::ofstream f(path);
        f << "this is not valid\n";   // no '=' → malformed
    }
    NotificationSettings out2 = sentinel;
    REQUIRE_FALSE(NotificationService::LoadSettingsFrom(path, out2));
    REQUIRE(out2.masterVolume == 42);

    // Empty file = no recognised entries → false.
    {
        std::ofstream f(path);
    }
    NotificationSettings out3 = sentinel;
    REQUIRE_FALSE(NotificationService::LoadSettingsFrom(path, out3));
    REQUIRE(out3.masterVolume == 42);
    deleteIfExists(path);
}

TEST_CASE("EventName is canonical for all events", "[notify]") {
    using E = NotificationEvent;
    REQUIRE(std::string(NotificationService::EventName(E::OrderFilled))        == "order_filled");
    REQUIRE(std::string(NotificationService::EventName(E::ConnectionLost))     == "connection_lost");
    REQUIRE(std::string(NotificationService::EventName(E::ConnectionRestored)) == "connection_restored");
    REQUIRE(std::string(NotificationService::EventName(E::UnguardedPosition))  == "unguarded_position");
    REQUIRE(std::string(NotificationService::EventName(E::Test))               == "test");
}
