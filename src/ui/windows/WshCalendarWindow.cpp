#include "ui/windows/WshCalendarWindow.h"
#include "ui/UiScale.h"
#include "ui/WshData.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numeric>

namespace ui {

// ============================================================================
// Column indices (must match TableSetupColumn order in DrawTable)
// ============================================================================
enum ColIdx { kColDate = 0, kColSymbol, kColType, kColImportance, kColDescription, kNumTableCols };

// ============================================================================
// Constructor / destructor
// ============================================================================
WshCalendarWindow::WshCalendarWindow() {}

WshCalendarWindow::~WshCalendarWindow() { CancelAll(); }

// ============================================================================
// Slot allocation
// ============================================================================
int WshCalendarWindow::AllocSlot() const {
    for (int i = 0; i < kSlots; ++i)
        if (!m_usedSlots[i]) return i;
    return -1;
}

// ============================================================================
// Subscription management
// ============================================================================
void WshCalendarWindow::Subscribe(int conId, const std::string& symbol) {
    if (m_subs.count(conId)) return;  // already subscribed
    int slot = AllocSlot();
    if (slot < 0) return;
    int reqId = kReqBase + slot;
    m_usedSlots[slot] = true;
    m_subs[conId] = {reqId, symbol};
    if (OnReqWshEvents) OnReqWshEvents(reqId, static_cast<long>(conId));
}

void WshCalendarWindow::SubscribeConId(int conId, const std::string& symbol) {
    if (conId <= 0) return;
    Subscribe(conId, symbol);
}

void WshCalendarWindow::CancelAll() {
    for (auto& [conId, sub] : m_subs) {
        int slot = sub.reqId - kReqBase;
        if (slot >= 0 && slot < kSlots) m_usedSlots[slot] = false;
        if (OnCancelWshEvents) OnCancelWshEvents(sub.reqId);
    }
    m_subs.clear();
}

// ============================================================================
// WSH event arrival
// ============================================================================
void WshCalendarWindow::OnWshEvent(int reqId, const std::string& jsonData) {
    WshData::WshEvent ev = WshData::ParseWshEvent(jsonData);
    if (ev.date.empty()) return;

    // Find symbol for this reqId
    std::string symbol;
    for (auto& [conId, sub] : m_subs) {
        if (sub.reqId == reqId) { symbol = sub.symbol; break; }
    }
    if (symbol.empty()) return;

    // Deduplicate by conId + type + date
    for (auto& e : m_events)
        if (e.conId == ev.conId && e.type == ev.type && e.date == ev.date)
            return;

    WshCalendarEntry entry;
    entry.conId       = ev.conId;
    entry.symbol      = symbol;
    entry.date        = ev.date;
    entry.type        = ev.type;
    entry.description = ev.description;
    entry.importance  = ev.importance;
    m_events.push_back(std::move(entry));
}

// ============================================================================
// Filter helpers
// ============================================================================
bool WshCalendarWindow::PassesFilter(const WshCalendarEntry& e) const {
    if (m_filterSymbol[0] != '\0' && e.symbol.find(m_filterSymbol) == std::string::npos)
        return false;
    if (m_filterFrom[0] != '\0' && e.date < m_filterFrom) return false;
    if (m_filterTo[0]   != '\0' && e.date > m_filterTo)   return false;
    if (m_filterType > 0) {
        switch (m_filterType) {
            case 1: if (e.type != "Earnings") return false; break;
            case 2: if (e.type != "Dividend") return false; break;
            case 3: if (e.type != "Split")    return false; break;
            case 4: // Other
                if (e.type == "Earnings" || e.type == "Dividend" || e.type == "Split")
                    return false;
                break;
        }
    }
    if (m_filterImportance > 0) {
        const char* imps[] = {"", "High", "Medium", "Low"};
        if (e.importance != imps[m_filterImportance]) return false;
    }
    return true;
}

ImVec4 WshCalendarWindow::TypeColor(const std::string& type) {
    if (type == "Earnings") return ImVec4(0.95f, 0.85f, 0.10f, 1.0f);  // yellow
    if (type == "Dividend") return ImVec4(0.20f, 0.85f, 0.90f, 1.0f);  // cyan
    if (type == "Split")    return ImVec4(0.70f, 0.30f, 0.90f, 1.0f);  // purple
    return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);                          // grey
}

// ============================================================================
// Date-picker helpers
// ============================================================================
static int DaysInMonth(int year, int month) {
    static const int kDays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return kDays[month];
}

// Returns 0=Sunday … 6=Saturday for the 1st of the given month.
static int DayOfWeek1st(int year, int month) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = 1;
    mktime(&t);
    return t.tm_wday;
}

// idx=0 → From, idx=1 → To.
// Renders a button showing the stored date (or "Any"). On click, opens a
// popup calendar. Sets buf (size 11) to "YYYY-MM-DD" on day select.
void WshCalendarWindow::DrawDatePicker(int idx, const char* label, char* buf) {
    const char* kPopupId[] = {"##wshdp0", "##wshdp1"};
    const char* kBtnId[]   = {"##wshdb0", "##wshdb1"};

    // Button label: "From: YYYY-MM-DD" / "To: Any" etc.
    char btnLabel[32];
    if (buf[0] != '\0')
        std::snprintf(btnLabel, sizeof(btnLabel), "%s: %s", label, buf);
    else
        std::snprintf(btnLabel, sizeof(btnLabel), "%s: Any", label);

    float btnW = em(90);
    ImGui::SetNextItemWidth(btnW);
    if (ImGui::Button(btnLabel, ImVec2(btnW, 0)))
        ImGui::OpenPopup(kPopupId[idx]);

    ImGui::SetItemTooltip("%s date", label);

    if (!ImGui::BeginPopup(kPopupId[idx])) return;

    int& navY = m_calNavYear[idx];
    int& navM = m_calNavMonth[idx];

    // Initialise nav to stored date or today
    if (buf[0] != '\0') {
        int sy = 0, sm = 0;
        std::sscanf(buf, "%d-%d", &sy, &sm);
        if (sy > 0 && sm >= 1 && sm <= 12) { navY = sy; navM = sm; }
    } else {
        std::time_t now = std::time(nullptr);
        struct tm* lt   = std::localtime(&now);
        navY = lt->tm_year + 1900;
        navM = lt->tm_mon + 1;
    }

    // Header: "<" [Month YYYY] ">"
    static const char* kMonths[] = {
        "January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };
    if (ImGui::ArrowButton(idx == 0 ? "##wl0" : "##wl1", ImGuiDir_Left)) {
        if (--navM < 1) { navM = 12; --navY; }
    }
    ImGui::SameLine();
    ImGui::Text("%s %d", kMonths[navM - 1], navY);
    ImGui::SameLine();
    if (ImGui::ArrowButton(idx == 0 ? "##wr0" : "##wr1", ImGuiDir_Right)) {
        if (++navM > 12) { navM = 1; ++navY; }
    }

    ImGui::Separator();

    // Day-of-week header
    static const char* kDow[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    for (int d = 0; d < 7; ++d) {
        if (d) ImGui::SameLine(0, em(2));
        ImGui::TextDisabled("%s", kDow[d]);
    }

    // Day grid
    int firstDow  = DayOfWeek1st(navY, navM);
    int daysInMon = DaysInMonth(navY, navM);
    int col       = firstDow;
    float cellW   = em(26);
    float cellH   = em(20);

    if (col > 0)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + col * (cellW + em(2)));

    for (int day = 1; day <= daysInMon; ++day) {
        char dayId[16];
        std::snprintf(dayId, sizeof(dayId), "%d##%d%d%d", day, idx, navY, navM);

        // Highlight selected day
        char candidate[12];
        std::snprintf(candidate, sizeof(candidate), "%04d-%02d-%02d",
                      navY % 10000, navM, day);
        bool selected = (std::strcmp(buf, candidate) == 0);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }

        if (ImGui::Button(dayId, ImVec2(cellW, cellH))) {
            std::snprintf(buf, 11, "%04d-%02d-%02d", navY % 10000, navM, day);
            ImGui::CloseCurrentPopup();
        }
        if (selected) ImGui::PopStyleColor();

        ++col;
        if (col < 7) ImGui::SameLine(0, em(2));
        else         col = 0;
    }

    ImGui::Separator();
    if (ImGui::SmallButton("Clear")) {
        buf[0] = '\0';
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

// ============================================================================
// Filter toolbar
// ============================================================================
void WshCalendarWindow::DrawFilterToolbar() {
    FlexRow row;

    row.item(em(90));
    ImGui::SetNextItemWidth(em(80));
    ImGui::InputText("##wshsym", m_filterSymbol, sizeof(m_filterSymbol));
    ImGui::SetItemTooltip("Symbol filter");

    row.item(em(100));
    DrawDatePicker(0, "From", m_filterFrom);

    row.item(em(100));
    DrawDatePicker(1, "To", m_filterTo);

    static const char* kTypeItems[] = {"All Types","Earnings","Dividend","Split","Other"};
    row.item(em(90));
    ImGui::SetNextItemWidth(em(88));
    ImGui::Combo("##wshtype", &m_filterType, kTypeItems, 5);

    static const char* kImpItems[] = {"All Importance","High","Medium","Low"};
    row.item(em(110));
    ImGui::SetNextItemWidth(em(108));
    ImGui::Combo("##wshimp", &m_filterImportance, kImpItems, 4);
}

// ============================================================================
// Event table
// ============================================================================
void WshCalendarWindow::DrawTable() {
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;

    const float tableH = ImGui::GetContentRegionAvail().y;
    if (!ImGui::BeginTable("##wshcal", kNumTableCols, kFlags, ImVec2(0, tableH)))
        return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Date",        ImGuiTableColumnFlags_DefaultSort, em(80));
    ImGui::TableSetupColumn("Symbol",      ImGuiTableColumnFlags_None,        em(55));
    ImGui::TableSetupColumn("Type",        ImGuiTableColumnFlags_None,        em(70));
    ImGui::TableSetupColumn("Importance",  ImGuiTableColumnFlags_None,        em(75));
    ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_None,        em(260));
    ImGui::TableHeadersRow();

    // Handle sort specs
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            m_sortCol = specs->Specs[0].ColumnIndex;
            m_sortAsc = (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
            specs->SpecsDirty = false;
        }
    }

    // Build filtered + sorted index list
    std::vector<int> indices;
    indices.reserve(m_events.size());
    for (int i = 0; i < (int)m_events.size(); ++i)
        if (PassesFilter(m_events[i])) indices.push_back(i);

    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) {
        const auto& ea = m_events[a];
        const auto& eb = m_events[b];
        int cmp = 0;
        switch (m_sortCol) {
            case kColDate:        cmp = ea.date.compare(eb.date);             break;
            case kColSymbol:      cmp = ea.symbol.compare(eb.symbol);         break;
            case kColType:        cmp = ea.type.compare(eb.type);             break;
            case kColImportance:  cmp = ea.importance.compare(eb.importance); break;
            case kColDescription: cmp = ea.description.compare(eb.description); break;
        }
        return m_sortAsc ? cmp < 0 : cmp > 0;
    });

    // Render rows
    for (int idx : indices) {
        const auto& e = m_events[idx];

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(kColDate);
        bool clicked = ImGui::Selectable(e.date.c_str(), false,
                                         ImGuiSelectableFlags_SpanAllColumns
                                         | ImGuiSelectableFlags_AllowOverlap,
                                         ImVec2(0, ImGui::GetTextLineHeightWithSpacing() - 2));
        if (clicked && OnBroadcastSymbol) OnBroadcastSymbol(e.symbol);

        ImGui::TableSetColumnIndex(kColSymbol);
        ImGui::TextUnformatted(e.symbol.c_str());

        ImGui::TableSetColumnIndex(kColType);
        ImGui::TextColored(TypeColor(e.type), "%s", e.type.c_str());

        ImGui::TableSetColumnIndex(kColImportance);
        ImGui::TextUnformatted(e.importance.c_str());

        ImGui::TableSetColumnIndex(kColDescription);
        ImGui::TextUnformatted(e.description.c_str());
    }

    ImGui::EndTable();
}

// ============================================================================
// Render
// ============================================================================
bool WshCalendarWindow::Render() {
    if (!m_open) return false;

    ImGui::SetNextWindowSize(ImVec2(em(560), em(340)), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin("WSH Calendar##wshcal", &m_open, flags)) {
        ImGui::End();
        return m_open;
    }

    DrawFilterToolbar();
    ImGui::Separator();
    DrawTable();

    ImGui::End();
    return m_open;
}

}  // namespace ui
