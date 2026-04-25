#pragma once

#include "core/models/WatchlistData.h"
#include <deque>
#include <string>
#include <vector>
#include <functional>

namespace ui {

// ============================================================================
// WatchlistWindow — multi-instance symbol watchlist with live IB ticks
//
//  ┌────────────────────────────────────────────────────────┐
//  │  G1  [ Watchlist 1 × ] [ Watchlist 2 × ] [ + ]        │
//  │  [Preset: Mag 7 ▼] [Save As] [Delete]                  │
//  │  [ Add Symbol ]                                        │
//  ├────────────────────────────────────────────────────────┤
//  │  Symbol │ Company │ Last │ Chg │ %Chg │ Bid │ Ask │ … │
//  └────────────────────────────────────────────────────────┘
//
// reqId layout (per instance idx 0-9):
//   contract details: 6900 + idx   (transient, one in flight)
//   market data:      7000 + idx*100 + symbolSlot  (0-99 per instance)
// ============================================================================
class WatchlistWindow {
public:
    // ---- Saved preset — shared across all WatchlistWindow instances ----------
    struct SavedPreset {
        std::string              name;
        std::vector<core::Watchlist> watchlists;
    };

    WatchlistWindow();
    ~WatchlistWindow();

    bool  Render();
    bool& open() { return m_open; }
    void  setGroupId(int id) { m_groupId = id; }
    int   groupId() const    { return m_groupId; }
    void  setInstanceId(int id);
    int   instanceId() const { return m_instanceId; }

    // reqId allocation — called by SpawnWatchlistWindow
    void setReqIdBase(int cdReqId, int mktBase) { m_cdReqId = cdReqId; m_mktBase = mktBase; }

    // Called after callbacks are wired — loads preset 0 if window is empty
    void AddDefaultsIfEmpty();

    // Called from main.cpp when contractDetails arrives for m_cdReqId
    void SetContractDetails(int reqId, long conId, const std::string& description,
                            const std::string& secType, const std::string& primaryExch,
                            const std::string& currency);

    // Called from main.cpp on tick updates (routed by reqId range)
    void OnTickPrice(int reqId, int field, double price);
    void OnTickSize(int reqId, int field, double size);

    // Cancel all active market-data subscriptions (call before destroying)
    void CancelAll();

    // ---- Preset management (static — shared across all instances) -----------
    static const std::vector<SavedPreset>& GetPresets();
    static void LoadPresetsFile();
    static void SavePresetsFile();

    void SwitchToPreset(int idx);
    void SaveCurrentAsPreset(const std::string& name);
    void DeletePreset(int idx);

    // ---- Callbacks wired by SpawnWatchlistWindow ----------------------------
    std::function<void(int reqId, const std::string& symbol)> OnReqContractDetails;
    std::function<void(int reqId, const std::string& symbol,
                       const std::string& secType, const std::string& exchange,
                       const std::string& currency)> OnReqMktData;
    std::function<void(int reqId)> OnCancelMktData;
    std::function<void(const std::string& symbol)> OnBroadcastSymbol;
    std::function<void(const std::string& symbol)> OnOpenChart;
    std::function<void(const std::string& symbol)> OnOpenOrderBook;

    // ---- Serialisation (Task #49) -------------------------------------------
    std::string serialize() const;
    static std::vector<core::Watchlist> deserialize(const std::string& data);
    void LoadWatchlists(std::vector<core::Watchlist> wls);
    const std::vector<core::Watchlist>& watchlists() const { return m_watchlists; }

    static constexpr int kNumCols = 22;

private:
    // ---- State --------------------------------------------------------------
    bool m_open        = true;
    int  m_groupId     = 0;
    int  m_instanceId  = 1;
    char m_title[40]   = "Watchlist 1##watchlist1";

    int m_cdReqId  = 6900;
    int m_mktBase  = 7000;

    // ---- Watchlist data -----------------------------------------------------
    std::vector<core::Watchlist> m_watchlists;
    int m_activeTab       = 0;
    int m_requestSelectTab = -1;   // -1 = none; set once to programmatically jump to a tab

    // ---- Add-symbol inline state --------------------------------------------
    bool        m_addSymActive  = false;
    char        m_addSymBuf[16] = {};
    int         m_pendingSlot   = -1;
    std::string m_pendingSymbol;

    // ---- Description enrichment queue (for bulk-loaded symbols) -------------
    std::deque<std::string> m_cdQueue;
    bool        m_cdEnrichPending = false;
    std::string m_cdEnrichSymbol;

    // ---- Tab rename state ---------------------------------------------------
    bool m_renameMode   = false;
    int  m_renameTabIdx = -1;
    char m_renameBuf[32] = {};

    // ---- Column visibility --------------------------------------------------
    bool m_colEnabled[kNumCols] = {};
    bool m_colPopupOpen = false;

    // ---- Sort state ---------------------------------------------------------
    int  m_sortCol = -1;
    bool m_sortAsc = true;

    // ---- Preset UI state ----------------------------------------------------
    int  m_activePreset  = -1;   // index into s_presets; -1 = custom
    bool m_savePopupOpen = false;
    char m_saveNameBuf[48] = {};

    // ---- Static preset store ------------------------------------------------
    static std::vector<SavedPreset> s_presets;
    static bool                     s_presetsLoaded;

    // ---- Private helpers ----------------------------------------------------
    void DrawToolbar();
    void DrawWatchlistTable(core::Watchlist& wl);
    void ProcessCdQueue();

    int AllocSlot() const;
    void RemoveItem(core::Watchlist& wl, int itemIdx);
    void SubscribeItem(core::WatchlistItem& item, int slot);
    void UnsubscribeItem(core::WatchlistItem& item);
    core::WatchlistItem* FindByReqId(int reqId);
    core::WatchlistItem* FindBySymbol(const std::string& sym);
    std::vector<int> SortedIndices(const core::Watchlist& wl) const;

    static void PushRowTint(double changePct);
    static void PopRowTint();
    static void EnsureDefaultPreset();
};

}  // namespace ui
