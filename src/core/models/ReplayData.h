#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "core/models/MarketData.h"
#include "core/models/OrderData.h"

namespace core {

// ============================================================================
// Replay-mode data models. Plain POD types — no IB API / ImGui dependency.
// Consumed by core::services::ReplayEngine and ui::ReplayWindow.
// See .claude/plans/replay.md.
// ============================================================================

// One historical tick. Mirrors the three IB tick streams a Replay session can
// fetch via reqHistoricalTicks: TRADES, BID_ASK, MIDPOINT. The engine's
// EvaluateTick() picks fields per type:
//   TRADES   → price, size                  (use for STP/MKT/MIT trigger)
//   BID_ASK  → bidPrice/askPrice/sizes      (use for LMT trigger)
//   MIDPOINT → price                        (use for MIDPRICE / sanity)
enum class TickType { Trades, BidAsk, Midpoint };

struct HistoricalTick {
    TickType    type      = TickType::Trades;
    std::time_t time      = 0;
    double      price     = 0.0;   // TRADES / MIDPOINT only
    double      size      = 0.0;   // TRADES only
    double      bidPrice  = 0.0;   // BID_ASK only
    double      askPrice  = 0.0;   // BID_ASK only
    double      bidSize   = 0.0;   // BID_ASK only
    double      askSize   = 0.0;   // BID_ASK only
    std::string exchange;          // TRADES only
    std::string specialConds;      // TRADES only
};

// A contiguous historical range for a single symbol — one or more trading days
// of bars, with ticks (cursor-day-only) and the user's real fills filtered to
// that range. See .claude/plans/replay-indicators.md §3a.
//
// `dateFrom == dateTo` is the single-day case (back-compat with the original
// HistoricalDay shape). The `HistoricalDay` alias below keeps existing call
// sites compiling; new code should prefer `HistoricalRange`.
//
// `ticksDate` records *which* day inside [dateFrom, dateTo] the tick stream
// covers — empty when no ticks loaded. The engine's tick evaluator falls back
// to bar mode when the cursor sits outside `ticksDate`.
struct HistoricalRange {
    std::string                       symbol;
    std::string                       dateFrom;     // "YYYY-MM-DD"
    std::string                       dateTo;       // "YYYY-MM-DD" (== dateFrom for one day)
    std::vector<core::Bar>            bars;         // any TF (M1 by default)
    std::vector<core::HistoricalTick> ticks;        // empty unless tick-fills toggle on
    std::string                       ticksDate;    // which day the ticks cover (empty = none)
    std::vector<core::Fill>           userFills;    // real fills from g_executions, filtered
    std::time_t                       fetchedAt = 0;
};

// Alias for back-compat. Single-day call sites can keep using HistoricalDay;
// `date` reads should switch to `dateFrom` (or `dateTo` — they're equal).
using HistoricalDay = HistoricalRange;

}  // namespace core
