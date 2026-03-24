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

    // ── Callbacks wired by main.cpp ───────────────────────────────────────
    std::function<void(int orderId)> OnCancelOrder;
    std::function<void()>            OnRefresh;    // calls ReqOpenOrders

private:
    bool m_open    = true;
    int  m_activeTab = 0;   // 0 = Open, 1 = History

    std::unordered_map<int, core::Order> m_orders;   // orderId → Order

    void DrawOpenTab();
    void DrawHistoryTab();
    void DrawOrderRow(core::Order& o, bool showCancel);

    static ImVec4 StatusColor(core::OrderStatus s);
    static bool   IsTerminal(core::OrderStatus s);
};

}  // namespace ui
