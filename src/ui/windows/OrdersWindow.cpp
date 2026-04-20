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

void OrdersWindow::OnQueriedFill(const core::Fill& fill) {
    // Deduplicate by execId so repeated Load calls don't stack duplicates
    for (const auto& f : m_queriedFills)
        if (f.execId == fill.execId) return;
    m_queriedFills.push_back(fill);
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
    // ── Filter toolbar ────────────────────────────────────────────────────
    ImGui::SetNextItemWidth(88);
    ImGui::InputText("##fsym", m_filterSymbol, sizeof(m_filterSymbol));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Symbol (empty = all)");
    ImGui::SameLine(0, 4);
    static constexpr const char* kSides[] = {"All", "BUY", "SELL"};
    ImGui::SetNextItemWidth(62);
    ImGui::Combo("##fside", &m_filterSideIdx, kSides, 3);
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(78);
    ImGui::InputText("##fdate", m_filterDate, sizeof(m_filterDate));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Date from: YYYYMMDD (empty = today)");
    ImGui::SameLine(0, 4);
    if (ImGui::Button("Load##hist")) {
        if (OnLoadHistory) {
            const char* sideStr = (m_filterSideIdx == 1) ? "BUY"
                                : (m_filterSideIdx == 2) ? "SELL" : "";
            OnLoadHistory(m_filterSymbol, sideStr, m_filterDate);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Query IB for filtered execution history");
    if (!m_queriedFills.empty()) {
        ImGui::SameLine(0, 8);
        if (ImGui::SmallButton("Clear##qf")) m_queriedFills.clear();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear queried results");
    }
    ImGui::Separator();

    bool anyHistory = false;
    for (const auto& [id, o] : m_orders)
        if (IsTerminal(o.status)) { anyHistory = true; break; }
    bool hasQueried = !m_queriedFills.empty();

    float availH = ImGui::GetContentRegionAvail().y;
    float liveH  = hasQueried ? availH * 0.55f : availH;

    // ── Live session history ──────────────────────────────────────────────
    if (!anyHistory) {
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild("##livehistory", ImVec2(-1, liveH));
        ImGui::Spacing();
        ImGui::TextDisabled("  No live order history.");
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        static ImGuiTableFlags flags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_SizingFixedFit;

        if (ImGui::BeginTable("##history", 15, flags, ImVec2(-1, liveH))) {
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
    }

    // ── Queried fills (from filtered reqExecutions) ───────────────────────
    if (hasQueried) {
        float queryH = availH - liveH - ImGui::GetFrameHeightWithSpacing();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.30f, 1.0f));
        ImGui::Text("Queried results (%d)", static_cast<int>(m_queriedFills.size()));
        ImGui::PopStyleColor();
        ImGui::Separator();

        static ImGuiTableFlags qflags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_SizingFixedFit;

        if (ImGui::BeginTable("##qfills", 8, qflags, ImVec2(-1, queryH - ImGui::GetFrameHeightWithSpacing()))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed,  62);
            ImGui::TableSetupColumn("Symbol",  ImGuiTableColumnFlags_WidthFixed,  68);
            ImGui::TableSetupColumn("Side",    ImGuiTableColumnFlags_WidthFixed,  42);
            ImGui::TableSetupColumn("Qty",     ImGuiTableColumnFlags_WidthFixed,  55);
            ImGui::TableSetupColumn("Price",   ImGuiTableColumnFlags_WidthFixed,  80);
            ImGui::TableSetupColumn("Comm $",  ImGuiTableColumnFlags_WidthFixed,  62);
            ImGui::TableSetupColumn("P&L",     ImGuiTableColumnFlags_WidthFixed,  72);
            ImGui::TableSetupColumn("OrderId", ImGuiTableColumnFlags_WidthFixed,  58);
            ImGui::TableHeadersRow();

            for (const auto& f : m_queriedFills)
                DrawQueriedFillRow(f);
            ImGui::EndTable();
        }
    }
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
// Queried fill row (8 columns: Time | Symbol | Side | Qty | Price | Comm | P&L | OrderId)
// ============================================================================
void OrdersWindow::DrawQueriedFillRow(const core::Fill& f) {
    ImGui::TableNextRow();
    ImGui::PushID(f.execId.c_str());

    // Amber tint to distinguish from live-session orders
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.20f, 0.05f, 0.35f)));

    // 0 — Time
    ImGui::TableSetColumnIndex(0);
    if (f.timestamp != 0) {
        char tbuf[16];
        std::tm* lt = std::localtime(&f.timestamp);
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", lt);
        ImGui::TextDisabled("%s", tbuf);
    } else {
        ImGui::TextDisabled("—");
    }

    // 1 — Symbol
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(f.symbol.c_str());

    // 2 — Side
    ImGui::TableSetColumnIndex(2);
    ImGui::PushStyleColor(ImGuiCol_Text,
        f.side == core::OrderSide::Buy
            ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
            : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::TextUnformatted(core::OrderSideStr(f.side));
    ImGui::PopStyleColor();

    // 3 — Qty
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%.0f", f.quantity);

    // 4 — Price
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("$%.2f", f.price);

    // 5 — Commission
    ImGui::TableSetColumnIndex(5);
    if (f.commission > 0.0) ImGui::Text("-$%.2f", f.commission);
    else                    ImGui::TextDisabled("—");

    // 6 — Realized P&L
    ImGui::TableSetColumnIndex(6);
    if (f.realizedPnL != 0.0) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            f.realizedPnL >= 0.0
                ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
        ImGui::Text("%+.2f", f.realizedPnL);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("—");
    }

    // 7 — OrderId
    ImGui::TableSetColumnIndex(7);
    ImGui::TextDisabled("%d", f.orderId);

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
