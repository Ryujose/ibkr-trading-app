#pragma once

#include <string>
#include <ctime>

namespace core {

// ---- Enumerations -----------------------------------------------------------

enum class OrderSide   { Buy, Sell };
enum class OrderType   { Market, Limit, Stop, StopLimit };
enum class TimeInForce { Day, GTC, IOC, FOK };
enum class OrderStatus { Pending, Working, PartialFill, Filled, Cancelled, Rejected };

// ---- Market depth -----------------------------------------------------------

struct DepthLevel {
    double price    = 0.0;
    double size     = 0.0;
    int    numOrders = 1;
    float  flashAge = 0.0f;  // seconds since last price change (for highlight)
};

// ---- Tick (time & sales) ----------------------------------------------------

struct Tick {
    double      price    = 0.0;
    double      size     = 0.0;
    std::time_t timestamp = 0;
    bool        isUptick = true;   // vs downtick
};

// ---- Order ------------------------------------------------------------------

struct Order {
    int         orderId     = 0;
    std::string symbol;
    OrderSide   side        = OrderSide::Buy;
    OrderType   type        = OrderType::Market;
    TimeInForce tif         = TimeInForce::Day;
    double      quantity    = 0.0;
    double      limitPrice  = 0.0;
    double      stopPrice   = 0.0;
    double      filledQty   = 0.0;
    double      avgFillPrice = 0.0;
    OrderStatus status      = OrderStatus::Pending;
    std::time_t submittedAt = 0;
    std::time_t updatedAt   = 0;
};

// ---- Fill (execution report) ------------------------------------------------

struct Fill {
    int         orderId   = 0;
    std::string symbol;
    OrderSide   side      = OrderSide::Buy;
    double      quantity  = 0.0;
    double      price     = 0.0;
    double      commission = 0.0;
    std::time_t timestamp = 0;
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
        default:                   return "?";
    }
}
inline const char* TIFStr(TimeInForce t) {
    switch (t) {
        case TimeInForce::Day: return "DAY";
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        default:               return "?";
    }
}
inline const char* OrderStatusStr(OrderStatus s) {
    switch (s) {
        case OrderStatus::Pending:     return "PENDING";
        case OrderStatus::Working:     return "WORKING";
        case OrderStatus::PartialFill: return "PARTIAL";
        case OrderStatus::Filled:      return "FILLED";
        case OrderStatus::Cancelled:   return "CANCELLED";
        case OrderStatus::Rejected:    return "REJECTED";
        default:                       return "?";
    }
}

}  // namespace core
