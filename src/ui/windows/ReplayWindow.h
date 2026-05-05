#pragma once

#include "core/models/MarketData.h"
#include "core/models/OrderData.h"
#include "core/models/ReplayData.h"
#include "core/services/ReplayEngine.h"
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ui {

// ============================================================================
// ReplayWindow — Pre / Intra / Post-Market playback with paper-trading.
// See .claude/plans/replay.md.
// ============================================================================
class ReplayWindow {
public:
    // Pushed by main.cpp once per frame (mirrors ChartWindow::UnguardedHint).
    struct UnguardedHint {
        bool        active   = false;
        std::string symbol;
        double      qty      = 0.0;
        double      avgCost  = 0.0;
        double      stopTrig = 0.0;
        double      stopLmt  = 0.0;
        double      pctRisk  = 0.0;
    };

    enum class Mode { Analysis = 0, Operate = 1 };

    // Indicator settings — same shape as ChartWindow's IndicatorSettings, but
    // ReplayWindow defaults are tuned for short-TF replay sessions (BB/VWAP on,
    // SMA50 off, RSI off — flip in toolbar).
    struct IndicatorSettings {
        bool sma20      = true;
        bool sma50      = false;
        bool ema20      = false;
        bool bbands     = true;
        bool vwap       = true;
        bool vwapBands  = true;
        bool volume     = true;
        bool rsi        = false;

        int    smaPeriod1 = 20;
        int    smaPeriod2 = 50;
        int    emaPeriod  = 20;
        int    bbPeriod   = 20;
        double bbSigma    = 2.0;
        int    rsiPeriod  = 14;
    };
    void setIndicatorSettings(const IndicatorSettings& s);
    const IndicatorSettings& getIndicatorSettings() const { return m_ind; }

    ReplayWindow();

    bool Render();
    bool& open() { return m_open; }

    void SetDay(const core::HistoricalDay& day);
    void setGroupId(int id) { m_groupId = id; }
    int  groupId() const    { return m_groupId; }
    void setInstanceId(int id);
    int  instanceId() const { return m_instanceId; }

    std::string getSymbol() const { return m_symbol; }
    void SetSymbol(const std::string& sym);

    // Seek cursor to nearest bar matching targetTime (§6.3 group sync).
    void SeekToTime(std::time_t t);

    // Unguarded-position hint (same pattern as ChartWindow).
    void SetUnguardedSuggestion(const UnguardedHint& h);

    // Fired when the user changes the symbol or date range (host fetches the
    // historical range from IB). dateFrom == dateTo for the single-day case.
    std::function<void(const std::string& sym,
                       const std::string& dateFrom,
                       const std::string& dateTo,
                       core::services::ReplaySession session,
                       core::Timeframe tf)> OnDataRequest;

    // Fired when a paper order is submitted in Operate mode.
    // Returns the assigned localId (or -1 on rejection).
    std::function<int(const core::Order&)> OnPaperOrderSubmit;

    // Working orders (for main.cpp inspection during per-frame engine tick).
    core::services::SimulatedOrderBook& book() { return m_book; }
    core::services::SimulatedAccount&   account() { return m_account; }
    int  nextLocalId() const { return m_nextLocalId; }
    void incNextLocalId() { ++m_nextLocalId; }

    // Getters for persistence
    const char* getDateFromBuf() const { return m_dateFromBuf; }
    const char* getDateToBuf()   const { return m_dateToBuf; }
    core::services::ReplaySession getSession() const { return m_session; }
    core::Timeframe   getTimeframe() const { return m_tf; }
    double            getSpeed()      const { return m_clock.speed; }
    Mode              getMode()       const { return m_mode; }
    int               getCursorBarIdx() const { return m_clock.cursorBarIdx; }
    double            getStartingCash() const { return m_startingCash; }
    bool              getTickFills()   const { return m_tickFills; }

    // Setters for restore
    void setDateFrom(const char* d) {
        std::strncpy(m_dateFromBuf, d, sizeof(m_dateFromBuf)-1);
        m_dateFromBuf[sizeof(m_dateFromBuf)-1] = '\0';
    }
    void setDateTo(const char* d) {
        std::strncpy(m_dateToBuf, d, sizeof(m_dateToBuf)-1);
        m_dateToBuf[sizeof(m_dateToBuf)-1] = '\0';
    }
    // Back-compat: legacy DATE: line in cfg sets both endpoints.
    void setDate(const char* d) { setDateFrom(d); setDateTo(d); }
    void setSession(core::services::ReplaySession s) { m_session = s; }
    void setTimeframe(core::Timeframe tf)            { m_tf = tf; }
    void setSpeed(double s)                          { m_clock.speed = s; }
    void setMode(Mode m)                             { m_mode = m; }
    void setCursorBarIdx(int i)                      { m_clock.cursorBarIdx = i; }
    void setStartingCash(double c)                   { m_startingCash = c;
                                                       core::services::Reset(m_account, c); }
    void setTickFills(bool v)                        { m_tickFills = v; }

    // Fired when a paper order is cancelled.
    std::function<void(int localId)> OnPaperOrderCancel;

    // Fired when the cursor bar changes (auto-tick or manual scrub).
    // Carries the new bar's timestamp for group-time-sync (§6.3).
    std::function<void(std::time_t cursorTime)> OnCursorMove;

    // Mode constants for display
    static constexpr const char* kModeLabels[] = {"Analysis", "Operate"};

private:
    // ---- State ----------------------------------------------------------
    int  m_groupId    = 0;
    int  m_instanceId = 1;
    char m_title[48]  = "Replay AAPL##replay0";
    char m_symbol[32] = "AAPL";
    bool m_open       = true;
    bool m_loading    = false;
    bool m_hasData    = false;

    // Replay config
    core::Timeframe                   m_tf        = core::Timeframe::M1;
    core::services::ReplaySession      m_session   = core::services::ReplaySession::Intraday;
    core::services::ReplayClock        m_clock;
    Mode                              m_mode      = Mode::Analysis;
    // Date range. m_dateFromBuf == m_dateToBuf is the single-day case.
    char                              m_dateFromBuf[16] = "2026-04-15";
    char                              m_dateToBuf[16]   = "2026-04-15";
    int                               m_calNavYearFrom  = 0;
    int                               m_calNavMonthFrom = 0;
    int                               m_calNavYearTo    = 0;
    int                               m_calNavMonthTo   = 0;
    double                            m_startingCash  = 100000.0;
    bool                              m_tickFills     = false;

    // Flat arrays for ImPlot
    std::vector<double> m_idxs;
    std::vector<double> m_xs;       // UNIX timestamps
    std::vector<double> m_opens;
    std::vector<double> m_highs;
    std::vector<double> m_lows;
    std::vector<double> m_closes;
    std::vector<double> m_volumes;

    // Indicator state — mirrors ChartWindow exactly. Computed once per data load
    // (and on toggle / period change) — not per-frame.
    IndicatorSettings   m_ind;
    std::vector<double> m_sma1, m_sma2, m_ema;
    std::vector<double> m_bbMid, m_bbUpper, m_bbLower;
    std::vector<double> m_rsi;
    std::vector<double> m_vwap, m_vwapSd1Up, m_vwapSd1Dn, m_vwapSd2Up, m_vwapSd2Dn;
    bool                m_indSettingsOpen = false;

    // Simulated engine state (Operate mode)
    core::services::SimulatedOrderBook m_book;
    core::services::SimulatedAccount   m_account;
    int                               m_nextLocalId      = 1;
    int                               m_lastProcessedIdx  = -1;
    int                               m_lastFiredCursorIdx = -1;

    // Real fills overlaid on chart
    std::vector<core::Fill> m_userFills;

    // Sim fills accumulated
    std::vector<core::services::SimulatedFill> m_simFills;

    // Unguarded hint
    UnguardedHint m_unguarded;
    std::unordered_set<std::string> m_dismissedUnguarded;
    double m_lastWarnedQty = 0.0;

    // ImPlot view state
    bool   m_viewInitialized = false;
    double m_xMin = 0.0, m_xMax = 0.0;
    double m_priceMin = 0.0, m_priceMax = 0.0;

    // ---- Trade panel state (sandbox trading, mirrors ChartWindow) -------
    int  m_orderQty       = 100;
    int  m_orderTypeIdx   = 0;       // index into kOrderTypes[] in cpp
    int  m_tifIdx         = 0;       // 0=DAY, 1=GTC
    bool m_transmitInstantly = true;

    // Per-type extras
    bool   m_trailByPct      = false;
    double m_trailAmount     = 0.10;
    double m_trailPercent    = 0.5;
    double m_limitOffset     = 0.10;
    double m_trailStopPrice  = 0.0;
    double m_pegOffset       = 0.05;

    // Armed chart-click state
    bool        m_limitArmed       = false;
    std::string m_limitSide;
    bool        m_firstPricePlaced = false;
    double      m_firstPrice       = 0.0;
    double      m_liveCursorPrice  = 0.0;

    // Confirmation popup
    core::Order m_pendingConfirmOrder;
    bool        m_showConfirmPopup = false;

    // ---- Rendering methods ----------------------------------------------
    void DrawToolbar();
    void DrawScrubber();
    void DrawChart();
    void DrawCandlesticks();
    void DrawCursorLine();
    void DrawStatusBar();
    void DrawTradePanel();
    void DrawPositionStrip();
    void DrawOrderImpactBadge();
    void DrawWorkingOrders();
    void DrawArmedLineAndHandleClick();
    void DrawConfirmPopup();
    void DrawBottomTabs();
    void DrawUnguardedStrip();
    void DrawModeBadge();
    void DrawFillMarkers();

    // ---- Indicator helpers ----------------------------------------------
    void ComputeIndicators();
    void DrawVolumeChart();
    void DrawRsiChart();
    void DrawIndicatorSettingsPopup();
    static int VolTickFormatter(double value, char* buf, int size, void* user_data);

    // ---- Helpers ---------------------------------------------------------
    void RebuildFlatArrays();
    void ApplyEngineTick();
    static int XTickFormatter(double idx, char* buf, int size, void* userData);
};

}  // namespace ui
