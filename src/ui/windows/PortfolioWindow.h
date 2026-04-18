#pragma once

#include "core/models/PortfolioData.h"
#include "imgui.h"
#include <vector>
#include <string>

namespace ui {

// ============================================================================
// PortfolioWindow  — Task #8
//
// Layout:
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  [Summary cards: Net Liq | Cash | Day P&L | Total P&L | Buying Power]  │
//  ├──────────────────────────────────────┬──────────────────────────────────┤
//  │  Positions table (sortable)          │  Equity Curve (90-day ImPlot)    │
//  │  Symbol│Qty│AvgCost│Price│MktVal│P&L │──────────────────────────────────│
//  │  AAPL  │100│181.50 │...  │...   │... │  Allocation donut (ImDrawList)   │
//  │  …                                   │  + legend                        │
//  ├──────────────────────────────────────┴──────────────────────────────────┤
//  │  [Trade History] [Performance] [Risk]   tabs                            │
//  └─────────────────────────────────────────────────────────────────────────┘
//
// IB Gateway stubs (wire into EWrapper):
//   updateAccountValue()   → OnAccountValue()
//   updatePortfolio()      → OnPositionUpdate()
//   execDetails()          → OnTradeExecuted()
//   accountDownloadEnd()   → OnAccountEnd()
// ============================================================================

class PortfolioWindow {
public:
    PortfolioWindow();
    ~PortfolioWindow() = default;

    // Call once per frame. Returns false when window is closed.
    bool Render();
    bool& open() { return m_open; }

    // --- IB Gateway callbacks (future integration) ---
    void OnAccountValue(const std::string& key, const std::string& val,
                        const std::string& currency, const std::string& accountName);
    // Called by main.cpp with the reliable base currency from reqAccountSummary.
    void SetBaseCurrency(const std::string& currency) { m_account.baseCurrency = currency; }
    void OnPositionUpdate(const core::Position& pos);
    void OnTradeExecuted(const core::TradeRecord& trade);
    void OnAccountEnd();

    // Real-time P&L from reqPnL / reqPnLSingle (supersedes updateAccountValue values).
    void OnPnL(double daily, double unrealized, double realized);
    void OnPnLSingle(int reqId, const std::string& symbol, double daily);

private:
    // ---- Window state -------------------------------------------------------
    bool m_open = true;

    // ---- Account data -------------------------------------------------------
    core::AccountValues              m_account;
    std::vector<core::Position>      m_positions;
    std::vector<core::TradeRecord>   m_trades;
    std::vector<core::EquityPoint>   m_equityCurve;
    core::PerformanceMetrics         m_perf;

    // ---- Positions sort state -----------------------------------------------
    core::PositionColumn m_sortCol       = core::PositionColumn::MarketValue;
    bool                 m_sortAscending = false;
    int                  m_selectedPos   = -1;

    // ---- Column visibility --------------------------------------------------
    bool m_showDesc      = false;
    bool m_showAvgCost   = true;
    bool m_showCostBasis = false;
    bool m_showRealPnL   = true;
    bool m_showDayPnL    = true;
    bool m_showDayChg    = true;
    bool m_showWeight    = true;

    // ---- Bottom tab ---------------------------------------------------------
    int m_activeTab = 0;   // 0=History 1=Performance 2=Risk

    // ---- Trade history filter -----------------------------------------------
    char m_tradeFilterBuf[32] = "";

    // ---- Sub-renderers ------------------------------------------------------
    void DrawSummaryCards();
    void DrawMainArea();
    void DrawPositionsTable();
    void DrawSideCharts();
    void DrawEquityCurve();
    void DrawAllocationDonut();
    void DrawBottomTabs();
    void DrawTradeHistory();
    void DrawPerformanceTab();
    void DrawRiskTab();
    void DrawColumnChooserPopup();

    // ---- Helpers ------------------------------------------------------------
    void SortPositions();
    void RecalcAccountTotals();
    void RecalcPerformanceMetrics();

    // ---- Formatting ---------------------------------------------------------
    static std::string FmtDollar(double v, bool sign = false);
    static std::string FmtPct(double v, bool sign = true);
    static std::string FmtShares(double v);
    static std::string FmtDate(std::time_t t);
    static std::string FmtDateTime(std::time_t t);
    static ImVec4      PnLColor(double v);

    // ---- Summary card helper ------------------------------------------------
    static void DrawSummaryCard(const char* label, const char* value,
                                const char* subvalue, ImVec4 valueColor,
                                float width, float height);
};

}  // namespace ui
