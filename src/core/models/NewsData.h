#pragma once

#include <string>
#include <vector>
#include <ctime>

namespace core {

enum class NewsSentiment {
    Positive,
    Negative,
    Neutral,
};

enum class NewsCategory {
    Market,
    Portfolio,
    Stock,
};

struct NewsItem {
    int                      id          = 0;
    NewsCategory             category    = NewsCategory::Market;
    NewsSentiment            sentiment   = NewsSentiment::Neutral;
    std::string              headline;
    std::string              summary;        // full body (multi-sentence)
    std::string              source;         // e.g. "Reuters", "Bloomberg"
    std::vector<std::string> symbols;        // related tickers
    std::time_t              timestamp   = 0;
    bool                     isBreaking  = false;
    bool                     isExpanded  = false;   // UI state
};

}  // namespace core
