#pragma once

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/models/MarketData.h"
#include "core/models/OrderData.h"
#include "core/models/ReplayData.h"

namespace core::services {

// ============================================================================
// Pure-logic replay engine consumed by ui::ReplayWindow.
// No IB API / ImGui / ImPlot dependency — testable from tests-core.
// See .claude/plans/replay.md.
// ============================================================================

constexpr double kDefaultCommissionPerShare = 0.005;

// ---- ReplaySession ----------------------------------------------------------

enum class ReplaySession : int {
    PreMarket = 0,   // 04:00–09:30 ET
    Intraday  = 1,   // 09:30–16:00 ET
    PostMarket= 2,   // 16:00–20:00 ET
    All       = 3    // 04:00–20:00 ET
};

inline const char* ReplaySessionLabel(ReplaySession s) {
    switch (s) {
        case ReplaySession::PreMarket: return "Pre-Market";
        case ReplaySession::Intraday:  return "Intraday";
        case ReplaySession::PostMarket:return "Post-Market";
        case ReplaySession::All:       return "All";
    }
    return "?";
}

inline const char* ReplaySessionShort(ReplaySession s) {
    switch (s) {
        case ReplaySession::PreMarket: return "PRE";
        case ReplaySession::Intraday:  return "RTH";
        case ReplaySession::PostMarket:return "POST";
        case ReplaySession::All:       return "ALL";
    }
    return "?";
}

// ---- ReplayClock ------------------------------------------------------------

struct ReplayClock {
    int    cursorBarIdx   = 0;
    double cursorSeconds  = 0.0;
    double speed          = 1.0;   // 0 = paused; 0.25 / 1 / 2 / 5 / 20 / 60; INFINITY = MAX
    bool   paused         = true;
    bool   scrubbing      = false;
    int    sessionFirstIdx = 0;
    int    sessionLastIdx  = 0;
};

// Advance cursor. deltaSec = wall-clock seconds since last frame,
// barSec = market-time seconds per bar (e.g. 60 for M1).
// At speed 1× one market minute takes one real minute.
// At speed 60× one market minute takes one real second.
// No-op when paused or scrubbing. Clamps to [first, last].
inline void Tick(ReplayClock& c, double deltaSec, double barSec) {
    if (c.paused || c.scrubbing || c.speed <= 0.0) return;
    if (barSec <= 0.0) return;
    c.cursorSeconds += deltaSec * c.speed;   // accumulated wall seconds
    while (c.cursorSeconds >= barSec) {
        c.cursorSeconds -= barSec;
        ++c.cursorBarIdx;
    }
    if (c.cursorBarIdx > c.sessionLastIdx)
        c.cursorBarIdx = c.sessionLastIdx;
}

inline void StepBars(ReplayClock& c, int delta) {
    c.cursorBarIdx += delta;
    if (c.cursorBarIdx < c.sessionFirstIdx) c.cursorBarIdx = c.sessionFirstIdx;
    if (c.cursorBarIdx > c.sessionLastIdx)  c.cursorBarIdx = c.sessionLastIdx;
    c.cursorSeconds = 0.0;
}

inline void SeekToBar(ReplayClock& c, int idx) {
    if (idx < c.sessionFirstIdx) idx = c.sessionFirstIdx;
    if (idx > c.sessionLastIdx)  idx = c.sessionLastIdx;
    c.cursorBarIdx = idx;
    c.cursorSeconds = 0.0;
}

// Binary search on bars (sorted by .timestamp ascending). Returns the index
// whose bar.timestamp is closest to targetTime. Tie-break to the earlier bar.
// Out-of-range clamps to first/last. Empty vector returns 0.
inline int SnapCursorToNearestBar(std::time_t targetTime,
                                  const std::vector<core::Bar>& bars) {
    int n = static_cast<int>(bars.size());
    if (n == 0) return 0;
    double target = static_cast<double>(targetTime);
    if (target <= bars[0].timestamp) return 0;
    if (target >= bars[n - 1].timestamp) return n - 1;

    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (bars[mid].timestamp < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    // lo is the first bar with timestamp >= target
    int cand = lo;
    if (cand > 0) {
        double distCur  = bars[cand].timestamp - target;
        double distPrev = target - bars[cand - 1].timestamp;
        if (distPrev <= distCur) cand = cand - 1;
    }
    return cand;
}

inline void SeekToTime(ReplayClock& c, std::time_t t,
                       const std::vector<core::Bar>& bars) {
    c.cursorBarIdx = SnapCursorToNearestBar(t, bars);
    c.cursorSeconds = 0.0;
}

// ---- BarRangeForSession -----------------------------------------------------

struct BarRange { int firstIdx = 0; int lastIdx = 0; };

inline BarRange BarRangeForSession(const std::vector<core::Bar>& bars,
                                    ReplaySession session) {
    int n = static_cast<int>(bars.size());
    if (n == 0) return {};

    int first = -1, last = -1;
    for (int i = 0; i < n; ++i) {
        core::Session s = core::BarSession(static_cast<std::time_t>(bars[i].timestamp));
        bool include = false;
        switch (session) {
            case ReplaySession::PreMarket: include = (s == core::Session::PreMarket); break;
            case ReplaySession::Intraday:  include = (s == core::Session::Regular);    break;
            case ReplaySession::PostMarket:include = (s == core::Session::AfterHours);  break;
            case ReplaySession::All:
                include = (s == core::Session::PreMarket ||
                           s == core::Session::Regular ||
                           s == core::Session::AfterHours);
                break;
        }
        if (include) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return {0, 0};  // no bars in this session
    return {first, last};
}

// ---- SimulatedAccount -------------------------------------------------------

struct SimulatedPosition {
    long        conId   = 0;
    std::string symbol;
    double      qty     = 0.0;   // positive = long, negative = short
    double      avgCost = 0.0;
};

struct SimulatedFill {
    std::time_t     time        = 0;
    std::string     symbol;
    core::OrderSide side        = core::OrderSide::Buy;
    double          qty         = 0.0;
    double          price       = 0.0;
    double          commission  = 0.0;
    int             intentOrderId = 0;
    std::string     intentNote;    // "limit fill", "stop trigger", "market open", etc.
};

struct SimulatedAccount {
    double startingCash    = 100000.0;
    double cash            = 100000.0;
    std::unordered_map<std::string, SimulatedPosition> positions;
    std::vector<SimulatedFill> fills;
    double realizedPnL     = 0.0;
    double commissionPaid  = 0.0;
};

inline void ApplyFill(SimulatedAccount& acct, const SimulatedFill& f) {
    acct.fills.push_back(f);
    acct.commissionPaid += f.commission;
    acct.cash -= f.commission;

    auto it = acct.positions.find(f.symbol);
    bool exists = (it != acct.positions.end());
    double oldQty = exists ? it->second.qty : 0.0;
    double oldAvg = exists ? it->second.avgCost : 0.0;

    if (f.side == core::OrderSide::Buy) {
        double cost = f.qty * f.price;
        acct.cash -= cost;

        if (oldQty < 0.0) {
            // Closing short — realize PnL on the closed portion
            double closeQty = std::min(f.qty, -oldQty);
            double pnl = closeQty * (oldAvg - f.price);
            acct.realizedPnL += pnl;
            double remainShort = -oldQty - closeQty;
            if (remainShort <= 0.0) {
                // Fully closed; may flip long
                double flipQty = f.qty - closeQty;
                if (flipQty > 0.0) {
                    acct.positions[f.symbol] = {0, f.symbol, flipQty, f.price};
                } else {
                    acct.positions.erase(it);
                }
            } else {
                it->second.qty = -remainShort;
            }
        } else {
            // Adding to or opening long
            double newQty = oldQty + f.qty;
            double newAvg = (oldQty * oldAvg + f.qty * f.price) / newQty;
            acct.positions[f.symbol] = {0, f.symbol, newQty, newAvg};
        }
    } else {  // Sell
        double proceeds = f.qty * f.price;
        acct.cash += proceeds;

        if (oldQty > 0.0) {
            // Closing long
            double closeQty = std::min(f.qty, oldQty);
            double pnl = closeQty * (f.price - oldAvg);
            acct.realizedPnL += pnl;
            double remainLong = oldQty - closeQty;
            if (remainLong <= 0.0) {
                double flipQty = f.qty - closeQty;
                if (flipQty > 0.0) {
                    acct.positions[f.symbol] = {0, f.symbol, -flipQty, f.price};
                } else {
                    acct.positions.erase(it);
                }
            } else {
                it->second.qty = remainLong;
            }
        } else {
            // Adding to or opening short
            double newQty = oldQty - f.qty;
            double newAvg = oldQty < 0.0
                ? ((-oldQty) * oldAvg + f.qty * f.price) / (-newQty)
                : f.price;
            acct.positions[f.symbol] = {0, f.symbol, newQty, newAvg};
        }
    }
}

inline double Equity(const SimulatedAccount& acct, double lastPrice) {
    double posVal = 0.0;
    for (const auto& [sym, pos] : acct.positions) {
        (void)sym;
        posVal += pos.qty * lastPrice;
    }
    return acct.cash + posVal;
}

inline double UnrealizedPnL(const SimulatedAccount& acct, double lastPrice) {
    double upl = 0.0;
    for (const auto& [sym, pos] : acct.positions) {
        (void)sym;
        upl += pos.qty * (lastPrice - pos.avgCost);
    }
    return upl;
}

inline void Reset(SimulatedAccount& acct, double startingCash) {
    acct.startingCash   = startingCash;
    acct.cash           = startingCash;
    acct.positions.clear();
    acct.fills.clear();
    acct.realizedPnL    = 0.0;
    acct.commissionPaid = 0.0;
}

// ---- SimulatedOrderBook -----------------------------------------------------

struct WorkingOrder {
    int          localId      = 0;
    core::Order  order;
    std::time_t  placedAt     = 0;
    double       trailRef     = 0.0;    // high-water (SELL) / low-water (BUY)
    double       trailStop    = 0.0;    // current trailing stop price
    bool         stopTriggered = false; // StopLimit / TrailLimit / LIT leg fired
};

using SimulatedOrderBook = std::vector<WorkingOrder>;

struct EvaluateResult {
    std::vector<SimulatedFill> fills;
    std::vector<int>           filledIds;   // localIds to remove from book
};

// ---- Forward helpers --------------------------------------------------------

namespace {
    inline bool IsIntradayBar(const core::Bar& bar) {
        return core::BarSession(static_cast<std::time_t>(bar.timestamp)) == core::Session::Regular;
    }

    // Trail amount in dollars from an order. Uses auxPrice (absolute) or
    // trailingPercent (relative to refPrice), in that precedence.
    inline double TrailAmount(const core::Order& o, double refPrice) {
        if (o.auxPrice > 0.0) return o.auxPrice;
        if (o.trailingPercent > 0.0) return o.trailingPercent / 100.0 * refPrice;
        return 0.0;
    }

    // Initialise trail state when a Trail/TrailLimit order is first evaluated.
    inline void InitTrail(WorkingOrder& wo, const core::Bar& bar) {
        wo.trailRef = bar.open;
        double amt = TrailAmount(wo.order, bar.open);
        if (wo.order.side == core::OrderSide::Buy) {
            wo.trailStop = bar.open + amt;   // stop rises as price falls
        } else {
            wo.trailStop = bar.open - amt;   // stop rises as price rises
        }
    }

    // Update trail state from a new bar.
    inline void UpdateTrail(WorkingOrder& wo, const core::Bar& bar) {
        double amt = TrailAmount(wo.order, wo.trailRef);
        if (wo.order.side == core::OrderSide::Buy) {
            // Trail-stop BUY: falls as new lows come in
            if (bar.low < wo.trailRef) wo.trailRef = bar.low;
            wo.trailStop = wo.trailRef + amt;
        } else {
            // Trail-stop SELL: rises as new highs come in
            if (bar.high > wo.trailRef) wo.trailRef = bar.high;
            wo.trailStop = wo.trailRef - amt;
        }
    }

    inline bool TrailTriggered(const WorkingOrder& wo, const core::Bar& bar) {
        if (wo.trailStop <= 0.0) return false;
        if (wo.order.side == core::OrderSide::Buy)
            return bar.high >= wo.trailStop;
        else
            return bar.low <= wo.trailStop;
    }

    inline SimulatedFill MakeFill(const WorkingOrder& wo, const core::Bar& bar,
                                  double price, double commPerShare,
                                  const char* note) {
        SimulatedFill f;
        f.time          = static_cast<std::time_t>(bar.timestamp);
        f.symbol        = wo.order.symbol;
        f.side          = wo.order.side;
        f.qty           = wo.order.quantity;
        f.price         = price;
        f.commission    = wo.order.quantity * commPerShare;
        f.intentOrderId = wo.localId;
        f.intentNote    = note;
        return f;
    }

    inline SimulatedFill MakeFill(const WorkingOrder& wo,
                                  const core::HistoricalTick& tick,
                                  double price, double commPerShare,
                                  const char* note) {
        SimulatedFill f;
        f.time          = tick.time;
        f.symbol        = wo.order.symbol;
        f.side          = wo.order.side;
        f.qty           = wo.order.quantity;
        f.price         = price;
        f.commission    = wo.order.quantity * commPerShare;
        f.intentOrderId = wo.localId;
        f.intentNote    = note;
        return f;
    }
}  // anonymous namespace

// ---- EvaluateBar (per-bar resolution) ---------------------------------------

// Processes one OHLCV bar against the working order book. Returns fills and
// the localIds that should be removed. The book is mutated in place for
// stateful order types (StopLimit stop-triggered flag, Trail reference prices).
//
// isSessionClose: true when this bar is the last bar of the Intraday session.
// Used for MOC/LOC fills and TIF DAY auto-cancel.
inline EvaluateResult EvaluateBar(SimulatedOrderBook& book,
                                  const core::Bar& bar,
                                  double commissionPerShare = kDefaultCommissionPerShare,
                                  bool isSessionClose = false) {
    EvaluateResult result;
    bool isRth = IsIntradayBar(bar);

    for (auto& wo : book) {
        // Order hasn't been placed yet in replay time
        if (static_cast<double>(wo.placedAt) > bar.timestamp) continue;

        // Market orders cannot fill on the placement bar
        bool isMarketFamily = (wo.order.type == core::OrderType::Market ||
                               wo.order.type == core::OrderType::MTL ||
                               wo.order.type == core::OrderType::Midprice ||
                               wo.order.type == core::OrderType::Relative);
        if (isMarketFamily && static_cast<double>(wo.placedAt) == bar.timestamp) continue;

        // outsideRth filter
        if (!wo.order.outsideRth && !isRth) continue;

        SimulatedFill fill{};
        bool filled = false;

        switch (wo.order.type) {

        case core::OrderType::Market:
            filled = true;
            fill = MakeFill(wo, bar, bar.open, commissionPerShare, "market fill");
            break;

        case core::OrderType::Limit:
            if (wo.order.side == core::OrderSide::Buy) {
                if (bar.low <= wo.order.limitPrice) {
                    filled = true;
                    fill = MakeFill(wo, bar,
                        std::min(bar.open, wo.order.limitPrice),
                        commissionPerShare, "limit fill");
                }
            } else {
                if (bar.high >= wo.order.limitPrice) {
                    filled = true;
                    fill = MakeFill(wo, bar,
                        std::max(bar.open, wo.order.limitPrice),
                        commissionPerShare, "limit fill");
                }
            }
            break;

        case core::OrderType::Stop:
            if (wo.order.side == core::OrderSide::Buy) {
                if (bar.high >= wo.order.stopPrice) {
                    filled = true;
                    fill = MakeFill(wo, bar,
                        std::max(bar.open, wo.order.stopPrice),
                        commissionPerShare, "stop trigger");
                }
            } else {
                if (bar.low <= wo.order.stopPrice) {
                    filled = true;
                    fill = MakeFill(wo, bar,
                        std::min(bar.open, wo.order.stopPrice),
                        commissionPerShare, "stop trigger");
                }
            }
            break;

        case core::OrderType::StopLimit:
            if (!wo.stopTriggered) {
                // Check the stop leg
                if (wo.order.side == core::OrderSide::Buy) {
                    if (bar.high >= wo.order.stopPrice) wo.stopTriggered = true;
                } else {
                    if (bar.low <= wo.order.stopPrice) wo.stopTriggered = true;
                }
            }
            if (wo.stopTriggered) {
                // Check the limit leg
                if (wo.order.side == core::OrderSide::Buy) {
                    if (bar.low <= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, bar,
                            std::min(bar.open, wo.order.limitPrice),
                            commissionPerShare, "stop-limit fill");
                    }
                } else {
                    if (bar.high >= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, bar,
                            std::max(bar.open, wo.order.limitPrice),
                            commissionPerShare, "stop-limit fill");
                    }
                }
            }
            break;

        case core::OrderType::Trail: {
            if (wo.trailStop <= 0.0) InitTrail(wo, bar);
            else UpdateTrail(wo, bar);
            if (TrailTriggered(wo, bar)) {
                filled = true;
                fill = MakeFill(wo, bar, bar.open, commissionPerShare, "trail stop trigger");
            }
            break;
        }

        case core::OrderType::TrailLimit: {
            if (wo.trailStop <= 0.0) InitTrail(wo, bar);
            else UpdateTrail(wo, bar);
            if (!wo.stopTriggered && TrailTriggered(wo, bar))
                wo.stopTriggered = true;
            if (wo.stopTriggered) {
                if (wo.order.side == core::OrderSide::Buy) {
                    if (bar.low <= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, bar,
                            std::min(bar.open, wo.order.limitPrice),
                            commissionPerShare, "trail-limit fill");
                    }
                } else {
                    if (bar.high >= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, bar,
                            std::max(bar.open, wo.order.limitPrice),
                            commissionPerShare, "trail-limit fill");
                    }
                }
            }
            break;
        }

        case core::OrderType::MOC:
            if (isSessionClose) {
                filled = true;
                fill = MakeFill(wo, bar, bar.close, commissionPerShare, "MOC fill");
            }
            break;

        case core::OrderType::LOC:
            if (isSessionClose) {
                bool triggered = false;
                if (wo.order.side == core::OrderSide::Buy)
                    triggered = (bar.low <= wo.order.limitPrice);
                else
                    triggered = (bar.high >= wo.order.limitPrice);
                if (triggered) {
                    filled = true;
                    fill = MakeFill(wo, bar, bar.close, commissionPerShare, "LOC fill");
                }
            }
            break;

        case core::OrderType::MTL:
            filled = true;
            fill = MakeFill(wo, bar, bar.close, commissionPerShare, "MTL fill");
            break;

        case core::OrderType::MIT:
            if (wo.order.side == core::OrderSide::Buy) {
                if (bar.high >= wo.order.auxPrice) {
                    filled = true;
                    fill = MakeFill(wo, bar, bar.open, commissionPerShare, "MIT trigger");
                }
            } else {
                if (bar.low <= wo.order.auxPrice) {
                    filled = true;
                    fill = MakeFill(wo, bar, bar.open, commissionPerShare, "MIT trigger");
                }
            }
            break;

        case core::OrderType::LIT:
            if (!wo.stopTriggered) {
                // MIT leg
                if (wo.order.side == core::OrderSide::Buy) {
                    if (bar.high >= wo.order.auxPrice) wo.stopTriggered = true;
                } else {
                    if (bar.low <= wo.order.auxPrice) wo.stopTriggered = true;
                }
            }
            if (wo.stopTriggered) {
                // Limit leg
                if (wo.order.side == core::OrderSide::Buy) {
                    if (bar.low <= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, bar,
                            std::min(bar.open, wo.order.limitPrice),
                            commissionPerShare, "LIT fill");
                    }
                } else {
                    if (bar.high >= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, bar,
                            std::max(bar.open, wo.order.limitPrice),
                            commissionPerShare, "LIT fill");
                    }
                }
            }
            break;

        case core::OrderType::Midprice: {
            double mid = (bar.high + bar.low) / 2.0;
            double fillPx = mid;
            if (wo.order.limitPrice > 0.0 && mid > wo.order.limitPrice)
                fillPx = wo.order.limitPrice;
            filled = true;
            fill = MakeFill(wo, bar, fillPx, commissionPerShare, "midprice fill");
            break;
        }

        case core::OrderType::Relative: {
            double peg = wo.order.side == core::OrderSide::Buy
                ? bar.open - wo.order.auxPrice
                : bar.open + wo.order.auxPrice;
            filled = true;
            fill = MakeFill(wo, bar, peg, commissionPerShare, "relative fill");
            break;
        }

        }  // switch

        if (filled) {
            result.fills.push_back(fill);
            result.filledIds.push_back(wo.localId);
        }
    }

    // TIF DAY auto-cancel at session close
    if (isSessionClose) {
        for (auto& wo : book) {
            if (wo.order.tif == core::TimeInForce::Day) {
                // Don't duplicate if already filled this bar
                if (std::find(result.filledIds.begin(), result.filledIds.end(),
                              wo.localId) == result.filledIds.end()) {
                    result.filledIds.push_back(wo.localId);
                }
            }
        }
    }

    std::sort(result.filledIds.begin(), result.filledIds.end());
    return result;
}

// ---- EvaluateTick (per-tick resolution) -------------------------------------

// Processes a single historical tick against the working order book.
// TRADES ticks trigger Stop / Market / MIT; BID_ASK ticks trigger Limit
// (BUY checks ask, SELL checks bid); MIDPOINT ticks feed Midprice orders.
//
// The book is mutated (same stateful types as EvaluateBar). The caller
// is responsible for calling this on every tick in chronological order.
inline EvaluateResult EvaluateTick(SimulatedOrderBook& book,
                                   const core::HistoricalTick& tick,
                                   double commissionPerShare = kDefaultCommissionPerShare,
                                   bool isSessionClose = false) {
    EvaluateResult result;

    for (auto& wo : book) {
        if (wo.placedAt > tick.time) continue;

        // Market-family orders can't fill on the placement tick
        bool isMarketFamily = (wo.order.type == core::OrderType::Market ||
                               wo.order.type == core::OrderType::MTL ||
                               wo.order.type == core::OrderType::Midprice ||
                               wo.order.type == core::OrderType::Relative);
        if (isMarketFamily && wo.placedAt == tick.time) continue;

        SimulatedFill fill{};
        bool filled = false;

        switch (wo.order.type) {

        case core::OrderType::Market:
            if (tick.type == core::TickType::Trades && tick.price > 0.0) {
                filled = true;
                fill = MakeFill(wo, tick, tick.price,
                                commissionPerShare, "market fill");
            }
            break;

        case core::OrderType::Limit:
            if (tick.type == core::TickType::BidAsk) {
                if (wo.order.side == core::OrderSide::Buy) {
                    if (tick.askPrice > 0.0 && tick.askPrice <= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.askPrice,
                                        commissionPerShare, "limit fill");
                    }
                } else {
                    if (tick.bidPrice > 0.0 && tick.bidPrice >= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.bidPrice,
                                        commissionPerShare, "limit fill");
                    }
                }
            }
            break;

        case core::OrderType::Stop:
            if (tick.type == core::TickType::Trades && tick.price > 0.0) {
                if (wo.order.side == core::OrderSide::Buy) {
                    if (tick.price >= wo.order.stopPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.price,
                                        commissionPerShare, "stop trigger");
                    }
                } else {
                    if (tick.price <= wo.order.stopPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.price,
                                        commissionPerShare, "stop trigger");
                    }
                }
            }
            break;

        case core::OrderType::StopLimit:
            if (tick.type == core::TickType::Trades && tick.price > 0.0) {
                if (!wo.stopTriggered) {
                    if (wo.order.side == core::OrderSide::Buy) {
                        if (tick.price >= wo.order.stopPrice) wo.stopTriggered = true;
                    } else {
                        if (tick.price <= wo.order.stopPrice) wo.stopTriggered = true;
                    }
                }
            }
            if (wo.stopTriggered && tick.type == core::TickType::BidAsk) {
                if (wo.order.side == core::OrderSide::Buy) {
                    if (tick.askPrice > 0.0 && tick.askPrice <= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.askPrice,
                                        commissionPerShare, "stop-limit fill");
                    }
                } else {
                    if (tick.bidPrice > 0.0 && tick.bidPrice >= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.bidPrice,
                                        commissionPerShare, "stop-limit fill");
                    }
                }
            }
            break;

        case core::OrderType::MIT:
            if (tick.type == core::TickType::Trades && tick.price > 0.0) {
                if (wo.order.side == core::OrderSide::Buy) {
                    if (tick.price >= wo.order.auxPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.price,
                                        commissionPerShare, "MIT trigger");
                    }
                } else {
                    if (tick.price <= wo.order.auxPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.price,
                                        commissionPerShare, "MIT trigger");
                    }
                }
            }
            break;

        case core::OrderType::LIT:
            if (tick.type == core::TickType::Trades && tick.price > 0.0) {
                if (!wo.stopTriggered) {
                    if (wo.order.side == core::OrderSide::Buy) {
                        if (tick.price >= wo.order.auxPrice) wo.stopTriggered = true;
                    } else {
                        if (tick.price <= wo.order.auxPrice) wo.stopTriggered = true;
                    }
                }
            }
            if (wo.stopTriggered && tick.type == core::TickType::BidAsk) {
                if (wo.order.side == core::OrderSide::Buy) {
                    if (tick.askPrice > 0.0 && tick.askPrice <= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.askPrice,
                                        commissionPerShare, "LIT fill");
                    }
                } else {
                    if (tick.bidPrice > 0.0 && tick.bidPrice >= wo.order.limitPrice) {
                        filled = true;
                        fill = MakeFill(wo, tick, tick.bidPrice,
                                        commissionPerShare, "LIT fill");
                    }
                }
            }
            break;

        case core::OrderType::Midprice:
            if (tick.type == core::TickType::Midpoint && tick.price > 0.0) {
                double fillPx = tick.price;
                if (wo.order.limitPrice > 0.0 && tick.price > wo.order.limitPrice)
                    fillPx = wo.order.limitPrice;
                filled = true;
                fill = MakeFill(wo, tick, fillPx,
                                commissionPerShare, "midprice fill");
            }
            break;

        // Trail / TrailLimit / MOC / LOC / MTL / Relative: tick-resolution
        // evaluation is complex (trail needs bar OHLC for reference update,
        // closing orders need session-end awareness). For v1 the tick path
        // defers to the bar path for these types — they fall through as no-ops.

        default:
            break;
        }

        if (filled) {
            result.fills.push_back(fill);
            result.filledIds.push_back(wo.localId);
        }
    }

    // TIF DAY auto-cancel at session close
    if (isSessionClose) {
        for (auto& wo : book) {
            if (wo.order.tif == core::TimeInForce::Day) {
                if (std::find(result.filledIds.begin(), result.filledIds.end(),
                              wo.localId) == result.filledIds.end()) {
                    result.filledIds.push_back(wo.localId);
                }
            }
        }
    }

    std::sort(result.filledIds.begin(), result.filledIds.end());
    return result;
}

}  // namespace core::services
