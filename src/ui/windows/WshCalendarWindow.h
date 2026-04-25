#pragma once

#include "imgui.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

struct WshCalendarEntry {
    int         conId       = 0;
    std::string symbol;
    std::string date;        // "YYYY-MM-DD"
    std::string type;        // "Earnings", "Dividend", "Split", …
    std::string description;
    std::string importance;  // "High", "Medium", "Low"
};

// ============================================================================
// WshCalendarWindow — cross-symbol WSH corporate event calendar
//
// Singleton. Subscribes reqWshEventData for every held position's conId plus
// any open chart symbol conIds passed via SubscribeConId(). Stores all
// upcoming events in a flat list with filter and sort controls.
//
// reqId layout: 8070–8199 (130 slots)
// ============================================================================
class WshCalendarWindow {
public:
    static constexpr int kReqBase = 8070;
    static constexpr int kReqEnd  = 8199;
    static constexpr int kSlots   = kReqEnd - kReqBase + 1;  // 130

    WshCalendarWindow();
    ~WshCalendarWindow();

    bool  Render();
    bool& open() { return m_open; }

    // Add / refresh a position-conId subscription (idempotent).
    // Called from main.cpp on each position update and from FinishConnect.
    void SubscribeConId(int conId, const std::string& symbol);

    // Route a raw WSH JSON blob (reqIds 8070–8199 from main.cpp onWshEvent).
    void OnWshEvent(int reqId, const std::string& jsonData);

    // Cancel all active subscriptions (called before destroy).
    void CancelAll();

    // Callbacks wired by main.cpp
    std::function<void(int reqId, long conId)> OnReqWshEvents;
    std::function<void(int reqId)>             OnCancelWshEvents;
    std::function<void(const std::string&)>    OnBroadcastSymbol;

private:
    bool m_open = true;

    // Subscription tracking: conId → {reqId, symbol}
    struct Sub { int reqId; std::string symbol; };
    std::unordered_map<int, Sub> m_subs;
    bool m_usedSlots[kSlots] = {};

    // Received events (deduplicated by conId+type+date)
    std::vector<WshCalendarEntry> m_events;

    // ---- Filter state -------------------------------------------------------
    char m_filterSymbol[16] = {};
    char m_filterFrom[11]   = {};  // "YYYY-MM-DD" or empty
    char m_filterTo[11]     = {};  // "YYYY-MM-DD" or empty
    int  m_filterType       = 0;   // 0=All 1=Earnings 2=Dividend 3=Split 4=Other
    int  m_filterImportance = 0;   // 0=All 1=High 2=Medium 3=Low

    // Calendar picker nav state: [0]=From picker, [1]=To picker
    int m_calNavYear[2]  = {2025, 2025};
    int m_calNavMonth[2] = {1, 1};   // 1–12

    // ---- Sort state ---------------------------------------------------------
    int  m_sortCol = 0;   // 0=Date (default) 1=Symbol 2=Type 3=Importance 4=Desc
    bool m_sortAsc = true;

    // ---- Helpers ------------------------------------------------------------
    int  AllocSlot() const;
    void Subscribe(int conId, const std::string& symbol);

    bool         PassesFilter(const WshCalendarEntry& e) const;
    static ImVec4 TypeColor(const std::string& type);
    // idx=0 → From, idx=1 → To
    void DrawDatePicker(int idx, const char* label, char* buf);
    void DrawFilterToolbar();
    void DrawTable();
};

}  // namespace ui
