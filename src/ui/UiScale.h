#pragma once
#include "imgui.h"

// Convert a design-time pixel width (authored at 13 px base font) to a
// font-scale-aware value.
inline float em(float px) { return px * ImGui::GetFontSize() / 13.0f; }

// ============================================================================
// FlexRow — CSS flex-wrap: wrap equivalent for ImGui toolbars.
//
// Usage:
//   FlexRow row;
//   row.item(em(80));  ImGui::SetNextItemWidth(em(80));  // symbol input
//   row.item(em(55));  ImGui::SetNextItemWidth(em(55));  // timeframe combo
//   row.item(row.checkboxW("SMA20"));  ImGui::Checkbox("SMA20", &v);
//
// Call row.item(width) BEFORE each widget; it calls SameLine only when the
// item fits on the current line, otherwise it wraps to the next line.
// ============================================================================
struct FlexRow {
    float avail;
    float cursor = 0.0f;
    bool  started = false;

    FlexRow() : avail(ImGui::GetContentRegionAvail().x) {}

    void item(float itemW, float spacing = -1.0f) {
        float sp = (spacing < 0.0f) ? ImGui::GetStyle().ItemSpacing.x : spacing;
        if (started && cursor + sp + itemW <= avail) {
            ImGui::SameLine(0.0f, sp);
            cursor += sp + itemW;
        } else {
            cursor  = itemW;
            started = true;
        }
    }

    // Width estimators — use these to pass the right value to item().
    static float buttonW(const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }
    static float checkboxW(const char* label) {
        return ImGui::GetFrameHeight()
             + ImGui::GetStyle().ItemInnerSpacing.x
             + ImGui::CalcTextSize(label).x;
    }
    static float textW(const char* label) {
        return ImGui::CalcTextSize(label).x;
    }
};
