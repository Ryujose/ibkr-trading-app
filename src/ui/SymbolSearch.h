#pragma once
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "imgui.h"

namespace ui {

struct SymbolResult {
    std::string symbol;
    std::string secType;
    std::string primaryExch;
    std::string currency;
};

// Set by main.cpp once the IB client is connected; cleared on disconnect.
// DrawSymbolInput calls this with the debounced search pattern.
inline std::function<void(const std::string&)> g_symbolSearchFn;

// Called from main.cpp's onSymbolSamples callback to populate the dropdown.
inline void UpdateSymbolSearchResults(std::vector<SymbolResult> r);

namespace detail {
    struct SymbolSearchState {
        std::vector<SymbolResult> results;
        double  debounceEnd       = 0.0;
        int     selected          = -1;
        char    lastQuery[33]     = {};
        char    lastConfirmed[33] = {}; // last IB-validated symbol for this input
        char    searchedQuery[33] = {}; // query that was actually sent to IB
        bool    popupOpen         = false;
        ImGuiID ownerID           = 0;
        bool    searching         = false;
        bool    searched          = false;
    };
    inline SymbolSearchState g_ss;
}

inline void UpdateSymbolSearchResults(std::vector<SymbolResult> r) {
    auto& ss = detail::g_ss;
    ss.searching = false;
    ss.searched  = true;
    ss.results   = std::move(r);
    ss.selected  = ss.results.empty() ? -1 : 0;
    ss.popupOpen = !ss.results.empty();
}

// Reusable symbol InputText with live IB search dropdown.
//
// When g_symbolSearchFn is set (IB connected):
//   - Plain Enter is blocked once any search is pending or done.
//     The user must select from the dropdown to confirm.
//   - If Enter is pressed while the debounce is still pending, the search
//     fires immediately so the dropdown appears right away.
//   - If IB returns no results the input reverts to the last confirmed symbol.
// When g_symbolSearchFn is null (disconnected):
//   - Plain Enter always confirms the raw buffer.
//
// onConfirm: called with the confirmed symbol; buf already contains it.
inline bool DrawSymbolInput(const char* id, char* buf, int bufSize, float width,
                            const std::function<void(const std::string&)>& onConfirm) {
    auto& ss = detail::g_ss;

    ImGui::SetNextItemWidth(width);
    const bool textChanged  = ImGui::InputText(id, buf, bufSize,
                                               ImGuiInputTextFlags_CharsUppercase);
    const ImGuiID  itemID      = ImGui::GetItemID();
    const ImVec2   itemMin     = ImGui::GetItemRectMin();
    const ImVec2   itemMax     = ImGui::GetItemRectMax();
    const bool     inputActive = ImGui::IsItemActive();
    const bool     inputDeact  = ImGui::IsItemDeactivated();

    // When a different field becomes active, reset state for the new owner
    // and capture the current buf as the baseline confirmed symbol.
    if (inputActive && ss.ownerID != itemID) {
        ss.ownerID     = itemID;
        ss.popupOpen   = false;
        ss.results.clear();
        ss.debounceEnd = 0.0;
        ss.selected    = -1;
        ss.searching   = false;
        ss.searched    = false;
        std::memset(ss.lastQuery,     0, sizeof(ss.lastQuery));
        std::memset(ss.searchedQuery, 0, sizeof(ss.searchedQuery));
        std::strncpy(ss.lastConfirmed, buf, sizeof(ss.lastConfirmed) - 1);
        ss.lastConfirmed[sizeof(ss.lastConfirmed) - 1] = '\0';
    }

    bool confirmed = false;

    if (ss.ownerID != itemID)
        return false;

    // Schedule debounced search on text change.
    if (textChanged) {
        if (buf[0] == '\0') {
            ss.results.clear();
            ss.popupOpen   = false;
            ss.debounceEnd = 0.0;
            ss.searching   = false;
            ss.searched    = false;
        } else if (std::strncmp(buf, ss.lastQuery, sizeof(ss.lastQuery) - 1) != 0) {
            std::strncpy(ss.lastQuery, buf, sizeof(ss.lastQuery) - 1);
            ss.debounceEnd = ImGui::GetTime() + 0.30;
            ss.results.clear();
            ss.popupOpen = false;
            ss.selected  = -1;
            ss.searching = false;
            ss.searched  = false;
        }
    }

    // Fire search after debounce expires.
    if (ss.debounceEnd > 0.0 && ImGui::GetTime() >= ss.debounceEnd && buf[0] != '\0') {
        ss.debounceEnd = 0.0;
        ss.searching   = true;
        std::strncpy(ss.searchedQuery, buf, sizeof(ss.searchedQuery) - 1);
        ss.searchedQuery[sizeof(ss.searchedQuery) - 1] = '\0';
        if (g_symbolSearchFn) g_symbolSearchFn(std::string(buf));
    }

    // If IB returned no results for exactly what's in buf, revert to the last
    // confirmed symbol so the input doesn't show an unrecognised string.
    if (ss.searched && ss.results.empty()
            && std::strncmp(buf, ss.searchedQuery, sizeof(ss.searchedQuery) - 1) == 0) {
        std::strncpy(buf, ss.lastConfirmed, bufSize - 1);
        buf[bufSize - 1] = '\0';
        ss.searching = false;
        ss.searched  = false;
        ss.debounceEnd = 0.0;
        std::memset(ss.lastQuery,     0, sizeof(ss.lastQuery));
        std::memset(ss.searchedQuery, 0, sizeof(ss.searchedQuery));
    }

    // Keyboard navigation while the InputText has focus.
    if (inputActive && ss.popupOpen && !ss.results.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
            ss.selected = std::min(ss.selected + 1, (int)ss.results.size() - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
            ss.selected = std::max(ss.selected - 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ss.popupOpen = false;
            ss.results.clear();
            ss.ownerID   = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ss.selected >= 0) {
            const std::string sym = ss.results[ss.selected].symbol;
            std::strncpy(buf, sym.c_str(), bufSize - 1);
            buf[bufSize - 1] = '\0';
            std::strncpy(ss.lastConfirmed, sym.c_str(), sizeof(ss.lastConfirmed) - 1);
            ss.lastConfirmed[sizeof(ss.lastConfirmed) - 1] = '\0';
            onConfirm(sym);
            ss.popupOpen   = false;
            ss.results.clear();
            ss.debounceEnd = 0.0;
            ss.searching   = false;
            ss.searched    = false;
            ss.ownerID     = 0;
            confirmed = true;
        }
    }

    // Plain Enter with no popup open.
    // When IB is connected: only allow if no search has started (re-confirm/reload).
    // If debounce is pending, fire immediately and block — dropdown will appear.
    if (!confirmed && inputActive && !ss.popupOpen
            && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && buf[0] != '\0') {
        const bool ibActive = static_cast<bool>(g_symbolSearchFn);
        if (!ibActive) {
            onConfirm(std::string(buf));
            ss.debounceEnd = 0.0;
            ss.searching   = false;
            ss.searched    = false;
            confirmed = true;
        } else if (!ss.searching && !ss.searched && ss.debounceEnd == 0.0) {
            // Nothing changed — re-confirm current symbol (reload).
            std::strncpy(ss.lastConfirmed, buf, sizeof(ss.lastConfirmed) - 1);
            ss.lastConfirmed[sizeof(ss.lastConfirmed) - 1] = '\0';
            onConfirm(std::string(buf));
            confirmed = true;
        } else {
            // Search pending or done: fire immediately if still debouncing,
            // then block — user must pick from the dropdown.
            if (ss.debounceEnd > 0.0) {
                ss.debounceEnd = 0.0;
                ss.searching   = true;
                std::strncpy(ss.searchedQuery, buf, sizeof(ss.searchedQuery) - 1);
                ss.searchedQuery[sizeof(ss.searchedQuery) - 1] = '\0';
                g_symbolSearchFn(std::string(buf));
            }
        }
    }

    // Render the dropdown as a floating window positioned below the input.
    bool popupHovered = false;
    if (ss.popupOpen && !ss.results.empty()) {
        const float dropW = std::max(width + 80.0f, itemMax.x - itemMin.x + 80.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
        ImGui::SetNextWindowPos(ImVec2(itemMin.x, itemMax.y + 2.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(dropW, 0.0f), ImGuiCond_Always);
        constexpr ImGuiWindowFlags kDropFlags =
            ImGuiWindowFlags_NoTitleBar     | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoScrollbar    | ImGuiWindowFlags_NoMove    |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav   |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##symdrop", nullptr, kDropFlags)) {
            popupHovered = ImGui::IsWindowHovered(
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            for (int i = 0; i < (int)ss.results.size() && i < 10; ++i) {
                const auto& r     = ss.results[i];
                const bool  isSel = (i == ss.selected);
                char row[80];
                std::snprintf(row, sizeof(row), "%-8s %-4s %-8s %s",
                              r.symbol.c_str(), r.secType.c_str(),
                              r.primaryExch.c_str(), r.currency.c_str());
                if (ImGui::Selectable(row, isSel)) {
                    std::strncpy(buf, r.symbol.c_str(), bufSize - 1);
                    buf[bufSize - 1] = '\0';
                    std::strncpy(ss.lastConfirmed, r.symbol.c_str(), sizeof(ss.lastConfirmed) - 1);
                    ss.lastConfirmed[sizeof(ss.lastConfirmed) - 1] = '\0';
                    onConfirm(r.symbol);
                    ss.popupOpen   = false;
                    ss.results.clear();
                    ss.debounceEnd = 0.0;
                    ss.searching   = false;
                    ss.searched    = false;
                    ss.ownerID     = 0;
                    confirmed = true;
                }
                if (isSel && !confirmed) ImGui::SetScrollHereY();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Close popup when the InputText loses focus and the mouse is not over the dropdown.
    // Also revert the buffer to the last confirmed symbol if text was changed but not confirmed.
    if (inputDeact && !popupHovered && !confirmed) {
        if (g_symbolSearchFn && ss.lastConfirmed[0] != '\0'
                && std::strncmp(buf, ss.lastConfirmed, bufSize - 1) != 0) {
            std::strncpy(buf, ss.lastConfirmed, bufSize - 1);
            buf[bufSize - 1] = '\0';
        }
        ss.popupOpen   = false;
        ss.results.clear();
        ss.debounceEnd = 0.0;
        ss.searching   = false;
        ss.searched    = false;
    }

    return confirmed;
}

} // namespace ui
