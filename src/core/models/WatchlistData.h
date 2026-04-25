#pragma once

#include <string>
#include <vector>

namespace core {

struct WatchlistItem {
    std::string symbol;
    std::string description;
    std::string secType;
    std::string primaryExch;
    std::string currency;
    int         conId      = 0;
    int         reqId      = 0;    // assigned market-data reqId (0 = not subscribed)
    double      last       = 0.0;
    double      bid        = 0.0;
    double      ask        = 0.0;
    double      open       = 0.0;
    double      high       = 0.0;
    double      low        = 0.0;
    double      change     = 0.0;
    double      changePct  = 0.0;
    double      volume     = 0.0;
    double      bidSize    = 0.0;
    double      askSize    = 0.0;
    double      lastSize   = 0.0;
    double      prevClose  = 0.0;
    double      avgVolume  = 0.0;
    double      high52w    = 0.0;
    double      low52w     = 0.0;
    bool        subscribed = false;
};

struct Watchlist {
    std::string              name  = "Watchlist";
    std::vector<WatchlistItem> items;
};

}  // namespace core
