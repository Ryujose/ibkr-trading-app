#pragma once

#include "imgui.h"
#include "core/models/OrderData.h"
#include <unordered_map>
#include <vector>
#include <functional>
#include <string>

namespace ui {

// ============================================================================
// OrdersWindow — Live order blotter.
//
// Two tabs:
//   Open    — Submitted / Working / Partially filled orders with Cancel button.
//   History — Filled and Cancelled orders.
//
// Data fed by:
//   OnOpenOrder()       — from IBKRClient::openOrder (full detail on submit/reqOpenOrders)
//   OnOrderStatus()     — status+filled+avgPrice updates
//   OnFill()            — actual commission after execution
// ============================================================================
class OrdersWindow {
public:
    OrdersWindow();
    bool Render();   // returns false when window is closed
    bool& open() { return m_open; }

    // ── Data push-ins ─────────────────────────────────────────────────────
    void OnOpenOrder(const core::Order& order);
    void OnOrderStatus(int orderId, core::OrderStatus status,
                       double filled, double avgPrice,
                       const std::string& reason = {});
    void OnFill(const core::Fill& fill);
    void OnQueriedFill(const core::Fill& fill);   // from filtered reqExecutions

    // ── Callbacks wired by main.cpp ───────────────────────────────────────
    std::function<void(int orderId)> OnCancelOrder;
    std::function<void()>            OnRefresh;    // calls ReqOpenOrders
    // Filter toolbar "Load" → calls ReqExecutions(8001, sym, side, dateFrom)
    std::function<void(const std::string& sym, const std::string& side,
                       const std::string& dateFrom)> OnLoadHistory;

private:
    bool m_open    = true;
    int  m_activeTab = 0;   // 0 = Open, 1 = History

    std::unordered_map<int, core::Order> m_orders;   // orderId → Order

    // ── Queried fills (from filtered reqExecutions) ───────────────────────
    std::vector<core::Fill> m_queriedFills;

    // ── History tab filter state ──────────────────────────────────────────
    char m_filterSymbol[16] = "";
    int  m_filterSideIdx    = 0;   // 0=All 1=BUY 2=SELL
    char m_filterDate[12]   = "";  // YYYYMMDD or empty

    void DrawOpenTab();
    void DrawHistoryTab();
    void DrawOrderRow(core::Order& o, bool showCancel);
    void DrawQueriedFillRow(const core::Fill& f);

    static ImVec4 StatusColor(core::OrderStatus s);
    static bool   IsTerminal(core::OrderStatus s);
};

}  // namespace ui
