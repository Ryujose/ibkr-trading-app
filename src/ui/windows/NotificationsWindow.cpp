#include "ui/windows/NotificationsWindow.h"

#include "core/services/NotificationService.h"

#include "imgui.h"

#include <cstdio>
#include <ctime>

namespace ui {

namespace {

const char* SeverityLabel(core::services::NotificationSeverity s) {
    using S = core::services::NotificationSeverity;
    switch (s) {
        case S::Success: return "OK";
        case S::Info:    return "INFO";
        case S::Warning: return "WARN";
        case S::Error:   return "ERR";
    }
    return "?";
}

ImVec4 SeverityColor(core::services::NotificationSeverity s) {
    using S = core::services::NotificationSeverity;
    switch (s) {
        case S::Success: return ImVec4(0.30f, 0.85f, 0.40f, 1.0f);   // green
        case S::Info:    return ImVec4(0.25f, 0.77f, 1.00f, 1.0f);   // cyan
        case S::Warning: return ImVec4(1.00f, 0.69f, 0.18f, 1.0f);   // amber
        case S::Error:   return ImVec4(1.00f, 0.32f, 0.32f, 1.0f);   // red
    }
    return ImVec4(1, 1, 1, 1);
}

const char* CategoryLabel(core::services::NotificationCategory c) {
    using C = core::services::NotificationCategory;
    switch (c) {
        case C::Orders:     return "Orders";
        case C::Connection: return "Connection";
        case C::Signals:    return "Signals";
        case C::System:     return "System";
    }
    return "?";
}

}   // namespace

bool NotificationsWindow::Render(core::services::NotificationService& svc) {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(720, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Notifications###notif_history", &m_open,
                      ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return true;
    }

    auto items = svc.history(500);

    // Toolbar
    ImGui::Text("%d events", (int)items.size());
    ImGui::SameLine(0, 16);
    if (ImGui::Button("Clear##notif_clear")) {
        svc.clearHistory();
        ImGui::End();
        return true;
    }
    ImGui::SameLine(0, 16);
    ImGui::Checkbox("Auto-scroll to newest##notif_auto", &m_autoScroll);

    ImGui::Separator();

    // Table — rendered newest-first (history() returns reverse-chronological).
    ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg      | ImGuiTableFlags_Borders     |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##notif_table", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time",      ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Severity",  ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Category",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Title",     ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Body",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (const auto& n : items) {
            ImGui::TableNextRow();

            // Time
            ImGui::TableSetColumnIndex(0);
            char tbuf[16] = "—";
            if (n.ts != 0) {
                std::tm lt{};
#if defined(_WIN32)
                localtime_s(&lt, &n.ts);
#else
                localtime_r(&n.ts, &lt);
#endif
                std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                              lt.tm_hour, lt.tm_min, lt.tm_sec);
            }
            ImGui::TextUnformatted(tbuf);

            // Severity
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(SeverityColor(n.severity), "%s",
                               SeverityLabel(n.severity));

            // Category
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(CategoryLabel(n.category));

            // Title
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(n.title.c_str());

            // Body — wrap-if-long, hover tooltip for the full string when truncated.
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(n.body.c_str());
            if (!n.body.empty() && ImGui::IsItemHovered() &&
                ImGui::CalcTextSize(n.body.c_str()).x >
                ImGui::GetContentRegionAvail().x)
            {
                ImGui::SetTooltip("%s", n.body.c_str());
            }
        }

        // Newest-first means the freshest entry is at the top — auto-scroll
        // pins to top so the user always sees the most recent event without
        // dragging the scrollbar.
        if (m_autoScroll) ImGui::SetScrollY(0.0f);

        ImGui::EndTable();
    }

    ImGui::End();
    return true;
}

}   // namespace ui
