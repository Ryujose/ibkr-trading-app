#include "ui/NotificationOverlay.h"

#include "core/services/NotificationService.h"
#include "ui/UiScale.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImRect for hit-testing the toast rect

#include <algorithm>
#include <chrono>
#include <vector>

namespace ui {

namespace {

constexpr int    kMaxVisible       = 3;
constexpr double kFadeInMs         = 150.0;
constexpr double kHoldSuccessMs    = 5500.0;
constexpr double kHoldWarningMs    = 8000.0;
constexpr double kHoldErrorMs      = 12000.0;
constexpr double kFadeOutMs        = 350.0;

struct VisibleToast {
    core::services::Notification n;
    double firstShownAt = 0.0;
    double hideAt       = 0.0;       // pushed forward while hovered
    bool   dismissed    = false;
};

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

double HoldMsForSeverity(core::services::NotificationSeverity s) {
    using S = core::services::NotificationSeverity;
    switch (s) {
        case S::Success: return kHoldSuccessMs;
        case S::Info:    return kHoldSuccessMs;
        case S::Warning: return kHoldWarningMs;
        case S::Error:   return kHoldErrorMs;
    }
    return kHoldSuccessMs;
}

ImU32 AccentColorFor(core::services::NotificationSeverity s) {
    using S = core::services::NotificationSeverity;
    switch (s) {
        case S::Success: return IM_COL32( 76, 217, 100, 255);   // green
        case S::Info:    return IM_COL32( 64, 196, 255, 255);   // cyan
        case S::Warning: return IM_COL32(255, 176,  46, 255);   // amber
        case S::Error:   return IM_COL32(255,  82,  82, 255);   // red
    }
    return IM_COL32(255, 255, 255, 255);
}

}   // namespace

void RenderNotificationOverlay(core::services::NotificationService& svc) {
    static std::vector<VisibleToast> s_visible;

    const double tNow = NowSeconds();

    // Drain new notifications until the visible stack is full.
    if ((int)s_visible.size() < kMaxVisible) {
        const int remaining = kMaxVisible - (int)s_visible.size();
        auto fresh = svc.Drain(remaining);
        for (auto& n : fresh) {
            VisibleToast t;
            t.n            = std::move(n);
            t.firstShownAt = tNow;
            t.hideAt       = tNow + (kFadeInMs + HoldMsForSeverity(t.n.severity)) / 1000.0;
            s_visible.push_back(std::move(t));
        }
    }

    if (s_visible.empty()) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) return;

    const float kRightMargin = em(16.0f);
    const float kTopMargin   = em(40.0f);    // below custom title bar
    const float kSpacing     = em(8.0f);
    const float kWidth       = em(280.0f);

    ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
    if (!dl) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    float yCursor = vp->Pos.y + kTopMargin;
    const float xRight = vp->Pos.x + vp->Size.x - kRightMargin;

    // Draw + hit-test each toast. We iterate in display order and prune at end.
    for (auto& t : s_visible) {
        // Compute alpha from fade-in / hold / fade-out timeline.
        const double age = (tNow - t.firstShownAt) * 1000.0;
        const double fadeIn  = kFadeInMs;
        const double hold    = HoldMsForSeverity(t.n.severity);
        const double total   = fadeIn + hold + kFadeOutMs;
        double a = 1.0;
        if      (age < fadeIn)        a = age / fadeIn;
        else if (age < fadeIn + hold) a = 1.0;
        else if (age < total)         a = 1.0 - (age - fadeIn - hold) / kFadeOutMs;
        else                          a = 0.0;
        a = std::clamp(a, 0.0, 1.0);

        // Geometry: title row + optional 2-line body.
        const bool hasBody  = !t.n.body.empty();
        const float titleH  = ImGui::GetFontSize() + em(2.0f);
        const float bodyH   = hasBody ? (ImGui::GetFontSize() * 1.2f + em(2.0f)) : 0.0f;
        const float padY    = em(8.0f);
        const float height  = padY * 2.0f + titleH + (hasBody ? em(2.0f) + bodyH : 0.0f);

        ImVec2 tl(xRight - kWidth, yCursor);
        ImVec2 br(xRight,          yCursor + height);

        // Hover detection in screen space — pause fade and offer click-to-dismiss.
        const bool hovered =
            mouse.x >= tl.x && mouse.x <= br.x &&
            mouse.y >= tl.y && mouse.y <= br.y;
        if (hovered) {
            // Push hideAt forward so the hold timer effectively pauses.
            t.hideAt = std::max(t.hideAt, tNow + (kFadeOutMs / 1000.0));
            // Click anywhere on the toast (or the × glyph) dismisses it.
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                t.dismissed = true;
        }

        // Background — slightly translucent dark panel.
        const ImU32 bg     = ImGui::GetColorU32(ImVec4(0.06f, 0.08f, 0.11f, 0.94f * (float)a));
        const ImU32 border = ImGui::GetColorU32(ImVec4(0.18f, 0.22f, 0.24f, 0.85f * (float)a));
        dl->AddRectFilled(tl, br, bg, em(4.0f));
        dl->AddRect      (tl, br, border, em(4.0f), 0, 1.0f);

        // Severity accent bar on the left edge.
        const ImU32 accent = AccentColorFor(t.n.severity);
        const ImU32 accentA = (accent & 0x00FFFFFF) | ((ImU32)(((accent >> 24) & 0xFF) * a) << 24);
        dl->AddRectFilled(tl, ImVec2(tl.x + em(3.0f), br.y), accentA, em(2.0f));

        // Title (with 1px shadow for legibility against busy charts).
        const ImU32 textCol   = ImGui::GetColorU32(ImVec4(1, 1, 1, (float)a));
        const ImU32 shadowCol = ImGui::GetColorU32(ImVec4(0, 0, 0, 0.65f * (float)a));
        const ImU32 bodyCol   = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.88f * (float)a));

        const ImVec2 titlePos(tl.x + em(10.0f) + em(3.0f), tl.y + padY);
        dl->AddText(ImVec2(titlePos.x + 1, titlePos.y + 1), shadowCol, t.n.title.c_str());
        dl->AddText(titlePos, textCol, t.n.title.c_str());

        if (hasBody) {
            const ImVec2 bodyPos(titlePos.x, titlePos.y + titleH + em(2.0f));
            dl->AddText(bodyPos, bodyCol, t.n.body.c_str());
        }

        // Close glyph (top-right). Hit-tested by the whole-toast click above.
        const float xClose = br.x - em(14.0f);
        const float yClose = tl.y + em(6.0f);
        dl->AddText(ImVec2(xClose, yClose), bodyCol, "x");

        yCursor += height + kSpacing;

        // If past the timeline end, mark as dismissed so we cull below.
        if (age >= total) t.dismissed = true;
    }

    // Cull dismissed toasts.
    s_visible.erase(
        std::remove_if(s_visible.begin(), s_visible.end(),
                       [](const VisibleToast& t) { return t.dismissed; }),
        s_visible.end());
}

}   // namespace ui
