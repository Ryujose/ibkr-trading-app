#include "ui/UiScale.h"
#include "ui/windows/WatchlistWindow.h"
#include "ui/SymbolSearch.h"
#include "core/models/WindowGroup.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <filesystem>

namespace ui {

// ============================================================================
// Static members
// ============================================================================
std::vector<WatchlistWindow::SavedPreset> WatchlistWindow::s_presets;
bool                                       WatchlistWindow::s_presetsLoaded = false;

// ============================================================================
// File-path helpers
// ============================================================================
static std::string presetsFilePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    return std::string(home) + "/.config/ibkr-trading-app/watchlist-presets.cfg";
}

static void ensureConfigDir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return;
    std::filesystem::create_directories(std::string(home) + "/.config/ibkr-trading-app");
}

// ============================================================================
// EnsureDefaultPreset — adds Mag 7 if s_presets is empty
// ============================================================================
void WatchlistWindow::EnsureDefaultPreset() {
    if (!s_presets.empty()) return;
    SavedPreset p;
    p.name = "Mag 7";
    core::Watchlist wl;
    wl.name = "Mag 7";
    for (const char* sym : {"AAPL","MSFT","NVDA","AMZN","GOOGL","META","TSLA"}) {
        core::WatchlistItem item;
        item.symbol  = sym;
        item.secType = "STK";
        wl.items.push_back(std::move(item));
    }
    p.watchlists.push_back(std::move(wl));
    s_presets.push_back(std::move(p));
}

// ============================================================================
// Preset file I/O
// Format per preset block:
//   PRESET:<name>
//   WATCH:<tabName>
//   <symbol>,<secType>,<primaryExch>,<currency>,<conId>,<description>
// ============================================================================
void WatchlistWindow::LoadPresetsFile() {
    s_presetsLoaded = true;
    std::ifstream f(presetsFilePath());
    if (!f.is_open()) { EnsureDefaultPreset(); return; }

    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= 7 && line.substr(0, 7) == "PRESET:") {
            s_presets.push_back({line.substr(7), {}});
        } else if (!s_presets.empty()
                   && line.size() >= 6 && line.substr(0, 6) == "WATCH:") {
            s_presets.back().watchlists.push_back({line.substr(6), {}});
        } else if (!s_presets.empty()
                   && !s_presets.back().watchlists.empty()
                   && !line.empty()) {
            core::WatchlistItem item;
            std::istringstream ss(line);
            std::string tok;
            int col = 0;
            while (std::getline(ss, tok, ',')) {
                switch (col++) {
                    case 0: item.symbol      = tok; break;
                    case 1: item.secType     = tok; break;
                    case 2: item.primaryExch = tok; break;
                    case 3: item.currency    = tok; break;
                    case 4: try { item.conId = std::stoi(tok); } catch (...) {} break;
                    case 5: item.description = tok; break;
                }
            }
            if (!item.symbol.empty())
                s_presets.back().watchlists.back().items.push_back(std::move(item));
        }
    }
    if (s_presets.empty()) EnsureDefaultPreset();
}

void WatchlistWindow::SavePresetsFile() {
    ensureConfigDir();
    std::string tmp = presetsFilePath() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (const auto& preset : s_presets) {
            f << "PRESET:" << preset.name << '\n';
            for (const auto& wl : preset.watchlists) {
                f << "WATCH:" << wl.name << '\n';
                for (const auto& it : wl.items)
                    f << it.symbol << ',' << it.secType << ','
                      << it.primaryExch << ',' << it.currency << ','
                      << it.conId << ',' << it.description << '\n';
            }
        }
    }
    std::rename(tmp.c_str(), presetsFilePath().c_str());
}

const std::vector<WatchlistWindow::SavedPreset>& WatchlistWindow::GetPresets() {
    if (!s_presetsLoaded) LoadPresetsFile();
    return s_presets;
}

// ============================================================================
// Constructor / destructor
// ============================================================================
// Column definitions — order matches kNumCols enum 0-21
struct ColDef { const char* name; float width; bool defaultOn; };
static constexpr ColDef kColDefs[WatchlistWindow::kNumCols] = {
    {"Symbol",    60,  true },
    {"Company",  120,  true },
    {"Last",      55,  true },
    {"Chg",       50,  true },
    {"%Chg",      55,  true },
    {"Bid",       50,  true },
    {"Ask",       50,  true },
    {"Bid Sz",    50,  false},
    {"Ask Sz",    50,  false},
    {"Last Sz",   55,  false},
    {"Spread",    50,  false},
    {"Volume",    65,  true },
    {"Avg Vol",   65,  false},
    {"High",      50,  true },
    {"Low",       50,  true },
    {"Open",      50,  true },
    {"Prev Cls",  55,  false},
    {"52W Hi",    55,  false},
    {"52W Lo",    55,  false},
    {"Type",      45,  false},
    {"Exchange",  70,  false},
    {"Currency",  55,  false},
};

WatchlistWindow::WatchlistWindow() {
    if (!s_presetsLoaded) LoadPresetsFile();
    m_watchlists.push_back(core::Watchlist{});
    for (int c = 0; c < kNumCols; ++c)
        m_colEnabled[c] = kColDefs[c].defaultOn;
}

WatchlistWindow::~WatchlistWindow() {}

void WatchlistWindow::setInstanceId(int id) {
    m_instanceId = id;
    std::snprintf(m_title, sizeof(m_title),
                  "Watchlist %d##watchlist%d", id, id);
}

// ============================================================================
// AddDefaultsIfEmpty — called after callbacks are wired
// ============================================================================
void WatchlistWindow::AddDefaultsIfEmpty() {
    // Only populate if the first tab is truly empty
    if (!m_watchlists.empty() && !m_watchlists[0].items.empty()) return;

    if (!s_presets.empty()) {
        // Load preset 0 without marking m_activePreset yet (SwitchToPreset does)
        SwitchToPreset(0);
    }
}

// ============================================================================
// Preset switching — operates on the current tab only
// ============================================================================
void WatchlistWindow::SwitchToPreset(int idx) {
    const auto& presets = GetPresets();
    if (idx < 0 || idx >= (int)presets.size()) return;
    if (presets[idx].watchlists.empty()) return;

    // Ensure there is at least one tab to load into
    if (m_watchlists.empty()) m_watchlists.push_back(core::Watchlist{});
    int tab = (m_activeTab >= 0 && m_activeTab < (int)m_watchlists.size())
                  ? m_activeTab : 0;

    // Cancel subscriptions for this tab's current items only
    for (auto& item : m_watchlists[tab].items) UnsubscribeItem(item);

    // Replace this tab's symbols; name the tab after the preset (the combo label)
    const core::Watchlist& src = presets[idx].watchlists[0];
    m_watchlists[tab].name  = presets[idx].name;
    m_watchlists[tab].items = src.items;
    // Tab label changed → ImGui's stored selected-ID is now stale; re-select explicitly
    m_requestSelectTab = tab;

    m_activePreset = idx;

    // Clear enrichment queue and re-populate for the new items
    m_cdQueue.clear();
    m_cdEnrichPending = false;
    m_cdEnrichSymbol.clear();

    for (auto& item : m_watchlists[tab].items) {
        item.subscribed = false;
        item.reqId      = 0;
        // Reset live-tick fields so stale values from the preset file don't linger
        item.last = item.bid = item.ask = item.open =
        item.high = item.low = item.change = item.changePct = item.volume = 0.0;

        int slot = AllocSlot();
        if (slot < 0) break;
        SubscribeItem(item, slot);
        if (item.description.empty())
            m_cdQueue.push_back(item.symbol);
    }
}

void WatchlistWindow::SaveCurrentAsPreset(const std::string& name) {
    if (name.empty()) return;
    if (m_activeTab < 0 || m_activeTab >= (int)m_watchlists.size()) return;

    // A preset stores only the current tab's symbols
    SavedPreset p;
    p.name = name;
    p.watchlists = { m_watchlists[m_activeTab] };

    for (int i = 0; i < (int)s_presets.size(); ++i) {
        if (s_presets[i].name == name) {
            s_presets[i] = std::move(p);
            m_activePreset = i;
            SavePresetsFile();
            return;
        }
    }
    s_presets.push_back(std::move(p));
    m_activePreset = (int)s_presets.size() - 1;
    SavePresetsFile();
}

void WatchlistWindow::DeletePreset(int idx) {
    if (idx < 0 || idx >= (int)s_presets.size()) return;
    s_presets.erase(s_presets.begin() + idx);
    if (m_activePreset == idx)      m_activePreset = -1;
    else if (m_activePreset > idx)  --m_activePreset;
    SavePresetsFile();
}

// ============================================================================
// Tick callbacks
// ============================================================================
void WatchlistWindow::OnTickPrice(int reqId, int field, double price) {
    auto* item = FindByReqId(reqId);
    if (!item) return;
    switch (field) {
        case 1:  item->bid  = price; break;
        case 2:  item->ask  = price; break;
        case 4:  item->last = price;
                 if (item->open > 0.0) {
                     item->change    = price - item->open;
                     item->changePct = (item->change / item->open) * 100.0;
                 }
                 break;
        case 6:  item->high = price; break;
        case 7:  item->low  = price; break;
        case 9:  item->prevClose = price;
                 if (item->open == 0.0 && price > 0.0) {
                     item->open = price;
                     if (item->last > 0.0) {
                         item->change    = item->last - price;
                         item->changePct = (item->change / price) * 100.0;
                     }
                 }
                 break;
        case 14: item->open = price;
                 if (item->last > 0.0 && price > 0.0) {
                     item->change    = item->last - price;
                     item->changePct = (item->change / price) * 100.0;
                 }
                 break;
        case 72: item->high52w = price; break;
        case 73: item->low52w  = price; break;
        default: break;
    }
}

void WatchlistWindow::OnTickSize(int reqId, int field, double size) {
    auto* item = FindByReqId(reqId);
    if (!item) return;
    switch (field) {
        case 0:  item->bidSize   = size; break;
        case 3:  item->askSize   = size; break;
        case 5:  item->lastSize  = size; break;
        case 8:  item->volume    = size; break;
        case 21: item->avgVolume = size; break;
        default: break;
    }
}

// ============================================================================
// Contract details arrival
// ============================================================================
void WatchlistWindow::SetContractDetails(int reqId, long conId,
                                         const std::string& description,
                                         const std::string& secType,
                                         const std::string& primaryExch,
                                         const std::string& currency) {
    if (reqId != m_cdReqId) return;

    if (m_pendingSlot >= 0) {
        // ---- New-symbol add flow --------------------------------------------
        if (m_activeTab < 0 || m_activeTab >= (int)m_watchlists.size()) {
            m_pendingSlot = -1; return;
        }
        auto& wl = m_watchlists[m_activeTab];
        if ((int)wl.items.size() >= 100) { m_pendingSlot = -1; return; }

        core::WatchlistItem item;
        item.symbol      = m_pendingSymbol;
        item.description = description;
        item.secType     = secType.empty() ? "STK" : secType;
        item.primaryExch = primaryExch;
        item.currency    = currency;
        item.conId       = (int)conId;

        SubscribeItem(item, m_pendingSlot);
        wl.items.push_back(std::move(item));
        m_pendingSlot  = -1;
        m_pendingSymbol.clear();

    } else if (m_cdEnrichPending && !m_cdEnrichSymbol.empty()) {
        // ---- Description enrichment for bulk-loaded items -------------------
        if (auto* item = FindBySymbol(m_cdEnrichSymbol)) {
            item->description = description;
            item->conId       = (int)conId;
            if (item->primaryExch.empty()) item->primaryExch = primaryExch;
            if (item->currency.empty())    item->currency    = currency;
            if (item->secType.empty())     item->secType     = secType.empty() ? "STK" : secType;
        }
        m_cdEnrichSymbol.clear();
        m_cdEnrichPending = false;
    }
}

// ============================================================================
// Description enrichment queue — processed one per frame inside Render()
// ============================================================================
void WatchlistWindow::ProcessCdQueue() {
    if (m_cdEnrichPending || m_pendingSlot >= 0) return;
    if (m_cdQueue.empty()) return;
    if (!OnReqContractDetails) return;

    m_cdEnrichSymbol  = m_cdQueue.front();
    m_cdQueue.pop_front();
    m_cdEnrichPending = true;
    OnReqContractDetails(m_cdReqId, m_cdEnrichSymbol);
}

// ============================================================================
// Subscription management
// ============================================================================
int WatchlistWindow::AllocSlot() const {
    bool used[100] = {};
    for (const auto& wl : m_watchlists)
        for (const auto& it : wl.items)
            if (it.subscribed) {
                int slot = it.reqId - m_mktBase;
                if (slot >= 0 && slot < 100) used[slot] = true;
            }
    for (int i = 0; i < 100; ++i)
        if (!used[i]) return i;
    return -1;
}

void WatchlistWindow::SubscribeItem(core::WatchlistItem& item, int slot) {
    item.reqId      = m_mktBase + slot;
    item.subscribed = true;
    if (OnReqMktData)
        OnReqMktData(item.reqId, item.symbol, item.secType, item.primaryExch, item.currency);
}

void WatchlistWindow::UnsubscribeItem(core::WatchlistItem& item) {
    if (item.subscribed && OnCancelMktData)
        OnCancelMktData(item.reqId);
    item.subscribed = false;
    item.reqId      = 0;
}

void WatchlistWindow::RemoveItem(core::Watchlist& wl, int idx) {
    if (idx < 0 || idx >= (int)wl.items.size()) return;
    UnsubscribeItem(wl.items[idx]);
    wl.items.erase(wl.items.begin() + idx);
}

void WatchlistWindow::CancelAll() {
    for (auto& wl : m_watchlists)
        for (auto& it : wl.items)
            UnsubscribeItem(it);
}

core::WatchlistItem* WatchlistWindow::FindByReqId(int reqId) {
    for (auto& wl : m_watchlists)
        for (auto& it : wl.items)
            if (it.reqId == reqId) return &it;
    return nullptr;
}

core::WatchlistItem* WatchlistWindow::FindBySymbol(const std::string& sym) {
    for (auto& wl : m_watchlists)
        for (auto& it : wl.items)
            if (it.symbol == sym) return &it;
    return nullptr;
}

// ============================================================================
// Row tint
// ============================================================================
void WatchlistWindow::PushRowTint(double changePct) {
    ImVec4 tint(0.f, 0.f, 0.f, 0.f);
    if (changePct > 0.0)       tint = ImVec4(0.10f, 0.35f, 0.10f, 0.30f);
    else if (changePct < 0.0)  tint = ImVec4(0.35f, 0.08f, 0.08f, 0.30f);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(tint));
}

void WatchlistWindow::PopRowTint() {}

// ============================================================================
// Sorted indices
// ============================================================================
std::vector<int> WatchlistWindow::SortedIndices(const core::Watchlist& wl) const {
    std::vector<int> idx(wl.items.size());
    std::iota(idx.begin(), idx.end(), 0);
    if (m_sortCol < 0) return idx;

    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        const auto& ia = wl.items[a];
        const auto& ib = wl.items[b];
        double va = 0.0, vb = 0.0;
        std::string sa, sb;
        bool useStr = false;
        switch (m_sortCol) {
            case 0:  sa = ia.symbol;           sb = ib.symbol;           useStr = true; break;
            case 1:  sa = ia.description;       sb = ib.description;       useStr = true; break;
            case 2:  va = ia.last;              vb = ib.last;              break;
            case 3:  va = ia.change;            vb = ib.change;            break;
            case 4:  va = ia.changePct;         vb = ib.changePct;         break;
            case 5:  va = ia.bid;               vb = ib.bid;               break;
            case 6:  va = ia.ask;               vb = ib.ask;               break;
            case 7:  va = ia.bidSize;           vb = ib.bidSize;           break;
            case 8:  va = ia.askSize;           vb = ib.askSize;           break;
            case 9:  va = ia.lastSize;          vb = ib.lastSize;          break;
            case 10: va = ia.ask - ia.bid;      vb = ib.ask - ib.bid;      break;
            case 11: va = ia.volume;            vb = ib.volume;            break;
            case 12: va = ia.avgVolume;         vb = ib.avgVolume;         break;
            case 13: va = ia.high;              vb = ib.high;              break;
            case 14: va = ia.low;               vb = ib.low;               break;
            case 15: va = ia.open;              vb = ib.open;              break;
            case 16: va = ia.prevClose;         vb = ib.prevClose;         break;
            case 17: va = ia.high52w;           vb = ib.high52w;           break;
            case 18: va = ia.low52w;            vb = ib.low52w;            break;
            case 19: sa = ia.secType;           sb = ib.secType;           useStr = true; break;
            case 20: sa = ia.primaryExch;       sb = ib.primaryExch;       useStr = true; break;
            case 21: sa = ia.currency;          sb = ib.currency;          useStr = true; break;
            default: break;
        }
        if (useStr) return m_sortAsc ? sa < sb : sa > sb;
        return m_sortAsc ? va < vb : va > vb;
    });
    return idx;
}

// ============================================================================
// Serialisation
// ============================================================================
std::string WatchlistWindow::serialize() const {
    std::ostringstream os;
    os << "INSTANCE:" << m_instanceId << '\n';
    os << "GROUP:" << m_groupId << '\n';
    for (const auto& wl : m_watchlists) {
        os << "WATCH:" << wl.name << '\n';
        for (const auto& it : wl.items)
            os << it.symbol << ',' << it.secType << ','
               << it.primaryExch << ',' << it.currency << ','
               << it.conId << ',' << it.description << '\n';
    }
    return os.str();
}

std::vector<core::Watchlist> WatchlistWindow::deserialize(const std::string& data) {
    std::vector<core::Watchlist> result;
    std::istringstream is(data);
    std::string line;
    while (std::getline(is, line)) {
        if (line.size() >= 6 && line.substr(0, 6) == "WATCH:") {
            result.push_back({line.substr(6), {}});
        } else if (!result.empty() && !line.empty()
                   && (line.size() < 9  || line.substr(0,9)  != "INSTANCE:")
                   && (line.size() < 6  || line.substr(0,6)  != "GROUP:")) {
            core::WatchlistItem item;
            std::istringstream row(line);
            std::string tok;
            int col = 0;
            while (std::getline(row, tok, ',')) {
                switch (col++) {
                    case 0: item.symbol      = tok; break;
                    case 1: item.secType     = tok; break;
                    case 2: item.primaryExch = tok; break;
                    case 3: item.currency    = tok; break;
                    case 4: try { item.conId = std::stoi(tok); } catch (...) {} break;
                    case 5: item.description = tok; break;
                }
            }
            if (!item.symbol.empty())
                result.back().items.push_back(std::move(item));
        }
    }
    return result;
}

void WatchlistWindow::LoadWatchlists(std::vector<core::Watchlist> wls) {
    CancelAll();
    m_watchlists = std::move(wls);
    if (m_watchlists.empty()) m_watchlists.push_back(core::Watchlist{});
    m_activeTab       = 0;
    m_activePreset    = -1;
    m_cdQueue.clear();
    m_cdEnrichPending = false;

    for (auto& wl : m_watchlists) {
        for (auto& item : wl.items) {
            int slot = AllocSlot();
            if (slot < 0) break;
            SubscribeItem(item, slot);
            if (item.description.empty())
                m_cdQueue.push_back(item.symbol);
        }
    }
}

// ============================================================================
// Render
// ============================================================================
bool WatchlistWindow::Render() {
    if (!m_open) return false;

    ProcessCdQueue();

    ImGui::SetNextWindowSize(ImVec2(em(460), em(320)), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing;
    if (!ImGui::Begin(m_title, &m_open, flags)) { ImGui::End(); return m_open; }

    DrawToolbar();
    ImGui::Separator();

    if (m_activeTab >= 0 && m_activeTab < (int)m_watchlists.size())
        DrawWatchlistTable(m_watchlists[m_activeTab]);

    ImGui::End();
    return m_open;
}

// ============================================================================
// Toolbar — group picker, tab bar, preset row, add-symbol
// ============================================================================
void WatchlistWindow::DrawToolbar() {
    // ---- Group picker -------------------------------------------------------
    core::DrawGroupPicker(m_groupId, "##wl_grp");
    ImGui::SameLine();

    // ---- Tab bar for watchlists + "+" button --------------------------------
    ImGuiTabBarFlags tbFlags = ImGuiTabBarFlags_Reorderable
                              | ImGuiTabBarFlags_FittingPolicyScroll;
    if (ImGui::BeginTabBar("##wltabs", tbFlags)) {
        for (int t = 0; t < (int)m_watchlists.size(); ++t) {
            // SetSelected is a one-shot signal — consume it for the target tab only
            ImGuiTabItemFlags tiFlags = ImGuiTabItemFlags_None;
            if (m_requestSelectTab == t) {
                tiFlags |= ImGuiTabItemFlags_SetSelected;
                m_requestSelectTab = -1;
            }

            char tabLabel[40];
            std::snprintf(tabLabel, sizeof(tabLabel), "%s×##wlclose%d",
                          m_watchlists[t].name.c_str(), t);

            bool selected = ImGui::BeginTabItem(tabLabel, nullptr, tiFlags);
            if (selected) {
                m_activeTab = t;
                ImGui::EndTabItem();
            }

            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_renameMode   = true;
                m_renameTabIdx = t;
                std::strncpy(m_renameBuf, m_watchlists[t].name.c_str(),
                             sizeof(m_renameBuf) - 1);
                m_renameBuf[sizeof(m_renameBuf)-1] = '\0';
                ImGui::OpenPopup("##wl_rename");
            }
        }

        if (ImGui::TabItemButton("+##wlnew", ImGuiTabItemFlags_Trailing)) {
            core::Watchlist wl;
            char name[20];
            std::snprintf(name, sizeof(name), "List %d", (int)m_watchlists.size() + 1);
            wl.name = name;
            m_watchlists.push_back(std::move(wl));
            m_activeTab        = (int)m_watchlists.size() - 1;
            m_requestSelectTab = m_activeTab;
        }

        ImGui::EndTabBar();
    }

    // ---- Rename popup -------------------------------------------------------
    if (ImGui::BeginPopup("##wl_rename")) {
        ImGui::Text("Rename:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em(100));
        if (ImGui::InputText("##wlrename", m_renameBuf, sizeof(m_renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (m_renameTabIdx >= 0 && m_renameTabIdx < (int)m_watchlists.size())
                m_watchlists[m_renameTabIdx].name = m_renameBuf;
            m_renameMode   = false;
            m_renameTabIdx = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- Preset row ---------------------------------------------------------
    {
        const auto& presets = GetPresets();
        FlexRow prow;

        // Combo label shows active preset name or "Custom"
        const char* presetLabel = (m_activePreset >= 0 && m_activePreset < (int)presets.size())
                                       ? presets[m_activePreset].name.c_str()
                                       : "Custom";

        prow.item(em(130));
        ImGui::SetNextItemWidth(em(120));
        if (ImGui::BeginCombo("##wlpre", presetLabel)) {
            if (ImGui::Selectable("Custom", m_activePreset < 0))
                m_activePreset = -1;
            ImGui::Separator();
            for (int i = 0; i < (int)presets.size(); ++i) {
                bool sel = (m_activePreset == i);
                if (ImGui::Selectable(presets[i].name.c_str(), sel))
                    SwitchToPreset(i);
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Switch saved preset");

        prow.item(em(68));
        if (ImGui::Button("Save As##wlsave", ImVec2(em(68), 0))) {
            // Pre-fill name from active preset
            const char* defName = (m_activePreset >= 0 && m_activePreset < (int)presets.size())
                                       ? presets[m_activePreset].name.c_str()
                                       : "";
            std::strncpy(m_saveNameBuf, defName, sizeof(m_saveNameBuf) - 1);
            m_saveNameBuf[sizeof(m_saveNameBuf) - 1] = '\0';
            m_savePopupOpen = true;
        }

        if (m_activePreset >= 0 && m_activePreset < (int)presets.size()) {
            prow.item(em(56));
            if (ImGui::Button("Delete##wldel", ImVec2(em(56), 0)))
                DeletePreset(m_activePreset);
        }
    }

    // Save As modal
    if (m_savePopupOpen) {
        ImGui::OpenPopup("Save Preset##wlsavepop");
        m_savePopupOpen = false;
    }
    if (ImGui::BeginPopupModal("Save Preset##wlsavepop", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Preset name:");
        ImGui::SetNextItemWidth(em(180));
        bool confirm = ImGui::InputText("##wlsavename", m_saveNameBuf, sizeof(m_saveNameBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        if (confirm || ImGui::Button("Save##wlsavebtn")) {
            SaveCurrentAsPreset(m_saveNameBuf);
            std::memset(m_saveNameBuf, 0, sizeof(m_saveNameBuf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##wlcancelbtn"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- Add-symbol / Remove-tab / Columns row --------------------------------
    {
        FlexRow row;
        if (m_addSymActive) {
            ImGui::SetNextItemWidth(em(100));
            if (DrawSymbolInput("##wladd", m_addSymBuf, sizeof(m_addSymBuf),
                                em(100),
                                [&](const std::string& sym) {
                                    if (sym.empty()) { m_addSymActive = false; return; }
                                    int slot = AllocSlot();
                                    if (slot < 0) { m_addSymActive = false; return; }
                                    m_pendingSlot   = slot;
                                    m_pendingSymbol = sym;
                                    if (OnReqContractDetails)
                                        OnReqContractDetails(m_cdReqId, sym);
                                    m_addSymActive = false;
                                    std::memset(m_addSymBuf, 0, sizeof(m_addSymBuf));
                                })) { /* confirmed in lambda */ }
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_addSymActive = false;
                std::memset(m_addSymBuf, 0, sizeof(m_addSymBuf));
            }
        } else {
            row.item(em(90));
            if (ImGui::Button("+ Add Symbol##wladd", ImVec2(em(90), 0))) {
                m_addSymActive = true;
                std::memset(m_addSymBuf, 0, sizeof(m_addSymBuf));
            }

            if (m_activeTab >= 0 && m_activeTab < (int)m_watchlists.size()
                    && m_watchlists.size() > 1) {
                row.item(em(80));
                if (ImGui::Button("Remove Tab##wlrmtab", ImVec2(em(80), 0))) {
                    auto& wl = m_watchlists[m_activeTab];
                    for (auto& it : wl.items) UnsubscribeItem(it);
                    m_watchlists.erase(m_watchlists.begin() + m_activeTab);
                    if (m_activeTab >= (int)m_watchlists.size())
                        m_activeTab = (int)m_watchlists.size() - 1;
                }
            }

            row.item(em(65));
            if (ImGui::Button("Columns##wlcols", ImVec2(em(65), 0)))
                m_colPopupOpen = true;
        }
    }

    // ---- Columns popup -------------------------------------------------------
    if (m_colPopupOpen) {
        ImGui::OpenPopup("##wlcolspop");
        m_colPopupOpen = false;
    }
    if (ImGui::BeginPopup("##wlcolspop")) {
        ImGui::TextUnformatted("Show / Hide Columns");
        ImGui::Separator();
        // Render in two side-by-side columns for compactness
        if (ImGui::BeginTable("##colchk", 2, ImGuiTableFlags_None)) {
            for (int c = 0; c < kNumCols; ++c) {
                ImGui::TableNextColumn();
                if (c == 0) {
                    // Symbol is mandatory
                    bool dummy = true;
                    ImGui::BeginDisabled();
                    ImGui::Checkbox(kColDefs[c].name, &dummy);
                    ImGui::EndDisabled();
                } else {
                    ImGui::Checkbox(kColDefs[c].name, &m_colEnabled[c]);
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndPopup();
    }
}

// ============================================================================
// Watchlist table
// ============================================================================
void WatchlistWindow::DrawWatchlistTable(core::Watchlist& wl) {
    // Build mapping: tableColIdx → kColDefs index (only enabled cols)
    int colMap[kNumCols];
    int numCols = 0;
    for (int c = 0; c < kNumCols; ++c)
        if (m_colEnabled[c]) colMap[numCols++] = c;
    if (numCols == 0) return;

    constexpr ImGuiTableFlags kTblFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;

    const float rowH   = ImGui::GetTextLineHeightWithSpacing();
    const float tableH = ImGui::GetContentRegionAvail().y - rowH;

    if (!ImGui::BeginTable("##wltbl", numCols, kTblFlags, ImVec2(0, tableH)))
        return;

    ImGui::TableSetupScrollFreeze(1, 1);
    for (int tc = 0; tc < numCols; ++tc) {
        int c = colMap[tc];
        ImGuiTableColumnFlags cflags = (c == 0) ? ImGuiTableColumnFlags_DefaultSort
                                                 : ImGuiTableColumnFlags_None;
        ImGui::TableSetupColumn(kColDefs[c].name, cflags, em(kColDefs[c].width));
    }
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            // Map table column index → kColDefs index
            m_sortCol = colMap[specs->Specs[0].ColumnIndex];
            m_sortAsc = (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
            specs->SpecsDirty = false;
        }
    }

    // Formatting helpers
    auto priceStr = [](double v, char* buf, int sz) {
        if (v == 0.0) std::snprintf(buf, sz, "--");
        else          std::snprintf(buf, sz, "%.2f", v);
    };
    auto sizeStr = [](double v, char* buf, int sz) {
        if (v == 0.0)   std::snprintf(buf, sz, "--");
        else if (v >= 1e6) std::snprintf(buf, sz, "%.1fM", v / 1e6);
        else if (v >= 1e3) std::snprintf(buf, sz, "%.0fK", v / 1e3);
        else               std::snprintf(buf, sz, "%.0f",  v);
    };

    const auto order = SortedIndices(wl);
    int removeIdx = -1;
    std::string openChart, openBook;

    for (int ri = 0; ri < (int)order.size(); ++ri) {
        int i = order[ri];
        auto& item = wl.items[i];

        ImGui::TableNextRow();
        PushRowTint(item.changePct);

        char cellBuf[32];

        for (int tc = 0; tc < numCols; ++tc) {
            ImGui::TableSetColumnIndex(tc);
            int c = colMap[tc];

            switch (c) {
            case 0: { // Symbol — selectable spanning all columns + context menu
                char selId[32];
                std::snprintf(selId, sizeof(selId), "##wlrow%d", i);
                bool clicked = ImGui::Selectable(item.symbol.c_str(), false,
                                                 ImGuiSelectableFlags_SpanAllColumns
                                                 | ImGuiSelectableFlags_AllowOverlap,
                                                 ImVec2(0, rowH - 2));
                if (clicked && OnBroadcastSymbol)
                    OnBroadcastSymbol(item.symbol);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    openChart = item.symbol;
                if (ImGui::BeginPopupContextItem(selId)) {
                    if (ImGui::MenuItem("Open Chart"))      openChart = item.symbol;
                    if (ImGui::MenuItem("Open Order Book")) openBook  = item.symbol;
                    ImGui::Separator();
                    if (ImGui::MenuItem("Remove"))          removeIdx = i;
                    ImGui::EndPopup();
                }
                break;
            }
            case 1:  ImGui::TextUnformatted(item.description.c_str()); break;
            case 2:  priceStr(item.last, cellBuf, sizeof(cellBuf));
                     ImGui::TextUnformatted(cellBuf); break;
            case 3:
                if (item.change != 0.0) {
                    ImVec4 col = item.change > 0 ? ImVec4(0.2f,0.85f,0.4f,1) : ImVec4(0.9f,0.28f,0.28f,1);
                    ImGui::TextColored(col, "%+.2f", item.change);
                } else { ImGui::TextUnformatted("--"); }
                break;
            case 4:
                if (item.changePct != 0.0) {
                    ImVec4 col = item.changePct > 0 ? ImVec4(0.2f,0.85f,0.4f,1) : ImVec4(0.9f,0.28f,0.28f,1);
                    ImGui::TextColored(col, "%+.2f%%", item.changePct);
                } else { ImGui::TextUnformatted("--"); }
                break;
            case 5:  priceStr(item.bid,    cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 6:  priceStr(item.ask,    cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 7:  sizeStr(item.bidSize,  cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 8:  sizeStr(item.askSize,  cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 9:  sizeStr(item.lastSize, cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 10: {
                double spread = (item.bid > 0.0 && item.ask > 0.0) ? item.ask - item.bid : 0.0;
                priceStr(spread, cellBuf, sizeof(cellBuf));
                ImGui::TextUnformatted(cellBuf);
                break;
            }
            case 11: sizeStr(item.volume,    cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 12: sizeStr(item.avgVolume, cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 13: priceStr(item.high,      cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 14: priceStr(item.low,       cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 15: priceStr(item.open,      cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 16: priceStr(item.prevClose, cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 17: priceStr(item.high52w,   cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 18: priceStr(item.low52w,    cellBuf, sizeof(cellBuf)); ImGui::TextUnformatted(cellBuf); break;
            case 19: ImGui::TextUnformatted(item.secType.c_str());     break;
            case 20: ImGui::TextUnformatted(item.primaryExch.c_str()); break;
            case 21: ImGui::TextUnformatted(item.currency.c_str());    break;
            default: break;
            }
        }
    }

    ImGui::EndTable();

    if (removeIdx >= 0)                          RemoveItem(wl, removeIdx);
    if (!openChart.empty() && OnOpenChart)       OnOpenChart(openChart);
    if (!openBook.empty()  && OnOpenOrderBook)   OnOpenOrderBook(openBook);
}

}  // namespace ui
