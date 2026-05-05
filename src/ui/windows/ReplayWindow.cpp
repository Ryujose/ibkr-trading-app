#include "ui/UiScale.h"
#include "ui/windows/ReplayWindow.h"
#include "ui/SymbolSearch.h"
#include "ui/ChartRender.h"
#include "ui/DatePicker.h"

#include "imgui.h"
#include "core/models/WindowGroup.h"
#include "core/services/ChartAnalysis.h"
#include "core/services/IBKRUtils.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace ui {

// ============================================================================
// Order type table — local copy of the one in ChartWindow.cpp.
// Drives the trade panel + chart-click arming.
// ============================================================================
namespace {

struct OrderTypeDef {
    const char*     label;
    const char*     ibStr;
    core::OrderType coreType;
    bool needsPrice;   // arms chart-click placement
    bool needsTrail;   // show trail $/% input
    bool isDualPrice;  // two chart clicks needed
    bool firstIsAux;   // first click → auxPrice (LIT)
    bool tifLocked;    // lock TIF combo to DAY (MOC / LOC)
    bool noRth;        // disable outside RTH option (MOC / LOC)
};

constexpr OrderTypeDef kOrderTypes[] = {
    { "Market",          "MKT",         core::OrderType::Market,    false, false, false, false, false, false },
    { "Limit",           "LMT",         core::OrderType::Limit,     true,  false, false, false, false, false },
    { "Stop",            "STP",         core::OrderType::Stop,      true,  false, false, false, false, false },
    { "Stop Limit",      "STP LMT",     core::OrderType::StopLimit, true,  false, true,  false, false, false },
    { "Trail Stop",      "TRAIL",       core::OrderType::Trail,     false, true,  false, false, false, false },
    { "Trail Limit",     "TRAIL LIMIT", core::OrderType::TrailLimit,false, true,  false, false, false, false },
    { "Market On Close", "MOC",         core::OrderType::MOC,       false, false, false, false, true,  true  },
    { "Limit On Close",  "LOC",         core::OrderType::LOC,       true,  false, false, false, true,  true  },
    { "Market to Limit", "MTL",         core::OrderType::MTL,       false, false, false, false, false, false },
    { "Mkt If Touched",  "MIT",         core::OrderType::MIT,       true,  false, false, false, false, false },
    { "Lmt If Touched",  "LIT",         core::OrderType::LIT,       true,  false, true,  true,  false, false },
    { "Midprice",        "MIDPRICE",    core::OrderType::Midprice,  false, false, false, false, false, false },
    { "Relative",        "REL",         core::OrderType::Relative,  false, false, false, false, false, false },
};
constexpr int kNumOrderTypes = (int)std::size(kOrderTypes);

inline void DrawDashedHLine(ImDrawList* dl, float x0, float x1, float y,
                            ImU32 col, float thick = 1.5f,
                            float dashLen = 6.f, float gapLen = 4.f) {
    float x = x0;
    while (x < x1) {
        float xe = std::min(x + dashLen, x1);
        dl->AddLine(ImVec2(x, y), ImVec2(xe, y), col, thick);
        x += dashLen + gapLen;
    }
}

}  // anonymous namespace

// ============================================================================
// Construction
// ============================================================================

ReplayWindow::ReplayWindow() {
    std::snprintf(m_title, sizeof(m_title), "Replay \?\?\?\?-##replay%d", m_instanceId);
    m_clock.speed  = 60.0;   // 60× = 1 bar/sec for M1, visually responsive
    m_clock.paused = true;
}

void ReplayWindow::setInstanceId(int id) {
    m_instanceId = id;
    std::snprintf(m_title, sizeof(m_title), "Replay %s##replay%d", m_symbol, id);
}

void ReplayWindow::SetSymbol(const std::string& sym) {
    std::strncpy(m_symbol, sym.c_str(), sizeof(m_symbol) - 1);
    m_symbol[sizeof(m_symbol) - 1] = '\0';
    m_hasData = false;
    m_viewInitialized = false;
    m_idxs.clear(); m_xs.clear();
    m_opens.clear(); m_highs.clear(); m_lows.clear(); m_closes.clear(); m_volumes.clear();
    std::snprintf(m_title, sizeof(m_title), "Replay %s##replay%d", m_symbol, m_instanceId);
}

void ReplayWindow::SetDay(const core::HistoricalDay& day) {
    m_hasData = true;
    m_loading = false;
    fprintf(stderr, "[ReplayWindow] SetDay: %s %zu bars, session=%d\n",
            day.symbol.c_str(), day.bars.size(), (int)m_session);
    if (day.symbol != m_symbol)
        SetSymbol(day.symbol);
    m_userFills = day.userFills;

    // Copy bars
    m_xs.clear(); m_opens.clear(); m_highs.clear(); m_lows.clear();
    m_closes.clear(); m_volumes.clear();
    int n = static_cast<int>(day.bars.size());
    m_xs.reserve(n); m_opens.reserve(n); m_highs.reserve(n);
    m_lows.reserve(n); m_closes.reserve(n); m_volumes.reserve(n);
    for (const auto& b : day.bars) {
        m_xs.push_back(b.timestamp);
        m_opens.push_back(b.open);
        m_highs.push_back(b.high);
        m_lows.push_back(b.low);
        m_closes.push_back(b.close);
        m_volumes.push_back(b.volume);
    }

    RebuildFlatArrays();
    ComputeIndicators();

    // Set session range
    auto r = core::services::BarRangeForSession(day.bars, m_session);
    fprintf(stderr, "[ReplayWindow] BarRangeForSession: first=%d last=%d (session=%d, n=%d)\n",
            r.firstIdx, r.lastIdx, (int)m_session, (int)day.bars.size());
    if (r.lastIdx > r.firstIdx || n > 0) {
        m_clock.sessionFirstIdx = r.firstIdx;
        m_clock.sessionLastIdx  = (r.lastIdx > 0) ? r.lastIdx : n - 1;
        m_clock.cursorBarIdx    = m_clock.sessionFirstIdx;
    } else {
        m_clock.sessionFirstIdx = 0;
        m_clock.sessionLastIdx  = n > 0 ? n - 1 : 0;
        m_clock.cursorBarIdx    = 0;
    }
    m_clock.cursorSeconds = 0.0;
    m_clock.paused = true;

    // Init view range immediately so the first DrawChart frame shows data.
    // Must happen before BeginPlot/SetupFinish in the render path.
    int first = m_clock.sessionFirstIdx;
    int last  = m_clock.sessionLastIdx;
    int nb = static_cast<int>(m_xs.size());
    if (nb > 0) {
        m_xMin = static_cast<double>(first) - 5.0;
        m_xMax = static_cast<double>(last) + 5.0;
        double pMin = *std::min_element(m_lows.begin() + first, m_lows.begin() + last + 1);
        double pMax = *std::max_element(m_highs.begin() + first, m_highs.begin() + last + 1);
        double pad = (pMax - pMin) * 0.05;
        m_priceMin = pMin - pad;
        m_priceMax = pMax + pad;
        m_viewInitialized = true;
    } else {
        m_viewInitialized = false;
    }
    m_lastProcessedIdx = -1;
    m_simFills.clear();
    m_book.clear();
    core::services::Reset(m_account, m_startingCash);
}

void ReplayWindow::RebuildFlatArrays() {
    int n = static_cast<int>(m_xs.size());
    m_idxs.resize(n);
    for (int i = 0; i < n; ++i) m_idxs[i] = static_cast<double>(i);
}

namespace {
inline bool ReplayIsIntraday(core::Timeframe tf) {
    return tf == core::Timeframe::M1  || tf == core::Timeframe::M5  ||
           tf == core::Timeframe::M15 || tf == core::Timeframe::M30 ||
           tf == core::Timeframe::H1  || tf == core::Timeframe::H4;
}
}  // namespace

void ReplayWindow::ComputeIndicators() {
    m_sma1 = core::services::SMA(m_closes, m_ind.smaPeriod1);
    m_sma2 = core::services::SMA(m_closes, m_ind.smaPeriod2);
    m_ema  = core::services::EMA(m_closes, m_ind.emaPeriod);
    {
        auto bb   = core::services::ComputeBollinger(m_closes, m_ind.bbPeriod, m_ind.bbSigma);
        m_bbMid   = std::move(bb.mid);
        m_bbUpper = std::move(bb.upper);
        m_bbLower = std::move(bb.lower);
    }
    m_rsi  = core::services::RSI(m_closes, m_ind.rsiPeriod);

    // Session-anchored VWAP — same per-day reset logic as ChartWindow.
    std::vector<int> sessionStarts;
    if (ReplayIsIntraday(m_tf)) {
        for (int i = 1; i < (int)m_xs.size(); ++i) {
            std::time_t a = (std::time_t)m_xs[i - 1];
            std::time_t b = (std::time_t)m_xs[i];
            std::tm* ta = std::gmtime(&a);
            int dayA = ta ? ta->tm_yday : 0;
            std::tm* tb = std::gmtime(&b);
            int dayB = tb ? tb->tm_yday : 0;
            if (dayA != dayB) sessionStarts.push_back(i);
        }
    }
    auto vw = core::services::SessionVwap(m_highs, m_lows, m_closes,
                                          m_volumes, sessionStarts);
    m_vwap      = std::move(vw.vwap);
    m_vwapSd1Up = std::move(vw.sd1Up);
    m_vwapSd1Dn = std::move(vw.sd1Dn);
    m_vwapSd2Up = std::move(vw.sd2Up);
    m_vwapSd2Dn = std::move(vw.sd2Dn);
}

void ReplayWindow::setIndicatorSettings(const IndicatorSettings& s) {
    m_ind = s;
    if (m_hasData) ComputeIndicators();
}

// ============================================================================
// Render
// ============================================================================

bool ReplayWindow::Render() {
    ImGui::SetNextWindowSize(ImVec2(em(800), em(520)), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing;
    if (!m_open) return false;

    // Per-frame engine tick — advance clock and evaluate crossed bars
    ApplyEngineTick();

    bool visible = ImGui::Begin(m_title, &m_open, flags);
    if (!visible) { ImGui::End(); return m_open; }

    DrawToolbar();
    DrawScrubber();
    DrawUnguardedStrip();

    if (!m_hasData) {
        ImGui::TextDisabled("No data loaded. Select a symbol and date, then press Load.");
        ImGui::End();
        return m_open;
    }

    if (m_mode == Mode::Operate) {
        DrawTradePanel();
        DrawPositionStrip();
    }
    DrawChart();
    DrawStatusBar();
    DrawBottomTabs();
    DrawConfirmPopup();

    ImGui::End();
    return m_open;
}

// ============================================================================
// Toolbar
// ============================================================================

void ReplayWindow::DrawToolbar() {
    FlexRow row;

    // Group picker
    row.item(FlexRow::buttonW("G1"), 0);
    core::DrawGroupPicker(m_groupId, "##replay_grp");

    // Symbol input
    row.item(em(70), 8);
    ImGui::SetNextItemWidth(em(70));
    ImGui::InputText("##sym", m_symbol, sizeof(m_symbol));

    // Date range — From / To. dateFrom == dateTo for the single-day case.
    // Caps: 30 days for M1/M5; 1 year for M15+ (per replay-indicators plan §2c.2).
    auto parseYmd = [](const char* s) -> std::time_t {
        int y = 0, m = 0, d = 0;
        if (std::sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 0;
        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
        tm.tm_hour = 12;       // noon UTC, robust against DST
        return core::services::Timegm(&tm);
    };
    auto rangeCapDays = [](core::Timeframe tf) -> int {
        return (tf == core::Timeframe::M1 || tf == core::Timeframe::M5) ? 30 : 365;
    };

    char fromBefore[16];
    std::strncpy(fromBefore, m_dateFromBuf, sizeof(fromBefore));
    fromBefore[sizeof(fromBefore)-1] = '\0';
    char toBefore[16];
    std::strncpy(toBefore, m_dateToBuf, sizeof(toBefore));
    toBefore[sizeof(toBefore)-1] = '\0';

    row.item(em(115), 8);
    ui::DrawDatePicker("From", m_dateFromBuf, sizeof(m_dateFromBuf),
                       m_calNavYearFrom, m_calNavMonthFrom, "##dpfrom");
    row.item(em(115), 8);
    ui::DrawDatePicker("To", m_dateToBuf, sizeof(m_dateToBuf),
                       m_calNavYearTo, m_calNavMonthTo, "##dpto");

    // Validate after either pick: To >= From, and span <= cap.
    bool fromChanged = (std::strcmp(fromBefore, m_dateFromBuf) != 0);
    bool toChanged   = (std::strcmp(toBefore,   m_dateToBuf)   != 0);
    if (fromChanged || toChanged) {
        std::time_t tFrom = parseYmd(m_dateFromBuf);
        std::time_t tTo   = parseYmd(m_dateToBuf);
        if (tFrom > 0 && tTo > 0) {
            if (tTo < tFrom) {
                // Reject — revert the side that just moved past the other.
                if (toChanged) {
                    std::strncpy(m_dateToBuf, toBefore, sizeof(m_dateToBuf));
                    m_dateToBuf[sizeof(m_dateToBuf)-1] = '\0';
                } else {
                    std::strncpy(m_dateFromBuf, fromBefore, sizeof(m_dateFromBuf));
                    m_dateFromBuf[sizeof(m_dateFromBuf)-1] = '\0';
                }
            } else {
                int cap = rangeCapDays(m_tf);
                int spanDays = static_cast<int>((tTo - tFrom) / 86400);
                if (spanDays > cap) {
                    // Clamp the side that just moved.
                    if (toChanged) {
                        std::strncpy(m_dateToBuf, toBefore, sizeof(m_dateToBuf));
                        m_dateToBuf[sizeof(m_dateToBuf)-1] = '\0';
                    } else {
                        std::strncpy(m_dateFromBuf, fromBefore, sizeof(m_dateFromBuf));
                        m_dateFromBuf[sizeof(m_dateFromBuf)-1] = '\0';
                    }
                }
            }
        }
    }

    // Session combo
    row.item(em(80), 8);
    ImGui::SetNextItemWidth(em(80));
    static constexpr const char* kSessionNames[] = {"Pre-Market", "Intraday", "Post-Market", "All"};
    int sessIdx = static_cast<int>(m_session);
    if (ImGui::Combo("##sess", &sessIdx, kSessionNames, 4)) {
        m_session = static_cast<core::services::ReplaySession>(sessIdx);
        // Recompute session range on existing data
        if (m_hasData) {
            std::vector<core::Bar> bars;
            for (size_t i = 0; i < m_xs.size(); ++i) {
                core::Bar b;
                b.timestamp = m_xs[i];
                b.open = m_opens[i]; b.high = m_highs[i];
                b.low = m_lows[i]; b.close = m_closes[i];
                b.volume = m_volumes[i];
                bars.push_back(b);
            }
            auto r = core::services::BarRangeForSession(bars, m_session);
            m_clock.sessionFirstIdx = r.firstIdx;
            m_clock.sessionLastIdx  = r.lastIdx > 0 ? r.lastIdx : static_cast<int>(m_xs.size()) - 1;
            core::services::SeekToBar(m_clock, m_clock.sessionFirstIdx);
        }
    }

    // TF combo — same labels as ChartWindow
    row.item(em(60), 8);
    ImGui::SetNextItemWidth(em(60));
    static constexpr core::Timeframe kReplayTfs[] = {
        core::Timeframe::M1, core::Timeframe::M5, core::Timeframe::M15,
        core::Timeframe::M30, core::Timeframe::H1, core::Timeframe::H4,
        core::Timeframe::D1
    };
    static constexpr int kNumReplayTfs = (int)std::size(kReplayTfs);
    int tfIdx = 0;
    for (int i = 0; i < kNumReplayTfs; ++i) {
        if (m_tf == kReplayTfs[i]) { tfIdx = i; break; }
    }
    if (ImGui::Combo("##tf", &tfIdx,
            [](void*, int idx, const char** out) {
                *out = core::TimeframeLabel(kReplayTfs[idx]);
                return true;
            },
            nullptr, kNumReplayTfs)) {
        m_tf = kReplayTfs[tfIdx];
        if (OnDataRequest)
            OnDataRequest(m_symbol, m_dateFromBuf, m_dateToBuf, m_session, m_tf);
    }

    ImGui::SameLine();

    // Load button — fires OnDataRequest for the current symbol/date/session/tf
    if (ImGui::SmallButton("Load")) {
        m_loading = true;
        if (OnDataRequest)
            OnDataRequest(m_symbol, m_dateFromBuf, m_dateToBuf, m_session, m_tf);
    }

    ImGui::SameLine();

    // Pause button
    if (ImGui::SmallButton(m_clock.paused ? ">" : "||"))
        m_clock.paused = !m_clock.paused;

    ImGui::SameLine();
    // Step back
    if (ImGui::SmallButton("|<"))
        core::services::StepBars(m_clock, -1);

    ImGui::SameLine();
    // Step forward
    if (ImGui::SmallButton(">|"))
        core::services::StepBars(m_clock, 1);

    ImGui::SameLine();

    // Speed combo
    ImGui::SetNextItemWidth(em(60));
    static constexpr const char* kSpeeds[] = {"0.25x", "1x", "2x", "5x", "20x", "60x", "MAX"};
    static constexpr double   kSpeedVals[] = {0.25, 1.0, 2.0, 5.0, 20.0, 60.0, 1e9};
    int speedIdx = 1;  // default 1x
    for (int i = 0; i < 7; ++i) {
        if (std::abs(m_clock.speed - kSpeedVals[i]) < 0.01) { speedIdx = i; break; }
    }
    if (ImGui::Combo("##speed", &speedIdx, kSpeeds, 7))
        m_clock.speed = kSpeedVals[speedIdx];

    ImGui::SameLine();

    // Mode badge + combo
    DrawModeBadge();

    // Starting equity (Operate mode only)
    if (m_mode == Mode::Operate) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(75));
        ImGui::InputDouble("Equity $", &m_startingCash, 0.0, 0.0, "%.0f");
        if (m_startingCash < 1000.0) m_startingCash = 1000.0;
    }

    // Tick-fills toggle (§6.2 hybrid — tick fetch wired in Phase 15)
    ImGui::SameLine();
    ImGui::Checkbox("Tick fills", &m_tickFills);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tick-resolution fills — requires historical tick fetch.\n"
                          "When ON with no cached ticks, triggers background fetch.");

    ImGui::SameLine();

    // Reset button
    if (ImGui::SmallButton("Reset")) {
        core::services::Reset(m_account, m_startingCash);
        m_book.clear();
        m_simFills.clear();
        m_nextLocalId = 1;
        m_lastProcessedIdx = -1;
        m_clock.paused = true;
        core::services::SeekToBar(m_clock, m_clock.sessionFirstIdx);
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "PAPER");

    // ── Indicators row (FlexRow-wrapped to match ChartWindow toolbar) ──────
    FlexRow indRow;
    indRow.item(FlexRow::checkboxW("SMA20"), 0);
    if (ImGui::Checkbox("SMA20##rind", &m_ind.sma20))  ComputeIndicators();
    indRow.item(FlexRow::checkboxW("SMA50"), 6);
    if (ImGui::Checkbox("SMA50##rind", &m_ind.sma50))  ComputeIndicators();
    indRow.item(FlexRow::checkboxW("EMA20"), 6);
    if (ImGui::Checkbox("EMA20##rind", &m_ind.ema20))  ComputeIndicators();
    indRow.item(FlexRow::checkboxW("BB"), 6);
    if (ImGui::Checkbox("BB##rind",    &m_ind.bbands)) ComputeIndicators();
    indRow.item(FlexRow::checkboxW("VWAP"), 6);
    if (ImGui::Checkbox("VWAP##rind",  &m_ind.vwap))   ComputeIndicators();
    if (m_ind.vwap) {
        indRow.item(FlexRow::checkboxW("\xC2\xB1\xCF\x83"), 4);
        ImGui::Checkbox("\xC2\xB1\xCF\x83##rind", &m_ind.vwapBands);
        ImGui::SetItemTooltip("Show \xC2\xB1" "1\xCF\x83 / \xC2\xB1" "2\xCF\x83 volume-weighted bands.");
    }
    indRow.item(FlexRow::checkboxW("Vol"), 6);
    ImGui::Checkbox("Vol##rind",   &m_ind.volume);
    indRow.item(FlexRow::checkboxW("RSI"), 6);
    ImGui::Checkbox("RSI##rind",   &m_ind.rsi);
    indRow.item(FlexRow::buttonW("Indicators..."), 8);
    if (ImGui::SmallButton("Indicators...##rind")) m_indSettingsOpen = true;

    // Settings popup body — opened via m_indSettingsOpen flag.
    DrawIndicatorSettingsPopup();
    if (ImGui::BeginPopup("Replay Indicators##indpop")) {
        ImGui::TextDisabled("Periods");
        ImGui::Separator();
        bool changed = false;
        ImGui::SetNextItemWidth(em(80));
        if (ImGui::InputInt("SMA20 period", &m_ind.smaPeriod1)) changed = true;
        ImGui::SetNextItemWidth(em(80));
        if (ImGui::InputInt("SMA50 period", &m_ind.smaPeriod2)) changed = true;
        ImGui::SetNextItemWidth(em(80));
        if (ImGui::InputInt("EMA period",   &m_ind.emaPeriod))  changed = true;
        ImGui::SetNextItemWidth(em(80));
        if (ImGui::InputInt("BB period",    &m_ind.bbPeriod))   changed = true;
        ImGui::SetNextItemWidth(em(80));
        if (ImGui::InputDouble("BB sigma",  &m_ind.bbSigma, 0.1, 0.5, "%.2f")) changed = true;
        ImGui::SetNextItemWidth(em(80));
        if (ImGui::InputInt("RSI period",   &m_ind.rsiPeriod))  changed = true;
        // Clamp to safe ranges.
        m_ind.smaPeriod1 = std::clamp(m_ind.smaPeriod1, 1, 500);
        m_ind.smaPeriod2 = std::clamp(m_ind.smaPeriod2, 1, 500);
        m_ind.emaPeriod  = std::clamp(m_ind.emaPeriod,  1, 500);
        m_ind.bbPeriod   = std::clamp(m_ind.bbPeriod,   1, 500);
        m_ind.bbSigma    = std::clamp(m_ind.bbSigma, 0.1, 5.0);
        m_ind.rsiPeriod  = std::clamp(m_ind.rsiPeriod,  1, 500);
        if (changed) ComputeIndicators();
        ImGui::EndPopup();
    }
}

// ============================================================================
// Mode badge
// ============================================================================

void ReplayWindow::DrawModeBadge() {
    const char* label = (m_mode == Mode::Analysis) ? "Analysis" : "Operate";
    ImVec4 bg = (m_mode == Mode::Analysis)
        ? ImVec4(0.10f, 0.30f, 0.55f, 0.95f)
        : ImVec4(0.55f, 0.30f, 0.05f, 0.95f);

    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg);
    if (ImGui::SmallButton(label)) {
        if (m_mode == Mode::Analysis) {
            m_mode = Mode::Operate;
            // Reset engine state on switch to Operate
            core::services::Reset(m_account, m_startingCash);
            m_book.clear();
            m_simFills.clear();
            m_nextLocalId = 1;
        } else {
            m_mode = Mode::Analysis;
        }
    }
    ImGui::PopStyleColor(3);
}

// ============================================================================
// Scrubber
// ============================================================================

void ReplayWindow::DrawScrubber() {
    if (m_clock.sessionLastIdx <= m_clock.sessionFirstIdx) return;

    int cursor = m_clock.cursorBarIdx;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##scrub", &cursor, m_clock.sessionFirstIdx, m_clock.sessionLastIdx,
                         "", ImGuiSliderFlags_NoInput)) {
        m_clock.scrubbing = ImGui::IsItemActive();
        core::services::SeekToBar(m_clock, cursor);
        m_clock.scrubbing = ImGui::IsItemActive();
    }

    // Time label next to scrubber
    ImGui::SameLine();
    if (m_clock.cursorBarIdx >= 0 && m_clock.cursorBarIdx < static_cast<int>(m_xs.size())) {
        std::time_t t = static_cast<std::time_t>(m_xs[m_clock.cursorBarIdx]);
        std::tm* tm = std::localtime(&t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", tm);
        ImGui::Text("%s", buf);
    }
}

// ============================================================================
// Chart
// ============================================================================

void ReplayWindow::DrawChart() {
    int n = static_cast<int>(m_idxs.size());
    if (n == 0) return;

    // Reserve space for sub-plots (Volume / RSI) when enabled. Same height
    // logic as ChartWindow::DrawCandleChart — 90px each.
    float available = ImGui::GetContentRegionAvail().y;
    available = std::max(available, 240.0f);
    float spacing = ImGui::GetStyle().ItemSpacing.y;
    float volumeH = m_ind.volume ? std::max(90.0f, available * 0.18f) : 0.0f;
    float rsiH    = m_ind.rsi    ? std::max(90.0f, available * 0.18f) : 0.0f;
    float chartH  = available - volumeH - rsiH
                  - (m_ind.volume ? spacing : 0.0f)
                  - (m_ind.rsi    ? spacing : 0.0f);
    chartH = std::max(chartH, 120.0f);

    // ── Price chart (with indicator overlays) ──────────────────────────────
    if (!ImPlot::BeginPlot("##replay_chart", ImVec2(-1, chartH), ImPlotFlags_NoMouseText))
        return;

    ImPlot::SetupAxes(nullptr, "Price ($)", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_X1, XTickFormatter, this);
    ImPlot::SetupAxisLinks(ImAxis_X1, &m_xMin, &m_xMax);
    ImPlot::SetupAxisLinks(ImAxis_Y1, &m_priceMin, &m_priceMax);
    ImPlot::SetupFinish();

    // Indicator overlays — mirror ChartWindow::DrawCandleChart layering.
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
        ImPlot::SetNextLineStyle(ImVec4(1.f, 0.85f, 0.f, 1.f), 1.5f);
        ImPlot::PlotLine("VWAP", m_idxs.data(), m_vwap.data(), n);
        if (m_ind.vwapBands && (int)m_vwapSd2Up.size() == n) {
            ImPlot::SetNextLineStyle(ImVec4(1.f, 0.85f, 0.0f, 0.30f), 1.f);
            ImPlot::PlotLine("VWAP+1\xCF\x83", m_idxs.data(), m_vwapSd1Up.data(), n);
            ImPlot::SetNextLineStyle(ImVec4(1.f, 0.85f, 0.0f, 0.30f), 1.f);
            ImPlot::PlotLine("VWAP-1\xCF\x83", m_idxs.data(), m_vwapSd1Dn.data(), n);
            ImPlot::SetNextLineStyle(ImVec4(1.f, 0.85f, 0.0f, 0.18f), 1.f);
            ImPlot::PlotLine("VWAP+2\xCF\x83", m_idxs.data(), m_vwapSd2Up.data(), n);
            ImPlot::SetNextLineStyle(ImVec4(1.f, 0.85f, 0.0f, 0.18f), 1.f);
            ImPlot::PlotLine("VWAP-2\xCF\x83", m_idxs.data(), m_vwapSd2Dn.data(), n);
        }
    }

    // Progressive reveal: only show candles up to the cursor (overlays span
    // the whole loaded range — they're an analytical aid, not a tape).
    int visible = m_clock.cursorBarIdx + 1;
    RenderCandlestickBodies(m_idxs, m_opens, m_highs, m_lows, m_closes,
                            m_clock.sessionFirstIdx, m_clock.sessionLastIdx, visible);
    RenderCursorLine(m_clock.cursorBarIdx, m_idxs, m_highs, m_lows);
    DrawFillMarkers();
    if (m_mode == Mode::Operate) {
        DrawWorkingOrders();
        DrawArmedLineAndHandleClick();
    }

    ImPlot::EndPlot();

    if (m_ind.volume) DrawVolumeChart();
    if (m_ind.rsi)    DrawRsiChart();
}

// ============================================================================
// Volume / RSI sub-charts — mirrors ChartWindow's helpers, simplified to
// reserved-height arithmetic that's already done by DrawChart's caller.
// ============================================================================

int ReplayWindow::VolTickFormatter(double v, char* buf, int size, void*) {
    if      (v >= 1e6) return std::snprintf(buf, (size_t)size, "%.2fM", v / 1e6);
    else if (v >= 1e3) return std::snprintf(buf, (size_t)size, "%.0fK", v / 1e3);
    else               return std::snprintf(buf, (size_t)size, "%.0f",  v);
}

void ReplayWindow::DrawVolumeChart() {
    int n = (int)m_idxs.size();
    if (n == 0) return;

    // Use whatever space was reserved by DrawChart.
    float available = ImGui::GetContentRegionAvail().y;
    float rsiH      = m_ind.rsi ? std::max(90.0f, available * 0.40f) : 0.0f;
    float volH      = available - rsiH - (m_ind.rsi ? ImGui::GetStyle().ItemSpacing.y : 0.0f);
    volH = std::max(volH, 60.0f);

    double maxVol = 1.0;
    for (int i = 0; i < n; ++i)
        if (m_idxs[i] >= m_xMin - 1.0 && m_idxs[i] <= m_xMax + 1.0)
            maxVol = std::max(maxVol, m_volumes[i]);

    if (!ImPlot::BeginPlot("##replay_volume", ImVec2(-1, volH),
                           ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend))
        return;

    ImPlot::SetupAxes(nullptr, "Volume", ImPlotAxisFlags_NoTickLabels, ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_X1, XTickFormatter, this);
    ImPlot::SetupAxisLinks(ImAxis_X1, &m_xMin, &m_xMax);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, maxVol * 1.15, ImGuiCond_Always);
    ImPlot::SetupAxisFormat(ImAxis_Y1, VolTickFormatter);
    ImPlot::SetupFinish();

    static constexpr double kBarW = 0.7;
    int visible = m_clock.cursorBarIdx + 1;
    if (visible > n) visible = n;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    for (int i = 0; i < visible; ++i) {
        bool   bull = m_closes[i] >= m_opens[i];
        ImU32  col  = bull ? IM_COL32(52, 211, 100, 160) : IM_COL32(220, 60, 60, 160);
        ImVec2 top = ImPlot::PlotToPixels(m_idxs[i] - kBarW * 0.5, m_volumes[i]);
        ImVec2 bot = ImPlot::PlotToPixels(m_idxs[i] + kBarW * 0.5, 0.0);
        if (top.y < bot.y) dl->AddRectFilled(top, bot, col);
    }
    ImPlot::PopPlotClipRect();
    RenderCursorLine(m_clock.cursorBarIdx, m_idxs,
                     std::vector<double>(n, maxVol * 1.15),
                     std::vector<double>(n, 0.0));
    ImPlot::EndPlot();
}

void ReplayWindow::DrawRsiChart() {
    int n = (int)m_idxs.size();
    if (n == 0 || (int)m_rsi.size() != n) return;

    float rsiAvail = std::max(60.0f, ImGui::GetContentRegionAvail().y);
    if (!ImPlot::BeginPlot("##replay_rsi", ImVec2(-1, rsiAvail),
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
    RenderCursorLine(m_clock.cursorBarIdx, m_idxs,
                     std::vector<double>(n, 100.0),
                     std::vector<double>(n, 0.0));
    ImPlot::EndPlot();
}

void ReplayWindow::DrawIndicatorSettingsPopup() {
    if (!m_indSettingsOpen) return;
    ImGui::OpenPopup("Replay Indicators##indpop");
    m_indSettingsOpen = false;
}

// (Helper to render the popup body when shown; called from DrawToolbar's
//  BeginPopup path — defined here so the rendering math stays grouped.)

void ReplayWindow::DrawCandlesticks() {
    RenderCandlestickBodies(m_idxs, m_opens, m_highs, m_lows, m_closes,
                            m_clock.sessionFirstIdx, m_clock.sessionLastIdx);
}

void ReplayWindow::DrawCursorLine() {
    RenderCursorLine(m_clock.cursorBarIdx, m_idxs, m_highs, m_lows);
}

void ReplayWindow::DrawFillMarkers() {
    int n = static_cast<int>(m_idxs.size());
    if (n == 0) return;

    // Build Bar vector once for Snapping
    std::vector<core::Bar> bars;
    bars.reserve(n);
    for (size_t j = 0; j < m_xs.size(); ++j)
        bars.push_back({m_xs[j], m_opens[j], m_highs[j], m_lows[j], m_closes[j], m_volumes[j]});

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    // Real fills — green/red triangles
    for (const auto& f : m_userFills) {
        int idx = core::services::SnapCursorToNearestBar(f.timestamp, bars);
        if (idx < 0 || idx >= n) continue;

        bool isBuy = (f.side == core::OrderSide::Buy);
        ImU32 col = isBuy ? IM_COL32(52, 211, 100, 220) : IM_COL32(220, 60, 60, 220);
        ImVec2 p = ImPlot::PlotToPixels(m_idxs[idx], f.price);
        float sz = 5.0f;
        if (isBuy)
            dl->AddTriangleFilled(ImVec2(p.x, p.y - sz), ImVec2(p.x - sz, p.y + sz),
                                  ImVec2(p.x + sz, p.y + sz), col);
        else
            dl->AddTriangleFilled(ImVec2(p.x, p.y + sz), ImVec2(p.x - sz, p.y - sz),
                                  ImVec2(p.x + sz, p.y - sz), col);
    }

    // Sim fills — blue diamonds
    for (const auto& f : m_simFills) {
        int idx = core::services::SnapCursorToNearestBar(f.time, bars);
        if (idx < 0 || idx >= n) continue;

        ImVec2 p = ImPlot::PlotToPixels(m_idxs[idx], f.price);
        float sz = 4.5f;
        ImU32 col = IM_COL32(80, 160, 255, 220);
        dl->AddQuadFilled(ImVec2(p.x, p.y - sz), ImVec2(p.x + sz, p.y),
                          ImVec2(p.x, p.y + sz), ImVec2(p.x - sz, p.y), col);
    }

    ImPlot::PopPlotClipRect();
}

// ============================================================================
// Status bar
// ============================================================================

void ReplayWindow::DrawStatusBar() {
    int n = static_cast<int>(m_idxs.size());
    if (m_clock.cursorBarIdx < 0 || m_clock.cursorBarIdx >= n) return;

    std::time_t t = static_cast<std::time_t>(m_xs[m_clock.cursorBarIdx]);
    std::tm* tm = std::localtime(&t);
    char timeBuf[16];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", tm);

    double lastPrice = m_closes[m_clock.cursorBarIdx];
    double equity = (m_mode == Mode::Operate)
        ? core::services::Equity(m_account, lastPrice)
        : m_startingCash;

    char buf[320];
    std::snprintf(buf, sizeof(buf),
        "Cursor: %s  |  Bar %d/%d  |  Sim Equity: $%.2f  |  Sim PnL: %+.2f",
        timeBuf, m_clock.cursorBarIdx - m_clock.sessionFirstIdx + 1,
        m_clock.sessionLastIdx - m_clock.sessionFirstIdx + 1,
        equity, equity - m_startingCash);

    ImGui::Text("%s", buf);

    // Range info — count distinct trading days in the loaded range.
    if (n > 0 && std::strcmp(m_dateFromBuf, m_dateToBuf) != 0) {
        int days = 1;
        std::time_t prev = static_cast<std::time_t>(m_xs[0]);
        std::tm prevTm   = *std::localtime(&prev);
        for (int i = 1; i < n; ++i) {
            std::time_t cur = static_cast<std::time_t>(m_xs[i]);
            std::tm curTm   = *std::localtime(&cur);
            if (curTm.tm_yday != prevTm.tm_yday || curTm.tm_year != prevTm.tm_year) {
                ++days;
                prevTm = curTm;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|  Loaded %d trading day%s (%s … %s)",
            days, days == 1 ? "" : "s", m_dateFromBuf, m_dateToBuf);
    }
}

// ============================================================================
// Trade panel (Operate mode) — ChartWindow-style BUY/SELL row.
// Mirrors src/ui/windows/ChartWindow.cpp::DrawTradePanel().
// ============================================================================

void ReplayWindow::DrawTradePanel() {
    static constexpr const char* kTIFs[] = { "DAY", "GTC" };

    FlexRow row;
    const float kBtnW = em(56);

    // ── Qty ────────────────────────────────────────────────────────────────
    row.item(FlexRow::textW("Trade:"), 0);
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "PAPER:");
    row.item(em(62), 6);
    ImGui::SetNextItemWidth(em(62));
    ImGui::InputInt("Qty##rord", &m_orderQty, 0, 0);
    if (m_orderQty < 1) m_orderQty = 1;

    // ── Order type combo ───────────────────────────────────────────────────
    row.item(em(170), 8);
    ImGui::SetNextItemWidth(em(170));
    if (ImGui::BeginCombo("##rotype", kOrderTypes[m_orderTypeIdx].label)) {
        for (int i = 0; i < kNumOrderTypes; ++i) {
            bool sel = (i == m_orderTypeIdx);
            if (ImGui::Selectable(kOrderTypes[i].label, sel)) {
                m_orderTypeIdx     = i;
                m_limitArmed       = false;
                m_firstPricePlaced = false;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // ── Trail amount (Trail / TrailLimit) ──────────────────────────────────
    if (kOrderTypes[m_orderTypeIdx].needsTrail) {
        row.item(em(28), 6);
        if (ImGui::Button(m_trailByPct ? "%##rtpct" : "$##rtpct", ImVec2(em(28), 0)))
            m_trailByPct = !m_trailByPct;

        if (m_trailByPct) {
            row.item(FlexRow::textW("Trail %:"), 4);
            ImGui::TextDisabled("Trail %%:");
            row.item(em(60), 4);
            ImGui::SetNextItemWidth(em(60));
            ImGui::InputDouble("##rtrailpct", &m_trailPercent, 0.0, 0.0, "%.2f");
            if (m_trailPercent <= 0.0) m_trailPercent = 0.1;
        } else {
            row.item(FlexRow::textW("Trail $:"), 4);
            ImGui::TextDisabled("Trail $:");
            row.item(em(60), 4);
            ImGui::SetNextItemWidth(em(60));
            ImGui::InputDouble("##rtrail", &m_trailAmount, 0.0, 0.0, "%.2f");
            if (m_trailAmount <= 0.0) m_trailAmount = 0.01;
        }

        if (kOrderTypes[m_orderTypeIdx].coreType == core::OrderType::TrailLimit) {
            row.item(FlexRow::textW("Lmt Off:"), 8);
            ImGui::TextDisabled("Lmt Off:");
            row.item(em(60), 4);
            ImGui::SetNextItemWidth(em(60));
            ImGui::InputDouble("##rlmtoff", &m_limitOffset, 0.0, 0.0, "%.2f");
        }
    }

    // ── Peg offset (Relative) ──────────────────────────────────────────────
    if (kOrderTypes[m_orderTypeIdx].coreType == core::OrderType::Relative) {
        row.item(FlexRow::textW("Offset:"), 6);
        ImGui::TextDisabled("Offset:");
        row.item(em(60), 4);
        ImGui::SetNextItemWidth(em(60));
        ImGui::InputDouble("##rpegoff", &m_pegOffset, 0.0, 0.0, "%.2f");
        if (m_pegOffset <= 0.0) m_pegOffset = 0.01;
    }

    // ── TIF ────────────────────────────────────────────────────────────────
    bool tifLocked = kOrderTypes[m_orderTypeIdx].tifLocked;
    row.item(em(52), 6);
    ImGui::SetNextItemWidth(em(52));
    if (tifLocked) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##rtif", tifLocked ? "DAY" : kTIFs[m_tifIdx])) {
        for (int i = 0; i < 2; ++i) {
            bool sel = (i == m_tifIdx);
            if (ImGui::Selectable(kTIFs[i], sel)) m_tifIdx = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (tifLocked) ImGui::EndDisabled();

    // ── BUY / SELL ─────────────────────────────────────────────────────────
    row.item(kBtnW, 14);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.08f, 0.52f, 0.08f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.72f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.04f, 0.38f, 0.04f, 1.f));
    bool buyClicked = ImGui::Button("  BUY  ##rord", ImVec2(kBtnW, 0));
    ImGui::PopStyleColor(3);

    row.item(kBtnW, 4);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.52f, 0.08f, 0.08f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.04f, 0.04f, 1.f));
    bool sellClicked = ImGui::Button(" SELL  ##rord", ImVec2(kBtnW, 0));
    ImGui::PopStyleColor(3);

    // ── Status hint when armed ──────────────────────────────────────────────
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

    row.item(FlexRow::checkboxW("Transmit Instantly"), 16);
    ImGui::Checkbox("Transmit Instantly##r", &m_transmitInstantly);

    // ── Build / fire ────────────────────────────────────────────────────────
    auto buildOrder = [&](const std::string& side) -> core::Order {
        const auto& ot = kOrderTypes[m_orderTypeIdx];
        core::TimeInForce tif = ot.tifLocked ? core::TimeInForce::Day
                              : (m_tifIdx == 0 ? core::TimeInForce::Day
                                                : core::TimeInForce::GTC);
        core::Order o;
        o.symbol     = m_symbol;
        o.side       = (side == "BUY") ? core::OrderSide::Buy : core::OrderSide::Sell;
        o.type       = ot.coreType;
        o.quantity   = static_cast<double>(m_orderQty);
        o.tif        = tif;
        o.outsideRth = !ot.noRth;   // replay covers all sessions; let bar evaluator decide
        switch (ot.coreType) {
            case core::OrderType::Trail:
                if (m_trailByPct) o.trailingPercent = m_trailPercent;
                else              o.auxPrice         = m_trailAmount;
                break;
            case core::OrderType::TrailLimit:
                if (m_trailByPct) o.trailingPercent = m_trailPercent;
                else              o.auxPrice         = m_trailAmount;
                o.lmtPriceOffset = m_limitOffset;
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
            // Market-family — submit immediately or via confirm
            core::Order o = buildOrder(side);
            if (m_transmitInstantly) {
                if (OnPaperOrderSubmit) {
                    int id = OnPaperOrderSubmit(o);
                    if (id > 0) {
                        core::services::WorkingOrder wo;
                        wo.localId  = id;
                        wo.order    = o;
                        wo.placedAt = (m_clock.cursorBarIdx >= 0 &&
                                       m_clock.cursorBarIdx < (int)m_xs.size())
                            ? static_cast<std::time_t>(m_xs[m_clock.cursorBarIdx])
                            : std::time(nullptr);
                        m_book.push_back(wo);
                    }
                }
            } else {
                m_pendingConfirmOrder = o;
                m_showConfirmPopup    = true;
            }
        } else {
            // Arm chart-click placement
            m_limitArmed       = true;
            m_limitSide        = side;
            m_firstPricePlaced = false;
        }
    };

    if (buyClicked)  fireOrder("BUY");
    if (sellClicked) fireOrder("SELL");

    DrawOrderImpactBadge();

    if (m_limitArmed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_limitArmed       = false;
        m_firstPricePlaced = false;
    }
}

// ============================================================================
// Position strip (Operate mode) — qty / entry / last / unreal PnL.
// ============================================================================

void ReplayWindow::DrawPositionStrip() {
    auto it = m_account.positions.find(m_symbol);
    bool hasPos = (it != m_account.positions.end() && std::abs(it->second.qty) > 1e-9);
    bool showOrder = m_limitArmed;
    if (!hasPos && !showOrder) return;

    double qty    = hasPos ? it->second.qty     : 0.0;
    double entry  = hasPos ? it->second.avgCost : 0.0;
    double last   = (m_clock.cursorBarIdx >= 0 && m_clock.cursorBarIdx < (int)m_closes.size())
                    ? m_closes[m_clock.cursorBarIdx] : entry;
    double unreal = (last - entry) * qty;

    float stripH = ImGui::GetTextLineHeightWithSpacing() * 2.0f
                 + ImGui::GetStyle().WindowPadding.y;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.12f, 1.0f));
    ImGui::BeginChild("##rposstrip", ImVec2(-1, stripH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    FlexRow row;

    if (hasPos) {
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
    }

    if (showOrder) {
        const auto& ot = kOrderTypes[m_orderTypeIdx];
        double price = m_liveCursorPrice;
        bool isBuy = (m_limitSide == "BUY");

        if (hasPos) {
            row.item(em(12), 12);
            ImGui::TextDisabled("|");
        }

        row.item(FlexRow::textW("Order:"), 12);
        ImGui::TextDisabled("Order:");

        char orderBuf[64];
        if (ot.isDualPrice && m_firstPricePlaced) {
            std::snprintf(orderBuf, sizeof(orderBuf), "%s %s  STP $%.2f  LMT $%.2f",
                          isBuy ? "BUY" : "SELL", ot.label, m_firstPrice,
                          price > 0.0 ? price : 0.0);
        } else if (ot.needsPrice && price > 0.0) {
            std::snprintf(orderBuf, sizeof(orderBuf), "%s %s @ $%.2f",
                          isBuy ? "BUY" : "SELL", ot.label, price);
        } else {
            std::snprintf(orderBuf, sizeof(orderBuf), "%s %s",
                          isBuy ? "BUY" : "SELL", ot.label);
        }
        row.item(FlexRow::textW(orderBuf), 4);
        ImGui::PushStyleColor(ImGuiCol_Text,
            isBuy ? ImVec4(0.25f, 0.80f, 0.40f, 1.f)
                  : ImVec4(0.95f, 0.35f, 0.30f, 1.f));
        ImGui::TextUnformatted(orderBuf);
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================================
// Order-impact badge — side-intent + simulated PnL preview.
// ============================================================================

void ReplayWindow::DrawOrderImpactBadge() {
    if (m_orderQty <= 0 || !m_limitArmed) return;
    bool isBuy = (m_limitSide == "BUY");

    const auto& ot = kOrderTypes[m_orderTypeIdx];
    double last = (m_clock.cursorBarIdx >= 0 && m_clock.cursorBarIdx < (int)m_closes.size())
                  ? m_closes[m_clock.cursorBarIdx] : 0.0;
    double fillPrice = 0.0;

    if (ot.needsPrice) {
        if (m_liveCursorPrice > 0.0) fillPrice = m_liveCursorPrice;
        else return;
    } else {
        if (last <= 0.0) return;
        switch (ot.coreType) {
            case core::OrderType::Market:
            case core::OrderType::MOC:
            case core::OrderType::MTL:
            case core::OrderType::Midprice:
                fillPrice = last; break;
            case core::OrderType::Trail:
            case core::OrderType::TrailLimit:
                fillPrice = isBuy ? (last - m_trailAmount) : (last + m_trailAmount);
                break;
            case core::OrderType::Relative:
                fillPrice = isBuy ? (last + m_pegOffset) : (last - m_pegOffset);
                break;
            default: return;
        }
    }
    if (fillPrice <= 0.0) return;

    auto it = m_account.positions.find(m_symbol);
    double posQty = (it != m_account.positions.end()) ? it->second.qty : 0.0;
    double avgCost = (it != m_account.positions.end()) ? it->second.avgCost : 0.0;
    double commPerShare = core::services::kDefaultCommissionPerShare;

    auto imp = core::services::ComputeOrderImpact(posQty, avgCost, commPerShare,
                                                   isBuy, (double)m_orderQty, fillPrice);
    if (imp.kind == core::services::OrderImpactKind::Invalid) return;

    static constexpr ImVec4 kOpenAddBg   = ImVec4(0.05f, 0.12f, 0.22f, 0.90f);
    static constexpr ImVec4 kOpenAddBdr  = ImVec4(0.20f, 0.50f, 0.85f, 0.90f);
    static constexpr ImVec4 kGreenBg     = ImVec4(0.05f, 0.18f, 0.08f, 0.90f);
    static constexpr ImVec4 kGreenBdr    = ImVec4(0.15f, 0.65f, 0.25f, 0.90f);
    static constexpr ImVec4 kRedBg       = ImVec4(0.22f, 0.08f, 0.05f, 0.90f);
    static constexpr ImVec4 kRedBdr      = ImVec4(0.80f, 0.25f, 0.15f, 0.90f);
    static constexpr ImVec4 kFlipBg      = ImVec4(0.25f, 0.15f, 0.05f, 0.90f);
    static constexpr ImVec4 kFlipBdr     = ImVec4(0.90f, 0.55f, 0.10f, 0.90f);

    ImVec4 bgCol, bdrCol, textCol;
    bool isOpenOrAdd = false;

    switch (imp.kind) {
        case core::services::OrderImpactKind::OpenLong:
        case core::services::OrderImpactKind::OpenShort:
        case core::services::OrderImpactKind::AddToLong:
        case core::services::OrderImpactKind::AddToShort:
            bgCol = kOpenAddBg; bdrCol = kOpenAddBdr;
            textCol = ImVec4(0.40f, 0.70f, 1.00f, 1.f);
            isOpenOrAdd = true;
            break;
        case core::services::OrderImpactKind::ReduceLong:
        case core::services::OrderImpactKind::ReduceShort:
        case core::services::OrderImpactKind::CloseLong:
        case core::services::OrderImpactKind::CloseShort:
            if (imp.closePnL > 0.0) {
                bgCol = kGreenBg; bdrCol = kGreenBdr;
                textCol = ImVec4(0.25f, 0.90f, 0.35f, 1.f);
            } else {
                bgCol = kRedBg; bdrCol = kRedBdr;
                textCol = ImVec4(0.95f, 0.40f, 0.30f, 1.f);
            }
            break;
        case core::services::OrderImpactKind::FlipToShort:
        case core::services::OrderImpactKind::FlipToLong:
            bgCol = kFlipBg; bdrCol = kFlipBdr;
            textCol = ImVec4(1.00f, 0.70f, 0.20f, 1.f);
            break;
        default: return;
    }

    float stripH = ImGui::GetTextLineHeightWithSpacing()
                 + ImGui::GetStyle().FramePadding.y * 2.0f + 6.0f;

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bgCol);
    ImGui::PushStyleColor(ImGuiCol_Border,  bdrCol);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    ImGui::BeginChild("##rorderimpact", ImVec2(-1, stripH),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, textCol);

    char buf[200];
    const char* kindStr = "";
    switch (imp.kind) {
        case core::services::OrderImpactKind::OpenLong:    kindStr = "OPEN LONG";     break;
        case core::services::OrderImpactKind::OpenShort:   kindStr = "OPEN SHORT";    break;
        case core::services::OrderImpactKind::AddToLong:   kindStr = "ADD TO LONG";   break;
        case core::services::OrderImpactKind::AddToShort:  kindStr = "ADD TO SHORT";  break;
        case core::services::OrderImpactKind::ReduceLong:  kindStr = "REDUCE LONG";   break;
        case core::services::OrderImpactKind::ReduceShort: kindStr = "REDUCE SHORT";  break;
        case core::services::OrderImpactKind::CloseLong:   kindStr = "CLOSE LONG";    break;
        case core::services::OrderImpactKind::CloseShort:  kindStr = "CLOSE SHORT";   break;
        case core::services::OrderImpactKind::FlipToShort: kindStr = "FLIP TO SHORT"; break;
        case core::services::OrderImpactKind::FlipToLong:  kindStr = "FLIP TO LONG";  break;
        default: break;
    }

    if (isOpenOrAdd) {
        double cost = fillPrice * (double)m_orderQty;
        std::snprintf(buf, sizeof(buf), "  %s  ·  %.0f sh @ $%.2f  ·  cost ~ $%'.0f",
                      kindStr, (double)m_orderQty, fillPrice, cost);
    } else if (imp.kind == core::services::OrderImpactKind::FlipToShort ||
               imp.kind == core::services::OrderImpactKind::FlipToLong) {
        const char* openDir = (imp.kind == core::services::OrderImpactKind::FlipToShort)
                              ? "short" : "long";
        std::snprintf(buf, sizeof(buf), "  %s  ·  close %.0f (%+.2f)  →  open %.0f %s @ $%.2f",
                      kindStr, imp.closeQty, imp.closePnL,
                      imp.openQty, openDir, fillPrice);
    } else {
        double pctPnL = 0.0;
        if (imp.closeQty > 0.0 && avgCost > 0.0)
            pctPnL = (imp.closePnL / (avgCost * imp.closeQty)) * 100.0;
        std::snprintf(buf, sizeof(buf), "  %s  ·  %.0f sh  ·  est. P&L %+.2f (%+.2f%%)",
                      kindStr, imp.closeQty, imp.closePnL, pctPnL);
    }
    ImGui::TextUnformatted(buf);

    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// ============================================================================
// Working orders — dashed h-lines on the chart.
// Called inside BeginPlot/EndPlot.
// ============================================================================

void ReplayWindow::DrawWorkingOrders() {
    if (m_book.empty()) return;
    int n = (int)m_idxs.size();
    if (n == 0) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    ImVec2 pMin = ImPlot::GetPlotPos();
    ImVec2 pMax = ImVec2(pMin.x + ImPlot::GetPlotSize().x,
                         pMin.y + ImPlot::GetPlotSize().y);

    auto drawLeg = [&](const core::services::WorkingOrder& wo, double price,
                       bool isAuxLeg, ImU32 col, ImU32 bg) {
        if (price <= 0.0) return;
        ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, price);
        DrawDashedHLine(dl, pMin.x, pMax.x, lp0.y, col, 1.4f, 6.f, 4.f);

        char buf[80];
        const char* sideStr = (wo.order.side == core::OrderSide::Buy) ? "BUY" : "SELL";
        const char* legStr  = isAuxLeg ? "LMT" : core::OrderTypeStr(wo.order.type);
        std::snprintf(buf, sizeof(buf), " #%d %s %s %.0f @ $%.2f ",
                      wo.localId, sideStr, legStr, wo.order.quantity, price);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        float bx = pMin.x + 6.f, by = lp0.y - sz.y - 4.f;
        dl->AddRectFilled(ImVec2(bx - 2, by), ImVec2(bx + sz.x + 4, by + sz.y + 2),
                          bg, 2.f);
        dl->AddText(ImVec2(bx, by + 1), IM_COL32(255, 255, 255, 255), buf);
    };

    for (const auto& wo : m_book) {
        bool isBuy = (wo.order.side == core::OrderSide::Buy);
        ImU32 stopCol = isBuy ? IM_COL32(80, 140, 255, 220) : IM_COL32(255, 80, 80, 220);
        ImU32 stopBg  = isBuy ? IM_COL32(15,  55, 130, 235) : IM_COL32(130, 25, 25, 235);
        ImU32 lmtCol  = IM_COL32(220, 140, 30, 220);
        ImU32 lmtBg   = IM_COL32(100,  55,  5, 235);

        switch (wo.order.type) {
            case core::OrderType::Limit:
            case core::OrderType::LOC:
                drawLeg(wo, wo.order.limitPrice, false, lmtCol, lmtBg);
                break;
            case core::OrderType::Stop:
                drawLeg(wo, wo.order.stopPrice, false, stopCol, stopBg);
                break;
            case core::OrderType::StopLimit:
                drawLeg(wo, wo.order.stopPrice,  false, stopCol, stopBg);
                drawLeg(wo, wo.order.limitPrice, true,  lmtCol,  lmtBg);
                break;
            case core::OrderType::MIT:
                drawLeg(wo, wo.order.auxPrice, false, stopCol, stopBg);
                break;
            case core::OrderType::LIT:
                drawLeg(wo, wo.order.auxPrice,   false, stopCol, stopBg);
                drawLeg(wo, wo.order.limitPrice, true,  lmtCol,  lmtBg);
                break;
            case core::OrderType::Trail:
            case core::OrderType::TrailLimit:
                if (wo.trailStop > 0.0)
                    drawLeg(wo, wo.trailStop, false, stopCol, stopBg);
                break;
            default: break;
        }
    }

    ImPlot::PopPlotClipRect();
}

// ============================================================================
// Armed line + chart-click handling. Inside BeginPlot/EndPlot.
// ============================================================================

void ReplayWindow::DrawArmedLineAndHandleClick() {
    m_liveCursorPrice = 0.0;
    if (!m_limitArmed) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    ImVec2 pMin = ImPlot::GetPlotPos();
    ImVec2 pMax = ImVec2(pMin.x + ImPlot::GetPlotSize().x,
                         pMin.y + ImPlot::GetPlotSize().y);
    bool hovered = ImGui::IsMouseHoveringRect(pMin, pMax, false);
    if (!hovered) { ImPlot::PopPlotClipRect(); return; }

    ImPlotPoint mp = ImPlot::GetPlotMousePos();
    double cursorPrice = std::round(mp.y / 0.01) * 0.01;
    m_liveCursorPrice = cursorPrice;

    bool isBuy = (m_limitSide == "BUY");
    bool isDual = kOrderTypes[m_orderTypeIdx].isDualPrice;

    auto drawArmed = [&](double linePrice, ImU32 col, ImU32 bg, const char* tag,
                         bool followsCursor) {
        ImVec2 lp0 = ImPlot::PlotToPixels(m_xMin, linePrice);
        ImVec2 lp1 = ImPlot::PlotToPixels(m_xMax, linePrice);
        DrawDashedHLine(dl, pMin.x, pMax.x, lp0.y, col, 2.0f, 8.f, 5.f);

        char bubBuf[80];
        if (m_orderQty > 0) {
            double cost = linePrice * (double)m_orderQty;
            std::snprintf(bubBuf, sizeof(bubBuf), "%s %s $%.2f  ~ $%'.0f",
                          m_limitSide.c_str(), tag, linePrice, cost);
        } else {
            std::snprintf(bubBuf, sizeof(bubBuf), "%s %s $%.2f",
                          m_limitSide.c_str(), tag, linePrice);
        }
        ImVec2 bSz = ImGui::CalcTextSize(bubBuf);
        float mouseX = followsCursor ? ImGui::GetIO().MousePos.x
                                      : (lp0.x + lp1.x) * 0.5f;
        mouseX = std::max(pMin.x + 4.f, std::min(pMax.x - bSz.x - 12.f, mouseX));
        float bx = mouseX, by = lp0.y - bSz.y - 6.f;
        dl->AddRectFilled(ImVec2(bx - 4, by),
                          ImVec2(bx + bSz.x + 6, by + bSz.y + 4), bg, 3.f);
        dl->AddText(ImVec2(bx, by + 2), IM_COL32(255, 255, 255, 255), bubBuf);
    };

    ImU32 stopCol = isBuy ? IM_COL32(80, 140, 255, 220) : IM_COL32(255, 80, 80, 220);
    ImU32 stopBg  = isBuy ? IM_COL32(15,  55, 130, 255) : IM_COL32(130, 25, 25, 255);
    ImU32 lmtCol  = IM_COL32(220, 140, 30, 220);
    ImU32 lmtBg   = IM_COL32(100,  55,  5, 255);

    auto submitOrder = [&](const core::Order& o) {
        if (m_transmitInstantly) {
            if (OnPaperOrderSubmit) {
                int id = OnPaperOrderSubmit(o);
                if (id > 0) {
                    core::services::WorkingOrder wo;
                    wo.localId  = id;
                    wo.order    = o;
                    wo.placedAt = (m_clock.cursorBarIdx >= 0 &&
                                   m_clock.cursorBarIdx < (int)m_xs.size())
                        ? static_cast<std::time_t>(m_xs[m_clock.cursorBarIdx])
                        : std::time(nullptr);
                    m_book.push_back(wo);
                }
            }
            m_limitArmed       = false;
            m_firstPricePlaced = false;
        } else {
            m_pendingConfirmOrder = o;
            m_showConfirmPopup    = true;
        }
    };

    auto buildArmedOrder = [&](double primary, double secondary) -> core::Order {
        const auto& ot = kOrderTypes[m_orderTypeIdx];
        core::TimeInForce tif = ot.tifLocked ? core::TimeInForce::Day
                              : (m_tifIdx == 0 ? core::TimeInForce::Day
                                                : core::TimeInForce::GTC);
        core::Order o;
        o.symbol     = m_symbol;
        o.side       = isBuy ? core::OrderSide::Buy : core::OrderSide::Sell;
        o.type       = ot.coreType;
        o.quantity   = (double)m_orderQty;
        o.tif        = tif;
        o.outsideRth = !ot.noRth;
        if (ot.isDualPrice) {
            if (ot.firstIsAux) { o.auxPrice  = primary; o.limitPrice = secondary; }
            else               { o.stopPrice = primary; o.limitPrice = secondary; }
        } else {
            switch (ot.coreType) {
                case core::OrderType::Stop: o.stopPrice  = primary; break;
                case core::OrderType::MIT:  o.auxPrice   = primary; break;
                default:                    o.limitPrice = primary; break;
            }
        }
        return o;
    };

    if (isDual && m_firstPricePlaced) {
        const char* firstTag = kOrderTypes[m_orderTypeIdx].firstIsAux ? "TRIG" : "STOP";
        drawArmed(m_firstPrice, stopCol, stopBg, firstTag, false);
        drawArmed(cursorPrice,  lmtCol,  lmtBg,  "LMT",  true);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            core::Order o = buildArmedOrder(m_firstPrice, cursorPrice);
            submitOrder(o);
        }
    } else {
        const char* tag = isDual ? "STOP" : "";
        drawArmed(cursorPrice, stopCol, stopBg, tag, true);

        bool ctrlHeld = ImGui::GetIO().KeyCtrl;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (isDual) {
                m_firstPrice       = cursorPrice;
                m_firstPricePlaced = true;
            } else {
                core::Order o = buildArmedOrder(cursorPrice, 0.0);
                if (ctrlHeld || !m_transmitInstantly) {
                    m_pendingConfirmOrder = o;
                    m_showConfirmPopup    = true;
                } else {
                    submitOrder(o);
                }
            }
        }
    }

    ImPlot::PopPlotClipRect();
}

// ============================================================================
// Confirmation popup
// ============================================================================

void ReplayWindow::DrawConfirmPopup() {
    if (m_showConfirmPopup) {
        ImGui::OpenPopup("##rconfirm");
        m_showConfirmPopup = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetWindowViewport()->GetCenter(),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em(300), 0), ImGuiCond_Always);

    if (!ImGui::BeginPopupModal("##rconfirm", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoTitleBar)) return;

    core::Order& o = m_pendingConfirmOrder;
    bool isBuy = (o.side == core::OrderSide::Buy);

    ImGui::PushStyleColor(ImGuiCol_Text,
        isBuy ? ImVec4(0.20f, 0.90f, 0.40f, 1.f)
              : ImVec4(0.95f, 0.30f, 0.30f, 1.f));
    ImGui::Text("  %s PAPER ORDER", isBuy ? "BUY" : "SELL");
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::Text("Symbol:     %s",  o.symbol.c_str());
    ImGui::Text("Type:       %s",  core::OrderTypeStr(o.type));
    ImGui::Text("Quantity:   %.0f shares", o.quantity);

    ImGui::Spacing();
    switch (o.type) {
        case core::OrderType::Limit:
        case core::OrderType::LOC:
            ImGui::Text("Limit:      $%.4f", o.limitPrice); break;
        case core::OrderType::Stop:
            ImGui::Text("Stop:       $%.4f", o.stopPrice); break;
        case core::OrderType::StopLimit:
            ImGui::Text("Stop:       $%.4f", o.stopPrice);
            ImGui::Text("Limit:      $%.4f", o.limitPrice); break;
        case core::OrderType::Trail:
            if (o.trailingPercent > 0.0) ImGui::Text("Trail %%:    %.2f%%", o.trailingPercent);
            else                         ImGui::Text("Trail $:    $%.4f", o.auxPrice);
            break;
        case core::OrderType::TrailLimit:
            if (o.trailingPercent > 0.0) ImGui::Text("Trail %%:    %.2f%%", o.trailingPercent);
            else                         ImGui::Text("Trail $:    $%.4f", o.auxPrice);
            ImGui::Text("Lmt Offset: $%.4f", o.lmtPriceOffset);
            break;
        case core::OrderType::MIT:
            ImGui::Text("Trigger:    $%.4f", o.auxPrice); break;
        case core::OrderType::LIT:
            ImGui::Text("Trigger:    $%.4f", o.auxPrice);
            ImGui::Text("Limit:      $%.4f", o.limitPrice); break;
        case core::OrderType::Relative:
            ImGui::Text("Peg Offset: $%.4f", o.auxPrice); break;
        case core::OrderType::Market:
        case core::OrderType::MOC:
        case core::OrderType::MTL:
        case core::OrderType::Midprice:
            ImGui::TextDisabled("Price:      market"); break;
        default: break;
    }

    ImGui::Spacing();
    ImGui::Text("TIF:        %s", core::TIFStr(o.tif));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
        "PAPER ORDER — does NOT reach the live market.");

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, isBuy
        ? ImVec4(0.12f, 0.55f, 0.25f, 1.0f)
        : ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, isBuy
        ? ImVec4(0.18f, 0.70f, 0.35f, 1.0f)
        : ImVec4(0.80f, 0.22f, 0.22f, 1.0f));
    if (ImGui::Button("Confirm##rcpok", ImVec2(em(130), 0))) {
        if (OnPaperOrderSubmit) {
            int id = OnPaperOrderSubmit(o);
            if (id > 0) {
                core::services::WorkingOrder wo;
                wo.localId  = id;
                wo.order    = o;
                wo.placedAt = (m_clock.cursorBarIdx >= 0 &&
                               m_clock.cursorBarIdx < (int)m_xs.size())
                    ? static_cast<std::time_t>(m_xs[m_clock.cursorBarIdx])
                    : std::time(nullptr);
                m_book.push_back(wo);
            }
        }
        m_limitArmed       = false;
        m_firstPricePlaced = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Cancel##rcpcancel", ImVec2(em(130), 0))) {
        m_limitArmed       = false;
        m_firstPricePlaced = false;
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_limitArmed       = false;
        m_firstPricePlaced = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ============================================================================
// Unguarded-position strip
// ============================================================================

void ReplayWindow::DrawUnguardedStrip() {
    if (!m_unguarded.active) return;
    if (m_unguarded.symbol != m_symbol) return;
    if (m_dismissedUnguarded.count(m_symbol)) return;
    if (m_unguarded.stopTrig <= 0.0) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.30f, 0.24f, 0.05f, 0.90f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);

    ImGui::BeginChild("##unguarded_replay", ImVec2(0, em(22)), ImGuiChildFlags_Borders);
    FlexRow strip;
    const char* side = m_unguarded.qty > 0 ? "LONG" : "SHORT";
    double qty = std::abs(m_unguarded.qty);

    char msg[256];
    std::snprintf(msg, sizeof(msg), "WARNING: %s %.0f %s @ $%.2f — no protective stop. Suggested: $%.2f (-%.1f%%)",
        m_unguarded.symbol.c_str(), qty, side, m_unguarded.avgCost,
        m_unguarded.stopTrig, m_unguarded.pctRisk);
    strip.item(FlexRow::textW(msg), 8);
    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "%s", msg);

    strip.item(em(50), 8);
    if (ImGui::SmallButton("Dismiss"))
        m_dismissedUnguarded.insert(m_symbol);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void ReplayWindow::SetUnguardedSuggestion(const UnguardedHint& h) {
    if (h.active && std::abs(h.qty - m_lastWarnedQty) > 0.001)
        m_dismissedUnguarded.erase(h.symbol);
    m_unguarded = h;
    m_lastWarnedQty = h.qty;
}

// ============================================================================
// Bottom tabs
// ============================================================================

void ReplayWindow::DrawBottomTabs() {
    float availH = ImGui::GetContentRegionAvail().y;
    if (availH < em(60)) return;  // not enough room

    if (!ImGui::BeginTabBar("##replay_tabs")) return;

    // ---- Actual Trades tab -------------------------------------------------
    if (ImGui::BeginTabItem("Actual Trades")) {
        if (m_userFills.empty()) {
            ImGui::TextDisabled("No real fills for this symbol/date.");
        } else {
            if (ImGui::BeginTable("##actual_fills", 6,
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY, ImVec2(0, availH * 0.65f))) {
                ImGui::TableSetupColumn("Time");
                ImGui::TableSetupColumn("Side");
                ImGui::TableSetupColumn("Qty");
                ImGui::TableSetupColumn("Price");
                ImGui::TableSetupColumn("Comm");
                ImGui::TableSetupColumn("PnL");
                ImGui::TableHeadersRow();

                for (size_t i = 0; i < m_userFills.size(); ++i) {
                    const auto& f = m_userFills[i];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    std::time_t ts = f.timestamp;
                    std::tm* tm = std::localtime(&ts);
                    char tbuf[16];
                    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
                    // Double-click → scrub to that bar
                    if (ImGui::Selectable(tbuf, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            std::vector<core::Bar> bars;
                            for (size_t j = 0; j < m_xs.size(); ++j)
                                bars.push_back({m_xs[j], m_opens[j], m_highs[j],
                                                m_lows[j], m_closes[j], m_volumes[j]});
                            core::services::SeekToTime(m_clock, f.timestamp, bars);
                        }
                    }
                    ImGui::TableNextColumn();
                    bool isBuy = (f.side == core::OrderSide::Buy);
                    ImGui::TextColored(isBuy ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                                             : ImVec4(0.9f, 0.3f, 0.3f, 1.f),
                        "%s", core::OrderSideStr(f.side));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", f.quantity);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", f.price);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", f.commission);
                    ImGui::TableNextColumn();
                    if (f.realizedPnL != 0.0) {
                        ImGui::TextColored(f.realizedPnL > 0
                            ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                            : ImVec4(0.9f, 0.3f, 0.3f, 1.f),
                            "%+.2f", f.realizedPnL);
                    } else {
                        ImGui::Text("-");
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    // ---- Simulated Orders tab (Operate mode only) --------------------------
    if (m_mode == Mode::Operate && ImGui::BeginTabItem("Sim Orders")) {
        if (m_simFills.empty() && m_book.empty()) {
            ImGui::TextDisabled("No simulated activity yet. Place an order and hit Play.");
        } else {
            if (ImGui::BeginTable("##sim_fills", 6,
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY, ImVec2(0, availH * 0.5f))) {
                ImGui::TableSetupColumn("Time");
                ImGui::TableSetupColumn("Side");
                ImGui::TableSetupColumn("Qty");
                ImGui::TableSetupColumn("Price");
                ImGui::TableSetupColumn("Note");
                ImGui::TableSetupColumn("ID");
                ImGui::TableHeadersRow();

                // Show filled orders first, then working
                for (const auto& f : m_simFills) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    std::time_t ts = f.time;
                    std::tm* tm = std::localtime(&ts);
                    char tbuf[16];
                    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
                    // Double-click → scrub to fill time
                    if (ImGui::Selectable(tbuf, false, ImGuiSelectableFlags_SpanAllColumns)) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            std::vector<core::Bar> bars;
                            for (size_t j = 0; j < m_xs.size(); ++j)
                                bars.push_back({m_xs[j], m_opens[j], m_highs[j],
                                                m_lows[j], m_closes[j], m_volumes[j]});
                            core::services::SeekToTime(m_clock, f.time, bars);
                        }
                    }
                    ImGui::TableNextColumn();
                    bool isBuy = (f.side == core::OrderSide::Buy);
                    ImGui::TextColored(isBuy ? ImVec4(0.2f, 0.8f, 0.4f, 1.f)
                                             : ImVec4(0.9f, 0.3f, 0.3f, 1.f),
                        "%s", core::OrderSideStr(f.side));
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f", f.qty);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", f.price);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", f.intentNote.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("#%d", f.intentOrderId);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndTabItem();
    }

    // ---- AI Analysis tab ---------------------------------------------------
    if (ImGui::BeginTabItem("AI Analysis")) {
        ImGui::TextDisabled("Phase 15 — Configure API keys in Settings to enable AI analysis.");
        ImGui::Spacing();

        if (ImGui::Button("Copy day summary to clipboard", ImVec2(em(200), 0))) {
            // Build a Markdown summary ready for pasting into any AI provider
            std::string md;
            md += "## Replay Summary\n\n";
            md += "- **Symbol:** " + std::string(m_symbol) + "\n";
            if (std::strcmp(m_dateFromBuf, m_dateToBuf) == 0)
                md += "- **Date:** " + std::string(m_dateFromBuf) + "\n";
            else
                md += "- **Date range:** " + std::string(m_dateFromBuf)
                    + " → " + std::string(m_dateToBuf) + "\n";
            md += "- **Session:** " + std::string(core::services::ReplaySessionLabel(m_session)) + "\n";
            md += "- **TF:** " + std::string(core::TimeframeLabel(m_tf)) + "\n";
            md += "- **Bar count:** " + std::to_string(m_idxs.size()) + "\n";

            if (!m_userFills.empty()) {
                md += "\n### Real Trades\n\n";
                double rPnL = 0.0;
                for (const auto& f : m_userFills) {
                    rPnL += f.realizedPnL;
                    std::time_t ts = f.timestamp;
                    std::tm* tm = std::localtime(&ts);
                    char tbuf[16];
                    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
                    md += "- " + std::string(tbuf) + " " + std::string(core::OrderSideStr(f.side))
                       + " " + std::to_string((int)f.quantity) + "@$"
                       + std::to_string(f.price) + " PnL:$"
                       + std::to_string(f.realizedPnL) + "\n";
                }
                md += "\n**Total real PnL: $" + std::to_string(rPnL) + "**\n";
            }

            if (m_mode == Mode::Operate && !m_simFills.empty()) {
                md += "\n### Simulated Trades\n\n";
                for (const auto& f : m_simFills) {
                    std::time_t ts = f.time;
                    std::tm* tm = std::localtime(&ts);
                    char tbuf[16];
                    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
                    md += "- " + std::string(tbuf) + " " + std::string(core::OrderSideStr(f.side))
                       + " " + std::to_string((int)f.qty) + "@$"
                       + std::to_string(f.price) + " (" + f.intentNote + ")\n";
                }
                md += "\n**Sim PnL: $" + std::to_string(m_account.realizedPnL) + "**\n";
            }

            ImGui::SetClipboardText(md.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copies a Markdown summary to clipboard.\n"
                              "Paste it into Claude.ai, ChatGPT, or DeepSeek\n"
                              "for a post-session analysis.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Provider setup (Phase 15):");
        ImGui::BeginDisabled();
        ImGui::Button("Anthropic — Configure API key in Settings", ImVec2(em(280), 0));
        ImGui::Button("OpenAI — Configure API key in Settings", ImVec2(em(280), 0));
        ImGui::Button("DeepSeek — Configure API key in Settings", ImVec2(em(280), 0));
        ImGui::EndDisabled();

        ImGui::EndTabItem();
    }

    // ---- Stats tab ---------------------------------------------------------
    if (ImGui::BeginTabItem("Stats")) {
        // Real PnL from user fills
        double realPnL = 0.0;
        int    realTrades = 0, realWins = 0;
        for (const auto& f : m_userFills) {
            if (f.realizedPnL != 0.0) {
                realPnL += f.realizedPnL;
                ++realTrades;
                if (f.realizedPnL > 0) ++realWins;
            }
        }

        // Sim PnL from engine
        double simPnL = (m_mode == Mode::Operate) ? m_account.realizedPnL : 0.0;
        int    simTrades = (int)m_simFills.size();

        ImGui::Text("Realized PnL (real):  $%+.2f  (%d trades, %d wins)",
            realPnL, realTrades, realWins);
        if (realTrades > 0)
            ImGui::Text("Win rate (real):       %.0f%%", 100.0 * realWins / realTrades);

        ImGui::Separator();

        double lastPx = m_clock.cursorBarIdx >= 0 && m_clock.cursorBarIdx < (int)m_closes.size()
            ? m_closes[m_clock.cursorBarIdx] : 0.0;
        double simEquity = (m_mode == Mode::Operate)
            ? core::services::Equity(m_account, lastPx) : m_startingCash;

        ImGui::Text("Sim equity:            $%.2f", simEquity);
        ImGui::Text("Sim PnL:               $%+.2f", simPnL);
        ImGui::Text("Sim trades:            %d", simTrades);
        ImGui::Text("Sim commission paid:   $%.2f", m_account.commissionPaid);

        ImGui::Separator();
        ImGui::Text("Bar count:             %d", (int)m_idxs.size());
        ImGui::Text("Session range:         %d – %d",
            m_clock.sessionFirstIdx, m_clock.sessionLastIdx);

        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
}

void ReplayWindow::SeekToTime(std::time_t t) {
    std::vector<core::Bar> bars;
    for (size_t j = 0; j < m_xs.size(); ++j)
        bars.push_back({m_xs[j], m_opens[j], m_highs[j], m_lows[j], m_closes[j], m_volumes[j]});
    core::services::SeekToTime(m_clock, t, bars);
}

int ReplayWindow::XTickFormatter(double idx, char* buf, int size, void* userData) {
    auto* self = static_cast<ReplayWindow*>(userData);
    int i = (int)std::round(idx);
    if (i < 0 || i >= (int)self->m_xs.size()) {
        if (size > 0) buf[0] = '\0';
        return 0;
    }
    std::time_t t = (std::time_t)self->m_xs[i];
    // Intraday → HH:MM, non-intraday → YYYY-MM-DD
    if (core::TimeframeSeconds(self->m_tf) < 86400) {
        std::tm* tm = std::localtime(&t);
        if (!tm) return 0;
        return (int)std::strftime(buf, (size_t)size, "%H:%M", tm);
    } else {
        std::tm* tm = std::gmtime(&t);
        if (!tm) return 0;
        return (int)std::strftime(buf, (size_t)size, "%Y-%m-%d", tm);
    }
}

void ReplayWindow::ApplyEngineTick() {
    if (!m_hasData) return;
    int n = static_cast<int>(m_idxs.size());
    if (n == 0) return;

    // Advance clock — always, regardless of mode. The cursor and chart
    // must move in both Analysis and Operate modes.
    double barSec = static_cast<double>(core::TimeframeSeconds(m_tf));
    double deltaSec = ImGui::GetIO().DeltaTime;
    core::services::Tick(m_clock, deltaSec, barSec);

    int curIdx = m_clock.cursorBarIdx;

    // Fire cursor-move callback for group-time-sync whenever cursor changes
    if (curIdx != m_lastFiredCursorIdx && curIdx >= 0 && curIdx < n && OnCursorMove) {
        m_lastFiredCursorIdx = curIdx;
        OnCursorMove(static_cast<std::time_t>(m_xs[curIdx]));
    }

    // On first tick after data load or after a scrub/seek, sync without evaluating
    if (m_lastProcessedIdx < 0 || m_clock.scrubbing) {
        m_lastProcessedIdx = curIdx;
        return;
    }

    // Bar evaluation and fill simulation only in Operate mode
    if (m_mode != Mode::Operate) return;

    // Evaluate bars the cursor crossed
    if (curIdx > m_lastProcessedIdx && m_clock.speed > 0.0 && !m_clock.paused) {
        for (int i = m_lastProcessedIdx + 1; i <= curIdx && i < n; ++i) {
            core::Bar bar;
            bar.timestamp = m_xs[i];
            bar.open      = m_opens[i];
            bar.high      = m_highs[i];
            bar.low       = m_lows[i];
            bar.close     = m_closes[i];
            bar.volume    = m_volumes[i];

            bool isLast = (i == m_clock.sessionLastIdx);
            auto result = core::services::EvaluateBar(m_book, bar,
                core::services::kDefaultCommissionPerShare, isLast);

            // Apply fills
            for (auto& f : result.fills) {
                core::services::ApplyFill(m_account, f);
                m_simFills.push_back(f);
            }

            // Remove filled/cancelled orders
            for (int localId : result.filledIds) {
                auto it = std::find_if(m_book.begin(), m_book.end(),
                    [localId](const core::services::WorkingOrder& wo) {
                        return wo.localId == localId;
                    });
                if (it != m_book.end()) m_book.erase(it);
            }
        }
    }
    m_lastProcessedIdx = curIdx;
}

}  // namespace ui
