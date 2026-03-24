#include "ScannerWindow.h"

#include "imgui.h"
#include "core/models/WindowGroup.h"
#include "implot.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>

namespace ui {

// ============================================================================
// Preset descriptors
// ============================================================================

static const core::ScanPreset kPresets[] = {
    core::ScanPreset::TopGainers,
    core::ScanPreset::TopLosers,
    core::ScanPreset::VolumeLeaders,
    core::ScanPreset::MostActive,
    core::ScanPreset::NewHighs,
    core::ScanPreset::NewLows,
    core::ScanPreset::RSIOverbought,
    core::ScanPreset::RSIOversold,
    core::ScanPreset::NearEarnings,
    core::ScanPreset::Custom,
};
static constexpr int kNumPresets = static_cast<int>(std::size(kPresets));

// ============================================================================
// Constructor
// ============================================================================

ScannerWindow::ScannerWindow()
    : m_rng(std::random_device{}())
{
    // Use epoch so the elapsed check in Render() fires on the very first frame,
    // giving an immediate initial scan rather than waiting m_autoRefreshSec.
    m_lastScanTime    = Clock::time_point{};
    m_lastQuoteUpdate = Clock::now();
}

// ============================================================================
// Public: IB Gateway stubs
// ============================================================================

// ============================================================================
// Technical indicator helpers (file-scope, no state needed)
// ============================================================================

// Compute RSI, approximate MACD and ATR from a sparkline of close prices.
// Requires at least 2 data points; more points → more accurate.
static void ComputeTechnicals(core::ScanResult& r)
{
    const auto& sp = r.sparkline;
    int n = static_cast<int>(sp.size());
    if (n < 2) return;

    // ── RSI (Wilder, 14-period) ──────────────────────────────────────────
    {
        int period = std::min(14, n - 1);
        double gain = 0.0, loss = 0.0;
        int start = n - period;
        for (int i = start; i < n; ++i) {
            double d = sp[i] - sp[i - 1];
            if (d > 0) gain += d; else loss -= d;
        }
        gain /= period;  loss /= period;
        if (loss < 1e-12) r.rsi = 100.0;
        else              r.rsi = 100.0 - 100.0 / (1.0 + gain / loss);
    }

    // ── MACD (EMA12 − EMA26, signal = EMA9 of macd) ─────────────────────
    {
        // Use however many points we have; clamp periods to available data
        int p12 = std::min(12, n);
        int p26 = std::min(26, n);
        double k12 = 2.0 / (p12 + 1);
        double k26 = 2.0 / (p26 + 1);
        double e12 = sp[0], e26 = sp[0];
        for (int i = 1; i < n; ++i) {
            e12 = sp[i] * k12 + e12 * (1.0 - k12);
            e26 = sp[i] * k26 + e26 * (1.0 - k26);
        }
        r.macdLine = e12 - e26;
        // Signal: EMA9 of macd — approximate from the last few macd values
        // by computing EMA9 over a synthetic series derived from EMA12-EMA26
        // for each recent bar (backwards-compatible approximation)
        int p9 = std::min(9, n);
        double k9 = 2.0 / (p9 + 1);
        r.macdSignal = r.macdLine * k9 + r.macdSignal * (1.0 - k9);
    }

    // ── ATR approximation (close-to-close true range, no H/L available) ──
    {
        int period = std::min(14, n - 1);
        double sum = 0.0;
        int start = n - period;
        for (int i = start; i < n; ++i)
            sum += std::abs(sp[i] - sp[i - 1]);
        r.atr = sum / period;
    }
}

// ============================================================================
// Public: IB Gateway stubs
// ============================================================================

void ScannerWindow::OnScanData(int /*reqId*/,
                               const std::vector<core::ScanResult>& results)
{
    m_scanning     = false;
    m_lastScanTime = Clock::now();   // prevent immediate re-scan
    // Update the displayed timestamp (only reachable via IB path; simulation
    // updates it at the bottom of RunScan() which doesn't early-return).
    std::time_t now2 = std::time(nullptr);
    if (std::tm* tm2 = std::localtime(&now2))
        std::strftime(m_lastScanTimeStr, sizeof(m_lastScanTimeStr), "%H:%M:%S", tm2);
    if (results.empty()) return;     // scan failed (e.g. IND/FUT on paper) — don't wipe table
    m_hasRealData = true;
    m_results = results;
    SortResults();
}

void ScannerWindow::OnQuoteUpdate(const std::string& symbol, double price,
                                  double change, double changePct, double volume)
{
    for (auto& r : m_results) {
        if (r.symbol != symbol) continue;

        if (price > 0.0) {
            r.price     = price;
            r.change    = change;
            r.changePct = changePct;
            // Derive prevClose from first tick if not already set
            if (r.prevClose <= 0.0 && change != 0.0)
                r.prevClose = price - change;
            r.high = r.high > 0.0 ? std::max(r.high, price) : price;
            r.low  = r.low  > 0.0 ? std::min(r.low,  price) : price;

            // Update pctFrom52H / pctFrom52L whenever price changes
            if (r.high52 > 0.0)
                r.pctFrom52H = ((price - r.high52) / r.high52) * 100.0;
            if (r.low52 > 0.0)
                r.pctFrom52L = ((price - r.low52) / r.low52) * 100.0;

            // Grow / slide sparkline (max 40 points so MACD has enough history)
            static constexpr int kSparkLen = 40;
            if ((int)r.sparkline.size() < kSparkLen)
                r.sparkline.push_back(static_cast<float>(price));
            else {
                r.sparkline.erase(r.sparkline.begin());
                r.sparkline.push_back(static_cast<float>(price));
            }
            ComputeTechnicals(r);
        }

        if (volume > 0.0) {
            r.volume    = volume;
            r.relVolume = r.avgVolume > 0.0 ? volume / r.avgVolume : 0.0;
        }

        r.updatedAt = std::time(nullptr);
        break;
    }
}

void ScannerWindow::Set52WHigh(const std::string& symbol, double high52)
{
    for (auto& r : m_results) {
        if (r.symbol != symbol) continue;
        r.high52     = high52;
        if (r.price > 0.0 && high52 > 0.0)
            r.pctFrom52H = ((r.price - high52) / high52) * 100.0;
        break;
    }
}

void ScannerWindow::Set52WLow(const std::string& symbol, double low52)
{
    for (auto& r : m_results) {
        if (r.symbol != symbol) continue;
        r.low52      = low52;
        if (r.price > 0.0 && low52 > 0.0)
            r.pctFrom52L = ((r.price - low52) / low52) * 100.0;
        break;
    }
}

void ScannerWindow::SetAvgVolume(const std::string& symbol, double avgVol)
{
    for (auto& r : m_results) {
        if (r.symbol != symbol) continue;
        r.avgVolume  = avgVol;
        r.relVolume  = avgVol > 0.0 ? r.volume / avgVol : 0.0;
        break;
    }
}

void ScannerWindow::SetPrevClose(const std::string& symbol, double prevClose)
{
    if (prevClose <= 0.0) return;
    for (auto& r : m_results) {
        if (r.symbol != symbol) continue;
        r.prevClose = prevClose;
        // Recompute change / changePct now that we have a reliable prevClose
        if (r.price > 0.0) {
            r.change    = r.price - prevClose;
            r.changePct = (r.change / prevClose) * 100.0;
        }
        break;
    }
}

void ScannerWindow::SetPortfolioSymbols(const std::vector<std::string>& symbols)
{
    m_portfolioSymbols = symbols;
}

void ScannerWindow::setInstanceId(int id) {
    m_instanceId = id;
    std::snprintf(m_title, sizeof(m_title), "Market Scanner %d##scanner%d", id, id);
}

// ============================================================================
// Render
// ============================================================================

bool ScannerWindow::Render()
{
    if (!m_open) return false;

    // Auto-refresh timer.
    // For IB connections (OnScanRequest set): re-subscribe to get a fresh server ranking.
    // For simulation (no connection): skip regenerating from seeds — the 0.5s quote drift
    // already keeps prices live; only run a full scan if we have no results yet.
    if (m_autoRefresh) {
        auto now = Clock::now();
        float elapsed = std::chrono::duration<float>(now - m_lastScanTime).count();
        if (elapsed >= m_autoRefreshSec) {
            if (OnScanRequest || m_results.empty()) {
                RunScan();
            } else {
                // Simulation with existing data: just reset the timer so columns
                // keep drifting without a full seed-reset.
                m_lastScanTime = now;
            }
        }
    }

    // Live quote drift (every 0.5s) — only when no real IB data
    if (!m_hasRealData) {
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - m_lastQuoteUpdate).count();
        if (dt >= 0.5f) {
            UpdateQuotes(dt);
            m_lastQuoteUpdate = now;
        }
    }

    ImGui::SetNextWindowSize(ImVec2(1100, 660), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(m_title, &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    DrawToolbar();
    if (m_showFilters) DrawFilterBar();
    DrawResultsTable();
    DrawDetailPanel();
    DrawStatusBar();

    ImGui::End();
    return m_open;
}

// ============================================================================
// DrawToolbar
// ============================================================================

void ScannerWindow::DrawToolbar()
{
    core::DrawGroupPicker(m_groupId, "##scanner_grp");
    ImGui::SameLine(0, 10);

    // Asset class tabs
    const core::AssetClass classes[] = {
        core::AssetClass::Stocks,
        core::AssetClass::Indexes,
        core::AssetClass::ETFs,
        core::AssetClass::Futures,
    };
    for (auto ac : classes) {
        bool active = (ac == m_activeClass);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(core::AssetClassLabel(ac))) {
            m_activeClass = ac;
            RunScan();
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    ImGui::SameLine(0, 8);
    ImGui::TextDisabled("|");
    ImGui::SameLine(0, 8);

    // Preset combo
    ImGui::SetNextItemWidth(180);
    const char* previewLabel = core::ScanPresetLabel(kPresets[m_presetIdx]);
    if (ImGui::BeginCombo("##preset", previewLabel)) {
        for (int i = 0; i < kNumPresets; ++i) {
            bool sel = (i == m_presetIdx);
            if (ImGui::Selectable(core::ScanPresetLabel(kPresets[i]), sel)) {
                m_presetIdx = i;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    // Scan button
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.22f, 0.70f, 0.22f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.12f, 0.40f, 0.12f, 1.f));
    if (ImGui::Button("  Scan  ")) RunScan();
    ImGui::PopStyleColor(3);
    ImGui::SameLine();

    // Symbol search
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##search", "Search symbol/name…",
                              m_searchBuf, sizeof(m_searchBuf));
    ImGui::SameLine();

    // Filters toggle
    if (ImGui::Button(m_showFilters ? "Filters [-]" : "Filters [+]"))
        m_showFilters = !m_showFilters;
    ImGui::SameLine();

    // Column chooser
    if (ImGui::Button("Cols")) ImGui::OpenPopup("##ColChooser");
    DrawColumnChooserPopup();
    ImGui::SameLine();

    // Auto refresh toggle
    if (m_autoRefresh) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.60f, 1.f));
        if (ImGui::Button("Auto ON")) m_autoRefresh = false;
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Auto OFF")) m_autoRefresh = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto-refresh every %.0f seconds", m_autoRefreshSec);
    }
    ImGui::SameLine();

    // Refresh interval slider
    ImGui::SetNextItemWidth(80);
    ImGui::SliderFloat("##interval", &m_autoRefreshSec, 5.f, 120.f, "%.0fs");

}

// ============================================================================
// DrawFilterBar
// ============================================================================

void ScannerWindow::DrawFilterBar()
{
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.f));
    ImGui::BeginChild("##filterbar", ImVec2(0, 52), false, ImGuiWindowFlags_NoScrollbar);

    float w = 70.f;

    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##minP", m_minPriceBuf, sizeof(m_minPriceBuf));
    ImGui::SameLine(0, 2); ImGui::TextUnformatted("≤ Price ≤"); ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##maxP", m_maxPriceBuf, sizeof(m_maxPriceBuf));
    ImGui::SameLine(0, 16);

    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##minC", m_minChgBuf, sizeof(m_minChgBuf));
    ImGui::SameLine(0, 2); ImGui::TextUnformatted("≤ Chg% ≤"); ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(w);
    ImGui::InputText("##maxC", m_maxChgBuf, sizeof(m_maxChgBuf));
    ImGui::SameLine(0, 16);

    ImGui::TextUnformatted("Min Vol");  ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(80);
    ImGui::InputText("##minVol", m_minVolBuf, sizeof(m_minVolBuf));
    ImGui::SameLine(0, 16);

    ImGui::TextUnformatted("Sector");  ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(100);
    ImGui::InputText("##sector", m_sectorBuf, sizeof(m_sectorBuf));
    ImGui::SameLine(0, 16);

    if (ImGui::Button("Apply")) {
        m_filter.minPrice    = std::atof(m_minPriceBuf);
        m_filter.maxPrice    = std::atof(m_maxPriceBuf);
        m_filter.minChangePct= std::atof(m_minChgBuf);
        m_filter.maxChangePct= std::atof(m_maxChgBuf);
        m_filter.minVolume   = std::atof(m_minVolBuf);
        m_filter.sector      = m_sectorBuf;
        RunScan();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        m_filter = core::ScanFilter{};
        m_minPriceBuf[0] = '\0'; std::strcat(m_minPriceBuf, "0");
        m_maxPriceBuf[0] = '\0'; std::strcat(m_maxPriceBuf, "99999");
        m_minChgBuf[0]   = '\0'; std::strcat(m_minChgBuf, "-100");
        m_maxChgBuf[0]   = '\0'; std::strcat(m_maxChgBuf, "100");
        m_minVolBuf[0]   = '\0'; std::strcat(m_minVolBuf, "0");
        m_sectorBuf[0]   = '\0';
        RunScan();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================================
// DrawColumnChooserPopup
// ============================================================================

void ScannerWindow::DrawColumnChooserPopup()
{
    if (!ImGui::BeginPopup("##ColChooser")) return;
    ImGui::TextUnformatted("Visible Columns");
    ImGui::Separator();
    ImGui::Checkbox("Company",     &m_showCompany);
    ImGui::Checkbox("Change $",    &m_showChange);
    ImGui::Checkbox("Change %",    &m_showChangePct);
    ImGui::Checkbox("Volume",      &m_showVolume);
    ImGui::Checkbox("Rel.Volume",  &m_showRelVol);
    ImGui::Checkbox("Mkt Cap",     &m_showMktCap);
    ImGui::Checkbox("P/E",         &m_showPE);
    ImGui::Checkbox("52W High",    &m_showHigh52);
    ImGui::Checkbox("52W Low",     &m_showLow52);
    ImGui::Checkbox("% from High", &m_showPctH52);
    ImGui::Checkbox("RSI",         &m_showRSI);
    ImGui::Checkbox("MACD",        &m_showMACD);
    ImGui::Checkbox("ATR",         &m_showATR);
    ImGui::Checkbox("Sparkline",   &m_showSparkline);
    ImGui::EndPopup();
}

// ============================================================================
// DrawResultsTable
// ============================================================================

void ScannerWindow::DrawResultsTable()
{
    ImGui::Separator();

    // Reserve bottom for detail panel + status bar
    float tableHeight = ImGui::GetContentRegionAvail().y - 110.0f;
    if (tableHeight < 80.f) tableHeight = 80.f;

    // Count visible columns
    int colCount = 2; // Symbol + Price always visible
    if (m_showCompany)   ++colCount;
    if (m_showChange)    ++colCount;
    if (m_showChangePct) ++colCount;
    if (m_showVolume)    ++colCount;
    if (m_showRelVol)    ++colCount;
    if (m_showMktCap)    ++colCount;
    if (m_showPE)        ++colCount;
    if (m_showHigh52)    ++colCount;
    if (m_showLow52)     ++colCount;
    if (m_showPctH52)    ++colCount;
    if (m_showRSI)       ++colCount;
    if (m_showMACD)      ++colCount;
    if (m_showATR)       ++colCount;
    if (m_showSparkline) ++colCount;

    ImGuiTableFlags tflags =
        ImGuiTableFlags_ScrollY        |
        ImGuiTableFlags_RowBg          |
        ImGuiTableFlags_BordersOuter   |
        ImGuiTableFlags_BordersV       |
        ImGuiTableFlags_Resizable      |
        ImGuiTableFlags_Sortable       |
        ImGuiTableFlags_SortTristate   |
        ImGuiTableFlags_Hideable       |
        ImGuiTableFlags_SizingFixedFit;

    if (!ImGui::BeginTable("##scanner", colCount, tflags,
                            ImVec2(0, tableHeight)))
        return;

    // --- Column headers ---
    auto ColHdr = [](const char* label, ImGuiTableColumnFlags flags = 0,
                     float width = 0.f) {
        constexpr ImGuiTableColumnFlags kSizingMask =
            ImGuiTableColumnFlags_WidthFixed  |
            ImGuiTableColumnFlags_WidthStretch;
        if (width > 0.f && !(flags & kSizingMask))
            flags |= ImGuiTableColumnFlags_WidthFixed;
        ImGui::TableSetupColumn(label,
            ImGuiTableColumnFlags_DefaultSort | flags,
            width);
    };

    ColHdr("Symbol",   0, 70.f);
    if (m_showCompany)   ColHdr("Company",  ImGuiTableColumnFlags_WidthStretch);
    ColHdr("Price",    0, 72.f);
    if (m_showChange)    ColHdr("Chg $",   0, 62.f);
    if (m_showChangePct) ColHdr("Chg %",   0, 62.f);
    if (m_showVolume)    ColHdr("Volume",  0, 80.f);
    if (m_showRelVol)    ColHdr("RelVol",  0, 58.f);
    if (m_showMktCap)    ColHdr("MktCap",  0, 72.f);
    if (m_showPE)        ColHdr("P/E",     0, 52.f);
    if (m_showHigh52)    ColHdr("52W Hi",  0, 68.f);
    if (m_showLow52)     ColHdr("52W Lo",  0, 68.f);
    if (m_showPctH52)    ColHdr("%Hi",     0, 58.f);
    if (m_showRSI)       ColHdr("RSI",     0, 50.f);
    if (m_showMACD)      ColHdr("MACD",    0, 58.f);
    if (m_showATR)       ColHdr("ATR",     0, 52.f);
    if (m_showSparkline) ColHdr("Trend",   ImGuiTableColumnFlags_NoSort, 80.f);

    ImGui::TableHeadersRow();

    // --- Sorting ---
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            const auto& s = specs->Specs[0];
            // Map column index → ScanColumn. Each pick() call consumes one column
            // slot (via post-increment) and compares against ColumnIndex directly.
            int ci  = s.ColumnIndex;
            int col = 0;
            auto pick = [&](core::ScanColumn sc) {
                if (ci == col) {
                    m_sortCol       = sc;
                    m_sortAscending = (s.SortDirection == ImGuiSortDirection_Ascending);
                }
                ++col;
            };
            pick(core::ScanColumn::Symbol);
            if (m_showCompany)   pick(core::ScanColumn::Company);
            pick(core::ScanColumn::Price);
            if (m_showChange)    pick(core::ScanColumn::Change);
            if (m_showChangePct) pick(core::ScanColumn::ChangePct);
            if (m_showVolume)    pick(core::ScanColumn::Volume);
            if (m_showRelVol)    pick(core::ScanColumn::RelVolume);
            if (m_showMktCap)    pick(core::ScanColumn::MktCap);
            if (m_showPE)        pick(core::ScanColumn::PE);
            if (m_showHigh52)    pick(core::ScanColumn::High52);
            if (m_showLow52)     pick(core::ScanColumn::Low52);
            if (m_showPctH52)    pick(core::ScanColumn::PctFrom52H);
            if (m_showRSI)       pick(core::ScanColumn::RSI);
            if (m_showMACD)      pick(core::ScanColumn::MACD);
            if (m_showATR)       pick(core::ScanColumn::ATR);
            SortResults();
            specs->SpecsDirty = false;
        }
    }

    // --- Rows ---
    for (int i = 0; i < static_cast<int>(m_results.size()); ++i) {
        const core::ScanResult& r = m_results[i];

        if (!MatchesFilter(r)) continue;
        if (!MatchesSearch(r)) continue;

        bool inPortfolio = std::find(m_portfolioSymbols.begin(),
                                     m_portfolioSymbols.end(),
                                     r.symbol) != m_portfolioSymbols.end();

        ImGui::TableNextRow();

        // Row background tint
        if (m_selectedRow == i) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f,0.3f,0.5f,0.6f)));
        } else if (inPortfolio) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f,0.20f,0.08f,0.4f)));
        } else if (r.changePct > 0) {
            float alpha = std::min(0.20f, (float)(r.changePct / 10.0) * 0.20f);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.0f,0.25f,0.0f,alpha)));
        } else if (r.changePct < 0) {
            float alpha = std::min(0.20f, (float)(-r.changePct / 10.0) * 0.20f);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f,0.0f,0.0f,alpha)));
        }

        // --- Symbol cell (selectable spanning full row) ---
        ImGui::TableSetColumnIndex(0);
        char selId[32];
        std::snprintf(selId, sizeof(selId), "##row%d", i);
        bool rowSel = (m_selectedRow == i);

        ImGui::PushStyleColor(ImGuiCol_Header,
            ImVec4(0.20f,0.30f,0.50f,0.60f));

        if (ImGui::Selectable(r.symbol.c_str(), rowSel,
                ImGuiSelectableFlags_SpanAllColumns |
                ImGuiSelectableFlags_AllowOverlap,
                ImVec2(0, 0))) {
            m_selectedRow = i;
            if (OnSymbolSelected) OnSymbolSelected(r.symbol);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (OnSymbolSelected) OnSymbolSelected(r.symbol);
        }
        ImGui::PopStyleColor();

        // --- Remaining cells ---
        int col = 1;

        if (m_showCompany) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextUnformatted(r.company.c_str());
        }

        // Price
        ImGui::TableSetColumnIndex(col++);
        ImGui::Text("%.2f", r.price);

        // Change $
        if (m_showChange) {
            ImGui::TableSetColumnIndex(col++);
            ImVec4 c = r.change >= 0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                     : ImVec4(0.9f,0.3f,0.3f,1.f);
            ImGui::TextColored(c, "%+.2f", r.change);
        }

        // Change %
        if (m_showChangePct) {
            ImGui::TableSetColumnIndex(col++);
            ImVec4 c = r.changePct >= 0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                        : ImVec4(0.9f,0.3f,0.3f,1.f);
            ImGui::TextColored(c, "%+.2f%%", r.changePct);
        }

        // Volume
        if (m_showVolume) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextUnformatted(FmtVolume(r.volume).c_str());
        }

        // Rel Volume
        if (m_showRelVol) {
            ImGui::TableSetColumnIndex(col++);
            if (r.avgVolume > 0.0) {
                ImVec4 c = r.relVolume >= 1.5 ? ImVec4(0.9f,0.8f,0.1f,1.f)
                                               : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TextColored(c, "%.2fx", r.relVolume);
            } else {
                ImGui::TextDisabled("—");
            }
        }

        // Mkt Cap
        if (m_showMktCap) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextUnformatted(FmtMktCap(r.mktCapM).c_str());
        }

        // P/E
        if (m_showPE) {
            ImGui::TableSetColumnIndex(col++);
            if (r.pe > 0) ImGui::Text("%.1f", r.pe);
            else          ImGui::TextUnformatted("—");
        }

        // 52W High
        if (m_showHigh52) {
            ImGui::TableSetColumnIndex(col++);
            if (r.high52 > 0.0) ImGui::Text("%.2f", r.high52);
            else                 ImGui::TextDisabled("—");
        }

        // 52W Low
        if (m_showLow52) {
            ImGui::TableSetColumnIndex(col++);
            if (r.low52 > 0.0) ImGui::Text("%.2f", r.low52);
            else                ImGui::TextDisabled("—");
        }

        // % from 52W High
        if (m_showPctH52) {
            ImGui::TableSetColumnIndex(col++);
            if (r.high52 > 0.0) {
                ImVec4 c = r.pctFrom52H > -2.0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                           : r.pctFrom52H < -20.0 ? ImVec4(0.9f,0.3f,0.3f,1.f)
                           : ImGui::GetStyleColorVec4(ImGuiCol_Text);
                ImGui::TextColored(c, "%.1f%%", r.pctFrom52H);
            } else {
                ImGui::TextDisabled("—");
            }
        }

        // RSI
        if (m_showRSI) {
            ImGui::TableSetColumnIndex(col++);
            ImVec4 c;
            if      (r.rsi >= 70) c = ImVec4(0.9f,0.3f,0.3f,1.f);
            else if (r.rsi <= 30) c = ImVec4(0.3f,0.9f,0.3f,1.f);
            else                  c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::TextColored(c, "%.0f", r.rsi);
        }

        // MACD
        if (m_showMACD) {
            ImGui::TableSetColumnIndex(col++);
            ImVec4 c = r.macdLine >= r.macdSignal ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                                   : ImVec4(0.9f,0.3f,0.3f,1.f);
            ImGui::TextColored(c, "%+.3f", r.macdLine - r.macdSignal);
        }

        // ATR
        if (m_showATR) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::Text("%.2f", r.atr);
        }

        // Sparkline (using ImPlot mini-chart)
        if (m_showSparkline) {
            ImGui::TableSetColumnIndex(col++);
            if (!r.sparkline.empty()) {
                // Draw a tiny sparkline using ImPlot
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float   w = avail.x;
                float   h = 24.f;

                // Determine colour from trend
                bool up = r.sparkline.back() >= r.sparkline.front();
                ImVec4 lineCol = up ? ImVec4(0.2f,0.8f,0.2f,1.f)
                                    : ImVec4(0.8f,0.2f,0.2f,1.f);

                std::string pid = "##spark" + r.symbol;
                ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(0,0));
                ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 1.5f);
                ImPlot::PushStyleColor(ImPlotCol_Line, lineCol);
                ImPlot::PushStyleColor(ImPlotCol_PlotBg,  ImVec4(0,0,0,0));
                ImPlot::PushStyleColor(ImPlotCol_PlotBorder, ImVec4(0,0,0,0));

                ImPlotFlags pf = ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs;
                ImPlotAxisFlags af = ImPlotAxisFlags_NoDecorations;
                if (ImPlot::BeginPlot(pid.c_str(), ImVec2(w, h), pf)) {
                    ImPlot::SetupAxes(nullptr, nullptr, af, af);
                    int n = static_cast<int>(r.sparkline.size());
                    ImPlot::PlotLine("##sl", r.sparkline.data(), n);
                    ImPlot::EndPlot();
                }

                ImPlot::PopStyleColor(3);
                ImPlot::PopStyleVar(2);
            }
        }
    }

    ImGui::EndTable();
}

// ============================================================================
// DrawDetailPanel
// ============================================================================

void ScannerWindow::DrawDetailPanel()
{
    // Fixed-height panel at the bottom
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.f));
    ImGui::BeginChild("##detail", ImVec2(0, 82), false,
                       ImGuiWindowFlags_NoScrollbar);

    if (m_selectedRow >= 0 &&
        m_selectedRow < static_cast<int>(m_results.size()))
    {
        const core::ScanResult& r = m_results[m_selectedRow];

        // Left: symbol/company header
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,1.f,0.4f,1.f));
        ImGui::Text("%s", r.symbol.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);
        ImGui::TextUnformatted(r.company.c_str());
        ImGui::SameLine(0, 24);

        // Quote row
        ImGui::Text("%.2f", r.price);
        ImGui::SameLine(0, 8);
        ImVec4 chgC = r.changePct >= 0 ? ImVec4(0.3f,0.9f,0.3f,1.f)
                                        : ImVec4(0.9f,0.3f,0.3f,1.f);
        ImGui::TextColored(chgC, "%+.2f (%+.2f%%)", r.change, r.changePct);

        // Detail grid
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
        auto Cell = [](const char* label, const char* val) {
            ImGui::TextDisabled("%s", label);
            ImGui::SameLine(0, 4);
            ImGui::TextUnformatted(val);
            ImGui::SameLine(0, 18);
        };

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", r.open);
        Cell("O:", buf);
        std::snprintf(buf, sizeof(buf), "%.2f", r.high);
        Cell("H:", buf);
        std::snprintf(buf, sizeof(buf), "%.2f", r.low);
        Cell("L:", buf);
        std::snprintf(buf, sizeof(buf), "%.2f", r.prevClose);
        Cell("Prev:", buf);
        Cell("Vol:", FmtVolume(r.volume).c_str());
        Cell("RelVol:", (std::to_string(static_cast<int>(r.relVolume * 100) / 100.0).substr(0,4) + "x").c_str());
        Cell("MktCap:", FmtMktCap(r.mktCapM).c_str());
        std::snprintf(buf, sizeof(buf), "%.0f", r.rsi);
        Cell("RSI:", buf);
        std::snprintf(buf, sizeof(buf), "%.2f / %.2f", r.high52, r.low52);
        Cell("52W:", buf);
        Cell("Sector:", r.sector.c_str());
    } else {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 28);
        ImGui::TextDisabled("  Click a row to see details");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================================
// DrawStatusBar
// ============================================================================

void ScannerWindow::DrawStatusBar()
{
    // Count visible rows
    int visible = 0;
    for (auto& r : m_results)
        if (MatchesFilter(r) && MatchesSearch(r)) ++visible;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.07f, 1.f));
    ImGui::BeginChild("##statusbar", ImVec2(0, 22), false,
                       ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
    ImGui::TextDisabled("  %d result(s)   |   Preset: %s   |   Last scan: %s",
                         visible,
                         core::ScanPresetLabel(kPresets[m_presetIdx]),
                         m_lastScanTimeStr);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================================
// RunScan
// ============================================================================

void ScannerWindow::RunScan()
{
    m_selectedRow = -1;
    m_lastScanTime = Clock::now();

    if (OnScanRequest) {
        // Map preset → IB scanner code
        const char* scanCode = "TOP_PERC_GAIN";
        switch (kPresets[m_presetIdx]) {
            case core::ScanPreset::TopGainers:    scanCode = "TOP_PERC_GAIN";      break;
            case core::ScanPreset::TopLosers:     scanCode = "TOP_PERC_LOSE";      break;
            case core::ScanPreset::VolumeLeaders: scanCode = "TOP_VOLUME_RATE";    break;
            case core::ScanPreset::MostActive:    scanCode = "HOT_BY_VOLUME";      break;
            case core::ScanPreset::NewHighs:      scanCode = "HIGH_52_WK_HL";      break;
            case core::ScanPreset::NewLows:       scanCode = "LOW_52_WK_HL";       break;
            case core::ScanPreset::RSIOverbought: scanCode = "RSI_OVER_BUY";       break;
            case core::ScanPreset::RSIOversold:   scanCode = "RSI_OVER_SELL";      break;
            case core::ScanPreset::NearEarnings:  scanCode = "NEAR_EARNINGS_DATE"; break;
            case core::ScanPreset::Custom:        scanCode = "TOP_PERC_GAIN";      break;
        }
        // Map asset class → IB instrument / location
        const char* instrument = "STK";
        const char* location   = "STK.US.MAJOR";
        switch (m_activeClass) {
            case core::AssetClass::ETFs:
                instrument = "ETF"; location = "STK.US.MAJOR"; break;
            case core::AssetClass::Indexes:
                instrument = "IND"; location = "IND.US";        break;
            case core::AssetClass::Futures:
                instrument = "FUT"; location = "FUT.US";        break;
            default: break;
        }
        // Keep existing results visible while the new scan is in progress;
        // OnScanData() will replace them when the server responds.
        m_scanning = true;
        OnScanRequest(scanCode, instrument, location);
        return;
    }

    // Simulation fallback (no IB connection)
    m_results.clear();
    switch (m_activeClass) {
        case core::AssetClass::Stocks:  SimulateStocks();  break;
        case core::AssetClass::Indexes: SimulateIndexes(); break;
        case core::AssetClass::ETFs:    SimulateETFs();    break;
        case core::AssetClass::Futures: SimulateFutures(); break;
    }

    // Apply preset filter/sort
    auto preset = kPresets[m_presetIdx];
    switch (preset) {
        case core::ScanPreset::TopGainers:
            m_sortCol = core::ScanColumn::ChangePct;
            m_sortAscending = false;
            break;
        case core::ScanPreset::TopLosers:
            m_sortCol = core::ScanColumn::ChangePct;
            m_sortAscending = true;
            break;
        case core::ScanPreset::VolumeLeaders:
        case core::ScanPreset::MostActive:
            m_sortCol = core::ScanColumn::Volume;
            m_sortAscending = false;
            break;
        case core::ScanPreset::NewHighs:
            m_sortCol = core::ScanColumn::PctFrom52H;
            m_sortAscending = false;
            m_results.erase(std::remove_if(m_results.begin(), m_results.end(),
                [](const core::ScanResult& r){ return r.pctFrom52H < -2.0; }),
                m_results.end());
            break;
        case core::ScanPreset::NewLows:
            m_sortCol = core::ScanColumn::PctFrom52H;
            m_sortAscending = true;
            m_results.erase(std::remove_if(m_results.begin(), m_results.end(),
                [](const core::ScanResult& r){ return r.pctFrom52H > -70.0; }),
                m_results.end());
            break;
        case core::ScanPreset::RSIOverbought:
            m_sortCol = core::ScanColumn::RSI;
            m_sortAscending = false;
            m_results.erase(std::remove_if(m_results.begin(), m_results.end(),
                [](const core::ScanResult& r){ return r.rsi < 70.0; }),
                m_results.end());
            break;
        case core::ScanPreset::RSIOversold:
            m_sortCol = core::ScanColumn::RSI;
            m_sortAscending = true;
            m_results.erase(std::remove_if(m_results.begin(), m_results.end(),
                [](const core::ScanResult& r){ return r.rsi > 30.0; }),
                m_results.end());
            break;
        default:
            break;
    }
    SortResults();
    std::time_t now2 = std::time(nullptr);
    std::tm* tm2 = std::localtime(&now2);
    if (tm2)
        std::strftime(m_lastScanTimeStr, sizeof(m_lastScanTimeStr), "%H:%M:%S", tm2);
}

// ============================================================================
// SortResults
// ============================================================================

void ScannerWindow::SortResults()
{
    bool asc = m_sortAscending;
    core::ScanColumn col = m_sortCol;

    std::stable_sort(m_results.begin(), m_results.end(),
        [col, asc](const core::ScanResult& a, const core::ScanResult& b) {
            double va = 0, vb = 0;
            std::string sa, sb;
            bool useStr = false;
            switch (col) {
                case core::ScanColumn::Symbol:    sa = a.symbol;    sb = b.symbol;    useStr = true; break;
                case core::ScanColumn::Company:   sa = a.company;   sb = b.company;   useStr = true; break;
                case core::ScanColumn::Price:      va = a.price;     vb = b.price;     break;
                case core::ScanColumn::Change:     va = a.change;    vb = b.change;    break;
                case core::ScanColumn::ChangePct:  va = a.changePct; vb = b.changePct; break;
                case core::ScanColumn::Volume:     va = a.volume;    vb = b.volume;    break;
                case core::ScanColumn::RelVolume:  va = a.relVolume; vb = b.relVolume; break;
                case core::ScanColumn::MktCap:     va = a.mktCapM;   vb = b.mktCapM;   break;
                case core::ScanColumn::PE:         va = a.pe;        vb = b.pe;        break;
                case core::ScanColumn::High52:     va = a.high52;    vb = b.high52;    break;
                case core::ScanColumn::Low52:      va = a.low52;     vb = b.low52;     break;
                case core::ScanColumn::PctFrom52H: va = a.pctFrom52H;vb = b.pctFrom52H;break;
                case core::ScanColumn::RSI:        va = a.rsi;       vb = b.rsi;       break;
                case core::ScanColumn::MACD:       va = a.macdLine - a.macdSignal;
                                                   vb = b.macdLine - b.macdSignal; break;
                case core::ScanColumn::ATR:        va = a.atr;       vb = b.atr;       break;
                default:                            va = a.changePct; vb = b.changePct; break;
            }
            if (useStr) return asc ? (sa < sb) : (sa > sb);
            return asc ? (va < vb) : (va > vb);
        });
}

// ============================================================================
// UpdateQuotes  (live drift simulation)
// ============================================================================

void ScannerWindow::UpdateQuotes(float /*dtSec*/)
{
    for (auto& r : m_results) {
        double drift = m_noise(m_rng);
        r.price    += r.price * drift;
        r.price     = std::round(r.price * 100.0) / 100.0;
        r.change    = r.price - r.prevClose;
        r.changePct = (r.prevClose > 0) ? (r.change / r.prevClose) * 100.0 : 0.0;
        r.high      = std::max(r.high, r.price);
        r.low       = std::min(r.low, r.price);

        // Update 52W distance
        if (r.high52 > 0.0)
            r.pctFrom52H = ((r.price - r.high52) / r.high52) * 100.0;
        if (r.low52 > 0.0)
            r.pctFrom52L = ((r.price - r.low52) / r.low52) * 100.0;

        // Drift volume slightly and keep relVolume live
        double volDrift = std::abs(m_noise(m_rng)) * 2.0;
        r.volume = std::round(r.volume * (1.0 + volDrift));
        if (r.avgVolume > 0.0)
            r.relVolume = r.volume / r.avgVolume;

        // Slide sparkline forward
        if (!r.sparkline.empty()) {
            r.sparkline.erase(r.sparkline.begin());
            r.sparkline.push_back(static_cast<float>(r.price));
        }

        // Recompute RSI, MACD, ATR from updated sparkline
        ComputeTechnicals(r);
    }
}

// ============================================================================
// Filter helpers
// ============================================================================

bool ScannerWindow::MatchesFilter(const core::ScanResult& r) const
{
    if (r.price < m_filter.minPrice || r.price > m_filter.maxPrice) return false;
    if (r.changePct < m_filter.minChangePct ||
        r.changePct > m_filter.maxChangePct) return false;
    if (r.volume < m_filter.minVolume) return false;
    if (r.mktCapM < m_filter.minMktCapM ||
        r.mktCapM > m_filter.maxMktCapM) return false;
    if (r.rsi < m_filter.minRsi || r.rsi > m_filter.maxRsi) return false;
    if (!m_filter.sector.empty() && r.sector != m_filter.sector) return false;
    return true;
}

bool ScannerWindow::MatchesSearch(const core::ScanResult& r) const
{
    if (m_searchBuf[0] == '\0') return true;
    std::string q = m_searchBuf;
    // case-insensitive substring match
    auto ci = [](unsigned char c){ return static_cast<char>(std::toupper(c)); };
    std::string sym = r.symbol, cmp = r.company;
    std::transform(sym.begin(), sym.end(), sym.begin(), ci);
    std::transform(cmp.begin(), cmp.end(), cmp.begin(), ci);
    std::transform(q.begin(),   q.end(),   q.begin(),   ci);
    return sym.find(q) != std::string::npos ||
           cmp.find(q) != std::string::npos;
}

// ============================================================================
// Simulation data
// ============================================================================

std::vector<float> ScannerWindow::GenerateSparkline(double startPrice,
                                                     double volatility,
                                                     bool trending,
                                                     int points)
{
    std::vector<float> sp;
    sp.reserve(points);
    double p = startPrice;
    std::normal_distribution<double> nd(trending ? 0.002 : 0.0, volatility);
    for (int i = 0; i < points; ++i) {
        p *= (1.0 + nd(m_rng));
        sp.push_back(static_cast<float>(p));
    }
    return sp;
}

// Seed the sparkline then compute initial technicals for a freshly-built ScanResult.
static void SeedTechnicals(core::ScanResult& r)
{
    ComputeTechnicals(r);
    // pctFrom52H / pctFrom52L
    if (r.high52 > 0.0)
        r.pctFrom52H = ((r.price - r.high52) / r.high52) * 100.0;
    if (r.low52 > 0.0)
        r.pctFrom52L = ((r.price - r.low52) / r.low52) * 100.0;
}

// Helper to build a result with computed fields
static core::ScanResult MakeResult(
    std::string sym, std::string company, std::string sector,
    std::string exchange, double price, double prevClose,
    double high, double low, double volume, double avgVolume,
    double mktCapM, double pe, double high52, double low52,
    double rsi, double macdLine, double macdSignal, double atr,
    std::vector<float> spark)
{
    core::ScanResult r;
    r.symbol      = std::move(sym);
    r.company     = std::move(company);
    r.sector      = std::move(sector);
    r.exchange    = std::move(exchange);
    r.price       = price;
    r.prevClose   = prevClose;
    r.change      = price - prevClose;
    r.changePct   = prevClose > 0 ? (r.change / prevClose) * 100.0 : 0.0;
    r.open        = prevClose * (1.0 + (price > prevClose ? 0.002 : -0.002));
    r.high        = high;
    r.low         = low;
    r.volume      = volume;
    r.avgVolume   = avgVolume;
    r.relVolume   = avgVolume > 0 ? volume / avgVolume : 1.0;
    r.mktCapM     = mktCapM;
    r.pe          = pe;
    r.eps         = pe > 0 ? price / pe : 0.0;
    r.high52      = high52;
    r.low52       = low52;
    r.pctFrom52H  = high52 > 0 ? ((price - high52) / high52) * 100.0 : 0.0;
    r.pctFrom52L  = low52  > 0 ? ((price - low52)  / low52)  * 100.0 : 0.0;
    r.rsi         = rsi;
    r.macdLine    = macdLine;
    r.macdSignal  = macdSignal;
    r.atr         = atr;
    r.sparkline   = std::move(spark);
    r.updatedAt   = std::time(nullptr);
    return r;
}

void ScannerWindow::SimulateStocks()
{
    struct Seed {
        const char* sym; const char* co; const char* sector; const char* exch;
        double px; double prevC; double hi52; double lo52;
        double vol; double avgVol; double mktCapM; double pe; double rsi;
    };

    static const Seed kSeeds[] = {
        {"AAPL",  "Apple Inc.",              "Technology",      "NASDAQ", 184.20, 181.80, 199.62, 124.17, 68.4e6, 56.9e6, 2840000, 29.5,  62.1},
        {"MSFT",  "Microsoft Corp.",         "Technology",      "NASDAQ", 415.30, 412.10, 430.82, 309.45, 22.1e6, 19.8e6, 3080000, 34.2,  58.7},
        {"NVDA",  "NVIDIA Corp.",            "Technology",      "NASDAQ", 876.50, 840.20, 974.00, 373.04, 42.3e6, 38.7e6, 2160000, 65.8,  72.4},
        {"AMZN",  "Amazon.com Inc.",         "Consumer Cycl.",  "NASDAQ", 182.75, 180.30, 201.20, 101.26, 38.2e6, 35.1e6, 1910000, 58.3,  55.3},
        {"GOOGL", "Alphabet Inc.",           "Technology",      "NASDAQ", 172.63, 170.10, 193.31, 116.73, 29.8e6, 26.4e6, 2130000, 24.7,  51.8},
        {"META",  "Meta Platforms Inc.",     "Technology",      "NASDAQ", 520.40, 514.80, 531.49, 173.73, 16.4e6, 15.2e6,  133000, 27.8,  66.9},
        {"TSLA",  "Tesla Inc.",              "Consumer Cycl.",  "NASDAQ", 247.10, 260.40, 299.29, 101.81, 95.6e6, 88.3e6,  792000, 55.2,  28.3},
        {"JPM",   "JPMorgan Chase & Co.",   "Financials",      "NYSE",   207.30, 205.60, 220.82, 131.09, 12.8e6, 11.9e6,  598000, 12.4,  60.5},
        {"V",     "Visa Inc.",               "Financials",      "NYSE",   279.80, 277.50, 290.96, 220.16,  8.4e6,  7.9e6,  570000, 31.6,  57.2},
        {"JNJ",   "Johnson & Johnson",       "Healthcare",      "NYSE",   152.40, 153.80, 175.97, 143.94,  7.3e6,  7.1e6,  368000, 15.3,  40.7},
        {"WMT",   "Walmart Inc.",            "Consumer Def.",   "NYSE",    68.90,  67.80,  74.88,  46.88, 22.7e6, 20.4e6,  556000, 30.1,  63.4},
        {"XOM",   "Exxon Mobil Corp.",       "Energy",          "NYSE",   118.60, 116.30, 126.34,  93.16, 18.9e6, 17.2e6,  476000,  8.9,  59.8},
        {"BAC",   "Bank of America Corp.",   "Financials",      "NYSE",    40.10,  39.60,  44.44,  24.96, 42.1e6, 38.5e6,  316000, 12.8,  54.3},
        {"HD",    "Home Depot Inc.",         "Consumer Cycl.",  "NYSE",   382.20, 379.80, 395.40, 265.41,  4.6e6,  4.3e6,  381000, 22.5,  58.1},
        {"MA",    "Mastercard Inc.",         "Financials",      "NYSE",   462.50, 458.30, 493.66, 349.29,  3.2e6,  3.0e6,  429000, 35.4,  60.9},
        {"PFE",   "Pfizer Inc.",             "Healthcare",      "NYSE",    28.30,  29.40,  37.34,  25.20, 26.8e6, 24.3e6,  160000, 15.7,  27.6},
        {"KO",    "Coca-Cola Co.",           "Consumer Def.",   "NYSE",    64.80,  64.20,  67.20,  51.55,  9.4e6,  8.7e6,  280000, 24.3,  53.8},
        {"CVX",   "Chevron Corp.",           "Energy",          "NYSE",   160.70, 158.40, 189.68, 132.35, 11.2e6, 10.4e6,  310000,  9.6,  61.2},
        {"ABBV",  "AbbVie Inc.",             "Healthcare",      "NYSE",   168.50, 166.30, 175.27, 134.17,  7.8e6,  7.3e6,  298000, 22.1,  65.7},
        {"AVGO",  "Broadcom Inc.",           "Technology",      "NASDAQ", 154.20, 149.80, 185.16, 102.08, 18.6e6, 16.9e6,  718000, 30.4,  70.2},
        {"LLY",   "Eli Lilly & Co.",         "Healthcare",      "NYSE",   775.30, 760.10, 803.75, 436.00,  3.4e6,  3.1e6,  735000, 60.8,  68.4},
        {"AMD",   "Advanced Micro Devices", "Technology",      "NASDAQ", 162.40, 169.80, 227.30,  93.12, 58.3e6, 52.7e6,  263000, 45.2,  29.8},
        {"INTC",  "Intel Corp.",             "Technology",      "NASDAQ",  42.80,  44.30,  68.36,  18.84, 33.7e6, 30.8e6,  181000,  0.0,  31.4},
        {"BA",    "Boeing Co.",              "Industrials",     "NYSE",   181.90, 179.60, 267.54, 159.70, 10.2e6,  9.4e6,  110000,  0.0,  48.6},
        {"GS",    "Goldman Sachs Group",     "Financials",      "NYSE",   492.80, 488.40, 513.74, 296.62,  4.1e6,  3.8e6,  164000, 14.2,  62.3},
    };

    for (const auto& s : kSeeds) {
        bool upTrend = s.px > s.prevC;
        // 40-point sparkline starting near the 52W low, ending at current price
        auto spark = GenerateSparkline(s.lo52, 0.012, upTrend, 40);
        if (!spark.empty()) spark.back() = static_cast<float>(s.px);

        auto r = MakeResult(
            s.sym, s.co, s.sector, s.exch,
            s.px, s.prevC,
            s.px * 1.003, s.px * 0.997,
            s.vol, s.avgVol, s.mktCapM, s.pe,
            s.hi52, s.lo52, s.rsi,
            0.0, 0.0, 0.0, std::move(spark));
        SeedTechnicals(r);
        m_results.push_back(std::move(r));
    }
}

void ScannerWindow::SimulateIndexes()
{
    struct Seed { const char* sym; const char* name; double px; double prevC; double hi52; double lo52; };
    static const Seed kIdx[] = {
        {"SPX",  "S&P 500",           5305.0, 5280.0, 5342.00, 4103.78},
        {"NDX",  "NASDAQ-100",       18642.0,18511.0,18671.00,13520.00},
        {"DJIA", "Dow Jones Ind.",   39050.0,38800.0,39889.00,32327.20},
        {"RUT",  "Russell 2000",      2072.0, 2055.0, 2136.00, 1633.67},
        {"VIX",  "CBOE Volatility",    13.82,  14.60,   23.81,    11.81},
        {"FTSE", "FTSE 100",          8250.0, 8210.0,  8474.00,  7000.0},
        {"DAX",  "DAX 40",           18520.0,18450.0, 18567.00, 14477.0},
        {"NKY",  "Nikkei 225",       38100.0,37900.0, 41087.00, 30487.0},
    };

    for (const auto& s : kIdx) {
        bool up = s.px > s.prevC;
        auto spark = GenerateSparkline(s.lo52, 0.008, up, 40);
        if (!spark.empty()) spark.back() = static_cast<float>(s.px);

        core::ScanResult r = MakeResult(
            s.sym, s.name, "Index", "--",
            s.px, s.prevC,
            s.px * 1.002, s.px * 0.998,
            0, 0, 0, 0,
            s.hi52, s.lo52, 50.0, 0.0, 0.0, 0.0, std::move(spark));
        r.isIndex = true;
        SeedTechnicals(r);
        m_results.push_back(std::move(r));
    }
}

void ScannerWindow::SimulateETFs()
{
    struct Seed { const char* sym; const char* name; const char* sector;
                  double px; double prevC; double hi52; double lo52;
                  double vol; double mktCapM; };
    static const Seed kETF[] = {
        {"SPY", "SPDR S&P 500 ETF",        "Broad Market", 530.50, 527.80, 534.48, 410.09,  82.5e6, 485000},
        {"QQQ", "Invesco QQQ Trust",        "Technology",   456.20, 453.10, 458.62, 332.55,  48.2e6, 220000},
        {"IWM", "iShares Russell 2000",     "Broad Market", 206.40, 204.90, 213.72, 162.54,  29.7e6,  56000},
        {"GLD", "SPDR Gold Shares",         "Commodities",  237.80, 235.60, 241.14, 169.65,  11.4e6,  57000},
        {"TLT", "iShares 20+ Yr Treasury", "Fixed Income", 101.20, 102.80, 118.04,  82.42,  22.8e6,  12800},
        {"XLF", "Financial Select SPDR",    "Financials",    43.90,  43.50,  46.88,  31.72,  46.3e6,  36000},
        {"XLE", "Energy Select SPDR",       "Energy",        93.40,  92.10, 101.78,  71.08,  19.8e6,  27000},
        {"ARKK","ARK Innovation ETF",       "Technology",    52.80,  54.90,  68.00,  29.41,  14.7e6,   8900},
        {"SOXX","iShares Semi Cond. ETF",   "Technology",   232.10, 226.40, 264.43, 150.20,   8.6e6,  16600},
        {"VNQ", "Vanguard Real Estate ETF", "Real Estate",   90.30,  89.80,  97.72,  72.46,  10.2e6,  32000},
    };

    for (const auto& s : kETF) {
        bool up = s.px > s.prevC;
        auto spark = GenerateSparkline(s.lo52, 0.010, up, 40);
        if (!spark.empty()) spark.back() = static_cast<float>(s.px);

        auto r = MakeResult(
            s.sym, s.name, s.sector, "NYSE",
            s.px, s.prevC,
            s.px * 1.002, s.px * 0.998,
            s.vol, s.vol * 1.1, s.mktCapM, 0.0,
            s.hi52, s.lo52, 50.0, 0.0, 0.0, 0.0, std::move(spark));
        SeedTechnicals(r);
        m_results.push_back(std::move(r));
    }
}

void ScannerWindow::SimulateFutures()
{
    struct Seed { const char* sym; const char* name; const char* sector;
                  double px; double prevC; double hi52; double lo52; };
    static const Seed kFut[] = {
        {"ES",  "E-mini S&P 500",    "Equity Futures",  5308.25, 5283.00, 5347.00, 4108.00},
        {"NQ",  "E-mini NASDAQ",     "Equity Futures", 18658.00,18524.00,18688.00,13535.00},
        {"YM",  "E-mini Dow",        "Equity Futures", 39072.00,38825.00,39913.00,32345.00},
        {"RTY", "E-mini Russell",    "Equity Futures",  2075.10, 2058.00, 2138.00, 1635.00},
        {"CL",  "Crude Oil (WTI)",   "Energy",           83.45,   82.10,   95.03,   66.74},
        {"GC",  "Gold",              "Metals",         2350.80, 2332.40, 2431.50, 1810.40},
        {"SI",  "Silver",            "Metals",           28.92,   28.40,   32.51,   20.02},
        {"ZN",  "10-Yr T-Note",      "Fixed Income",    110.09,  110.70,  118.00,   99.09},
        {"6E",  "Euro FX",           "Currencies",       1.0815,  1.0790,  1.1139,  1.0460},
        {"6J",  "Japanese Yen",      "Currencies",       0.006420,0.006450,0.007052,0.006271},
    };

    for (const auto& s : kFut) {
        bool up = s.px > s.prevC;
        auto spark = GenerateSparkline(s.lo52, 0.009, up, 40);
        if (!spark.empty()) spark.back() = static_cast<float>(s.px);

        auto r = MakeResult(
            s.sym, s.name, s.sector, "CME",
            s.px, s.prevC,
            s.px * 1.003, s.px * 0.997,
            0, 0, 0, 0,
            s.hi52, s.lo52, 50.0, 0.0, 0.0, 0.0, std::move(spark));
        SeedTechnicals(r);
        m_results.push_back(std::move(r));
    }
}

// ============================================================================
// Formatting helpers
// ============================================================================

std::string ScannerWindow::FmtPrice(double p)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", p);
    return buf;
}

std::string ScannerWindow::FmtChange(double c, bool withSign)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), withSign ? "%+.2f" : "%.2f", c);
    return buf;
}

std::string ScannerWindow::FmtVolume(double v)
{
    char buf[32];
    if      (v >= 1.0e9) std::snprintf(buf, sizeof(buf), "%.2fB", v / 1.0e9);
    else if (v >= 1.0e6) std::snprintf(buf, sizeof(buf), "%.2fM", v / 1.0e6);
    else if (v >= 1.0e3) std::snprintf(buf, sizeof(buf), "%.1fK", v / 1.0e3);
    else                  std::snprintf(buf, sizeof(buf), "%.0f",  v);
    return buf;
}

std::string ScannerWindow::FmtMktCap(double mM)
{
    char buf[32];
    if      (mM >= 1.0e6) std::snprintf(buf, sizeof(buf), "%.1fT", mM / 1.0e6);
    else if (mM >= 1.0e3) std::snprintf(buf, sizeof(buf), "%.1fB", mM / 1.0e3);
    else if (mM > 0)      std::snprintf(buf, sizeof(buf), "%.0fM", mM);
    else                   std::snprintf(buf, sizeof(buf), "—");
    return buf;
}

std::string ScannerWindow::FmtPct(double p, bool withSign)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), withSign ? "%+.2f%%" : "%.2f%%", p);
    return buf;
}

void ScannerWindow::PushRowColor(double /*changePct*/, bool /*isPortfolio*/) {}
void ScannerWindow::PopRowColor() {}

}  // namespace ui
