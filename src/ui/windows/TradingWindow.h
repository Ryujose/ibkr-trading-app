#pragma once

#include "core/models/OrderData.h"
#include <vector>
#include <string>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace ui {

// ============================================================================
// TradingWindow  ("Book Trading") — real-data-only (IB Gateway / TWS API)
//
//  ┌──────────────────────────┬───────────────────┐
//  │  DOM Ladder              │  Order Entry       │
//  │  BidSz|CumBid|Price|     │                   │
//  │       CumAsk|AskSz|P&L|Bar                   │
//  ├──────────────────────────┴───────────────────┤
//  │  Open Orders │ Execution Log │ Time & Sales   │
//  └───────────────────────────────────────────────┘
// ============================================================================
class TradingWindow {
public:
    TradingWindow();
    ~TradingWindow() = default;

    bool Render();
    bool& open() { return m_open; }
    void setGroupId(int id)    { m_groupId = id; }
    int  groupId() const       { return m_groupId; }
    void setInstanceId(int id);
    int  instanceId() const    { return m_instanceId; }
    bool useL2() const         { return m_useL2; }
    int  numDepthRows() const  { return m_numDepthRows; }
    void setNumDepthRows(int n);

    // Subscription status per data stream
    enum class SubStatus { Unknown, Ok, NeedSubscription, NotAllowed };

    // ── IB Gateway callbacks ─────────────────────────────────────────────────
    // op: 0=insert, 1=update, 2=delete
    void OnDepthUpdate(int reqId, bool isBid, int pos, int op,
                       double price, double size,
                       const std::string& exchange = "",
                       bool isSmartDepth = false);
    void OnOrderStatus(int orderId, core::OrderStatus status,
                       double filled, double avgPrice);
    void OnFill(const core::Fill& fill);
    void OnTick(double price, double size, bool isUptick);
    void OnTickByTick(const core::Tick& tick);
    void OnNBBO(double bid, double bidSz, double ask, double askSz);
    void OnMktDataError(int code);
    void OnDepthError(int code);

    std::string getSymbol() const { return m_symbol; }
    void SetSymbol(const std::string& symbol, double midPrice);
    void UpdateMidPrice(double price);
    void SetNextOrderId(int id);
    // Inject position from IB portfolio data (qty>0 long, <0 short, 0 flat)
    void SetPosition(double qty, double avgCost);

    // Unguarded-position warning hint — pushed once per frame from main.cpp.
    // active=false (or different-symbol) clears the strip. Same shape as
    // ChartWindow::UnguardedHint; declared here so the two windows stay
    // independent (no header coupling between TradingWindow and ChartWindow).
    struct UnguardedHint {
        bool        active   = false;
        std::string symbol;
        double      qty      = 0.0;
        double      avgCost  = 0.0;
        double      stopTrig = 0.0;
        double      stopLmt  = 0.0;
        double      pctRisk  = 0.0;
    };
    void SetUnguardedSuggestion(const UnguardedHint& h);

    // Receive auto-detected support/resistance levels from the first chart
    // window on the same symbol. Used to overlay S/R markers in the DOM.
    // atrLast = 0.0 means "not yet computed" — markers stay hidden.
    void SetAutoLevels(const std::vector<double>& supports,
                       const std::vector<double>& resistances,
                       double atrLast);

    std::function<void(const core::Order&)> OnOrderSubmit;
    std::function<void(int orderId)> OnOrderCancel;
    // Fired when the user types a new symbol and presses Enter in Order Entry.
    std::function<void(const std::string& symbol)> OnSymbolChanged;

    // Fired when the user toggles between L1 aggregated depth and L2 per-exchange
    // depth.  main.cpp cancels the current depth subscription and re-subscribes
    // with the appropriate isSmartDepth flag.
    std::function<void(bool useL2)> OnDepthModeChanged;

    // Replace the exchange combo list with fresh smart-component data.
    // Always leads with "SMART"; resets selected index to 0.
    void SetExchangeList(const std::vector<std::string>& exchanges);

private:
    bool m_open       = true;
    int  m_groupId    = 0;
    int  m_instanceId = 1;
    char m_title[40]  = "Book Trading 1##trading1";

    // ── Subscription status ──────────────────────────────────────────────────
    SubStatus m_mktDataStatus = SubStatus::Unknown;
    SubStatus m_depthStatus   = SubStatus::Unknown;

    // ── NBBO ─────────────────────────────────────────────────────────────────
    double m_nbboBid   = 0.0;
    double m_nbboAsk   = 0.0;
    double m_nbboBidSz = 0.0;
    double m_nbboAskSz = 0.0;

    // ── Symbol / price ───────────────────────────────────────────────────────
    char   m_symbol[32]   = "AAPL";
    double m_midPrice     = 0.0;
    double m_prevMidPrice = 0.0;
    double m_lastPrice    = 0.0;   // last traded price (for DOM row highlight)

    // ── Position tracking (updated from OnFill) ──────────────────────────────
    double m_positionQty   = 0.0;   // + = long, - = short, 0 = flat
    double m_avgEntryPrice = 0.0;

    // ── Order Book (Level II) ────────────────────────────────────────────────
    static constexpr int kDepthLevels = 300;  // max display cap; request rows is separate
    int m_numDepthRows = 25;  // numRows sent to IB, synced with m_ladderRows
    std::vector<core::DepthLevel> m_asks;
    std::vector<core::DepthLevel> m_bids;
    double m_maxDepthSize = 1.0;

    // Per-exchange L2 mode (toggle in DOM header)
    bool m_useL2             = false;
    int  m_exchangeFilterIdx = 0;   // index into m_exchangeList for L2 filtering
    // Buckets keyed by exchange name, one full level list per side
    std::unordered_map<std::string, std::vector<core::DepthLevel>> m_askBuckets;
    std::unordered_map<std::string, std::vector<core::DepthLevel>> m_bidBuckets;
    void RefreshExchangeFilterList();
    void RebuildDepthView();

    // ── Volume profile (accumulates traded size per price level) ─────────────
    // key = round(price / 0.01) as int, value = cumulative traded size
    std::unordered_map<int, double> m_volAtPrice;
    double m_maxVolAtPrice = 1.0;

    // ── Click-to-trade ───────────────────────────────────────────────────────
    bool m_clickToTrade  = false;
    bool m_expandSpread  = true;    // show individual tick rows inside the spread
    int  m_ladderRows    = 25;    // virtual price levels above ask / below bid
    int  m_ladderRowsIdx = 4;     // index into kLadderOptions[] — default 25 (index 4)

    struct DOMOrder {
        int               orderId;
        double            price;
        bool              isBuy;
        core::OrderStatus status;
        float             fillAge;   // seconds since fill; -1 if not yet filled
    };
    std::vector<DOMOrder> m_domOrders;

    void     DrawOrderBook();
    void     PlaceDomOrder(bool isBuy, double price);
    DOMOrder* FindDomOrder(double price);

    // ── Order entry ──────────────────────────────────────────────────────────
    int  m_sideIdx          = 0;
    int  m_typeIdx          = 0;
    int  m_tifIdx           = 0;
    int  m_exchangeIdx      = 0;
    std::vector<std::string> m_exchangeList = {"SMART"};
    char m_qtyBuf[32]       = "100";
    char m_lmtBuf[32]       = "";    // limit price / lmt offset (TRAIL LIMIT) / price cap (MIDPRICE/REL)
    char m_stpBuf[32]       = "";    // stop price / trigger price (MIT/LIT) / trail stop cap
    char m_trailAmtBuf[32]  = "1.00"; // trailing amount in $ (TRAIL / TRAIL LIMIT)
    char m_trailPctBuf[32]  = "1.0";  // trailing percent (TRAIL / TRAIL LIMIT)
    bool m_trailByPct       = false;   // false = trail by $ amount, true = trail by %
    char m_offsetBuf[32]    = "0.05"; // peg offset for Relative orders
    bool m_outsideRth       = false;
    bool m_showConfirm      = false;

    void DrawOrderEntry();
    bool ValidateOrder(std::string& err) const;
    void SubmitOrder();
    void DrawConfirmationPopup();
    void DrawUnguardedStrip();   // yellow protective-stop warning above order entry
    void DrawOrderImpactBadge();  // side-intent + P&amp;L preview below order form

    // ── Unguarded-position warning state ────────────────────────────────────
    UnguardedHint                   m_unguarded;
    std::unordered_set<std::string> m_dismissedUnguarded;
    double                          m_lastWarnedQty = 0.0;

    // ── Open orders ──────────────────────────────────────────────────────────
    std::vector<core::Order> m_openOrders;
    int m_nextOrderId = 1001;

    void DrawOpenOrders();
    void CancelOrder(int orderId);
    void PruneFinishedOrders();

    // ── Execution log ────────────────────────────────────────────────────────
    std::vector<core::Fill> m_fills;
    void DrawExecutionLog();

    // ── Time & Sales ─────────────────────────────────────────────────────────
    static constexpr int kMaxTicks = 2000;
    std::deque<core::Tick> m_ticks;
    double m_maxTickSize = 1.0;   // max size in current visible window (for histogram bar)
    void DrawTimeSales();

    // ── Layout ratios (user-draggable splitters) ─────────────────────────────
    float m_topHeightRatio  = 0.65f;   // top / total height
    float m_bookWidthRatio  = 0.54f;   // book panel / total width

    // ── Auto S/R overlay from chart ──────────────────────────────────────────
    std::vector<double> m_supportPrices;
    std::vector<double> m_resistancePrices;
    double              m_atrForSR = 0.0;   // ATR(14) for proximity tolerance

    // ── Helpers ──────────────────────────────────────────────────────────────
    static std::string FmtTime(std::time_t t);
    static double      RoundTick(double price, double tick = 0.01);
};

}  // namespace ui
