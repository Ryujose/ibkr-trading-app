#pragma once

#include "core/models/NewsData.h"
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>

namespace ui {

// ============================================================================
// NewsWindow — Three-tab IB news feed: Market / Portfolio / Stock.
//
// Market tab  : populated from IB tickNews (real-time push, no manual refresh).
// Portfolio tab: populated from reqHistoricalNews for each held symbol.
// Stock tab   : populated from reqHistoricalNews for a user-chosen symbol.
//
// Article bodies are fetched on demand (reqNewsArticle) when the user expands
// an item.  All data comes from IB — there is no simulation fallback.
// ============================================================================
class NewsWindow {
public:
    NewsWindow();

    // Call once per frame inside an ImGui context.
    // Returns false if the window was closed.
    bool Render();
    bool& open() { return m_open; }
    void setGroupId(int id) { m_groupId = id; }
    int  groupId() const    { return m_groupId; }
    // Switch to Stock tab and load news for the given symbol (called by group sync).
    void SetSymbol(const std::string& sym);

    // ── IB data push-ins (called by main.cpp callbacks) ──────────────────
    // Real-time headline from tickNews; item.summary carries articleId as
    // transport (main.cpp convention); window extracts and clears it.
    void OnMarketNewsItem(const core::NewsItem& item);

    // Historical headline from reqHistoricalNews; item.summary carries
    // articleId as transport; reqId decides which tab receives the item.
    void OnHistoricalNewsItem(int reqId, const core::NewsItem& item);
    void OnHistoricalNewsEnd(int reqId);

    // Full article body arrived for a previously expanded item.
    void OnArticleReceived(int itemId, const std::string& text);

    // Called when portfolio positions are known (drives Portfolio tab fetch).
    void SetPortfolioSymbols(std::vector<std::string> symbols);

    // ── Callbacks wired by main.cpp after connection ──────────────────────
    std::function<void(const std::string& symbol)>              OnStockNewsRequested;
    std::function<void(const std::vector<std::string>& symbols)> OnPortfolioNewsRequested;
    std::function<void(int itemId, const std::string& providerCode,
                       const std::string& articleId)>            OnArticleRequested;

    // reqId ranges set by main.cpp so the window can route responses correctly.
    void SetStockNewsReqId(int reqId)   { m_stockHistReqId    = reqId; }
    void SetPortNewsReqIdBase(int base) { m_portHistReqIdBase = base;  }
    void SetMktNewsReqIdBase(int base)  { m_mktHistReqIdBase  = base;  }

private:
    // ---- UI state -----------------------------------------------------------
    bool m_open            = true;
    int  m_groupId         = 0;
    int  m_activeTab       = 0;       // 0=Market, 1=Portfolio, 2=Stock
    char m_stockSymbol[16] = "AAPL";
    int  m_expandedId      = -1;      // which item is expanded (-1=none)
    char m_filterText[64]  = "";

    // ---- Data ---------------------------------------------------------------
    std::vector<core::NewsItem> m_marketNews;
    std::vector<core::NewsItem> m_portfolioNews;
    std::vector<core::NewsItem> m_stockNews;
    std::vector<std::string>    m_portfolioSymbols;

    // ---- Loading state ------------------------------------------------------
    bool m_loadingStock     = false;
    bool m_loadingPortfolio = false;

    // ---- reqId routing (set by main.cpp) ------------------------------------
    int  m_stockHistReqId    = -1;
    int  m_portHistReqIdBase = -1;
    int  m_mktHistReqIdBase  = -1;

    // ---- Article fetch tracking: itemId → providerCode / articleId ----------
    std::unordered_map<int, std::string> m_itemProviders;   // itemId → provider
    std::unordered_map<int, std::string> m_itemArticleIds;  // itemId → articleId

    // ---- Private helpers ----------------------------------------------------
    void DrawToolbar();
    void DrawTabMarket();
    void DrawTabPortfolio();
    void DrawTabStock();
    void DrawNewsList(std::vector<core::NewsItem>& items);
    void DrawNewsItem(core::NewsItem& item, int index);
    void DrawSentimentBadge(core::NewsSentiment s);
    void DrawBreakingBanner(const std::vector<core::NewsItem>& items);

    void RefreshAll();
    void RefreshPortfolio();
    void RefreshStock(const std::string& symbol);

    static std::string FormatTimeAgo(std::time_t ts);
    static bool        MatchesFilter(const core::NewsItem& item, const char* filter);
};

}  // namespace ui
