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
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##open", 15, flags, ImVec2(-1, -1))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed,  52);
    ImGui::TableSetupColumn("Symbol",   ImGuiTableColumnFlags_WidthFixed,  68);
    ImGui::TableSetupColumn("Side",     ImGuiTableColumnFlags_WidthFixed,  42);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed,  72);
    ImGui::TableSetupColumn("Qty",      ImGuiTableColumnFlags_WidthFixed,  55);
    ImGui::TableSetupColumn("Price",    ImGuiTableColumnFlags_WidthFixed,  80);
    ImGui::TableSetupColumn("Aux",      ImGuiTableColumnFlags_WidthFixed,  78);
    ImGui::TableSetupColumn("TIF",      ImGuiTableColumnFlags_WidthFixed,  38);
    ImGui::TableSetupColumn("Ext",      ImGuiTableColumnFlags_WidthFixed,  30);
    ImGui::TableSetupColumn("Filled",   ImGuiTableColumnFlags_WidthFixed,  52);
    ImGui::TableSetupColumn("Avg $",    ImGuiTableColumnFlags_WidthFixed,  70);
    ImGui::TableSetupColumn("Comm $",   ImGuiTableColumnFlags_WidthFixed,  62);
    ImGui::TableSetupColumn("Time",     ImGuiTableColumnFlags_WidthFixed,  62);
    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthFixed,  58);
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
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##history", 15, flags, ImVec2(-1, -1))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID",       ImGuiTableColumnFlags_WidthFixed,  52);
    ImGui::TableSetupColumn("Symbol",   ImGuiTableColumnFlags_WidthFixed,  68);
    ImGui::TableSetupColumn("Side",     ImGuiTableColumnFlags_WidthFixed,  42);
    ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_WidthFixed,  72);
    ImGui::TableSetupColumn("Qty",      ImGuiTableColumnFlags_WidthFixed,  55);
    ImGui::TableSetupColumn("Price",    ImGuiTableColumnFlags_WidthFixed,  80);
    ImGui::TableSetupColumn("Aux",      ImGuiTableColumnFlags_WidthFixed,  78);
    ImGui::TableSetupColumn("TIF",      ImGuiTableColumnFlags_WidthFixed,  38);
    ImGui::TableSetupColumn("Ext",      ImGuiTableColumnFlags_WidthFixed,  30);
    ImGui::TableSetupColumn("Filled",   ImGuiTableColumnFlags_WidthFixed,  52);
    ImGui::TableSetupColumn("Avg $",    ImGuiTableColumnFlags_WidthFixed,  70);
    ImGui::TableSetupColumn("Comm $",   ImGuiTableColumnFlags_WidthFixed,  62);
    ImGui::TableSetupColumn("Updated",  ImGuiTableColumnFlags_WidthFixed,  62);
    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("Reject",   ImGuiTableColumnFlags_WidthFixed, 120);
    ImGui::TableHeadersRow();

    for (auto& [id, o] : m_orders) {
        if (!IsTerminal(o.status)) continue;
        DrawOrderRow(o, false);
    }
    ImGui::EndTable();
}

// ============================================================================
// Single order row
// Columns (0-14):
//   0 ID | 1 Symbol | 2 Side | 3 Type | 4 Qty | 5 Price | 6 Aux | 7 TIF |
//   8 Ext | 9 Filled | 10 Avg$ | 11 Comm$ | 12 Time | 13 Status |
//   14 Action(open=Cancel) / Reject reason(history)
// ============================================================================
void OrdersWindow::DrawOrderRow(core::Order& o, bool showCancel) {
    ImGui::TableNextRow();
    ImGui::PushID(o.orderId);

    // Row tint
    ImVec4 rowTint = (o.side == core::OrderSide::Buy)
        ? ImVec4(0.10f, 0.22f, 0.12f, 0.30f)
        : ImVec4(0.26f, 0.10f, 0.10f, 0.30f);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
        ImGui::ColorConvertFloat4ToU32(rowTint));

    // 0 — ID
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%d", o.orderId);

    // 1 — Symbol
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(o.symbol.c_str());

    // 2 — Side
    ImGui::TableSetColumnIndex(2);
    ImGui::PushStyleColor(ImGuiCol_Text,
        o.side == core::OrderSide::Buy
            ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
            : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::TextUnformatted(core::OrderSideStr(o.side));
    ImGui::PopStyleColor();

    // 3 — Type
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(core::OrderTypeStr(o.type));

    // 4 — Qty
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%.0f", o.quantity);

    // 5 — Price (main price per order type)
    ImGui::TableSetColumnIndex(5);
    switch (o.type) {
        case core::OrderType::Market:
        case core::OrderType::MOC:
        case core::OrderType::MTL:
            ImGui::TextDisabled("MKT");
            break;
        case core::OrderType::Limit:
        case core::OrderType::LOC:
            if (o.limitPrice > 0.0) ImGui::Text("$%.2f", o.limitPrice);
            else                    ImGui::TextDisabled("—");
            break;
        case core::OrderType::Stop:
        case core::OrderType::StopLimit:
            if (o.stopPrice > 0.0) ImGui::Text("stp $%.2f", o.stopPrice);
            else                   ImGui::TextDisabled("—");
            break;
        case core::OrderType::Trail:
        case core::OrderType::TrailLimit:
            if (o.trailingPercent > 0.0) ImGui::Text("tr %.2f%%", o.trailingPercent);
            else if (o.auxPrice  > 0.0)  ImGui::Text("tr $%.2f",  o.auxPrice);
            else                         ImGui::TextDisabled("—");
            break;
        case core::OrderType::MIT:
            if (o.auxPrice > 0.0) ImGui::Text("trig $%.2f", o.auxPrice);
            else                  ImGui::TextDisabled("—");
            break;
        case core::OrderType::LIT:
            if (o.auxPrice > 0.0) ImGui::Text("trig $%.2f", o.auxPrice);
            else                  ImGui::TextDisabled("—");
            break;
        case core::OrderType::Midprice:
            ImGui::TextDisabled("mid");
            break;
        case core::OrderType::Relative:
            if (o.auxPrice > 0.0) ImGui::Text("off $%.3f", o.auxPrice);
            else                  ImGui::TextDisabled("REL");
            break;
        default:
            ImGui::TextDisabled("—");
            break;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (o.limitPrice      > 0.0) ImGui::Text("Limit:    $%.4f", o.limitPrice);
        if (o.stopPrice       > 0.0) ImGui::Text("Stop:     $%.4f", o.stopPrice);
        if (o.auxPrice        > 0.0) ImGui::Text("Aux:      $%.4f", o.auxPrice);
        if (o.trailingPercent > 0.0) ImGui::Text("Trail %%: %.4f",  o.trailingPercent);
        if (o.trailStopPrice  > 0.0) ImGui::Text("Stop cap: $%.4f", o.trailStopPrice);
        if (o.lmtPriceOffset != 0.0) ImGui::Text("Lmt off:  $%.4f", o.lmtPriceOffset);
        ImGui::EndTooltip();
    }

    // 6 — Aux (secondary price for dual-leg / trail orders)
    ImGui::TableSetColumnIndex(6);
    switch (o.type) {
        case core::OrderType::StopLimit:
            if (o.limitPrice > 0.0) ImGui::Text("lmt $%.2f", o.limitPrice);
            else                    ImGui::TextDisabled("—");
            break;
        case core::OrderType::LIT:
            if (o.limitPrice > 0.0) ImGui::Text("lmt $%.2f", o.limitPrice);
            else                    ImGui::TextDisabled("—");
            break;
        case core::OrderType::TrailLimit:
            if (o.lmtPriceOffset != 0.0) ImGui::Text("off $%.3f", o.lmtPriceOffset);
            else                         ImGui::TextDisabled("—");
            if (o.trailStopPrice > 0.0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Stop cap: $%.2f", o.trailStopPrice);
            break;
        case core::OrderType::Trail:
            if (o.trailStopPrice > 0.0) ImGui::Text("cap $%.2f", o.trailStopPrice);
            else                        ImGui::TextDisabled("—");
            break;
        case core::OrderType::Midprice:
            if (o.limitPrice > 0.0) ImGui::Text("cap $%.2f", o.limitPrice);
            else                    ImGui::TextDisabled("—");
            break;
        default:
            ImGui::TextDisabled("—");
            break;
    }

    // 7 — TIF
    ImGui::TableSetColumnIndex(7);
    ImGui::TextUnformatted(core::TIFStr(o.tif));

    // 8 — Ext RTH
    ImGui::TableSetColumnIndex(8);
    if (o.outsideRth)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
    ImGui::TextUnformatted(o.outsideRth ? "Y" : "—");
    if (o.outsideRth) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Outside Regular Trading Hours allowed");
    }

    // 9 — Filled qty
    ImGui::TableSetColumnIndex(9);
    if (o.filledQty > 0.0)
        ImGui::Text("%.0f", o.filledQty);
    else
        ImGui::TextDisabled("—");

    // 10 — Avg fill price
    ImGui::TableSetColumnIndex(10);
    if (o.avgFillPrice > 0.0)
        ImGui::Text("$%.2f", o.avgFillPrice);
    else
        ImGui::TextDisabled("—");

    // 11 — Commission
    ImGui::TableSetColumnIndex(11);
    if (o.commission > 0.0)
        ImGui::Text("-$%.2f", o.commission);
    else
        ImGui::TextDisabled("—");

    // 12 — Time (submitted for open orders, updated for history)
    ImGui::TableSetColumnIndex(12);
    {
        std::time_t ts = showCancel ? o.submittedAt : o.updatedAt;
        if (ts != 0) {
            char tbuf[16];
            std::tm* lt = std::localtime(&ts);
            std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", lt);
            ImGui::TextDisabled("%s", tbuf);
        } else {
            ImGui::TextDisabled("—");
        }
    }

    // 13 — Status
    ImGui::TableSetColumnIndex(13);
    ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(o.status));
    ImGui::TextUnformatted(core::OrderStatusStr(o.status));
    ImGui::PopStyleColor();

    // 14 — Cancel (open) or Reject reason (history)
    ImGui::TableSetColumnIndex(14);
    if (showCancel) {
        bool isActive = !IsTerminal(o.status);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
            if (ImGui::SmallButton("Cancel") && OnCancelOrder)
                OnCancelOrder(o.orderId);
            ImGui::PopStyleColor(2);
        }
    } else {
        if (!o.rejectReason.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.35f, 0.35f, 1.0f));
            ImGui::TextUnformatted(o.rejectReason.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", o.rejectReason.c_str());
        } else {
            ImGui::TextDisabled("—");
        }
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
