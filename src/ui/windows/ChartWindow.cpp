#include "ui/UiScale.h"
#include "ui/windows/ChartWindow.h"
#include "ui/SymbolSearch.h"

#include "imgui.h"
#include "core/models/WindowGroup.h"
#include "implot.h"

#include <cmath>
#include <limits>
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

// ============================================================================
// Order type table — shared by DrawTradePanel and DrawOverlays
// ============================================================================
struct OrderTypeDef {
    const char*     label;
    const char*     ibStr;        // IB order-type string (also used in PendingOrderLine)
    core::OrderType coreType;
    bool needsPrice;   // arms chart-click price placement
    bool needsTrail;   // show trail $/% input
    bool isDualPrice;  // two chart clicks needed (first line + second line)
    bool firstIsAux;   // first click → auxPrice/trigger (true for LIT), else stop/price
    bool tifLocked;    // lock TIF combo to DAY (MOC / LOC)
    bool noRth;        // disable outside RTH option (MOC / LOC)
};
//                                                                       nP     nT     dP     fA     tL     nR
static constexpr OrderTypeDef kOrderTypes[] = {
    { "Market",          "MKT",        core::OrderType::Market,   false, false, false, false, false, false },
    { "Limit",           "LMT",        core::OrderType::Limit,    true,  false, false, false, false, false },
    { "Stop",            "STP",        core::OrderType::Stop,     true,  false, false, false, false, false },
    { "Stop Limit",      "STP LMT",    core::OrderType::StopLimit,true,  false, true,  false, false, false },
    { "Trail Stop",      "TRAIL",      core::OrderType::Trail,    false, true,  false, false, false, false },
    { "Trail Limit",     "TRAIL LIMIT",core::OrderType::TrailLimit,false, true,  false, false, false, false },
    { "Market On Close", "MOC",        core::OrderType::MOC,      false, false, false, false, true,  true  },
    { "Limit On Close",  "LOC",        core::OrderType::LOC,      true,  false, false, false, true,  true  },
    { "Market to Limit", "MTL",        core::OrderType::MTL,      false, false, false, false, false, false },
    { "Mkt If Touched",  "MIT",        core::OrderType::MIT,      true,  false, false, false, false, false },
    { "Lmt If Touched",  "LIT",        core::OrderType::LIT,      true,  false, true,  true,  false, false },
    { "Midprice",        "MIDPRICE",   core::OrderType::Midprice, false, false, false, false, false, false },
    { "Relative",        "REL",        core::OrderType::Relative, false, false, false, false, false, false },
};
static constexpr int kNumOrderTypes = (int)std::size(kOrderTypes);

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
// setInstanceId / SetSymbol / AddBar / SetHistoricalData
// ============================================================================
void ChartWindow::setInstanceId(int id) {
    m_instanceId = id;
    std::snprintf(m_title, sizeof(m_title), "Chart %d##chart%d", id, id);
}

void ChartWindow::SetSymbol(const std::string& symbol) {
    if (symbol.empty() || symbol.size() >= sizeof(m_symbol)) return;
    std::memcpy(m_symbol, symbol.c_str(), symbol.size() + 1);
    m_viewInitialized = false;
    m_loadingMore     = false;
    m_historyAtStart  = false;
    // Reset drawing / order state so NoInputs is never left armed on the new symbol
    m_drawTool    = DrawTool::Cursor;
    m_drawPending = false;
    m_limitArmed        = false;
    m_showConfirmPopup  = false;
    m_limitPlaced       = false;
    m_placedDragging = false;
    m_dragPendingIdx    = -1;
    m_dragPendingActive = false;
    m_dragPendingIsAux  = false;
    m_firstPricePlaced  = false;
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
    m_loadingMore     = false;
    m_historyAtStart  = false;
    m_viewInitialized = false;
    RebuildFlatArrays();
    ComputeIndicators();
}

void ChartWindow::PrependHistoricalData(const core::BarSeries& older) {
    m_loadingMore = false;
    if (older.bars.empty()) {
        m_historyAtStart = true;   // IB returned nothing — we're at the oldest data
        return;
    }

    // Only keep bars strictly older than our current first bar to avoid duplicates
    double firstTs = m_xs.empty() ? 1e18 : m_xs[0];
    core::BarSeries merged;
    merged.symbol    = m_series.symbol;
    merged.timeframe = m_series.timeframe;
    for (const auto& b : older.bars)
        if (b.timestamp < firstTs) merged.bars.push_back(b);

    if (merged.bars.empty()) {
        m_historyAtStart = true;
        return;
    }

    int prependCount = (int)merged.bars.size();

    // Append existing bars after the older ones
    for (const auto& b : m_series.bars) merged.bars.push_back(b);
    m_series = std::move(merged);

    RebuildFlatArrays();
    ComputeIndicators();

    // Shift the X axis links right so the visible window stays on the same candles
    m_xMin += prependCount;
    m_xMax += prependCount;
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
    // Use UTC for date comparisons so the result agrees with ParseIBTime's noon-UTC
    // convention and with XTickFormatter which uses gmtime() for D1+ labels.
    std::time_t now = std::time(nullptr);
    struct tm nowTm = *std::gmtime(&now);

    // No bars on weekends
    if (nowTm.tm_wday == 0 || nowTm.tm_wday == 6) return;

    // Check if today's bar is already the last bar
    if (!m_xs.empty()) {
        std::time_t lastTs = static_cast<std::time_t>(m_xs.back());
        struct tm lastTm = *std::gmtime(&lastTs);
        if (nowTm.tm_year == lastTm.tm_year && nowTm.tm_yday == lastTm.tm_yday)
            return;  // already have today
    }

    // Build today's bar at noon UTC — same convention as ParseIBTime("YYYYMMDD").
    struct tm noon = nowTm;
    noon.tm_hour = 12; noon.tm_min = noon.tm_sec = 0;
    noon.tm_isdst = 0;
#ifdef _WIN32
    auto todayTs = static_cast<double>(_mkgmtime(&noon));
#else
    auto todayTs = static_cast<double>(timegm(&noon));
#endif

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

void ChartWindow::OnWshEvent(const WshData::WshEvent& event) {
    for (const auto& e : m_wshEvents)
        if (e.date == event.date && e.type == event.type) return;
    m_wshEvents.push_back(event);
}

void ChartWindow::RequestNewData() {
    m_series = core::BarSeries{};
    m_xs.clear(); m_opens.clear(); m_highs.clear();
    m_lows.clear(); m_closes.clear(); m_volumes.clear();
    m_vwap.clear();
    m_loadingMore    = false;
    m_historyAtStart = false;

    if (OnDataRequest) {
        m_hasRealData  = false;
        m_loading      = true;
        m_needsRefresh = true;  // fallback: simulated data shows if IB never responds
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
    // Enforce a minimum height so Volume / RSI sub-plots are never clipped out of view.
    float minH = 460.0f;
    if (m_ind.volume) minH += 90.0f;
    if (m_ind.rsi)    minH += 90.0f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(500.0f, minH), ImVec2(FLT_MAX, FLT_MAX));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                           | ImGuiWindowFlags_NoFocusOnAppearing;
    char grp[8];
    if (m_groupId > 0) std::snprintf(grp, sizeof(grp), "G%d", m_groupId);
    else                std::strncpy(grp, "G-", sizeof(grp));
    char title[80];
    std::snprintf(title, sizeof(title), "Chart %s %s###chart%d",
        m_symbol[0] == '\0' ? "--" : m_symbol, grp, m_instanceId);
    if (!ImGui::Begin(title, &m_open, flags)) {
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

    DrawConfirmPopup();

    ImGui::End();
    return m_open;
}

// ============================================================================
// Toolbar row 1 — symbol, timeframe, zoom, indicators
// ============================================================================
void ChartWindow::DrawToolbar() {
    FlexRow row;

    // Group picker
    row.item(FlexRow::buttonW("G1"), 0);
    core::DrawGroupPicker(m_groupId, "##chart_grp");

    // Symbol input with live IB autocomplete
    row.item(em(80), 8);
    DrawSymbolInput("##sym", m_symbol, sizeof(m_symbol), em(80),
                    [this](const std::string& sym) {
                        std::strncpy(m_symbol, sym.c_str(), sizeof(m_symbol) - 1);
                        m_symbol[sizeof(m_symbol) - 1] = '\0';
                        m_viewInitialized = false;
                        AddToHistory(m_symbol);
                        RequestNewData();
                    });

    // History dropdown button
    row.item(FlexRow::buttonW("v"), 2);
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

    // Quick symbol buttons
    static constexpr const char* kQuickSyms[] = {"AAPL", "MSFT", "GOOGL", "TSLA", "SPY"};
    for (const char* s : kQuickSyms) {
        row.item(FlexRow::buttonW(s), 4);
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

    // Timeframe combo
    row.item(em(55), 16);
    ImGui::SetNextItemWidth(em(55));
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
    row.item(FlexRow::buttonW("[+]"), 8);
    if (ImGui::SmallButton("[+]") && m_viewInitialized) {
        double center = (m_xMin + m_xMax) * 0.5;
        double half   = (m_xMax - m_xMin) * 0.5 * 0.75;
        m_xMin = center - half;
        m_xMax = center + half;
    }
    row.item(FlexRow::buttonW("[-]"), 2);
    if (ImGui::SmallButton("[-]") && m_viewInitialized) {
        double center = (m_xMin + m_xMax) * 0.5;
        double half   = (m_xMax - m_xMin) * 0.5 * 1.333;
        m_xMin = center - half;
        m_xMax = center + half;
    }

    // Extended hours toggles (intraday only)
    if (IsIntraday(m_timeframe)) {
        row.item(FlexRow::checkboxW("Ext.Hrs"), 16);
        bool extHours = !m_useRTH;
        if (ImGui::Checkbox("Ext.Hrs", &extHours)) {
            m_useRTH = !extHours;
            if (m_useRTH) m_showOvernight = false;  // reset when ext hours disabled
            RequestNewData();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Include pre-market & after-hours bars");

        if (!m_useRTH) {
            row.item(FlexRow::checkboxW("Overnight"), 4);
            if (ImGui::Checkbox("Overnight", &m_showOvernight)) {
                RebuildFlatArrays();
                ComputeIndicators();
                m_viewInitialized = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Include overnight bars (00:00-04:00 ET)");
        }

        row.item(FlexRow::checkboxW("Sessions"), 4);
        ImGui::Checkbox("Sessions", &m_showSessions);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Highlight trading session backgrounds");
    }

    // Indicator checkboxes
    row.item(FlexRow::checkboxW("SMA20"), 16);
    ImGui::Checkbox("SMA20",  &m_ind.sma20);
    row.item(FlexRow::checkboxW("SMA50"));
    ImGui::Checkbox("SMA50",  &m_ind.sma50);
    row.item(FlexRow::checkboxW("EMA20"));
    ImGui::Checkbox("EMA20",  &m_ind.ema20);
    row.item(FlexRow::checkboxW("BB"));
    ImGui::Checkbox("BB",     &m_ind.bbands);
    row.item(FlexRow::checkboxW("VWAP"));
    ImGui::Checkbox("VWAP",   &m_ind.vwap);
    row.item(FlexRow::checkboxW("Vol"));
    ImGui::Checkbox("Vol",    &m_ind.volume);
    row.item(FlexRow::checkboxW("RSI"));
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

    FlexRow row;
    row.item(FlexRow::textW("Analysis:"), 0);
    ImGui::TextDisabled("Analysis:");
    for (const auto& e : kTools) {
        row.item(FlexRow::buttonW(e.label), 4);
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

    row.item(FlexRow::buttonW("Clear All"), 12);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.15f, 0.15f, 1.f));
    if (ImGui::SmallButton("Clear All")) {
        m_drawings.clear();
        m_drawPending = false;
        m_drawTool    = DrawTool::Cursor;
    }
    ImGui::PopStyleColor();

    if (m_drawPending) {
        row.item(FlexRow::textW("(click second point...)"), 16);
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "(click second point...)");
    }
}

// ============================================================================
// Toolbar row 3 — trade panel
// ============================================================================
void ChartWindow::DrawTradePanel() {
    // kOrderTypes, kNumOrderTypes defined at file scope above.

    static constexpr const char* kSessions[] = {
        "Regular", "Pre-Market", "After Hours", "Overnight"
    };
    static constexpr const char* kTIFs[] = { "DAY", "GTC", "GTD", "OPG", "OVERNIGHT" };

    ImGui::Spacing();

    FlexRow row;
    const float kBtnW = em(56);  // BUY / SELL button width

    // ── Qty ─────────────────────────────────────────────────────────────────
    row.item(FlexRow::textW("Trade:"), 0);
    ImGui::TextDisabled("Trade:");
    row.item(em(62), 6);
    ImGui::SetNextItemWidth(em(62));
    ImGui::InputInt("Qty##ord", &m_orderQty, 0, 0);
    if (m_orderQty < 1) m_orderQty = 1;

    // ── Order type ──────────────────────────────────────────────────────────
    row.item(em(170), 8);
    ImGui::SetNextItemWidth(em(170));
    if (ImGui::BeginCombo("##otype", kOrderTypes[m_orderTypeIdx].label)) {
        for (int i = 0; i < kNumOrderTypes; i++) {
            bool sel = (i == m_orderTypeIdx);
            if (ImGui::Selectable(kOrderTypes[i].label, sel)) {
                m_orderTypeIdx     = i;
                m_limitArmed       = false;
                m_firstPricePlaced = false;
                m_limitPlaced      = false;
                m_placedDragging   = false;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ── Trail amount (Trail Stop / Trail Limit) ───────────────────────────
    if (kOrderTypes[m_orderTypeIdx].needsTrail) {
        // $/% toggle button
        row.item(em(28), 6);
        if (ImGui::Button(m_trailByPct ? "%##tpct" : "$##tpct", ImVec2(em(28), 0)))
            m_trailByPct = !m_trailByPct;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle trail by $$ / %%");

        if (m_trailByPct) {
            row.item(FlexRow::textW("Trail %:"), 4);
            ImGui::TextDisabled("Trail %%:");
            row.item(em(60), 4);
            ImGui::SetNextItemWidth(em(60));
            ImGui::InputDouble("##trailpct", &m_trailPercent, 0.0, 0.0, "%.2f");
            if (m_trailPercent <= 0.0) m_trailPercent = 0.1;
        } else {
            row.item(FlexRow::textW("Trail $:"), 4);
            ImGui::TextDisabled("Trail $:");
            row.item(em(60), 4);
            ImGui::SetNextItemWidth(em(60));
            ImGui::InputDouble("##trail", &m_trailAmount, 0.0, 0.0, "%.2f");
            if (m_trailAmount <= 0.0) m_trailAmount = 0.01;
        }

        // Lmt offset (Trail Limit only)
        if (kOrderTypes[m_orderTypeIdx].coreType == core::OrderType::TrailLimit) {
            row.item(FlexRow::textW("Lmt Off:"), 8);
            ImGui::TextDisabled("Lmt Off:");
            row.item(em(60), 4);
            ImGui::SetNextItemWidth(em(60));
            ImGui::InputDouble("##lmtoff", &m_limitOffset, 0.0, 0.0, "%.2f");
        }

        // Optional stop cap (both Trail Stop and Trail Limit)
        row.item(FlexRow::textW("Stop Cap:"), 8);
        ImGui::TextDisabled("Stop Cap:");
        row.item(em(68), 4);
        ImGui::SetNextItemWidth(em(68));
        ImGui::InputDouble("##trailstp", &m_trailStopPrice, 0.0, 0.0, "%.2f");
        if (m_trailStopPrice < 0.0) m_trailStopPrice = 0.0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Initial stop price cap (0 = let IB compute)");
    }

    // ── Peg offset (Relative orders) ─────────────────────────────────────
    if (kOrderTypes[m_orderTypeIdx].coreType == core::OrderType::Relative) {
        row.item(FlexRow::textW("Offset:"), 6);
        ImGui::TextDisabled("Offset:");
        row.item(em(60), 4);
        ImGui::SetNextItemWidth(em(60));
        ImGui::InputDouble("##pegoff", &m_pegOffset, 0.0, 0.0, "%.2f");
        if (m_pegOffset <= 0.0) m_pegOffset = 0.01;
    }

    // ── Session ─────────────────────────────────────────────────────────────
    bool rthDisabled = kOrderTypes[m_orderTypeIdx].noRth;
    row.item(em(95), 10);
    ImGui::SetNextItemWidth(em(95));
    if (rthDisabled) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##sess", kSessions[m_sessionIdx])) {
        for (int i = 0; i < 4; i++) {
            bool sel = (i == m_sessionIdx);
            if (ImGui::Selectable(kSessions[i], sel)) m_sessionIdx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (rthDisabled) ImGui::EndDisabled();

    // ── TIF ─────────────────────────────────────────────────────────────────
    bool tifLocked = kOrderTypes[m_orderTypeIdx].tifLocked;
    row.item(em(52), 6);
    ImGui::SetNextItemWidth(em(52));
    if (tifLocked) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##tif", tifLocked ? "DAY" : kTIFs[m_tifIdx])) {
        for (int i = 0; i < 3; i++) {
            bool sel = (i == m_tifIdx);
            if (ImGui::Selectable(kTIFs[i], sel)) m_tifIdx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (tifLocked) ImGui::EndDisabled();

    // ── BUY / SELL buttons ────────────────────────────────────────────────
    row.item(kBtnW, 14);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.08f, 0.52f, 0.08f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.72f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.04f, 0.38f, 0.04f, 1.f));
    bool buyClicked = ImGui::Button("  BUY  ##ord", ImVec2(kBtnW, 0));
    ImGui::PopStyleColor(3);

    row.item(kBtnW, 4);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.52f, 0.08f, 0.08f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.04f, 0.04f, 1.f));
    bool sellClicked = ImGui::Button(" SELL  ##ord", ImVec2(kBtnW, 0));
    ImGui::PopStyleColor(3);

    // ── Status hint when armed ───────────────────────────────────────────────
    if (m_limitArmed) {
        ImVec4 hintCol = (m_limitSide == "BUY")
                         ? ImVec4(0.4f, 0.7f, 1.f, 1.f)
                         : ImVec4(1.f, 0.4f, 0.4f, 1.f);
        const char* hint = m_firstPricePlaced
            ? (kOrderTypes[m_orderTypeIdx].firstIsAux
                ? "Trigger set — click chart for limit price | Esc=cancel"
                : "Stop set — click chart for limit price | Esc=cancel")
            : kOrderTypes[m_orderTypeIdx].isDualPrice
                ? (kOrderTypes[m_orderTypeIdx].firstIsAux
                    ? "Click chart to set TRIGGER price | Esc=cancel"
                    : "Click chart to set STOP price | Esc=cancel")
                : m_transmitInstantly
                    ? "Click to send | Ctrl+click for confirmation | Esc=cancel"
                    : "Click to preview & confirm | Esc=cancel";
        row.item(FlexRow::textW(hint), 12);
        ImGui::TextColored(hintCol, "%s", hint);
    }

    // ── Transmit Instantly checkbox ─────────────────────────────────────────
    row.item(FlexRow::checkboxW("Transmit Instantly"), 16);
    ImGui::Checkbox("Transmit Instantly", &m_transmitInstantly);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When ON: orders fire immediately on chart click.\n"
                          "When OFF: a confirmation popup shows full order\n"
                          "details before sending.");

    // ── Button logic ────────────────────────────────────────────────────────
    auto buildOrder = [&](const std::string& side) -> core::Order {
        const auto& ot = kOrderTypes[m_orderTypeIdx];
        static constexpr core::TimeInForce kTIFEnum[] = {
            core::TimeInForce::Day, core::TimeInForce::GTC, core::TimeInForce::GTC,
            core::TimeInForce::OPG, core::TimeInForce::Overnight
        };
        core::Order o;
        o.symbol     = m_symbol;
        o.side       = (side == "BUY") ? core::OrderSide::Buy : core::OrderSide::Sell;
        o.type       = ot.coreType;
        o.quantity   = static_cast<double>(m_orderQty);
        o.tif        = ot.tifLocked ? core::TimeInForce::Day : kTIFEnum[m_tifIdx];
        o.outsideRth = !ot.noRth && (m_sessionIdx != 0);
        switch (ot.coreType) {
            case core::OrderType::Trail:
                if (m_trailByPct) o.trailingPercent = m_trailPercent;
                else              o.auxPrice         = m_trailAmount;
                if (m_trailStopPrice > 0.0) o.trailStopPrice = m_trailStopPrice;
                break;
            case core::OrderType::TrailLimit:
                if (m_trailByPct) o.trailingPercent = m_trailPercent;
                else              o.auxPrice         = m_trailAmount;
                o.lmtPriceOffset = m_limitOffset;
                if (m_trailStopPrice > 0.0) o.trailStopPrice = m_trailStopPrice;
                break;
            case core::OrderType::Relative:
                o.auxPrice = m_pegOffset;
                break;
            default: break;
        }
        return o;
    };

    auto fireOrder = [&](const std::string& side) {
        const auto& ot = kOrderTypes[m_orderTypeIdx];
        if (!ot.needsPrice) {
            // MKT / MTL / MOC / Trail / Midprice / REL — no chart click needed
            core::Order o = buildOrder(side);
            if (m_transmitInstantly) {
                if (OnOrderSubmit) OnOrderSubmit(o);
            } else {
                m_pendingConfirmOrder = o;
                m_showConfirmPopup    = true;
            }
        } else {
            // Arm chart-click placement mode (LMT, STP, STP LMT, LOC, MIT, LIT)
            m_limitArmed = true;
            m_limitSide  = side;
        }
    };

    if (buyClicked)  fireOrder("BUY");
    if (sellClicked) fireOrder("SELL");

    // Escape cancels armed state (both phases)
    if (m_limitArmed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_limitArmed       = false;
        m_firstPricePlaced = false;
    }
}

// ============================================================================
// Order confirmation popup
// ============================================================================
void ChartWindow::DrawConfirmPopup() {
    if (m_showConfirmPopup) {
        ImGui::OpenPopup("##chartconfirm");
        m_showConfirmPopup = false;
    }

    // Centre over this window's viewport (works whether docked or floating)
    ImVec2 center = ImGui::GetWindowViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em(300), 0), ImGuiCond_Always);

    if (!ImGui::BeginPopupModal("##chartconfirm", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoTitleBar)) return;

    core::Order& o = m_pendingConfirmOrder;
    bool isBuy = (o.side == core::OrderSide::Buy);

    // ── Title ────────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text,
        isBuy ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
              : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::Text("  %s ORDER", isBuy ? "BUY" : "SELL");
    ImGui::PopStyleColor();
    ImGui::Separator();

    // ── Order fields (read-only) ─────────────────────────────────────────────
    ImGui::Text("Symbol:     %s",  o.symbol.c_str());
    ImGui::Text("Type:       %s",  core::OrderTypeStr(o.type));
    ImGui::Text("Quantity:   %.0f shares", o.quantity);

    ImGui::Spacing();

    // Per-type price display
    switch (o.type) {
        case core::OrderType::Limit:
        case core::OrderType::LOC:
            ImGui::Text("Limit:      $%.4f", o.limitPrice);
            break;

        case core::OrderType::Stop:
            ImGui::Text("Stop:       $%.4f", o.stopPrice);
            break;

        case core::OrderType::StopLimit:
            ImGui::Text("Stop:       $%.4f", o.stopPrice);
            ImGui::Text("Limit:      $%.4f", o.limitPrice);
            break;

        case core::OrderType::Trail:
            if (o.trailingPercent > 0.0)
                ImGui::Text("Trail %%:    %.2f%%", o.trailingPercent);
            else
                ImGui::Text("Trail $:    $%.4f", o.auxPrice);
            if (o.trailStopPrice > 0.0)
                ImGui::Text("Stop Cap:   $%.4f", o.trailStopPrice);
            else
                ImGui::TextDisabled("Stop cap:   (IB computes initial stop)");
            break;

        case core::OrderType::TrailLimit:
            if (o.trailingPercent > 0.0)
                ImGui::Text("Trail %%:    %.2f%%", o.trailingPercent);
            else
                ImGui::Text("Trail $:    $%.4f", o.auxPrice);
            ImGui::Text("Lmt Offset: $%.4f", o.lmtPriceOffset);
            if (o.trailStopPrice > 0.0)
                ImGui::Text("Stop Cap:   $%.4f", o.trailStopPrice);
            else
                ImGui::TextDisabled("Stop cap:   (IB computes initial stop)");
            break;

        case core::OrderType::MIT:
            ImGui::Text("Trigger:    $%.4f", o.auxPrice);
            break;

        case core::OrderType::LIT:
            ImGui::Text("Trigger:    $%.4f", o.auxPrice);
            ImGui::Text("Limit:      $%.4f", o.limitPrice);
            break;

        case core::OrderType::Midprice:
            if (o.limitPrice > 0.0)
                ImGui::Text("Price Cap:  $%.4f", o.limitPrice);
            else
                ImGui::TextDisabled("Price:      midpoint (no cap)");
            break;

        case core::OrderType::Relative:
            ImGui::Text("Peg Offset: $%.4f", o.auxPrice);
            if (o.limitPrice > 0.0)
                ImGui::Text("Price Cap:  $%.4f", o.limitPrice);
            break;

        case core::OrderType::Market:
        case core::OrderType::MOC:
        case core::OrderType::MTL:
            ImGui::TextDisabled("Price:      market (no limit)");
            break;

        default: break;
    }

    ImGui::Spacing();

    // TIF + outside RTH
    ImGui::Text("TIF:        %s", core::TIFStr(o.tif));
    if (o.outsideRth) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.20f, 1.0f));
        ImGui::TextUnformatted("Outside RTH: YES");
        ImGui::PopStyleColor();
    }

    // Estimated value
    {
        double refP = 0.0;
        switch (o.type) {
            case core::OrderType::Limit:
            case core::OrderType::LOC:
            case core::OrderType::LIT:       refP = o.limitPrice; break;
            case core::OrderType::Stop:
            case core::OrderType::StopLimit: refP = o.stopPrice;  break;
            case core::OrderType::MIT:       refP = o.auxPrice;   break;
            default:
                if (!m_closes.empty()) refP = m_closes.back();
                break;
        }
        if (refP > 0.0) {
            ImGui::Spacing();
            ImGui::Text("Est. value: $%.2f", o.quantity * refP);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Confirm / Cancel ─────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Button, isBuy
        ? ImVec4(0.12f, 0.55f, 0.25f, 1.0f)
        : ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isBuy
        ? ImVec4(0.18f, 0.70f, 0.35f, 1.0f)
        : ImVec4(0.80f, 0.22f, 0.22f, 1.0f));
    if (ImGui::Button("Confirm##cpok", ImVec2(em(130), 0))) {
        if (OnOrderSubmit) OnOrderSubmit(o);
        m_limitArmed       = false;
        m_firstPricePlaced = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine();

    if (ImGui::Button("Cancel##cpcancel", ImVec2(em(130), 0))) {
        m_limitArmed       = false;
        m_firstPricePlaced = false;
        ImGui::CloseCurrentPopup();
    }

    // Escape key also cancels
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_limitArmed       = false;
        m_firstPricePlaced = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
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

    FlexRow row;

    char symDateBuf[64];
    std::snprintf(symDateBuf, sizeof(symDateBuf), "%s  [%s]", m_symbol, dateBuf);
    row.item(FlexRow::textW(symDateBuf), 0);
    ImGui::TextDisabled("%s", symDateBuf);

    char ohlcBuf[64];
    std::snprintf(ohlcBuf, sizeof(ohlcBuf), "O:%.2f  H:%.2f  L:%.2f  C:%.2f",
                  m_opens[idx], m_highs[idx], m_lows[idx], m_closes[idx]);
    row.item(FlexRow::textW(ohlcBuf), 12);
    ImGui::TextDisabled("%s", ohlcBuf);

    char chgBuf[32];
    std::snprintf(chgBuf, sizeof(chgBuf), "%+.2f  (%+.2f%%)", change, changePct);
    row.item(FlexRow::textW(chgBuf), 12);
    ImGui::TextColored(chgCol, "%s", chgBuf);

    if (m_ind.vwap && idx < (int)m_vwap.size() && m_vwap[idx] > 0.0) {
        char vwapBuf[24];
        std::snprintf(vwapBuf, sizeof(vwapBuf), "VWAP:%.2f", m_vwap[idx]);
        row.item(FlexRow::textW(vwapBuf), 12);
        ImGui::TextColored(ImVec4(1.f, 1.f, 1.f, 1.f), "%s", vwapBuf);
    }

    if (m_position.hasPosition && std::abs(m_position.qty) >= 1e-9) {
        double netPnL = m_position.unrealPnL - m_position.commission;
        ImVec4 pnlCol = netPnL >= 0.0
            ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
            : ImVec4(0.90f, 0.28f, 0.28f, 1.f);
        char pnlBuf[24];
        std::snprintf(pnlBuf, sizeof(pnlBuf), "PnL:%+.2f", netPnL);
        row.item(FlexRow::textW(pnlBuf), 18);
        ImGui::TextColored(pnlCol, "%s", pnlBuf);

        char commBuf[28];
        std::snprintf(commBuf, sizeof(commBuf), "(comm -%.2f)", m_position.commission);
        row.item(FlexRow::textW(commBuf), 6);
        ImGui::TextDisabled("%s", commBuf);
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

    // ── Position break-even line ──────────────────────────────────────────────
    // Shows the price at which net P&L = 0 after the commissions already paid.
    // For long: be = avgCost + commission/qty.  For short: commission/qty is negative.
    if (m_position.hasPosition && std::abs(m_position.qty) > 1e-9 &&
        m_position.avgCost > 0.0) {
        double qty  = m_position.qty;
        double comm = m_position.commission;
        double be   = m_position.avgCost + (comm > 1e-9 ? comm / qty : 0.0);

        static constexpr ImU32 kBeCol = IM_COL32(255, 215,  50, 220);
        static constexpr ImU32 kBeBg  = IM_COL32( 70,  55,   5, 235);

        ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, be);
        ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, be);
        DrawDashedHLine(dl, lp0.x, lp1.x, lp0.y, kBeCol, 1.5f, 5.f, 3.f);

        // Centre label
        char beBuf[80];
        if (comm > 1e-9)
            std::snprintf(beBuf, sizeof(beBuf),
                          " B/E  $%.2f   (net 0 incl. $%.2f comm) ", be, comm);
        else
            std::snprintf(beBuf, sizeof(beBuf), " B/E  $%.2f ", be);
        ImVec2 beSz = ImGui::CalcTextSize(beBuf);
        float  beX  = lp0.x + 20.f;
        dl->AddRectFilled(ImVec2(beX - 2, lp0.y - 8),
                          ImVec2(beX + beSz.x + 2, lp0.y + 8), kBeBg, 2.f);
        dl->AddText(ImVec2(beX, lp0.y - 7), kBeCol, beBuf);

        // Right-edge price tag
        char edgeBuf[24];
        std::snprintf(edgeBuf, sizeof(edgeBuf), " B/E %.2f ", be);
        ImVec2 eSz = ImGui::CalcTextSize(edgeBuf);
        dl->AddRectFilled(ImVec2(lp1.x, lp0.y - 9),
                          ImVec2(lp1.x + eSz.x + 4, lp0.y + 9), kBeBg, 2.f);
        dl->AddText(ImVec2(lp1.x + 2, lp0.y - 7), kBeCol, edgeBuf);
    }

    // ── Current price line ────────────────────────────────────────────────────
    if (!m_closes.empty()) {
        double curPrice = m_closes.back();
        static constexpr ImU32 kCurCol = IM_COL32(200, 200, 200, 200);
        static constexpr ImU32 kCurBg  = IM_COL32( 45,  45,  45, 230);

        float lineY = ImPlot::PlotToPixels(m_xMin, curPrice).y;
        DrawDashedHLine(dl, pMin.x, pMax.x, lineY, kCurCol, 1.0f, 4.f, 3.f);

        // Right-aligned price tag — stays inside the plot clip rect, flush to the right edge.
        char curBuf[24];
        std::snprintf(curBuf, sizeof(curBuf), " %.2f ", curPrice);
        ImVec2 curSz = ImGui::CalcTextSize(curBuf);
        float  tagX  = pMax.x - curSz.x - 2.f;
        dl->AddRectFilled(ImVec2(tagX - 2,        lineY - 9),
                          ImVec2(tagX + curSz.x + 2, lineY + 9), kCurBg, 2.f);
        dl->AddText(ImVec2(tagX, lineY - 7), kCurCol, curBuf);
    }

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
    if (hovered && !m_limitArmed && !m_dragPendingActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
    // Clear stale drag state if the dragged order was removed mid-frame
    // (e.g. cancel fired during rendering and SetPendingOrders() shrank the vector).
    if (m_dragPendingActive &&
        m_dragPendingIdx >= (int)m_pendingOrders.size()) {
        m_dragPendingActive = false;
        m_dragPendingIdx    = -1;
        m_dragPendingIsAux  = false;
    }

    // Cancel drag if Escape pressed
    if (m_dragPendingActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_dragPendingActive = false;
        m_dragPendingIdx    = -1;
        m_dragPendingIsAux  = false;
    }

    // Helper: draw one draggable price line for a pending order leg
    // Returns true when drag commits (caller updates the order).
    // isAux=false → main (stop/trigger) leg; isAux=true → aux (limit) leg.
    auto drawOrderLeg = [&](int oi, bool isAux) {
        auto& order = m_pendingOrders[oi];
        double legPrice = isAux ? order.auxPrice : order.price;
        if (legPrice <= 0.0) return;

        bool isDragging = m_dragPendingActive && m_dragPendingIdx == oi
                          && m_dragPendingIsAux == isAux;
        double drawPrice = isDragging ? m_dragPendingPrice : legPrice;

        // Color: main leg uses side color; aux (limit) leg uses orange
        ImU32 lineCol, txtCol, lblBg;
        if (isAux) {
            lineCol = isDragging ? IM_COL32(255, 180,  50, 255) : IM_COL32(220, 140,  30, 210);
            txtCol  = IM_COL32(255, 230, 160, 255);
            lblBg   = IM_COL32(100,  55,   5, 255);
        } else {
            lineCol = order.isBuy
                      ? (isDragging ? IM_COL32(100,190,255,255) : IM_COL32( 60,140,255,200))
                      : (isDragging ? IM_COL32(255,130,100,255) : IM_COL32(255, 80, 80,200));
            txtCol  = order.isBuy ? IM_COL32(190,230,255,255) : IM_COL32(255,190,160,255);
            lblBg   = order.isBuy ? IM_COL32(10,45,110,255)   : IM_COL32(110,20, 20,255);
        }

        ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, drawPrice);
        ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, drawPrice);

        // ── Position-aware P&L helper ─────────────────────────────────────────
        // Returns net P&L (gross minus entry commission) when this order closes the
        // current position at the given price; returns NaN when not applicable.
        auto calcOrderPnL = [&](double price) -> double {
            if (isAux) return std::nan("");            // aux leg: P&L on main leg
            if (!m_position.hasPosition) return std::nan("");
            double posQty = m_position.qty;
            bool closingLong  = !order.isBuy && posQty >  1e-9;
            bool closingShort =  order.isBuy && posQty < -1e-9;
            if (!closingLong && !closingShort) return std::nan("");
            double closeQty = std::min((double)order.qty, std::abs(posQty));
            double gross = closingLong
                           ? (price - m_position.avgCost) * closeQty
                           : (m_position.avgCost - price) * closeQty;
            return gross - m_position.commission;
        };

        // Pre-compute label text and cancel-button rect so we can exclude the
        // button area from the drag-start hit-test (same click must not do both).
        char lbl[128];
        const char* legTag = isAux ? "LMT"
                           : (order.orderType == "STP LMT" ? "STP"
                           : order.orderType == "LIT"      ? "TRIG" : "");
        {
            double pnl = calcOrderPnL(drawPrice);
            if (isDragging) {
                if (!std::isnan(pnl))
                    std::snprintf(lbl, sizeof(lbl),
                                  " %s %.0f  $%.2f   P&L %+.2f  [release] ",
                                  order.isBuy ? "BUY" : "SELL", order.qty, drawPrice, pnl);
                else
                    std::snprintf(lbl, sizeof(lbl), " %s %s%.0f  $%.2f  [release] ",
                                  order.isBuy ? "BUY" : "SELL",
                                  legTag[0] ? legTag : "", order.qty, drawPrice);
            } else if (legTag[0]) {
                std::snprintf(lbl, sizeof(lbl), " %s %s $%.2f ",
                              order.isBuy ? "BUY" : "SELL", legTag, drawPrice);
            } else if (!std::isnan(pnl)) {
                std::snprintf(lbl, sizeof(lbl), " %s %.0f @ $%.2f  %+.2f ",
                              order.isBuy ? "BUY" : "SELL", order.qty, drawPrice, pnl);
            } else {
                std::snprintf(lbl, sizeof(lbl), " %s %.0f @ $%.2f ",
                              order.isBuy ? "BUY" : "SELL", order.qty, drawPrice);
            }
        }

        ImVec2 lblSz  = ImGui::CalcTextSize(lbl);
        float  lblX   = lp0.x + 20.f;
        float  btnX   = lblX + lblSz.x + 4.f;
        float  btnY   = lp0.y - 7.f;
        ImVec2 btnMin(btnX, btnY), btnMax(btnX + 14.f, btnY + 14.f);
        bool   cancelBtnHovered = !isAux && !isDragging &&
                                  ImGui::IsMouseHoveringRect(btnMin, btnMax, false);

        // Proximity + interaction guard
        bool canInteract = !m_limitArmed &&
                           (!m_dragPendingActive || isDragging);
        bool nearLine    = canInteract && hovered && !cancelBtnHovered &&
                           std::abs(ImGui::GetIO().MousePos.y - lp0.y) < 8.f;

        // Start drag (excluded when cursor is on the cancel button)
        if (nearLine && !m_dragPendingActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_dragPendingIdx    = oi;
            m_dragPendingActive = true;
            m_dragPendingIsAux  = isAux;
            m_dragPendingPrice  = legPrice;
        }

        // Update / commit drag
        if (isDragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                m_dragPendingPrice = std::round(mp.y / 0.01) * 0.01;
                lp0 = ImPlot::PlotToPixels(m_xMin, m_dragPendingPrice);
                lp1 = ImPlot::PlotToPixels(m_xMax, m_dragPendingPrice);
                drawPrice = m_dragPendingPrice;
            } else {
                // Released — optimistic update + callback
                if (m_dragPendingPrice != legPrice) {
                    double newMain = isAux ? order.price          : m_dragPendingPrice;
                    double newAux  = isAux ? m_dragPendingPrice   : order.auxPrice;
                    if (isAux) order.auxPrice = m_dragPendingPrice;
                    else       order.price    = m_dragPendingPrice;
                    if (OnModifyOrder)
                        OnModifyOrder(order.orderId, newMain, newAux);
                }
                m_dragPendingActive = false;
                m_dragPendingIdx    = -1;
                m_dragPendingIsAux  = false;
                isDragging = false;
                drawPrice  = legPrice;  // already updated above
            }
        }

        // Rebuild label with the live drawPrice (updated above during drag) so the
        // displayed price and P&L always match where the line is actually drawn.
        if (isDragging || !std::isnan(calcOrderPnL(drawPrice))) {
            double pnl = calcOrderPnL(drawPrice);
            if (isDragging) {
                if (!std::isnan(pnl))
                    std::snprintf(lbl, sizeof(lbl),
                                  " %s %.0f  $%.2f   P&L %+.2f  [release] ",
                                  order.isBuy ? "BUY" : "SELL", order.qty, drawPrice, pnl);
                else
                    std::snprintf(lbl, sizeof(lbl), " %s %s%.0f  $%.2f  [release] ",
                                  order.isBuy ? "BUY" : "SELL",
                                  legTag[0] ? legTag : "", order.qty, drawPrice);
            } else if (!std::isnan(pnl)) {
                std::snprintf(lbl, sizeof(lbl), " %s %.0f @ $%.2f  %+.2f ",
                              order.isBuy ? "BUY" : "SELL", order.qty, drawPrice, pnl);
            }
            lblSz = ImGui::CalcTextSize(lbl);
        }

        float lineThick = (nearLine || isDragging) ? 2.5f : 1.5f;
        DrawDashedHLine(dl, lp0.x, lp1.x, lp0.y, lineCol, lineThick, 8.f, 5.f);

        // Grip dots
        if (nearLine || isDragging) {
            for (int gi = 0; gi < 3; gi++) {
                float gy = lp0.y - 4.f + gi * 4.f;
                dl->AddCircleFilled(ImVec2(lp0.x + 10.f, gy), 2.f,
                                    IM_COL32(220,220,220,200));
            }
        }

        // Label (text pre-computed above)
        dl->AddRectFilled(ImVec2(lblX-2, lp0.y-8), ImVec2(lblX+lblSz.x+2, lp0.y+8),
                          lblBg, 2.f);
        dl->AddText(ImVec2(lblX, lp0.y-7), txtCol, lbl);

        // ✕ cancel button on main leg only, not while dragging
        if (!isAux && !isDragging) {
            bool hoverBtn = cancelBtnHovered;
            dl->AddRectFilled(btnMin, btnMax,
                              hoverBtn ? IM_COL32(200,50,50,220) : IM_COL32(120,30,30,180));
            dl->AddText(ImVec2(btnX+3, btnY), IM_COL32(255,210,210,255), "x");
            if (hoverBtn && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                if (OnCancelOrder) OnCancelOrder(order.orderId);
        }
    };

    for (int oi = 0; oi < (int)m_pendingOrders.size(); oi++) {
        auto& order = m_pendingOrders[oi];
        if (order.price <= 0.0) continue;
        bool isDualOrder = ((order.orderType == "STP LMT" || order.orderType == "LIT")
                            && order.auxPrice > 0.0);
        drawOrderLeg(oi, false);          // main (stop / trigger) leg
        // Guard: OnCancelOrder fired inside drawOrderLeg may have replaced
        // m_pendingOrders (via SetPendingOrders). Check index is still valid.
        if (isDualOrder && oi < (int)m_pendingOrders.size())
            drawOrderLeg(oi, true);       // aux (limit) leg — orange
    }

    // ── Armed price line(s) ───────────────────────────────────────────────
    if (m_limitArmed && hovered) {
        bool isDual   = kOrderTypes[m_orderTypeIdx].isDualPrice;
        bool isBuy    = (m_limitSide == "BUY");
        double cursorPrice = std::round(mp.y / 0.01) * 0.01;

        // Helper: draw a floating dashed line with a price bubble at the cursor.
        // pnl=NaN → no P&L annotation.
        auto drawArmedLine = [&](double linePrice, ImU32 lineCol, ImU32 bubBg,
                                  const char* tag, bool followsCursor,
                                  double pnl = std::numeric_limits<double>::quiet_NaN()) {
            ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, linePrice);
            ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, linePrice);
            DrawDashedHLine(dl, lp0.x, lp1.x, lp0.y, lineCol,
                            followsCursor ? 2.0f : 2.5f, 8.f, 5.f);

            // Floating price bubble at cursor X (or center for fixed line)
            char bubBuf[64];
            if (!std::isnan(pnl))
                std::snprintf(bubBuf, sizeof(bubBuf), "%s $%.2f   P&L %+.2f",
                              tag[0] ? tag : m_limitSide.c_str(), linePrice, pnl);
            else
                std::snprintf(bubBuf, sizeof(bubBuf), "%s $%.2f",
                              tag[0] ? tag : m_limitSide.c_str(), linePrice);
            ImVec2 bubSz  = ImGui::CalcTextSize(bubBuf);
            float  mouseX = followsCursor
                            ? ImGui::GetIO().MousePos.x
                            : (lp0.x + lp1.x) * 0.5f;
            mouseX = std::max(pMin.x + 4.f,
                              std::min(pMax.x - bubSz.x - 12.f, mouseX));
            float bx = mouseX, by = lp0.y - bubSz.y - 6.f;
            dl->AddRectFilled(ImVec2(bx-4, by), ImVec2(bx+bubSz.x+6, by+bubSz.y+4),
                              bubBg, 3.f);
            dl->AddText(ImVec2(bx, by+2), IM_COL32(255,255,255,255), bubBuf);
            float midX = bx + bubSz.x * 0.5f;
            dl->AddTriangleFilled(ImVec2(midX-4, by+bubSz.y+4),
                                  ImVec2(midX+4, by+bubSz.y+4),
                                  ImVec2(midX,   lp0.y), bubBg);

            // Right-edge label (shows P&L when available)
            char edgeBuf[64];
            if (!std::isnan(pnl))
                std::snprintf(edgeBuf, sizeof(edgeBuf), " %s  %s $%.2f   %+.2f ",
                              m_limitSide.c_str(), tag, linePrice, pnl);
            else
                std::snprintf(edgeBuf, sizeof(edgeBuf), " %s  %s $%.2f ",
                              m_limitSide.c_str(), tag, linePrice);
            ImVec2 eSz = ImGui::CalcTextSize(edgeBuf);
            dl->AddRectFilled(ImVec2(lp1.x, lp0.y-9),
                              ImVec2(lp1.x+eSz.x+4, lp0.y+9), bubBg, 2.f);
            dl->AddText(ImVec2(lp1.x+2, lp0.y-7), IM_COL32(255,255,255,255), edgeBuf);
        };

        // Compute P&L for an armed (new) order at the given price against the position.
        auto calcArmedPnL = [&](double price) -> double {
            if (!m_position.hasPosition || std::abs(m_position.qty) < 1e-9 ||
                m_position.avgCost <= 0.0)
                return std::numeric_limits<double>::quiet_NaN();
            bool closingLong  = (m_limitSide == "SELL") && (m_position.qty >  1e-9);
            bool closingShort = (m_limitSide == "BUY")  && (m_position.qty < -1e-9);
            if (!closingLong && !closingShort)
                return std::numeric_limits<double>::quiet_NaN();
            double closeQty = std::min((double)m_orderQty, std::abs(m_position.qty));
            double gross = closingLong
                           ? (price - m_position.avgCost) * closeQty
                           : (m_position.avgCost - price) * closeQty;
            return gross - m_position.commission;
        };

        // Colors
        ImU32 stopCol = isBuy ? IM_COL32( 80,140,255,220) : IM_COL32(255, 80, 80,220);
        ImU32 stopBg  = isBuy ? IM_COL32( 15, 55,130,255) : IM_COL32(130, 25, 25,255);
        ImU32 lmtCol  = IM_COL32(220,140, 30,220);
        ImU32 lmtBg   = IM_COL32(100, 55,  5,255);

        if (isDual && m_firstPricePlaced) {
            // Phase 2: fixed first line + moving limit line
            // P&L shown on the limit leg (the fill price that determines profit)
            bool firstIsAux = kOrderTypes[m_orderTypeIdx].firstIsAux;
            const char* firstTag = firstIsAux ? "TRIG" : "STOP";
            drawArmedLine(m_firstPrice, stopCol, stopBg, firstTag, false);
            drawArmedLine(cursorPrice,  lmtCol,  lmtBg,  "LMT",  true,
                          calcArmedPnL(cursorPrice));

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const auto& ot = kOrderTypes[m_orderTypeIdx];
                static constexpr core::TimeInForce kTIFEnum[] = {
                    core::TimeInForce::Day, core::TimeInForce::GTC, core::TimeInForce::GTC,
                    core::TimeInForce::OPG, core::TimeInForce::Overnight
                };
                core::Order o;
                o.symbol     = m_symbol;
                o.side       = (m_limitSide == "BUY") ? core::OrderSide::Buy : core::OrderSide::Sell;
                o.type       = ot.coreType;
                o.quantity   = static_cast<double>(m_orderQty);
                o.tif        = ot.tifLocked ? core::TimeInForce::Day : kTIFEnum[m_tifIdx];
                o.outsideRth = !ot.noRth && (m_sessionIdx != 0);
                if (firstIsAux) {
                    // LIT: first click = trigger (auxPrice), second = limit
                    o.auxPrice   = m_firstPrice;
                    o.limitPrice = cursorPrice;
                } else {
                    // STP LMT: first click = stop, second = limit
                    o.stopPrice  = m_firstPrice;
                    o.limitPrice = cursorPrice;
                }
                if (m_transmitInstantly) {
                    if (OnOrderSubmit) OnOrderSubmit(o);
                    m_limitArmed       = false;
                    m_firstPricePlaced = false;
                } else {
                    m_pendingConfirmOrder = o;
                    m_showConfirmPopup    = true;
                    // leave m_limitArmed / m_firstPricePlaced — reset in popup confirm/cancel
                }
            }
        } else {
            // Phase 1: single moving line (stop/trigger for dual, price for single)
            const char* tag = isDual ? "STOP" : "";
            // For dual types the P&L is shown later (phase 2 limit leg); for single types
            // show it now — a stop on a long position exits at the stop price.
            double armedPnL = isDual ? std::numeric_limits<double>::quiet_NaN()
                                     : calcArmedPnL(cursorPrice);
            drawArmedLine(cursorPrice, stopCol, stopBg, tag, true, armedPnL);

            bool ctrlHeld = ImGui::GetIO().KeyCtrl;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (isDual) {
                    // Dual-price: lock first price, arm second-click line
                    m_firstPrice       = cursorPrice;
                    m_firstPricePlaced = true;
                    // m_limitArmed stays true for phase 2
                } else {
                    // Single-price: build order from cursor position
                    const auto& ot = kOrderTypes[m_orderTypeIdx];
                    static constexpr core::TimeInForce kTIFEnum[] = {
                        core::TimeInForce::Day, core::TimeInForce::GTC, core::TimeInForce::GTC,
                    core::TimeInForce::OPG, core::TimeInForce::Overnight
                    };
                    core::Order o;
                    o.symbol     = m_symbol;
                    o.side       = (m_limitSide == "BUY") ? core::OrderSide::Buy : core::OrderSide::Sell;
                    o.type       = ot.coreType;
                    o.quantity   = static_cast<double>(m_orderQty);
                    o.tif        = ot.tifLocked ? core::TimeInForce::Day : kTIFEnum[m_tifIdx];
                    o.outsideRth = !ot.noRth && (m_sessionIdx != 0);
                    switch (ot.coreType) {
                        case core::OrderType::Stop: o.stopPrice  = cursorPrice; break;
                        case core::OrderType::MIT:  o.auxPrice   = cursorPrice; break;
                        default:                    o.limitPrice = cursorPrice; break;
                    }
                    // Ctrl+click always shows confirmation; otherwise respect m_transmitInstantly
                    if (ctrlHeld || !m_transmitInstantly) {
                        m_pendingConfirmOrder = o;
                        m_showConfirmPopup    = true;
                        // leave m_limitArmed armed — reset when popup is confirmed/cancelled
                    } else {
                        if (OnOrderSubmit) OnOrderSubmit(o);
                        m_limitArmed = false;
                    }
                }
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

    // Height accommodates up to 2 wrapped lines
    float stripH = ImGui::GetTextLineHeightWithSpacing() * 2.0f
                 + ImGui::GetStyle().WindowPadding.y;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.12f, 1.0f));
    ImGui::BeginChild("##posstrip", ImVec2(-1, stripH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    FlexRow row;

    row.item(FlexRow::textW("Position:"), 0);
    ImGui::TextDisabled("Position:");

    char qtyBuf[24];
    std::snprintf(qtyBuf, sizeof(qtyBuf), "%.0f sh", qty);
    row.item(FlexRow::textW(qtyBuf), 6);
    ImGui::PushStyleColor(ImGuiCol_Text,
        qty >= 0 ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                 : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::TextUnformatted(qtyBuf);
    ImGui::PopStyleColor();

    row.item(FlexRow::textW("Entry:"), 12);
    ImGui::TextDisabled("Entry:");
    char entryBuf[16];
    std::snprintf(entryBuf, sizeof(entryBuf), "$%.2f", entry);
    row.item(FlexRow::textW(entryBuf), 4);
    ImGui::TextUnformatted(entryBuf);

    row.item(FlexRow::textW("Last:"), 12);
    ImGui::TextDisabled("Last:");
    char lastBuf[16];
    std::snprintf(lastBuf, sizeof(lastBuf), "$%.2f", last);
    row.item(FlexRow::textW(lastBuf), 4);
    ImGui::TextUnformatted(lastBuf);

    row.item(FlexRow::textW("Unreal P&L:"), 16);
    ImGui::TextDisabled("Unreal P&L:");
    char unrealBuf[16];
    std::snprintf(unrealBuf, sizeof(unrealBuf), "%+.2f", unreal);
    row.item(FlexRow::textW(unrealBuf), 4);
    ImGui::PushStyleColor(ImGuiCol_Text,
        unreal >= 0 ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                    : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::TextUnformatted(unrealBuf);
    ImGui::PopStyleColor();

    if (comm > 0.0) {
        row.item(FlexRow::textW("Comm:"), 12);
        ImGui::TextDisabled("Comm:");
        char commBuf[16];
        std::snprintf(commBuf, sizeof(commBuf), "-$%.2f", comm);
        row.item(FlexRow::textW(commBuf), 4);
        ImGui::TextUnformatted(commBuf);
    }

    // Prefer IB-computed daily P&L when available; fall back to gross-minus-comm.
    double displayNet   = (m_position.dailyPnL != 0.0) ? m_position.dailyPnL : net;
    const char* netLabel = (m_position.dailyPnL != 0.0) ? "Day P&L:" : "Net:";
    row.item(FlexRow::textW(netLabel), 16);
    ImGui::TextDisabled("%s", netLabel);
    char netBuf[16];
    std::snprintf(netBuf, sizeof(netBuf), "%+.2f", displayNet);
    row.item(FlexRow::textW(netBuf), 4);
    ImGui::PushStyleColor(ImGuiCol_Text,
        displayNet >= 0 ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
                        : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::TextUnformatted(netBuf);
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
    chartH  = std::max(chartH,  80.0f);
    volumeH = std::max(volumeH, m_ind.volume ? 60.0f : 0.0f);
    rsiH    = std::max(rsiH,    m_ind.rsi    ? 60.0f : 0.0f);

    // Disable ImPlot panning for drawing tools and while dragging an order line.
    // Do NOT add NoInputs for m_limitArmed: ImPlot must keep its mouse-position
    // state live so GetPlotMousePos() returns the correct price on click.
    bool drawingActive = (m_drawTool != DrawTool::Cursor);
    ImPlotFlags plotFlags = ImPlotFlags_NoMouseText;
    if (drawingActive || m_dragPendingActive) plotFlags |= ImPlotFlags_NoInputs;

    if (!ImPlot::BeginPlot("##candles", ImVec2(-1, chartH), plotFlags))
        return;

    // Index-based X axis — eliminates weekend/overnight/holiday gaps.
    // Custom formatter maps index → timestamp label.
    ImPlot::SetupAxes(nullptr, "Price ($)", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_X1, XTickFormatter, this);
    ImPlot::SetupAxisLinks(ImAxis_X1, &m_xMin, &m_xMax);
    ImPlot::SetupAxisLinks(ImAxis_Y1, &m_priceMin, &m_priceMax);
    ImPlot::SetupFinish();

    // ── Pan-to-load-more: fire OnExtendHistory when user drags past first bar ──
    // Trigger when the left edge of the view is 3+ bars before the start of data.
    if (!m_loading && !m_loadingMore && !m_historyAtStart &&
        !m_xs.empty() && m_xMin < -3.0 && OnExtendHistory) {
        m_loadingMore = true;
        // Format the timestamp of the first bar as IB endDateTime (1 second before
        // so the new series ends strictly before what we already have).
        std::time_t endTs = static_cast<std::time_t>(m_xs[0]) - 1;
        struct tm   endTm = *std::gmtime(&endTs);
        char endBuf[32];
        std::strftime(endBuf, sizeof(endBuf), "%Y%m%d %H:%M:%S UTC", &endTm);
        OnExtendHistory(m_symbol, m_timeframe, endBuf, m_useRTH);
    }

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
    DrawWshMarkers();

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
// WSH corporate event markers — vertical dashed lines with label and tooltip
// Called inside BeginPlot / EndPlot of the candle chart.
// ============================================================================
void ChartWindow::DrawWshMarkers() {
    if (m_wshEvents.empty()) return;
    int n = (int)m_xs.size();
    if (n == 0) return;

    ImDrawList* dl   = ImPlot::GetPlotDrawList();
    ImVec2      pMin = ImPlot::GetPlotPos();
    ImVec2      pMax = ImVec2(pMin.x + ImPlot::GetPlotSize().x,
                               pMin.y + ImPlot::GetPlotSize().y);
    ImVec2 mouse = ImGui::GetMousePos();

    for (const auto& ev : m_wshEvents) {
        int yr = 0, mo = 0, dy = 0;
        if (std::sscanf(ev.date.c_str(), "%d-%d-%d", &yr, &mo, &dy) != 3) continue;
        char evDate[11];
        std::snprintf(evDate, sizeof(evDate), "%04d-%02d-%02d", yr, mo, dy);

        // First bar on or after the event date
        double plotIdx = -1.0;
        for (int i = 0; i < n; ++i) {
            std::time_t t  = (std::time_t)m_xs[i];
            struct tm*  tm = std::gmtime(&t);
            char barDate[11];
            std::snprintf(barDate, sizeof(barDate), "%04d-%02d-%02d",
                          tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
            if (std::strcmp(barDate, evDate) >= 0) { plotIdx = m_idxs[i]; break; }
        }
        if (plotIdx < 0.0) continue;

        float x = ImPlot::PlotToPixels(plotIdx, 0.0).x;
        if (x < pMin.x || x > pMax.x) continue;

        // Color by event type
        std::string tl = ev.type;
        for (auto& c : tl) c = (char)std::tolower((unsigned char)c);
        unsigned int col =
            (tl.find("earn")  != std::string::npos) ? IM_COL32(255, 220,   0, 220) :
            (tl.find("div")   != std::string::npos) ? IM_COL32(  0, 220, 220, 220) :
            (tl.find("split") != std::string::npos) ? IM_COL32(180, 100, 255, 220) :
                                                       IM_COL32(200, 200, 200, 180);

        // Dashed vertical line
        for (float y = pMin.y; y < pMax.y; y += 10.f)
            dl->AddLine(ImVec2(x, y), ImVec2(x, std::min(y + 6.f, pMax.y)), col, 1.5f);

        // Single-char label box at top
        const char* label =
            (tl.find("earn")  != std::string::npos) ? "E" :
            (tl.find("div")   != std::string::npos) ? "D" :
            (tl.find("split") != std::string::npos) ? "S" : "W";
        ImVec2 ts = ImGui::CalcTextSize(label);
        float  bx = x - ts.x * 0.5f - 2.f;
        float  by = pMin.y + 1.f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + ts.x + 4.f, by + ts.y + 2.f),
                          (col & 0x00FFFFFF) | 0x88000000);
        dl->AddText(ImVec2(bx + 2.f, by + 1.f), col, label);

        // Hover tooltip
        if (std::fabs(mouse.x - x) < 6.f && mouse.y >= pMin.y && mouse.y <= pMax.y) {
            ImGui::BeginTooltip();
            ImGui::Text("%s  %s", ev.date.c_str(), ev.type.c_str());
            if (!ev.description.empty()) ImGui::TextUnformatted(ev.description.c_str());
            if (!ev.importance.empty())  ImGui::Text("Importance: %s", ev.importance.c_str());
            ImGui::EndTooltip();
        }
    }
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
    rsiH = std::max(rsiH, m_ind.rsi ? 60.0f : 0.0f);
    volH = std::max(volH, 60.0f);

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

    float rsiAvail = std::max(60.0f, ImGui::GetContentRegionAvail().y);
    if (!ImPlot::BeginPlot("##rsi", ImVec2(-1, rsiAvail),
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
    // For short intraday timeframes the fixed 200-bar window can land entirely
    // in Overnight / weekend hours and be filtered out by RebuildFlatArrays,
    // leaving the chart blank.  Always cover at least 3 calendar days so that
    // regular-session bars are present regardless of what time the app runs.
    int64_t tfSec = core::TimeframeSeconds(m_timeframe);
    int      count = std::max((int)(3LL * 24 * 3600 / tfSec), 200);
    m_series   = GenerateSimulatedBars(m_symbol, m_timeframe, count);
    m_loading  = false;   // unblock render if IB never responded
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
        if (symbol == "AAPL")  return {253.0, 0.015, 0.0003, 55e6};
        if (symbol == "MSFT")  return {380.0, 0.013, 0.0004, 25e6};
        if (symbol == "GOOGL") return {190.0, 0.016, 0.0003, 20e6};
        if (symbol == "TSLA")  return {320.0, 0.030, 0.0002, 90e6};
        if (symbol == "SPY")   return {575.0, 0.008, 0.0002, 80e6};
        return {100.0, 0.020, 0.0002, 10e6};
    }();

    int64_t tfSec = core::TimeframeSeconds(tf);
    std::time_t now = (std::time(nullptr) / tfSec) * tfSec;
    int64_t startTs = now - (int64_t)count * tfSec;

    core::BarSeries series;
    series.symbol = symbol; series.timeframe = tf; series.bars.reserve(count);
    // Walk from 1.0; rescale at the end so the last close always equals cfg.price.
    double price = 1.0;

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
    // Rescale every bar so the last close lands exactly at cfg.price.
    if (!series.bars.empty() && price > 0.0) {
        double scale = cfg.price / price;
        for (auto& b : series.bars) {
            b.open  *= scale;  b.high  *= scale;
            b.low   *= scale;  b.close *= scale;
        }
    }
    return series;
}

}  // namespace ui
