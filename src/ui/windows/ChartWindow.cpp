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
#include <cstdio>
#include <fstream>

namespace ui {

static constexpr const char* kHistoryFile = "symbol_history.txt";

static constexpr core::Timeframe kAllTimeframes[] = {
    core::Timeframe::M1,  core::Timeframe::M5,  core::Timeframe::M15,
    core::Timeframe::M30, core::Timeframe::H1,  core::Timeframe::H4,
    core::Timeframe::D1,  core::Timeframe::W1,  core::Timeframe::MN,
};

static constexpr double kFibLevels[]  = { 0.0, 0.236, 0.382, 0.5, 0.618, 1.0 };
static constexpr unsigned int kFibColors[] = {
    IM_COL32(255, 100, 100, 200),
    IM_COL32(255, 200,  50, 200),
    IM_COL32( 80, 220,  80, 200),
    IM_COL32( 80, 180, 255, 200),
    IM_COL32(180,  80, 255, 200),
    IM_COL32(255, 100, 100, 200),
};

static bool IsIntraday(core::Timeframe tf) {
    return tf == core::Timeframe::M1  || tf == core::Timeframe::M5  ||
           tf == core::Timeframe::M15 || tf == core::Timeframe::M30 ||
           tf == core::Timeframe::H1  || tf == core::Timeframe::H4;
}

// ============================================================================
// Construction
// ============================================================================
ChartWindow::ChartWindow() {
    LoadHistory();
    RefreshData();
}

// ============================================================================
// Symbol history
// ============================================================================
void ChartWindow::LoadHistory() {
    std::ifstream f(kHistoryFile);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line) && (int)m_symbolHistory.size() < kMaxHistory)
        if (!line.empty()) m_symbolHistory.push_back(line);
}

void ChartWindow::SaveHistory() const {
    if (m_symbolHistory.empty()) return;
    std::ofstream f(kHistoryFile, std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& s : m_symbolHistory) f << s << '\n';
}

void ChartWindow::AddToHistory(const std::string& symbol) {
    if (symbol.empty()) return;
    auto it = std::find(m_symbolHistory.begin(), m_symbolHistory.end(), symbol);
    if (it != m_symbolHistory.end()) m_symbolHistory.erase(it);
    m_symbolHistory.push_front(symbol);
    if ((int)m_symbolHistory.size() > kMaxHistory) m_symbolHistory.pop_back();
    SaveHistory();
}

// ============================================================================
// SetSymbol / AddBar / SetHistoricalData
// ============================================================================
void ChartWindow::SetSymbol(const std::string& symbol) {
    if (symbol.empty() || symbol.size() >= sizeof(m_symbol)) return;
    std::memcpy(m_symbol, symbol.c_str(), symbol.size() + 1);
    m_viewInitialized = false;
    // Reset drawing / order state so NoInputs is never left armed on the new symbol
    m_drawTool    = DrawTool::Cursor;
    m_drawPending = false;
    m_limitArmed  = false;
    m_ctrlClickPopup = false;
    AddToHistory(symbol);
    RequestNewData();
}

void ChartWindow::AddBar(const core::Bar& bar, bool done) {
    if (!m_hasRealData) {
        m_series.bars.clear();
        m_series.symbol    = m_symbol;
        m_series.timeframe = m_timeframe;
        m_hasRealData      = true;
        m_needsRefresh     = false;
    }
    if (!done) {
        m_series.bars.push_back(bar);
    } else {
        m_loading         = false;
        m_viewInitialized = false;
        RebuildFlatArrays();
        ComputeIndicators();
    }
}

void ChartWindow::SetHistoricalData(const core::BarSeries& series) {
    m_series          = series;
    m_hasRealData     = true;
    m_needsRefresh    = false;
    m_loading         = false;
    m_viewInitialized = false;
    RebuildFlatArrays();
    ComputeIndicators();
}

void ChartWindow::UpdateLiveBar(const core::Bar& bar) {
    // Respect the same session filters used in RebuildFlatArrays
    if (IsIntraday(m_timeframe)) {
        auto s = core::BarSession((std::time_t)bar.timestamp);
        if (s != core::Session::Regular) {
            if (m_useRTH) return;
            if (s == core::Session::Overnight && !m_showOvernight) return;
        }
    }
    if (m_series.bars.empty() || m_xs.empty()) return;

    if (bar.timestamp == m_xs.back()) {
        // Update the forming bar in-place (same timestamp)
        m_series.bars.back() = bar;
        int i = (int)m_xs.size() - 1;
        m_opens[i]  = bar.open;  m_highs[i] = bar.high;
        m_lows[i]   = bar.low;   m_closes[i] = bar.close;
        m_volumes[i] = bar.volume;
        ComputeIndicators();
    } else if (bar.timestamp > m_xs.back()) {
        // New bar completed — append
        m_series.bars.push_back(bar);
        int n = (int)m_series.bars.size();
        m_xs.push_back(bar.timestamp);
        m_idxs.push_back((double)(n - 1));
        m_opens.push_back(bar.open);   m_highs.push_back(bar.high);
        m_lows.push_back(bar.low);     m_closes.push_back(bar.close);
        m_volumes.push_back(bar.volume);

        // Scroll X view to keep the new bar visible
        double newIdx = m_idxs.back();
        if (m_viewInitialized && newIdx >= m_xMax - 0.5) {
            double span = m_xMax - m_xMin;
            m_xMax = newIdx + 1.0;
            m_xMin = m_xMax - span;
        }
        ComputeIndicators();
    }
}

void ChartWindow::OnLastPrice(double price) {
    // Only for intraday bars — for D1/W1/MN use OnDayTick() instead.
    if (!IsIntraday(m_timeframe) || m_closes.empty() || price <= 0.0) return;
    int i = (int)m_closes.size() - 1;
    m_closes[i] = price;
    if (price > m_highs[i]) m_highs[i] = price;
    if (price < m_lows[i])  m_lows[i]  = price;
    auto& b = m_series.bars.back();
    b.close = price;
    if (price > b.high) b.high = price;
    if (price < b.low)  b.low  = price;
    ComputeIndicators();
}

void ChartWindow::EnsureTodayBar(double price) {
    // Compute today's date in local time
    std::time_t now = std::time(nullptr);
    struct tm nowTm{};
    localtime_r(&now, &nowTm);

    // No bars on weekends — IB won't send ticks either, but guard anyway
    if (nowTm.tm_wday == 0 || nowTm.tm_wday == 6) return;

    // Check if today's bar is already the last bar
    if (!m_xs.empty()) {
        std::time_t lastTs = static_cast<std::time_t>(m_xs.back());
        struct tm lastTm{};
        localtime_r(&lastTs, &lastTm);
        if (nowTm.tm_year == lastTm.tm_year && nowTm.tm_yday == lastTm.tm_yday)
            return;  // already have today
    }

    // Build today's bar at local midnight
    struct tm midnight = nowTm;
    midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
    midnight.tm_isdst = -1;
    auto todayTs = static_cast<double>(mktime(&midnight));

    core::Bar today{};
    today.timestamp = todayTs;
    today.open = today.high = today.low = today.close = price;
    today.volume = 0.0;

    m_series.bars.push_back(today);
    int n = static_cast<int>(m_xs.size());
    m_xs.push_back(todayTs);
    m_idxs.push_back(static_cast<double>(n));
    m_opens.push_back(price);
    m_highs.push_back(price);
    m_lows.push_back(price);
    m_closes.push_back(price);
    m_volumes.push_back(0.0);

    // Scroll the view to include the new bar
    if (m_viewInitialized) {
        double span = m_xMax - m_xMin;
        m_xMax = static_cast<double>(n) + 1.0;
        m_xMin = m_xMax - span;
    }
}

void ChartWindow::OnDayTick(int field, double price) {
    // Only for non-intraday timeframes (D1, W1, MN).
    // For intraday, use OnLastPrice() — day-range ticks (12/13) span multiple bars.
    if (IsIntraday(m_timeframe) || price <= 0.0 || m_xs.empty()) return;

    EnsureTodayBar(price);  // no-op if today's bar already exists

    int i = static_cast<int>(m_closes.size()) - 1;
    auto& b = m_series.bars.back();

    switch (field) {
        case 4:   // LAST — update close; also extend H/L if day stats not yet received
            m_closes[i] = price; b.close = price;
            if (price > m_highs[i]) { m_highs[i] = price; b.high = price; }
            if (price < m_lows[i])  { m_lows[i]  = price; b.low  = price; }
            break;
        case 6:   // HIGH (TickType::HIGH=6)
            m_highs[i] = price; b.high = price;
            break;
        case 7:   // LOW  (TickType::LOW=7)
            m_lows[i] = price; b.low = price;
            break;
        case 14:  // OPEN (TickType::OPEN=14)
            m_opens[i] = price; b.open = price;
            break;
        default:
            return;  // nothing changed, skip recompute
    }
    ComputeIndicators();
}

void ChartWindow::SetPendingOrders(const std::vector<PendingOrderLine>& orders) {
    m_pendingOrders = orders;
}

void ChartWindow::SetPosition(const PositionInfo& pos) {
    m_position = pos;
}

void ChartWindow::RequestNewData() {
    m_series = core::BarSeries{};
    m_xs.clear(); m_opens.clear(); m_highs.clear();
    m_lows.clear(); m_closes.clear(); m_volumes.clear();
    m_vwap.clear();

    if (OnDataRequest) {
        m_hasRealData = false;
        m_loading     = true;
        // For intraday timeframes, honour m_useRTH (user-toggleable).
        // For daily/weekly/monthly, always use RTH-only (no extended hours concept).
        bool rth = m_useRTH || !IsIntraday(m_timeframe);
        OnDataRequest(m_symbol, m_timeframe, rth);
    } else {
        m_hasRealData  = false;
        m_needsRefresh = true;
    }
}

// ============================================================================
// Render
// ============================================================================
bool ChartWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(1000, 720), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("Chart", &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    DrawToolbar();
    DrawAnalysisToolbar();
    DrawTradePanel();
    ImGui::Separator();

    if (m_needsRefresh) { RefreshData(); m_needsRefresh = false; }

    if (m_loading) {
        ImGui::TextDisabled("Loading %s [%s]...", m_symbol, core::TimeframeLabel(m_timeframe));
        ImGui::End();
        return m_open;
    }
    if (m_series.empty()) {
        ImGui::TextDisabled("No data available.");
        ImGui::End();
        return m_open;
    }

    if (!m_viewInitialized) InitViewRange();

    DrawInfoBar();
    DrawPositionStrip();
    DrawCandleChart();
    if (m_ind.volume) DrawVolumeChart();
    if (m_ind.rsi)    DrawRsiChart();

    ImGui::End();
    return m_open;
}

// ============================================================================
// Toolbar row 1 — symbol, timeframe, zoom, indicators
// ============================================================================
void ChartWindow::DrawToolbar() {
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputText("##sym", m_symbol, sizeof(m_symbol),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_viewInitialized = false;
        AddToHistory(m_symbol);
        RequestNewData();
    }

    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("v##hist")) ImGui::OpenPopup("##symhist");
    if (ImGui::BeginPopup("##symhist")) {
        ImGui::TextDisabled("Recent symbols");
        ImGui::Separator();
        if (m_symbolHistory.empty()) {
            ImGui::TextDisabled("(none yet)");
        } else {
            for (const auto& s : m_symbolHistory) {
                bool cur = (std::strcmp(m_symbol, s.c_str()) == 0);
                if (cur) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.f, 1.f));
                if (ImGui::Selectable(s.c_str())) {
                    std::strncpy(m_symbol, s.c_str(), sizeof(m_symbol) - 1);
                    m_symbol[sizeof(m_symbol) - 1] = '\0';
                    m_viewInitialized = false;
                    AddToHistory(s);
                    RequestNewData();
                    ImGui::CloseCurrentPopup();
                }
                if (cur) ImGui::PopStyleColor();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine(0, 8);
    const char* syms[] = {"AAPL", "MSFT", "GOOGL", "TSLA", "SPY"};
    for (const char* s : syms) {
        ImGui::SameLine();
        bool active = (std::strcmp(m_symbol, s) == 0);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.9f, 1.f));
        if (ImGui::SmallButton(s)) {
            std::strncpy(m_symbol, s, sizeof(m_symbol) - 1);
            m_symbol[sizeof(m_symbol) - 1] = '\0';
            m_viewInitialized = false;
            AddToHistory(s);
            RequestNewData();
        }
        if (active) ImGui::PopStyleColor();
    }

    ImGui::SameLine(0, 16);
    ImGui::SetNextItemWidth(55);
    if (ImGui::BeginCombo("##tf", core::TimeframeLabel(m_timeframe))) {
        for (core::Timeframe tf : kAllTimeframes) {
            bool sel = (m_timeframe == tf);
            if (ImGui::Selectable(core::TimeframeLabel(tf), sel)) {
                if (m_timeframe != tf) {
                    m_timeframe = tf;
                    m_viewInitialized = false;
                    RequestNewData();
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Horizontal zoom buttons — contract/expand the visible X window by 25%
    ImGui::SameLine(0, 8);
    if (ImGui::SmallButton("[+]") && m_viewInitialized) {
        double center = (m_xMin + m_xMax) * 0.5;
        double half   = (m_xMax - m_xMin) * 0.5 * 0.75;
        m_xMin = center - half;
        m_xMax = center + half;
    }
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("[-]") && m_viewInitialized) {
        double center = (m_xMin + m_xMax) * 0.5;
        double half   = (m_xMax - m_xMin) * 0.5 * 1.333;
        m_xMin = center - half;
        m_xMax = center + half;
    }

    // Extended hours toggles (intraday only)
    if (IsIntraday(m_timeframe)) {
        ImGui::SameLine(0, 16);
        bool extHours = !m_useRTH;
        if (ImGui::Checkbox("Ext.Hrs", &extHours)) {
            m_useRTH = !extHours;
            if (m_useRTH) m_showOvernight = false;  // reset when ext hours disabled
            RequestNewData();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Include pre-market & after-hours bars");

        if (!m_useRTH) {
            ImGui::SameLine(0, 4);
            if (ImGui::Checkbox("Overnight", &m_showOvernight)) {
                RebuildFlatArrays();
                ComputeIndicators();
                m_viewInitialized = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Include overnight bars (00:00-04:00 ET)");
        }

        ImGui::SameLine(0, 4);
        ImGui::Checkbox("Sessions", &m_showSessions);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Highlight trading session backgrounds");
    }

    ImGui::SameLine(0, 16);
    ImGui::Checkbox("SMA20",  &m_ind.sma20);  ImGui::SameLine();
    ImGui::Checkbox("SMA50",  &m_ind.sma50);  ImGui::SameLine();
    ImGui::Checkbox("EMA20",  &m_ind.ema20);  ImGui::SameLine();
    ImGui::Checkbox("BB",     &m_ind.bbands); ImGui::SameLine();
    ImGui::Checkbox("VWAP",   &m_ind.vwap);   ImGui::SameLine();
    ImGui::Checkbox("Vol",    &m_ind.volume); ImGui::SameLine();
    ImGui::Checkbox("RSI",    &m_ind.rsi);
}

// ============================================================================
// Toolbar row 2 — drawing / analysis tools
// ============================================================================
void ChartWindow::DrawAnalysisToolbar() {
    ImGui::Spacing();

    struct ToolEntry { DrawTool tool; const char* label; const char* tooltip; };
    static constexpr ToolEntry kTools[] = {
        { DrawTool::Cursor,    "Cursor",   "Pan / zoom (default)"          },
        { DrawTool::HLine,     "H-Line",   "Click to place a horizontal price level" },
        { DrawTool::TrendLine, "Trend",    "Click twice to draw a trend line"       },
        { DrawTool::Fibonacci, "Fib",      "Click high then low for Fibonacci levels"},
        { DrawTool::Erase,     "Erase",    "Click near a drawing to remove it"      },
    };

    ImGui::TextDisabled("Analysis:");
    for (const auto& e : kTools) {
        ImGui::SameLine(0, 4);
        bool active = (m_drawTool == e.tool);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.f));
        if (ImGui::SmallButton(e.label)) {
            if (m_drawTool == e.tool) {
                m_drawTool    = DrawTool::Cursor;  // toggle off
                m_drawPending = false;
            } else {
                m_drawTool    = e.tool;
                m_drawPending = false;
            }
        }
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.tooltip);
    }

    ImGui::SameLine(0, 12);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.15f, 1.f));
    if (ImGui::SmallButton("Clear All")) {
        m_drawings.clear();
        m_drawPending = false;
        m_drawTool    = DrawTool::Cursor;
    }
    ImGui::PopStyleColor();

    if (m_drawPending) {
        ImGui::SameLine(0, 16);
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "(click second point...)");
    }
}

// ============================================================================
// Toolbar row 3 — trade panel
// ============================================================================
void ChartWindow::DrawTradePanel() {
    // ── Order type definitions ──────────────────────────────────────────────
    struct OrderTypeDef {
        const char* label;   // displayed in combo
        const char* ibStr;   // IB API string
        bool needsPrice;     // arm chart-click mode
        bool needsTrail;     // show trail amount input
    };
    static constexpr OrderTypeDef kOrderTypes[] = {
        { "Market",                    "MKT",        false, false },
        { "Limit",                     "LMT",        true,  false },
        { "Stop",                      "STP",        true,  false },
        { "Stop Limit",                "STP LMT",    true,  false },
        { "Market to Limit",           "MTL",        false, false },
        { "Adjustable Stop",           "STP",        true,  false },
        { "Trailing",                  "TRAIL",      false, true  },
        { "Trailing Limit",            "TRAIL LIMIT",false, true  },
        { "Trailing Limit If Touched", "LIT",        true,  true  },
        { "Trailing Market If Touched","MIT",        true,  true  },
    };
    static constexpr int kNumOrderTypes = (int)std::size(kOrderTypes);

    static constexpr const char* kSessions[] = {
        "Regular", "Pre-Market", "After Hours", "Overnight"
    };
    static constexpr const char* kTIFs[] = { "DAY", "GTC", "GTD" };

    ImGui::Spacing();

    // ── Qty ─────────────────────────────────────────────────────────────────
    ImGui::TextDisabled("Trade:"); ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(62);
    ImGui::InputInt("Qty##ord", &m_orderQty, 0, 0);
    if (m_orderQty < 1) m_orderQty = 1;

    // ── Order type ──────────────────────────────────────────────────────────
    ImGui::SameLine(0, 8);
    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("##otype", kOrderTypes[m_orderTypeIdx].label)) {
        for (int i = 0; i < kNumOrderTypes; i++) {
            bool sel = (i == m_orderTypeIdx);
            if (ImGui::Selectable(kOrderTypes[i].label, sel)) {
                m_orderTypeIdx = i;
                m_limitArmed   = false; // cancel any pending placement
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ── Trail amount (only for trailing types) ───────────────────────────
    if (kOrderTypes[m_orderTypeIdx].needsTrail) {
        ImGui::SameLine(0, 6);
        ImGui::TextDisabled("Trail $:");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(58);
        ImGui::InputDouble("##trail", &m_trailAmount, 0.01, 0.10, "%.2f");
        if (m_trailAmount <= 0.0) m_trailAmount = 0.01;
    }

    // ── Stop-limit offset ────────────────────────────────────────────────
    if (m_orderTypeIdx == 3) { // Stop Limit
        ImGui::SameLine(0, 6);
        ImGui::TextDisabled("Lmt offset $:");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(52);
        ImGui::InputDouble("##lmtoff", &m_limitOffset, 0.01, 0.10, "%.2f");
    }

    // ── Session ─────────────────────────────────────────────────────────────
    ImGui::SameLine(0, 10);
    ImGui::SetNextItemWidth(95);
    if (ImGui::BeginCombo("##sess", kSessions[m_sessionIdx])) {
        for (int i = 0; i < 4; i++) {
            bool sel = (i == m_sessionIdx);
            if (ImGui::Selectable(kSessions[i], sel)) m_sessionIdx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ── TIF ─────────────────────────────────────────────────────────────────
    ImGui::SameLine(0, 6);
    ImGui::SetNextItemWidth(52);
    if (ImGui::BeginCombo("##tif", kTIFs[m_tifIdx])) {
        for (int i = 0; i < 3; i++) {
            bool sel = (i == m_tifIdx);
            if (ImGui::Selectable(kTIFs[i], sel)) m_tifIdx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ── BUY / SELL buttons (right-aligned, larger) ───────────────────────
    ImGui::SameLine(0, 14);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.08f, 0.52f, 0.08f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.72f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.04f, 0.38f, 0.04f, 1.f));
    bool buyClicked = ImGui::Button("  BUY  ##ord", ImVec2(72, 26));
    ImGui::PopStyleColor(3);

    ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.52f, 0.08f, 0.08f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.04f, 0.04f, 1.f));
    bool sellClicked = ImGui::Button(" SELL  ##ord", ImVec2(72, 26));
    ImGui::PopStyleColor(3);

    // ── Status hint when armed ──────────────────────────────────────────────
    if (m_limitArmed) {
        ImGui::SameLine(0, 12);
        ImVec4 hintCol = (m_limitSide == "BUY")
                         ? ImVec4(0.4f, 0.7f, 1.f, 1.f)
                         : ImVec4(1.f, 0.4f, 0.4f, 1.f);
        ImGui::TextColored(hintCol,
            "Hover chart → click [Ctrl+click to confirm]  [Esc=cancel]");
    }

    // ── Ctrl+Click price confirmation popup ─────────────────────────────────
    if (m_ctrlClickPopup)
        ImGui::OpenPopup("##limitconfirm");
    m_ctrlClickPopup = false;

    if (ImGui::BeginPopup("##limitconfirm")) {
        ImVec4 titleCol = (m_limitSide == "BUY")
                          ? ImVec4(0.4f, 0.8f, 1.f, 1.f)
                          : ImVec4(1.f, 0.45f, 0.45f, 1.f);
        ImGui::TextColored(titleCol, "Confirm %s Order", m_limitSide.c_str());
        ImGui::Separator();
        ImGui::Text("Type: %s", kOrderTypes[m_orderTypeIdx].label);
        ImGui::SetNextItemWidth(100);
        ImGui::InputDouble("Price##confirm", &m_pendingPrice, 0.01, 0.10, "$%.2f");
        ImGui::SetNextItemWidth(80);
        int qty = m_orderQty;
        ImGui::InputInt("Qty##confirm", &qty, 1, 10);
        if (qty < 1) qty = 1;
        m_orderQty = qty;
        ImGui::Spacing();
        bool confirm = ImGui::Button("Confirm##cp", ImVec2(80, 0));
        ImGui::SameLine(0, 8);
        bool cancel = ImGui::Button("Cancel##cp",  ImVec2(80, 0));
        if (confirm) {
            if (OnOrderSubmit) {
                const auto& ot = kOrderTypes[m_orderTypeIdx];
                bool outsideRth = (m_sessionIdx != 0);
                std::string tif = kTIFs[m_tifIdx];
                double aux = ot.needsTrail ? m_trailAmount
                           : (m_orderTypeIdx == 3 ? m_pendingPrice - m_limitOffset : 0.0);
                OnOrderSubmit(m_symbol, m_limitSide, ot.ibStr,
                              m_orderQty, m_pendingPrice, tif, outsideRth, aux);
            }
            m_limitArmed = false;
            ImGui::CloseCurrentPopup();
        }
        if (cancel) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── Button logic ────────────────────────────────────────────────────────
    auto fireOrder = [&](const std::string& side) {
        const auto& ot      = kOrderTypes[m_orderTypeIdx];
        bool outsideRth     = (m_sessionIdx != 0);
        std::string tif     = kTIFs[m_tifIdx];

        if (!ot.needsPrice) {
            // Market / Market-to-Limit / Trailing (no chart click needed)
            if (OnOrderSubmit)
                OnOrderSubmit(m_symbol, side, ot.ibStr,
                              m_orderQty, 0.0, tif, outsideRth, m_trailAmount);
        } else {
            // Arm chart-click placement mode
            m_limitArmed = true;
            m_limitSide  = side;
        }
    };

    if (buyClicked)  fireOrder("BUY");
    if (sellClicked) fireOrder("SELL");

    // Escape cancels armed placement
    if (m_limitArmed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_limitArmed = false;
    }
}

// ============================================================================
// Info bar
// ============================================================================
void ChartWindow::DrawInfoBar() {
    int n = (int)m_xs.size();
    if (n == 0) return;

    int idx = (m_hoverIdx >= 0 && m_hoverIdx < n) ? m_hoverIdx : (n - 1);

    std::time_t t  = (std::time_t)m_xs[idx];
    char dateBuf[32];
    if (IsIntraday(m_timeframe)) {
        std::tm* tm = std::localtime(&t);
        std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d  %H:%M", tm ? tm : std::gmtime(&t));
    } else {
        std::tm* tm = std::gmtime(&t);
        std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", tm);
    }

    double change    = m_closes[idx] - m_opens[idx];
    double changePct = (m_opens[idx] != 0.0) ? (change / m_opens[idx] * 100.0) : 0.0;
    bool   bull      = change >= 0.0;
    ImVec4 chgCol    = bull ? ImVec4(0.2f, 0.9f, 0.4f, 1.f) : ImVec4(0.9f, 0.3f, 0.3f, 1.f);

    ImGui::TextDisabled("%s  [%s]", m_symbol, dateBuf);
    ImGui::SameLine(0, 12);
    ImGui::TextDisabled("O:%.2f  H:%.2f  L:%.2f  C:%.2f",
                        m_opens[idx], m_highs[idx], m_lows[idx], m_closes[idx]);
    ImGui::SameLine(0, 12);
    ImGui::TextColored(chgCol, "%+.2f  (%+.2f%%)", change, changePct);

    if (m_ind.vwap && idx < (int)m_vwap.size() && m_vwap[idx] > 0.0) {
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "VWAP:%.2f", m_vwap[idx]);
    }

    if (m_position.hasPosition && std::abs(m_position.qty) >= 1e-9) {
        double netPnL = m_position.unrealPnL - m_position.commission;
        ImVec4 pnlCol = netPnL >= 0.0
            ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
            : ImVec4(0.90f, 0.28f, 0.28f, 1.f);
        ImGui::SameLine(0, 18);
        ImGui::TextColored(pnlCol, "PnL:%+.2f", netPnL);
        ImGui::SameLine(0, 6);
        ImGui::TextDisabled("(comm -%.2f)", m_position.commission);
    }
}

// ============================================================================
// InitViewRange
// ============================================================================
void ChartWindow::InitViewRange() {
    int n = (int)m_idxs.size();
    if (n == 0) return;

    int dc   = std::min(n, 100);

    m_xMin = m_idxs[n - dc] - 0.5;
    m_xMax = m_idxs[n - 1]  + 1.5;

    double pMin =  1e18, pMax = -1e18;
    for (int i = n - dc; i < n; i++) {
        pMin = std::min(pMin, m_lows[i]);
        pMax = std::max(pMax, m_highs[i]);
    }
    double margin = (pMax - pMin) * 0.08;
    m_priceMin    = pMin - margin;
    m_priceMax    = pMax + margin;

    m_viewInitialized = true;
}

// ============================================================================
// DrawDashedHLine — helper (screen-space pixels)
// ============================================================================
void ChartWindow::DrawDashedHLine(ImDrawList* dl,
                                   float x0, float x1, float y,
                                   unsigned int color, float thickness,
                                   float dashLen, float gapLen) {
    float x = x0;
    while (x < x1) {
        float xe = std::min(x + dashLen, x1);
        dl->AddLine(ImVec2(x, y), ImVec2(xe, y), color, thickness);
        x += dashLen + gapLen;
    }
}

// ============================================================================
// IsNearDrawing — check mouse proximity for erase tool
// ============================================================================
bool ChartWindow::IsNearDrawing(const Drawing& d, double mx, double my,
                                 double yTol, double xTol) const {
    switch (d.type) {
        case Drawing::Type::HLine:
            return std::abs(my - d.y1) < yTol;

        case Drawing::Type::TrendLine: {
            // Distance from point to line segment
            double dx = d.x2 - d.x1, dy = d.y2 - d.y1;
            double len2 = dx * dx + dy * dy;
            if (len2 < 1e-12) return std::abs(mx - d.x1) < xTol && std::abs(my - d.y1) < yTol;
            double t = ((mx - d.x1) * dx + (my - d.y1) * dy) / len2;
            t = std::max(0.0, std::min(1.0, t));
            double projY = d.y1 + t * dy;
            return std::abs(my - projY) < yTol;
        }

        case Drawing::Type::Fibonacci: {
            double lo = std::min(d.y1, d.y2), hi = std::max(d.y1, d.y2);
            for (double lvl : kFibLevels) {
                double price = lo + lvl * (hi - lo);
                if (std::abs(my - price) < yTol) return true;
            }
            return false;
        }
    }
    return false;
}

// ============================================================================
// DrawOverlays — called inside BeginPlot / EndPlot
// Renders all stored drawings, handles new drawing clicks, draws limit line.
// ============================================================================
void ChartWindow::DrawOverlays(double /*step*/) {
    ImDrawList* dl   = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    // Use direct rect-hit for hover (robust even if NoInputs is active for drawing tools)
    ImVec2 pMin = ImPlot::GetPlotPos();
    ImVec2 pMax = ImVec2(pMin.x + ImPlot::GetPlotSize().x,
                         pMin.y + ImPlot::GetPlotSize().y);
    bool hovered   = ImGui::IsMouseHoveringRect(pMin, pMax, false);
    ImPlotPoint mp = hovered ? ImPlot::GetPlotMousePos() : ImPlotPoint{0, 0};

    // ── Render stored drawings ─────────────────────────────────────────────
    for (const auto& dr : m_drawings) {
        switch (dr.type) {
            case Drawing::Type::HLine: {
                ImVec2 p0 = ImPlot::PlotToPixels(m_xMin, dr.y1);
                ImVec2 p1 = ImPlot::PlotToPixels(m_xMax, dr.y1);
                DrawDashedHLine(dl, p0.x, p1.x, p0.y,
                                IM_COL32(255, 220, 50, 220), 1.5f);
                // Price label
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.2f", dr.y1);
                dl->AddText(ImVec2(p1.x + 4, p0.y - 6),
                            IM_COL32(255, 220, 50, 255), buf);
                break;
            }
            case Drawing::Type::TrendLine: {
                ImVec2 p0 = ImPlot::PlotToPixels(dr.x1, dr.y1);
                ImVec2 p1 = ImPlot::PlotToPixels(dr.x2, dr.y2);
                dl->AddLine(p0, p1, IM_COL32(255, 255, 255, 200), 1.5f);
                dl->AddCircleFilled(p0, 3.f, IM_COL32(255, 255, 255, 180));
                dl->AddCircleFilled(p1, 3.f, IM_COL32(255, 255, 255, 180));
                break;
            }
            case Drawing::Type::Fibonacci: {
                double lo = std::min(dr.y1, dr.y2), hi = std::max(dr.y1, dr.y2);
                for (int fi = 0; fi < 6; fi++) {
                    double price = lo + kFibLevels[fi] * (hi - lo);
                    ImVec2 p0 = ImPlot::PlotToPixels(m_xMin, price);
                    ImVec2 p1 = ImPlot::PlotToPixels(m_xMax, price);
                    DrawDashedHLine(dl, p0.x, p1.x, p0.y, kFibColors[fi], 1.0f);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f%%  %.2f",
                                  kFibLevels[fi] * 100.0, price);
                    dl->AddText(ImVec2(p1.x + 4, p0.y - 6), kFibColors[fi], buf);
                }
                break;
            }
        }
    }

    // ── Preview: pending second click ─────────────────────────────────────
    if (m_drawPending && hovered) {
        ImVec2 p0 = ImPlot::PlotToPixels(m_drawPt1X, m_drawPt1Y);
        ImVec2 p1 = ImPlot::PlotToPixels(mp.x, mp.y);
        if (m_drawTool == DrawTool::TrendLine) {
            dl->AddLine(p0, p1, IM_COL32(255, 255, 255, 130), 1.5f);
            dl->AddCircleFilled(p0, 3.f, IM_COL32(255, 255, 255, 180));
        } else if (m_drawTool == DrawTool::Fibonacci) {
            // Preview fib levels
            double lo = std::min(m_drawPt1Y, mp.y), hi = std::max(m_drawPt1Y, mp.y);
            for (int fi = 0; fi < 6; fi++) {
                double price = lo + kFibLevels[fi] * (hi - lo);
                ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, price);
                ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, price);
                DrawDashedHLine(dl, lp0.x, lp1.x, lp0.y,
                                (kFibColors[fi] & 0x00FFFFFF) | 0x66000000, 1.0f);
            }
        }
    }

    // ── Handle drawing tool clicks ─────────────────────────────────────────
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        double yTol = (m_priceMax - m_priceMin) * 0.015;
        double xTol = (m_xMax - m_xMin) * 0.015;

        switch (m_drawTool) {
            case DrawTool::HLine: {
                Drawing dr;
                dr.type = Drawing::Type::HLine;
                dr.y1   = mp.y;
                m_drawings.push_back(dr);
                break;
            }
            case DrawTool::TrendLine:
            case DrawTool::Fibonacci: {
                if (!m_drawPending) {
                    m_drawPt1X    = mp.x;
                    m_drawPt1Y    = mp.y;
                    m_drawPending = true;
                } else {
                    Drawing dr;
                    dr.type = (m_drawTool == DrawTool::TrendLine)
                                  ? Drawing::Type::TrendLine
                                  : Drawing::Type::Fibonacci;
                    dr.x1 = m_drawPt1X; dr.y1 = m_drawPt1Y;
                    dr.x2 = mp.x;       dr.y2 = mp.y;
                    m_drawings.push_back(dr);
                    m_drawPending = false;
                }
                break;
            }
            case DrawTool::Erase: {
                auto it = std::find_if(m_drawings.begin(), m_drawings.end(),
                    [&](const Drawing& d) {
                        return IsNearDrawing(d, mp.x, mp.y, yTol, xTol);
                    });
                if (it != m_drawings.end()) m_drawings.erase(it);
                break;
            }
            default: break;
        }
    }

    // ── Pending order lines (from live orders for this symbol) ────────────
    for (const auto& order : m_pendingOrders) {
        if (order.price <= 0.0) continue;
        ImU32 lineCol = order.isBuy ? IM_COL32( 60, 140, 255, 200)
                                    : IM_COL32(255,  80,  80, 200);
        ImU32 txtCol  = order.isBuy ? IM_COL32(140, 190, 255, 255)
                                    : IM_COL32(255, 140, 140, 255);

        ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, order.price);
        ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, order.price);
        DrawDashedHLine(dl, lp0.x, lp1.x, lp0.y, lineCol, 1.5f, 8.f, 5.f);

        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), " %s %.0f @ $%.2f ",
                      order.isBuy ? "BUY" : "SELL", order.qty, order.price);
        ImVec2 lblSz = ImGui::CalcTextSize(lbl);
        float  lblX  = lp0.x + 6.f;
        dl->AddRectFilled(ImVec2(lblX - 2, lp0.y - 8),
                          ImVec2(lblX + lblSz.x + 2, lp0.y + 8),
                          IM_COL32(20, 20, 30, 200));
        dl->AddText(ImVec2(lblX, lp0.y - 7), txtCol, lbl);

        // ✕ cancel button
        float  btnX = lblX + lblSz.x + 4.f;
        float  btnY = lp0.y - 7.f;
        ImVec2 btnMin(btnX, btnY);
        ImVec2 btnMax(btnX + 14.f, btnY + 14.f);
        bool   hoverBtn = ImGui::IsMouseHoveringRect(btnMin, btnMax, false);
        dl->AddRectFilled(btnMin, btnMax,
                          hoverBtn ? IM_COL32(200, 50, 50, 220)
                                   : IM_COL32(120, 30, 30, 180));
        dl->AddText(ImVec2(btnX + 3, btnY), IM_COL32(255, 210, 210, 255), "x");
        if (hoverBtn && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (OnCancelOrder) OnCancelOrder(order.orderId);
        }
    }

    // ── Limit / Stop price line ────────────────────────────────────────────
    if (m_limitArmed && hovered) {
        // Snap to the minimum price variation (0.01 for US equities >= $1).
        // IB rejects orders whose price doesn't conform (error 110).
        double price  = std::round(mp.y / 0.01) * 0.01;
        bool   isBuy  = (m_limitSide == "BUY");
        ImU32  lineCol = isBuy ? IM_COL32( 80, 140, 255, 220)
                               : IM_COL32(255,  80,  80, 220);
        ImU32  txtCol  = isBuy ? IM_COL32(140, 180, 255, 255)
                               : IM_COL32(255, 130, 130, 255);

        ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, price);
        ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, price);
        DrawDashedHLine(dl, lp0.x, lp1.x, lp0.y, lineCol, 1.5f, 8.f, 5.f);

        bool ctrlHeld = ImGui::GetIO().KeyCtrl;

        // Price label at right edge (shows Ctrl hint)
        char priceBuf[48];
        if (ctrlHeld)
            std::snprintf(priceBuf, sizeof(priceBuf), " %s  $%.2f  [Ctrl: confirm]",
                          m_limitSide.c_str(), price);
        else
            std::snprintf(priceBuf, sizeof(priceBuf), " %s  $%.2f",
                          m_limitSide.c_str(), price);
        ImVec2 lblSz = ImGui::CalcTextSize(priceBuf);
        dl->AddRectFilled(ImVec2(lp1.x, lp0.y - 9),
                          ImVec2(lp1.x + lblSz.x + 6, lp0.y + 9),
                          IM_COL32(30, 30, 30, 210));
        dl->AddText(ImVec2(lp1.x + 3, lp0.y - 7), txtCol, priceBuf);

        // Click to place (or Ctrl+click to open confirmation popup)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (ctrlHeld) {
                // Open confirmation popup with price pre-filled
                m_pendingPrice   = price;
                m_ctrlClickPopup = true;
                // popup is opened in DrawTradePanel next frame
            } else {
                // Immediate submit
                struct OT { const char* ibStr; bool needsTrail; };
                static constexpr OT kOT[] = {
                    {"MKT",false},{"LMT",false},{"STP",false},{"STP LMT",false},
                    {"MTL",false},{"STP",false},{"TRAIL",true},{"TRAIL LIMIT",true},
                    {"LIT",true},{"MIT",true}
                };
                if (OnOrderSubmit) {
                    bool outsideRth = (m_sessionIdx != 0);
                    static constexpr const char* kTIFs[] = {"DAY","GTC","GTD"};
                    double rawAux = kOT[m_orderTypeIdx].needsTrail ? m_trailAmount
                                 : (m_orderTypeIdx == 3 ? price - m_limitOffset : 0.0);
                    double aux = (m_orderTypeIdx == 3)
                                 ? std::round(rawAux / 0.01) * 0.01
                                 : rawAux;
                    OnOrderSubmit(m_symbol, m_limitSide, kOT[m_orderTypeIdx].ibStr,
                                  m_orderQty, price,
                                  kTIFs[m_tifIdx], outsideRth, aux);
                }
                m_limitArmed = false;
            }
        }
    }

    ImPlot::PopPlotClipRect();
}

// ============================================================================
// Position P&L strip
// ============================================================================
void ChartWindow::DrawPositionStrip() {
    if (!m_position.hasPosition || std::abs(m_position.qty) < 1e-9) return;

    double qty    = m_position.qty;
    double entry  = m_position.avgCost;
    double last   = m_position.lastPrice > 0.0 ? m_position.lastPrice
                                                : m_position.avgCost;
    double comm   = m_position.commission;
    double unreal = m_position.unrealPnL;   // IB-computed; fallback below
    if (unreal == 0.0 && entry > 0.0)
        unreal = (last - entry) * qty;
    double net    = unreal - comm;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.12f, 1.0f));
    ImGui::BeginChild("##posstrip", ImVec2(-1, 28), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    float cy = (ImGui::GetWindowHeight() - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorPosY(cy);

    ImGui::TextDisabled("Position:");
    ImGui::SameLine(0, 6);
    ImGui::PushStyleColor(ImGuiCol_Text,
        qty >= 0 ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                 : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::Text("%.0f sh", qty);
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 12); ImGui::TextDisabled("Entry:"); ImGui::SameLine(0, 4);
    ImGui::Text("$%.2f", entry);

    ImGui::SameLine(0, 12); ImGui::TextDisabled("Last:"); ImGui::SameLine(0, 4);
    ImGui::Text("$%.2f", last);

    ImGui::SameLine(0, 16); ImGui::TextDisabled("Unreal P&L:"); ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Text,
        unreal >= 0 ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                    : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::Text("%+.2f", unreal);
    ImGui::PopStyleColor();

    if (comm > 0.0) {
        ImGui::SameLine(0, 12); ImGui::TextDisabled("Comm:"); ImGui::SameLine(0, 4);
        ImGui::Text("-$%.2f", comm);
    }

    ImGui::SameLine(0, 16); ImGui::TextDisabled("Net:"); ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Text,
        net >= 0 ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                 : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::Text("%+.2f", net);
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================================
// Candlestick + overlay chart
// ============================================================================
void ChartWindow::DrawCandleChart() {
    int n = (int)m_idxs.size();
    if (n == 0) return;

    float available = ImGui::GetContentRegionAvail().y;
    float volumeH   = m_ind.volume ? available * m_volumeHeightRatio : 0.0f;
    float rsiH      = m_ind.rsi    ? available * 0.20f               : 0.0f;
    float spacing   = ImGui::GetStyle().ItemSpacing.y;
    float chartH    = available - volumeH - rsiH
                      - (m_ind.volume ? spacing : 0.0f)
                      - (m_ind.rsi    ? spacing : 0.0f);

    // Disable ImPlot panning for drawing tools.
    // Do NOT add NoInputs for m_limitArmed: ImPlot must keep its mouse-position
    // state live so GetPlotMousePos() returns the correct price on click.
    bool drawingActive = (m_drawTool != DrawTool::Cursor);
    ImPlotFlags plotFlags = ImPlotFlags_NoMouseText;
    if (drawingActive) plotFlags |= ImPlotFlags_NoInputs;

    if (!ImPlot::BeginPlot("##candles", ImVec2(-1, chartH), plotFlags))
        return;

    // Index-based X axis — eliminates weekend/overnight/holiday gaps.
    // Custom formatter maps index → timestamp label.
    ImPlot::SetupAxes(nullptr, "Price ($)", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_X1, XTickFormatter, this);
    ImPlot::SetupAxisLinks(ImAxis_X1, &m_xMin, &m_xMax);
    ImPlot::SetupAxisLinks(ImAxis_Y1, &m_priceMin, &m_priceMax);
    ImPlot::SetupFinish();

    // Session background bands (pre/post/overnight shading)
    DrawSessionBands();

    double halfBarW = 0.4;  // 0.4 index units each side

    if (m_ind.bbands && (int)m_bbUpper.size() == n) {
        ImPlot::SetNextFillStyle(ImVec4(0.5f, 0.5f, 1.f, 0.12f));
        ImPlot::PlotShaded("##bb_fill", m_idxs.data(), m_bbLower.data(), m_bbUpper.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.f, 0.7f), 1.f);
        ImPlot::PlotLine("BB Upper", m_idxs.data(), m_bbUpper.data(), n);
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.f, 0.5f), 1.f);
        ImPlot::PlotLine("BB Mid",   m_idxs.data(), m_bbMid.data(),   n);
        ImPlot::SetNextLineStyle(ImVec4(0.4f, 0.4f, 1.f, 0.7f), 1.f);
        ImPlot::PlotLine("BB Lower", m_idxs.data(), m_bbLower.data(), n);
    }
    if (m_ind.sma20 && (int)m_sma1.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(1.f, 0.8f, 0.f, 1.f), 1.5f);
        ImPlot::PlotLine("SMA20", m_idxs.data(), m_sma1.data(), n);
    }
    if (m_ind.sma50 && (int)m_sma2.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(1.f, 0.5f, 0.f, 1.f), 1.5f);
        ImPlot::PlotLine("SMA50", m_idxs.data(), m_sma2.data(), n);
    }
    if (m_ind.ema20 && (int)m_ema.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(0.f, 0.9f, 1.f, 1.f), 1.5f);
        ImPlot::PlotLine("EMA20", m_idxs.data(), m_ema.data(), n);
    }
    if (m_ind.vwap && (int)m_vwap.size() == n) {
        ImPlot::SetNextLineStyle(ImVec4(1.f, 1.f, 1.f, 1.f), 2.f);
        ImPlot::PlotLine("VWAP", m_idxs.data(), m_vwap.data(), n);
    }

    DrawCandlesticks(halfBarW);
    DrawOverlays(1.0);
    DrawHoverTooltip();

    ImPlot::EndPlot();
}

// ============================================================================
// Custom candlestick renderer
// ============================================================================
void ChartWindow::DrawCandlesticks(double /*halfBarWidth*/) {
    int n = (int)m_idxs.size();
    if (n == 0) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    m_hoverIdx = -1;
    if (ImPlot::IsPlotHovered() && m_drawTool == DrawTool::Cursor && !m_limitArmed) {
        ImPlotPoint mp = ImPlot::GetPlotMousePos();
        // Snap to the nearest bar index
        int nearest = (int)std::round(mp.x);
        if (nearest >= 0 && nearest < n)
            m_hoverIdx = nearest;
    }

    static constexpr double kHalf = 0.4;  // half-width in index units

    bool intraday = IsIntraday(m_timeframe);

    for (int i = 0; i < n; i++) {
        bool   bull  = m_closes[i] >= m_opens[i];
        double bodyH = std::max(m_opens[i], m_closes[i]);
        double bodyL = std::min(m_opens[i], m_closes[i]);

        ImVec2 topL  = ImPlot::PlotToPixels(m_idxs[i] - kHalf, bodyH);
        ImVec2 botR  = ImPlot::PlotToPixels(m_idxs[i] + kHalf, bodyL);
        ImVec2 wHigh = ImPlot::PlotToPixels(m_idxs[i], m_highs[i]);
        ImVec2 wLow  = ImPlot::PlotToPixels(m_idxs[i], m_lows[i]);
        float  midX  = (topL.x + botR.x) * 0.5f;

        // Session-based color: dimmer / slightly different hue for extended hours
        core::Session sess = intraday
            ? core::BarSession((std::time_t)m_xs[i])
            : core::Session::Regular;

        ImU32 col, colDim, colHov;
        if (sess == core::Session::Regular) {
            col    = bull ? IM_COL32( 52, 211, 100, 255) : IM_COL32(220,  60,  60, 255);
            colDim = bull ? IM_COL32( 30, 140,  60, 255) : IM_COL32(160,  30,  30, 255);
            colHov = bull ? IM_COL32(100, 255, 150, 255) : IM_COL32(255, 110, 110, 255);
        } else {
            // Extended hours: desaturated, lower alpha
            col    = bull ? IM_COL32( 40, 160,  90, 180) : IM_COL32(160,  50,  50, 180);
            colDim = bull ? IM_COL32( 25, 100,  55, 160) : IM_COL32(110,  30,  30, 160);
            colHov = bull ? IM_COL32( 80, 200, 130, 220) : IM_COL32(200,  90,  90, 220);
        }

        bool  hov     = (i == m_hoverIdx);
        ImU32 fillCol = hov ? colHov : col;
        ImU32 wickCol = hov ? colHov : colDim;

        dl->AddLine(ImVec2(midX, wHigh.y), ImVec2(midX, wLow.y), wickCol, 1.0f);
        float bh = std::abs(botR.y - topL.y);
        if (bh < 1.5f)
            dl->AddLine(ImVec2(topL.x, topL.y), ImVec2(botR.x, topL.y), fillCol, 1.5f);
        else {
            dl->AddRectFilled(topL, botR, fillCol);
            dl->AddRect(topL, botR, wickCol, 0.f, 0, 0.5f);
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

    std::time_t t = (std::time_t)m_xs[i];
    char dateBuf[32];
    {
        std::tm* tm = IsIntraday(m_timeframe) ? std::localtime(&t) : std::gmtime(&t);
        std::strftime(dateBuf, sizeof(dateBuf),
                      IsIntraday(m_timeframe) ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", tm);
    }

    double change    = m_closes[i] - m_opens[i];
    double changePct = (m_opens[i] != 0.0) ? (change / m_opens[i] * 100.0) : 0.0;
    bool   bull      = change >= 0;
    ImVec4 col       = bull ? ImVec4(0.2f, 0.9f, 0.4f, 1.f) : ImVec4(0.9f, 0.3f, 0.3f, 1.f);

    double vol = m_volumes[i];
    char   volBuf[16];
    if      (vol >= 1e6) std::snprintf(volBuf, sizeof(volBuf), "%.2fM", vol / 1e6);
    else if (vol >= 1e3) std::snprintf(volBuf, sizeof(volBuf), "%.1fK", vol / 1e3);
    else                 std::snprintf(volBuf, sizeof(volBuf), "%.0f",  vol);

    ImGui::BeginTooltip();
    ImGui::Text("%s  %s", m_symbol, dateBuf);
    ImGui::Separator();
    ImGui::Text("Open:   $%.2f", m_opens[i]);
    ImGui::Text("High:   $%.2f", m_highs[i]);
    ImGui::Text("Low:    $%.2f", m_lows[i]);
    ImGui::Text("Close:  $%.2f", m_closes[i]);
    ImGui::TextColored(col, "Change: %+.2f  (%+.2f%%)", change, changePct);
    ImGui::Text("Volume: %s", volBuf);
    if (m_ind.vwap && i < (int)m_vwap.size() && m_vwap[i] > 0.0)
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "VWAP:   $%.2f", m_vwap[i]);
    if (m_ind.sma20 && i < (int)m_sma1.size() && m_sma1[i] > 0.0)
        ImGui::TextDisabled("SMA20:  $%.2f", m_sma1[i]);
    if (m_ind.sma50 && i < (int)m_sma2.size() && m_sma2[i] > 0.0)
        ImGui::TextDisabled("SMA50:  $%.2f", m_sma2[i]);
    if (m_ind.ema20 && i < (int)m_ema.size()  && m_ema[i]  > 0.0)
        ImGui::TextDisabled("EMA20:  $%.2f", m_ema[i]);
    if (m_ind.rsi   && i < (int)m_rsi.size()  && m_rsi[i]  > 0.0)
        ImGui::TextDisabled("RSI14:  %.1f",  m_rsi[i]);
    ImGui::EndTooltip();
}

// ============================================================================
// Volume tick formatter
// ============================================================================
int ChartWindow::VolTickFormatter(double value, char* buf, int size, void* /*user_data*/) {
    if      (value >= 1e6) return std::snprintf(buf, (size_t)size, "%.2fM", value / 1e6);
    else if (value >= 1e3) return std::snprintf(buf, (size_t)size, "%.0fK", value / 1e3);
    else                   return std::snprintf(buf, (size_t)size, "%.0f",  value);
}

// ============================================================================
// X-axis tick formatter — maps bar index → date/time string
// ============================================================================
int ChartWindow::XTickFormatter(double idx, char* buf, int size, void* userData) {
    auto* self = static_cast<ChartWindow*>(userData);
    int i = (int)std::round(idx);
    if (i < 0 || i >= (int)self->m_xs.size()) {
        if (size > 0) buf[0] = '\0';
        return 0;
    }
    std::time_t t = (std::time_t)self->m_xs[i];
    if (IsIntraday(self->m_timeframe)) {
        std::tm* tm = std::localtime(&t);   // local time so market open/close matches user's clock
        if (!tm) return 0;
        return (int)std::strftime(buf, (size_t)size, "%m/%d %H:%M", tm);
    } else {
        std::tm* tm = std::gmtime(&t);      // UTC/date-only for D1+
        if (!tm) return 0;
        return (int)std::strftime(buf, (size_t)size, "%Y-%m-%d", tm);
    }
}

// ============================================================================
// DrawSessionBands — shaded background rectangles for pre/post/overnight sessions
// Called inside BeginPlot / EndPlot of the candle chart.
// ============================================================================
void ChartWindow::DrawSessionBands() {
    if (!m_showSessions || !IsIntraday(m_timeframe)) return;
    int n = (int)m_xs.size();
    if (n == 0) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    auto sessionColor = [](core::Session s) -> ImU32 {
        switch (s) {
            case core::Session::PreMarket:  return IM_COL32( 40,  80, 140, 38);
            case core::Session::AfterHours: return IM_COL32(140,  80,  30, 38);
            case core::Session::Overnight:  return IM_COL32( 80,  40, 140, 38);
            default:                        return 0;  // Regular: no shading
        }
    };

    int i = 0;
    while (i < n) {
        core::Session s   = core::BarSession((std::time_t)m_xs[i]);
        ImU32         col = sessionColor(s);

        int j = i;
        while (j + 1 < n && core::BarSession((std::time_t)m_xs[j + 1]) == s) ++j;

        if (col != 0) {
            ImVec2 tl = ImPlot::PlotToPixels(m_idxs[i] - 0.5, m_priceMax);
            ImVec2 br = ImPlot::PlotToPixels(m_idxs[j] + 0.5, m_priceMin);
            if (tl.x < br.x)   // only draw if on screen
                dl->AddRectFilled(tl, br, col);
        }
        i = j + 1;
    }

    // Session legend (top-right corner of plot)
    {
        ImVec2 plotPos  = ImPlot::GetPlotPos();
        ImVec2 plotSize = ImPlot::GetPlotSize();
        float  lx = plotPos.x + plotSize.x - 130.f;
        float  ly = plotPos.y + 6.f;
        float  lh = ImGui::GetTextLineHeight();
        struct LegEntry { const char* label; ImU32 col; };
        static constexpr LegEntry kLeg[] = {
            { "Pre-Market",  IM_COL32( 40, 120, 220, 200) },
            { "After-Hours", IM_COL32(220, 140,  40, 200) },
            { "Overnight",   IM_COL32(140,  60, 220, 200) },
        };
        for (const auto& e : kLeg) {
            dl->AddRectFilled(ImVec2(lx, ly + 2), ImVec2(lx + 10, ly + lh - 2), e.col, 2.f);
            dl->AddText(ImVec2(lx + 14, ly), IM_COL32(180, 180, 180, 200), e.label);
            ly += lh + 2.f;
        }
    }

    ImPlot::PopPlotClipRect();
}

// ============================================================================
// Volume sub-chart
// ============================================================================
void ChartWindow::DrawVolumeChart() {
    int n = (int)m_idxs.size();
    if (n == 0) return;

    float available = ImGui::GetContentRegionAvail().y;
    float rsiH      = m_ind.rsi ? available * (0.20f / (1.0f - m_volumeHeightRatio)) : 0.0f;
    float volH      = available - rsiH - (m_ind.rsi ? ImGui::GetStyle().ItemSpacing.y : 0.0f);

    double maxVol = 1.0;
    for (int i = 0; i < n; i++)
        if (m_idxs[i] >= m_xMin - 1.0 && m_idxs[i] <= m_xMax + 1.0)
            maxVol = std::max(maxVol, m_volumes[i]);

    if (!ImPlot::BeginPlot("##volume", ImVec2(-1, volH),
                           ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
        return;

    ImPlot::SetupAxes(nullptr, "Volume", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_X1, XTickFormatter, this);
    ImPlot::SetupAxisLinks(ImAxis_X1, &m_xMin, &m_xMax);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maxVol * 1.15, ImGuiCond_Always);
    ImPlot::SetupAxisFormat(ImAxis_Y1, VolTickFormatter);
    ImPlot::SetupFinish();

    static constexpr double kBarW = 0.7;
    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    for (int i = 0; i < n; i++) {
        bool  bull = m_closes[i] >= m_opens[i];
        ImU32 col  = bull ? IM_COL32(52, 211, 100, 160) : IM_COL32(220, 60, 60, 160);
        ImVec2 top = ImPlot::PlotToPixels(m_idxs[i] - kBarW * 0.5, m_volumes[i]);
        ImVec2 bot = ImPlot::PlotToPixels(m_idxs[i] + kBarW * 0.5, 0.0);
        if (top.y < bot.y) dl->AddRectFilled(top, bot, col);
    }
    ImPlot::PopPlotClipRect();
    ImPlot::EndPlot();
}

// ============================================================================
// RSI sub-chart
// ============================================================================
void ChartWindow::DrawRsiChart() {
    int n = (int)m_idxs.size();
    if (n == 0 || (int)m_rsi.size() != n) return;

    if (!ImPlot::BeginPlot("##rsi", ImVec2(-1, -1),
                           ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
        return;

    ImPlot::SetupAxes(nullptr, "RSI", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_X1, XTickFormatter, this);
    ImPlot::SetupAxisLinks(ImAxis_X1, &m_xMin, &m_xMax);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 100.0, ImGuiCond_Always);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, 100.0);
    ImPlot::SetupFinish();

    double xL = m_xMin - 1.0, xR = m_xMax + 1.0;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    {
        ImVec2 ob0 = ImPlot::PlotToPixels(xL, 70.0), ob1 = ImPlot::PlotToPixels(xR, 70.0);
        ImVec2 os0 = ImPlot::PlotToPixels(xL, 30.0), os1 = ImPlot::PlotToPixels(xR, 30.0);
        dl->AddLine(ob0, ob1, IM_COL32(220, 60,  60, 100), 1.f);
        dl->AddLine(os0, os1, IM_COL32(52, 211, 100, 100), 1.f);
        ImVec2 top0 = ImPlot::PlotToPixels(xL, 100.0);
        dl->AddRectFilled(ImVec2(top0.x, top0.y), ImVec2(ob1.x, ob0.y), IM_COL32(220,60,60,20));
        ImVec2 bot0 = ImPlot::PlotToPixels(xL, 0.0), bot1 = ImPlot::PlotToPixels(xR, 0.0);
        dl->AddRectFilled(ImVec2(os0.x, os0.y), ImVec2(bot1.x, bot0.y), IM_COL32(52,211,100,20));
    }
    ImPlot::PopPlotClipRect();

    ImPlot::SetNextLineStyle(ImVec4(0.8f, 0.6f, 1.f, 1.f), 1.5f);
    ImPlot::PlotLine("RSI14", m_idxs.data(), m_rsi.data(), n);

    if (n > 0 && m_rsi[n - 1] > 0.0) {
        double rv = m_rsi[n - 1];
        ImVec4 lc = rv > 70.0 ? ImVec4(1,.3f,.3f,1) : rv < 30.0 ? ImVec4(.3f,1,.5f,1)
                                                                  : ImVec4(.8f,.8f,.8f,1);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%.1f", rv);
        ImPlot::Annotation(m_idxs[n - 1], rv, lc, ImVec2(4, 0), false, "%s", buf);
    }
    ImPlot::EndPlot();
}

// ============================================================================
// Data management
// ============================================================================
void ChartWindow::RefreshData() {
    if (m_hasRealData) return;
    m_series = GenerateSimulatedBars(m_symbol, m_timeframe, 200);
    RebuildFlatArrays();
    ComputeIndicators();
}

void ChartWindow::RebuildFlatArrays() {
    m_xs.clear(); m_idxs.clear();
    m_opens.clear(); m_highs.clear(); m_lows.clear();
    m_closes.clear(); m_volumes.clear();

    bool intraday = IsIntraday(m_timeframe);
    int idx = 0;
    for (const auto& b : m_series.bars) {
        if (intraday) {
            auto s = core::BarSession((std::time_t)b.timestamp);
            if (s != core::Session::Regular) {
                if (m_useRTH) continue;
                if (s == core::Session::Overnight && !m_showOvernight) continue;
            }
        }
        m_xs.push_back(b.timestamp);
        m_idxs.push_back((double)idx++);
        m_opens.push_back(b.open);   m_highs.push_back(b.high);
        m_lows.push_back(b.low);     m_closes.push_back(b.close);
        m_volumes.push_back(b.volume);
    }
    // Clear drawings — they are index-relative and stale after a data reload
    m_drawings.clear();
    m_drawPending = false;
}

void ChartWindow::ComputeIndicators() {
    m_sma1 = CalcSMA(m_closes, m_ind.smaPeriod1);
    m_sma2 = CalcSMA(m_closes, m_ind.smaPeriod2);
    m_ema  = CalcEMA(m_closes, m_ind.emaPeriod);
    CalcBollingerBands(m_closes, m_ind.bbPeriod, m_ind.bbSigma,
                       m_bbMid, m_bbUpper, m_bbLower);
    m_rsi  = CalcRSI(m_closes, m_ind.rsiPeriod);
    m_vwap = CalcVWAP(m_highs, m_lows, m_closes, m_volumes, m_xs,
                      IsIntraday(m_timeframe));
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
    for (int i = period; i < n; i++) { sum += close[i] - close[i - period]; out[i] = sum / period; }
    return out;
}

std::vector<double> ChartWindow::CalcEMA(const std::vector<double>& close, int period) {
    int n = (int)close.size();
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n < period) return out;
    double k = 2.0 / (period + 1.0), ema = 0.0;
    for (int i = 0; i < period; i++) ema += close[i];
    ema /= period; out[period - 1] = ema;
    for (int i = period; i < n; i++) { ema = close[i] * k + ema * (1.0 - k); out[i] = ema; }
    return out;
}

void ChartWindow::CalcBollingerBands(const std::vector<double>& close, int period, float sigma,
                                     std::vector<double>& mid,
                                     std::vector<double>& upper,
                                     std::vector<double>& lower) {
    int n = (int)close.size();
    mid.assign(n, 0.0); upper.assign(n, 0.0); lower.assign(n, 0.0);
    if (period <= 0 || n < period) return;
    for (int i = period - 1; i < n; i++) {
        double sum = 0.0;
        for (int j = i - period + 1; j <= i; j++) sum += close[j];
        double m = sum / period, var = 0.0;
        for (int j = i - period + 1; j <= i; j++) var += (close[j] - m) * (close[j] - m);
        double sd = std::sqrt(var / period);
        mid[i] = m; upper[i] = m + sigma * sd; lower[i] = m - sigma * sd;
    }
}

std::vector<double> ChartWindow::CalcRSI(const std::vector<double>& close, int period) {
    int n = (int)close.size();
    std::vector<double> out(n, 0.0);
    if (period <= 0 || n <= period) return out;
    double ag = 0.0, al = 0.0;
    for (int i = 1; i <= period; i++) {
        double d = close[i] - close[i - 1];
        if (d > 0) ag += d; else al -= d;
    }
    ag /= period; al /= period;
    auto rsi = [](double g, double l) { return l == 0.0 ? 100.0 : 100.0 - 100.0 / (1.0 + g / l); };
    out[period] = rsi(ag, al);
    for (int i = period + 1; i < n; i++) {
        double d = close[i] - close[i - 1];
        ag = (ag * (period - 1) + (d > 0 ? d : 0.0)) / period;
        al = (al * (period - 1) + (d < 0 ? -d : 0.0)) / period;
        out[i] = rsi(ag, al);
    }
    return out;
}

std::vector<double> ChartWindow::CalcVWAP(const std::vector<double>& high,
                                           const std::vector<double>& low,
                                           const std::vector<double>& close,
                                           const std::vector<double>& volume,
                                           const std::vector<double>& timestamps,
                                           bool intradayReset) {
    int n = (int)close.size();
    std::vector<double> out(n, 0.0);
    if (n == 0) return out;
    double cumTPV = 0.0, cumVol = 0.0;
    int prevDay = -1;
    for (int i = 0; i < n; i++) {
        if (intradayReset) {
            std::time_t t = (std::time_t)timestamps[i];
            std::tm* tm   = std::gmtime(&t);
            int day       = tm->tm_year * 366 + tm->tm_yday;
            if (day != prevDay) { cumTPV = 0.0; cumVol = 0.0; prevDay = day; }
        }
        double tp = (high[i] + low[i] + close[i]) / 3.0;
        cumTPV   += tp * volume[i];
        cumVol   += volume[i];
        out[i]    = (cumVol > 0.0) ? (cumTPV / cumVol) : close[i];
    }
    return out;
}

// ============================================================================
// Simulated data generator
// ============================================================================
core::BarSeries ChartWindow::GenerateSimulatedBars(const std::string& symbol,
                                                    core::Timeframe tf, int count) {
    std::size_t seed = std::hash<std::string>{}(symbol);
    std::mt19937 rng((unsigned)seed);
    std::normal_distribution<double> dist(0.0, 1.0);

    struct SymConfig { double price, vol, drift, avgVol; };
    auto cfg = [&]() -> SymConfig {
        if (symbol == "AAPL")  return {185.0, 0.015, 0.0003, 55e6};
        if (symbol == "MSFT")  return {415.0, 0.013, 0.0004, 25e6};
        if (symbol == "GOOGL") return {175.0, 0.016, 0.0003, 20e6};
        if (symbol == "TSLA")  return {250.0, 0.030, 0.0002, 90e6};
        if (symbol == "SPY")   return {520.0, 0.008, 0.0002, 80e6};
        return {100.0, 0.020, 0.0002, 10e6};
    }();

    int64_t tfSec = core::TimeframeSeconds(tf);
    std::time_t now = (std::time(nullptr) / tfSec) * tfSec;
    int64_t startTs = now - (int64_t)count * tfSec;

    core::BarSeries series;
    series.symbol = symbol; series.timeframe = tf; series.bars.reserve(count);
    double price = cfg.price;

    for (int i = 0; i < count; i++) {
        double ts = (double)(startTs + (int64_t)i * tfSec);
        if (tf == core::Timeframe::D1) {
            std::time_t t = (std::time_t)ts;
            std::tm* gm   = std::gmtime(&t);
            if (gm && (gm->tm_wday == 0 || gm->tm_wday == 6)) {
                price *= std::exp(cfg.drift + cfg.vol * dist(rng) * 0.3);
                continue;
            }
        }
        double ret   = cfg.drift + cfg.vol * dist(rng);
        double open  = price, close = price * std::exp(ret);
        double high  = std::max(open, close) * std::exp(std::abs(dist(rng) * cfg.vol * 0.5));
        double low   = std::min(open, close) * std::exp(-std::abs(dist(rng) * cfg.vol * 0.5));
        double vol   = cfg.avgVol * std::exp(0.4 * dist(rng));
        series.bars.push_back({ts, open, high, low, close, vol});
        price = close;
    }
    return series;
}

}  // namespace ui
