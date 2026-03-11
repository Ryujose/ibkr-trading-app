#pragma once

#include "core/models/OrderData.h"
#include <vector>
#include <string>
#include <deque>
#include <chrono>
#include <random>
#include <functional>

namespace ui {

// ============================================================================
// TradingWindow
//
// Layout (internal child panels inside one ImGui window):
//
//  ┌──────────────┬──────────────┬──────────────┐
//  │  Order Book  │ Order Entry  │ Time & Sales │
//  │  (Level II)  │              │              │
//  ├──────────────┴──────────────┴──────────────┤
//  │  Open Orders  │  Execution Log             │
//  └─────────────────────────────────────────────┘
//
// IB Gateway integration: replace Simulate*() bodies with EClient calls:
//   reqMktDepth()         → fills m_bids / m_asks via OnDepthUpdate()
//   placeOrder()          → SubmitOrder() calls EClient::placeOrder()
//   cancelOrder()         → CancelOrder() calls EClient::cancelOrder()
//   EWrapper::orderStatus → OnOrderStatus()
//   EWrapper::execDetails → OnFill()
// ============================================================================
class TradingWindow {
public:
    TradingWindow();
    ~TradingWindow() = default;

    // Call once per frame. Returns false when the window is closed.
    bool Render();

    // --- IB Gateway callbacks (future task) ---
    void OnDepthUpdate(bool isBid, int pos, double price, double size);
    void OnOrderStatus(int orderId, core::OrderStatus status,
                       double filled, double avgPrice);
    void OnFill(const core::Fill& fill);
    void OnTick(double price, double size, bool isUptick);

    // Seed the mid-price when a symbol changes (from market data window later)
    void SetSymbol(const std::string& symbol, double midPrice);

    // Called by main once IB is connected
    void SetNextOrderId(int id);
    std::function<void(int orderId, const std::string& sym,
                       const std::string& action,    // "BUY"|"SELL"
                       const std::string& orderType, // "MKT"|"LMT"|"STP"|"STPLMT"
                       double qty, double limitPrice)> OnOrderSubmit;
    std::function<void(int orderId)> OnOrderCancel;

private:
    // ---- Window state -------------------------------------------------------
    bool m_open = true;
    bool m_hasRealData = false;

    // ---- Symbol / price -----------------------------------------------------
    char   m_symbol[16]     = "AAPL";
    double m_midPrice       = 185.50;
    double m_prevMidPrice   = 185.50;

    // ---- Order Book ---------------------------------------------------------
    static constexpr int kDepthLevels = 12;
    std::vector<core::DepthLevel> m_asks;  // ascending (closest spread first)
    std::vector<core::DepthLevel> m_bids;  // descending (closest spread first)
    double m_maxDepthSize = 1.0;           // for normalising bar widths

    void InitOrderBook();
    void SimulateBookTick(float dtSec);
    void DrawOrderBook();
    void DrawDepthTable(const std::vector<core::DepthLevel>& levels,
                        bool isAsk, float barMaxWidth);

    // ---- Order entry form ---------------------------------------------------
    int    m_sideIdx     = 0;   // 0=Buy 1=Sell
    int    m_typeIdx     = 0;   // 0=Market 1=Limit 2=Stop 3=StopLimit
    int    m_tifIdx      = 0;   // 0=DAY 1=GTC 2=IOC 3=FOK
    char   m_qtyBuf[32]  = "100";
    char   m_lmtBuf[32]  = "";
    char   m_stpBuf[32]  = "";
    bool   m_showConfirm = false;

    void DrawOrderEntry();
    bool ValidateOrder(std::string& err) const;
    void SubmitOrder();

    // ---- Confirmation popup -------------------------------------------------
    void DrawConfirmationPopup();

    // ---- Open orders --------------------------------------------------------
    std::vector<core::Order> m_openOrders;
    int m_nextOrderId = 1001;

    void DrawOpenOrders();
    void CancelOrder(int orderId);
    void SimulateOrderLifecycle(float dtSec);

    // ---- Execution log ------------------------------------------------------
    std::vector<core::Fill> m_fills;

    void DrawExecutionLog();

    // ---- Time & Sales -------------------------------------------------------
    static constexpr int kMaxTicks = 80;
    std::deque<core::Tick> m_ticks;

    void SimulateNewTick();
    void DrawTimeSales();

    // ---- Simulation timing --------------------------------------------------
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastBookUpdate;
    Clock::time_point m_lastTickUpdate;
    Clock::time_point m_lastOrderCheck;
    float m_bookUpdateIntervalSec = 0.40f;
    float m_tickIntervalSec       = 0.25f;
    float m_orderCheckIntervalSec = 0.50f;

    // ---- RNG ----------------------------------------------------------------
    std::mt19937                        m_rng;
    std::normal_distribution<double>    m_priceDist{0.0, 0.04};
    std::uniform_real_distribution<double> m_sizeDist{50.0, 1500.0};
    std::uniform_real_distribution<double> m_uniform{0.0, 1.0};

    // ---- Helpers ------------------------------------------------------------
    static std::string FmtTime(std::time_t t);
    static std::string FmtPrice(double p) ;
    static double      RoundTick(double price, double tick = 0.01);
};

}  // namespace ui
