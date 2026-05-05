#pragma once

#include "imgui.h"
#include "implot.h"
#include <vector>

// ============================================================================
// Shared candlestick-chart rendering helpers.
// Used by both ChartWindow and ReplayWindow so they render identically.
// Pure ImGui/ImPlot drawing — no business logic, no IB dependency.
// ============================================================================

inline void RenderCandlestickBodies(const std::vector<double>& idxs,
                                    const std::vector<double>& opens,
                                    const std::vector<double>& highs,
                                    const std::vector<double>& lows,
                                    const std::vector<double>& closes,
                                    int sessionFirstIdx,
                                    int sessionLastIdx,
                                    int visibleCount = -1) {
    int n = static_cast<int>(idxs.size());
    if (n == 0) return;
    // visibleCount < 0 → render all bars; otherwise render [0, visibleCount-1]
    int end = (visibleCount < 0 || visibleCount > n) ? n : visibleCount;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    if (!dl) return;

    ImPlot::PushPlotClipRect();

    static constexpr double kHalf = 0.4;

    for (int i = 0; i < end; ++i) {
        bool   bull  = closes[i] >= opens[i];
        double bodyH = std::max(opens[i], closes[i]);
        double bodyL = std::min(opens[i], closes[i]);

        ImVec2 topL  = ImPlot::PlotToPixels(idxs[i] - kHalf, bodyH);
        ImVec2 botR  = ImPlot::PlotToPixels(idxs[i] + kHalf, bodyL);
        ImVec2 wHigh = ImPlot::PlotToPixels(idxs[i], highs[i]);
        ImVec2 wLow  = ImPlot::PlotToPixels(idxs[i], lows[i]);
        float  midX  = (topL.x + botR.x) * 0.5f;

        ImU32 col    = bull ? IM_COL32(52, 211, 100, 255) : IM_COL32(220, 60, 60, 255);
        ImU32 colDim = bull ? IM_COL32(30, 140, 60, 255)  : IM_COL32(160, 30, 30, 255);

        // Dim bars outside the active session range
        if (i < sessionFirstIdx || i > sessionLastIdx) {
            col    = bull ? IM_COL32(40, 160, 90, 180)  : IM_COL32(160, 50, 50, 180);
            colDim = bull ? IM_COL32(25, 100, 55, 160) : IM_COL32(110, 30, 30, 160);
        }

        // Wick
        dl->AddLine(ImVec2(midX, wHigh.y), ImVec2(midX, wLow.y), colDim, 1.0f);

        // Body
        float bh = std::abs(botR.y - topL.y);
        if (bh < 1.5f)
            dl->AddLine(ImVec2(topL.x, topL.y), ImVec2(botR.x, topL.y), col, 1.5f);
        else {
            dl->AddRectFilled(topL, botR, col);
            dl->AddRect(topL, botR, colDim, 0.f, 0, 0.5f);
        }
    }

    ImPlot::PopPlotClipRect();
}

// Vertical marker line at the given bar index
inline void RenderCursorLine(int barIdx,
                             const std::vector<double>& idxs,
                             const std::vector<double>& highs,
                             const std::vector<double>& lows) {
    int n = static_cast<int>(idxs.size());
    if (barIdx < 0 || barIdx >= n) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    if (!dl) return;

    ImPlot::PushPlotClipRect();

    ImVec2 top    = ImPlot::PlotToPixels(idxs[barIdx], highs[barIdx]);
    ImVec2 bottom = ImPlot::PlotToPixels(idxs[barIdx], lows[barIdx]);

    dl->AddLine(ImVec2(top.x, top.y), ImVec2(bottom.x, bottom.y),
                IM_COL32(255, 255, 100, 200), 2.0f);

    ImPlot::PopPlotClipRect();
}
