#pragma once

#include "core/models/NewsData.h"
#include <vector>
#include <string>
#include <chrono>

namespace ui {

// ============================================================================
// NewsWindow
// Three-tab news feed: Market / Portfolio / Stock.
// Data is simulated; the public RefreshXxx() methods will be wired to the
// IB Gateway EWrapper::newsArticle callbacks in a future task.
// ============================================================================
class NewsWindow {
public:
    NewsWindow();

    // Call once per frame inside an ImGui context.
    // Returns false if the window was closed.
    bool Render();

    // --- Called by IB Gateway integration (future task) ---
    void OnMarketNewsItem(const core::NewsItem& item);
    void OnPortfolioNewsItem(const core::NewsItem& item);
    void OnStockNewsItem(const core::NewsItem& item);

    // Symbols considered part of the portfolio (set by portfolio manager later)
    void SetPortfolioSymbols(std::vector<std::string> symbols);

private:
    // ---- UI state -----------------------------------------------------------
    bool m_open              = true;
    bool m_hasRealData       = false;
    int  m_activeTab         = 0;          // 0=Market, 1=Portfolio, 2=Stock
    char m_stockSymbol[16]   = "AAPL";
    char m_stockSymbolPrev[16] = "";       // detect symbol change
    bool m_autoRefresh       = true;
    int  m_expandedId        = -1;         // which item is expanded (-1=none)
    char m_filterText[64]    = "";         // keyword search/filter

    // ---- Data ---------------------------------------------------------------
    std::vector<core::NewsItem> m_marketNews;
    std::vector<core::NewsItem> m_portfolioNews;
    std::vector<core::NewsItem> m_stockNews;
    std::vector<std::string>    m_portfolioSymbols;

    // ---- Refresh timing -----------------------------------------------------
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastRefreshMarket;
    Clock::time_point m_lastRefreshPortfolio;
    Clock::time_point m_lastRefreshStock;
    float m_refreshIntervalSec = 30.0f;

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
    void RefreshMarket();
    void RefreshPortfolio();
    void RefreshStock(const std::string& symbol);

    // ---- Simulation ---------------------------------------------------------
    static std::vector<core::NewsItem> SimulateMarketNews();
    static std::vector<core::NewsItem> SimulatePortfolioNews(
        const std::vector<std::string>& symbols);
    static std::vector<core::NewsItem> SimulateStockNews(const std::string& symbol);

    static std::string FormatTimeAgo(std::time_t ts);
    static bool        MatchesFilter(const core::NewsItem& item, const char* filter);
};

}  // namespace ui
