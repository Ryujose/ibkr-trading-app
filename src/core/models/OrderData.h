#pragma once

#include <string>
#include <ctime>

namespace core {

// ---- Enumerations -----------------------------------------------------------

enum class OrderSide   { Buy, Sell };
enum class OrderType {
    Market, Limit, Stop, StopLimit,          // basic
    Trail, TrailLimit,                        // trailing stop
    MOC, LOC, MTL,                            // market/limit variants
    MIT, LIT,                                 // if-touched
    Midprice, Relative                        // smart / pegged-to-primary
};
enum class TimeInForce { Day, GTC, IOC, FOK, Overnight, OPG };
enum class OrderStatus { Pending, Working, PartialFill, Filled,
                         Cancelled, Rejected, PendingCancel };

// ---- Market depth -----------------------------------------------------------

struct DepthLevel {
    double      price    = 0.0;
    double      size     = 0.0;
    int         numOrders = 1;
    float       flashAge = 0.0f;  // seconds since last price change (for highlight)
    std::string exchange;         // exchange name (empty for L1 SMART depth)
};

// ---- Tick (time & sales) ----------------------------------------------------

struct Tick {
    double      price        = 0.0;
    double      size         = 0.0;
    std::time_t timestamp    = 0;
    bool        isUptick     = true;   // vs downtick; false = downtick, neutral when equal
    bool        isNeutral    = false;  // price == previous price
    std::string exchange;
    std::string specialConds;
};

// ---- Order ------------------------------------------------------------------

struct Order {
    int         orderId     = 0;
    std::string symbol;
    OrderSide   side        = OrderSide::Buy;
    OrderType   type        = OrderType::Market;
    TimeInForce tif         = TimeInForce::Day;
    double      quantity       = 0.0;
    double      limitPrice     = 0.0;
    double      stopPrice      = 0.0;
    double      auxPrice       = 0.0;   // trigger for MIT/LIT; offset for REL; trail $ for TRAIL*
    double      trailingPercent= 0.0;   // trailing % for TRAIL / TRAIL LIMIT
    double      trailStopPrice = 0.0;   // initial stop cap for TRAIL / TRAIL LIMIT (optional)
    double      lmtPriceOffset = 0.0;   // limit offset from trail stop for TRAIL LIMIT
    bool        outsideRth     = false; // allow pre/after-hours fills
    std::string account;               // IB account code (required for multi-account / FA)
    std::string exchange;              // routing exchange; empty / "SMART" = IB smart routing
    int         parentId       = 0;    // 0 = no parent; non-zero = bracket child
    std::string ocaGroup;              // OCA group id; siblings sharing this id are linked
    int         ocaType        = 0;    // 0 = none, 1 = cancel-with-block, 2/3 = reduce variants
    bool        transmit       = true; // IB transmit flag; false = stage, true = activate
    double      filledQty      = 0.0;
    double      avgFillPrice = 0.0;
    double      commission   = 0.0;  // actual commission from fills (or estimate from OrderState)
    OrderStatus status       = OrderStatus::Pending;
    std::string rejectReason;        // IB error message when status == Rejected
    std::time_t submittedAt  = 0;
    std::time_t updatedAt    = 0;
};

// ---- Fill (execution report) ------------------------------------------------

struct Fill {
    int         orderId     = 0;
    std::string execId;       // IB execution ID — correlates execDetails ↔ commissionReport
    std::string symbol;
    OrderSide   side        = OrderSide::Buy;
    double      quantity    = 0.0;
    double      price       = 0.0;
    double      commission  = 0.0;
    double      realizedPnL = 0.0;  // populated by commissionReport callback
    std::time_t timestamp   = 0;
};

// ---- String helpers ---------------------------------------------------------

inline const char* OrderSideStr(OrderSide s) {
    return s == OrderSide::Buy ? "BUY" : "SELL";
}
inline const char* OrderTypeStr(OrderType t) {
    switch (t) {
        case OrderType::Market:    return "MKT";
        case OrderType::Limit:     return "LMT";
        case OrderType::Stop:      return "STP";
        case OrderType::StopLimit: return "STP LMT";
        case OrderType::Trail:     return "TRAIL";
        case OrderType::TrailLimit:return "TRAIL LIMIT";
        case OrderType::MOC:       return "MOC";
        case OrderType::LOC:       return "LOC";
        case OrderType::MTL:       return "MTL";
        case OrderType::MIT:       return "MIT";
        case OrderType::LIT:       return "LIT";
        case OrderType::Midprice:  return "MIDPRICE";
        case OrderType::Relative:  return "REL";
        default:                   return "?";
    }
}
inline const char* TIFStr(TimeInForce t) {
    switch (t) {
        case TimeInForce::Day: return "DAY";
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK:       return "FOK";
        case TimeInForce::Overnight: return "OVERNIGHT";
        case TimeInForce::OPG:       return "OPG";
        default:                     return "?";
    }
}
inline const char* OrderStatusStr(OrderStatus s) {
    switch (s) {
        case OrderStatus::Pending:       return "PENDING";
        case OrderStatus::Working:       return "WORKING";
        case OrderStatus::PartialFill:   return "PARTIAL";
        case OrderStatus::Filled:        return "FILLED";
        case OrderStatus::Cancelled:     return "CANCELLED";
        case OrderStatus::Rejected:      return "REJECTED";
        case OrderStatus::PendingCancel: return "CANCELLING";
        default:                         return "?";
    }
}

// ============================================================================
// PendingBracketStop — bracket stop-loss / take-profit parameters staged at
// entry-fill time.  Moved here from ChartWindow.h so it can be unit-tested.
// ============================================================================
struct PendingBracketStop {
    std::string  symbol;
    OrderSide    stopSide    = OrderSide::Buy;
    double       qty         = 0.0;
    double       stopPrice   = 0.0;
    double       tpPrice     = 0.0;   // take-profit limit; 0 = no TP leg
    bool         outsideRth  = false; // allow fill outside RTH once stop triggers
    bool         useStopLmt  = false; // STP LMT instead of STP (safer outside RTH)
};

}  // namespace core
