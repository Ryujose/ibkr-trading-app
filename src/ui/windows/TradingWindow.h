#pragma once

#include "core/models/OrderData.h"
#include <vector>
#include <string>
#include <deque>
#include <functional>
#include <unordered_map>

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

    // Subscription status per data stream
    enum class SubStatus { Unknown, Ok, NeedSubscription, NotAllowed };

    // ── IB Gateway callbacks ─────────────────────────────────────────────────
    // op: 0=insert, 1=update, 2=delete
    void OnDepthUpdate(int reqId, bool isBid, int pos, int op,
                       double price, double size);
    void OnOrderStatus(int orderId, core::OrderStatus status,
                       double filled, double avgPrice);
    void OnFill(const core::Fill& fill);
    void OnTick(double price, double size, bool isUptick);
    void OnNBBO(double bid, double bidSz, double ask, double askSz);
    void OnMktDataError(int code);
    void OnDepthError(int code);

    void SetSymbol(const std::string& symbol, double midPrice);
    void UpdateMidPrice(double price);
    void SetNextOrderId(int id);

    std::function<void(int orderId, const std::string& sym,
                       const std::string& action,
                       const std::string& orderType,
                       double qty,
                       double price,          // lmt price for LMT; stop trigger for STP/STP LMT
                       double auxPrice,       // limit price for STP LMT; 0 otherwise
                       const std::string& tif,  // "DAY","GTC","IOC","FOK"
                       bool outsideRth           // true = allow pre/after-hours fills
                       )> OnOrderSubmit;
    std::function<void(int orderId)> OnOrderCancel;
    // Fired when the user types a new symbol and presses Enter in Order Entry.
    std::function<void(const std::string& symbol)> OnSymbolChanged;

private:
    bool m_open = true;

    // ── Subscription status ──────────────────────────────────────────────────
    SubStatus m_mktDataStatus = SubStatus::Unknown;
    SubStatus m_depthStatus   = SubStatus::Unknown;

    // ── NBBO ─────────────────────────────────────────────────────────────────
    double m_nbboBid   = 0.0;
    double m_nbboAsk   = 0.0;
    double m_nbboBidSz = 0.0;
    double m_nbboAskSz = 0.0;

    // ── Symbol / price ───────────────────────────────────────────────────────
    char   m_symbol[16]   = "AAPL";
    double m_midPrice     = 0.0;
    double m_prevMidPrice = 0.0;

    // ── Order Book (Level II) ────────────────────────────────────────────────
    static constexpr int kDepthLevels = 50;
    std::vector<core::DepthLevel> m_asks;
    std::vector<core::DepthLevel> m_bids;
    double m_maxDepthSize = 1.0;

    // ── Volume profile (accumulates traded size per price level) ─────────────
    // key = round(price / 0.01) as int, value = cumulative traded size
    std::unordered_map<int, double> m_volAtPrice;
    double m_maxVolAtPrice = 1.0;

    // ── Click-to-trade ───────────────────────────────────────────────────────
    bool m_clickToTrade  = false;
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
    int  m_sideIdx     = 0;
    int  m_typeIdx     = 0;
    int  m_tifIdx      = 0;
    char m_qtyBuf[32]  = "100";
    char m_lmtBuf[32]  = "";
    char m_stpBuf[32]  = "";
    bool m_outsideRth  = false;
    bool m_showConfirm = false;

    void DrawOrderEntry();
    bool ValidateOrder(std::string& err) const;
    void SubmitOrder();
    void DrawConfirmationPopup();

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
    static constexpr int kMaxTicks = 80;
    std::deque<core::Tick> m_ticks;
    void DrawTimeSales();

    // ── Helpers ──────────────────────────────────────────────────────────────
    static std::string FmtTime(std::time_t t);
    static double      RoundTick(double price, double tick = 0.01);
};

}  // namespace ui
