#include "ui/windows/OrdersWindow.h"
#include "imgui.h"
#include <ctime>
#include <cstring>

namespace ui {

// ============================================================================
OrdersWindow::OrdersWindow() {}

// ============================================================================
// Data push-ins
// ============================================================================
void OrdersWindow::OnOpenOrder(const core::Order& order) {
    auto it = m_orders.find(order.orderId);
    if (it == m_orders.end()) {
        m_orders[order.orderId] = order;
    } else {
        // Preserve commission and fill info already received from fills/status
        core::Order& existing = it->second;
        existing.symbol     = order.symbol;
        existing.side       = order.side;
        existing.type       = order.type;
        existing.tif        = order.tif;
        existing.quantity   = order.quantity;
        existing.limitPrice = order.limitPrice;
        existing.stopPrice  = order.stopPrice;
        // Only update status/commission if not already terminal from a fill
        if (!IsTerminal(existing.status))
            existing.status = order.status;
        if (existing.commission == 0.0 && order.commission != 0.0)
            existing.commission = order.commission;
        existing.updatedAt = order.updatedAt;
    }
}

void OrdersWindow::OnOrderStatus(int orderId, core::OrderStatus status,
                                  double filled, double avgPrice,
                                  const std::string& reason) {
    auto it = m_orders.find(orderId);
    if (it == m_orders.end()) {
        // Status arrived before openOrder — create a skeleton entry
        core::Order o;
        o.orderId      = orderId;
        o.status       = status;
        o.filledQty    = filled;
        o.avgFillPrice = avgPrice;
        o.updatedAt    = std::time(nullptr);
        if (!reason.empty()) o.rejectReason = reason;
        m_orders[orderId] = o;
    } else {
        it->second.status       = status;
        it->second.filledQty    = filled;
        it->second.avgFillPrice = avgPrice;
        it->second.updatedAt    = std::time(nullptr);
        if (!reason.empty()) it->second.rejectReason = reason;
    }
}

void OrdersWindow::OnFill(const core::Fill& fill) {
    auto it = m_orders.find(fill.orderId);
    if (it != m_orders.end()) {
        it->second.commission += fill.commission;
    }
}

// ============================================================================
// Render
// ============================================================================
bool OrdersWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(880, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Orders", &m_open, ImGuiWindowFlags_NoFocusOnAppearing)) { ImGui::End(); return m_open; }

    // ── Header bar ────────────────────────────────────────────────────────
    int nOpen = 0, nHistory = 0;
    for (const auto& [id, o] : m_orders)
        (IsTerminal(o.status) ? nHistory : nOpen)++;

    ImGui::Text("Open: %d  |  History: %d", nOpen, nHistory);
    ImGui::SameLine(0, 20);
    if (ImGui::Button("Refresh"))
        if (OnRefresh) OnRefresh();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-request open orders from IB");

    ImGui::Separator();

    if (ImGui::BeginTabBar("##orderstabs")) {
        if (ImGui::BeginTabItem("Open")) {
            m_activeTab = 0;
            DrawOpenTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("History")) {
            m_activeTab = 1;
            DrawHistoryTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    return m_open;
}

// ============================================================================
// Tabs
// ============================================================================
void OrdersWindow::DrawOpenTab() {
    bool anyOpen = false;
    for (const auto& [id, o] : m_orders)
        if (!IsTerminal(o.status)) { anyOpen = true; break; }

    if (!anyOpen) {
        ImGui::Spacing();
        ImGui::TextDisabled("  No open orders.");
        return;
    }

    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##open", 10, flags, ImVec2(-1, -1))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed,  55);
    ImGui::TableSetupColumn("Symbol",   ImGuiTableColumnFlags_WidthFixed,  70);
    ImGui::TableSetupColumn("Side",     ImGuiTableColumnFlags_WidthFixed,  45);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed,  65);
    ImGui::TableSetupColumn("Qty",      ImGuiTableColumnFlags_WidthFixed,  60);
    ImGui::TableSetupColumn("Filled",   ImGuiTableColumnFlags_WidthFixed,  60);
    ImGui::TableSetupColumn("Limit $",  ImGuiTableColumnFlags_WidthFixed,  75);
    ImGui::TableSetupColumn("Avg Fill", ImGuiTableColumnFlags_WidthFixed,  75);
    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthFixed,  65);
    ImGui::TableHeadersRow();

    for (auto& [id, o] : m_orders) {
        if (IsTerminal(o.status)) continue;
        DrawOrderRow(o, true);
    }
    ImGui::EndTable();
}

void OrdersWindow::DrawHistoryTab() {
    bool anyHistory = false;
    for (const auto& [id, o] : m_orders)
        if (IsTerminal(o.status)) { anyHistory = true; break; }

    if (!anyHistory) {
        ImGui::Spacing();
        ImGui::TextDisabled("  No order history yet.");
        return;
    }

    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##history", 10, flags, ImVec2(-1, -1))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed,  55);
    ImGui::TableSetupColumn("Symbol",   ImGuiTableColumnFlags_WidthFixed,  70);
    ImGui::TableSetupColumn("Side",     ImGuiTableColumnFlags_WidthFixed,  45);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed,  65);
    ImGui::TableSetupColumn("Qty",      ImGuiTableColumnFlags_WidthFixed,  60);
    ImGui::TableSetupColumn("Filled",   ImGuiTableColumnFlags_WidthFixed,  60);
    ImGui::TableSetupColumn("Limit $",  ImGuiTableColumnFlags_WidthFixed,  75);
    ImGui::TableSetupColumn("Avg Fill", ImGuiTableColumnFlags_WidthFixed,  75);
    ImGui::TableSetupColumn("Comm $",   ImGuiTableColumnFlags_WidthFixed,  70);
    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (auto& [id, o] : m_orders) {
        if (!IsTerminal(o.status)) continue;
        DrawOrderRow(o, false);
    }
    ImGui::EndTable();
}

// ============================================================================
// Single order row
// ============================================================================
void OrdersWindow::DrawOrderRow(core::Order& o, bool showCancel) {
    ImGui::TableNextRow();
    ImGui::PushID(o.orderId);

    ImVec4 statusCol = StatusColor(o.status);

    // ID
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%d", o.orderId);

    // Symbol
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(o.symbol.c_str());

    // Side
    ImGui::TableSetColumnIndex(2);
    bool isBuy = (o.side == core::OrderSide::Buy);
    ImGui::PushStyleColor(ImGuiCol_Text,
        isBuy ? ImVec4(0.2f, 0.9f, 0.4f, 1.f) : ImVec4(0.95f, 0.3f, 0.3f, 1.f));
    ImGui::TextUnformatted(core::OrderSideStr(o.side));
    ImGui::PopStyleColor();

    // Type
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(core::OrderTypeStr(o.type));

    // Qty
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%.0f", o.quantity);

    // Filled
    ImGui::TableSetColumnIndex(5);
    if (o.filledQty > 0.0)
        ImGui::Text("%.0f", o.filledQty);
    else
        ImGui::TextDisabled("—");

    // Limit price
    ImGui::TableSetColumnIndex(6);
    if (o.limitPrice > 0.0)
        ImGui::Text("$%.2f", o.limitPrice);
    else
        ImGui::TextDisabled("MKT");

    // Avg fill
    ImGui::TableSetColumnIndex(7);
    if (o.avgFillPrice > 0.0)
        ImGui::Text("$%.2f", o.avgFillPrice);
    else
        ImGui::TextDisabled("—");

    // Commission (history tab) or Status (open tab)
    if (!showCancel) {
        // History: commission column
        ImGui::TableSetColumnIndex(8);
        if (o.commission > 0.0)
            ImGui::Text("-$%.2f", o.commission);
        else
            ImGui::TextDisabled("—");
        // Status
        ImGui::TableSetColumnIndex(9);
    } else {
        ImGui::TableSetColumnIndex(8);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, statusCol);
    ImGui::TextUnformatted(core::OrderStatusStr(o.status));
    ImGui::PopStyleColor();
    if (!o.rejectReason.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", o.rejectReason.c_str());

    // Cancel button (open tab only)
    if (showCancel) {
        ImGui::TableSetColumnIndex(9);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.10f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
        if (ImGui::SmallButton("Cancel") && OnCancelOrder)
            OnCancelOrder(o.orderId);
        ImGui::PopStyleColor(2);
    }

    ImGui::PopID();
}

// ============================================================================
// Helpers
// ============================================================================
ImVec4 OrdersWindow::StatusColor(core::OrderStatus s) {
    switch (s) {
        case core::OrderStatus::Pending:
        case core::OrderStatus::Working:     return ImVec4(1.0f, 0.85f, 0.20f, 1.0f);
        case core::OrderStatus::PartialFill: return ImVec4(1.0f, 0.60f, 0.10f, 1.0f);
        case core::OrderStatus::Filled:      return ImVec4(0.20f, 0.90f, 0.40f, 1.0f);
        case core::OrderStatus::Cancelled:   return ImVec4(0.55f, 0.55f, 0.58f, 1.0f);
        case core::OrderStatus::Rejected:    return ImVec4(0.90f, 0.25f, 0.25f, 1.0f);
        default:                             return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    }
}

bool OrdersWindow::IsTerminal(core::OrderStatus s) {
    return s == core::OrderStatus::Filled   ||
           s == core::OrderStatus::Cancelled ||
           s == core::OrderStatus::Rejected;
}

}  // namespace ui
