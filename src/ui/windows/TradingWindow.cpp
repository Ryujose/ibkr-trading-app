#include "ui/UiScale.h"
#include "ui/windows/TradingWindow.h"
#include "ui/SymbolSearch.h"
#include "core/services/ChartAnalysis.h"
#include "imgui.h"
#include "core/models/WindowGroup.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
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
                                   double price, double size,
                                   const std::string& exchange, bool isSmartDepth) {
    (void)isSmartDepth;
    m_depthStatus = SubStatus::Ok;

    if (m_useL2) {
        // L2 mode: write into per-exchange bucket.
        // IB's pos index is per-exchange — each exchange has its own book.
        auto& buckets = isBid ? m_bidBuckets : m_askBuckets;
        std::string key = exchange.empty() ? "SMART" : exchange;
        auto& levels = buckets[key];

        if (op == 0) {
            core::DepthLevel lvl;
            lvl.price    = price;
            lvl.size     = size;
            lvl.flashAge = 0.0f;
            lvl.exchange = key;
            if (pos >= (int)levels.size())
                levels.push_back(lvl);
            else
                levels.insert(levels.begin() + pos, lvl);
            if ((int)levels.size() > kDepthLevels)
                levels.resize(kDepthLevels);
        } else if (op == 1) {
            if (pos < 0 || pos >= (int)levels.size()) return;
            levels[pos].flashAge = 0.0f;
            levels[pos].price    = price;
            levels[pos].size     = size;
            levels[pos].exchange = key;
        } else if (op == 2) {
            if (pos < 0 || pos >= (int)levels.size()) return;
            levels.erase(levels.begin() + pos);
        }

        RefreshExchangeFilterList();
        RebuildDepthView();
    } else {
        // L1 mode: flat vectors as before.
        auto& levels = isBid ? m_bids : m_asks;

        if (op == 0) {
            core::DepthLevel lvl;
            lvl.price    = price;
            lvl.size     = size;
            lvl.flashAge = 0.0f;
            lvl.exchange = exchange;
            if (pos >= (int)levels.size())
                levels.push_back(lvl);
            else
                levels.insert(levels.begin() + pos, lvl);
            if ((int)levels.size() > kDepthLevels)
                levels.resize(kDepthLevels);
        } else if (op == 1) {
            if (pos < 0 || pos >= (int)levels.size()) return;
            levels[pos].flashAge = 0.0f;
            levels[pos].price    = price;
            levels[pos].size     = size;
            levels[pos].exchange = exchange;
        } else if (op == 2) {
            if (pos < 0 || pos >= (int)levels.size()) return;
            levels.erase(levels.begin() + pos);
        }
    }

    m_maxDepthSize = 100.0;
    for (auto& l : m_asks) m_maxDepthSize = std::max(m_maxDepthSize, l.size);
    for (auto& l : m_bids) m_maxDepthSize = std::max(m_maxDepthSize, l.size);
}

void TradingWindow::RefreshExchangeFilterList() {
    std::set<std::string> seen;
    seen.insert("All");
    for (const auto& [exch, _] : m_askBuckets) if (!exch.empty()) seen.insert(exch);
    for (const auto& [exch, _] : m_bidBuckets) if (!exch.empty()) seen.insert(exch);
    m_exchangeList.assign(seen.begin(), seen.end());
    if (m_exchangeFilterIdx >= (int)m_exchangeList.size())
        m_exchangeFilterIdx = 0;
}

void TradingWindow::RebuildDepthView() {
    if (!m_useL2) return;
    const std::string& filter = (m_exchangeFilterIdx < (int)m_exchangeList.size())
                                ? m_exchangeList[m_exchangeFilterIdx] : "All";
    auto rebuild = [&](auto& flat, auto& buckets, bool asc) {
        flat.clear();
        if (filter == "All") {
            // Merge all buckets into flat view sorted by price
            for (auto& [_, vec] : buckets)
                for (auto& l : vec) flat.push_back(l);
            std::sort(flat.begin(), flat.end(), [asc](auto& a, auto& b) {
                return asc ? (a.price < b.price) : (a.price > b.price);
            });
        } else {
            auto it = buckets.find(filter);
            if (it != buckets.end()) flat = it->second;
        }
        if ((int)flat.size() > kDepthLevels) flat.resize(kDepthLevels);
    };
    rebuild(m_asks, m_askBuckets, true);   // asks: low → high   (best ask at [0])
    rebuild(m_bids, m_bidBuckets, false);  // bids: high → low   (best bid at [0])
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

    // Update mid-price if we have both sides, otherwise use what we have
    if (m_nbboBid > 0 && m_nbboAsk > 0) {
        UpdateMidPrice((m_nbboBid + m_nbboAsk) * 0.5);
    } else if (m_nbboBid > 0 && m_midPrice <= 0) {
        UpdateMidPrice(m_nbboBid);
    } else if (m_nbboAsk > 0 && m_midPrice <= 0) {
        UpdateMidPrice(m_nbboAsk);
    }
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
    if (price > 0.0) m_lastPrice = price;
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

void TradingWindow::OnTickByTick(const core::Tick& tick) {
    if (tick.price > 0.0) m_lastPrice = tick.price;
    m_ticks.push_front(tick);
    if ((int)m_ticks.size() > kMaxTicks) m_ticks.pop_back();
    if (tick.size > m_maxTickSize) m_maxTickSize = tick.size;

    if (tick.price > 0.0 && tick.size > 0.0) {
        int key = static_cast<int>(std::round(tick.price / 0.01));
        double& v = m_volAtPrice[key];
        v += tick.size;
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
    m_lastPrice       = 0.0;
    m_bids.clear();
    m_asks.clear();
    m_askBuckets.clear();
    m_bidBuckets.clear();
    m_exchangeList = {"SMART"};
    m_exchangeFilterIdx = 0;
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
    // A LAST tick means the data subscription is working even if no bid/ask
    // has arrived yet (paper accounts outside RTH, or delayed-frozen data).
    if (m_mktDataStatus == SubStatus::Unknown)
        m_mktDataStatus = SubStatus::Ok;
}

void TradingWindow::setNumDepthRows(int n) {
    if (n == m_numDepthRows) return;
    m_numDepthRows = n;
    // Re-subscribe with the new row count.  Fire OnDepthModeChanged so main.cpp
    // cancels the old subscription and re-subscribes with the correct rows.
    if (OnDepthModeChanged) OnDepthModeChanged(m_useL2);
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
    DrawUnguardedStrip();   // yellow protective-stop warning when applicable
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
    o.outsideRth  = m_outsideRth;
    o.status      = core::OrderStatus::Working;
    o.submittedAt = std::time(nullptr);
    o.updatedAt   = o.submittedAt;
    m_openOrders.push_back(o);

    if (OnOrderSubmit)
        OnOrderSubmit(o);
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
        static constexpr int  kLadderOptions[] = {5, 10, 15, 20, 25, 30, 40, 50,
                                                   75, 100, 150, 200, 250, 300};
        static const char*    kLadderLabels[]  = {"5","10","15","20","25","30","40","50",
                                                   "75","100","150","200","250","300"};
        static constexpr int  kLadderCount = 14;
        hdr.item(em(58), 4);
        ImGui::SetNextItemWidth(em(58));
        if (ImGui::Combo("##ladder_rows", &m_ladderRowsIdx, kLadderLabels, kLadderCount)) {
            m_ladderRows = kLadderOptions[m_ladderRowsIdx];
            setNumDepthRows(m_ladderRows);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Market depth levels requested from IB.\n"
                              "Also sets virtual fallback rows when no depth subscription is active.");
    }

    // ── L1 / L2 depth toggle ─────────────────────────────────────────────────
    hdr.item(FlexRow::buttonW("L1"), 8);
    if (ImGui::Button(m_useL2 ? "L2" : "L1")) {
        m_useL2 = !m_useL2;
        if (m_useL2) {
            m_askBuckets.clear();
            m_bidBuckets.clear();
            m_bids.clear();
            m_asks.clear();
            m_exchangeFilterIdx = 0;
        }
        RebuildDepthView();
        if (OnDepthModeChanged) OnDepthModeChanged(m_useL2);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("L1 = SMART aggregated depth\nL2 = per-exchange depth (TotalView/OpenBook)");

    if (m_useL2) {
        hdr.item(em(80), 4);
        ImGui::SetNextItemWidth(em(80));
        const char* filterLabel = (m_exchangeFilterIdx < (int)m_exchangeList.size())
                                  ? m_exchangeList[m_exchangeFilterIdx].c_str()
                                  : "All";
        if (ImGui::BeginCombo("##exch_filter", filterLabel)) {
            for (int i = 0; i < (int)m_exchangeList.size(); ++i) {
                bool sel = (i == m_exchangeFilterIdx);
                if (ImGui::Selectable(m_exchangeList[i].c_str(), sel, 0, ImVec2(0, 0))) {
                    m_exchangeFilterIdx = i;
                    RebuildDepthView();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Filter L2 depth by exchange");
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
        if (m_nbboBid > 0.0 || m_nbboAsk > 0.0) {
            // Full NBBO available
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
        } else if (m_midPrice > 0.0) {
            // Only LAST price available (paper/delayed-frozen outside RTH)
            char lastBuf[32];
            std::snprintf(lastBuf, sizeof(lastBuf), "LAST $%.2f", m_midPrice);
            nbbo.item(FlexRow::textW(lastBuf), 0);
            ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
            ImGui::TextUnformatted(lastBuf);
            ImGui::PopStyleColor();
            nbbo.item(FlexRow::textW("(no bid/ask)"), 16);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted("(no bid/ask)");
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

    // ── Per-row overlay: hover highlight, DOM order tint, volume tooltip ─────
    // Call at column 0 before rendering any text in that row.
    //  tag: 'a' = ask-side row, 'b' = bid-side row, 'm' = mid/spread row
    // Click-to-trade is handled separately in the Price column so only a click
    // on the price itself fires an order (not on the BidSz / AskSz columns).
    auto RowOverlay = [&](double rowPrice, char tag) {
        bool isAskRow = (tag == 'a');
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        float ry = ImGui::GetCursorScreenPos().y;
        ImVec2 wMin = ImGui::GetWindowPos();
        float  wW   = ImGui::GetWindowSize().x;

        bool hovered = panelHovered &&
                       ImGui::IsMouseHoveringRect(ImVec2(wMin.x, ry),
                                                  ImVec2(wMin.x + wW, ry + rowH),
                                                  false);

        if (m_clickToTrade && rowPrice > 0.0 && hovered) {
            ImU32 hCol = isAskRow ? IM_COL32(80, 20, 20, 70)
                                   : IM_COL32(20, 80, 30, 70);
            ldl->AddRectFilled(ImVec2(wMin.x, ry),
                               ImVec2(wMin.x + wW, ry + rowH), hCol);
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

    // ── S/R proximity helper ──────────────────────────────────────────────────
    // Uses the same tolerance as ChartWindow::DetectStructure: max(0.3% × price,
    // 0.5 × ATR14). When ATR hasn't been computed yet (m_atrForSR ≤ 0), falls
    // back to 0.3% × price alone.
    //
    // The S/R lists are detected on the chart's last close; by the time they
    // reach the DOM the live price may have moved through one or more levels.
    // To keep the labels consistent with the ladder order, classify each
    // matched level by its position relative to the live reference price
    // (level > ref → R, level < ref → S). Pick the nearest level on ties.
    const double refPrice = (m_lastPrice > 0.0) ? m_lastPrice
                          : (m_midPrice  > 0.0) ? m_midPrice
                          : (m_nbboBid > 0.0 && m_nbboAsk > 0.0) ? 0.5 * (m_nbboBid + m_nbboAsk)
                                                                 : 0.0;
    auto srTag = [&](double price) -> const char* {
        if (m_supportPrices.empty() && m_resistancePrices.empty()) return nullptr;
        const double tol = (m_atrForSR > 0.0) ? std::max(0.003 * price, 0.5 * m_atrForSR)
                                              : 0.003 * price;
        double  bestDist = tol;
        double  bestLvl  = 0.0;
        bool    bestFromSupport = false;
        bool    found    = false;
        for (double s : m_supportPrices) {
            double d = std::abs(price - s);
            if (d <= bestDist) { bestDist = d; bestLvl = s; bestFromSupport = true;  found = true; }
        }
        for (double r : m_resistancePrices) {
            double d = std::abs(price - r);
            if (d <= bestDist) { bestDist = d; bestLvl = r; bestFromSupport = false; found = true; }
        }
        if (!found) return nullptr;
        if (refPrice > 0.0) return (bestLvl >= refPrice) ? "R" : "S";
        return bestFromSupport ? "S" : "R";
    };

    // ── Price-column click-to-trade ───────────────────────────────────────────
    // Renders an invisible button at the current cursor position without
    // disrupting subsequent text layout (saves/restores cursor Y).
    int priceClickSeq = 0;
    auto PriceClickCell = [&](double price, bool isBuy) {
        if (m_clickToTrade && price > 0.0) {
            ImVec2 savePos = ImGui::GetCursorPos();
            float  colW    = ImGui::GetContentRegionAvail().x;
            ImGui::PushID(priceClickSeq++);
            ImGui::InvisibleButton("##priceclick", ImVec2(colW, rowH),
                                   ImGuiButtonFlags_MouseButtonLeft);
            ImGui::PopID();
            ImGui::SetCursorPos(savePos);  // restore so text renders on top
            if (ImGui::IsItemClicked(0))
                PlaceDomOrder(isBuy, price);
        }
    };

    // ── DOM table ─────────────────────────────────────────────────────────────
    ImGuiTableFlags tflags =
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY       |
        ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_Resizable;

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

    // ── Ask rows: highest first, descending to best (lowest) at the spread ───
    // m_asks sorted low→high: index 0 = best, index nAsks-1 = highest.
    for (int i = nAsks - 1; i >= 0; --i) {
        const auto& lvl  = m_asks[i];
        const bool  best = (i == 0);  // best = lowest price, nearest to spread
        const ImVec4 col = FlashCol(kSellRed, lvl.flashAge);
        const double pnl = CalcPnl(lvl.price);

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            best ? IM_COL32(100, 22, 22, 110) : IM_COL32(70, 18, 18, 70));

        ImGui::TableSetColumnIndex(0);
        RowOverlay(lvl.price, 'a');  // ask row

        ImGui::TableSetColumnIndex(2);
        PriceClickCell(lvl.price, true);  // click ask price → BUY
        {
            const char* sr = srTag(lvl.price);
            if (sr) {
                bool isResistance = (sr[0] == 'R');
                ImGui::PushStyleColor(ImGuiCol_Text,
                    isResistance ? ImVec4(0.95f, 0.40f, 0.30f, 1.f)
                                 : ImVec4(0.25f, 0.85f, 0.35f, 1.f));
                ImGui::Text("%s", sr);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 2.f);
            }
            bool isLast = (m_lastPrice > 0.0 && std::abs(lvl.price - m_lastPrice) < 0.005);
            ImGui::PushStyleColor(ImGuiCol_Text, isLast ? ImVec4(1.0f, 0.85f, 0.2f, 1.f)
                                  : best ? col : ImVec4(0.82f, 0.82f, 0.85f, 1.f));
            if (m_useL2 && !lvl.exchange.empty())
                ImGui::Text("%.2f [%s]%s", lvl.price, lvl.exchange.c_str(), isLast ? " *" : "");
            else
                ImGui::Text("%.2f%s", lvl.price, isLast ? " *" : "");
            ImGui::PopStyleColor();
        }

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
                    RowOverlay(price, 'm');
                    ImGui::TableSetColumnIndex(2);
                    {
                        const char* sr = srTag(price);
                        if (sr) {
                            bool isRes = (sr[0] == 'R');
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                isRes ? ImVec4(0.95f, 0.40f, 0.30f, 1.f)
                                      : ImVec4(0.25f, 0.85f, 0.35f, 1.f));
                            ImGui::Text("%s", sr);
                            ImGui::PopStyleColor();
                            ImGui::SameLine(0.f, 2.f);
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                        ImGui::Text("%.2f", price);
                        ImGui::PopStyleColor();
                    }
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
                RowOverlay(midP, 'm');
                ImGui::TableSetColumnIndex(2);
                ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                ImGui::Text("spread $%.2f (%d ticks)", spreadVal, nSpread + 1);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(6);
                DrawBar(0.0, midP, IM_COL32(160, 160, 170, 60));
            }
        }
    } else if (hasNBBO || m_midPrice > 0.0) {
        // No L2 — render a virtual price ladder anchored on NBBO when available,
        // or on LAST price when only delayed/frozen data is present.
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
            RowOverlay(price, 'a');
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
            RowOverlay(m_nbboAsk, 'a');
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
                    RowOverlay(price, 'm');
                    ImGui::TableSetColumnIndex(2);
                    {
                        const char* sr = srTag(price);
                        if (sr) {
                            bool isRes = (sr[0] == 'R');
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                isRes ? ImVec4(0.95f, 0.40f, 0.30f, 1.f)
                                      : ImVec4(0.25f, 0.85f, 0.35f, 1.f));
                            ImGui::Text("%s", sr);
                            ImGui::PopStyleColor();
                            ImGui::SameLine(0.f, 2.f);
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, kNeutral);
                        ImGui::Text("%.2f", price);
                        ImGui::PopStyleColor();
                    }
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
                RowOverlay(midP, 'm');
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
            RowOverlay(m_nbboBid, 'b');
            ImGui::PushStyleColor(ImGuiCol_Text, kBuyGreen);
            ImGui::Text("%.0f", m_nbboBidSz);
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            {
                const char* sr = srTag(m_nbboBid);
                if (sr) {
                    bool isRes = (sr[0] == 'R');
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        isRes ? ImVec4(0.95f, 0.40f, 0.30f, 1.f)
                              : ImVec4(0.25f, 0.85f, 0.35f, 1.f));
                    ImGui::Text("%s", sr);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.f, 2.f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, kBuyGreen);
                ImGui::Text("%.2f *", m_nbboBid);
                ImGui::PopStyleColor();
            }
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
            RowOverlay(price, 'b');
            ImGui::TableSetColumnIndex(2);
            {
                const char* sr = srTag(price);
                if (sr) {
                    bool isRes = (sr[0] == 'R');
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        isRes ? ImVec4(0.95f, 0.40f, 0.30f, 1.f)
                              : ImVec4(0.25f, 0.85f, 0.35f, 1.f));
                    ImGui::Text("%s", sr);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.f, 2.f);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.55f, 0.38f, 1.f));
                ImGui::Text("%.2f", price);
                ImGui::PopStyleColor();
            }
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

    // ── Bid rows: highest (best) first, descending to lowest ────────────────────
    // m_bids sorted high→low: index 0 = best bid (highest) → nearest to spread.
    for (int i = 0; i < nBids; ++i) {
        const auto& lvl  = m_bids[i];
        const bool  best = (i == 0);  // best = highest price, closest to spread
        const ImVec4 col = FlashCol(kBuyGreen, lvl.flashAge);
        const double pnl = CalcPnl(lvl.price);

        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            best ? IM_COL32(18, 90, 32, 110) : IM_COL32(15, 60, 22, 70));

        ImGui::TableSetColumnIndex(0);
        RowOverlay(lvl.price, 'b');
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("%.0f", lvl.size);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.38f, 0.72f, 0.42f, 1.f));
        ImGui::Text("%.0f", bidCum[i]);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(2);
        PriceClickCell(lvl.price, false);  // click bid price → SELL
        {
            const char* sr = srTag(lvl.price);
            if (sr) {
                bool isResistance = (sr[0] == 'R');
                ImGui::PushStyleColor(ImGuiCol_Text,
                    isResistance ? ImVec4(0.95f, 0.40f, 0.30f, 1.f)
                                 : ImVec4(0.25f, 0.85f, 0.35f, 1.f));
                ImGui::Text("%s", sr);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.f, 2.f);
            }
            bool isLast = (m_lastPrice > 0.0 && std::abs(lvl.price - m_lastPrice) < 0.005);
            ImGui::PushStyleColor(ImGuiCol_Text, isLast ? ImVec4(1.0f, 0.85f, 0.2f, 1.f)
                                  : best ? col : ImVec4(0.82f, 0.82f, 0.85f, 1.f));
            if (m_useL2 && !lvl.exchange.empty())
                ImGui::Text("%.2f [%s]%s", lvl.price, lvl.exchange.c_str(), isLast ? " *" : "");
            else
                ImGui::Text("%.2f%s", lvl.price, isLast ? " *" : "");
            ImGui::PopStyleColor();
        }

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
        auto applySymbol = [this]() {
            m_bids.clear();
            m_asks.clear();
            m_askBuckets.clear();
            m_bidBuckets.clear();
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
        };
        DrawSymbolInput("##sym", m_symbol, sizeof(m_symbol), em(72),
                        [&](const std::string&) { applySymbol(); });
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
    static constexpr const char* kTypes[] = {
        "Market", "Limit", "Stop", "Stop Limit",          // 0-3
        "Trail Stop", "Trail Limit",                       // 4-5
        "Market On Close", "Limit On Close", "Mkt To Limit", // 6-8
        "Mkt If Touched", "Lmt If Touched",               // 9-10
        "Midprice", "Relative"                             // 11-12
    };
    ImGui::Text("Type");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(160));
    ImGui::Combo("##type", &m_typeIdx, kTypes, IM_ARRAYSIZE(kTypes));

    // ---- Quantity -----------------------------------------------------------
    ImGui::Text("Quantity");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(100));
    ImGui::InputText("##qty", m_qtyBuf, sizeof(m_qtyBuf),
                     ImGuiInputTextFlags_CharsDecimal);

    // ---- Conditional price fields per type ----------------------------------
    // idx 1  LMT          → Limit Price
    // idx 2  STP          → Stop Price
    // idx 3  STP LMT      → Stop Price + Limit Price
    // idx 4  TRAIL        → Trail Amt ($/%) toggle + optional Stop Cap
    // idx 5  TRAIL LIMIT  → Trail Amt + Stop Cap (opt.) + Lmt Offset
    // idx 7  LOC          → Limit Price
    // idx 9  MIT          → Trigger Price  (reuses m_stpBuf)
    // idx 10 LIT          → Trigger Price + Limit Price
    // idx 11 MIDPRICE     → Price Cap (optional, reuses m_lmtBuf)
    // idx 12 REL          → Offset Amt + Price Cap (optional)
    // idx 0,6,8 — no price fields

    bool showLmt     = (m_typeIdx == 1 || m_typeIdx == 3 || m_typeIdx == 7 || m_typeIdx == 10);
    bool showStp     = (m_typeIdx == 2 || m_typeIdx == 3);
    bool showTrigger = (m_typeIdx == 9 || m_typeIdx == 10);
    bool showTrail   = (m_typeIdx == 4 || m_typeIdx == 5);
    bool showLmtOff  = (m_typeIdx == 5);
    bool showCapOpt  = (m_typeIdx == 11 || m_typeIdx == 12);
    bool showOffset  = (m_typeIdx == 12);

    if (showLmt) {
        ImGui::Text("Lmt Price");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##lmt", m_lmtBuf, sizeof(m_lmtBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        if (m_typeIdx == 1 || m_typeIdx == 7 || m_typeIdx == 10) {
            ImGui::SameLine();
            if (ImGui::SmallButton(isBuy ? "Ask" : "Bid")) {
                double fillPrice = isBuy ? (m_asks.empty() ? m_midPrice : m_asks[0].price)
                                         : (m_bids.empty() ? m_midPrice : m_bids[0].price);
                std::snprintf(m_lmtBuf, sizeof(m_lmtBuf), "%.2f", fillPrice);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fill from best %s", isBuy ? "ask" : "bid");
        }
    }

    if (showStp) {
        ImGui::Text("Stp Price");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##stp", m_stpBuf, sizeof(m_stpBuf),
                         ImGuiInputTextFlags_CharsDecimal);
    }

    if (showTrigger) {
        ImGui::Text("Trigger");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##stp", m_stpBuf, sizeof(m_stpBuf),
                         ImGuiInputTextFlags_CharsDecimal);
    }

    if (showTrail) {
        // $/% toggle button
        ImGui::Text(m_trailByPct ? "Trail %%" : "Trail $");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(80));
        if (m_trailByPct)
            ImGui::InputText("##trailpct", m_trailPctBuf, sizeof(m_trailPctBuf),
                             ImGuiInputTextFlags_CharsDecimal);
        else
            ImGui::InputText("##trailamt", m_trailAmtBuf, sizeof(m_trailAmtBuf),
                             ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::SmallButton(m_trailByPct ? "$" : "%"))
            m_trailByPct = !m_trailByPct;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Switch between trailing by %s",
                              m_trailByPct ? "percent and dollar amount" : "dollar amount and percent");

        // Optional initial stop cap
        ImGui::Text("Stop Cap");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##stpcap", m_stpBuf, sizeof(m_stpBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Optional: sets the initial stop price.\nLeave blank to let IB compute from current price.");
    }

    if (showLmtOff) {
        ImGui::Text("Lmt Offset");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##lmtoff", m_lmtBuf, sizeof(m_lmtBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Limit price offset from the trail stop price.\nPositive = limit above stop (buy), negative = below (sell).");
    }

    if (showCapOpt) {
        const char* capLabel = (m_typeIdx == 11) ? "Price Cap" : "Cap (opt.)";
        ImGui::Text("%s", capLabel);
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##cap", m_lmtBuf, sizeof(m_lmtBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Optional maximum (buy) or minimum (sell) price.\nLeave blank for no cap.");
    }

    if (showOffset) {
        ImGui::Text("Peg Offset");
        ImGui::SameLine(labelCol);
        ImGui::SetNextItemWidth(em(100));
        ImGui::InputText("##offset", m_offsetBuf, sizeof(m_offsetBuf),
                         ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Dollar offset from the primary quote.\nNegative = below (buy), positive = above (sell).");
    }

    // ---- Time In Force ------------------------------------------------------
    // MOC/LOC must be DAY-only; auto-clamp and grey out the combo.
    bool tifLocked = (m_typeIdx == 6 || m_typeIdx == 7); // MOC, LOC
    if (tifLocked) m_tifIdx = 0;

    const char* tifs[] = {"DAY", "GTC", "IOC", "FOK", "OVERNIGHT", "OPG"};
    ImGui::Text("TIF");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(80));
    if (tifLocked) ImGui::BeginDisabled();
    ImGui::Combo("##tif", &m_tifIdx, tifs, IM_ARRAYSIZE(tifs));
    if (tifLocked) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("On-Close orders must use DAY TIF.");
    }

    // ---- Exchange routing ---------------------------------------------------
    ImGui::Text("Exchange");
    ImGui::SameLine(labelCol);
    ImGui::SetNextItemWidth(em(120));
    const char* exchPreview = m_exchangeIdx < (int)m_exchangeList.size()
                              ? m_exchangeList[m_exchangeIdx].c_str() : "SMART";
    if (ImGui::BeginCombo("##exch", exchPreview)) {
        for (int i = 0; i < (int)m_exchangeList.size(); ++i) {
            bool sel = (i == m_exchangeIdx);
            if (ImGui::Selectable(m_exchangeList[i].c_str(), sel))
                m_exchangeIdx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("SMART = IB smart routing.\nSpecific exchange = direct route (bypasses smart routing).");

    // ---- Outside RTH --------------------------------------------------------
    // Close orders (MOC/LOC) can't be outside RTH — disable the checkbox.
    bool rthLocked = (m_typeIdx == 6 || m_typeIdx == 7);
    if (rthLocked) m_outsideRth = false;

    ImGui::Spacing();
    if (rthLocked) ImGui::BeginDisabled();
    ImGui::Checkbox("Outside RTH", &m_outsideRth);
    if (rthLocked) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("On-Close orders always execute at the regular session close.");
    } else {
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Allow order to be active and fill during pre-market\n"
                "and after-hours sessions.\n"
                "Requires a limit price — market orders are rejected\n"
                "outside regular trading hours by IB.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Price summary for the submit label and order preview ---------------
    // Returns a short string describing the key price(s) for the current type.
    char priceSummary[96] = "";
    switch (m_typeIdx) {
        case 0: case 6: case 8:  // MKT, MOC, MTL
            std::snprintf(priceSummary, sizeof(priceSummary), "MKT");
            break;
        case 1: case 7:  // LMT, LOC
            std::snprintf(priceSummary, sizeof(priceSummary), "$%s", m_lmtBuf);
            break;
        case 2:  // STP
            std::snprintf(priceSummary, sizeof(priceSummary), "stp $%s", m_stpBuf);
            break;
        case 3:  // STP LMT
            std::snprintf(priceSummary, sizeof(priceSummary), "stp $%s / lmt $%s",
                          m_stpBuf, m_lmtBuf);
            break;
        case 4:  // TRAIL
            if (m_trailByPct)
                std::snprintf(priceSummary, sizeof(priceSummary), "%s%% trail",
                              m_trailPctBuf);
            else
                std::snprintf(priceSummary, sizeof(priceSummary), "$%s trail",
                              m_trailAmtBuf);
            break;
        case 5:  // TRAIL LIMIT
            if (m_trailByPct)
                std::snprintf(priceSummary, sizeof(priceSummary), "%s%% / off $%s",
                              m_trailPctBuf, m_lmtBuf);
            else
                std::snprintf(priceSummary, sizeof(priceSummary), "$%s / off $%s",
                              m_trailAmtBuf, m_lmtBuf);
            break;
        case 9:  // MIT
            std::snprintf(priceSummary, sizeof(priceSummary), "trig $%s", m_stpBuf);
            break;
        case 10: // LIT
            std::snprintf(priceSummary, sizeof(priceSummary), "trig $%s / lmt $%s",
                          m_stpBuf, m_lmtBuf);
            break;
        case 11: // MIDPRICE
            if (m_lmtBuf[0] != '\0')
                std::snprintf(priceSummary, sizeof(priceSummary), "mid / cap $%s",
                              m_lmtBuf);
            else
                std::snprintf(priceSummary, sizeof(priceSummary), "mid");
            break;
        case 12: // REL
            std::snprintf(priceSummary, sizeof(priceSummary), "off $%s", m_offsetBuf);
            break;
        default: break;
    }

    // ---- Order preview ------------------------------------------------------
    double qty    = std::atof(m_qtyBuf);
    double lmt    = std::atof(m_lmtBuf);
    double refP   = (showLmt && lmt > 0) ? lmt : m_midPrice;
    double estVal = qty * refP;

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("Est. value: $%.2f  |  %s  %s  %s",
                estVal,
                m_sideIdx == 0 ? "BUY" : "SELL",
                kTypes[m_typeIdx],
                tifs[m_tifIdx]);
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ---- Order-impact badge -------------------------------------------------
    DrawOrderImpactBadge();

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

    char submitLabel[96];
    std::snprintf(submitLabel, sizeof(submitLabel),
                  "%s  %s  %.0f @ %s",
                  isBuy ? "BUY" : "SELL",
                  kTypes[m_typeIdx], qty,
                  priceSummary);

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

    // DOM ladder legend
    ImGui::Spacing();
    ImGui::SeparatorText("Ladder");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 1));
    auto LegendDot = [](ImU32 col, const char* label) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float  h = ImGui::GetTextLineHeight();
        dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + h - 4, p.y + h - 2), col);
        ImGui::Dummy(ImVec2(h, 0)); ImGui::SameLine(0, 4);
        ImGui::TextUnformatted(label);
    };
    LegendDot(IM_COL32(35,  170, 65,  220), "Bid depth");
    LegendDot(IM_COL32(190, 45,  45,  220), "Ask depth");
    LegendDot(IM_COL32(55,  130, 210, 220), "Executed volume");
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
        static constexpr const char* kTypes[] = {
            "Market", "Limit", "Stop", "Stop Limit",
            "Trail Stop", "Trail Limit",
            "Market On Close", "Limit On Close", "Mkt To Limit",
            "Mkt If Touched", "Lmt If Touched",
            "Midprice", "Relative"
        };
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

        ImGui::Text("Symbol:    %s",          m_symbol);
        ImGui::Text("Type:      %s",          kTypes[m_typeIdx]);
        ImGui::Text("Quantity:  %.0f shares", qty);
        // Show relevant price fields per type
        if (m_typeIdx == 1 || m_typeIdx == 7 || m_typeIdx == 10) // LMT, LOC, LIT
            ImGui::Text("Limit:     $%.2f", lmt);
        if (m_typeIdx == 2 || m_typeIdx == 3)  // STP, STP LMT
            ImGui::Text("Stop:      $%.2f", stp);
        if (m_typeIdx == 3)                    // STP LMT also shows limit
            ImGui::Text("Limit:     $%.2f", lmt);
        if (m_typeIdx == 4 || m_typeIdx == 5) { // TRAIL, TRAIL LIMIT
            if (m_trailByPct) ImGui::Text("Trail %%:   %.2f%%", std::atof(m_trailPctBuf));
            else              ImGui::Text("Trail $:   $%.2f",   std::atof(m_trailAmtBuf));
            if (stp > 0)      ImGui::Text("Stop Cap:  $%.2f",   stp);
        }
        if (m_typeIdx == 5)                    // TRAIL LIMIT also shows lmt offset
            ImGui::Text("Lmt Offset:$%.2f", lmt);
        if (m_typeIdx == 9)                    // MIT trigger
            ImGui::Text("Trigger:   $%.2f", stp);
        if (m_typeIdx == 10) {                 // LIT trigger + limit (limit shown above)
            ImGui::Text("Trigger:   $%.2f", stp);
        }
        if (m_typeIdx == 11 && lmt > 0)        // MIDPRICE cap
            ImGui::Text("Price Cap: $%.2f", lmt);
        if (m_typeIdx == 12) {                 // REL
            ImGui::Text("Offset:    $%.4f", std::atof(m_offsetBuf));
            if (lmt > 0) ImGui::Text("Cap:       $%.2f", lmt);
        }
        ImGui::Text("TIF:       %s",           tifs[m_tifIdx]);
        if (m_exchangeIdx > 0 && m_exchangeIdx < (int)m_exchangeList.size())
            ImGui::Text("Exchange:  %s", m_exchangeList[m_exchangeIdx].c_str());
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

    switch (m_typeIdx) {
        case 1:  // LMT
        case 7:  // LOC
        {
            double lmt = std::atof(m_lmtBuf);
            if (lmt <= 0) { err = "Limit price must be > 0"; return false; }
            break;
        }
        case 2:  // STP
        {
            double stp = std::atof(m_stpBuf);
            if (stp <= 0) { err = "Stop price must be > 0"; return false; }
            break;
        }
        case 3:  // STP LMT
        {
            double stp = std::atof(m_stpBuf);
            double lmt = std::atof(m_lmtBuf);
            if (stp <= 0) { err = "Stop price must be > 0"; return false; }
            if (lmt <= 0) { err = "Limit price must be > 0"; return false; }
            break;
        }
        case 4:  // TRAIL
        {
            double amt = m_trailByPct ? std::atof(m_trailPctBuf) : std::atof(m_trailAmtBuf);
            if (amt <= 0) { err = "Trailing amount must be > 0"; return false; }
            break;
        }
        case 5:  // TRAIL LIMIT
        {
            double amt = m_trailByPct ? std::atof(m_trailPctBuf) : std::atof(m_trailAmtBuf);
            if (amt <= 0) { err = "Trailing amount must be > 0"; return false; }
            double off = std::atof(m_lmtBuf);
            if (off == 0.0) { err = "Limit offset must be non-zero"; return false; }

            double stp = std::atof(m_stpBuf);
            if (stp <= 0.0 && m_midPrice <= 0.0) {
                err = "Need Stop Cap if market price is unavailable";
                return false;
            }
            break;
        }
        case 9:  // MIT
        {
            double trig = std::atof(m_stpBuf);
            if (trig <= 0) { err = "Trigger price must be > 0"; return false; }
            break;
        }
        case 10: // LIT
        {
            double trig = std::atof(m_stpBuf);
            double lmt  = std::atof(m_lmtBuf);
            if (trig <= 0) { err = "Trigger price must be > 0"; return false; }
            if (lmt  <= 0) { err = "Limit price must be > 0";   return false; }
            break;
        }
        case 12: // REL
        {
            double off = std::atof(m_offsetBuf);
            if (off == 0.0) { err = "Peg offset must be non-zero"; return false; }
            break;
        }
        default: break;  // MKT(0), MOC(6), MTL(8), MIDPRICE(11) — no price required
    }
    return true;
}

void TradingWindow::SubmitOrder() {
    // Explicit typeIdx → OrderType mapping (no fragile enum cast)
    static constexpr core::OrderType kTypeMap[] = {
        core::OrderType::Market,   // 0
        core::OrderType::Limit,    // 1
        core::OrderType::Stop,     // 2
        core::OrderType::StopLimit,// 3
        core::OrderType::Trail,    // 4
        core::OrderType::TrailLimit,// 5
        core::OrderType::MOC,      // 6
        core::OrderType::LOC,      // 7
        core::OrderType::MTL,      // 8
        core::OrderType::MIT,      // 9
        core::OrderType::LIT,      // 10
        core::OrderType::Midprice, // 11
        core::OrderType::Relative, // 12
    };

    core::Order o;
    o.orderId     = m_nextOrderId++;
    o.symbol      = m_symbol;
    o.side        = (m_sideIdx == 0) ? core::OrderSide::Buy : core::OrderSide::Sell;
    o.type        = kTypeMap[m_typeIdx];
    o.tif         = static_cast<core::TimeInForce>(m_tifIdx);
    o.quantity    = std::atof(m_qtyBuf);
    o.outsideRth  = m_outsideRth;
    o.exchange    = (m_exchangeIdx < (int)m_exchangeList.size())
                    ? m_exchangeList[m_exchangeIdx] : "SMART";
    o.status      = core::OrderStatus::Working;
    o.submittedAt = std::time(nullptr);
    o.updatedAt   = o.submittedAt;

    // Populate price fields per type
    switch (m_typeIdx) {
        case 1:  // LMT
        case 7:  // LOC
            o.limitPrice = std::atof(m_lmtBuf);
            break;
        case 2:  // STP
            o.stopPrice = std::atof(m_stpBuf);
            break;
        case 3:  // STP LMT
            o.stopPrice  = std::atof(m_stpBuf);
            o.limitPrice = std::atof(m_lmtBuf);
            break;
        case 4:  // TRAIL
        case 5:  // TRAIL LIMIT
        {
            double trailAmtVal = 0.0;
            if (m_trailByPct) {
                o.trailingPercent = std::atof(m_trailPctBuf);
                trailAmtVal = m_midPrice * (o.trailingPercent / 100.0);
            } else {
                o.auxPrice = std::atof(m_trailAmtBuf);
                trailAmtVal = o.auxPrice;
            }

            o.trailStopPrice = std::atof(m_stpBuf);
            if (o.trailStopPrice <= 0.0) {
                if (m_midPrice > 0.0) {
                    // Default initial stop if left blank
                    if (o.side == core::OrderSide::Buy)
                        o.trailStopPrice = m_midPrice + trailAmtVal;
                    else
                        o.trailStopPrice = m_midPrice - trailAmtVal;
                } else {
                    // We need a stop price for TRAIL LIMIT but have no market price.
                    // Fallback: if it's a TRAIL LIMIT and we are BUYING, 0.0 is definitely wrong.
                    // But we can't guess. For now, let's at least try to avoid sending 0.
                    // IBKRClient will check this too.
                }
            }

            if (m_typeIdx == 5)
                o.lmtPriceOffset = std::atof(m_lmtBuf);
            break;
        }
        case 9:  // MIT
            o.auxPrice = std::atof(m_stpBuf);
            break;
        case 10: // LIT
            o.auxPrice   = std::atof(m_stpBuf);  // trigger
            o.limitPrice = std::atof(m_lmtBuf);
            break;
        case 11: // MIDPRICE — optional cap
            if (m_lmtBuf[0] != '\0')
                o.limitPrice = std::atof(m_lmtBuf);
            break;
        case 12: // REL
            o.auxPrice = std::atof(m_offsetBuf);
            if (m_lmtBuf[0] != '\0')
                o.limitPrice = std::atof(m_lmtBuf);
            break;
        default: break; // MKT(0), MOC(6), MTL(8)
    }

    if (OnOrderSubmit)
        OnOrderSubmit(o);

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

void TradingWindow::SetExchangeList(const std::vector<std::string>& exchanges) {
    m_exchangeList = exchanges;
    m_exchangeIdx  = 0;
}

void TradingWindow::SetPosition(double qty, double avgCost) {
    m_positionQty   = qty;
    m_avgEntryPrice = (std::abs(qty) > 1e-9) ? avgCost : 0.0;
}

void TradingWindow::SetAutoLevels(const std::vector<double>& supports,
                                   const std::vector<double>& resistances,
                                   double atrLast) {
    m_supportPrices    = supports;
    m_resistancePrices = resistances;
    m_atrForSR         = atrLast;
}

void TradingWindow::SetUnguardedSuggestion(const UnguardedHint& h) {
    // A position-quantity change clears the per-symbol dismissal.
    if (std::abs(h.qty - m_lastWarnedQty) > 1e-9) {
        m_dismissedUnguarded.erase(h.symbol);
        m_lastWarnedQty = h.qty;
    }
    m_unguarded = h;
}

// ============================================================================
// DrawOrderImpactBadge — side-intent + PnL preview below the order-entry form.
//
// Reads the current form buffers (side, type, qty, prices) to derive an
// estimated fill price, then computes and renders what the order would do to
// the current position. Pure visualisation — no behaviour change.
// ============================================================================
void TradingWindow::DrawOrderImpactBadge() {
    // Parse quantity
    double qty = std::atof(m_qtyBuf);
    if (qty <= 0.0) return;

    bool isBuy = (m_sideIdx == 0);
    double last = m_midPrice;
    double fillPrice = 0.0;

    // ── Derive fill price from the form buffers ────────────────────────────
    static constexpr const char* kTypes[] = {
        "Market", "Limit", "Stop", "Stop Limit",
        "Trail Stop", "Trail Limit",
        "Market On Close", "Limit On Close", "Mkt To Limit",
        "Mkt If Touched", "Lmt If Touched",
        "Midprice", "Relative"
    };

    switch (m_typeIdx) {
        case 0:  // Market
        case 6:  // MOC
        case 8:  // MTL
        case 11: // Midprice
            if (last <= 0.0) return;
            fillPrice = last;
            break;
        case 1:  // Limit
        case 7:  // LOC
        case 10: // LIT — use limit price for fill leg
            fillPrice = std::atof(m_lmtBuf);
            break;
        case 2:  // Stop
        case 9:  // MIT
            fillPrice = std::atof(m_stpBuf);
            break;
        case 3:  // StopLimit — use stop as best estimate
            fillPrice = std::atof(m_stpBuf);
            break;
        case 4:  // Trail Stop
        case 5:  // Trail Limit
            if (last <= 0.0) return;
            {
                double trailAmt = std::atof(m_trailAmtBuf);
                double trailPct = std::atof(m_trailPctBuf);
                double trail = m_trailByPct ? (last * trailPct / 100.0) : trailAmt;
                fillPrice = isBuy ? (last - trail) : (last + trail);
            }
            break;
        case 12: // Relative
            if (last <= 0.0) return;
            {
                double peg = std::atof(m_offsetBuf);
                fillPrice = isBuy ? (last + peg) : (last - peg);
            }
            break;
        default: return;
    }

    if (fillPrice <= 0.0) return;

    // ── Compute impact ─────────────────────────────────────────────────────
    double posQty = m_positionQty;
    double avgCost = m_avgEntryPrice;
    double commPerShare = 0.0;   // TradingWindow does not track commission

    auto imp = core::services::ComputeOrderImpact(posQty, avgCost, commPerShare,
                                                   isBuy, qty, fillPrice);
    if (imp.kind == core::services::OrderImpactKind::Invalid) return;

    // ── Colour palette ─────────────────────────────────────────────────────
    static constexpr ImVec4 kOpenAddBg   = ImVec4(0.05f, 0.12f, 0.22f, 0.90f);
    static constexpr ImVec4 kOpenAddBdr  = ImVec4(0.20f, 0.50f, 0.85f, 0.90f);
    static constexpr ImVec4 kGreenBg     = ImVec4(0.05f, 0.18f, 0.08f, 0.90f);
    static constexpr ImVec4 kGreenBdr    = ImVec4(0.15f, 0.65f, 0.25f, 0.90f);
    static constexpr ImVec4 kRedBg       = ImVec4(0.22f, 0.08f, 0.05f, 0.90f);
    static constexpr ImVec4 kRedBdr      = ImVec4(0.80f, 0.25f, 0.15f, 0.90f);
    static constexpr ImVec4 kFlipBg      = ImVec4(0.25f, 0.15f, 0.05f, 0.90f);
    static constexpr ImVec4 kFlipBdr     = ImVec4(0.90f, 0.55f, 0.10f, 0.90f);

    ImVec4 bgCol, bdrCol, textCol;
    bool isOpenOrAdd = false;

    switch (imp.kind) {
        case core::services::OrderImpactKind::OpenLong:
        case core::services::OrderImpactKind::OpenShort:
        case core::services::OrderImpactKind::AddToLong:
        case core::services::OrderImpactKind::AddToShort:
            bgCol = kOpenAddBg;  bdrCol = kOpenAddBdr;
            textCol = ImVec4(0.40f, 0.70f, 1.00f, 1.f);
            isOpenOrAdd = true;
            break;
        case core::services::OrderImpactKind::ReduceLong:
        case core::services::OrderImpactKind::ReduceShort:
        case core::services::OrderImpactKind::CloseLong:
        case core::services::OrderImpactKind::CloseShort:
            if (imp.closePnL > 0.0) {
                bgCol = kGreenBg; bdrCol = kGreenBdr;
                textCol = ImVec4(0.25f, 0.90f, 0.35f, 1.f);
            } else {
                bgCol = kRedBg; bdrCol = kRedBdr;
                textCol = ImVec4(0.95f, 0.40f, 0.30f, 1.f);
            }
            break;
        case core::services::OrderImpactKind::FlipToShort:
        case core::services::OrderImpactKind::FlipToLong:
            bgCol = kFlipBg; bdrCol = kFlipBdr;
            textCol = ImVec4(1.00f, 0.70f, 0.20f, 1.f);
            break;
        default: return;
    }

    float stripH = ImGui::GetTextLineHeightWithSpacing()
                 + ImGui::GetStyle().FramePadding.y * 2.0f + 6.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg,    bgCol);
    ImGui::PushStyleColor(ImGuiCol_Border,     bdrCol);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    ImGui::BeginChild("##orderimpact", ImVec2(-1, stripH),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, textCol);

    char buf[200];
    const char* kindStr = "";
    switch (imp.kind) {
        case core::services::OrderImpactKind::OpenLong:     kindStr = "OPEN LONG";      break;
        case core::services::OrderImpactKind::OpenShort:    kindStr = "OPEN SHORT";     break;
        case core::services::OrderImpactKind::AddToLong:    kindStr = "ADD TO LONG";    break;
        case core::services::OrderImpactKind::AddToShort:   kindStr = "ADD TO SHORT";   break;
        case core::services::OrderImpactKind::ReduceLong:   kindStr = "REDUCE LONG";    break;
        case core::services::OrderImpactKind::ReduceShort:  kindStr = "REDUCE SHORT";   break;
        case core::services::OrderImpactKind::CloseLong:    kindStr = "CLOSE LONG";     break;
        case core::services::OrderImpactKind::CloseShort:   kindStr = "CLOSE SHORT";    break;
        case core::services::OrderImpactKind::FlipToShort:  kindStr = "FLIP TO SHORT";  break;
        case core::services::OrderImpactKind::FlipToLong:   kindStr = "FLIP TO LONG";   break;
        default: break;
    }

    if (isOpenOrAdd) {
        double cost = fillPrice * qty;
        std::snprintf(buf, sizeof(buf), "  %s  ·  %.0f sh @ $%.2f  ·  cost ~ $%'.0f",
                      kindStr, qty, fillPrice, cost);
    } else if (imp.kind == core::services::OrderImpactKind::FlipToShort ||
               imp.kind == core::services::OrderImpactKind::FlipToLong) {
        const char* openDir = (imp.kind == core::services::OrderImpactKind::FlipToShort)
                              ? "short" : "long";
        std::snprintf(buf, sizeof(buf), "  %s  ·  close %.0f (%+.2f)  ->  open %.0f %s @ $%.2f",
                      kindStr, imp.closeQty, imp.closePnL,
                      imp.openQty, openDir, fillPrice);
    } else {
        double pctPnL = 0.0;
        if (imp.closeQty > 0.0 && avgCost > 0.0)
            pctPnL = (imp.closePnL / (avgCost * imp.closeQty)) * 100.0;
        std::snprintf(buf, sizeof(buf), "  %s  ·  %.0f sh  ·  est. PnL %+.2f (%+.2f%%)",
                      kindStr, imp.closeQty, imp.closePnL, pctPnL);
    }
    ImGui::TextUnformatted(buf);

    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// ============================================================================
// DrawUnguardedStrip — yellow protective-stop warning above the order entry
// panel. "Place stop" pre-populates the order-entry buffers (StopLimit on the
// opposite side of the position) and triggers the existing confirmation modal,
// so the user reviews the order before sending. "Dismiss" hides the warning
// for this symbol until the position quantity changes.
// ============================================================================
void TradingWindow::DrawUnguardedStrip() {
    if (!m_unguarded.active)               return;
    if (m_unguarded.symbol != m_symbol)    return;
    if (m_dismissedUnguarded.count(m_symbol)) return;
    if (m_unguarded.stopTrig <= 0.0)       return;

    bool   isLong = (m_unguarded.qty > 0.0);
    double absQty = std::abs(m_unguarded.qty);

    static constexpr ImVec4 kStripBg     = ImVec4(0.30f, 0.24f, 0.05f, 0.90f);
    static constexpr ImVec4 kStripBorder = ImVec4(1.00f, 0.80f, 0.20f, 0.90f);
    static constexpr ImVec4 kWarnText    = ImVec4(1.00f, 0.85f, 0.25f, 1.00f);

    float stripH = ImGui::GetTextLineHeightWithSpacing()
                 + ImGui::GetStyle().FramePadding.y * 2.0f + 6.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kStripBg);
    ImGui::PushStyleColor(ImGuiCol_Border,  kStripBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    ImGui::BeginChild("##trade_unguarded", ImVec2(-1, stripH),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    FlexRow row;

    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "WARNING  %s %s %.0f sh @ $%.2f - no protective stop. "
                  "Suggested $%.2f (-%.2f%%).",
                  m_symbol,
                  isLong ? "long" : "short",
                  absQty,
                  m_unguarded.avgCost,
                  m_unguarded.stopTrig,
                  m_unguarded.pctRisk);
    row.item(FlexRow::textW(msg), 0);
    ImGui::PushStyleColor(ImGuiCol_Text, kWarnText);
    ImGui::TextUnformatted(msg);
    ImGui::PopStyleColor();

    bool placeClicked   = false;
    bool dismissClicked = false;

    const float kPlaceW   = em(110);
    const float kDismissW = em(80);

    row.item(kPlaceW, 12);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.42f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.60f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.42f, 0.32f, 0.06f, 1.f));
    placeClicked = ImGui::Button("Place stop", ImVec2(kPlaceW, 0));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pre-fill the Order Entry with a Stop-Limit on the "
                          "opposite side of the position; review before sending.");

    row.item(kDismissW, 4);
    dismissClicked = ImGui::Button("Dismiss", ImVec2(kDismissW, 0));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hide this warning until the position quantity "
                          "changes.");

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    if (dismissClicked) {
        m_dismissedUnguarded.insert(m_symbol);
        return;
    }

    if (placeClicked) {
        // Pre-populate the order-entry fields so the existing
        // DrawConfirmationPopup() reads the right values, then trigger it.
        m_sideIdx = isLong ? 1 : 0;     // Sell for long, Buy for short
        m_typeIdx = 3;                  // StopLimit
        m_tifIdx  = 0;                  // DAY
        m_outsideRth = false;
        std::snprintf(m_qtyBuf, sizeof(m_qtyBuf), "%.0f", absQty);
        std::snprintf(m_stpBuf, sizeof(m_stpBuf), "%.2f", m_unguarded.stopTrig);
        std::snprintf(m_lmtBuf, sizeof(m_lmtBuf), "%.2f", m_unguarded.stopLmt);
        m_showConfirm = true;
    }
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
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("##orders", 14, tblFlags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID",     ImGuiTableColumnFlags_WidthFixed,  50);
    ImGui::TableSetupColumn("Symbol", ImGuiTableColumnFlags_WidthFixed,  60);
    ImGui::TableSetupColumn("Side",   ImGuiTableColumnFlags_WidthFixed,  42);
    ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthFixed,  68);
    ImGui::TableSetupColumn("Qty",    ImGuiTableColumnFlags_WidthFixed,  50);
    ImGui::TableSetupColumn("Price",  ImGuiTableColumnFlags_WidthFixed,  80);
    ImGui::TableSetupColumn("Aux",    ImGuiTableColumnFlags_WidthFixed,  72);
    ImGui::TableSetupColumn("TIF",    ImGuiTableColumnFlags_WidthFixed,  38);
    ImGui::TableSetupColumn("Filled", ImGuiTableColumnFlags_WidthFixed,  50);
    ImGui::TableSetupColumn("Avg $",  ImGuiTableColumnFlags_WidthFixed,  68);
    ImGui::TableSetupColumn("Comm",   ImGuiTableColumnFlags_WidthFixed,  58);
    ImGui::TableSetupColumn("Time",   ImGuiTableColumnFlags_WidthFixed,  60);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed,  52);
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

        // ID
        ImGui::TableNextColumn(); ImGui::Text("%d", o.orderId);
        // Symbol
        ImGui::TableNextColumn(); ImGui::TextUnformatted(o.symbol.c_str());
        // Side
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text,
            o.side == core::OrderSide::Buy ? kBuyGreen : kSellRed);
        ImGui::TextUnformatted(core::OrderSideStr(o.side));
        ImGui::PopStyleColor();
        // Type
        ImGui::TableNextColumn(); ImGui::TextUnformatted(core::OrderTypeStr(o.type));
        // Qty
        ImGui::TableNextColumn(); ImGui::Text("%.0f", o.quantity);

        // Price — main price per order type
        ImGui::TableNextColumn();
        switch (o.type) {
            case core::OrderType::Market:
            case core::OrderType::MOC:
            case core::OrderType::MTL:
                ImGui::TextDisabled("MKT");
                break;
            case core::OrderType::Stop:
            case core::OrderType::StopLimit:
                ImGui::Text("stp $%.2f", o.stopPrice);
                break;
            case core::OrderType::Trail:
            case core::OrderType::TrailLimit:
                if (o.trailingPercent > 0.0)
                    ImGui::Text("tr %.2f%%", o.trailingPercent);
                else
                    ImGui::Text("tr $%.2f", o.auxPrice);
                break;
            case core::OrderType::MIT:
                ImGui::Text("trig $%.2f", o.auxPrice);
                break;
            case core::OrderType::LIT:
                ImGui::Text("trig $%.2f", o.auxPrice);
                break;
            case core::OrderType::Midprice:
                ImGui::TextDisabled("mid");
                break;
            case core::OrderType::Relative:
                ImGui::Text("off $%.3f", o.auxPrice);
                break;
            default:  // LMT, LOC
                if (o.limitPrice > 0.0) ImGui::Text("$%.2f", o.limitPrice);
                else                    ImGui::TextDisabled("—");
                break;
        }
        if (ImGui::IsItemHovered()) {
            // Full price detail on hover
            ImGui::BeginTooltip();
            if (o.limitPrice  > 0.0) ImGui::Text("Limit:    $%.4f", o.limitPrice);
            if (o.stopPrice   > 0.0) ImGui::Text("Stop:     $%.4f", o.stopPrice);
            if (o.auxPrice    > 0.0) ImGui::Text("Aux:      $%.4f", o.auxPrice);
            if (o.trailingPercent > 0.0) ImGui::Text("Trail %%: %.4f", o.trailingPercent);
            if (o.trailStopPrice > 0.0)  ImGui::Text("Stop cap: $%.4f", o.trailStopPrice);
            if (o.lmtPriceOffset != 0.0) ImGui::Text("Lmt off:  $%.4f", o.lmtPriceOffset);
            ImGui::EndTooltip();
        }

        // Aux — secondary price for dual-leg / trail orders
        ImGui::TableNextColumn();
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
                ImGui::Text("off $%.3f", o.lmtPriceOffset);
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

        // TIF
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(core::TIFStr(o.tif));

        // Filled qty
        ImGui::TableNextColumn();
        if (o.filledQty > 0.0)
            ImGui::Text("%.0f", o.filledQty);
        else
            ImGui::TextDisabled("—");

        // Avg fill price
        ImGui::TableNextColumn();
        if (o.avgFillPrice > 0.0)
            ImGui::Text("$%.2f", o.avgFillPrice);
        else
            ImGui::TextDisabled("—");

        // Commission
        ImGui::TableNextColumn();
        if (o.commission > 0.0)
            ImGui::Text("-$%.2f", o.commission);
        else
            ImGui::TextDisabled("—");

        // Submitted time
        ImGui::TableNextColumn();
        if (o.submittedAt != 0) {
            char tbuf[16];
            std::tm* lt = std::localtime(&o.submittedAt);
            std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", lt);
            ImGui::TextDisabled("%s", tbuf);
        } else {
            ImGui::TextDisabled("—");
        }

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
        ImGui::TextUnformatted(core::OrderStatusStr(o.status));
        ImGui::PopStyleColor();
        if (!o.rejectReason.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", o.rejectReason.c_str());

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
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_Resizable;

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
// Draw — Time & Sales
// ============================================================================
void TradingWindow::DrawTimeSales() {
    // Recompute max visible size for histogram bar scaling
    double maxVis = 1.0;
    for (const auto& t : m_ticks) if (t.size > maxVis) maxVis = t.size;
    m_maxTickSize = maxVis;

    if (m_ticks.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("  Waiting for tick-by-tick data...");
        return;
    }

    static ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("##ts", 5, flags, ImVec2(-1, -1))) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed,   62);
    ImGui::TableSetupColumn("Price", ImGuiTableColumnFlags_WidthFixed,   72);
    ImGui::TableSetupColumn("Size",  ImGuiTableColumnFlags_WidthFixed,   58);
    ImGui::TableSetupColumn("Vol",   ImGuiTableColumnFlags_WidthFixed,   72);
    ImGui::TableSetupColumn("Exch",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();

    for (const auto& t : m_ticks) {
        ImGui::TableNextRow();

        ImVec4 rowTint = t.isNeutral
            ? ImVec4(0.22f, 0.22f, 0.25f, 0.35f)
            : t.isUptick ? ImVec4(0.08f, 0.22f, 0.10f, 0.35f)
                         : ImVec4(0.24f, 0.08f, 0.08f, 0.35f);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(rowTint));

        ImVec4 textCol = t.isNeutral
            ? ImVec4(0.65f, 0.65f, 0.68f, 1.0f)
            : t.isUptick ? kBuyGreen : kSellRed;

        // 0 — Time
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.58f, 1.f));
        ImGui::TextUnformatted(FmtTime(t.timestamp).c_str());
        ImGui::PopStyleColor();

        // 1 — Price
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
        ImGui::Text("%.2f", t.price);
        ImGui::PopStyleColor();

        // 2 — Size
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
        ImGui::Text("%.0f", t.size);
        ImGui::PopStyleColor();

        // 3 — Size histogram bar
        ImGui::TableSetColumnIndex(3);
        float frac = (m_maxTickSize > 0.0) ? (float)(t.size / m_maxTickSize) : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
            t.isNeutral ? ImVec4(0.42f, 0.42f, 0.45f, 0.85f)
          : t.isUptick  ? ImVec4(0.18f, 0.68f, 0.32f, 0.85f)
                        : ImVec4(0.78f, 0.22f, 0.22f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.10f, 0.10f, 0.12f, 0.5f));
        ImGui::ProgressBar(frac, ImVec2(-1.0f, ImGui::GetTextLineHeight()));
        ImGui::PopStyleColor(2);

        // 4 — Exchange / Conditions
        ImGui::TableSetColumnIndex(4);
        if (!t.exchange.empty() || !t.specialConds.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.52f, 0.52f, 0.56f, 1.f));
            if (t.specialConds.empty())
                ImGui::TextUnformatted(t.exchange.c_str());
            else if (t.exchange.empty())
                ImGui::TextUnformatted(t.specialConds.c_str());
            else
                ImGui::Text("%s %s", t.exchange.c_str(), t.specialConds.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
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
