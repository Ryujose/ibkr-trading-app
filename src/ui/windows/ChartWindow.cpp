#include "ui/windows/ChartWindow.h"

#include "imgui.h"
#include "implot.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <numeric>
#include <random>
#include <sstream>
#include <iomanip>

namespace ui {

// ============================================================================
// Construction
// ============================================================================
ChartWindow::ChartWindow() {
    RefreshData();
}

// ============================================================================
// SetSymbol — called externally (e.g., from scanner row selection)
// ============================================================================
void ChartWindow::SetSymbol(const std::string& symbol) {
    if (symbol.size() < sizeof(m_symbol)) {
        std::memcpy(m_symbol, symbol.c_str(), symbol.size() + 1);
        m_hasRealData  = false;   // clear real data; RefreshData will regenerate simulated
        m_needsRefresh = true;
    }
}

void ChartWindow::AddBar(const core::Bar& bar, bool done) {
    if (!m_hasRealData) {
        // First real bar arriving: discard any simulated data
        m_series.bars.clear();
        m_series.symbol    = m_symbol;
        m_series.timeframe = m_timeframe;
        m_hasRealData      = true;
        m_needsRefresh     = false;
    }
    if (!done) {
        m_series.bars.push_back(bar);
    } else {
        // Historical data stream complete — rebuild display arrays
        RebuildFlatArrays();
        ComputeIndicators();
    }
}

void ChartWindow::SetHistoricalData(const core::BarSeries& series) {
    m_series       = series;
    m_hasRealData  = true;
    m_needsRefresh = false;
    RebuildFlatArrays();
    ComputeIndicators();
}

// ============================================================================
// Public render entry point
// ============================================================================
bool ChartWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("Chart", &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    DrawToolbar();
    ImGui::Separator();

    if (m_needsRefresh) {
        RefreshData();
        m_needsRefresh = false;
    }

    if (m_series.empty()) {
        ImGui::TextDisabled("No data available.");
        ImGui::End();
        return m_open;
    }

    DrawCandleChart();

    if (m_ind.volume) DrawVolumeChart();
    if (m_ind.rsi)    DrawRsiChart();

    ImGui::End();
    return m_open;
}

// ============================================================================
// Toolbar
// ============================================================================
void ChartWindow::DrawToolbar() {
    // Symbol input
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputText("##sym", m_symbol, sizeof(m_symbol),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        m_needsRefresh = true;

    ImGui::SameLine();

    // Quick symbol buttons
    const char* syms[] = {"AAPL","MSFT","GOOGL","TSLA","SPY"};
    for (const char* s : syms) {
        ImGui::SameLine();
        bool active = (std::strcmp(m_symbol, s) == 0);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
        if (ImGui::SmallButton(s)) {
            std::strncpy(m_symbol, s, sizeof(m_symbol) - 1);
            m_needsRefresh = true;
        }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::SameLine(0, 16);

    // Timeframe buttons
    const core::Timeframe tfs[] = {
        core::Timeframe::M5, core::Timeframe::M15, core::Timeframe::M30,
        core::Timeframe::H1, core::Timeframe::H4,  core::Timeframe::D1,
    };
    for (core::Timeframe tf : tfs) {
        ImGui::SameLine();
        bool active = (m_timeframe == tf);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.9f, 1.0f));
        if (ImGui::SmallButton(core::TimeframeLabel(tf))) {
            m_timeframe    = tf;
            m_needsRefresh = true;
        }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::SameLine(0, 16);

    // Indicator toggles
    ImGui::Checkbox("SMA20",  &m_ind.sma20);   ImGui::SameLine();
    ImGui::Checkbox("SMA50",  &m_ind.sma50);   ImGui::SameLine();
    ImGui::Checkbox("EMA20",  &m_ind.ema20);   ImGui::SameLine();
    ImGui::Checkbox("BB",     &m_ind.bbands);  ImGui::SameLine();
    ImGui::Checkbox("Vol",    &m_ind.volume);  ImGui::SameLine();
    ImGui::Checkbox("RSI",    &m_ind.rsi);
}

// ============================================================================
// Candlestick + overlay chart
// ============================================================================
void ChartWindow::DrawCandleChart() {
    int n = (int)m_xs.size();
    if (n == 0) return;

    // How many bars to display by default (show last 100)
    int displayCount = std::min(n, 100);
    double xMin = m_xs[n - displayCount];
    double xMax = m_xs[n - 1] + (m_xs[1] - m_xs[0]);

    // Price range for displayed bars
    double priceMin =  1e18, priceMax = -1e18;
    for (int i = n - displayCount; i < n; i++) {
        priceMin = std::min(priceMin, m_lows[i]);
        priceMax = std::max(priceMax, m_highs[i]);
    }
    double priceMargin = (priceMax - priceMin) * 0.08;

    float available = ImGui::GetContentRegionAvail().y;
    float volumeH   = m_ind.volume ? available * m_volumeHeightRatio : 0.0f;
    float rsiH      = m_ind.rsi    ? available * 0.20f               : 0.0f;
    float spacing   = ImGui::GetStyle().ItemSpacing.y;
    float chartH    = available - volumeH - rsiH
                      - (m_ind.volume ? spacing : 0.0f)
                      - (m_ind.rsi    ? spacing : 0.0f);

    ImPlotFlags plotFlags = ImPlotFlags_NoMouseText;
    if (!ImPlot::BeginPlot("##candles", ImVec2(-1, chartH), plotFlags))
        return;

    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    ImPlot::SetupAxes(nullptr, "Price ($)", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImGuiCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, priceMin - priceMargin, priceMax + priceMargin, ImGuiCond_Always);
    ImPlot::SetupFinish();

    double halfBarW = (n > 1 ? (m_xs[1] - m_xs[0]) * 0.4 : 3600.0 * 0.4);

    // ---- Bollinger Bands (shaded + lines) ----
    if (m_ind.bbands && (int)m_bbUpper.size() == n) {
        ImPlot::SetNextFillStyle(ImVec4(0.5f, 0.5f, 1.0f, 0.12f));
        ImPlot::PlotShaded("##bb_fill", m_xs.data(), m_bbLower.data(), m_bbUpper.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.0f, 0.7f), 1.0f);
        ImPlot::PlotLine("BB Upper", m_xs.data(), m_bbUpper.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.0f, 0.5f), 1.0f);
        ImPlot::PlotLine("BB Mid",   m_xs.data(), m_bbMid.data(),   n);
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.0f, 0.7f), 1.0f);
        ImPlot::PlotLine("BB Lower", m_xs.data(), m_bbLower.data(), n);
    }

    // ---- SMA20 ----
    if (m_ind.sma20 && (int)m_sma1.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), 1.5f);
        ImPlot::PlotLine("SMA20", m_xs.data(), m_sma1.data(), n);
    }

    // ---- SMA50 ----
    if (m_ind.sma50 && (int)m_sma2.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), 1.5f);
        ImPlot::PlotLine("SMA50", m_xs.data(), m_sma2.data(), n);
    }

    // ---- EMA20 ----
    if (m_ind.ema20 && (int)m_ema.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), 1.5f);
        ImPlot::PlotLine("EMA20", m_xs.data(), m_ema.data(), n);
    }

    // ---- Candlesticks (custom drawing) ----
    DrawCandlesticks(halfBarW);

    // ---- Hover tooltip ----
    DrawHoverTooltip();

    ImPlot::EndPlot();
}

// ============================================================================
// Custom candlestick renderer (public ImPlot API only)
// ============================================================================
void ChartWindow::DrawCandlesticks(double halfBarWidth) {
    int n = (int)m_xs.size();
    if (n == 0) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    // Detect hovered candle
    m_hoverIdx = -1;
    if (ImPlot::IsPlotHovered()) {
        ImPlotPoint mp = ImPlot::GetPlotMousePos();
        for (int i = 0; i < n; i++) {
            if (std::abs(m_xs[i] - mp.x) < halfBarWidth * 2.0) {
                m_hoverIdx = i;
                break;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        bool bull    = m_closes[i] >= m_opens[i];
        double bodyH = std::max(m_opens[i], m_closes[i]);
        double bodyL = std::min(m_opens[i], m_closes[i]);

        ImVec2 topL  = ImPlot::PlotToPixels(m_xs[i] - halfBarWidth, bodyH);
        ImVec2 botR  = ImPlot::PlotToPixels(m_xs[i] + halfBarWidth, bodyL);
        ImVec2 wHigh = ImPlot::PlotToPixels(m_xs[i], m_highs[i]);
        ImVec2 wLow  = ImPlot::PlotToPixels(m_xs[i], m_lows[i]);
        float  midX  = (topL.x + botR.x) * 0.5f;

        ImU32 col     = bull ? IM_COL32(52, 211, 100, 255)  : IM_COL32(220, 60,  60, 255);
        ImU32 colDim  = bull ? IM_COL32(30, 140,  60, 255)  : IM_COL32(160, 30,  30, 255);
        ImU32 colHov  = bull ? IM_COL32(100, 255, 150, 255) : IM_COL32(255, 110, 110, 255);

        bool hovered  = (i == m_hoverIdx);
        ImU32 fillCol = hovered ? colHov : col;
        ImU32 wickCol = hovered ? colHov : colDim;

        // Wick
        dl->AddLine(ImVec2(midX, wHigh.y), ImVec2(midX, wLow.y), wickCol, 1.0f);

        // Body
        float bh = std::abs(botR.y - topL.y);
        if (bh < 1.5f) {
            // Doji — single horizontal line
            dl->AddLine(ImVec2(topL.x, topL.y), ImVec2(botR.x, topL.y), fillCol, 1.5f);
        } else {
            dl->AddRectFilled(topL, botR, fillCol);
            dl->AddRect(topL, botR, wickCol, 0.0f, 0, 0.5f);
        }
    }

    ImPlot::PopPlotClipRect();
}

// ============================================================================
// Hover tooltip
// ============================================================================
void ChartWindow::DrawHoverTooltip() {
    if (m_hoverIdx < 0) return;
    int i = m_hoverIdx;

    // Format the timestamp as a date string
    std::time_t t = (std::time_t)m_xs[i];
    std::tm* tm   = std::gmtime(&t);
    char dateBuf[32];
    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tm);

    double change    = m_closes[i] - m_opens[i];
    double changePct = (m_opens[i] != 0.0) ? (change / m_opens[i] * 100.0) : 0.0;
    bool   bull      = change >= 0;
    ImVec4 col       = bull ? ImVec4(0.2f, 0.9f, 0.4f, 1.0f)
                            : ImVec4(0.9f, 0.3f, 0.3f, 1.0f);

    ImGui::BeginTooltip();
    ImGui::Text("%s  %s", m_symbol, dateBuf);
    ImGui::Separator();
    ImGui::Text("Open:   $%.2f", m_opens[i]);
    ImGui::Text("High:   $%.2f", m_highs[i]);
    ImGui::Text("Low:    $%.2f", m_lows[i]);
    ImGui::Text("Close:  $%.2f", m_closes[i]);
    ImGui::TextColored(col, "Change: %+.2f  (%+.2f%%)", change, changePct);
    ImGui::Text("Volume: %.0f", m_volumes[i]);

    // Indicator values at this bar
    if (m_ind.sma20 && i < (int)m_sma1.size() && m_sma1[i] > 0.0)
        ImGui::TextDisabled("SMA20:  $%.2f", m_sma1[i]);
    if (m_ind.sma50 && i < (int)m_sma2.size() && m_sma2[i] > 0.0)
        ImGui::TextDisabled("SMA50:  $%.2f", m_sma2[i]);
    if (m_ind.ema20 && i < (int)m_ema.size() && m_ema[i] > 0.0)
        ImGui::TextDisabled("EMA20:  $%.2f", m_ema[i]);
    if (m_ind.rsi   && i < (int)m_rsi.size()  && m_rsi[i] > 0.0)
        ImGui::TextDisabled("RSI14:  %.1f",   m_rsi[i]);

    ImGui::EndTooltip();
}

// ============================================================================
// Volume sub-chart
// ============================================================================
void ChartWindow::DrawVolumeChart() {
    int n = (int)m_xs.size();
    if (n == 0) return;

    float available = ImGui::GetContentRegionAvail().y;
    float rsiH      = m_ind.rsi ? available * (0.20f / (1.0f - m_volumeHeightRatio)) : 0.0f;
    float volH      = available - rsiH - (m_ind.rsi ? ImGui::GetStyle().ItemSpacing.y : 0.0f);

    if (!ImPlot::BeginPlot("##volume", ImVec2(-1, volH),
                           ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
        return;

    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    ImPlot::SetupAxes(nullptr, "Volume", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);

    int displayCount = std::min(n, 100);
    double xMin = m_xs[n - displayCount];
    double xMax = m_xs[n - 1] + (m_xs[1] - m_xs[0]);
    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImGuiCond_Always);
    ImPlot::SetupFinish();

    double barW = (n > 1 ? (m_xs[1] - m_xs[0]) * 0.7 : 3600.0 * 0.7);

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    for (int i = 0; i < n; i++) {
        bool bull = m_closes[i] >= m_opens[i];
        ImU32 col = bull ? IM_COL32(52, 211, 100, 160) : IM_COL32(220, 60, 60, 160);
        ImVec2 top = ImPlot::PlotToPixels(m_xs[i] - barW * 0.5, m_volumes[i]);
        ImVec2 bot = ImPlot::PlotToPixels(m_xs[i] + barW * 0.5, 0.0);
        if (top.y < bot.y) dl->AddRectFilled(top, bot, col);
    }
    ImPlot::PopPlotClipRect();

    ImPlot::EndPlot();
}

// ============================================================================
// RSI sub-chart
// ============================================================================
void ChartWindow::DrawRsiChart() {
    int n = (int)m_xs.size();
    if (n == 0 || (int)m_rsi.size() != n) return;

    if (!ImPlot::BeginPlot("##rsi", ImVec2(-1, -1),
                           ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
        return;

    ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
    ImPlot::SetupAxes(nullptr, "RSI", ImPlotAxisFlags_None, ImPlotAxisFlags_None);

    int displayCount = std::min(n, 100);
    double xMin = m_xs[n - displayCount];
    double xMax = m_xs[n - 1] + (m_xs[1] - m_xs[0]);
    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImGuiCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImGuiCond_Always);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, 100.0);
    ImPlot::SetupFinish();

    // Overbought / oversold reference lines
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    {
        ImVec2 ob0 = ImPlot::PlotToPixels(xMin, 70.0);
        ImVec2 ob1 = ImPlot::PlotToPixels(xMax, 70.0);
        ImVec2 os0 = ImPlot::PlotToPixels(xMin, 30.0);
        ImVec2 os1 = ImPlot::PlotToPixels(xMax, 30.0);
        dl->AddLine(ob0, ob1, IM_COL32(220, 60, 60, 100), 1.0f);
        dl->AddLine(os0, os1, IM_COL32(52, 211, 100, 100), 1.0f);

        // Overbought fill
        ImVec2 top0 = ImPlot::PlotToPixels(xMin, 100.0);
        (void)ImPlot::PlotToPixels(xMax, 100.0);
        dl->AddRectFilled(ImVec2(top0.x, top0.y), ImVec2(ob1.x, ob0.y), IM_COL32(220, 60, 60, 20));

        // Oversold fill
        ImVec2 bot0 = ImPlot::PlotToPixels(xMin, 0.0);
        ImVec2 bot1 = ImPlot::PlotToPixels(xMax, 0.0);
        dl->AddRectFilled(ImVec2(os0.x, os0.y), ImVec2(bot1.x, bot0.y), IM_COL32(52, 211, 100, 20));
    }
    ImPlot::PopPlotClipRect();

    // RSI line coloured by zone
    ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), 1.5f);
    ImPlot::PlotLine("RSI14", m_xs.data(), m_rsi.data(), n);

    // Label latest RSI value
    if (n > 0 && m_rsi[n - 1] > 0.0) {
        double rsiNow = m_rsi[n - 1];
        ImVec4 labelCol = rsiNow > 70.0 ? ImVec4(1, 0.3f, 0.3f, 1)
                        : rsiNow < 30.0 ? ImVec4(0.3f, 1, 0.5f, 1)
                                        : ImVec4(0.8f, 0.8f, 0.8f, 1);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", rsiNow);
        ImPlot::Annotation(m_xs[n - 1], rsiNow, labelCol, ImVec2(4, 0), false, "%s", buf);
    }

    ImPlot::EndPlot();
}

// ============================================================================
// Data management
// ============================================================================
void ChartWindow::RefreshData() {
    if (m_hasRealData) return;  // real data already loaded; don't overwrite
    m_series = GenerateSimulatedBars(m_symbol, m_timeframe, 200);
    RebuildFlatArrays();
    ComputeIndicators();
}

void ChartWindow::RebuildFlatArrays() {
    int n = m_series.size();
    m_xs.resize(n);     m_opens.resize(n);  m_highs.resize(n);
    m_lows.resize(n);   m_closes.resize(n); m_volumes.resize(n);
    for (int i = 0; i < n; i++) {
        const auto& b = m_series.bars[i];
        m_xs[i]      = b.timestamp;
        m_opens[i]   = b.open;
        m_highs[i]   = b.high;
        m_lows[i]    = b.low;
        m_closes[i]  = b.close;
        m_volumes[i] = b.volume;
    }
}

void ChartWindow::ComputeIndicators() {
    m_sma1    = CalcSMA(m_closes, m_ind.smaPeriod1);
    m_sma2    = CalcSMA(m_closes, m_ind.smaPeriod2);
    m_ema     = CalcEMA(m_closes, m_ind.emaPeriod);
    CalcBollingerBands(m_closes, m_ind.bbPeriod, m_ind.bbSigma,
                       m_bbMid, m_bbUpper, m_bbLower);
    m_rsi     = CalcRSI(m_closes, m_ind.rsiPeriod);
}

// ============================================================================
// Indicator math
// ============================================================================
std::vector<double> ChartWindow::CalcSMA(const std::vector<double>& close, int period) {
    int n = (int)close.size();
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n < period) return out;
    double sum = 0.0;
    for (int i = 0; i < period; i++) sum += close[i];
    out[period - 1] = sum / period;
    for (int i = period; i < n; i++) {
        sum += close[i] - close[i - period];
        out[i] = sum / period;
    }
    return out;
}

std::vector<double> ChartWindow::CalcEMA(const std::vector<double>& close, int period) {
    int n = (int)close.size();
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n < period) return out;
    double k = 2.0 / (period + 1.0);
    double ema = 0.0;
    for (int i = 0; i < period; i++) ema += close[i];
    ema /= period;
    out[period - 1] = ema;
    for (int i = period; i < n; i++) {
        ema = close[i] * k + ema * (1.0 - k);
        out[i] = ema;
    }
    return out;
}

void ChartWindow::CalcBollingerBands(const std::vector<double>& close, int period, float sigma,
                                     std::vector<double>& mid,
                                     std::vector<double>& upper,
                                     std::vector<double>& lower) {
    int n = (int)close.size();
    mid.assign(n, 0.0);
    upper.assign(n, 0.0);
    lower.assign(n, 0.0);
    if (period <= 0 || n < period) return;

    for (int i = period - 1; i < n; i++) {
        double sum = 0.0;
        for (int j = i - period + 1; j <= i; j++) sum += close[j];
        double m = sum / period;
        double var = 0.0;
        for (int j = i - period + 1; j <= i; j++) var += (close[j] - m) * (close[j] - m);
        double sd  = std::sqrt(var / period);
        mid[i]     = m;
        upper[i]   = m + sigma * sd;
        lower[i]   = m - sigma * sd;
    }
}

std::vector<double> ChartWindow::CalcRSI(const std::vector<double>& close, int period) {
    int n = (int)close.size();
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n <= period) return out;

    double avgGain = 0.0, avgLoss = 0.0;
    for (int i = 1; i <= period; i++) {
        double diff = close[i] - close[i - 1];
        if (diff > 0) avgGain += diff;
        else          avgLoss -= diff;
    }
    avgGain /= period;
    avgLoss /= period;

    auto rsiFromRL = [](double g, double l) -> double {
        if (l == 0.0) return 100.0;
        double rs = g / l;
        return 100.0 - (100.0 / (1.0 + rs));
    };

    out[period] = rsiFromRL(avgGain, avgLoss);

    for (int i = period + 1; i < n; i++) {
        double diff = close[i] - close[i - 1];
        double gain = diff > 0 ?  diff : 0.0;
        double loss = diff < 0 ? -diff : 0.0;
        avgGain = (avgGain * (period - 1) + gain) / period;
        avgLoss = (avgLoss * (period - 1) + loss) / period;
        out[i]  = rsiFromRL(avgGain, avgLoss);
    }
    return out;
}

// ============================================================================
// Simulated data generator
// Produces realistic OHLCV bars using a geometric Brownian motion model.
// This will be replaced by live IB Gateway data in a future task.
// ============================================================================
core::BarSeries ChartWindow::GenerateSimulatedBars(const std::string& symbol,
                                                    core::Timeframe tf,
                                                    int count) {
    // Seed by symbol name so each symbol gives a distinct chart
    std::size_t seed = std::hash<std::string>{}(symbol);
    std::mt19937 rng((unsigned)seed);
    std::normal_distribution<double> dist(0.0, 1.0);

    // Starting price and volatility, keyed by symbol
    struct SymConfig { double price; double vol; double drift; double avgVol; };
    auto cfg = [&]() -> SymConfig {
        if (symbol == "AAPL")  return {185.0,  0.015, 0.0003, 55e6};
        if (symbol == "MSFT")  return {415.0,  0.013, 0.0004, 25e6};
        if (symbol == "GOOGL") return {175.0,  0.016, 0.0003, 20e6};
        if (symbol == "TSLA")  return {250.0,  0.030, 0.0002, 90e6};
        if (symbol == "SPY")   return {520.0,  0.008, 0.0002, 80e6};
        return {100.0, 0.020, 0.0002, 10e6};
    }();

    // Work out the starting timestamp (go back `count` bars from now)
    int64_t tfSec  = core::TimeframeSeconds(tf);
    std::time_t now = std::time(nullptr);
    // Align to a round boundary
    now = (now / tfSec) * tfSec;
    int64_t startTs = now - (int64_t)count * tfSec;

    core::BarSeries series;
    series.symbol    = symbol;
    series.timeframe = tf;
    series.bars.reserve(count);

    double price = cfg.price;
    for (int i = 0; i < count; i++) {
        double ts = (double)(startTs + (int64_t)i * tfSec);

        // Skip weekends for daily bars
        if (tf == core::Timeframe::D1) {
            std::time_t t = (std::time_t)ts;
            std::tm* gm   = std::gmtime(&t);
            if (gm->tm_wday == 0 || gm->tm_wday == 6) {
                // Don't add a bar but still advance the price slightly
                price *= std::exp(cfg.drift + cfg.vol * dist(rng) * 0.3);
                continue;
            }
        }

        // GBM step
        double ret   = cfg.drift + cfg.vol * dist(rng);
        double open  = price;
        double close = price * std::exp(ret);

        // Intra-bar noise for high/low
        double intraVol = cfg.vol * 0.5;
        double wH = std::abs(dist(rng) * intraVol);
        double wL = std::abs(dist(rng) * intraVol);
        double high  = std::max(open, close) * std::exp(wH);
        double low   = std::min(open, close) * std::exp(-wL);

        // Volume: log-normal around average
        double volNoise = std::exp(0.4 * dist(rng));
        double volume   = cfg.avgVol * volNoise;

        core::Bar bar;
        bar.timestamp = ts;
        bar.open      = open;
        bar.high      = high;
        bar.low       = low;
        bar.close     = close;
        bar.volume    = volume;
        series.bars.push_back(bar);

        price = close;
    }

    return series;
}

}  // namespace ui
