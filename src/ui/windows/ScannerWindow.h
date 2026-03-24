#pragma once

#include "core/models/ScannerData.h"
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <functional>

namespace ui {

// ============================================================================
// ScannerWindow
//
// Layout:
//
//  ┌──────────────────────────────────────────────────────────────────────────┐
//  │  [Stocks][Indexes][ETFs][Futures]   Preset▼  [Scan]  Filters  Cols▼    │
//  ├──────────────────────────────────────────────────────────────────────────┤
//  │  Filter bar: Price [$___-$___]  %Chg [___-___]  Vol [___]  Sector [___]│
//  ├──────────────────────────────────────────────────────────────────────────┤
//  │  Symbol │ Company │ Price │ Chg │ Chg% │ Vol │ RelVol │ MCap │ RSI …  │
//  │  (sortable table, color-coded rows, row → click selects symbol)         │
//  │  …                                                                       │
//  ├──────────────────────────────────────────────────────────────────────────┤
//  │  [Sparkline detail panel for selected row]   Status: 42 results 09:32   │
//  └──────────────────────────────────────────────────────────────────────────┘
//
// IB Gateway integration stubs:
//   reqScannerSubscription()   → IB scanner API, fills results via OnScanData()
//   void OnScanData(int reqId, const std::vector<ScanResult>&)
//   reqScannerParameters()     → XML of available scan params
// ============================================================================

class ScannerWindow {
public:
    ScannerWindow();
    ~ScannerWindow() = default;

    // Call once per frame. Returns false when window is closed.
    bool Render();
    bool& open() { return m_open; }
    void setGroupId(int id)    { m_groupId = id; }
    int  groupId() const       { return m_groupId; }
    void setInstanceId(int id);
    int  instanceId() const    { return m_instanceId; }

    // --- IB Gateway callbacks (future integration) ---
    void OnScanData(int reqId, const std::vector<core::ScanResult>& results);
    void OnQuoteUpdate(const std::string& symbol, double price,
                       double change, double changePct, double volume);

    // Notify scanner of portfolio symbols for watchlist highlighting
    void SetPortfolioSymbols(const std::vector<std::string>& symbols);

    // Called when user double-clicks a row — host wires this to ChartWindow
    std::function<void(const std::string& symbol)> OnSymbolSelected;

    // IB 52-week high/low tick (field 79/80 from generic tick list 165)
    void Set52WHigh(const std::string& symbol, double high52);
    void Set52WLow (const std::string& symbol, double low52);

    // IB average volume tick (field 87 from generic tick list 221)
    void SetAvgVolume(const std::string& symbol, double avgVol);

    // IB previous-close tick (field 9 / 75→9 delayed) — recomputes change/% immediately
    void SetPrevClose(const std::string& symbol, double prevClose);

    // Called by main once IB is connected — triggers a real IB scanner request
    std::function<void(const std::string& scanCode,
                       const std::string& instrument,
                       const std::string& location)> OnScanRequest;

private:
    // ---- Window state -------------------------------------------------------
    bool m_open       = true;
    int  m_groupId    = 0;
    int  m_instanceId = 1;
    char m_title[40]  = "Market Scanner 1##scanner1";
    bool m_hasRealData = false;

    // ---- Asset class tabs ---------------------------------------------------
    core::AssetClass m_activeClass = core::AssetClass::Stocks;

    // ---- Preset / filter ----------------------------------------------------
    int              m_presetIdx   = 0;   // index into kPresets[]
    core::ScanFilter m_filter;
    bool             m_showFilters = false;

    // filter UI buffers
    char m_minPriceBuf[16] = "0";
    char m_maxPriceBuf[16] = "99999";
    char m_minChgBuf[16]   = "-100";
    char m_maxChgBuf[16]   = "100";
    char m_minVolBuf[16]   = "0";
    char m_sectorBuf[32]   = "";
    char m_searchBuf[32]   = "";        // symbol/company search

    // ---- Column visibility --------------------------------------------------
    bool m_showCompany   = true;
    bool m_showChange    = true;
    bool m_showChangePct = true;
    bool m_showVolume    = true;
    bool m_showRelVol    = true;
    bool m_showMktCap    = true;
    bool m_showPE        = false;
    bool m_showHigh52    = false;
    bool m_showLow52     = false;
    bool m_showPctH52    = true;
    bool m_showRSI       = true;
    bool m_showMACD      = false;
    bool m_showATR       = false;
    bool m_showSparkline = true;

    // ---- Results ------------------------------------------------------------
    std::vector<core::ScanResult> m_results;
    int   m_selectedRow = -1;
    bool  m_scanning    = false;        // animating scan in progress

    // ---- Sort state ---------------------------------------------------------
    core::ScanColumn m_sortCol       = core::ScanColumn::ChangePct;
    bool             m_sortAscending = false;

    // ---- Portfolio symbols (for row highlighting) ---------------------------
    std::vector<std::string> m_portfolioSymbols;

    // ---- Scan timing --------------------------------------------------------
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastScanTime;
    Clock::time_point m_lastQuoteUpdate;
    float m_autoRefreshSec   = 30.0f;
    bool  m_autoRefresh      = true;
    char  m_lastScanTimeStr[32] = "--:--:--";

    // ---- RNG ----------------------------------------------------------------
    std::mt19937                           m_rng;
    std::uniform_real_distribution<double> m_uniform{0.0, 1.0};
    std::normal_distribution<double>       m_noise{0.0, 0.005};

    // ---- Sub-renderers ------------------------------------------------------
    void DrawToolbar();
    void DrawFilterBar();
    void DrawResultsTable();
    void DrawDetailPanel();
    void DrawStatusBar();
    void DrawColumnChooserPopup();

    // ---- Scan logic ---------------------------------------------------------
    void RunScan();
    void SortResults();
    void UpdateQuotes(float dtSec);    // simulate live quote drift
    bool MatchesFilter(const core::ScanResult& r) const;
    bool MatchesSearch(const core::ScanResult& r) const;

    // ---- Simulation helpers -------------------------------------------------
    void SimulateStocks();
    void SimulateIndexes();
    void SimulateETFs();
    void SimulateFutures();
    std::vector<float> GenerateSparkline(double startPrice, double volatility,
                                         bool trending, int points = 20);

    // ---- Formatting helpers -------------------------------------------------
    static std::string FmtPrice(double p);
    static std::string FmtChange(double c, bool withSign = true);
    static std::string FmtVolume(double v);
    static std::string FmtMktCap(double mM);    // millions → "1.2B", "345M"
    static std::string FmtPct(double p, bool withSign = true);

    // ---- Row colour helpers -------------------------------------------------
    static void PushRowColor(double changePct, bool isPortfolio);
    static void PopRowColor();
};

}  // namespace ui
