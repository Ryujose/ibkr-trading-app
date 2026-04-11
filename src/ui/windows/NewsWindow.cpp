#include "ui/UiScale.h"
#include "ui/windows/NewsWindow.h"
#include "imgui.h"
#include "core/models/WindowGroup.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace ui {

// ============================================================================
// Palette helpers
// ============================================================================
static ImVec4 SentimentColor(core::NewsSentiment s) {
    switch (s) {
        case core::NewsSentiment::Positive: return ImVec4(0.20f, 0.85f, 0.40f, 1.0f);
        case core::NewsSentiment::Negative: return ImVec4(0.90f, 0.28f, 0.28f, 1.0f);
        default:                            return ImVec4(0.60f, 0.60f, 0.62f, 1.0f);
    }
}

static const char* SentimentLabel(core::NewsSentiment s) {
    switch (s) {
        case core::NewsSentiment::Positive: return "POS";
        case core::NewsSentiment::Negative: return "NEG";
        default:                            return "NEU";
    }
}

// ============================================================================
// Constructor
// ============================================================================
NewsWindow::NewsWindow() {
    // reqId routing is set later by main.cpp via SetStockNewsReqId /
    // SetPortNewsReqIdBase.  No simulation, no pre-population.
}

// ============================================================================
// IB data push-ins
// ============================================================================
void NewsWindow::OnMarketNewsItem(const core::NewsItem& item) {
    // main.cpp convention: item.summary carries articleId as transport.
    m_itemProviders[item.id]  = item.source;   // providerCode
    m_itemArticleIds[item.id] = item.summary;  // articleId

    core::NewsItem clean = item;
    clean.summary = "";   // body fetched on demand when user expands

    m_marketNews.insert(m_marketNews.begin(), clean);
    if (m_marketNews.size() > 100) m_marketNews.resize(100);
}

void NewsWindow::OnHistoricalNewsItem(int reqId, const core::NewsItem& item) {
    // main.cpp convention: item.summary carries articleId as transport.
    m_itemProviders[item.id]  = item.source;   // providerCode
    m_itemArticleIds[item.id] = item.summary;  // articleId

    core::NewsItem clean = item;
    clean.summary = "";

    if (reqId == m_stockHistReqId) {
        m_stockNews.push_back(clean);
    } else if (m_mktHistReqIdBase >= 0 && reqId >= m_mktHistReqIdBase && reqId < m_mktHistReqIdBase + 10) {
        // Historical seed for Market tab — insert newest-first, de-dup by headline+time
        bool dup = false;
        for (const auto& existing : m_marketNews)
            if (existing.headline == clean.headline && existing.timestamp == clean.timestamp)
                { dup = true; break; }
        if (!dup) {
            m_marketNews.push_back(clean);
            if (m_marketNews.size() > 200) m_marketNews.resize(200);
        }
    } else if (m_portHistReqIdBase >= 0 && reqId >= m_portHistReqIdBase) {
        // De-duplicate: IB may send the same headline from multiple symbol requests
        bool dup = false;
        for (const auto& existing : m_portfolioNews)
            if (existing.headline == clean.headline && existing.timestamp == clean.timestamp)
                { dup = true; break; }
        if (!dup) m_portfolioNews.push_back(clean);
    }
}

void NewsWindow::OnHistoricalNewsEnd(int reqId) {
    if (reqId == m_stockHistReqId) {
        m_loadingStock = false;
    } else if (m_mktHistReqIdBase >= 0 && reqId >= m_mktHistReqIdBase && reqId < m_mktHistReqIdBase + 10) {
        // Sort market news newest-first once a batch finishes
        std::sort(m_marketNews.begin(), m_marketNews.end(),
                  [](const core::NewsItem& a, const core::NewsItem& b) {
                      return a.timestamp > b.timestamp;
                  });
    } else if (m_portHistReqIdBase >= 0 && reqId >= m_portHistReqIdBase) {
        m_loadingPortfolio = false;
    }
}

void NewsWindow::OnArticleReceived(int itemId, const std::string& text) {
    auto setBody = [&](std::vector<core::NewsItem>& list) {
        for (auto& it : list)
            if (it.id == itemId) { it.summary = text; return true; }
        return false;
    };
    if (!setBody(m_stockNews)) if (!setBody(m_portfolioNews)) setBody(m_marketNews);
}

void NewsWindow::SetPortfolioSymbols(std::vector<std::string> symbols) {
    m_portfolioSymbols = std::move(symbols);
    if (OnPortfolioNewsRequested && !m_portfolioSymbols.empty()) {
        m_loadingPortfolio = true;
        m_portfolioNews.clear();
        OnPortfolioNewsRequested(m_portfolioSymbols);
    }
}

// ============================================================================
// Render
// ============================================================================
bool NewsWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(600, 520), ImGuiCond_FirstUseEver);
    char grp[8];
    if (m_groupId > 0) std::snprintf(grp, sizeof(grp), "G%d", m_groupId);
    else                std::strncpy(grp, "G-", sizeof(grp));
    char title[80];
    std::snprintf(title, sizeof(title), "News %s %s###news%d",
        m_stockSymbol[0] == '\0' ? "--" : m_stockSymbol, grp, m_instanceId);
    if (!ImGui::Begin(title, &m_open, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return m_open;
    }

    DrawToolbar();
    ImGui::Separator();

    if (ImGui::BeginTabBar("##newstabs")) {
        if (ImGui::BeginTabItem("Market")) {
            m_activeTab = 0;
            DrawTabMarket();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Portfolio")) {
            m_activeTab = 1;
            DrawTabPortfolio();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stock")) {
            m_activeTab = 2;
            DrawTabStock();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    return m_open;
}

// ============================================================================
// Toolbar
// ============================================================================
void NewsWindow::DrawToolbar() {
    FlexRow row;

    row.item(FlexRow::buttonW("G1"), 0);
    core::DrawGroupPicker(m_groupId, "##news_grp");

    row.item(em(180), 10);
    ImGui::SetNextItemWidth(em(180));
    ImGui::InputTextWithHint("##filter", "Search headlines...",
                             m_filterText, sizeof(m_filterText));

    row.item(FlexRow::buttonW("Refresh"), 12);
    if (ImGui::Button("Refresh")) RefreshAll();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-request news for portfolio and current stock symbol");
}

// ============================================================================
// Tab: Market news  (real-time push from tickNews — no manual refresh)
// ============================================================================
void NewsWindow::DrawTabMarket() {
    if (m_marketNews.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("  No real-time news yet.");
        ImGui::TextDisabled("  Headlines appear automatically when IB delivers them.");
        ImGui::TextDisabled("  (Requires an active IB market data + news subscription.)");
        return;
    }
    DrawBreakingBanner(m_marketNews);
    DrawNewsList(m_marketNews);
}

// ============================================================================
// Tab: Portfolio news
// ============================================================================
void NewsWindow::DrawTabPortfolio() {
    if (!m_portfolioSymbols.empty()) {
        ImGui::TextDisabled("Tracking: ");
        for (size_t i = 0; i < m_portfolioSymbols.size(); i++) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 1.0f, 1.0f));
            ImGui::Text("%s%s", m_portfolioSymbols[i].c_str(),
                        i + 1 < m_portfolioSymbols.size() ? "," : "");
            ImGui::PopStyleColor();
        }
    }
    ImGui::Spacing();

    if (m_loadingPortfolio) {
        ImGui::TextDisabled("  Loading portfolio news...");
        return;
    }
    if (m_portfolioSymbols.empty()) {
        ImGui::TextDisabled("  No portfolio positions found yet.");
        ImGui::TextDisabled("  News will load once positions are received from IB.");
        return;
    }
    if (m_portfolioNews.empty()) {
        ImGui::TextDisabled("  No news found for portfolio symbols.");
        ImGui::TextDisabled("  Requires an active IB news subscription.");
        return;
    }
    DrawBreakingBanner(m_portfolioNews);
    DrawNewsList(m_portfolioNews);
}

// ============================================================================
// Tab: Stock-specific news
// ============================================================================
void NewsWindow::DrawTabStock() {
    ImGui::Text("Symbol:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(80));
    bool entered = ImGui::InputText("##stocksym", m_stockSymbol, sizeof(m_stockSymbol),
                                    ImGuiInputTextFlags_EnterReturnsTrue |
                                    ImGuiInputTextFlags_CharsUppercase);
    for (char* p = m_stockSymbol; *p; ++p) *p = (char)toupper((unsigned char)*p);

    ImGui::SameLine();
    bool clicked = ImGui::Button("Load");

    if (entered || clicked)
        RefreshStock(m_stockSymbol);

    const char* syms[] = {"AAPL","MSFT","GOOGL","TSLA","AMZN","NVDA","SPY"};
    for (const char* s : syms) {
        ImGui::SameLine();
        bool active = (std::strcmp(m_stockSymbol, s) == 0);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.90f, 1.0f));
        if (ImGui::SmallButton(s)) {
            std::strncpy(m_stockSymbol, s, sizeof(m_stockSymbol) - 1);
            RefreshStock(m_stockSymbol);
        }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    if (m_loadingStock) {
        ImGui::TextDisabled("  Loading news for %s...", m_stockSymbol);
        return;
    }
    if (m_stockNews.empty()) {
        ImGui::TextDisabled("  No news found for %s.", m_stockSymbol);
        ImGui::TextDisabled("  Requires an active IB news subscription.");
        return;
    }
    DrawBreakingBanner(m_stockNews);
    DrawNewsList(m_stockNews);
}

// ============================================================================
// Breaking news banner
// ============================================================================
void NewsWindow::DrawBreakingBanner(const std::vector<core::NewsItem>& items) {
    for (const auto& item : items) {
        if (!item.isBreaking) continue;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.5f, 0.08f, 0.08f, 0.55f));
        ImGui::BeginChild(("##break_" + std::to_string(item.id)).c_str(),
                          ImVec2(-1, 34), false);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::Text(" BREAKING");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(item.headline.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
}

// ============================================================================
// Scrollable news list
// ============================================================================
void NewsWindow::DrawNewsList(std::vector<core::NewsItem>& items) {
    if (items.empty()) return;

    ImGui::BeginChild("##newslist", ImVec2(-1, -1), false);
    int shown = 0;
    for (int i = 0; i < (int)items.size(); i++) {
        if (!MatchesFilter(items[i], m_filterText)) continue;
        DrawNewsItem(items[i], i);
        shown++;
    }
    if (shown == 0)
        ImGui::TextDisabled("  No results match your filter.");
    ImGui::EndChild();
}

// ============================================================================
// Single news item row
// ============================================================================
void NewsWindow::DrawNewsItem(core::NewsItem& item, int /*index*/) {
    ImGui::PushID(item.id);

    bool expanded = (m_expandedId == item.id);

    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float  rowW     = ImGui::GetContentRegionAvail().x;

    float baseH    = ImGui::GetTextLineHeightWithSpacing();
    float innerPad = 4.0f;
    float rowH     = baseH + innerPad * 2;
    if (expanded) {
        int lines = 1;
        for (char c : item.summary) if (c == '\n') lines++;
        float wrapW  = rowW - 24.0f;
        float chW    = ImGui::CalcTextSize("A").x;
        int   chLine = (int)(wrapW / chW);
        int   sumLen = (int)item.summary.size();
        lines += sumLen / std::max(chLine, 1);
        rowH += (float)lines * baseH + 8.0f;
        if (!item.symbols.empty()) rowH += baseH;
    }

    bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));

    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec2      rowEnd = ImVec2(rowStart.x + rowW, rowStart.y + rowH);
    bool        hovered = ImGui::IsItemHovered();

    ImU32 bgCol = 0;
    if (expanded)     bgCol = IM_COL32(30, 50, 80, 120);
    else if (hovered) bgCol = IM_COL32(60, 60, 70,  80);
    if (bgCol) dl->AddRectFilled(rowStart, rowEnd, bgCol, 3.0f);

    if (clicked) {
        m_expandedId = expanded ? -1 : item.id;
        // Fetch article body on demand when expanding an item with no body
        if (!expanded && item.summary.empty() && OnArticleRequested) {
            auto pit = m_itemProviders.find(item.id);
            auto ait = m_itemArticleIds.find(item.id);
            if (pit != m_itemProviders.end() && ait != m_itemArticleIds.end()
                && !ait->second.empty())
                OnArticleRequested(item.id, pit->second, ait->second);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(rowStart.x + 8, rowStart.y + innerPad));

    if (item.isBreaking) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("*");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
    ImGui::Text(expanded ? "v" : ">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6);

    ImVec4 headlineCol = item.isBreaking ? ImVec4(1.0f, 0.85f, 0.85f, 1.0f)
                                         : ImVec4(0.92f, 0.92f, 0.95f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, headlineCol);
    ImGui::TextUnformatted(item.headline.c_str());
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(rowStart.x + 30, rowStart.y + innerPad + baseH));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.52f, 0.56f, 1.0f));
    ImGui::Text("%s", item.source.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.44f, 0.48f, 1.0f));
    ImGui::Text("%s", FormatTimeAgo(item.timestamp).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 10);
    DrawSentimentBadge(item.sentiment);

    for (const auto& sym : item.symbols) {
        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.75f, 1.0f, 1.0f));
        ImGui::Text("[%s]", sym.c_str());
        ImGui::PopStyleColor();
    }

    if (expanded) {
        ImGui::SetCursorScreenPos(ImVec2(rowStart.x + 24, rowStart.y + innerPad + baseH * 2));
        if (item.summary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.52f, 0.56f, 1.0f));
            ImGui::TextUnformatted("Fetching article...");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.80f, 0.84f, 1.0f));
            ImGui::PushTextWrapPos(rowStart.x + rowW - 16);
            ImGui::TextUnformatted(item.summary.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowStart.y + rowH));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.25f, 0.28f, 1.0f));
    ImGui::Separator();
    ImGui::PopStyleColor();

    ImGui::PopID();
}

// ============================================================================
// Sentiment badge
// ============================================================================
void NewsWindow::DrawSentimentBadge(core::NewsSentiment s) {
    ImVec4 col    = SentimentColor(s);
    ImVec4 bgCol  = ImVec4(col.x * 0.3f, col.y * 0.3f, col.z * 0.3f, 0.5f);
    const char* label = SentimentLabel(s);

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 textSz = ImGui::CalcTextSize(label);
    float  padX   = 5.0f, padY = 2.0f;
    ImVec2 boxMin = cursor;
    ImVec2 boxMax = ImVec2(cursor.x + textSz.x + padX * 2,
                           cursor.y + textSz.y + padY * 2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(bgCol), 3.0f);
    dl->AddRect(boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(col), 3.0f, 0, 1.0f);
    dl->AddText(ImVec2(cursor.x + padX, cursor.y + padY),
                ImGui::ColorConvertFloat4ToU32(col), label);

    ImGui::SetCursorScreenPos(ImVec2(boxMax.x, cursor.y));
    ImGui::Dummy(ImVec2(0, textSz.y + padY * 2));
}

// ============================================================================
// Refresh helpers
// ============================================================================
void NewsWindow::RefreshAll() {
    RefreshPortfolio();
    RefreshStock(m_stockSymbol);
    // Market tab is pure tickNews push — nothing to re-request.
}

void NewsWindow::RefreshPortfolio() {
    if (OnPortfolioNewsRequested && !m_portfolioSymbols.empty()) {
        m_loadingPortfolio = true;
        m_portfolioNews.clear();
        OnPortfolioNewsRequested(m_portfolioSymbols);
    }
}

void NewsWindow::SetSymbol(const std::string& sym) {
    if (std::strncmp(m_stockSymbol, sym.c_str(), sizeof(m_stockSymbol)) == 0) return;
    std::strncpy(m_stockSymbol, sym.c_str(), sizeof(m_stockSymbol) - 1);
    m_stockSymbol[sizeof(m_stockSymbol) - 1] = '\0';
    m_activeTab = 2;   // switch to Stock tab
    RefreshStock(sym);
}

void NewsWindow::RefreshStock(const std::string& symbol) {
    m_expandedId = -1;
    if (OnStockNewsRequested) {
        m_loadingStock = true;
        m_stockNews.clear();
        OnStockNewsRequested(symbol);
    }
}

// ============================================================================
// Utilities
// ============================================================================
std::string NewsWindow::FormatTimeAgo(std::time_t ts) {
    std::time_t now  = std::time(nullptr);
    long        diff = (long)(now - ts);
    if (diff < 0)          return "just now";
    if (diff < 60)         return std::to_string(diff) + "s ago";
    if (diff < 3600)       return std::to_string(diff / 60) + "m ago";
    if (diff < 86400)      return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

bool NewsWindow::MatchesFilter(const core::NewsItem& item, const char* filter) {
    if (!filter || filter[0] == '\0') return true;
    std::string hay  = item.headline + " " + item.source;
    std::string need = filter;
    auto toLow = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    return toLow(hay).find(toLow(need)) != std::string::npos;
}

}  // namespace ui
