#pragma once

#include "imgui.h"
#include <string>
#include <cstdio>

namespace core {

// ============================================================================
// GroupState — active symbol for a linked group (id 1–4; 0 = no group)
// ============================================================================
struct GroupState {
    int         id     = 0;
    std::string symbol;
};

// ============================================================================
// WindowPreset — visibility + group assignment snapshot for all 6 windows
// ============================================================================
struct WindowPreset {
    const char* name = "";
    struct WinCfg {
        bool visible = true;
        int  groupId = 0;
    };
    WinCfg chart, trading, news, scanner, portfolio, orders;
};

// ============================================================================
// GroupColor — RGBA tint for group id 1–4 (grey for 0 / unassigned)
// ============================================================================
inline ImVec4 GroupColor(int groupId) {
    switch (groupId) {
        case 1: return {0.30f, 0.60f, 1.00f, 1.f};   // blue
        case 2: return {0.20f, 0.80f, 0.40f, 1.f};   // green
        case 3: return {1.00f, 0.65f, 0.20f, 1.f};   // orange
        case 4: return {0.85f, 0.30f, 0.85f, 1.f};   // purple
        default: return {0.45f, 0.45f, 0.45f, 1.f};  // grey (none)
    }
}

// ============================================================================
// DrawGroupPicker — colored dot SmallButton that opens an assignment popup.
//
// Usage (inside an ImGui window, after other toolbar items):
//   ImGui::SameLine(0, 8);
//   core::DrawGroupPicker(m_groupId, "##grp_picker");
//
// Modifies groupId in-place; returns true if the value was changed.
// ============================================================================
inline bool DrawGroupPicker(int& groupId, const char* popupId) {
    ImVec4 col = GroupColor(groupId);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(col.x, col.y, col.z, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x, col.y, col.z, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(col.x, col.y, col.z, 0.75f));
    const char* dotLabel = groupId > 0 ? "●##grp" : "○##grp";
    bool clicked = ImGui::SmallButton(dotLabel);
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(groupId == 0
            ? "No group — click to link"
            : "Group %d — click to change", groupId);
    if (clicked) ImGui::OpenPopup(popupId);

    bool changed = false;
    if (ImGui::BeginPopup(popupId)) {
        ImGui::TextDisabled("Link to group");
        ImGui::Separator();
        if (ImGui::Selectable("None", groupId == 0)) { groupId = 0; changed = true; }
        ImGui::Separator();
        for (int i = 1; i <= 4; i++) {
            ImVec4 c = GroupColor(i);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            char label[24];
            std::snprintf(label, sizeof(label), "● Group %d", i);
            if (ImGui::Selectable(label, groupId == i)) { groupId = i; changed = true; }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    return changed;
}

} // namespace core
