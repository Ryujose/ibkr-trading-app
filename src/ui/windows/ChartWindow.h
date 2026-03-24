#pragma once

#include "core/models/MarketData.h"
#include <string>
#include <vector>
#include <deque>
#include <functional>

struct ImDrawList;   // forward-declare to avoid pulling imgui.h into every TU

namespace ui {

// ============================================================================
// ChartWindow
// ============================================================================
class ChartWindow {
public:
    // ---- Drawing overlay types ----------------------------------------------
    enum class DrawTool { Cursor, HLine, TrendLine, Fibonacci, Erase };

    struct Drawing {
        enum class Type { HLine, TrendLine, Fibonacci } type;
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    };

    // ---- Pending order overlay -----------------------------------------------
    struct PendingOrderLine {
        int         orderId   = 0;
        double      price     = 0.0;   // stop price for STP LMT; limit for LMT; stop for STP
        double      auxPrice  = 0.0;   // limit price for STP LMT (0 for single-leg types)
        bool        isBuy     = true;
        double      qty       = 0.0;
        std::string orderType;         // IB order-type string: "LMT", "STP", "STP LMT", …
    };

    // ---- Current position info (for the P&L strip) --------------------------
    struct PositionInfo {
        bool   hasPosition = false;
        double qty         = 0.0;   // positive = long, negative = short
        double avgCost     = 0.0;   // entry price per share
        double lastPrice   = 0.0;   // most recent market price
        double unrealPnL   = 0.0;   // from portfolio update (IB-computed)
        double commission  = 0.0;   // total commissions paid for this symbol
    };

    ChartWindow();

    bool Render();
    bool& open() { return m_open; }
    void SetSymbol(const std::string& symbol);
    void setGroupId(int id) { m_groupId = id; }
    int  groupId() const    { return m_groupId; }
    void AddBar(const core::Bar& bar, bool done);
    void SetHistoricalData(const core::BarSeries& series);
    // Prepend older bars to the left of the current series (extend-history result).
    void PrependHistoricalData(const core::BarSeries& older);

    // Update the currently-forming (live) bar without a full reload.
    // Called on each historicalDataUpdate callback while keepUpToDate is active.
    void UpdateLiveBar(const core::Bar& bar);

    // Intraday only: update last bar close/high/low from a LAST price tick.
    // Fills gaps between historicalDataUpdate bar-close events.
    void OnLastPrice(double price);

    // Non-intraday (D1/W1/MN): update the forming daily bar from reqMktData ticks.
    //   field  4 = LAST price → close (also extends high/low)
    //   field 12 = DAY HIGH
    //   field 13 = DAY LOW
    //   field 14 = OPEN (day open)
    // Synthesises today's bar automatically when the first tick arrives.
    void OnDayTick(int field, double price);

    // Push current pending orders for this symbol; replaces the previous list.
    void SetPendingOrders(const std::vector<PendingOrderLine>& orders);

    // Push current position info for this symbol.
    void SetPosition(const PositionInfo& pos);

    // Fired when user changes symbol/timeframe/rth — host wires to ReqHistoricalData
    // useRTH=false → include pre/post-market bars
    std::function<void(const std::string& sym, core::Timeframe tf, bool useRTH)> OnDataRequest;

    // Fired when user pans left past the first bar to request older history.
    // endDateTime: IB-formatted "YYYYMMDD HH:MM:SS UTC" of the oldest known bar.
    std::function<void(const std::string& sym, core::Timeframe tf,
                       const std::string& endDateTime, bool useRTH)> OnExtendHistory;

    // Fired when user places an order from the chart trade panel
    // auxPrice: stop price for STP LMT, trail amount for TRAIL/TRAIL LIMIT/LIT/MIT, else 0
    std::function<void(const std::string& sym, const std::string& side,
                       const std::string& orderType, double qty, double price,
                       const std::string& tif, bool outsideRth,
                       double auxPrice)> OnOrderSubmit;

    // Fired when user clicks the ✕ button on a pending order line in the chart.
    std::function<void(int orderId)> OnCancelOrder;

    // Fired when user drags an existing order line to a new price.
    // newPrice    = new stop/trigger price (or the only price for single-leg types).
    // newAuxPrice = new limit price for STP LMT; 0.0 for single-leg types.
    // Host must cancel the old order and re-submit a replacement.
    std::function<void(int orderId, double newPrice, double newAuxPrice)> OnModifyOrder;

private:
    // ---- Indicator settings -------------------------------------------------
    struct IndicatorSettings {
        bool sma20   = true;
        bool sma50   = true;
        bool ema20   = false;
        bool bbands  = true;
        bool vwap    = true;
        bool volume  = true;
        bool rsi     = true;

        int   smaPeriod1 = 20;
        int   smaPeriod2 = 50;
        int   emaPeriod  = 20;
        int   bbPeriod   = 20;
        float bbSigma    = 2.0f;
        int   rsiPeriod  = 14;
    };

    // ---- State --------------------------------------------------------------
    int               m_groupId         = 0;
    char              m_symbol[16]      = "AAPL";
    core::Timeframe   m_timeframe       = core::Timeframe::D1;
    bool              m_needsRefresh    = true;
    bool              m_open            = true;
    bool              m_hasRealData     = false;
    bool              m_loading         = false;
    bool              m_useRTH          = false;  // false = include extended hours
    bool              m_showOvernight   = false;  // show overnight bars (separate toggle)
    bool              m_showSessions    = true;   // draw session background bands

    // Linked axis ranges (shared across all sub-plots)
    double            m_xMin            = 0.0;
    double            m_xMax            = 0.0;
    double            m_priceMin        = 0.0;
    double            m_priceMax        = 0.0;
    bool              m_viewInitialized = false;

    IndicatorSettings  m_ind;
    core::BarSeries    m_series;

    // Flat arrays for ImPlot
    // m_xs     = actual UNIX timestamps (used for labels, VWAP, session detect)
    // m_idxs   = sequential integers 0,1,2,... used as the X axis (no-gap plot)
    std::vector<double> m_xs;
    std::vector<double> m_idxs;
    std::vector<double> m_opens;
    std::vector<double> m_highs;
    std::vector<double> m_lows;
    std::vector<double> m_closes;
    std::vector<double> m_volumes;

    // Computed indicators
    std::vector<double> m_sma1;
    std::vector<double> m_sma2;
    std::vector<double> m_ema;
    std::vector<double> m_bbMid;
    std::vector<double> m_bbUpper;
    std::vector<double> m_bbLower;
    std::vector<double> m_rsi;
    std::vector<double> m_vwap;

    int   m_hoverIdx          = -1;
    float m_chartHeightRatio  = 0.60f;
    float m_volumeHeightRatio = 0.20f;

    // ---- Extend-history state -----------------------------------------------
    bool  m_loadingMore    = false;   // extend request in flight
    bool  m_historyAtStart = false;   // no more older data available

    // ---- Pending order lines and position -----------------------------------
    std::vector<PendingOrderLine> m_pendingOrders;
    PositionInfo                  m_position;

    // ---- Drawing tool state -------------------------------------------------
    DrawTool             m_drawTool    = DrawTool::Cursor;
    bool                 m_drawPending = false;   // first point placed, awaiting second
    double               m_drawPt1X   = 0.0;
    double               m_drawPt1Y   = 0.0;
    std::vector<Drawing> m_drawings;

    // ---- Trade panel state --------------------------------------------------
    int         m_orderTypeIdx = 0;       // index into kOrderTypes[]
    int         m_sessionIdx   = 0;       // 0=Regular,1=Pre-Market,2=After Hours,3=Overnight
    int         m_tifIdx       = 0;       // 0=DAY,1=GTC,2=GTD
    int         m_orderQty     = 100;
    double      m_trailAmount  = 0.50;    // for TRAIL* order types
    double      m_limitOffset  = 0.10;    // stop-limit: gap between stop and limit
    bool        m_limitArmed   = false;   // waiting for chart click to set price
    std::string m_limitSide;              // "BUY" or "SELL"
    bool        m_ctrlClickPopup = false; // ctrl+click confirmation popup open
    double      m_pendingPrice   = 0.0;   // price from chart click, pending confirm

    // ---- Placed order line (drag-and-send mode) ------------------------------
    bool        m_limitPlaced    = false;  // line dropped on chart, awaiting send
    double      m_placedPrice    = 0.0;   // current price of the placed/dragged line
    bool        m_placedDragging = false; // user is currently dragging the placed line

    // ---- Drag-to-modify existing pending order --------------------------------
    int         m_dragPendingIdx    = -1;    // index in m_pendingOrders being dragged
    double      m_dragPendingPrice  = 0.0;  // live price shown during drag
    bool        m_dragPendingActive = false; // drag is in progress
    bool        m_dragPendingIsAux  = false; // true = dragging the limit (aux) leg

    // ---- Dual-price placement (STP LMT: click stop, then click limit) --------
    bool        m_firstPricePlaced  = false; // stop price has been clicked, limit line active
    double      m_firstPrice        = 0.0;  // the placed stop/trigger price

    // ---- Symbol history -----------------------------------------------------
    static constexpr int kMaxHistory = 10;
    std::deque<std::string> m_symbolHistory;

    // ---- Private helpers ----------------------------------------------------
    // Creates today's partial bar if it doesn't exist yet (D1/W1/MN only).
    void EnsureTodayBar(double price);

    void RequestNewData();
    void RefreshData();
    void RebuildFlatArrays();
    void ComputeIndicators();
    void InitViewRange();
    void DrawSessionBands();

    void AddToHistory(const std::string& symbol);
    void LoadHistory();
    void SaveHistory() const;

    // UI sections
    void DrawToolbar();
    void DrawAnalysisToolbar();
    void DrawTradePanel();
    void DrawInfoBar();
    void DrawPositionStrip();
    void DrawCandleChart();
    void DrawVolumeChart();
    void DrawRsiChart();
    void DrawCandlesticks(double halfBarWidth);
    void DrawHoverTooltip();

    // Called inside BeginPlot/EndPlot — renders drawings + limit line, handles clicks
    void DrawOverlays(double step);

    // Helpers
    static void DrawDashedHLine(ImDrawList* dl,
                                float x0, float x1, float y,
                                unsigned int color, float thickness,
                                float dashLen = 6.f, float gapLen = 4.f);
    bool IsNearDrawing(const Drawing& d, double mx, double my,
                       double yTol, double xTol) const;

    static int  VolTickFormatter(double value, char* buf, int size, void* user_data);
    static int  XTickFormatter(double idx,   char* buf, int size, void* user_data);

    static std::vector<double> CalcSMA(const std::vector<double>& close, int period);
    static std::vector<double> CalcEMA(const std::vector<double>& close, int period);
    static void CalcBollingerBands(const std::vector<double>& close, int period, float sigma,
                                   std::vector<double>& mid, std::vector<double>& upper,
                                   std::vector<double>& lower);
    static std::vector<double> CalcRSI(const std::vector<double>& close, int period);
    static std::vector<double> CalcVWAP(const std::vector<double>& high,
                                        const std::vector<double>& low,
                                        const std::vector<double>& close,
                                        const std::vector<double>& volume,
                                        const std::vector<double>& timestamps,
                                        bool intradayReset);

    static core::BarSeries GenerateSimulatedBars(const std::string& symbol,
                                                  core::Timeframe tf, int count);
};

}  // namespace ui
