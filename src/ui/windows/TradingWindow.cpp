#include "ui/UiScale.h"
#include "ui/windows/TradingWindow.h"
#include "imgui.h"
#include "core/models/WindowGroup.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
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
TradingWindow::TradingWindow() {}

// ============================================================================
// Public API — IB Gateway callbacks
// ============================================================================
void TradingWindow::OnDepthUpdate(int /*reqId*/, bool isBid, int pos, int op,
                                   double price, double size) {
    m_depthStatus = SubStatus::Ok;
    auto& levels = isBid ? m_bids : m_asks;

    if (op == 0) {
        // Insert
        core::DepthLevel lvl;
        lvl.price    = price;
        lvl.size     = size;
        lvl.flashAge = 0.0f;
        if (pos >= (int)levels.size())
            levels.push_back(lvl);
        else
            levels.insert(levels.begin() + pos, lvl);
        if ((int)levels.size() > kDepthLevels)
            levels.resize(kDepthLevels);
    } else if (op == 1) {
        // Update
        if (pos < 0 || pos >= (int)levels.size()) return;
        levels[pos].flashAge = 0.0f;
        levels[pos].price    = price;
        levels[pos].size     = size;
    } else if (op == 2) {
        // Delete
        if (pos < 0 || pos >= (int)levels.size()) return;
        levels.erase(levels.begin() + pos);
    }

    m_maxDepthSize = 100.0;
    for (auto& l : m_asks) m_maxDepthSize = std::max(m_maxDepthSize, l.size);
    for (auto& l : m_bids) m_maxDepthSize = std::max(m_maxDepthSize, l.size);
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
    for (auto& d : m_domOrders) {
        if (d.orderId != orderId) continue;
        d.status = status;
        if (status == core::OrderStatus::Filled && d.fillAge < 0.0f)
            d.fillAge = 0.0f;   // start fade-out timer
        break;
    }
}

void TradingWindow::OnFill(const core::Fill& fill) {
    m_fills.insert(m_fills.begin(), fill);
    if (m_fills.size() > 200) m_fills.resize(200);

    // Update running position and average entry price
    double delta   = (fill.side == core::OrderSide::Buy) ? fill.quantity : -fill.quantity;
    double prevQty = m_positionQty;
    double newQty  = prevQty + delta;

    if (std::abs(newQty) < 1e-9) {
        // Closed flat
        m_positionQty   = 0.0;
        m_avgEntryPrice = 0.0;
    } else if (std::abs(prevQty) < 1e-9) {
        // Was flat — opening fresh position
        m_positionQty   = newQty;
        m_avgEntryPrice = fill.price;
    } else if ((prevQty > 0) == (delta > 0)) {
        // Adding to same-side position — weighted average entry
        double absP = std::abs(prevQty);
        double absD = std::abs(delta);
        m_avgEntryPrice = (absP * m_avgEntryPrice + absD * fill.price) / (absP + absD);
        m_positionQty   = newQty;
    } else {
        // Reducing or flipping
        m_positionQty = newQty;
        if (std::abs(newQty) > 1e-9 && (newQty > 0) != (prevQty > 0))
            m_avgEntryPrice = fill.price;   // flipped side — reset entry to fill price
        // If just reducing (same sign, smaller abs), keep existing avg entry
    }
}

void TradingWindow::OnNBBO(double bid, double bidSz, double ask, double askSz) {
    m_mktDataStatus = SubStatus::Ok;
    if (bid   > 0) m_nbboBid   = bid;
    if (bidSz > 0) m_nbboBidSz = bidSz;
    if (ask   > 0) m_nbboAsk   = ask;
    if (askSz > 0) m_nbboAskSz = askSz;
}

void TradingWindow::OnMktDataError(int code) {
    // 354 = not subscribed (delayed available), 10090 = partially not subscribed
    if (code == 354 || code == 10090)
        m_mktDataStatus = SubStatus::NeedSubscription;
    else
        m_mktDataStatus = SubStatus::NotAllowed;
}

void TradingWindow::OnDepthError(int code) {
    // 354 = no depth subscription, 10092 = deep book not allowed, 322 = no permissions
    if (code == 354 || code == 10090)
        m_depthStatus = SubStatus::NeedSubscription;
    else  // 10092, 322 etc.
        m_depthStatus = SubStatus::NotAllowed;
}

void TradingWindow::OnTick(double price, double size, bool isUptick) {
    core::Tick t;
    t.price     = price;
    t.size      = size;
    t.isUptick  = isUptick;
    t.timestamp = std::time(nullptr);
    m_ticks.push_front(t);
    if ((int)m_ticks.size() > kMaxTicks) m_ticks.pop_back();

    // Accumulate volume profile
    if (price > 0.0 && size > 0.0) {
        int key = static_cast<int>(std::round(price / 0.01));
        double& v = m_volAtPrice[key];
        v += size;
        if (v > m_maxVolAtPrice) m_maxVolAtPrice = v;
    }
}

void TradingWindow::setInstanceId(int id) {
    m_instanceId = id;
    std::snprintf(m_title, sizeof(m_title), "Book Trading %d##trading%d", id, id);
}

void TradingWindow::SetSymbol(const std::string& symbol, double midPrice) {
    std::strncpy(m_symbol, symbol.c_str(), sizeof(m_symbol) - 1);
    m_symbol[sizeof(m_symbol) - 1] = '\0';
    m_prevMidPrice    = midPrice;
    m_midPrice        = midPrice;
    m_bids.clear();
    m_asks.clear();
    m_maxDepthSize    = 1.0;
    m_mktDataStatus   = SubStatus::Unknown;
    m_depthStatus     = SubStatus::Unknown;
    m_nbboBid = m_nbboAsk = m_nbboBidSz = m_nbboAskSz = 0.0;
    m_ticks.clear();
    m_volAtPrice.clear();
    m_maxVolAtPrice   = 1.0;
    m_domOrders.clear();
    m_positionQty     = 0.0;
    m_avgEntryPrice   = 0.0;
}

void TradingWindow::UpdateMidPrice(double price) {
    m_prevMidPrice = m_midPrice;
    m_midPrice     = price;
}

// ============================================================================
// Render
// ============================================================================
bool TradingWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(1100, 620), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                           | ImGuiWindowFlags_NoFocusOnAppearing;
    char grp[8];
    if (m_groupId > 0) std::snprintf(grp, sizeof(grp), "G%d", m_groupId);
    else                std::strncpy(grp, "G-", sizeof(grp));
    char title[80];
    std::snprintf(title, sizeof(title), "Order Book %s %s###trading%d",
        m_symbol[0] == '\0' ? "--" : m_symbol, grp, m_instanceId);
    if (!ImGui::Begin(title, &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    float frameDt = ImGui::GetIO().DeltaTime;
    PruneFinishedOrders();
    for (auto& lvl : m_asks) lvl.flashAge += frameDt;
    for (auto& lvl : m_bids) lvl.flashAge += frameDt;

    // Age fill-flash on DOM orders; remove cancelled/rejected and expired fills
    for (auto& d : m_domOrders)
        if (d.fillAge >= 0.0f) d.fillAge += frameDt;
    m_domOrders.erase(
        std::remove_if(m_domOrders.begin(), m_domOrders.end(), [](const DOMOrder& d) {
            return d.fillAge > 3.0f
                || d.status == core::OrderStatus::Cancelled
                || d.status == core::OrderStatus::Rejected;
        }),
        m_domOrders.end());

    float totalW    = ImGui::GetContentRegionAvail().x;
    float totalH    = ImGui::GetContentRegionAvail().y;
    float splitterW = 4.0f;
    float splitterH = 4.0f;
    float itemSpX   = ImGui::GetStyle().ItemSpacing.x;
    float itemSpY   = ImGui::GetStyle().ItemSpacing.y;

    float topH  = totalH * m_topHeightRatio;
    float botH  = totalH - topH - splitterH - 2.0f * itemSpY;
    float bookW = totalW * m_bookWidthRatio;
    float entryW = totalW - bookW - splitterW - 2.0f * itemSpX;

    // Guard: if the content area is too small, skip rendering this frame.
    if (topH < 10.f || botH < 10.f || entryW < 10.f) {
        DrawConfirmationPopup();
        ImGui::End();
        return m_open;
    }

    ImDrawList* wdl = ImGui::GetWindowDrawList();

    // ---- Top-left: DOM ladder -----------------------------------------------
    ImGui::BeginChild("##book_panel", ImVec2(bookW, topH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawOrderBook();
    ImGui::EndChild();

    // ---- Vertical splitter (left | right) -----------------------------------
    ImGui::SameLine();
    ImGui::InvisibleButton("##vsplit", ImVec2(splitterW, topH));
    if (ImGui::IsItemActive()) {
        m_bookWidthRatio = std::clamp(
            m_bookWidthRatio + ImGui::GetIO().MouseDelta.x / totalW, 0.15f, 0.85f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    {
        ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
        wdl->AddRectFilled(p, q,
            (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ? IM_COL32(160, 160, 160, 255) : IM_COL32(70, 70, 70, 200));
    }

    // ---- Top-right: order entry ---------------------------------------------
    ImGui::SameLine();
    ImGui::BeginChild("##entry_panel", ImVec2(entryW, topH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawOrderEntry();
    ImGui::EndChild();

    // ---- Horizontal splitter (top | bottom) ---------------------------------
    ImGui::InvisibleButton("##hsplit", ImVec2(-1, splitterH));
    if (ImGui::IsItemActive()) {
        m_topHeightRatio = std::clamp(
            m_topHeightRatio + ImGui::GetIO().MouseDelta.y / totalH, 0.15f, 0.85f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    {
        ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
        wdl->AddRectFilled(p, q,
            (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ? IM_COL32(160, 160, 160, 255) : IM_COL32(70, 70, 70, 200));
    }

    // ---- Bottom: tabbed ─────────────────────────────────────────────────────
    ImGui::BeginChild("##bottom_panel", ImVec2(-1, botH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("##trade_tabs")) {
        if (ImGui::BeginTabItem("Open Orders")) {
            DrawOpenOrders();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Execution Log")) {
            DrawExecutionLog();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Time & Sales")) {
            DrawTimeSales();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    DrawConfirmationPopup();

    ImGui::End();
    return m_open;
}

// ============================================================================
// Click-to-trade helpers
// ============================================================================
void TradingWindow::PlaceDomOrder(bool isBuy, double price) {
    double qty = std::atof(m_qtyBuf);
    if (qty <= 0.0 || price <= 0.0) return;

    const char* tifs[] = {"DAY", "GTC", "IOC", "FOK"};
    int orderId = m_nextOrderId++;

    DOMOrder dom;
    dom.orderId = orderId;
    dom.price   = price;
    dom.isBuy   = isBuy;
    dom.status  = core::OrderStatus::Working;
    dom.fillAge = -1.0f;
    m_domOrders.push_back(dom);

    core::Order o;
    o.orderId     = orderId;
    o.symbol      = m_symbol;
    o.side        = isBuy ? core::OrderSide::Buy : core::OrderSide::Sell;
    o.type        = core::OrderType::Limit;
    o.tif         = static_cast<core::TimeInForce>(m_tifIdx);
    o.quantity    = qty;
    o.limitPrice  = price;
    o.status      = core::OrderStatus::Working;
    o.submittedAt = std::time(nullptr);
    o.updatedAt   = o.submittedAt;
    m_openOrders.push_back(o);

    if (OnOrderSubmit)
        OnOrderSubmit(orderId, std::string(m_symbol),
                      isBuy ? "BUY" : "SELL",
                      "LMT", qty, price, 0.0,
                      tifs[m_tifIdx], m_outsideRth);
}

TradingWindow::DOMOrder* TradingWindow::FindDomOrder(double price) {
    for (auto& d : m_domOrders)
        if (std::abs(d.price - price) < 0.005)
            return &d;
    return nullptr;
}

// ============================================================================
// Draw — DOM Ladder (Book Trading)
// ============================================================================
void TradingWindow::DrawOrderBook() {
    const float rowH  = ImGui::GetTextLineHeightWithSpacing();
    // Only allow click-to-trade when the mouse is actually over this panel.
    // MouseClicked[0] is a global frame flag — without this gate it would fire
    // on the same frame the user clicks the bottom tabs.
    const bool panelHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    // ── Title + last price + click-to-trade toggle ────────────────────────────
    FlexRow hdr;
    char domLabel[32];
    std::snprintf(domLabel, sizeof(domLabel), "DOM  %s", m_symbol);
    hdr.item(FlexRow::textW(domLabel), 0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.90f, 1.0f));
    ImGui::Text("%s", domLabel);
    ImGui::PopStyleColor();
    {
        bool priceUp = (m_midPrice >= m_prevMidPrice);
        char priceLabel[32];
        std::snprintf(priceLabel, sizeof(priceLabel), "%s $%.2f",
                      priceUp ? "^ " : "v ", m_midPrice);
        hdr.item(FlexRow::textW(priceLabel), 12);
        ImGui::PushStyleColor(ImGuiCol_Text, priceUp ? kBuyGreen : kSellRed);
        ImGui::Text("%s", priceLabel);
        ImGui::PopStyleColor();
    }
    if (m_positionQty != 0.0) {
        char posLabel[48];
        std::snprintf(posLabel, sizeof(posLabel), "%s%.0f@%.2f",
                      m_positionQty > 0 ? "L " : "S ",
                      std::abs(m_positionQty), m_avgEntryPrice);
        hdr.item(FlexRow::textW(posLabel), 14);
        ImGui::PushStyleColor(ImGuiCol_Text, m_positionQty > 0 ? kBuyGreen : kSellRed);
        ImGui::TextUnformatted(posLabel);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open position: %.0f shares @ avg $%.2f\n"
                              "P&L column shows dollar P&L at each price level.",
                              std::abs(m_positionQty), m_avgEntryPrice);
    }
    hdr.item(FlexRow::checkboxW("Expand Spread"), 20);
    ImGui::Checkbox("Expand Spread", &m_expandSpread);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show every tick between bid and ask as a separate clickable row.");
    hdr.item(FlexRow::checkboxW("Click-to-Trade"), 12);
    ImGui::Checkbox("Click-to-Trade", &m_clickToTrade);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Click any ask row → BUY limit order at that price.\n"
            "Click any bid row → SELL limit order at that price.\n"
            "Uses Quantity and TIF from the Order Entry panel.");
    hdr.item(FlexRow::textW("Levels:"), 16);
    ImGui::TextDisabled("Levels:");
    {
        static constexpr int  kLadderOptions[] = {5, 10, 15, 20, 25, 30, 40, 50};
        static const char*    kLadderLabels[]  = {"5","10","15","20","25","30","40","50"};
        static constexpr int  kLadderCount = 8;
        hdr.item(em(58), 4);
        ImGui::SetNextItemWidth(em(58));
        if (ImGui::Combo("##ladder_rows", &m_ladderRowsIdx, kLadderLabels, kLadderCount))
            m_ladderRows = kLadderOptions[m_ladderRowsIdx];
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Virtual price rows shown above ask and below bid\nwhen no Level II subscription is active.");
    }

    // ── Subscription banners ──────────────────────────────────────────────────
    if (m_mktDataStatus == SubStatus::NeedSubscription) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.1f, 1.f));
        ImGui::TextUnformatted("⚠ Market data subscription required — prices may be delayed");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Enable market data in IB Account Management\n"
                "→ Market Data Subscriptions → US Stocks (e.g. OPRA, NBBO).");
    } else if (m_mktDataStatus == SubStatus::NotAllowed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
        ImGui::TextUnformatted("✗ Market data not permitted for this instrument");
        ImGui::PopStyleColor();
    }
    if (m_depthStatus == SubStatus::NeedSubscription) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.1f, 1.f));
        ImGui::TextUnformatted("⚠ Level II subscription required — showing NBBO only");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Subscribe to NYSE/NASDAQ TotalView (or equivalent)\n"
                "in IB Account Management → Market Data Subscriptions.");
    } else if (m_depthStatus == SubStatus::NotAllowed) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.4f, 0.4f, 1.f));
        ImGui::TextUnformatted("✗ Level II depth not permitted for this instrument");
        ImGui::PopStyleColor();
    }

    // ── NBBO compact bar ──────────────────────────────────────────────────────
    ImGui::Separator();
    if (m_mktDataStatus == SubStatus::Ok) {
        FlexRow nbbo;
        char bidBuf[32], askBuf[32];
        std::snprintf(bidBuf, sizeof(bidBuf), "BID $%.2f x %.0f", m_nbboBid, m_nbboBidSz);
        std::snprintf(askBuf, sizeof(askBuf), "ASK $%.2f x %.0f", m_nbboAsk, m_nbboAskSz);
        nbbo.item(FlexRow::textW(bidBuf), 0);
        ImGui::PushStyleColor(ImGuiCol_Text, kBuyGreen);
        ImGui::TextUnformatted(bidBuf);
        ImGui::PopStyleColor();
        nbbo.item(FlexRow::textW(askBuf), 20);
        ImGui::PushStyleColor(ImGuiCol_Text, kSellRed);
        ImGui::TextUnformatted(askBuf);
        ImGui::PopStyleColor();
        if (m_nbboAsk > 0 && m_nbboBid > 0) {
            char sprdBuf[24];
            std::snprintf(sprdBuf, sizeof(sprdBuf), "sprd $%.2f", m_nbboAsk - m_nbboBid);
            nbbo.item(FlexRow::textW(sprdBuf), 20);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(sprdBuf);
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("BID  —      ASK  —");
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    // ── Pre-compute cumulative sizes ─────────────────────────────────────────
    const int nAsks  = (int)m_asks.size();
    const int nBids  = (int)m_bids.size();
    const bool noL2  = (nAsks == 0 && nBids == 0);
    const bool hasNBBO = (m_nbboBid > 0.0 || m_nbboAsk > 0.0);

    std::vector<double> askCum(nAsks, 0.0);
    for (int i = 0; i < nAsks; ++i)
        askCum[i] = (i == 0 ? 0.0 : askCum[i - 1]) + m_asks[i].size;

    std::vector<double> bidCum(nBids, 0.0);
    for (int i = 0; i < nBids; ++i)
        bidCum[i] = (i == 0 ? 0.0 : bidCum[i - 1]) + m_bids[i].size;

    // ── Flash-interpolated colour ─────────────────────────────────────────────
    auto FlashCol = [](const ImVec4& base, float age) -> ImVec4 {
        float t = std::min(1.0f, age / 0.5f);
        return ImVec4(
            base.x + (kFlashYellow.x - base.x) * (1.0f - t),
            base.y + (kFlashYellow.y - base.y) * (1.0f - t),
            base.z + (kFlashYellow.z - base.z) * (1.0f - t),
            1.0f);
    };

    // ── P&L per price level: position P&L when holding, else price distance ──
    // Shows dollar P&L if closed at each row's price; falls back to ±offset
    // from mid when flat so the column is always informative.
    auto CalcPnl = [&](double price) -> double {
        if (m_positionQty != 0.0 && m_avgEntryPrice > 0.0)
            return m_positionQty * (price - m_avgEntryPrice);
        return (m_midPrice > 0.0) ? price - m_midPrice : 0.0;
    };

    // ── Volume-profile bar ────────────────────────────────────────────────────
    // Primary layer  : consolidated traded volume at this price (bright blue bar,
    //                  full row height, width proportional to max volume seen).
    // Secondary layer: live depth size (thin strip along the bottom of the row,
    //                  bid/ask colour).  Depth is an order-book snapshot; volume
    //                  is what has actually traded — keeping them visually distinct
    //                  prevents confusion between the two.
    auto DrawBar = [&](double depthSz, double price, ImU32 depthCol) {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        ImVec2 p  = ImGui::GetCursorScreenPos();
        float  cw = ImGui::GetContentRegionAvail().x - 2.f;
        float  bh = ImGui::GetTextLineHeight();

        // Track: dim full-width background so the column is never blank
        ldl->AddRectFilled(p, ImVec2(p.x + cw, p.y + bh), IM_COL32(28, 28, 33, 80));

        // Volume profile: primary visual — how much has been traded at this price
        int key = static_cast<int>(std::round(price / 0.01));
        auto it = m_volAtPrice.find(key);
        if (it != m_volAtPrice.end() && m_maxVolAtPrice > 0.0) {
            float vw = std::max(2.f, (float)(it->second / m_maxVolAtPrice) * cw);
            ldl->AddRectFilled(p, ImVec2(p.x + vw, p.y + bh), IM_COL32(55, 130, 210, 190));
        }

        // Depth strip: thin bar along the bottom row edge (25% height)
        if (depthSz > 0.0 && m_maxDepthSize > 0.0) {
            float bw      = std::max(2.f, (float)(depthSz / m_maxDepthSize) * cw);
            float stripH  = std::max(2.f, bh * 0.25f);
            ldl->AddRectFilled(ImVec2(p.x, p.y + bh - stripH),
                               ImVec2(p.x + bw, p.y + bh), depthCol);
        }

        ImGui::Dummy(ImVec2(cw, bh));
    };

    // ── Per-row overlay: hover highlight, click-to-trade, DOM order tint ──────
    // Call at column 0 before rendering any text in that row.
    auto RowOverlay = [&](double rowPrice, bool isAskRow) {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        float ry = ImGui::GetCursorScreenPos().y;
        ImVec2 wMin = ImGui::GetWindowPos();
        float  wW   = ImGui::GetWindowSize().x;
        // IsMouseHoveringRect respects the current clip rect, so rows that have
        // scrolled outside the ##dom table viewport correctly return false even
        // when their absolute Y happens to coincide with a header widget (e.g.
        // the Click-to-Trade checkbox).  The raw-coordinate check that was here
        // before had no knowledge of the scroll clip rect and caused an accidental
        // PlaceDomOrder when the checkbox was pressed while the table was scrolled.
        // clip=false: ignore the current column clip rect so the rect test spans
        // the full row width, not just column 0 (where RowOverlay is called from).
        bool hovered = panelHovered &&
                       ImGui::IsMouseHoveringRect(ImVec2(wMin.x, ry),
                                                  ImVec2(wMin.x + wW, ry + rowH),
                                                  false);

        if (m_clickToTrade && rowPrice > 0.0) {
            if (hovered) {
                ImU32 hCol = isAskRow ? IM_COL32(80, 20, 20, 70)
                                       : IM_COL32(20, 80, 30, 70);
                ldl->AddRectFilled(ImVec2(wMin.x, ry),
                                   ImVec2(wMin.x + wW, ry + rowH), hCol);
                // Guard with !IsAnyItemActive(): the Checkbox widget sets ActiveId on
                // mouse-PRESS but toggles its bool on mouse-RELEASE (one frame later).
                // Without this guard, unchecking the checkbox while the table is
                // scrolled fires PlaceDomOrder on the same frame as the press.
                if (ImGui::GetIO().MouseClicked[0] && !ImGui::IsAnyItemActive())
                    PlaceDomOrder(isAskRow, rowPrice);  // ask row → BUY, bid row → SELL
            }
        }

        // DOM order tint (amber = working, green/red fade = filled)
        DOMOrder* dom = FindDomOrder(rowPrice);
        if (dom) {
            ImU32 tint = 0;
            if (dom->status == core::OrderStatus::Working  ||
                dom->status == core::OrderStatus::Pending  ||
                dom->status == core::OrderStatus::PartialFill) {
                tint = IM_COL32(200, 160, 10, 85);
            } else if (dom->status == core::OrderStatus::Filled &&
                       dom->fillAge >= 0.0f && dom->fillAge < 3.0f) {
                auto a = (ImU32)(std::max(0.0f, 1.0f - dom->fillAge / 3.0f) * 100.f);
                tint = dom->isBuy ? IM_COL32(30, 180, 60, a) : IM_COL32(200, 40, 40, a);
            }
            if (tint)
                ldl->AddRectFilled(ImVec2(wMin.x, ry),
                                   ImVec2(wMin.x + wW, ry + rowH), tint);
        }

        // Volume tooltip: hover anywhere on the row to see consolidated traded size
        if (hovered && rowPrice > 0.0) {
            int key = static_cast<int>(std::round(rowPrice / 0.01));
            auto vit = m_volAtPrice.find(key);
            if (vit != m_volAtPrice.end() && vit->second > 0.0) {
                ImGui::BeginTooltip();
                ImGui::Text("$%.2f", rowPrice);
                ImGui::Separator();
                ImGui::Text("Traded: %.0f shares", vit->second);
                ImGui::Text("%.1f%% of session max", 100.0 * vit->second / m_maxVolAtPrice);
                ImGui::EndTooltip();
            }
        }
    };

    // ── DOM table ─────────────────────────────────────────────────────────────
    ImGuiTableFlags tflags =
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY       |
        ImGuiTableFlags_SizingFixedFit;

    float availH = ImGui::GetContentRegionAvail().y;
    if (availH < 10.f) return;   // guard: don't create a degenerate scroll table
    if (!ImGui::BeginTable("##dom", 7, tflags, ImVec2(-1, availH)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Bid Sz",  ImGuiTableColumnFlags_WidthFixed,   62);
    ImGui::TableSetupColumn("Cum Bid", ImGuiTableColumnFlags_WidthFixed,   62);
    ImGui::TableSetupColumn("Price",   ImGuiTableColumnFlags_WidthFixed,   72);
    ImGui::TableSetupColumn("Cum Ask", ImGuiTableColumnFlags_WidthFixed,   62);
    ImGui::TableSetupColumn("Ask Sz",  ImGuiTableColumnFlags_WidthFixed,   62);
    ImGui::TableSetupColumn("P&L",     ImGuiTableColumnFlags_WidthFixed,   68);
    ImGui::TableSetupColumn("Bar",     ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    // ── Ask rows: highest price first ────────────────────────────────────────
    for (int i = nAsks - 1; i >= 0; --i) {
        const auto& lvl  = m_asks[i];
        const bool  best = (i == 0);
        const ImVec4 col = FlashCol(kSellRed, lvl.flashAge);
        const double pnl = CalcPnl(lvl.price);

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            best ? IM_COL32(100, 22, 22, 110) : IM_COL32(70, 18, 18, 70));

        ImGui::TableSetColumnIndex(0);
        RowOverlay(lvl.price, true);  // Col 0: empty on ask side; use for overlay

        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, best ? col : ImVec4(0.82f, 0.82f, 0.85f, 1.f));
        ImGui::Text("%.2f", lvl.price);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(3);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.38f, 0.38f, 1.f));
        ImGui::Text("%.0f", askCum[i]);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(4);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%.0f", lvl.size);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(5);
        if (m_positionQty != 0.0 || m_midPrice > 0.0) {
            ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
            ImGui::Text("%+.2f", pnl);
            ImGui::PopStyleColor();
        } else { ImGui::TextDisabled("—"); }

        ImGui::TableSetColumnIndex(6);
        DrawBar(lvl.size, lvl.price, IM_COL32(190, 45, 45, 180));
    }

    // ── Mid section: spread or NBBO fallback or empty notice ─────────────────
    if (!noL2) {
        // Spread: expand into per-tick rows or collapse to a single summary row
        if (nAsks > 0 && nBids > 0) {
            double spreadVal = m_asks[0].price - m_bids[0].price;
            int    nSpread   = std::max(0, (int)std::lround(spreadVal / 0.01) - 1);
            if (m_expandSpread) {
                static constexpr int kMaxSpreadRows = 100;
                for (int s = 1; s <= std::min(nSpread, kMaxSpreadRows); ++s) {
                    double price = RoundTick(m_asks[0].price - s * 0.01);
                    double pnl   = CalcPnl(price);
                    ImGui::TableNextRow();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(44, 44, 52, 100));
                    ImGui::TableSetColumnIndex(0);
                    RowOverlay(price, m_sideIdx == 0);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                    ImGui::Text("%.2f", price);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(5);
                    if (m_positionQty != 0.0 || m_midPrice > 0.0) {
                        ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
                        ImGui::Text("%+.2f", pnl);
                        ImGui::PopStyleColor();
                    }
                    ImGui::TableSetColumnIndex(6);
                    DrawBar(0.0, price, IM_COL32(160, 160, 170, 60));
                }
                if (nSpread > kMaxSpreadRows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::Text("... %d more ...", nSpread - kMaxSpreadRows);
                    ImGui::PopStyleColor();
                }
            } else {
                // Collapsed: single summary row
                double midP = RoundTick((m_asks[0].price + m_bids[0].price) / 2.0);
                ImGui::TableNextRow();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(44, 44, 52, 100));
                ImGui::TableSetColumnIndex(0);
                RowOverlay(midP, m_sideIdx == 0);
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                ImGui::Text("spread $%.2f (%d ticks)", spreadVal, nSpread + 1);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(6);
                DrawBar(0.0, midP, IM_COL32(160, 160, 170, 60));
            }
        }
    } else if (hasNBBO) {
        // No L2 — render a virtual price ladder: 25 rows above ask, NBBO rows,
        // 25 rows below bid.  Sizes shown only on the actual NBBO best bid/ask.
        const int    kLadderRows = m_ladderRows;
        static constexpr double kTick = 0.01;

        double askAnchor = (m_nbboAsk > 0.0) ? m_nbboAsk : m_midPrice;
        double bidAnchor = (m_nbboBid > 0.0) ? m_nbboBid : m_midPrice;

        // ── 25 virtual ask rows above the best ask (highest first) ───────────
        for (int i = kLadderRows; i >= 1; --i) {
            double price = RoundTick(askAnchor + i * kTick);
            double pnl   = CalcPnl(price);
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(50, 15, 15, 50));
            ImGui::TableSetColumnIndex(0);
            RowOverlay(price, true);
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.35f, 0.35f, 1.f));
            ImGui::Text("%.2f", price);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(5);
            if (m_midPrice > 0.0) {
                ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
                ImGui::Text("%+.2f", pnl);
                ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(6);
            DrawBar(0.0, price, IM_COL32(190, 45, 45, 100));
        }

        // ── Best ask row (NBBO) ───────────────────────────────────────────────
        if (m_nbboAsk > 0.0) {
            double pnl = CalcPnl(m_nbboAsk);
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(100, 22, 22, 110));
            ImGui::TableSetColumnIndex(0);
            RowOverlay(m_nbboAsk, true);
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, kSellRed);
            ImGui::Text("%.2f *", m_nbboAsk);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(4);
            ImGui::PushStyleColor(ImGuiCol_Text, kSellRed);
            ImGui::Text("%.0f", m_nbboAskSz);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(5);
            if (m_midPrice > 0.0) {
                ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
                ImGui::Text("%+.2f", pnl);
                ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(6);
            DrawBar(m_nbboAskSz, m_nbboAsk, IM_COL32(190, 45, 45, 180));
        }

        // ── Spread: expand into per-tick rows or collapse to a single summary ─
        if (m_nbboAsk > 0.0 && m_nbboBid > 0.0) {
            double spreadVal = m_nbboAsk - m_nbboBid;
            int    nSpread   = std::max(0, (int)std::lround(spreadVal / 0.01) - 1);
            if (m_expandSpread) {
                static constexpr int kMaxSpreadRows = 100;
                for (int s = 1; s <= std::min(nSpread, kMaxSpreadRows); ++s) {
                    double price = RoundTick(m_nbboAsk - s * 0.01);
                    double pnl   = CalcPnl(price);
                    ImGui::TableNextRow();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(44, 44, 52, 100));
                    ImGui::TableSetColumnIndex(0);
                    RowOverlay(price, m_sideIdx == 0);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                    ImGui::Text("%.2f", price);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(5);
                    if (m_positionQty != 0.0 || m_midPrice > 0.0) {
                        ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
                        ImGui::Text("%+.2f", pnl);
                        ImGui::PopStyleColor();
                    }
                    ImGui::TableSetColumnIndex(6);
                    DrawBar(0.0, price, IM_COL32(160, 160, 170, 60));
                }
                if (nSpread > kMaxSpreadRows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::Text("... %d more ...", nSpread - kMaxSpreadRows);
                    ImGui::PopStyleColor();
                }
            } else {
                double midP = RoundTick((m_nbboAsk + m_nbboBid) / 2.0);
                ImGui::TableNextRow();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(44, 44, 52, 100));
                ImGui::TableSetColumnIndex(0);
                RowOverlay(midP, m_sideIdx == 0);
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                ImGui::Text("spread $%.2f (%d ticks)", spreadVal, nSpread + 1);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(6);
                DrawBar(0.0, midP, IM_COL32(160, 160, 170, 60));
            }
        }

        // ── Best bid row (NBBO) ───────────────────────────────────────────────
        if (m_nbboBid > 0.0) {
            double pnl = CalcPnl(m_nbboBid);
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(18, 90, 32, 110));
            ImGui::TableSetColumnIndex(0);
            RowOverlay(m_nbboBid, false);
            ImGui::PushStyleColor(ImGuiCol_Text, kBuyGreen);
            ImGui::Text("%.0f", m_nbboBidSz);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, kBuyGreen);
            ImGui::Text("%.2f *", m_nbboBid);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(5);
            if (m_midPrice > 0.0) {
                ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
                ImGui::Text("%+.2f", pnl);
                ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(6);
            DrawBar(m_nbboBidSz, m_nbboBid, IM_COL32(35, 170, 65, 180));
        }

        // ── 25 virtual bid rows below the best bid ────────────────────────────
        for (int i = 1; i <= kLadderRows; ++i) {
            double price = RoundTick(bidAnchor - i * kTick);
            double pnl   = CalcPnl(price);
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(15, 50, 18, 50));
            ImGui::TableSetColumnIndex(0);
            RowOverlay(price, false);
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.55f, 0.38f, 1.f));
            ImGui::Text("%.2f", price);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(5);
            if (m_midPrice > 0.0) {
                ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
                ImGui::Text("%+.2f", pnl);
                ImGui::PopStyleColor();
            }
            ImGui::TableSetColumnIndex(6);
            DrawBar(0.0, price, IM_COL32(35, 170, 65, 100));
        }
    } else if (m_depthStatus != SubStatus::NeedSubscription &&
               m_depthStatus != SubStatus::NotAllowed) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("No depth —");
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("outside RTH");
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("or no L2 sub");
        ImGui::PopStyleColor();
    }

    // ── Bid rows: best bid first ──────────────────────────────────────────────
    for (int i = 0; i < nBids; ++i) {
        const auto& lvl  = m_bids[i];
        const bool  best = (i == 0);
        const ImVec4 col = FlashCol(kBuyGreen, lvl.flashAge);
        const double pnl = CalcPnl(lvl.price);

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            best ? IM_COL32(18, 90, 32, 110) : IM_COL32(15, 60, 22, 70));

        ImGui::TableSetColumnIndex(0);
        RowOverlay(lvl.price, false);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%.0f", lvl.size);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.38f, 0.72f, 0.42f, 1.f));
        ImGui::Text("%.0f", bidCum[i]);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, best ? col : ImVec4(0.82f, 0.82f, 0.85f, 1.f));
        ImGui::Text("%.2f", lvl.price);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(5);
        if (m_positionQty != 0.0 || m_midPrice > 0.0) {
            ImGui::PushStyleColor(ImGuiCol_Text, pnl >= 0.0 ? kBuyGreen : kSellRed);
            ImGui::Text("%+.2f", pnl);
            ImGui::PopStyleColor();
        } else { ImGui::TextDisabled("—"); }

        ImGui::TableSetColumnIndex(6);
        DrawBar(lvl.size, lvl.price, IM_COL32(35, 170, 65, 180));
    }

    ImGui::EndTable();
}

// ============================================================================
// Draw — Order Entry
// ============================================================================
void TradingWindow::DrawOrderEntry() {
    // Label column width scales with font so labels never overflow their column
    const float labelCol = em(75);
    const float btnW     = em(62);
    const float btnH     = em(22);

    // ---- Symbol row ---------------------------------------------------------
    {
        FlexRow row;
        row.item(FlexRow::buttonW("G1"), 0);
        core::DrawGroupPicker(m_groupId, "##trading_grp");
        row.item(FlexRow::textW("Symbol"), 8);
        ImGui::Text("Symbol");
        row.item(em(72), 6);
        ImGui::SetNextItemWidth(em(72));
        bool symEntered = ImGui::InputText("##sym", m_symbol, sizeof(m_symbol),
                                           ImGuiInputTextFlags_CharsUppercase |
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Type a symbol and press Enter to subscribe");
        if (symEntered && m_symbol[0] != '\0') {
            m_bids.clear();
            m_asks.clear();
            m_volAtPrice.clear();
            m_maxVolAtPrice  = 1.0;
            m_maxDepthSize   = 1.0;
            m_nbboBid        = 0.0;
            m_nbboAsk        = 0.0;
            m_nbboBidSz      = 0.0;
            m_nbboAskSz      = 0.0;
            m_midPrice       = 0.0;
            m_prevMidPrice   = 0.0;
            m_mktDataStatus  = SubStatus::Unknown;
            m_depthStatus    = SubStatus::Unknown;
            if (OnSymbolChanged) OnSymbolChanged(m_symbol);
        }
        char midBuf[24];
        std::snprintf(midBuf, sizeof(midBuf), "Mid: $%.2f", m_midPrice);
        row.item(FlexRow::textW(midBuf), 8);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1.0f));
        ImGui::TextUnformatted(midBuf);
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Side buttons -------------------------------------------------------
    bool isBuy = (m_sideIdx == 0);
    ImGui::PushStyleColor(ImGuiCol_Button,
        isBuy ? ImVec4(0.12f, 0.60f, 0.28f, 1.0f) : ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        isBuy ? ImVec4(0.20f, 0.75f, 0.38f, 1.0f) : ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
    if (ImGui::Button("  BUY  ", ImVec2(btnW, btnH))) m_sideIdx = 0;
    ImGui::PopStyleColor(2);

    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,
        !isBuy ? ImVec4(0.68f, 0.18f, 0.18f, 1.0f) : ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        !isBuy ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f) : ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
    if (ImGui::Button("  SELL  ", ImVec2(btnW, btnH))) m_sideIdx = 1;
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    // ---- Order Type ---------------------------------------------------------
    const char* types[] = {"Market", "Limit", "Stop", "Stop Limit"};
    ImGui::Text("Type");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(130));
    ImGui::Combo("##type", &m_typeIdx, types, IM_ARRAYSIZE(types));

    // ---- Quantity -----------------------------------------------------------
    ImGui::Text("Quantity");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(100));
    ImGui::InputText("##qty", m_qtyBuf, sizeof(m_qtyBuf),
                     ImGuiInputTextFlags_CharsDecimal);

    // ---- Limit price (shown for Limit / StopLimit) --------------------------
    bool needsLimit = (m_typeIdx == 1 || m_typeIdx == 3);
    if (needsLimit) {
        ImGui::Text("Lmt Price");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##lmt", m_lmtBuf, sizeof(m_lmtBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
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
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##stp", m_stpBuf, sizeof(m_stpBuf),
                         ImGuiInputTextFlags_CharsDecimal);
    }

    // ---- Time In Force ------------------------------------------------------
    const char* tifs[] = {"DAY", "GTC", "IOC", "FOK"};
    ImGui::Text("TIF");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(80));
    ImGui::Combo("##tif", &m_tifIdx, tifs, IM_ARRAYSIZE(tifs));

    // ---- Outside RTH --------------------------------------------------------
    ImGui::Spacing();
    ImGui::Checkbox("Outside RTH", &m_outsideRth);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Allow order to be active and fill during pre-market\n"
            "and after-hours sessions.\n"
            "Requires a limit price — market orders are rejected\n"
            "outside regular trading hours by IB.");

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

    float submitW = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(submitLabel, ImVec2(submitW, em(25)))) {
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

    // Use GetWindowViewport() (the Book Trading window's own viewport) instead of
    // GetMainViewport().  When the window is undocked into a separate OS window
    // (ImGuiConfigFlags_ViewportsEnable), GetMainViewport() returns the primary
    // GLFW window — the modal appears there, invisible to the user, and blocks
    // all ImGui input so the window appears permanently frozen.
    ImVec2 center = ImGui::GetWindowViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Confirm Order", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
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
        if (m_outsideRth) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
            ImGui::TextUnformatted("Outside RTH: YES");
            ImGui::PopStyleColor();
        }

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

    if (OnOrderSubmit) {
        const char* action = (o.side == core::OrderSide::Buy) ? "BUY" : "SELL";
        const char* orderType = "MKT";
        double price    = 0.0;  // lmt price for LMT; stop trigger for STP / STP LMT
        double auxPrice = 0.0;  // limit price for STP LMT
        switch (o.type) {
            case core::OrderType::Limit:
                orderType = "LMT";
                price     = o.limitPrice;
                break;
            case core::OrderType::Stop:
                orderType = "STP";
                price     = o.stopPrice;
                break;
            case core::OrderType::StopLimit:
                orderType = "STP LMT";
                price     = o.stopPrice;   // stop trigger
                auxPrice  = o.limitPrice;  // limit price
                break;
            default: break;
        }
        const char* tifs[] = {"DAY", "GTC", "IOC", "FOK"};
        OnOrderSubmit(o.orderId, o.symbol, action, orderType, o.quantity, price, auxPrice,
                      tifs[m_tifIdx], m_outsideRth);
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
            if (OnOrderCancel) {
                OnOrderCancel(orderId);
                // IB will confirm via OnOrderStatus; don't mutate locally yet
            } else {
                o.status    = core::OrderStatus::Cancelled;
                o.updatedAt = std::time(nullptr);
                printf("[trade] Order #%d cancelled\n", orderId);
            }
        }
        break;
    }
}

void TradingWindow::SetNextOrderId(int id) {
    m_nextOrderId = id;
}

void TradingWindow::SetPosition(double qty, double avgCost) {
    m_positionQty   = qty;
    m_avgEntryPrice = (std::abs(qty) > 1e-9) ? avgCost : 0.0;
}

// ============================================================================
// Prune old terminal orders so the list doesn't grow unbounded
// ============================================================================
void TradingWindow::PruneFinishedOrders() {
    auto isDone = [](const core::Order& o) {
        return (o.status == core::OrderStatus::Filled    ||
                o.status == core::OrderStatus::Cancelled ||
                o.status == core::OrderStatus::Rejected);
    };
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

double TradingWindow::RoundTick(double price, double tick) {
    return std::round(price / tick) * tick;
}

}  // namespace ui
