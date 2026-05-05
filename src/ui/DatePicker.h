#pragma once

#include "imgui.h"
#include "ui/UiScale.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui {

// ============================================================================
// Reusable date-picker popup. Shared by WshCalendarWindow and ReplayWindow.
// Usage:
//   static int navYear=0, navMonth=0;
//   char date[16] = "2026-04-15";
//   DrawDatePicker("Date", date, sizeof(date), navYear, navMonth);
// ============================================================================

namespace {
    inline int DaysInMonth(int year, int month) {
        static const int kDays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        if (month == 2) {
            bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            return leap ? 29 : 28;
        }
        return kDays[month];
    }

    inline int DayOfWeek1st(int year, int month) {
        struct tm t = {};
        t.tm_year = year - 1900;
        t.tm_mon  = month - 1;
        t.tm_mday = 1;
        mktime(&t);
        return t.tm_wday;
    }
}  // anonymous namespace

inline void DrawDatePicker(const char* label, char* buf, int bufSize,
                           int& navYear, int& navMonth,
                           const char* popupId = "##datepick") {
    char btnLabel[64];
    if (buf[0] != '\0')
        std::snprintf(btnLabel, sizeof(btnLabel), "%s: %s###dp_%s", label, buf, label);
    else
        std::snprintf(btnLabel, sizeof(btnLabel), "%s: Any###dp_%s", label, label);

    float btnW = em(100);
    if (ImGui::Button(btnLabel, ImVec2(btnW, 0)))
        ImGui::OpenPopup(popupId);

    ImGui::SetItemTooltip("%s", label);

    if (!ImGui::BeginPopup(popupId)) return;

    // Init nav to stored date or today
    if (buf[0] != '\0') {
        int sy = 0, sm = 0;
        std::sscanf(buf, "%d-%d", &sy, &sm);
        if (sy > 0 && sm >= 1 && sm <= 12) { navYear = sy; navMonth = sm; }
    } else if (navYear == 0) {
        std::time_t now = std::time(nullptr);
        struct tm* lt   = std::localtime(&now);
        navYear  = lt->tm_year + 1900;
        navMonth = lt->tm_mon + 1;
    }

    static const char* kMonths[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (ImGui::ArrowButton("##dpleft", ImGuiDir_Left)) {
        if (--navMonth < 1) { navMonth = 12; --navYear; }
    }
    ImGui::SameLine();
    ImGui::Text("%s %d", kMonths[navMonth - 1], navYear);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##dpright", ImGuiDir_Right)) {
        if (++navMonth > 12) { navMonth = 1; ++navYear; }
    }

    ImGui::Separator();

    static const char* kDow[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    for (int d = 0; d < 7; ++d) {
        if (d) ImGui::SameLine(0, em(2));
        ImGui::TextDisabled("%s", kDow[d]);
    }

    int firstDow  = DayOfWeek1st(navYear, navMonth);
    int daysInMon = DaysInMonth(navYear, navMonth);
    int col       = firstDow;
    float cellW   = em(26);
    float cellH   = em(20);

    if (col > 0)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + col * (cellW + em(2)));

    for (int day = 1; day <= daysInMon; ++day) {
        char dayId[16];
        std::snprintf(dayId, sizeof(dayId), "%d##dp%d%d", day, navYear, navMonth);

        char candidate[12];
        std::snprintf(candidate, sizeof(candidate), "%04d-%02d-%02d",
                      navYear, navMonth, day);
        bool selected = (std::strcmp(buf, candidate) == 0);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }

        if (ImGui::Button(dayId, ImVec2(cellW, cellH))) {
            std::snprintf(buf, bufSize, "%04d-%02d-%02d", navYear, navMonth, day);
            ImGui::CloseCurrentPopup();
        }
        if (selected) ImGui::PopStyleColor();

        ++col;
        if (col < 7) ImGui::SameLine(0, em(2));
        else         col = 0;
    }

    ImGui::Separator();
    if (ImGui::SmallButton("Clear")) {
        buf[0] = '\0';
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

}  // namespace ui
