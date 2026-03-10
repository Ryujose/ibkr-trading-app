#include "ui/windows/TradingWindow.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <numeric>

namespace ui {

// ============================================================================
// Colour palette
// ============================================================================
static constexpr ImVec4 kBuyGreen    = {0.18f, 0.80f, 0.40f, 1.0f};
static constexpr ImVec4 kSellRed     = {0.88f, 0.25f, 0.25f, 1.0f};
static constexpr ImVec4 kNeutral     = {0.60f, 0.60f, 0.64f, 1.0f};
static constexpr ImVec4 kDim         = {0.40f, 0.40f, 0.44f, 1.0f};
static constexpr ImVec4 kFlashYellow = {1.00f, 0.90f, 0.20f, 1.0f};

// ============================================================================
// Construction
// ============================================================================
TradingWindow::TradingWindow()
    : m_rng(std::random_device{}())
{
    InitOrderBook();
    // Seed price buffers with the current mid
    std::snprintf(m_lmtBuf, sizeof(m_lmtBuf), "%.2f", m_midPrice);
    std::snprintf(m_stpBuf, sizeof(m_stpBuf), "%.2f", m_midPrice);
}

// ============================================================================
// Public API — IB Gateway callbacks (future)
// ============================================================================
void TradingWindow::OnDepthUpdate(bool isBid, int pos, double price, double size) {
    auto& levels = isBid ? m_bids : m_asks;
    if (pos < 0 || pos >= (int)levels.size()) return;
    levels[pos].flashAge = 0.0f;
    levels[pos].price    = price;
    levels[pos].size     = size;
}

void TradingWindow::OnOrderStatus(int orderId, core::OrderStatus status,
                                  double filled, double avgPrice) {
    for (auto& o : m_openOrders) {
        if (o.orderId != orderId) continue;
        o.status       = status;
        o.filledQty    = filled;
        o.avgFillPrice = avgPrice;
        o.updatedAt    = std::time(nullptr);
        break;
    }
}

void TradingWindow::OnFill(const core::Fill& fill) {
    m_fills.insert(m_fills.begin(), fill);
    if (m_fills.size() > 200) m_fills.resize(200);
}

void TradingWindow::OnTick(double price, double size, bool isUptick) {
    core::Tick t;
    t.price     = price;
    t.size      = size;
    t.isUptick  = isUptick;
    t.timestamp = std::time(nullptr);
    m_ticks.push_front(t);
    if ((int)m_ticks.size() > kMaxTicks) m_ticks.pop_back();
}

void TradingWindow::SetSymbol(const std::string& symbol, double midPrice) {
    std::strncpy(m_symbol, symbol.c_str(), sizeof(m_symbol) - 1);
    m_midPrice = midPrice;
    InitOrderBook();
}

// ============================================================================
// Render
// ============================================================================
bool TradingWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(1100, 600), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("Trading", &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    // ---- Simulation ticks ---------------------------------------------------
    auto now = Clock::now();
    auto elapsed = [&](Clock::time_point& last, float interval) -> float {
        float dt = std::chrono::duration<float>(now - last).count();
        if (dt >= interval) { last = now; return dt; }
        return 0.0f;
    };

    if (float dt = elapsed(m_lastBookUpdate, m_bookUpdateIntervalSec))  SimulateBookTick(dt);
    if (elapsed(m_lastTickUpdate,  m_tickIntervalSec) > 0)               SimulateNewTick();
    if (float dt = elapsed(m_lastOrderCheck,  m_orderCheckIntervalSec)) SimulateOrderLifecycle(dt);
    (void)elapsed; // suppress unused-lambda warning

    // Age flash highlights every frame
    float frameDt = ImGui::GetIO().DeltaTime;
    for (auto& lvl : m_asks) lvl.flashAge += frameDt;
    for (auto& lvl : m_bids) lvl.flashAge += frameDt;

    // ---- Top section: three panels side-by-side ----------------------------
    float totalW  = ImGui::GetContentRegionAvail().x;
    float totalH  = ImGui::GetContentRegionAvail().y;
    float topH    = totalH * 0.62f;
    float botH    = totalH - topH - ImGui::GetStyle().ItemSpacing.y;

    float bookW   = totalW * 0.30f;
    float entryW  = totalW * 0.38f;
    float tsW     = totalW - bookW - entryW - ImGui::GetStyle().ItemSpacing.x * 2;

    ImGui::BeginChild("##book_panel",  ImVec2(bookW,  topH), true);
    DrawOrderBook();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##entry_panel", ImVec2(entryW, topH), true);
    DrawOrderEntry();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##ts_panel",   ImVec2(tsW,    topH), true);
    DrawTimeSales();
    ImGui::EndChild();

    // ---- Bottom section: tabbed orders / log --------------------------------
    ImGui::BeginChild("##bottom_panel", ImVec2(-1, botH), true);
    if (ImGui::BeginTabBar("##trade_tabs")) {
        if (ImGui::BeginTabItem("Open Orders")) {
            DrawOpenOrders();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Execution Log")) {
            DrawExecutionLog();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // ---- Confirmation popup (opened from DrawOrderEntry) --------------------
    DrawConfirmationPopup();

    ImGui::End();
    return m_open;
}

// ============================================================================
// Order book — init
// ============================================================================
void TradingWindow::InitOrderBook() {
    double tick  = 0.01;
    double spread = 0.02;                      // one cent spread
    double askBase = RoundTick(m_midPrice + spread * 0.5);
    double bidBase = RoundTick(m_midPrice - spread * 0.5);

    m_asks.resize(kDepthLevels);
    m_bids.resize(kDepthLevels);

    for (int i = 0; i < kDepthLevels; i++) {
        m_asks[i].price    = RoundTick(askBase + i * tick);
        m_asks[i].size     = m_sizeDist(m_rng);
        m_asks[i].flashAge = 999.0f;

        m_bids[i].price    = RoundTick(bidBase - i * tick);
        m_bids[i].size     = m_sizeDist(m_rng);
        m_bids[i].flashAge = 999.0f;
    }
    m_maxDepthSize = 1500.0;
}

// ============================================================================
// Order book — simulation tick
// ============================================================================
void TradingWindow::SimulateBookTick(float /*dtSec*/) {
    // Random walk the mid price
    double move = m_priceDist(m_rng);
    m_prevMidPrice = m_midPrice;
    m_midPrice     = RoundTick(m_midPrice + move);

    double tick    = 0.01;
    double spread  = 0.02;
    double askBase = RoundTick(m_midPrice + spread * 0.5);
    double bidBase = RoundTick(m_midPrice - spread * 0.5);

    for (int i = 0; i < kDepthLevels; i++) {
        // Reprice — flash if the price actually changed
        double newAskP = RoundTick(askBase + i * tick);
        double newBidP = RoundTick(bidBase - i * tick);

        if (std::abs(newAskP - m_asks[i].price) > 0.005) m_asks[i].flashAge = 0.0f;
        if (std::abs(newBidP - m_bids[i].price) > 0.005) m_bids[i].flashAge = 0.0f;

        m_asks[i].price = newAskP;
        m_bids[i].price = newBidP;

        // Randomly update sizes (simulate order placement / cancellation)
        if (m_uniform(m_rng) < 0.40) {
            m_asks[i].size = m_sizeDist(m_rng);
            m_asks[i].flashAge = 0.0f;
        }
        if (m_uniform(m_rng) < 0.40) {
            m_bids[i].size = m_sizeDist(m_rng);
            m_bids[i].flashAge = 0.0f;
        }
    }

    // Recalculate max size for bar normalisation
    m_maxDepthSize = 100.0;
    for (auto& l : m_asks) m_maxDepthSize = std::max(m_maxDepthSize, l.size);
    for (auto& l : m_bids) m_maxDepthSize = std::max(m_maxDepthSize, l.size);

    // Check if any limit orders are now fillable
    for (auto& o : m_openOrders) {
        if (o.status != core::OrderStatus::Working) continue;
        bool fillable = false;
        if (o.type == core::OrderType::Limit) {
            fillable = (o.side == core::OrderSide::Buy  && m_midPrice <= o.limitPrice) ||
                       (o.side == core::OrderSide::Sell && m_midPrice >= o.limitPrice);
        }
        if (o.type == core::OrderType::Stop) {
            fillable = (o.side == core::OrderSide::Buy  && m_midPrice >= o.stopPrice) ||
                       (o.side == core::OrderSide::Sell && m_midPrice <= o.stopPrice);
        }
        if (fillable) {
            o.status       = core::OrderStatus::Filled;
            o.filledQty    = o.quantity;
            double slippage = (o.side == core::OrderSide::Buy ? 1 : -1) * 0.01;
            o.avgFillPrice  = m_midPrice + slippage;
            o.updatedAt     = std::time(nullptr);

            core::Fill fill;
            fill.orderId    = o.orderId;
            fill.symbol     = o.symbol;
            fill.side       = o.side;
            fill.quantity   = o.quantity;
            fill.price      = o.avgFillPrice;
            fill.commission = std::max(1.0, o.quantity * 0.005);
            fill.timestamp  = std::time(nullptr);
            m_fills.insert(m_fills.begin(), fill);
        }
    }
}

// ============================================================================
// Draw — Order Book
// ============================================================================
void TradingWindow::DrawOrderBook() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.90f, 1.0f));
    ImGui::Text("Order Book  %s", m_symbol);
    ImGui::PopStyleColor();
    ImGui::Separator();

    float panelW   = ImGui::GetContentRegionAvail().x;
    float barMaxW  = panelW * 0.40f;  // max width of the depth bar

    // Header row
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("%-10s %-8s", "Price", "Size");
    ImGui::PopStyleColor();

    // ---- Asks (sell side) — drawn top-to-bottom from furthest to closest ----
    {
        // Reverse so closest spread is at the bottom of the ask section
        std::vector<core::DepthLevel> asksDisplay(m_asks.rbegin(), m_asks.rend());
        for (auto& lvl : asksDisplay) {
            float t = std::min(1.0f, lvl.flashAge / 0.6f);
            ImVec4 col = ImVec4(
                kSellRed.x + (kFlashYellow.x - kSellRed.x) * (1.0f - t),
                kSellRed.y + (kFlashYellow.y - kSellRed.y) * (1.0f - t),
                kSellRed.z + (kFlashYellow.z - kSellRed.z) * (1.0f - t),
                1.0f);

            // Bar background
            float barW = (float)(lvl.size / m_maxDepthSize) * barMaxW;
            ImVec2 rowMin = ImGui::GetCursorScreenPos();
            rowMin.x += panelW - barW;
            ImVec2 rowMax = ImVec2(rowMin.x + barW,
                                   rowMin.y + ImGui::GetTextLineHeight());
            ImGui::GetWindowDrawList()->AddRectFilled(
                rowMin, rowMax, IM_COL32(180, 30, 30, 40));

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%-10.2f %-8.0f", lvl.price, lvl.size);
            ImGui::PopStyleColor();
        }
    }

    // ---- Spread row ---------------------------------------------------------
    double spread = m_asks.empty() || m_bids.empty() ? 0.0
                  : m_asks[0].price - m_bids[0].price;
    ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
    ImGui::Text("── sprd $%.2f ──", spread);
    ImGui::PopStyleColor();

    // ---- Bids (buy side) — closest spread at top ----------------------------
    for (auto& lvl : m_bids) {
        float t = std::min(1.0f, lvl.flashAge / 0.6f);
        ImVec4 col = ImVec4(
            kBuyGreen.x + (kFlashYellow.x - kBuyGreen.x) * (1.0f - t),
            kBuyGreen.y + (kFlashYellow.y - kBuyGreen.y) * (1.0f - t),
            kBuyGreen.z + (kFlashYellow.z - kBuyGreen.z) * (1.0f - t),
            1.0f);

        float barW = (float)(lvl.size / m_maxDepthSize) * barMaxW;
        ImVec2 rowMin = ImGui::GetCursorScreenPos();
        ImVec2 rowMax = ImVec2(rowMin.x + barW,
                               rowMin.y + ImGui::GetTextLineHeight());
        ImGui::GetWindowDrawList()->AddRectFilled(
            rowMin, rowMax, IM_COL32(30, 160, 60, 40));

        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%-10.2f %-8.0f", lvl.price, lvl.size);
        ImGui::PopStyleColor();
    }

    // ---- Last price indicator at bottom -------------------------------------
    ImGui::Separator();
    bool priceUp = (m_midPrice >= m_prevMidPrice);
    ImGui::PushStyleColor(ImGuiCol_Text, priceUp ? kBuyGreen : kSellRed);
    ImGui::Text("%s $%.2f", priceUp ? "▲" : "▼", m_midPrice);
    ImGui::PopStyleColor();
}

// ============================================================================
// Draw — Order Entry
// ============================================================================
void TradingWindow::DrawOrderEntry() {
    // ---- Symbol row ---------------------------------------------------------
    ImGui::Text("Symbol");
    ImGui::SameLine(75);
    ImGui::SetNextItemWidth(72);
    ImGui::InputText("##sym", m_symbol, sizeof(m_symbol),
                     ImGuiInputTextFlags_CharsUppercase);

    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1.0f));
    ImGui::Text("Mid: $%.2f", m_midPrice);
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Side buttons -------------------------------------------------------
    bool isBuy = (m_sideIdx == 0);
    ImGui::PushStyleColor(ImGuiCol_Button,
        isBuy ? ImVec4(0.12f, 0.60f, 0.28f, 1.0f) : ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        isBuy ? ImVec4(0.20f, 0.75f, 0.38f, 1.0f) : ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
    if (ImGui::Button("  BUY  ", ImVec2(80, 28))) m_sideIdx = 0;
    ImGui::PopStyleColor(2);

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,
        !isBuy ? ImVec4(0.68f, 0.18f, 0.18f, 1.0f) : ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        !isBuy ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f) : ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
    if (ImGui::Button("  SELL  ", ImVec2(80, 28))) m_sideIdx = 1;
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    // ---- Order Type ---------------------------------------------------------
    const char* types[] = {"Market", "Limit", "Stop", "Stop Limit"};
    ImGui::Text("Type");
    ImGui::SameLine(75);
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##type", &m_typeIdx, types, IM_ARRAYSIZE(types));

    // ---- Quantity -----------------------------------------------------------
    ImGui::Text("Quantity");
    ImGui::SameLine(75);
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("##qty", m_qtyBuf, sizeof(m_qtyBuf),
                     ImGuiInputTextFlags_CharsDecimal);

    // ---- Limit price (shown for Limit / StopLimit) --------------------------
    bool needsLimit = (m_typeIdx == 1 || m_typeIdx == 3);
    if (needsLimit) {
        ImGui::Text("Lmt Price");
        ImGui::SameLine(75);
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##lmt", m_lmtBuf, sizeof(m_lmtBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        // Quick-fill from book
        if (ImGui::SmallButton(isBuy ? "Ask" : "Bid")) {
            double fillPrice = isBuy ? (m_asks.empty() ? m_midPrice : m_asks[0].price)
                                     : (m_bids.empty() ? m_midPrice : m_bids[0].price);
            std::snprintf(m_lmtBuf, sizeof(m_lmtBuf), "%.2f", fillPrice);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill from best %s", isBuy ? "ask" : "bid");
    }

    // ---- Stop price (shown for Stop / StopLimit) ----------------------------
    bool needsStop  = (m_typeIdx == 2 || m_typeIdx == 3);
    if (needsStop) {
        ImGui::Text("Stp Price");
        ImGui::SameLine(75);
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("##stp", m_stpBuf, sizeof(m_stpBuf),
                         ImGuiInputTextFlags_CharsDecimal);
    }

    // ---- Time In Force ------------------------------------------------------
    const char* tifs[] = {"DAY", "GTC", "IOC", "FOK"};
    ImGui::Text("TIF");
    ImGui::SameLine(75);
    ImGui::SetNextItemWidth(80);
    ImGui::Combo("##tif", &m_tifIdx, tifs, IM_ARRAYSIZE(tifs));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Order preview ------------------------------------------------------
    double qty   = std::atof(m_qtyBuf);
    double lmt   = std::atof(m_lmtBuf);
    double refP  = needsLimit ? lmt : m_midPrice;
    double estVal = qty * refP;

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("Est. value: $%.2f  |  %s  %s  %s",
                estVal,
                m_sideIdx == 0 ? "BUY" : "SELL",
                types[m_typeIdx],
                tifs[m_tifIdx]);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ---- Submit button ------------------------------------------------------
    ImVec4 btnCol  = isBuy ? ImVec4(0.12f, 0.60f, 0.28f, 1.0f)
                           : ImVec4(0.68f, 0.18f, 0.18f, 1.0f);
    ImVec4 btnHov  = isBuy ? ImVec4(0.20f, 0.75f, 0.38f, 1.0f)
                           : ImVec4(0.85f, 0.25f, 0.25f, 1.0f);
    ImVec4 btnAct  = isBuy ? ImVec4(0.08f, 0.45f, 0.20f, 1.0f)
                           : ImVec4(0.50f, 0.12f, 0.12f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Button,        btnCol);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btnHov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  btnAct);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    char submitLabel[64];
    std::snprintf(submitLabel, sizeof(submitLabel),
                  "%s  %s  %.0f @ %s",
                  isBuy ? "BUY" : "SELL",
                  types[m_typeIdx], qty,
                  m_typeIdx == 0 ? "MKT" : m_lmtBuf);

    float btnW = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(submitLabel, ImVec2(btnW, 32))) {
        std::string err;
        if (ValidateOrder(err))
            m_showConfirm = true;
        else
            ImGui::SetTooltip("%s", err.c_str());
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // Validation error hint
    {
        std::string err;
        if (!ValidateOrder(err)) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.3f, 1.0f));
            ImGui::TextWrapped("! %s", err.c_str());
            ImGui::PopStyleColor();
        }
    }
}

// ============================================================================
// Confirmation popup
// ============================================================================
void TradingWindow::DrawConfirmationPopup() {
    if (m_showConfirm) {
        ImGui::OpenPopup("Confirm Order");
        m_showConfirm = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Confirm Order", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove)) {
        const char* types[] = {"Market", "Limit", "Stop", "Stop Limit"};
        const char* tifs[]  = {"DAY", "GTC", "IOC", "FOK"};
        double qty  = std::atof(m_qtyBuf);
        double lmt  = std::atof(m_lmtBuf);
        double stp  = std::atof(m_stpBuf);
        bool isBuy  = (m_sideIdx == 0);

        ImGui::PushStyleColor(ImGuiCol_Text,
            isBuy ? kBuyGreen : kSellRed);
        ImGui::Text("%s ORDER", isBuy ? "BUY" : "SELL");
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::Text("Symbol:    %s",           m_symbol);
        ImGui::Text("Type:      %s",           types[m_typeIdx]);
        ImGui::Text("Quantity:  %.0f shares",  qty);
        if (m_typeIdx >= 1) ImGui::Text("Limit:     $%.2f", lmt);
        if (m_typeIdx >= 2) ImGui::Text("Stop:      $%.2f", stp);
        ImGui::Text("TIF:       %s",           tifs[m_tifIdx]);

        double refP   = (m_typeIdx >= 1 && lmt > 0) ? lmt : m_midPrice;
        double estVal = qty * refP;
        ImGui::Spacing();
        ImGui::Text("Est. value: $%.2f", estVal);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Confirm / Cancel
        ImGui::PushStyleColor(ImGuiCol_Button, isBuy ?
            ImVec4(0.12f, 0.60f, 0.28f, 1.0f) : ImVec4(0.68f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button("Confirm", ImVec2(130, 0))) {
            SubmitOrder();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(130, 0)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

// ============================================================================
// Order submission / validation
// ============================================================================
bool TradingWindow::ValidateOrder(std::string& err) const {
    double qty = std::atof(m_qtyBuf);
    if (qty <= 0) { err = "Quantity must be > 0"; return false; }

    bool needsLimit = (m_typeIdx == 1 || m_typeIdx == 3);
    bool needsStop  = (m_typeIdx == 2 || m_typeIdx == 3);

    if (needsLimit) {
        double lmt = std::atof(m_lmtBuf);
        if (lmt <= 0) { err = "Limit price must be > 0"; return false; }
    }
    if (needsStop) {
        double stp = std::atof(m_stpBuf);
        if (stp <= 0) { err = "Stop price must be > 0"; return false; }
    }
    return true;
}

void TradingWindow::SubmitOrder() {
    core::Order o;
    o.orderId    = m_nextOrderId++;
    o.symbol     = m_symbol;
    o.side       = m_sideIdx == 0 ? core::OrderSide::Buy : core::OrderSide::Sell;
    o.type       = static_cast<core::OrderType>(m_typeIdx);
    o.tif        = static_cast<core::TimeInForce>(m_tifIdx);
    o.quantity   = std::atof(m_qtyBuf);
    o.limitPrice = (m_typeIdx == 1 || m_typeIdx == 3) ? std::atof(m_lmtBuf) : 0.0;
    o.stopPrice  = (m_typeIdx == 2 || m_typeIdx == 3) ? std::atof(m_stpBuf) : 0.0;
    o.status     = core::OrderStatus::Working;
    o.submittedAt = std::time(nullptr);
    o.updatedAt   = o.submittedAt;

    // Market orders fill immediately (with slippage)
    if (o.type == core::OrderType::Market) {
        double slip    = (o.side == core::OrderSide::Buy ? 1 : -1) * 0.01;
        o.filledQty    = o.quantity;
        o.avgFillPrice = m_midPrice + slip;
        o.status       = core::OrderStatus::Filled;
        o.updatedAt    = std::time(nullptr);

        core::Fill fill;
        fill.orderId    = o.orderId;
        fill.symbol     = o.symbol;
        fill.side       = o.side;
        fill.quantity   = o.quantity;
        fill.price      = o.avgFillPrice;
        fill.commission = std::max(1.0, o.quantity * 0.005);
        fill.timestamp  = std::time(nullptr);
        m_fills.insert(m_fills.begin(), fill);
    }

    m_openOrders.push_back(o);

    printf("[trade] Order #%d submitted: %s %s %.0f %s",
           o.orderId, core::OrderSideStr(o.side), o.symbol.c_str(),
           o.quantity, core::OrderTypeStr(o.type));
    if (o.limitPrice > 0) printf(" @ $%.2f", o.limitPrice);
    printf("\n");
}

void TradingWindow::CancelOrder(int orderId) {
    for (auto& o : m_openOrders) {
        if (o.orderId != orderId) continue;
        if (o.status == core::OrderStatus::Working ||
            o.status == core::OrderStatus::Pending  ||
            o.status == core::OrderStatus::PartialFill) {
            o.status    = core::OrderStatus::Cancelled;
            o.updatedAt = std::time(nullptr);
            printf("[trade] Order #%d cancelled\n", orderId);
        }
        break;
    }
}

// ============================================================================
// Order lifecycle simulation
// ============================================================================
void TradingWindow::SimulateOrderLifecycle(float /*dtSec*/) {
    // Already handled in SimulateBookTick (limit/stop orders)
    // Here we just prune very old filled/cancelled orders from the active list
    // (keep them for display, but limit list size)
    auto isDone = [](const core::Order& o) {
        return (o.status == core::OrderStatus::Filled    ||
                o.status == core::OrderStatus::Cancelled ||
                o.status == core::OrderStatus::Rejected);
    };
    // Keep done orders visible — only prune when list exceeds 50 terminal entries
    int doneCount = (int)std::count_if(m_openOrders.begin(), m_openOrders.end(), isDone);
    while (doneCount > 30) {
        auto it = std::find_if(m_openOrders.begin(), m_openOrders.end(), isDone);
        if (it == m_openOrders.end()) break;
        m_openOrders.erase(it);
        doneCount--;
    }
}

// ============================================================================
// Draw — Open Orders
// ============================================================================
void TradingWindow::DrawOpenOrders() {
    // Cancel-all button
    bool anyActive = std::any_of(m_openOrders.begin(), m_openOrders.end(),
        [](const core::Order& o){ return o.status == core::OrderStatus::Working ||
                                         o.status == core::OrderStatus::Pending; });
    if (anyActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::SmallButton("Cancel All Working")) {
            for (auto& o : m_openOrders)
                if (o.status == core::OrderStatus::Working ||
                    o.status == core::OrderStatus::Pending)
                    CancelOrder(o.orderId);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%d order(s)", (int)m_openOrders.size());
    ImGui::Spacing();

    ImGuiTableFlags tblFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##orders", 9, tblFlags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID",     ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Side",   ImGuiTableColumnFlags_WidthFixed, 42);
    ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthFixed, 55);
    ImGui::TableSetupColumn("Price",  ImGuiTableColumnFlags_WidthFixed, 65);
    ImGui::TableSetupColumn("Filled", ImGuiTableColumnFlags_WidthFixed, 55);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 52);
    ImGui::TableHeadersRow();

    // Show newest first
    for (int i = (int)m_openOrders.size() - 1; i >= 0; i--) {
        auto& o = m_openOrders[i];
        ImGui::TableNextRow();

        bool working = (o.status == core::OrderStatus::Working ||
                        o.status == core::OrderStatus::Pending  ||
                        o.status == core::OrderStatus::PartialFill);

        // Row background tint
        ImVec4 rowTint = o.side == core::OrderSide::Buy
            ? ImVec4(0.10f, 0.25f, 0.12f, 0.35f)
            : ImVec4(0.28f, 0.10f, 0.10f, 0.35f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(rowTint));

        ImGui::TableNextColumn(); ImGui::Text("%d", o.orderId);
        ImGui::TableNextColumn(); ImGui::Text("%s", o.symbol.c_str());

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
            o.side == core::OrderSide::Buy ? kBuyGreen : kSellRed);
        ImGui::Text("%s", core::OrderSideStr(o.side));
        ImGui::PopStyleColor();

        ImGui::TableNextColumn(); ImGui::Text("%s", core::OrderTypeStr(o.type));
        ImGui::TableNextColumn(); ImGui::Text("%.0f", o.quantity);

        ImGui::TableNextColumn();
        if (o.type == core::OrderType::Market)
            ImGui::TextDisabled("MKT");
        else
            ImGui::Text("$%.2f", o.limitPrice > 0 ? o.limitPrice : o.stopPrice);

        ImGui::TableNextColumn();
        if (o.filledQty > 0)
            ImGui::Text("%.0f@$%.2f", o.filledQty, o.avgFillPrice);
        else
            ImGui::TextDisabled("—");

        // Status with colour
        ImGui::TableNextColumn();
        ImVec4 statusCol = kDim;
        switch (o.status) {
            case core::OrderStatus::Working:     statusCol = ImVec4(0.9f,0.8f,0.1f,1); break;
            case core::OrderStatus::Filled:      statusCol = kBuyGreen;                 break;
            case core::OrderStatus::Cancelled:   statusCol = kNeutral;                  break;
            case core::OrderStatus::Rejected:    statusCol = kSellRed;                  break;
            case core::OrderStatus::PartialFill: statusCol = ImVec4(0.9f,0.6f,0.1f,1); break;
            default: break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, statusCol);
        ImGui::Text("%s", core::OrderStatusStr(o.status));
        ImGui::PopStyleColor();

        // Cancel button
        ImGui::TableNextColumn();
        if (working) {
            ImGui::PushID(o.orderId);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f,0.12f,0.12f,1));
            if (ImGui::SmallButton("Cancel")) CancelOrder(o.orderId);
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    ImGui::EndTable();
}

// ============================================================================
// Draw — Execution Log
// ============================================================================
void TradingWindow::DrawExecutionLog() {
    ImGui::TextDisabled("%d fill(s)", (int)m_fills.size());
    ImGui::Spacing();

    ImGuiTableFlags tblFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable("##fills", 7, tblFlags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed,   70);
    ImGui::TableSetupColumn("Order#",  ImGuiTableColumnFlags_WidthFixed,   55);
    ImGui::TableSetupColumn("Symbol",  ImGuiTableColumnFlags_WidthFixed,   55);
    ImGui::TableSetupColumn("Side",    ImGuiTableColumnFlags_WidthFixed,   42);
    ImGui::TableSetupColumn("Qty",     ImGuiTableColumnFlags_WidthFixed,   55);
    ImGui::TableSetupColumn("Price",   ImGuiTableColumnFlags_WidthFixed,   70);
    ImGui::TableSetupColumn("Comm",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const auto& f : m_fills) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", FmtTime(f.timestamp).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", f.orderId);
        ImGui::TableNextColumn(); ImGui::Text("%s", f.symbol.c_str());

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
            f.side == core::OrderSide::Buy ? kBuyGreen : kSellRed);
        ImGui::Text("%s", core::OrderSideStr(f.side));
        ImGui::PopStyleColor();

        ImGui::TableNextColumn(); ImGui::Text("%.0f",   f.quantity);
        ImGui::TableNextColumn(); ImGui::Text("$%.2f",  f.price);
        ImGui::TableNextColumn(); ImGui::Text("$%.2f",  f.commission);
    }

    ImGui::EndTable();
}

// ============================================================================
// Time & Sales simulation
// ============================================================================
void TradingWindow::SimulateNewTick() {
    double move    = m_priceDist(m_rng) * 0.5;
    double price   = RoundTick(m_midPrice + move);
    double size    = 50.0 + m_uniform(m_rng) * 950.0;
    bool   isUp    = (price >= (m_ticks.empty() ? m_midPrice : m_ticks.front().price));

    core::Tick t;
    t.price     = price;
    t.size      = size;
    t.isUptick  = isUp;
    t.timestamp = std::time(nullptr);
    m_ticks.push_front(t);
    if ((int)m_ticks.size() > kMaxTicks) m_ticks.pop_back();
}

// ============================================================================
// Draw — Time & Sales
// ============================================================================
void TradingWindow::DrawTimeSales() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.90f, 1.0f));
    ImGui::Text("Time & Sales");
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("%-8s %-9s %s", "Time", "Price", "Size");
    ImGui::PopStyleColor();

    ImGui::BeginChild("##ts_scroll", ImVec2(-1, -1), false,
                      ImGuiWindowFlags_NoScrollbar);

    for (const auto& t : m_ticks) {
        ImVec4 col = t.isUptick ? kBuyGreen : kSellRed;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%-8s %-9.2f %.0f",
                    FmtTime(t.timestamp).c_str(), t.price, t.size);
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
}

// ============================================================================
// Helpers
// ============================================================================
std::string TradingWindow::FmtTime(std::time_t t) {
    std::tm* tm = std::localtime(&t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    return buf;
}

std::string TradingWindow::FmtPrice(double p) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "$%.2f", p);
    return buf;
}

double TradingWindow::RoundTick(double price, double tick) {
    return std::round(price / tick) * tick;
}

}  // namespace ui
