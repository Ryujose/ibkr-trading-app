#include "PortfolioWindow.h"

#include "imgui.h"
#include "implot.h"

#define _USE_MATH_DEFINES
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstring>
#include <ctime>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace ui {

// ============================================================================
// Constructor
// ============================================================================

PortfolioWindow::PortfolioWindow()
    : m_rng(std::random_device{}())
{
    m_lastQuoteUpdate = Clock::now();
    SimulatePortfolio();
    SimulateEquityCurve();
    SimulateTradeHistory();
    SimulatePerformanceMetrics();
    RecalcAccountTotals();
    SortPositions();
}

// ============================================================================
// IB Gateway stubs
// ============================================================================

void PortfolioWindow::OnAccountValue(const std::string& key, const std::string& val,
                                     const std::string& /*currency*/,
                                     const std::string& /*accountName*/)
{
    m_hasRealData = true;
    double d = std::atof(val.c_str());
    if      (key == "NetLiquidation")    m_account.netLiquidation  = d;
    else if (key == "TotalCashValue")    m_account.totalCashValue  = d;
    else if (key == "BuyingPower")       m_account.buyingPower     = d;
    else if (key == "UnrealizedPnL")     m_account.unrealizedPnL   = d;
    else if (key == "RealizedPnL")       m_account.realizedPnL     = d;
    else if (key == "InitMarginReq")     m_account.initMarginReq   = d;
    else if (key == "MaintMarginReq")    m_account.maintMarginReq  = d;
    else if (key == "ExcessLiquidity")   m_account.excessLiquidity = d;
    m_account.updatedAt = std::time(nullptr);
}

void PortfolioWindow::OnPositionUpdate(const core::Position& pos)
{
    if (!m_hasRealData) {
        m_hasRealData = true;
        m_positions.clear();
    }
    for (auto& p : m_positions) {
        if (p.symbol == pos.symbol) { p = pos; return; }
    }
    m_positions.push_back(pos);
    RecalcAccountTotals();
    SortPositions();
}

void PortfolioWindow::OnTradeExecuted(const core::TradeRecord& trade)
{
    m_trades.insert(m_trades.begin(), trade);
}

void PortfolioWindow::OnAccountEnd()
{
    RecalcAccountTotals();
    SortPositions();
}

// ============================================================================
// Render
// ============================================================================

bool PortfolioWindow::Render()
{
    if (!m_open) return false;

    // Periodic quote drift — only when no real IB data
    if (!m_hasRealData) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - m_lastQuoteUpdate).count();
        if (dt >= m_quoteUpdateIntervalSec) {
            UpdateQuotes();
            m_lastQuoteUpdate = now;
        }
    }

    ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;

    if (!ImGui::Begin("Portfolio & Account", &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    DrawSummaryCards();
    ImGui::Separator();
    DrawMainArea();
    ImGui::Separator();
    DrawBottomTabs();

    ImGui::End();
    return m_open;
}

// ============================================================================
// DrawSummaryCards
// ============================================================================

void PortfolioWindow::DrawSummaryCards()
{
    float availW  = ImGui::GetContentRegionAvail().x;
    float cardH   = 62.f;
    float gap     = 8.f;
    int   nCards  = 6;
    float cardW   = (availW - gap * (nCards - 1)) / nCards;

    // Precompute display strings
    char netLiqVal[32], netLiqSub[32];
    std::snprintf(netLiqVal, sizeof(netLiqVal), "$%s",
                  FmtDollar(m_account.netLiquidation).c_str());
    std::snprintf(netLiqSub, sizeof(netLiqSub), "Net Liquidation");

    char cashVal[32], cashSub[32];
    std::snprintf(cashVal, sizeof(cashVal), "$%s",
                  FmtDollar(m_account.totalCashValue).c_str());
    std::snprintf(cashSub, sizeof(cashSub), "Cash Available");

    char dayPnlVal[32], dayPnlSub[32];
    std::snprintf(dayPnlVal, sizeof(dayPnlVal), "%s%s",
                  m_account.dayPnL >= 0 ? "+" : "",
                  FmtDollar(m_account.dayPnL, true).c_str());
    std::snprintf(dayPnlSub, sizeof(dayPnlSub), "%.2f%% today",
                  m_account.dayPnLPct);

    char uPnlVal[32], uPnlSub[32];
    std::snprintf(uPnlVal, sizeof(uPnlVal), "%s%s",
                  m_account.unrealizedPnL >= 0 ? "+" : "",
                  FmtDollar(m_account.unrealizedPnL, true).c_str());
    std::snprintf(uPnlSub, sizeof(uPnlSub), "Unrealized P&L");

    char rPnlVal[32], rPnlSub[32];
    std::snprintf(rPnlVal, sizeof(rPnlVal), "%s%s",
                  m_account.realizedPnL >= 0 ? "+" : "",
                  FmtDollar(m_account.realizedPnL, true).c_str());
    std::snprintf(rPnlSub, sizeof(rPnlSub), "Realized P&L");

    char bpVal[32], bpSub[32];
    std::snprintf(bpVal, sizeof(bpVal), "$%s",
                  FmtDollar(m_account.buyingPower).c_str());
    std::snprintf(bpSub, sizeof(bpSub), "Leverage: %.2fx", m_account.leverage);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 4));

    auto neutral = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    DrawSummaryCard("Net Liquidation", netLiqVal, netLiqSub, neutral,      cardW, cardH);
    ImGui::SameLine();
    DrawSummaryCard("Cash",           cashVal,   cashSub,   neutral,      cardW, cardH);
    ImGui::SameLine();
    DrawSummaryCard("Day P&L",        dayPnlVal, dayPnlSub, PnLColor(m_account.dayPnL),      cardW, cardH);
    ImGui::SameLine();
    DrawSummaryCard("Unrealized P&L", uPnlVal,   uPnlSub,   PnLColor(m_account.unrealizedPnL), cardW, cardH);
    ImGui::SameLine();
    DrawSummaryCard("Realized P&L",   rPnlVal,   rPnlSub,   PnLColor(m_account.realizedPnL),   cardW, cardH);
    ImGui::SameLine();
    DrawSummaryCard("Buying Power",   bpVal,     bpSub,     neutral,      cardW, cardH);

    ImGui::PopStyleVar();
}

void PortfolioWindow::DrawSummaryCard(const char* label, const char* value,
                                      const char* subvalue, ImVec4 valueColor,
                                      float width, float height)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.13f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.f);
    ImGui::BeginChild(label, ImVec2(width, height), true,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    ImGui::TextDisabled("%s", label);

    ImGui::PushStyleColor(ImGuiCol_Text, valueColor);
    ImGui::SetWindowFontScale(1.18f);
    ImGui::Text("%s", value);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::TextDisabled("%s", subvalue);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ============================================================================
// DrawMainArea  (positions left, charts right)
// ============================================================================

void PortfolioWindow::DrawMainArea()
{
    float totalH  = ImGui::GetContentRegionAvail().y - 180.f; // leave room for bottom tabs
    if (totalH < 120.f) totalH = 120.f;

    float leftW   = ImGui::GetContentRegionAvail().x * 0.60f;
    float rightW  = ImGui::GetContentRegionAvail().x - leftW - 8.f;

    // Left: positions table
    ImGui::BeginChild("##posLeft", ImVec2(leftW, totalH), false,
                       ImGuiWindowFlags_NoScrollbar);
    DrawPositionsTable();
    ImGui::EndChild();

    ImGui::SameLine(0, 8);

    // Right: charts
    ImGui::BeginChild("##chartsRight", ImVec2(rightW, totalH), false,
                       ImGuiWindowFlags_NoScrollbar);
    DrawSideCharts();
    ImGui::EndChild();
}

// ============================================================================
// DrawPositionsTable
// ============================================================================

void PortfolioWindow::DrawPositionsTable()
{
    // Toolbar above table
    ImGui::TextUnformatted("Positions");
    ImGui::SameLine();
    if (ImGui::Button("Cols")) ImGui::OpenPopup("##PosColChooser");
    DrawColumnChooserPopup();
    ImGui::SameLine();
    ImGui::TextDisabled("(%d)", static_cast<int>(m_positions.size()));

    // Count columns
    int colCount = 6; // Symbol, Qty, Price, MktVal, Unreal P&L, Unreal%
    if (m_showDesc)      ++colCount;
    if (m_showAvgCost)   ++colCount;
    if (m_showCostBasis) ++colCount;
    if (m_showRealPnL)   ++colCount;
    if (m_showDayChg)    ++colCount;
    if (m_showWeight)    ++colCount;

    float tableH = ImGui::GetContentRegionAvail().y;

    ImGuiTableFlags tflags =
        ImGuiTableFlags_ScrollY      |
        ImGuiTableFlags_RowBg        |
        ImGuiTableFlags_BordersOuter |
        ImGuiTableFlags_BordersV     |
        ImGuiTableFlags_Resizable    |
        ImGuiTableFlags_Sortable;

    if (!ImGui::BeginTable("##positions", colCount, tflags, ImVec2(0, tableH)))
        return;

    // Headers
    ImGui::TableSetupColumn("Symbol",     ImGuiTableColumnFlags_DefaultSort, 72.f);
    if (m_showDesc)      ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Qty",        0, 60.f);
    if (m_showAvgCost)   ImGui::TableSetupColumn("Avg Cost",    0, 72.f);
    ImGui::TableSetupColumn("Price",      0, 72.f);
    ImGui::TableSetupColumn("Mkt Value",  0, 88.f);
    if (m_showCostBasis) ImGui::TableSetupColumn("Cost Basis",  0, 88.f);
    ImGui::TableSetupColumn("Unreal P&L", 0, 88.f);
    ImGui::TableSetupColumn("Unreal %",   0, 68.f);
    if (m_showRealPnL)   ImGui::TableSetupColumn("Real P&L",    0, 88.f);
    if (m_showDayChg)    ImGui::TableSetupColumn("Day Chg%",    0, 68.f);
    if (m_showWeight)    ImGui::TableSetupColumn("Weight",      0, 58.f);

    ImGui::TableHeadersRow();

    // Sorting
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            // Map column index → PositionColumn (order must match header setup)
            static const core::PositionColumn kColMap[] = {
                core::PositionColumn::Symbol,
                core::PositionColumn::Quantity,
                core::PositionColumn::Price,
                core::PositionColumn::MarketValue,
                core::PositionColumn::UnrealizedPnL,
                core::PositionColumn::UnrealizedPct,
            };
            int ci = specs->Specs[0].ColumnIndex;
            if (ci < static_cast<int>(std::size(kColMap)))
                m_sortCol = kColMap[ci];
            m_sortAscending = (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
            SortPositions();
            specs->SpecsDirty = false;
        }
    }

    // Rows
    for (int i = 0; i < static_cast<int>(m_positions.size()); ++i) {
        const core::Position& p = m_positions[i];

        ImGui::TableNextRow();

        // Row color based on unrealized P&L
        if (i == m_selectedPos) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f,0.30f,0.50f,0.55f)));
        } else if (p.unrealizedPnL > 0) {
            float a = std::min(0.18f, (float)(p.unrealizedPnL / 2000.0) * 0.18f);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f, 0.28f, 0.0f, a)));
        } else if (p.unrealizedPnL < 0) {
            float a = std::min(0.18f, (float)(-p.unrealizedPnL / 2000.0) * 0.18f);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.28f, 0.0f, 0.0f, a)));
        }

        // Symbol (selectable)
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f,0.30f,0.50f,0.55f));
        bool sel = (i == m_selectedPos);
        if (ImGui::Selectable(p.symbol.c_str(), sel,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                ImVec2(0,0)))
            m_selectedPos = i;
        ImGui::PopStyleColor();

        int col = 1;

        if (m_showDesc) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextUnformatted(p.description.c_str());
        }

        // Qty
        ImGui::TableSetColumnIndex(col++);
        ImVec4 qtyC = p.quantity >= 0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                       : ImVec4(0.9f,0.3f,0.3f,1.f);
        ImGui::TextColored(qtyC, "%.0f", p.quantity);

        // Avg Cost
        if (m_showAvgCost) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::Text("%.2f", p.avgCost);
        }

        // Price
        ImGui::TableSetColumnIndex(col++);
        ImGui::Text("%.2f", p.marketPrice);

        // Market Value
        ImGui::TableSetColumnIndex(col++);
        ImGui::Text("$%s", FmtDollar(p.marketValue).c_str());

        // Cost Basis
        if (m_showCostBasis) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::Text("$%s", FmtDollar(p.costBasis).c_str());
        }

        // Unrealized P&L
        ImGui::TableSetColumnIndex(col++);
        ImGui::TextColored(PnLColor(p.unrealizedPnL), "%s$%s",
                           p.unrealizedPnL >= 0 ? "+" : "-",
                           FmtDollar(std::abs(p.unrealizedPnL)).c_str());

        // Unrealized %
        ImGui::TableSetColumnIndex(col++);
        ImGui::TextColored(PnLColor(p.unrealizedPct), "%+.2f%%", p.unrealizedPct);

        // Realized P&L
        if (m_showRealPnL) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextColored(PnLColor(p.realizedPnL), "%s$%s",
                               p.realizedPnL >= 0 ? "+" : "-",
                               FmtDollar(std::abs(p.realizedPnL)).c_str());
        }

        // Day Change %
        if (m_showDayChg) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextColored(PnLColor(p.dayChangePct), "%+.2f%%", p.dayChangePct);
        }

        // Portfolio Weight
        if (m_showWeight) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::Text("%.1f%%", p.portfolioWeight * 100.0);
        }
    }

    ImGui::EndTable();
}

// ============================================================================
// DrawColumnChooserPopup
// ============================================================================

void PortfolioWindow::DrawColumnChooserPopup()
{
    if (!ImGui::BeginPopup("##PosColChooser")) return;
    ImGui::TextUnformatted("Visible Columns");
    ImGui::Separator();
    ImGui::Checkbox("Description",  &m_showDesc);
    ImGui::Checkbox("Avg Cost",     &m_showAvgCost);
    ImGui::Checkbox("Cost Basis",   &m_showCostBasis);
    ImGui::Checkbox("Realized P&L", &m_showRealPnL);
    ImGui::Checkbox("Day Chg %",    &m_showDayChg);
    ImGui::Checkbox("Weight",       &m_showWeight);
    ImGui::EndPopup();
}

// ============================================================================
// DrawSideCharts
// ============================================================================

void PortfolioWindow::DrawSideCharts()
{
    float totalH = ImGui::GetContentRegionAvail().y;
    float curveH = totalH * 0.55f;
    float donutH = totalH - curveH - 4.f;
    if (donutH < 60.f) donutH = 60.f;

    ImGui::BeginChild("##equityCurve", ImVec2(0, curveH), false,
                       ImGuiWindowFlags_NoScrollbar);
    DrawEquityCurve();
    ImGui::EndChild();

    ImGui::BeginChild("##alloc", ImVec2(0, donutH), false,
                       ImGuiWindowFlags_NoScrollbar);
    DrawAllocationDonut();
    ImGui::EndChild();
}

// ============================================================================
// DrawEquityCurve
// ============================================================================

void PortfolioWindow::DrawEquityCurve()
{
    if (m_equityCurve.empty()) return;

    int n = static_cast<int>(m_equityCurve.size());

    // Build arrays for ImPlot
    std::vector<double> xs(n), equity(n), cash(n), pos(n);
    for (int i = 0; i < n; ++i) {
        xs[i]     = static_cast<double>(m_equityCurve[i].date);
        equity[i] = m_equityCurve[i].equity;
        cash[i]   = m_equityCurve[i].cash;
        pos[i]    = m_equityCurve[i].positions;
    }

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(6, 6));
    ImPlotFlags pf = ImPlotFlags_NoMenus;
    ImPlotAxisFlags xaf = ImPlotAxisFlags_AutoFit;
    ImPlotAxisFlags yaf = ImPlotAxisFlags_AutoFit;

    float h = ImGui::GetContentRegionAvail().y;
    if (ImPlot::BeginPlot("Equity Curve (90d)", ImVec2(-1, h), pf)) {
        ImPlot::SetupAxes("Date", "USD", xaf, yaf);
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
        ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.0f");

        // Shaded area: positions stack
        ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(0.2f, 0.6f, 0.2f, 0.25f));
        ImPlot::PlotShaded("Positions", xs.data(), pos.data(), n, 0.0);
        ImPlot::PopStyleColor();

        // Shaded area: equity above positions (cash layer)
        ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(0.2f, 0.4f, 0.8f, 0.15f));
        ImPlot::PlotShaded("Cash", xs.data(), equity.data(), pos.data(), n);
        ImPlot::PopStyleColor();

        // Equity line
        ImPlot::PushStyleColor(ImPlotCol_Line, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 2.0f);
        ImPlot::PlotLine("Total Equity", xs.data(), equity.data(), n);
        ImPlot::PopStyleVar();
        ImPlot::PopStyleColor();

        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar();
}

// ============================================================================
// DrawAllocationDonut  (ImDrawList-based)
// ============================================================================

void PortfolioWindow::DrawAllocationDonut()
{
    if (m_positions.empty()) return;

    ImGui::TextDisabled("Portfolio Allocation");

    ImVec2 avail    = ImGui::GetContentRegionAvail();
    float  diameter = std::min(avail.x * 0.45f, avail.y - 4.f);
    if (diameter < 20.f) return;
    float radius    = diameter * 0.5f;
    float innerR    = radius * 0.52f;  // donut hole

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 center    = ImVec2(canvasPos.x + radius + 4, canvasPos.y + radius);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Palette
    static const ImVec4 kPalette[] = {
        {0.26f,0.63f,0.96f,1.f}, {0.18f,0.80f,0.44f,1.f},
        {0.95f,0.61f,0.07f,1.f}, {0.91f,0.30f,0.24f,1.f},
        {0.61f,0.35f,0.71f,1.f}, {0.17f,0.76f,0.76f,1.f},
        {0.90f,0.49f,0.13f,1.f}, {0.45f,0.47f,0.51f,1.f},
    };
    int nPal = static_cast<int>(std::size(kPalette));

    // Compute total weight
    double totalW = 0.0;
    for (auto& p : m_positions) totalW += std::abs(p.portfolioWeight);
    if (totalW < 1e-9) totalW = 1.0;

    // Draw segments
    float startAngle = -M_PI * 0.5f; // start at 12 o'clock
    int   hovered    = -1;
    ImVec2 mousePos  = ImGui::GetMousePos();
    float  mx        = mousePos.x - center.x;
    float  my        = mousePos.y - center.y;
    float  mouseDist = std::sqrt(mx*mx + my*my);
    float  mouseAngle = std::atan2(my, mx);

    for (int i = 0; i < static_cast<int>(m_positions.size()); ++i) {
        const core::Position& p = m_positions[i];
        float sweep = static_cast<float>(
            std::abs(p.portfolioWeight) / totalW * 2.0 * M_PI);

        float endAngle = startAngle + sweep;

        // Check hover
        if (mouseDist >= innerR && mouseDist <= radius + 4) {
            float a = mouseAngle;
            if (a < startAngle) a += 2.f * M_PI;
            float e = endAngle;
            if (e < startAngle) e += 2.f * M_PI;
            float s = startAngle;
            if (a >= s && a <= e) hovered = i;
        }

        ImVec4 col4 = kPalette[i % nPal];
        float  rOuter = (i == hovered) ? radius + 5 : radius;

        // Draw arc as filled segments (approximate with triangles)
        const int kSegs = std::max(4, static_cast<int>(sweep * 20.f));
        float dA = sweep / kSegs;
        for (int s = 0; s < kSegs; ++s) {
            float a0 = startAngle + s * dA;
            float a1 = a0 + dA;

            ImVec2 p0 = center;
            ImVec2 p1 = ImVec2(center.x + std::cos(a0) * innerR,
                               center.y + std::sin(a0) * innerR);
            ImVec2 p2 = ImVec2(center.x + std::cos(a1) * innerR,
                               center.y + std::sin(a1) * innerR);
            ImVec2 p3 = ImVec2(center.x + std::cos(a1) * rOuter,
                               center.y + std::sin(a1) * rOuter);
            ImVec2 p4 = ImVec2(center.x + std::cos(a0) * rOuter,
                               center.y + std::sin(a0) * rOuter);

            ImU32 c = ImGui::ColorConvertFloat4ToU32(col4);
            dl->AddQuadFilled(p1, p2, p3, p4, c);
            (void)p0;
        }

        // Gap line between segments
        dl->AddLine(
            ImVec2(center.x + std::cos(startAngle) * innerR,
                   center.y + std::sin(startAngle) * innerR),
            ImVec2(center.x + std::cos(startAngle) * radius,
                   center.y + std::sin(startAngle) * radius),
            IM_COL32(10,10,12,255), 1.5f);

        startAngle = endAngle;
    }

    // Center label
    {
        char buf1[32], buf2[32];
        std::snprintf(buf1, sizeof(buf1), "$%s",
                      FmtDollar(m_account.netLiquidation).c_str());
        std::snprintf(buf2, sizeof(buf2), "Net Liq");
        ImVec2 sz1 = ImGui::CalcTextSize(buf1);
        ImVec2 sz2 = ImGui::CalcTextSize(buf2);
        dl->AddText(ImVec2(center.x - sz1.x * 0.5f, center.y - sz1.y - 1),
                    IM_COL32(220,220,220,255), buf1);
        dl->AddText(ImVec2(center.x - sz2.x * 0.5f, center.y + 1),
                    IM_COL32(160,160,160,255), buf2);
    }

    // Hover tooltip
    if (hovered >= 0) {
        const core::Position& hp = m_positions[hovered];
        ImGui::BeginTooltip();
        ImGui::Text("%s — %.1f%%", hp.symbol.c_str(), hp.portfolioWeight * 100.0);
        ImGui::Text("Mkt Value: $%s", FmtDollar(hp.marketValue).c_str());
        ImGui::TextColored(PnLColor(hp.unrealizedPnL),
                           "Unreal P&L: %s$%s",
                           hp.unrealizedPnL >= 0 ? "+" : "-",
                           FmtDollar(std::abs(hp.unrealizedPnL)).c_str());
        ImGui::EndTooltip();
    }

    // Legend to the right of donut
    float legendX = canvasPos.x + diameter + 12.f;
    float legendY = canvasPos.y;
    float lineH   = ImGui::GetTextLineHeightWithSpacing();

    for (int i = 0; i < static_cast<int>(m_positions.size()); ++i) {
        const core::Position& p = m_positions[i];
        ImVec4 col4 = kPalette[i % nPal];
        ImU32  c    = ImGui::ColorConvertFloat4ToU32(col4);
        dl->AddRectFilled(ImVec2(legendX, legendY + 3),
                          ImVec2(legendX + 10, legendY + 13), c, 2.f);

        char legBuf[48];
        std::snprintf(legBuf, sizeof(legBuf), "%s  %.1f%%",
                      p.symbol.c_str(), p.portfolioWeight * 100.0);
        dl->AddText(ImVec2(legendX + 14, legendY),
                    i == hovered ? IM_COL32(255,255,180,255)
                                 : IM_COL32(200,200,200,255),
                    legBuf);
        legendY += lineH;
    }

    // Advance cursor past the donut
    ImGui::Dummy(ImVec2(avail.x, diameter + 4));
}

// ============================================================================
// DrawBottomTabs
// ============================================================================

void PortfolioWindow::DrawBottomTabs()
{
    float h = ImGui::GetContentRegionAvail().y;
    if (h < 20.f) return;

    if (ImGui::BeginTabBar("##portTabs")) {
        if (ImGui::BeginTabItem("Trade History")) {
            DrawTradeHistory();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance")) {
            DrawPerformanceTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Risk & Margin")) {
            DrawRiskTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ============================================================================
// DrawTradeHistory
// ============================================================================

void PortfolioWindow::DrawTradeHistory()
{
    // Filter bar
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##tradeFilter", "Filter symbol…",
                              m_tradeFilterBuf, sizeof(m_tradeFilterBuf));
    ImGui::SameLine();
    ImGui::TextDisabled("(%d trades)", static_cast<int>(m_trades.size()));

    float tableH = ImGui::GetContentRegionAvail().y;
    ImGuiTableFlags tf = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                         ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV;
    if (!ImGui::BeginTable("##tradeHist", 7, tf, ImVec2(0, tableH))) return;

    ImGui::TableSetupColumn("Date/Time", 0, 140.f);
    ImGui::TableSetupColumn("Symbol",    0,  70.f);
    ImGui::TableSetupColumn("Side",      0,  50.f);
    ImGui::TableSetupColumn("Qty",       0,  60.f);
    ImGui::TableSetupColumn("Price",     0,  72.f);
    ImGui::TableSetupColumn("Comm.",     0,  60.f);
    ImGui::TableSetupColumn("Real. P&L", 0,  88.f);
    ImGui::TableHeadersRow();

    for (auto& t : m_trades) {
        // Apply symbol filter
        if (m_tradeFilterBuf[0] != '\0') {
            std::string q = m_tradeFilterBuf, sym = t.symbol;
            auto ci = [](unsigned char c){ return static_cast<char>(std::toupper(c)); };
            std::transform(q.begin(), q.end(), q.begin(), ci);
            std::transform(sym.begin(), sym.end(), sym.begin(), ci);
            if (sym.find(q) == std::string::npos) continue;
        }

        ImGui::TableNextRow();
        bool isBuy = (t.side == "BUY");

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(FmtDateTime(t.executedAt).c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(t.symbol.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(isBuy ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                 : ImVec4(0.9f,0.3f,0.3f,1.f),
                           "%s", t.side.c_str());

        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%.0f", t.quantity);

        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%.2f", t.price);

        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%.2f", t.commission);

        ImGui::TableSetColumnIndex(6);
        if (t.realizedPnL != 0.0)
            ImGui::TextColored(PnLColor(t.realizedPnL), "%s$%.2f",
                               t.realizedPnL >= 0 ? "+" : "-",
                               std::abs(t.realizedPnL));
        else
            ImGui::TextDisabled("—");
    }

    ImGui::EndTable();
}

// ============================================================================
// DrawPerformanceTab
// ============================================================================

void PortfolioWindow::DrawPerformanceTab()
{
    const core::PerformanceMetrics& m = m_perf;
    float colW = 200.f;

    auto Metric = [&](const char* label, const char* val,
                      ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text)) {
        ImGui::TextDisabled("%-22s", label);
        ImGui::SameLine();
        ImGui::TextColored(col, "%s", val);
    };

    ImGui::Columns(3, "##perfcols", false);
    ImGui::SetColumnWidth(0, colW); ImGui::SetColumnWidth(1, colW);

    char buf[32];

    // Column 1: Returns
    ImGui::TextUnformatted("Returns");
    ImGui::Separator();
    std::snprintf(buf, sizeof(buf), "%+.2f%%", m.dayReturn);
    Metric("Day Return:",    buf, PnLColor(m.dayReturn));
    std::snprintf(buf, sizeof(buf), "%+.2f%%", m.mtdReturn);
    Metric("MTD Return:",    buf, PnLColor(m.mtdReturn));
    std::snprintf(buf, sizeof(buf), "%+.2f%%", m.ytdReturn);
    Metric("YTD Return:",    buf, PnLColor(m.ytdReturn));
    std::snprintf(buf, sizeof(buf), "%+.2f%%", m.totalReturn);
    Metric("Total Return:",  buf, PnLColor(m.totalReturn));

    ImGui::NextColumn();

    // Column 2: Risk
    ImGui::TextUnformatted("Risk Metrics");
    ImGui::Separator();
    std::snprintf(buf, sizeof(buf), "%.3f", m.sharpeRatio);
    Metric("Sharpe Ratio:",  buf, m.sharpeRatio >= 1.0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                                         : ImVec4(0.9f,0.6f,0.1f,1.f));
    std::snprintf(buf, sizeof(buf), "%.2f%%", m.maxDrawdown);
    Metric("Max Drawdown:",  buf, ImVec4(0.9f,0.3f,0.3f,1.f));
    std::snprintf(buf, sizeof(buf), "%.2f%%", m.volatility);
    Metric("Ann. Volatility:", buf);
    std::snprintf(buf, sizeof(buf), "%.3f / %.3f%%", m.beta, m.alpha);
    Metric("Beta / Alpha:",  buf);

    ImGui::NextColumn();

    // Column 3: Trade stats
    ImGui::TextUnformatted("Trade Statistics");
    ImGui::Separator();
    std::snprintf(buf, sizeof(buf), "%.1f%%", m.winRate);
    Metric("Win Rate:",      buf, m.winRate >= 50.0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                                     : ImVec4(0.9f,0.4f,0.4f,1.f));
    std::snprintf(buf, sizeof(buf), "$%.2f", m.avgWin);
    Metric("Avg Win:",       buf, ImVec4(0.3f,0.9f,0.3f,1.f));
    std::snprintf(buf, sizeof(buf), "$%.2f", m.avgLoss);
    Metric("Avg Loss:",      buf, ImVec4(0.9f,0.3f,0.3f,1.f));
    std::snprintf(buf, sizeof(buf), "%.2f", m.profitFactor);
    Metric("Profit Factor:", buf, m.profitFactor >= 1.5 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                                         : ImVec4(0.9f,0.6f,0.1f,1.f));

    ImGui::Columns(1);
}

// ============================================================================
// DrawRiskTab
// ============================================================================

void PortfolioWindow::DrawRiskTab()
{
    const core::AccountValues& a = m_account;

    auto Row = [](const char* label, const char* val, ImVec4 col = {}) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        if (col.w > 0) ImGui::TextColored(col, "%s", val);
        else           ImGui::TextUnformatted(val);
    };

    ImGuiTableFlags tf = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerH;
    if (!ImGui::BeginTable("##risk", 2, tf, ImVec2(380, 0))) return;
    ImGui::TableSetupColumn("Metric", 0, 200.f);
    ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    char buf[64];

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.netLiquidation).c_str());
    Row("Net Liquidation", buf);

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.totalCashValue).c_str());
    Row("Total Cash Value", buf);

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.grossPosValue).c_str());
    Row("Gross Position Value", buf);

    std::snprintf(buf, sizeof(buf), "%.2fx", a.leverage);
    ImVec4 levC = a.leverage > 2.0 ? ImVec4(0.9f,0.3f,0.3f,1.f)
                : a.leverage > 1.0 ? ImVec4(0.9f,0.7f,0.1f,1.f)
                :                    ImVec4(0.3f,0.9f,0.3f,1.f);
    Row("Leverage", buf, levC);

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.initMarginReq).c_str());
    Row("Initial Margin Req.", buf);

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.maintMarginReq).c_str());
    Row("Maintenance Margin Req.", buf);

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.excessLiquidity).c_str());
    ImVec4 exLiqC = a.excessLiquidity < a.maintMarginReq * 0.1
                    ? ImVec4(0.9f,0.3f,0.3f,1.f)
                    : ImVec4(0.3f,0.9f,0.3f,1.f);
    Row("Excess Liquidity", buf, exLiqC);

    std::snprintf(buf, sizeof(buf), "$%s", FmtDollar(a.buyingPower).c_str());
    Row("Buying Power", buf);

    ImGui::EndTable();
}

// ============================================================================
// RecalcAccountTotals
// ============================================================================

void PortfolioWindow::RecalcAccountTotals()
{
    double grossPos = 0.0;
    double unreal   = 0.0;
    double real     = 0.0;

    for (auto& p : m_positions) {
        p.marketValue   = p.quantity * p.marketPrice;
        p.costBasis     = p.quantity * p.avgCost;
        p.unrealizedPnL = p.marketValue - p.costBasis;
        p.unrealizedPct = std::abs(p.costBasis) > 1e-9
                          ? (p.unrealizedPnL / std::abs(p.costBasis)) * 100.0
                          : 0.0;
        grossPos += std::abs(p.marketValue);
        unreal   += p.unrealizedPnL;
        real     += p.realizedPnL;
    }

    m_account.unrealizedPnL = unreal;
    m_account.realizedPnL   = real;
    m_account.grossPosValue = grossPos;
    m_account.leverage      = m_account.netLiquidation > 1e-9
                              ? grossPos / m_account.netLiquidation
                              : 0.0;

    // Portfolio weights
    for (auto& p : m_positions)
        p.portfolioWeight = m_account.netLiquidation > 1e-9
                            ? std::abs(p.marketValue) / m_account.netLiquidation
                            : 0.0;
}

// ============================================================================
// SortPositions
// ============================================================================

void PortfolioWindow::SortPositions()
{
    bool asc = m_sortAscending;
    core::PositionColumn col = m_sortCol;

    std::stable_sort(m_positions.begin(), m_positions.end(),
        [col, asc](const core::Position& a, const core::Position& b) {
            double va = 0, vb = 0;
            std::string sa, sb;
            bool useStr = false;
            switch (col) {
                case core::PositionColumn::Symbol:       sa = a.symbol;        sb = b.symbol;        useStr = true; break;
                case core::PositionColumn::Description:  sa = a.description;   sb = b.description;   useStr = true; break;
                case core::PositionColumn::Quantity:     va = a.quantity;      vb = b.quantity;      break;
                case core::PositionColumn::AvgCost:      va = a.avgCost;       vb = b.avgCost;       break;
                case core::PositionColumn::Price:        va = a.marketPrice;   vb = b.marketPrice;   break;
                case core::PositionColumn::MarketValue:  va = std::abs(a.marketValue);  vb = std::abs(b.marketValue);  break;
                case core::PositionColumn::CostBasis:    va = std::abs(a.costBasis);    vb = std::abs(b.costBasis);    break;
                case core::PositionColumn::UnrealizedPnL:va = a.unrealizedPnL; vb = b.unrealizedPnL; break;
                case core::PositionColumn::UnrealizedPct:va = a.unrealizedPct; vb = b.unrealizedPct; break;
                case core::PositionColumn::RealizedPnL:  va = a.realizedPnL;   vb = b.realizedPnL;   break;
                case core::PositionColumn::DayChange:    va = a.dayChange;     vb = b.dayChange;     break;
                case core::PositionColumn::DayChangePct: va = a.dayChangePct;  vb = b.dayChangePct;  break;
                case core::PositionColumn::Weight:       va = a.portfolioWeight; vb = b.portfolioWeight; break;
            }
            if (useStr) return asc ? (sa < sb) : (sa > sb);
            return asc ? (va < vb) : (va > vb);
        });
}

// ============================================================================
// UpdateQuotes  (simulated live drift)
// ============================================================================

void PortfolioWindow::UpdateQuotes()
{
    for (auto& p : m_positions) {
        double d   = m_drift(m_rng);
        double prev = p.marketPrice;
        p.marketPrice += p.marketPrice * d;
        p.marketPrice  = std::round(p.marketPrice * 100.0) / 100.0;
        p.dayChange    += (p.marketPrice - prev);
        p.dayChangePct  = p.avgCost > 0
                          ? (p.marketPrice - p.avgCost) / p.avgCost * 100.0
                          : 0.0;
    }

    RecalcAccountTotals();

    // Also push last equity curve point forward
    if (!m_equityCurve.empty()) {
        m_equityCurve.back().equity    = m_account.netLiquidation;
        m_equityCurve.back().positions = m_account.grossPosValue;
    }

    // Day P&L vs previous close
    double prevNetLiq = m_equityCurve.size() >= 2
                        ? m_equityCurve[m_equityCurve.size()-2].equity
                        : m_account.netLiquidation;
    m_account.dayPnL    = m_account.netLiquidation - prevNetLiq;
    m_account.dayPnLPct = prevNetLiq > 1e-9
                          ? m_account.dayPnL / prevNetLiq * 100.0
                          : 0.0;
}

// ============================================================================
// Simulation helpers
// ============================================================================

void PortfolioWindow::SimulatePortfolio()
{
    // Seed account
    m_account.accountId      = "U1234567";
    m_account.accountType    = "INDIVIDUAL";
    m_account.baseCurrency   = "USD";
    m_account.totalCashValue = 42320.0;
    m_account.settledCash    = 40100.0;
    m_account.buyingPower    = 84640.0;        // 2x margin
    m_account.initMarginReq  = 18500.0;
    m_account.maintMarginReq = 14800.0;
    m_account.excessLiquidity = 95000.0;

    // Positions
    struct Seed {
        const char* sym; const char* desc; const char* cls;
        double qty; double avgCost; double price; double realPnL;
        double dayChgPct;
    };
    static const Seed kPos[] = {
        {"AAPL",  "Apple Inc.",          "STK",  100, 155.40, 184.20, 320.00,  1.34},
        {"MSFT",  "Microsoft Corp.",     "STK",   50, 380.20, 415.30, 820.00,  0.78},
        {"NVDA",  "NVIDIA Corp.",        "STK",   30, 640.00, 876.50, 520.00,  4.32},
        {"GOOGL", "Alphabet Inc.",       "STK",   20, 148.00, 172.63,  80.00,  1.48},
        {"TSLA",  "Tesla Inc.",          "STK",  -50, 270.00, 247.10, 460.00, -5.09},
        {"JPM",   "JPMorgan Chase",      "STK",   80, 195.00, 207.30, 120.00,  0.83},
        {"GLD",   "SPDR Gold Shares",    "ETF",   40, 215.00, 237.80,  90.00,  0.93},
        {"SPY",   "SPDR S&P 500 ETF",   "ETF",   25, 490.00, 530.50, 210.00,  0.52},
    };

    std::normal_distribution<double> startNoise{0.0, 0.002};
    for (const auto& s : kPos) {
        core::Position p;
        p.symbol      = s.sym;
        p.description = s.desc;
        p.assetClass  = s.cls;
        p.exchange    = "SMART";
        p.currency    = "USD";
        p.quantity    = s.qty;
        p.avgCost     = s.avgCost;
        p.marketPrice = s.price * (1.0 + startNoise(m_rng));
        p.marketValue = p.quantity * p.marketPrice;
        p.costBasis   = p.quantity * p.avgCost;
        p.unrealizedPnL = p.marketValue - p.costBasis;
        p.unrealizedPct = std::abs(p.costBasis) > 1e-9
                          ? p.unrealizedPnL / std::abs(p.costBasis) * 100.0 : 0.0;
        p.realizedPnL   = s.realPnL;
        p.dayChangePct  = s.dayChgPct;
        p.dayChange     = p.marketPrice * s.dayChgPct / 100.0;
        p.updatedAt     = std::time(nullptr);
        m_positions.push_back(std::move(p));
    }

    // Net liquidation = cash + market value of positions
    double totalMktVal = 0.0;
    for (auto& p : m_positions) totalMktVal += p.marketValue;
    m_account.netLiquidation = m_account.totalCashValue + totalMktVal;
    m_account.updatedAt = std::time(nullptr);
}

void PortfolioWindow::SimulateEquityCurve(int days)
{
    m_equityCurve.clear();
    m_equityCurve.reserve(days);

    // Start 'days' ago with ~80% of current equity
    double equity = m_account.netLiquidation * 0.82;
    double cash   = m_account.totalCashValue  * 0.75;

    std::normal_distribution<double> nd{0.0008, 0.011}; // slight upward drift

    std::time_t now = std::time(nullptr);
    // Go back 'days' days (86400 seconds each)
    std::time_t startT = now - (std::time_t)days * 86400;

    // Skip to next weekday
    struct tm* tmPtr;
    std::tm   tmBuf;

    for (int i = 0; i < days; ++i) {
        std::time_t t = startT + (std::time_t)i * 86400;
        tmPtr = std::localtime(&t);
        if (!tmPtr) continue;
        tmBuf = *tmPtr;
        int wday = tmBuf.tm_wday;
        if (wday == 0 || wday == 6) continue; // skip weekends

        double r   = nd(m_rng);
        equity    *= (1.0 + r);
        cash      *= (1.0 + r * 0.05); // cash grows slowly

        core::EquityPoint ep;
        ep.date      = t;
        ep.equity    = equity;
        ep.cash      = cash;
        ep.positions = equity - cash;
        m_equityCurve.push_back(ep);
    }

    // Force last point to current values
    if (!m_equityCurve.empty()) {
        m_equityCurve.back().equity    = m_account.netLiquidation;
        m_equityCurve.back().cash      = m_account.totalCashValue;
        m_equityCurve.back().positions = m_account.grossPosValue;
        m_equityCurve.back().date      = now;
    }
}

void PortfolioWindow::SimulateTradeHistory(int count)
{
    static const char* syms[]  = {"AAPL","MSFT","NVDA","TSLA","GOOGL","META","AMZN","JPM","SPY","GLD"};
    static const char* sides[] = {"BUY","SELL"};

    std::uniform_int_distribution<int> symDist(0, 9);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_real_distribution<double> qtyDist(10, 200);
    std::uniform_real_distribution<double> commDist(0.35, 4.50);
    std::uniform_real_distribution<double> pnlDist(-800, 1200);

    std::time_t now = std::time(nullptr);

    for (int i = 0; i < count; ++i) {
        core::TradeRecord t;
        t.tradeId    = 10000 + i;
        t.symbol     = syms[symDist(m_rng)];
        t.side       = sides[sideDist(m_rng)];
        t.quantity   = std::round(qtyDist(m_rng));
        t.price      = 100.0 + m_rng() % 800;
        t.commission = commDist(m_rng);
        t.realizedPnL = t.side == "SELL" ? pnlDist(m_rng) : 0.0;
        t.executedAt = now - (std::time_t)(i * 3600 + m_rng() % 1800);
        m_trades.push_back(std::move(t));
    }
}

void PortfolioWindow::SimulatePerformanceMetrics()
{
    m_perf.totalReturn  =  18.42;
    m_perf.ytdReturn    =   7.81;
    m_perf.mtdReturn    =   1.23;
    m_perf.dayReturn    =   m_account.dayPnLPct;
    m_perf.sharpeRatio  =   1.34;
    m_perf.maxDrawdown  = -12.80;
    m_perf.winRate      =  58.40;
    m_perf.avgWin       = 412.50;
    m_perf.avgLoss      = 278.30;
    m_perf.profitFactor =   1.72;
    m_perf.beta         =   0.91;
    m_perf.alpha        =   4.20;
    m_perf.volatility   =  14.30;
}

// ============================================================================
// Formatting helpers
// ============================================================================

std::string PortfolioWindow::FmtDollar(double v, bool sign)
{
    char buf[64];
    if (sign)
        std::snprintf(buf, sizeof(buf), "%+.2f", v);
    else {
        // Insert thousands separator manually
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%.2f", v);
        // Simple: just use snprintf with grouping via stringstream
        std::ostringstream oss;
        oss.imbue(std::locale("C"));
        oss << std::fixed << std::setprecision(2) << v;
        return oss.str();
    }
    return buf;
}

std::string PortfolioWindow::FmtPct(double v, bool sign)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), sign ? "%+.2f%%" : "%.2f%%", v);
    return buf;
}

std::string PortfolioWindow::FmtShares(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

std::string PortfolioWindow::FmtDate(std::time_t t)
{
    if (!t) return "--";
    std::tm* tm = std::localtime(&t);
    if (!tm) return "--";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
    return buf;
}

std::string PortfolioWindow::FmtDateTime(std::time_t t)
{
    if (!t) return "--";
    std::tm* tm = std::localtime(&t);
    if (!tm) return "--";
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    return buf;
}

ImVec4 PortfolioWindow::PnLColor(double v)
{
    if (v > 0) return ImVec4(0.3f, 0.9f, 0.3f, 1.f);
    if (v < 0) return ImVec4(0.9f, 0.3f, 0.3f, 1.f);
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

}  // namespace ui
