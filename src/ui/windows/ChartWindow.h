#pragma once

#include "core/models/MarketData.h"
#include <string>
#include <vector>

namespace ui {

// ============================================================================
// ChartWindow
// Renders a full candlestick chart with overlay indicators (SMA, EMA, Bollinger
// Bands) and sub-charts for Volume and RSI.
// ============================================================================
class ChartWindow {
public:
    ChartWindow();

    // Call once per frame inside an ImGui context.
    // Returns false if the window was closed by the user.
    bool Render();

    // Change the symbol being charted (e.g., from scanner row click).
    void SetSymbol(const std::string& symbol);

private:
    // ---- Settings -----------------------------------------------------------
    struct IndicatorSettings {
        bool sma20   = true;
        bool sma50   = true;
        bool ema20   = false;
        bool bbands  = true;   // Bollinger Bands
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
    char              m_symbol[16]      = "AAPL";
    core::Timeframe   m_timeframe       = core::Timeframe::D1;
    bool              m_needsRefresh    = true;
    bool              m_open            = true;

    IndicatorSettings m_ind;
    core::BarSeries   m_series;

    // Flat arrays for ImPlot (rebuilt whenever m_series changes)
    std::vector<double> m_xs;       // timestamps
    std::vector<double> m_opens;
    std::vector<double> m_highs;
    std::vector<double> m_lows;
    std::vector<double> m_closes;
    std::vector<double> m_volumes;

    // Computed indicators
    std::vector<double> m_sma1;     // smaPeriod1
    std::vector<double> m_sma2;     // smaPeriod2
    std::vector<double> m_ema;
    std::vector<double> m_bbMid;
    std::vector<double> m_bbUpper;
    std::vector<double> m_bbLower;
    std::vector<double> m_rsi;

    int   m_hoverIdx     = -1;      // candle under mouse (-1 = none)
    float m_chartHeightRatio = 0.60f;
    float m_volumeHeightRatio = 0.20f;

    // ---- Private helpers ----------------------------------------------------
    void RefreshData();                 // load/generate bar data
    void RebuildFlatArrays();           // copy series into flat vectors
    void ComputeIndicators();

    void DrawToolbar();
    void DrawCandleChart();             // main OHLCV + overlay panel
    void DrawVolumeChart();
    void DrawRsiChart();
    void DrawCandlesticks(double halfBarWidth);
    void DrawHoverTooltip();

    // Indicator math
    static std::vector<double> CalcSMA(const std::vector<double>& close, int period);
    static std::vector<double> CalcEMA(const std::vector<double>& close, int period);
    static void CalcBollingerBands(const std::vector<double>& close, int period, float sigma,
                                   std::vector<double>& mid, std::vector<double>& upper,
                                   std::vector<double>& lower);
    static std::vector<double> CalcRSI(const std::vector<double>& close, int period);

    // Simulated data generator (replaced by IB Gateway feed in later tasks)
    static core::BarSeries GenerateSimulatedBars(const std::string& symbol,
                                                  core::Timeframe tf,
                                                  int count);
};

}  // namespace ui
