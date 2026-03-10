#include "ui/windows/NewsWindow.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>

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
    m_portfolioSymbols = {"AAPL", "MSFT", "GOOGL", "TSLA"};
    RefreshAll();
}

// ============================================================================
// Public API for IB Gateway callbacks (future)
// ============================================================================
void NewsWindow::OnMarketNewsItem(const core::NewsItem& item) {
    m_marketNews.insert(m_marketNews.begin(), item);
}
void NewsWindow::OnPortfolioNewsItem(const core::NewsItem& item) {
    m_portfolioNews.insert(m_portfolioNews.begin(), item);
}
void NewsWindow::OnStockNewsItem(const core::NewsItem& item) {
    m_stockNews.insert(m_stockNews.begin(), item);
}
void NewsWindow::SetPortfolioSymbols(std::vector<std::string> symbols) {
    m_portfolioSymbols = std::move(symbols);
    RefreshPortfolio();
}

// ============================================================================
// Render
// ============================================================================
bool NewsWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(600, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("News Feed", &m_open)) {
        ImGui::End();
        return m_open;
    }

    // Auto-refresh
    if (m_autoRefresh) {
        auto now = Clock::now();
        auto sec = [](float s){ return std::chrono::duration<float>(s); };
        if (now - m_lastRefreshMarket    > sec(m_refreshIntervalSec)) RefreshMarket();
        if (now - m_lastRefreshPortfolio > sec(m_refreshIntervalSec)) RefreshPortfolio();
        if (now - m_lastRefreshStock     > sec(m_refreshIntervalSec)) RefreshStock(m_stockSymbol);
    }

    DrawToolbar();
    ImGui::Separator();

    // Tab bar
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
    // Keyword filter
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##filter", "Search headlines...", m_filterText, sizeof(m_filterText));

    ImGui::SameLine(0, 12);

    // Refresh button
    if (ImGui::Button("Refresh")) RefreshAll();

    ImGui::SameLine();

    // Auto-refresh toggle
    ImGui::PushStyleColor(ImGuiCol_Button,
        m_autoRefresh ? ImVec4(0.15f, 0.55f, 0.25f, 1.0f)
                      : ImVec4(0.30f, 0.30f, 0.32f, 1.0f));
    if (ImGui::Button(m_autoRefresh ? "Auto ON" : "Auto OFF"))
        m_autoRefresh = !m_autoRefresh;
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 12);

    // Interval slider (only when auto is on)
    if (m_autoRefresh) {
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("##interval", &m_refreshIntervalSec, 10.0f, 120.0f, "%.0fs");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto-refresh interval");
    }
}

// ============================================================================
// Tab: Market news
// ============================================================================
void NewsWindow::DrawTabMarket() {
    DrawBreakingBanner(m_marketNews);
    DrawNewsList(m_marketNews);
}

// ============================================================================
// Tab: Portfolio news
// ============================================================================
void NewsWindow::DrawTabPortfolio() {
    // Show which symbols are tracked
    ImGui::TextDisabled("Tracking: ");
    for (size_t i = 0; i < m_portfolioSymbols.size(); i++) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 1.0f, 1.0f));
        ImGui::Text("%s%s", m_portfolioSymbols[i].c_str(),
                    i + 1 < m_portfolioSymbols.size() ? "," : "");
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    DrawBreakingBanner(m_portfolioNews);
    DrawNewsList(m_portfolioNews);
}

// ============================================================================
// Tab: Stock-specific news
// ============================================================================
void NewsWindow::DrawTabStock() {
    // Symbol selector
    ImGui::Text("Symbol:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    bool entered = ImGui::InputText("##stocksym", m_stockSymbol, sizeof(m_stockSymbol),
                                    ImGuiInputTextFlags_EnterReturnsTrue |
                                    ImGuiInputTextFlags_CharsUppercase);
    // Make uppercase in real time
    for (char* p = m_stockSymbol; *p; ++p) *p = (char)toupper((unsigned char)*p);

    ImGui::SameLine();
    bool clicked = ImGui::Button("Load");

    if (entered || clicked || std::strcmp(m_stockSymbol, m_stockSymbolPrev) != 0) {
        std::memcpy(m_stockSymbolPrev, m_stockSymbol, sizeof(m_stockSymbolPrev));
        RefreshStock(m_stockSymbol);
    }

    // Common symbol quick buttons
    const char* syms[] = {"AAPL","MSFT","GOOGL","TSLA","AMZN","NVDA","SPY"};
    for (const char* s : syms) {
        ImGui::SameLine();
        bool active = (std::strcmp(m_stockSymbol, s) == 0);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.90f, 1.0f));
        if (ImGui::SmallButton(s)) {
            std::strncpy(m_stockSymbol, s, sizeof(m_stockSymbol) - 1);
            std::strncpy(m_stockSymbolPrev, s, sizeof(m_stockSymbolPrev) - 1);
            RefreshStock(m_stockSymbol);
        }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    DrawBreakingBanner(m_stockNews);
    DrawNewsList(m_stockNews);
}

// ============================================================================
// Breaking news banner (shown at top of each tab if there are breaking items)
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
    if (items.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("  No news items available.");
        return;
    }

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

    // Background highlight on hover / expanded
    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float  rowW     = ImGui::GetContentRegionAvail().x;

    // Estimate row height before drawing
    float baseH   = ImGui::GetTextLineHeightWithSpacing();
    float innerPad = 4.0f;
    float rowH    = baseH + innerPad * 2;
    if (expanded) {
        int lines = 1;
        for (char c : item.summary) if (c == '\n') lines++;
        // Also account for word-wrap — rough estimate
        float wrapW  = rowW - 24.0f;
        float chW    = ImGui::CalcTextSize("A").x;
        int   chLine = (int)(wrapW / chW);
        int   sumLen = (int)item.summary.size();
        lines += sumLen / std::max(chLine, 1);
        rowH += (float)lines * baseH + 8.0f;
        // Symbol tags line
        if (!item.symbols.empty()) rowH += baseH;
    }

    // Invisible button over the full row for click detection
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));

    // Draw background
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 rowEnd  = ImVec2(rowStart.x + rowW, rowStart.y + rowH);
    bool   hovered = ImGui::IsItemHovered();

    ImU32 bgCol = 0;
    if (expanded)       bgCol = IM_COL32(30, 50, 80, 120);
    else if (hovered)   bgCol = IM_COL32(60, 60, 70,  80);
    if (bgCol) dl->AddRectFilled(rowStart, rowEnd, bgCol, 3.0f);

    if (clicked)
        m_expandedId = expanded ? -1 : item.id;

    // Draw content on top of the invisible button
    ImGui::SetCursorScreenPos(ImVec2(rowStart.x + 8, rowStart.y + innerPad));

    // Breaking dot
    if (item.isBreaking) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::Text("*");
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
    }

    // Expand arrow
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
    ImGui::Text(expanded ? "v" : ">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6);

    // Headline (bold style via font or just bright color)
    ImVec4 headlineCol = item.isBreaking ? ImVec4(1.0f, 0.85f, 0.85f, 1.0f)
                                         : ImVec4(0.92f, 0.92f, 0.95f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, headlineCol);
    ImGui::TextUnformatted(item.headline.c_str());
    ImGui::PopStyleColor();

    // Meta line: source | time | sentiment badge | symbols
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

    // Symbol tags
    for (const auto& sym : item.symbols) {
        ImGui::SameLine(0, 6);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.75f, 1.0f, 1.0f));
        ImGui::Text("[%s]", sym.c_str());
        ImGui::PopStyleColor();
    }

    // Expanded summary
    if (expanded) {
        ImGui::SetCursorScreenPos(ImVec2(rowStart.x + 24, rowStart.y + innerPad + baseH * 2));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.80f, 0.84f, 1.0f));
        ImGui::PushTextWrapPos(rowStart.x + rowW - 16);
        ImGui::TextUnformatted(item.summary.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // Separator
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
    ImVec4 col   = SentimentColor(s);
    ImVec4 bgCol = ImVec4(col.x * 0.3f, col.y * 0.3f, col.z * 0.3f, 0.5f);
    const char* label = SentimentLabel(s);

    ImVec2 cursor  = ImGui::GetCursorScreenPos();
    ImVec2 textSz  = ImGui::CalcTextSize(label);
    float  padX    = 5.0f, padY = 2.0f;
    ImVec2 boxMin  = cursor;
    ImVec2 boxMax  = ImVec2(cursor.x + textSz.x + padX * 2,
                            cursor.y + textSz.y + padY * 2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(bgCol), 3.0f);
    dl->AddRect(boxMin, boxMax, ImGui::ColorConvertFloat4ToU32(col), 3.0f, 0, 1.0f);
    dl->AddText(ImVec2(cursor.x + padX, cursor.y + padY),
                ImGui::ColorConvertFloat4ToU32(col), label);

    // Advance cursor past the badge
    ImGui::SetCursorScreenPos(ImVec2(boxMax.x, cursor.y));
    ImGui::Dummy(ImVec2(0, textSz.y + padY * 2));
}

// ============================================================================
// Refresh helpers
// ============================================================================
void NewsWindow::RefreshAll() {
    RefreshMarket();
    RefreshPortfolio();
    RefreshStock(m_stockSymbol);
}

void NewsWindow::RefreshMarket() {
    m_marketNews          = SimulateMarketNews();
    m_lastRefreshMarket   = Clock::now();
}

void NewsWindow::RefreshPortfolio() {
    m_portfolioNews          = SimulatePortfolioNews(m_portfolioSymbols);
    m_lastRefreshPortfolio   = Clock::now();
}

void NewsWindow::RefreshStock(const std::string& symbol) {
    m_stockNews          = SimulateStockNews(symbol);
    m_lastRefreshStock   = Clock::now();
    m_expandedId         = -1;
}

// ============================================================================
// Time formatting
// ============================================================================
std::string NewsWindow::FormatTimeAgo(std::time_t ts) {
    std::time_t now  = std::time(nullptr);
    long        diff = (long)(now - ts);
    if (diff < 60)         return std::to_string(diff) + "s ago";
    if (diff < 3600)       return std::to_string(diff / 60) + "m ago";
    if (diff < 86400)      return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

// ============================================================================
// Keyword filter
// ============================================================================
bool NewsWindow::MatchesFilter(const core::NewsItem& item, const char* filter) {
    if (!filter || filter[0] == '\0') return true;
    std::string hay  = item.headline + " " + item.source;
    std::string need = filter;
    // Case-insensitive search
    auto toLow = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    return toLow(hay).find(toLow(need)) != std::string::npos;
}

// ============================================================================
// Simulated data
// All timestamps are offset from "now" so FormatTimeAgo shows realistic values.
// Replace these bodies with EClient::reqNewsArticle / reqHistoricalNews calls.
// ============================================================================

std::vector<core::NewsItem> NewsWindow::SimulateMarketNews() {
    using core::NewsItem;
    using core::NewsSentiment;
    using core::NewsCategory;

    std::time_t now = std::time(nullptr);
    int id = 1000;

    auto make = [&](const char* headline, const char* summary, const char* source,
                    int secsAgo, NewsSentiment sent, bool breaking,
                    std::vector<std::string> syms = {}) -> NewsItem {
        NewsItem n;
        n.id        = id++;
        n.category  = NewsCategory::Market;
        n.headline  = headline;
        n.summary   = summary;
        n.source    = source;
        n.timestamp = now - secsAgo;
        n.sentiment = sent;
        n.isBreaking = breaking;
        n.symbols   = std::move(syms);
        return n;
    };

    return {
        make("Fed holds rates steady, signals two cuts in 2025",
             "The Federal Reserve held its benchmark interest rate unchanged at 5.25–5.50% on Wednesday, "
             "while updated projections showed officials still expect to cut rates twice by the end of "
             "2025. Chair Jerome Powell stated that the committee remains data-dependent and will "
             "continue to monitor inflation closely before making any adjustments.",
             "Reuters", 180, NewsSentiment::Positive, true),

        make("S&P 500 hits record high amid tech rally",
             "The S&P 500 closed at an all-time high on Thursday, driven by strong earnings from "
             "mega-cap technology companies. The index gained 1.4% while the Nasdaq Composite surged "
             "2.1%. Analysts attribute the rally to better-than-expected corporate results and easing "
             "inflation concerns.",
             "Bloomberg", 900, NewsSentiment::Positive, false, {"SPY","QQQ"}),

        make("Oil prices drop 3% on inventory build",
             "Crude oil fell sharply after the Energy Information Administration reported a larger-than-"
             "expected build in U.S. inventories. WTI crude settled at $74.50 per barrel, its lowest "
             "level in six weeks. Analysts warned that weaker demand from China could further pressure "
             "prices in the coming months.",
             "Reuters", 3600, NewsSentiment::Negative, false, {"XOM","CVX"}),

        make("Treasury yields rise as inflation data surprises to upside",
             "The 10-year Treasury yield climbed to 4.68% after CPI data showed headline inflation "
             "rose 0.4% month-over-month, above the 0.3% consensus estimate. The hotter-than-expected "
             "reading pushed back market expectations for Fed rate cuts and pressured bond prices.",
             "CNBC", 7200, NewsSentiment::Negative, false),

        make("Global PMI data signals resilient economic growth",
             "Manufacturing and services PMI readings across major economies came in above expectations "
             "in the latest survey, suggesting the global economy remains on solid footing despite "
             "elevated interest rates. The composite global PMI rose to 52.8, up from 51.9 in the "
             "prior month.",
             "Financial Times", 10800, NewsSentiment::Positive, false),

        make("Dollar strengthens as safe-haven demand rises",
             "The U.S. dollar index rose 0.6% to 105.4 as investors sought safety amid renewed "
             "geopolitical tensions. The euro fell to 1.073 against the dollar while the yen weakened "
             "to 153 per dollar, prompting fresh verbal intervention warnings from Japanese officials.",
             "Bloomberg", 14400, NewsSentiment::Neutral, false),

        make("Consumer confidence falls to three-month low",
             "The Conference Board's Consumer Confidence Index dropped to 98.3 in March, below the "
             "expected 102.5 and the lowest reading since December. Respondents cited concerns about "
             "higher prices for groceries and housing costs. The expectations sub-index, which measures "
             "consumers' short-term outlook, fell to a six-month low.",
             "MarketWatch", 21600, NewsSentiment::Negative, false),

        make("Earnings season kicks off with banks beating estimates",
             "Major U.S. banks reported first-quarter results that exceeded analyst expectations, "
             "setting a positive tone for the earnings season. Net interest income remained strong "
             "for most lenders, though provisions for credit losses ticked higher as consumer "
             "delinquencies continued to rise modestly.",
             "Wall Street Journal", 28800, NewsSentiment::Positive, false, {"JPM","BAC","GS"}),

        make("Semiconductor shortage easing, analysts say",
             "Global semiconductor supply constraints that plagued multiple industries have largely "
             "normalised, according to a report from industry consultancy IDC. Lead times for chips "
             "have fallen to near-historical norms, which is expected to benefit companies in the "
             "automotive and consumer electronics sectors.",
             "Reuters", 36000, NewsSentiment::Positive, false, {"NVDA","AMD","INTC"}),

        make("Housing starts miss estimates for third consecutive month",
             "U.S. housing starts fell 1.8% in March, missing estimates for the third straight month "
             "as elevated mortgage rates continued to dampen new construction activity. Building "
             "permits, a forward-looking indicator, also declined 2.3%, suggesting the weakness "
             "may persist into the second quarter.",
             "CNBC", 43200, NewsSentiment::Negative, false),
    };
}

std::vector<core::NewsItem> NewsWindow::SimulatePortfolioNews(
    const std::vector<std::string>& symbols)
{
    using core::NewsItem;
    using core::NewsSentiment;
    using core::NewsCategory;

    std::time_t now = std::time(nullptr);
    int id = 2000;

    auto make = [&](const char* headline, const char* summary, const char* source,
                    int secsAgo, NewsSentiment sent, bool breaking,
                    std::vector<std::string> syms) -> NewsItem {
        NewsItem n;
        n.id         = id++;
        n.category   = NewsCategory::Portfolio;
        n.headline   = headline;
        n.summary    = summary;
        n.source     = source;
        n.timestamp  = now - secsAgo;
        n.sentiment  = sent;
        n.isBreaking = breaking;
        n.symbols    = std::move(syms);
        return n;
    };

    // Filter to only include items relevant to the provided symbols
    std::vector<NewsItem> all = {
        make("Apple announces $110B share buyback and dividend raise",
             "Apple Inc. unveiled its largest-ever share repurchase program valued at $110 billion "
             "alongside a 4% increase to its quarterly dividend. The announcement came as part of the "
             "company's fiscal Q2 earnings release, which showed revenue of $90.8 billion, beating "
             "the consensus estimate of $88.6 billion. Services revenue hit a record $23.9 billion.",
             "Bloomberg", 600, NewsSentiment::Positive, true, {"AAPL"}),

        make("Microsoft Azure growth re-accelerates to 31%",
             "Microsoft reported cloud revenue growth that exceeded analyst forecasts, with Azure "
             "growing 31% year-over-year, up from 28% in the prior quarter. CEO Satya Nadella cited "
             "AI workloads as a primary driver. The company also raised its full-year guidance for "
             "cloud and productivity segments.",
             "CNBC", 3600, NewsSentiment::Positive, false, {"MSFT"}),

        make("Alphabet faces fresh antitrust ruling in ad-tech case",
             "A U.S. federal judge ruled that Alphabet's Google illegally monopolised the online "
             "advertising technology market, setting the stage for potential remedies that could "
             "include forcing the company to sell parts of its ad business. Shares fell more than "
             "4% on the news before recovering some losses.",
             "Wall Street Journal", 5400, NewsSentiment::Negative, true, {"GOOGL"}),

        make("Tesla deliveries beat Q1 estimates, raising optimism",
             "Tesla delivered 386,810 vehicles in the first quarter, topping the consensus estimate "
             "of 368,000 and reversing concerns about weakening demand. CEO Elon Musk credited "
             "improved manufacturing efficiency at Gigafactory Texas. The stock rallied 7% in "
             "after-hours trading.",
             "Reuters", 7200, NewsSentiment::Positive, false, {"TSLA"}),

        make("Apple supplier Foxconn expands India capacity",
             "Foxconn Technology Group announced a $1.5 billion investment to expand its iPhone "
             "assembly capacity in India as Apple continues to diversify its supply chain away from "
             "China. The new facility in Karnataka is expected to add 50,000 jobs and begin "
             "production in late 2025.",
             "Financial Times", 14400, NewsSentiment::Positive, false, {"AAPL"}),

        make("Microsoft and OpenAI deepen AI partnership",
             "Microsoft announced an expanded multibillion-dollar investment in OpenAI, including "
             "additional compute resources through Azure. The companies will jointly develop next-"
             "generation AI models for enterprise customers. Analysts estimate the partnership "
             "could add $10–15 billion in annual cloud revenue by 2026.",
             "Bloomberg", 18000, NewsSentiment::Positive, false, {"MSFT"}),

        make("Tesla faces NHTSA probe over autopilot incidents",
             "The National Highway Traffic Safety Administration has opened a formal investigation "
             "into Tesla's Autopilot system following 23 reported incidents involving emergency "
             "vehicle collisions. Tesla stated it is cooperating fully and remains confident in "
             "the safety record of its driver-assistance technology.",
             "Reuters", 28800, NewsSentiment::Negative, false, {"TSLA"}),

        make("Alphabet YouTube ad revenue tops $9B for first time",
             "YouTube advertising revenue exceeded $9 billion for the first time in a single quarter, "
             "driven by strength in brand advertising and YouTube Shorts monetisation. Alphabet's "
             "overall revenue grew 15% year-over-year to $80.5 billion, beating expectations of "
             "$79.0 billion.",
             "CNBC", 32400, NewsSentiment::Positive, false, {"GOOGL"}),
    };

    // Keep only items whose symbols overlap with the portfolio
    std::vector<NewsItem> filtered;
    for (auto& item : all) {
        for (const auto& sym : item.symbols) {
            bool inPortfolio = std::find(symbols.begin(), symbols.end(), sym) != symbols.end();
            if (inPortfolio) { filtered.push_back(item); break; }
        }
    }
    return filtered;
}

std::vector<core::NewsItem> NewsWindow::SimulateStockNews(const std::string& symbol) {
    using core::NewsItem;
    using core::NewsSentiment;
    using core::NewsCategory;

    std::time_t now = std::time(nullptr);
    int id = 3000;

    auto make = [&](std::string headline, std::string summary, const char* source,
                    int secsAgo, NewsSentiment sent, bool breaking = false) -> NewsItem {
        NewsItem n;
        n.id         = id++;
        n.category   = NewsCategory::Stock;
        n.headline   = std::move(headline);
        n.summary    = std::move(summary);
        n.source     = source;
        n.timestamp  = now - secsAgo;
        n.sentiment  = sent;
        n.isBreaking = breaking;
        n.symbols    = {symbol};
        return n;
    };

    // Generate symbol-specific headlines (shared template, personalised by symbol name)
    return {
        make((symbol + " beats Q1 earnings estimates; raises full-year outlook").c_str(),
             symbol + " reported first-quarter earnings per share of $2.18, beating the consensus "
             "estimate of $1.94. Revenue grew 12% year-over-year to $32.4 billion. Management "
             "raised full-year EPS guidance to $8.50–$8.70, above the prior street estimate of $8.30. "
             "The beat was driven by strong performance in the company's core business segments.",
             "Bloomberg", 1200, NewsSentiment::Positive, true),

        make(("Analyst upgrades " + symbol + " to Overweight, sets $230 target").c_str(),
             "Morgan Stanley upgraded " + symbol + " to Overweight from Equal-Weight, citing "
             "improving margin trends and underappreciated AI exposure. The firm set a 12-month "
             "price target of $230, representing approximately 24% upside from current levels. "
             "The upgrade follows a series of positive data points from industry checks.",
             "MarketWatch", 4800, NewsSentiment::Positive),

        make((symbol + " CFO to present at Goldman Sachs Technology Conference").c_str(),
             "The Chief Financial Officer of " + symbol + " is scheduled to present at the Goldman "
             "Sachs Technology and Internet Conference next Tuesday at 2:00 PM ET. The presentation "
             "will be webcast live on the company's investor relations website and is expected to "
             "cover the company's financial outlook and strategic priorities.",
             "PR Newswire", 9600, NewsSentiment::Neutral),

        make(("Insider buying: " + symbol + " CEO purchases $4.2M in shares").c_str(),
             "SEC filings show that the CEO of " + symbol + " purchased 18,500 shares of company "
             "stock for approximately $4.2 million in open-market transactions over the past week. "
             "The purchase follows recent stock weakness and is viewed by some analysts as a "
             "positive signal of management confidence in the company's near-term outlook.",
             "Barron's", 18000, NewsSentiment::Positive),

        make((symbol + " short interest rises to 5-year high amid margin concerns").c_str(),
             "Short interest in " + symbol + " climbed to its highest level in five years, according "
             "to the latest data from S3 Partners. Bears are focused on potential margin compression "
             "from rising input costs and increasing competition. Days-to-cover stands at 4.2, up "
             "from 3.1 a month ago.",
             "Reuters", 25200, NewsSentiment::Negative),

        make(("Options market implies 8% move for " + symbol + " earnings").c_str(),
             "Options pricing implies an 8.2% share price move for " + symbol + " following its "
             "upcoming earnings report, based on straddle pricing at the closest expiry. Implied "
             "volatility has risen to the 78th percentile relative to the past year. Analysts "
             "note that the stock has moved an average of 5.3% on earnings days over the past "
             "eight quarters.",
             "CNBC", 32400, NewsSentiment::Neutral),

        make((symbol + " launches new product line, targets enterprise market").c_str(),
             symbol + " announced the launch of its latest product suite aimed at enterprise "
             "customers, with pricing starting at $99 per user per month. The company expects "
             "the new offering to meaningfully contribute to revenue in the second half of 2025. "
             "Industry analysts estimate the addressable market at $45 billion annually.",
             "TechCrunch", 43200, NewsSentiment::Positive),

        make((symbol + " faces supply chain disruption from Southeast Asia floods").c_str(),
             "Severe flooding in key manufacturing regions of Southeast Asia is expected to disrupt "
             "supply chains for " + symbol + " and other technology companies in the coming weeks. "
             "Analysts estimate a potential 3–5% revenue impact if the disruption extends beyond "
             "30 days. The company has declined to comment but is understood to be assessing "
             "alternative sourcing options.",
             "Financial Times", 57600, NewsSentiment::Negative),
    };
}

}  // namespace ui
