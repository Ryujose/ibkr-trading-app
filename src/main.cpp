/**
 * Interactive Brokers Trading Application — Main Entry Point
 *
 * Connects to a running IB Gateway or TWS via the C++ TWS API.
 * Authentication is handled by TWS/Gateway itself; we only need host/port/clientId.
 */

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <filesystem>
#include <deque>

// Platform-specific exe-path discovery (used in the asset-dir resolver).
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "implot.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "ui/windows/ChartWindow.h"
#include "ui/windows/NewsWindow.h"
#include "ui/windows/TradingWindow.h"
#include "ui/windows/ScannerWindow.h"
#include "ui/windows/PortfolioWindow.h"
#include "ui/windows/OrdersWindow.h"
#include "ui/windows/WatchlistWindow.h"
#include "ui/windows/WshCalendarWindow.h"
#include "ui/windows/ReplayWindow.h"
#include "ui/windows/NotificationsWindow.h"
#include "ui/SymbolSearch.h"

#include "core/services/IBKRClient.h"
#include "core/services/IBKRUtils.h"
#include "core/services/NotificationService.h"
#include "core/services/state-io.h"
#include "core/models/WindowGroup.h"
#include "ui/NotificationOverlay.h"

// ============================================================================
// Multi-instance window entry structs
// ============================================================================
struct ChartEntry {
    ui::ChartWindow* win         = nullptr;
    int              histId      = 0;   // reqHistoricalData id
    int              extId       = 0;   // extend-history (pan-left) id
    int              mktId       = 0;   // reqMarketData id (chart ticks)
    int              wshId       = 0;   // reqContractDetails / reqWshEventData id
    bool             wshConIdFired = false;
    // Stream-active flags: a historical request enters "active" when issued
    // and exits when its done=true is dispatched (or a fresh request rotates
    // the id and abandons the old). A late historical bar arriving after the
    // stream ended would otherwise accumulate into pendingBars and trigger a
    // phantom second SetHistoricalData (visible symptom: "everything erased
    // and a single candle appears" when only one such bar got through).
    bool             histStreamActive = false;
    bool             extStreamActive  = false;
    core::BarSeries  pendingBars;
    core::BarSeries  pendingExtBars;
    std::string      bboExchange;       // last bboExchange code from tickReqParams
};

struct TradingEntry {
    ui::TradingWindow* win          = nullptr;
    int                depthId      = 0;   // reqMktDepth id
    int                mktId        = 0;   // reqMarketData id (NBBO ticks)
    int                tickId       = 0;   // reqTickByTickData id (Time & Sales)
    double nbboBid = 0, nbboBidSz = 0;
    double nbboAsk = 0, nbboAskSz = 0;
    double lastTickPrice = 0, lastTickSize = 0;
    std::string        bboExchange;         // last bboExchange code from tickReqParams
};

struct ScannerEntry {
    ui::ScannerWindow* win           = nullptr;
    int                scanBase      = 0;   // reqId pool base (scanBase..scanBase+99)
    int                activeScanId  = 0;
    bool               subActive     = false;
    int                mktBase       = 0;   // market-data base for live quotes
    static constexpr int kMktSlots   = 12;
    std::vector<core::ScanResult> pendingResults;
};

static constexpr int   kMaxMultiWin = 10;   // max instances per window type
static constexpr float kTitleBarH   = 32.0f; // custom title bar height
static bool            g_tbDragging = false; // title-bar drag in progress (shared with resize handler)

struct NewsEntry {
    ui::NewsWindow*              win          = nullptr;
    int                          nextArtReqId = 0;
    int                          artEnd       = 0;
    std::unordered_map<int,int>  artReqToItemId;
    std::unordered_map<int,bool> newsConIdFired;
};

struct WatchlistEntry {
    ui::WatchlistWindow* win = nullptr;
};

struct ReplayEntry {
    ui::ReplayWindow* win        = nullptr;
    int               baseReqId  = 0;    // 11000 + idx * 100
    int               histId     = 0;    // baseReqId + 0: reqHistoricalData
    int               extId      = 0;    // baseReqId + 1: extend-history
    core::HistoricalDay day;             // current loaded day
    core::BarSeries     pendingBars;     // accumulating bars from hist fetch
    std::vector<int>    pendingReqIds;   // in-flight IB reqIds
    bool               histActive  = false; // true while a hist fetch is in-flight
};

// ---- Multi-instance containers -----------------------------------------------
static std::vector<ChartEntry>      g_chartEntries;
static std::vector<TradingEntry>    g_tradingEntries;
static std::vector<ScannerEntry>    g_scannerEntries;
static std::vector<NewsEntry>       g_newsEntries;
static std::vector<WatchlistEntry>  g_watchlistEntries;
static std::vector<ReplayEntry>     g_replayEntries;

// ---- Singleton windows (one each) --------------------------------------------
static ui::PortfolioWindow*    g_PortfolioWindow    = nullptr;
static ui::OrdersWindow*       g_OrdersWindow       = nullptr;
static ui::WshCalendarWindow*  g_WshCalendarWindow  = nullptr;
static ui::NotificationsWindow* g_NotificationsWindow = nullptr;

// IB API client (created on Connect, deleted on Disconnect)
static core::services::IBKRClient* g_IBClient = nullptr;

// Notifications (created in main(), destroyed before ImGui shutdown).
// Audio init failure (no device, headless CI) leaves the service alive — toasts
// still queue, audio is silenced. All Notify() call sites guard on non-null.
static std::unique_ptr<core::services::NotificationService> g_NotificationService;

// Order ids we just placed locally and haven't yet observed accepted at IB.
// IB sends `onOpenOrder` (status=Working) before `onOrderStatusChanged`, and
// `onOpenOrder` also fires for every existing order on `reqAllOpenOrders`
// (connect/reconnect) — so we can't infer "first acceptance" from status
// alone. Inserted at every PlaceOrder() submission site; consumed on the
// first observation of Working/PartialFill (which then fires the
// OrderWorking / OrderHeld toast); also pruned on terminal status so a
// rejected-on-submit order doesn't leak.
static std::unordered_set<int> g_pendingLocalAccept;

// Previous-close prices keyed by symbol, used to compute change/% for scanner rows.
static std::unordered_map<std::string, double> g_scannerPrevClose;
// Day volume keyed by symbol for scanner rows.
static std::unordered_map<std::string, double> g_scannerVolume;

// tickerId → symbol mapping (for routing tick data to windows)
static std::unordered_map<int, std::string> g_tickerSymbols;

static int    g_nextOrderId          = 1;
static std::unordered_map<int, core::PendingBracketStop> g_pendingBracketStops;
static double g_reconnectNextAttempt = 0.0;   // glfwGetTime() of next auto-reconnect try
static constexpr double kReconnectIntervalSec = 5.0;

// ---- Settings ---------------------------------------------------------------
enum class FontSize { Small = 0, Medium = 1, Large = 2 };
static FontSize g_fontSize     = FontSize::Medium;
static bool     g_settingsOpen = false;

// Default trading style applied to every newly-spawned ChartWindow. Restored
// from app-prefs.cfg at startup; user changes it via Settings → "Default
// trading style for new charts". Existing charts keep their own style
// (persisted via chart-modes.cfg) — this only affects fresh spawns.
static core::services::TradingStyle g_defaultTradingStyle =
    core::services::TradingStyle::Swing;

static constexpr float kFontScales[] = { 0.85f, 1.0f, 1.5f }; // Small / Medium / Large
static ImGuiStyle      g_baseStyle;   // saved after initial style setup; used to re-scale cleanly

// ---- Window groups (10 slots; index 0 = group id 1) ------------------------
static std::array<core::GroupState, core::kNumGroups> g_groups;
// Guard against re-entrant group broadcasts when SetSymbol() re-fires callbacks.
static bool g_groupSyncInProgress = false;
static bool g_replayCursorSyncInProgress = false;

// Live orders for chart overlay (orderId → Order; refreshed on every status change)
static std::unordered_map<int, core::Order> g_liveOrders;

// Per-symbol positions and commissions for the chart P&L strip
static std::unordered_map<std::string, core::Position> g_positions;
static std::unordered_map<std::string, double>          g_symbolCommissions;

// Smart components cache: bboExchange code → routing destinations
// Populated by onSmartComponents; shared across all TradingWindow instances.
static std::unordered_map<std::string, std::vector<core::SmartRoute>> g_smartComponents;

// symbol → conId mapping populated from contractDetails callbacks.
// Used by the IB display-group outbound sync to build contractInfo strings.
static std::unordered_map<std::string, long> g_symbolConIds;

// IB TWS Display Group sync toggle and subscription state.
static bool g_twsGroupSync = false;   // user-controlled in Settings panel

// Multi-account state
static std::vector<std::string>         g_managedAccounts;     // all accounts from managedAccounts()
static std::string                      g_selectedAccount;     // currently active account
static bool                             g_pendingReconnect = false; // deferred reconnect flag

// News providers entitled to this account, populated once after FinishConnect
// via reqNewsProviders → onNewsProviders. Used as the colon-joined provider
// argument to reqHistoricalNews so we don't ask IB for unsubscribed providers
// (which fires error 321 / 502 "Not subscribed for 'BRFUPDN:...' provider").
// Empty = either not yet received or this account has no news entitlements;
// in that state historical-news requests are suppressed.
static std::vector<std::pair<std::string, std::string>> g_newsProvidersList;
static std::string                      g_entitledNewsProviders;

// User-disabled provider codes. The Settings panel writes this set; the
// rebuilt g_entitledNewsProviders excludes everything in here. Persisted to
// ~/.config/ibkr-trading-app/news-providers.cfg (one disabled code per line).
static std::unordered_set<std::string>  g_disabledNewsProviders;

// Real-time P&L subscription state
static std::string                      g_accountId;           // captured from first updateAccountValue
static bool                             g_pnlSubscribed = false;
static std::unordered_map<long, int>    g_pnlSingleConIds;     // conId → reqId
static int                              g_pnlSingleNextReqId = 9001;
static std::unordered_map<int, std::string> g_pnlReqIdToSymbol; // reqId → symbol

static bool IsTerminalOrderStatus(core::OrderStatus s) {
    return s == core::OrderStatus::Filled   ||
           s == core::OrderStatus::Cancelled ||
           s == core::OrderStatus::Rejected;
}

// Push pending order lines for a specific chart window (matched by symbol)
static void UpdateChartPendingOrders(ui::ChartWindow* win) {
    if (!win) return;
    const std::string sym = win->getSymbol();
    std::vector<ui::ChartWindow::PendingOrderLine> lines;
    for (const auto& [id, o] : g_liveOrders) {
        if (o.symbol != sym) continue;
        if (IsTerminalOrderStatus(o.status)) continue;
        ui::ChartWindow::PendingOrderLine ln;
        ln.orderId    = id;
        ln.isBuy      = (o.side == core::OrderSide::Buy);
        ln.qty        = o.quantity;
        ln.holdReason = o.holdReason;
        if (o.type == core::OrderType::StopLimit) {
            ln.price     = o.stopPrice;
            ln.auxPrice  = o.limitPrice;
            ln.orderType = "STP LMT";
        } else if (o.type == core::OrderType::LIT) {
            ln.price     = o.auxPrice;    // trigger price — drawn as main leg
            ln.auxPrice  = o.limitPrice;  // limit price — drawn as aux leg
            ln.orderType = "LIT";
        } else if (o.type == core::OrderType::Stop) {
            ln.price     = o.stopPrice;
            ln.orderType = "STP";
        } else if (o.type == core::OrderType::Limit) {
            ln.price     = o.limitPrice;
            ln.orderType = "LMT";
        } else if (o.type == core::OrderType::LOC) {
            ln.price     = o.limitPrice;
            ln.orderType = "LOC";
        } else if (o.type == core::OrderType::MIT) {
            ln.price     = o.auxPrice;
            ln.orderType = "MIT";
        } else {
            continue;
        }
        if (ln.price <= 0.0) continue;
        lines.push_back(ln);
    }
    win->SetPendingOrders(lines);
}

static void UpdateAllChartPendingOrders() {
    for (auto& e : g_chartEntries) UpdateChartPendingOrders(e.win);
}

// Fire OrderWorking or OrderHeld toast the first time a locally-placed order
// is observed accepted (status Working or PartialFill). Idempotent: removes
// the id from g_pendingLocalAccept so subsequent observations stay quiet.
// Safe to call from both onOpenOrder and onOrderStatusChanged — whichever
// arrives first wins, the other becomes a no-op.
static void MaybeNotifyOrderAccepted(int orderId) {
    if (!g_NotificationService) return;
    auto pit = g_pendingLocalAccept.find(orderId);
    if (pit == g_pendingLocalAccept.end()) return;
    auto it = g_liveOrders.find(orderId);
    if (it == g_liveOrders.end()) return;
    const core::OrderStatus s = it->second.status;
    if (s != core::OrderStatus::Working && s != core::OrderStatus::PartialFill)
        return;
    const auto& o = it->second;
    const bool isHeld = !o.holdReason.empty();
    char body[260];
    if (isHeld) {
        std::snprintf(body, sizeof(body), "%s %s — %s",
                      o.symbol.c_str(), core::OrderTypeStr(o.type),
                      o.holdReason.c_str());
    } else {
        std::snprintf(body, sizeof(body), "%s %s %g @ live",
                      o.symbol.c_str(), core::OrderTypeStr(o.type), o.quantity);
    }
    g_NotificationService->Notify(
        isHeld ? core::services::NotificationSeverity::Warning
               : core::services::NotificationSeverity::Info,
        core::services::NotificationCategory::Orders,
        isHeld ? core::services::NotificationEvent::OrderHeld
               : core::services::NotificationEvent::OrderWorking,
        isHeld ? "Order held" : "Order working", body);
    g_pendingLocalAccept.erase(pit);
}

// Push position info for a specific chart window (matched by symbol)
static void UpdateChartPosition(ui::ChartWindow* win) {
    if (!win) return;
    const std::string sym = win->getSymbol();
    ui::ChartWindow::PositionInfo info;
    auto it = g_positions.find(sym);
    if (it != g_positions.end() && std::abs(it->second.quantity) > 1e-9) {
        info.hasPosition = true;
        info.qty         = it->second.quantity;
        info.avgCost     = it->second.avgCost;
        info.lastPrice   = it->second.marketPrice;
        info.unrealPnL   = it->second.unrealizedPnL;
        info.dailyPnL    = it->second.dailyPnL;
        auto cit = g_symbolCommissions.find(sym);
        info.commission  = (cit != g_symbolCommissions.end()) ? cit->second : 0.0;
    }
    win->SetPosition(info);
}

static void UpdateAllChartPositions() {
    for (auto& e : g_chartEntries) UpdateChartPosition(e.win);
}

// External linkage: ChartWindow.cpp forward-declares this and calls it from
// ComputeSetupPlan() to size the suggested entry. Returns 0.0 before the first
// accountSummary() callback fires (or when no portfolio window is alive); the
// share-count helper treats 0 as "size unknown" and skips the share field.
double GetSelectedAccountEquity() {
    if (!g_PortfolioWindow) return 0.0;
    return g_PortfolioWindow->netLiquidation();
}

// ----------------------------------------------------------------------------
// Unguarded-position guard — Task C
//
// An "unguarded position" is one with non-zero quantity that has NO active
// protective Stop / StopLimit / Trail / TrailLimit order on the same symbol
// covering the position quantity. When any such position is detected the
// matching ChartWindow + TradingWindow show a yellow warning strip with a
// one-click "Place stop" button.
//
// `g_unguarded` is recomputed on every position/order callback (cheap O(N×M)
// walk, no IB calls). The actual stop *price* is filled in once per frame by
// PushUnguardedHintsToWindows() because it depends on the chart instance's
// auto-detected S/R levels.
// ----------------------------------------------------------------------------
struct UnguardedPosition {
    std::string symbol;
    long        conId   = 0;
    double      qty     = 0.0;       // signed
    double      avgCost = 0.0;
};
static std::vector<UnguardedPosition> g_unguarded;

// Push the current unguarded list to all chart/trading windows, computing each
// chart's protective-stop suggestion from its own auto-detected S/R. Called
// once per frame from RenderTradingUI() right before window Render() calls.
//
// For each ChartWindow / TradingWindow, exactly one matching unguarded symbol
// produces an `active=true` hint; everything else gets `active=false` so
// stale strips clear automatically. If a chart instance hasn't run
// auto-analysis yet (no S/R, no ATR) we still send `active=true` with stop=0,
// but the strip self-suppresses on `stopTrig <= 0`.
static void PushUnguardedHintsToWindows();

// Symbol-set of unguarded positions from the previous Recompute, so we can
// fire a "position unprotected" notification on transition into the unguarded
// set (and stay quiet for held conditions).
static std::vector<std::string> g_lastUnguardedSymbols;

static void RecomputeUnguardedPositions() {
    g_unguarded.clear();

    auto isProtectiveType = [](core::OrderType t) {
        return t == core::OrderType::Stop       ||
               t == core::OrderType::StopLimit  ||
               t == core::OrderType::Trail      ||
               t == core::OrderType::TrailLimit;
    };
    auto isLiveStatus = [](core::OrderStatus s) {
        return s == core::OrderStatus::Pending       ||
               s == core::OrderStatus::Working       ||
               s == core::OrderStatus::PartialFill   ||
               s == core::OrderStatus::PendingCancel;
    };

    for (const auto& [sym, pos] : g_positions) {
        if (std::abs(pos.quantity) < 1e-9) continue;          // flat
        bool isLong   = (pos.quantity > 0.0);
        core::OrderSide needSide = isLong ? core::OrderSide::Sell
                                          : core::OrderSide::Buy;
        bool hasStop = false;
        for (const auto& [oid, ord] : g_liveOrders) {
            if (ord.symbol != sym)            continue;
            if (ord.side   != needSide)       continue;
            if (!isLiveStatus(ord.status))    continue;
            if (!isProtectiveType(ord.type))  continue;
            if (ord.quantity <= 1e-9)         continue;
            hasStop = true;
            break;
        }
        if (!hasStop) {
            g_unguarded.push_back({ sym, pos.conId, pos.quantity, pos.avgCost });
        }
    }

    // Edge-trigger UnguardedPosition notification on (new ∖ last) symbols.
    if (g_NotificationService) {
        for (const auto& u : g_unguarded) {
            bool wasUnguarded = false;
            for (const auto& prev : g_lastUnguardedSymbols)
                if (prev == u.symbol) { wasUnguarded = true; break; }
            if (wasUnguarded) continue;
            char body[160];
            std::snprintf(body, sizeof(body), "%s %g sh @ $%.2f — no protective stop",
                          u.symbol.c_str(), u.qty, u.avgCost);
            g_NotificationService->Notify(
                core::services::NotificationSeverity::Warning,
                core::services::NotificationCategory::Signals,
                core::services::NotificationEvent::UnguardedPosition,
                "Unguarded position", body);
        }
    }
    g_lastUnguardedSymbols.clear();
    g_lastUnguardedSymbols.reserve(g_unguarded.size());
    for (const auto& u : g_unguarded) g_lastUnguardedSymbols.push_back(u.symbol);
}

static void PushUnguardedHintsToWindows() {
    // Read the global setup-suggestion knobs straight from any live ChartWindow
    // — they're per-instance settings, but for the protective-stop sizing all
    // we need is the (atrPad, roundPad, stopOffset) tuple. The plan defaults
    // are the right starting point for v1; users tune per-chart and the chart
    // picks its own levels anyway.
    constexpr double kAtrPad     = 0.5;
    constexpr double kRoundPad   = 0.07;
    constexpr double kStopOffset = 0.10;

    auto buildHintFromChart = [&](ui::ChartWindow* ch, const std::string& sym,
                                  double qty, double avgCost) -> ui::ChartWindow::UnguardedHint {
        ui::ChartWindow::UnguardedHint h;
        h.symbol  = sym;
        h.qty     = qty;
        h.avgCost = avgCost;

        if (!ch) return h;
        auto snap = ch->getAutoLevels();
        if (snap.atrLast <= 0.0) return h;

        bool isLong = qty > 0.0;
        const auto& levels = isLong ? snap.supports : snap.resistances;
        auto stop = core::services::SuggestStopForPosition(
            isLong, avgCost, levels, snap.atrLast,
            kAtrPad, kRoundPad, kStopOffset);
        if (!stop.valid) return h;

        h.active   = true;
        h.stopTrig = stop.stop;
        h.stopLmt  = stop.stopLmt;
        h.pctRisk  = stop.pctRisk;
        return h;
    };

    // Map: symbol -> first ChartWindow with that symbol (used to source S/R for
    // TradingWindows on the same symbol).
    std::unordered_map<std::string, ui::ChartWindow*> symToChart;
    for (auto& ce : g_chartEntries) {
        if (!ce.win) continue;
        const std::string s = ce.win->getSymbol();
        if (!s.empty() && symToChart.find(s) == symToChart.end())
            symToChart[s] = ce.win;
    }

    auto findUnguarded = [&](const std::string& sym) -> const UnguardedPosition* {
        for (const auto& u : g_unguarded)
            if (u.symbol == sym) return &u;
        return nullptr;
    };

    // Push to each chart window — uses its own getAutoLevels().
    for (auto& ce : g_chartEntries) {
        if (!ce.win) continue;
        const std::string sym = ce.win->getSymbol();
        const UnguardedPosition* up = findUnguarded(sym);
        if (!up) {
            ui::ChartWindow::UnguardedHint cleared;
            cleared.symbol = sym;
            ce.win->SetUnguardedSuggestion(cleared);
            continue;
        }
        ce.win->SetUnguardedSuggestion(
            buildHintFromChart(ce.win, sym, up->qty, up->avgCost));
    }

    // Push to each trading window — borrow S/R from the matching chart, if any.
    for (auto& te : g_tradingEntries) {
        if (!te.win) continue;
        const std::string sym = te.win->getSymbol();
        const UnguardedPosition* up = findUnguarded(sym);
        if (!up) {
            ui::TradingWindow::UnguardedHint cleared;
            cleared.symbol = sym;
            te.win->SetUnguardedSuggestion(cleared);
            continue;
        }
        ui::ChartWindow* sourceChart = nullptr;
        auto it = symToChart.find(sym);
        if (it != symToChart.end()) sourceChart = it->second;
        ui::ChartWindow::UnguardedHint ch = buildHintFromChart(
            sourceChart, sym, up->qty, up->avgCost);
        // Plan §3f: if no chart S/R is available, suppress the warning (v1).
        ui::TradingWindow::UnguardedHint th;
        th.symbol   = ch.symbol;
        th.active   = ch.active;
        th.qty      = ch.qty;
        th.avgCost  = ch.avgCost;
        th.stopTrig = ch.stopTrig;
        th.stopLmt  = ch.stopLmt;
        th.pctRisk  = ch.pctRisk;
        te.win->SetUnguardedSuggestion(th);
    }

    // Push S/R levels to each trading window from the matching chart (if any).
    // Uses the same symToChart map built above. When no chart is open for the
    // symbol, clears the levels so stale markers from a previous symbol don't
    // linger.
    for (auto& te : g_tradingEntries) {
        if (!te.win) continue;
        const std::string sym = te.win->getSymbol();
        if (sym.empty()) continue;
        auto it = symToChart.find(sym);
        if (it != symToChart.end() && it->second) {
            auto snap = it->second->getAutoLevels();
            std::vector<double> sPrices, rPrices;
            sPrices.reserve(snap.supports.size());
            rPrices.reserve(snap.resistances.size());
            for (const auto& L : snap.supports)    sPrices.push_back(L.price);
            for (const auto& L : snap.resistances) rPrices.push_back(L.price);
            te.win->SetAutoLevels(sPrices, rPrices, snap.atrLast);
        } else {
            te.win->SetAutoLevels({}, {}, 0.0);
        }
    }
}

// ----------------------------------------------------------------------------
// Trading-style mode-switch queue — Phase 13
//
// Switching a chart's TradingStyle re-issues its historical-data subscription
// with the new (timeframe + duration). When many charts switch at once
// (e.g. user opens 10 charts and bulk-picks a mode), we throttle to one
// IB request per second to stay well under IB's
// 60-requests-per-10-min-per-contract pacing limit.
//
// Latest-wins: enqueueing a switch for a chart drops any prior pending entry
// for the same chartIdx, so spamming the combo settles on the last pick
// rather than hopping through every intermediate mode.
//
// The symbol + useRTH are read at drain time (not enqueue time) so a symbol
// change between enqueue and drain doesn't strand the wrong contract.
// ----------------------------------------------------------------------------
struct PendingStyleSwitch {
    int                          chartIdx;
    core::services::TradingStyle style;
    std::string                  historyDuration;
    bool                         useRTH;
};
static std::deque<PendingStyleSwitch> g_pendingStyleSwitches;
static double                         g_nextStyleSwitchAllowed = 0.0;
static constexpr double               kStyleSwitchThrottleSec  = 1.0;

// Persistence flag — set by OnStyleChange (declared below in SpawnChartWindow),
// flushed once per second from RenderTradingUI(). Defined here so the lambda
// can capture it without forward-declaration tricks.
static bool   g_chartModesDirty      = false;
static bool   g_replayWindowsDirty    = false;
static double g_lastChartModesSave    = 0.0;
static double g_lastReplayWindowsSave = 0.0;

static int g_newsItemId    = 10000;  // unique IDs for real-time market news items
static int g_histNewsId    = 20000;  // unique IDs for historical news items
static std::vector<std::string> g_portfolioSymbols;  // known held symbols

// Request IDs (reserved ranges — no overlaps)
// Per-instance helpers: each window type has its own slot.
//   Chart   hist:  1,3,5,...,19  ext: 2,4,6,...,20  mkt: 100-109
//   Trading mkt:   110-119       depth: 120-129
//   Futures /ES,/NQ (front): 140-141  · /ES,/NQ (Dec): 142-143  (market health)
//   Scanner scan:  1000,1100,...,1900 (+99 ea)  mkt: 800,812,...,908 (+12 ea, 12 slots each)
//   News:          201(RT), 400-420(conId), 500-520(hist), 600-699(art), 700-759(mkt)
//   Account:       900
static constexpr int NEWS_RT_REQID       = 201;  // real-time news subscription (mdoff;292)

// Inline helpers — idx is 0-based instance index
inline int ChartHistId   (int idx) { return 1    + idx * 2; }   // 1,3,5,...,19
inline int ChartExtId    (int idx) { return 2    + idx * 2; }   // 2,4,6,...,20
inline int ChartMktId    (int idx) { return 100  + idx; }       // 100-109 (initial slot only)

// Rotating pool for chart market-data reqIds. On every symbol change we
// allocate a fresh id from this pool so stale ticks from the just-cancelled
// subscription (IB keeps streaming for a few ms after CancelMarketData)
// arrive with a reqId no chart owns and are silently dropped at the
// dispatcher — instead of landing on the new symbol's bar series and
// corrupting OHLC (e.g. a stale TSLA 398.55 high leaking onto a $268 stock).
inline int AllocChartMktId() {
    static int s_next = 10000;
    int id = s_next++;
    if (s_next > 10999) s_next = 10000;
    return id;
}
// Rotating pools for chart historical-data reqIds, same rationale as
// AllocChartMktId: cancel→reissue on the same id leaks stale bars from the
// previous symbol/timeframe into the new pendingBars (e.g. AAPL bars at $200
// merging with /ES bars at $5500), which makes InitViewRange fit a Y-range
// spanning both and renders every candle as a 1-pixel sliver.
inline int AllocChartHistId() {
    static int s_next = 12000;
    int id = s_next++;
    if (s_next > 12999) s_next = 12000;
    return id;
}
inline int AllocChartExtId() {
    static int s_next = 13000;
    int id = s_next++;
    if (s_next > 13999) s_next = 13000;
    return id;
}
// Rotating pools for trading-window reqIds — same rationale as
// AllocChartMktId. ApplyTradingSymbol cancels and immediately re-issues on the
// same depthId/mktId/tickId, but IB keeps streaming for a few ms after cancel.
// Without rotation those stale messages land on the new symbol's L2 buckets,
// producing a crossed/intercalated book (e.g. PSX showing bid=8.45/ask=8.08
// when stale data from a previous $8 ticker leaks through).
inline int AllocTradingMktId() {
    static int s_next = 14000;
    int id = s_next++;
    if (s_next > 14999) s_next = 14000;
    return id;
}
inline int AllocTradingDepthId() {
    static int s_next = 15000;
    int id = s_next++;
    if (s_next > 15999) s_next = 15000;
    return id;
}
inline int AllocTradingTickId() {
    static int s_next = 16000;
    int id = s_next++;
    if (s_next > 16999) s_next = 16000;
    return id;
}
inline int TradingMktId  (int idx) { return 110  + idx; }       // 110-119 (initial slot)
inline int TradingDepthId(int idx) { return 120  + idx; }       // 120-129 (initial slot)
inline int TradingTickId (int idx) { return 130  + idx; }       // 130-139 (initial slot)
inline int ChartWshId    (int idx) { return 8020 + idx; }       // 8020-8029
inline int ScannerBase   (int idx) { return 1000 + idx * 100; } // 1000,1100,...,1900
inline int ScannerMktBase(int idx) { return 800  + idx * 12; }  // 800,812,...,908

static constexpr int ACCT_SUMMARY_REQID  = 900;
static constexpr const char* ACCT_SUMMARY_TAGS =
    "Currency,NetLiquidation,TotalCashValue,BuyingPower,"
    "UnrealizedPnL,RealizedPnL,InitMarginReq,MaintMarginReq,ExcessLiquidity";

// News reqId layout (per-instance, idx 0-9):
//   Stock conId:   2000+idx
//   Port conId:    2010+idx*20  (+19 per entry, 20 portfolio symbols)
//   Stock hist:    2210+idx
//   Port hist:     2220+idx*20  (+19 per entry)
//   Article base:  2500+idx*100 (+99 per entry, rolling)
//   Mkt hist:      3500+idx*10  (+9 per entry, kMktSeedCount seed symbols)
//   Mkt conId:     3600+idx*10  (+9 per entry)
inline int NewsStockConId(int idx) { return 2000 + idx; }
inline int NewsPortConId (int idx) { return 2010 + idx * 20; }
inline int NewsHistStock (int idx) { return 2210 + idx; }
inline int NewsHistPort  (int idx) { return 2220 + idx * 20; }
inline int NewsArtBase   (int idx) { return 2500 + idx * 100; }
inline int NewsArtEnd    (int idx) { return 2599 + idx * 100; }
inline int NewsHistMkt   (int idx) { return 3500 + idx * 10; }
inline int NewsConIdMkt  (int idx) { return 3600 + idx * 10; }

// Symbols fetched on connection to seed the Market-tab news feed.
static const char* kMktSeedSymbols[] = { "AAPL", "SPY", "MSFT", "TSLA", "NVDA" };
static constexpr int kMktSeedCount   = 5;

// Watchlist reqId layout (per-instance, idx 0-9):
//   contract details: 6900+idx   (transient, one in flight per instance)
//   market data:      7000+idx*100  (+99 per instance, up to 100 symbols each)
inline int WatchlistCdId (int idx) { return 6900 + idx; }
inline int WatchlistMktBase(int idx) { return 7000 + idx * 100; }

// ============================================================================
// Connection / Login state
// ============================================================================
enum class ConnectionState { Disconnected, Connecting, Connected,
                             SelectingAccount, LostConnection, Error };
enum class ApiType          { TWS = 0, Gateway };

struct LoginState {
    char    host[128]  = "127.0.0.1";
    int     port       = 7497;
    int     clientId   = 1;
    bool    isLive     = false;
    ApiType apiType    = ApiType::TWS;

    ConnectionState state    = ConnectionState::Disconnected;
    std::string     errorMsg;
    std::string     connectedAs;

    void UpdatePort() {
        if (apiType == ApiType::TWS)
            port = isLive ? 7496 : 7497;
        else
            port = isLive ? 4001 : 4002;
    }
};

static LoginState  g_Login;
static GLFWwindow* g_AppWindow = nullptr;

// Returns the generic tick list appropriate for the current account type.
// Paper/delayed (type 4): gateway rejects ALL generic ticks → use "".
// Live (type 1): use "165" for 52-week hi/lo (fields 79/80 from Misc Stats).
static const char* MktDataTicks() {
    return g_Login.isLive ? "165" : "";
}

// ============================================================================
// Vulkan globals
// ============================================================================
static VkAllocationCallbacks*   g_Allocator      = nullptr;
static VkInstance               g_Instance       = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device         = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily    = (uint32_t)-1;
static VkQueue                  g_Queue          = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache  = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t                 g_MinImageCount    = 2;
static bool                     g_SwapChainRebuild = false;

// ============================================================================
// Helpers
// ============================================================================
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}
static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0) abort();
}
static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& props, const char* ext) {
    for (const VkExtensionProperties& p : props)
        if (strcmp(p.extensionName, ext) == 0) return true;
    return false;
}

// ============================================================================
// Vulkan setup / teardown
// ============================================================================
static void SetupVulkan(ImVector<const char*> instance_extensions) {
    VkResult err;
    {
        VkInstanceCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t cnt = 0;
        ImVector<VkExtensionProperties> props;
        vkEnumerateInstanceExtensionProperties(nullptr, &cnt, nullptr);
        props.resize((int)cnt);
        vkEnumerateInstanceExtensionProperties(nullptr, &cnt, props.Data);

        if (IsExtensionAvailable(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(props, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
        ci.enabledExtensionCount   = (uint32_t)instance_extensions.Size;
        ci.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&ci, g_Allocator, &g_Instance);
        check_vk_result(err);
    }

    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    {
        ImVector<const char*> dev_exts;
        dev_exts.push_back("VK_KHR_swapchain");
        uint32_t cnt2 = 0;
        ImVector<VkExtensionProperties> props2;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &cnt2, nullptr);
        props2.resize((int)cnt2);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &cnt2, props2.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(props2, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            dev_exts.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
        const float prio[] = {1.0f};
        VkDeviceQueueCreateInfo qi[1] = {};
        qi[0].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi[0].queueFamilyIndex = g_QueueFamily;
        qi[0].queueCount       = 1;
        qi[0].pQueuePriorities = prio;
        VkDeviceCreateInfo dci = {};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = IM_ARRAYSIZE(qi);
        dci.pQueueCreateInfos       = qi;
        dci.enabledExtensionCount   = (uint32_t)dev_exts.Size;
        dci.ppEnabledExtensionNames = dev_exts.Data;
        err = vkCreateDevice(g_PhysicalDevice, &dci, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
    }

    {
        VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
        };
        VkDescriptorPoolCreateInfo pi = {};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets       = 0;
        for (auto& ps : pool_sizes) pi.maxSets += ps.descriptorCount;
        pi.poolSizeCount = IM_ARRAYSIZE(pool_sizes);
        pi.pPoolSizes    = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pi, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int w, int h) {
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, surface, &res);
    if (res != VK_TRUE) { fprintf(stderr, "No WSI support\n"); exit(-1); }

    const VkFormat fmts[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,   VK_FORMAT_R8G8B8_UNORM,
    };
    wd->Surface       = surface;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, surface, fmts, IM_ARRAYSIZE(fmts),
        VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    VkPresentModeKHR pm[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, surface, pm, IM_ARRAYSIZE(pm));
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily,
        g_Allocator, w, h, g_MinImageCount, 0);
}

static void CleanupVulkan() {
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}
static void CleanupVulkanWindow(ImGui_ImplVulkanH_Window* wd) {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, wd, g_Allocator);
    vkDestroySurfaceKHR(g_Instance, wd->Surface, g_Allocator);
    wd->Surface = VK_NULL_HANDLE;
}

// ============================================================================
// Frame rendering
// ============================================================================
static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore img_sem  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore rend_sem = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;

    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX,
                                          img_sem, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (err != VK_SUBOPTIMAL_KHR) check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX); check_vk_result(err);
    err = vkResetFences(g_Device, 1, &fd->Fence);                         check_vk_result(err);
    err = vkResetCommandPool(g_Device, fd->CommandPool, 0);                check_vk_result(err);

    VkCommandBufferBeginInfo bi = {};
    bi.sType  = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(fd->CommandBuffer, &bi); check_vk_result(err);

    VkRenderPassBeginInfo ri = {};
    ri.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    ri.renderPass               = wd->RenderPass;
    ri.framebuffer              = fd->Framebuffer;
    ri.renderArea.extent.width  = (uint32_t)wd->Width;
    ri.renderArea.extent.height = (uint32_t)wd->Height;
    ri.clearValueCount          = 1;
    ri.pClearValues             = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &ri, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &img_sem;
    si.pWaitDstStageMask    = &wait_stage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &fd->CommandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &rend_sem;
    err = vkEndCommandBuffer(fd->CommandBuffer);   check_vk_result(err);
    err = vkQueueSubmit(g_Queue, 1, &si, fd->Fence); check_vk_result(err);
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild) return;
    VkSemaphore rend_sem = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR pi = {};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &rend_sem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &wd->Swapchain;
    pi.pImageIndices      = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &pi);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (err != VK_SUBOPTIMAL_KHR) check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// ============================================================================
// Group sync helper — must be defined before CreateTradingWindows lambdas
// ============================================================================

// Update a trading entry's displayed symbol AND re-subscribe IB market data + depth.
// Used both from BroadcastGroupSymbol and from OnSymbolChanged so the logic is in one place.
static void ApplyTradingSymbol(TradingEntry& te, const std::string& sym) {
    te.bboExchange.clear();
    if (te.win) {
        te.win->SetSymbol(sym, 0.0);
        te.win->SetExchangeList({"SMART"});  // reset until tickReqParams arrives
        // Re-inject position from portfolio cache for the new symbol
        auto it = g_positions.find(sym);
        if (it != g_positions.end())
            te.win->SetPosition(it->second.quantity, it->second.avgCost);
    }
    if (!g_IBClient) return;
    // Cancel + rotate to fresh reqIds so stale depth/tick messages from the
    // previous subscription (IB streams for a few ms after cancel) land on
    // ids no entry owns and are silently dropped at the dispatcher.
    g_IBClient->CancelMarketData(te.mktId);
    g_IBClient->CancelMktDepth(te.depthId, te.win ? te.win->useL2() : false);
    g_IBClient->CancelTickByTickData(te.tickId);
    g_tickerSymbols.erase(te.mktId);
    te.mktId   = AllocTradingMktId();
    te.depthId = AllocTradingDepthId();
    te.tickId  = AllocTradingTickId();
    g_tickerSymbols[te.mktId] = sym;
    g_IBClient->ReqMarketData(te.mktId, sym, MktDataTicks());
    g_IBClient->ReqMktDepth(te.depthId, sym, te.win ? te.win->numDepthRows() : 20,
                            te.win ? te.win->useL2() : false);
    g_IBClient->ReqTickByTickData(te.tickId, sym);
}

// Propagate a symbol change to all windows in the same group.
// Re-entrant guard prevents loops when SetSymbol() re-fires callbacks.
static void BroadcastGroupSymbol(int groupId, const std::string& sym) {
    if (groupId <= 0 || groupId > core::kNumGroups || g_groupSyncInProgress) return;
    core::GroupState& gs = g_groups[groupId - 1];
    if (gs.symbol == sym) return;   // already broadcast this symbol
    g_groupSyncInProgress = true;
    gs.id     = groupId;
    gs.symbol = sym;
    for (auto& e : g_chartEntries)
        if (e.win && e.win->groupId() == groupId) e.win->SetSymbol(sym);
    for (auto& te : g_tradingEntries)
        if (te.win && te.win->groupId() == groupId) ApplyTradingSymbol(te, sym);
    for (auto& ne : g_newsEntries)
        if (ne.win && ne.win->groupId() == groupId) ne.win->SetSymbol(sym);
    // ScannerWindow is a symbol source only — no inbound SetSymbol

    // Outbound IB display-group sync: push new symbol into the matching TWS group.
    if (g_twsGroupSync && g_IBClient && groupId >= 1 && groupId <= 4) {
        long conId = 0;
        auto cit = g_symbolConIds.find(sym);
        if (cit != g_symbolConIds.end()) conId = cit->second;
        if (conId > 0) {
            std::string ci = std::to_string(conId) + "@SMART";
            g_IBClient->UpdateDisplayGroup(8060 + groupId, ci);  // 8061–8064
        }
    }

    g_groupSyncInProgress = false;
}

// Throttled cursor sync across replay windows in the same group (§6.3).
// Called from OnCursorMove at most once per 100ms per group.
static void BroadcastReplayCursor(int groupId, std::time_t cursorTime, int sourceIdx) {
    if (groupId <= 0 || g_replayCursorSyncInProgress) return;
    g_replayCursorSyncInProgress = true;
    for (int i = 0; i < (int)g_replayEntries.size(); ++i) {
        if (i == sourceIdx) continue;
        auto& re = g_replayEntries[i];
        if (re.win && re.win->groupId() == groupId)
            re.win->SeekToTime(cursorTime);
    }
    g_replayCursorSyncInProgress = false;
}

// Throttle: at most once per 100ms per group to avoid CPU spike at MAX speed.
static std::unordered_map<int, double> g_lastReplayCursorBroadcast;

// ============================================================================
// Window lifecycle helpers — per-instance spawn functions
// ============================================================================

// Issue / re-issue a historical + market-data subscription for a chart instance.
// `durationOverride` is empty for the default-per-timeframe path; the
// trading-style queue passes the preset's `historyDuration` (e.g. "2 D" / "5 Y")
// so the prefetch covers the right horizon for the chosen mode.
//
// On a symbol change we allocate a fresh `ce.mktId` from `AllocChartMktId()`
// so in-flight ticks from the just-cancelled subscription land with a reqId
// no chart owns and are dropped by the dispatcher rather than corrupting
// the new symbol's OHLC.
static void ReqChartData(ChartEntry& ce, const std::string& sym, core::Timeframe tf,
                         bool useRTH, core::BarSeries& pendingBars,
                         const std::string& durationOverride = "") {
    if (!g_IBClient) return;
    // Rotate histId+extId on every cancel→reissue cycle so stale bars from the
    // just-cancelled subscription (IB keeps streaming for a few ms after
    // CancelHistoricalData) arrive with a reqId no chart owns and are silently
    // dropped at the dispatcher — instead of leaking into the new symbol's
    // pendingBars and corrupting the series. Cross-symbol leak (AAPL→/ES) is
    // the visible symptom: stale AAPL bars merge with /ES bars, InitViewRange
    // fits a Y-range from $200 to $5500, every candle becomes a 1-pixel sliver.
    g_IBClient->CancelHistoricalData(ce.histId);
    ce.histStreamActive = false;
    ce.histId = AllocChartHistId();
    g_IBClient->CancelHistoricalData(ce.extId);
    ce.extStreamActive = false;
    ce.extId = AllocChartExtId();
    ce.pendingExtBars = core::BarSeries{};
    pendingBars         = core::BarSeries{};
    pendingBars.symbol  = sym;
    pendingBars.timeframe = tf;
    const char* duration = durationOverride.empty()
                               ? core::TimeframeIBDuration(tf)
                               : durationOverride.c_str();
    g_IBClient->ReqHistoricalData(ce.histId, sym,
                                  duration,
                                  core::TimeframeIBBarSize(tf),
                                  useRTH);
    ce.histStreamActive = true;
    auto it = g_tickerSymbols.find(ce.mktId);
    bool symbolChanged = (it == g_tickerSymbols.end()) || (it->second != sym);
    if (symbolChanged) {
        g_IBClient->CancelMarketData(ce.mktId);
        g_tickerSymbols.erase(ce.mktId);
        ce.mktId = AllocChartMktId();
        g_tickerSymbols[ce.mktId] = sym;
        g_IBClient->ReqMarketData(ce.mktId, sym, MktDataTicks());
        ce.bboExchange.clear();
        if (ce.win) ce.win->SetExchangeList({"SMART"});
    }
}

// Pop one pending style switch per `kStyleSwitchThrottleSec`. Called from
// RenderTradingUI() each frame; cheap empty-list early-out keeps idle cost
// flat. Cancels any in-flight extend-history request on the same chart so
// older bars from the previous timeframe can't land into the new series.
static void DrainStyleSwitchQueue() {
    if (g_pendingStyleSwitches.empty() || !g_IBClient) return;
    double now = glfwGetTime();
    if (now < g_nextStyleSwitchAllowed) return;

    auto p = g_pendingStyleSwitches.front();
    g_pendingStyleSwitches.pop_front();

    if (p.chartIdx < 0 || p.chartIdx >= (int)g_chartEntries.size()) return;
    auto& ce = g_chartEntries[p.chartIdx];
    if (!ce.win) return;

    // Cancel any in-flight extend-history request — older bars from the
    // previous TF could otherwise land into the new series.
    g_IBClient->CancelHistoricalData(ce.extId);
    ce.extStreamActive  = false;
    ce.pendingExtBars   = core::BarSeries{};

    // Read symbol + useRTH at drain time, not enqueue time, so a symbol change
    // between the user's combo click and this frame doesn't strand the wrong
    // contract. For Free mode, also recompute duration from the chart's
    // current TF so a TF change between two enqueues + a drain in between
    // never produces a TF/duration mismatch.
    const std::string sym = ce.win->getSymbol();
    if (sym.empty()) return;

    std::string duration = p.historyDuration;
    if (p.style == core::services::TradingStyle::Free)
        duration = core::TimeframeIBDuration(ce.win->getTimeframe());

    ReqChartData(ce, sym, ce.win->getTimeframe(),
                 p.useRTH, ce.pendingBars, duration);

    g_nextStyleSwitchAllowed = now + kStyleSwitchThrottleSec;
}

static void SpawnChartWindow(int idx) {
    ChartEntry e;
    e.histId = ChartHistId(idx);
    e.extId  = ChartExtId(idx);
    e.mktId  = ChartMktId(idx);
    e.wshId  = ChartWshId(idx);
    e.win    = new ui::ChartWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId((idx % core::kNumGroups) + 1);

    // Adopt the app-wide default trading style (set via Settings; persisted
    // in app-prefs.cfg). silent=true so this doesn't enqueue a redundant IB
    // request — chart-modes.cfg restore will override this for known charts,
    // and fresh-spawn (no chart-modes entry) uses the default as-is.
    if (g_defaultTradingStyle != core::services::TradingStyle::Swing)
        e.win->setTradingStyle(g_defaultTradingStyle, /*silent=*/true);

    // Capture idx (not pointer — vector may reallocate)
    e.win->OnDataRequest = [idx](const std::string& sym, core::Timeframe tf, bool useRTH) {
        auto& ce = g_chartEntries[idx];
        ReqChartData(ce, sym, tf, useRTH, ce.pendingBars);
        UpdateChartPendingOrders(ce.win);
        UpdateChartPosition(ce.win);
        BroadcastGroupSymbol(ce.win->groupId(), sym);
        // WSH: clear stale markers and subscribe for the new symbol
        if (g_IBClient) {
            ce.win->ClearWshEvents();
            ce.wshConIdFired = false;
            g_IBClient->CancelWshEventData(ce.wshId);
            g_IBClient->ReqContractDetails(ce.wshId, sym);
        }
    };

    e.win->OnExtendHistory = [idx](const std::string& sym, core::Timeframe tf,
                                   const std::string& endDT, bool useRTH) {
        auto& ce = g_chartEntries[idx];
        if (!g_IBClient) { ce.win->PrependHistoricalData({}); return; }
        g_IBClient->CancelHistoricalData(ce.extId);
        ce.extStreamActive          = false;
        ce.pendingExtBars           = core::BarSeries{};
        ce.pendingExtBars.symbol    = sym;
        ce.pendingExtBars.timeframe = tf;
        g_IBClient->ReqHistoricalData(ce.extId, sym,
                                      core::TimeframeIBDuration(tf),
                                      core::TimeframeIBBarSize(tf),
                                      useRTH, endDT);
        ce.extStreamActive = true;
    };

    // Trading-style mode switch — push to the throttled queue so we don't
    // exceed IB's pacing limit when many charts switch at once. Drop any
    // previously-queued switch for this chart so spamming the combo settles
    // on the latest pick rather than walking through every intermediate mode.
    e.win->OnStyleChange = [idx](core::services::TradingStyle s,
                                 const std::string& historyDuration,
                                 bool useRTH) {
        for (auto it = g_pendingStyleSwitches.begin();
             it != g_pendingStyleSwitches.end(); ) {
            if (it->chartIdx == idx) it = g_pendingStyleSwitches.erase(it);
            else                     ++it;
        }
        g_pendingStyleSwitches.push_back({ idx, s, historyDuration, useRTH });
        g_chartModesDirty = true;
    };

    e.win->OnSignalChange = [](ui::ChartWindow::BreakoutDirection dir,
                               const std::string& sym, double last, double rr) {
        if (!g_NotificationService) return;
        const bool isLong = (dir == ui::ChartWindow::BreakoutDirection::LongSetup);
        char body[160];
        if (rr > 0.0)
            std::snprintf(body, sizeof(body), "%s @ $%.2f — R:R %.2f",
                          sym.c_str(), last, rr);
        else
            std::snprintf(body, sizeof(body), "%s @ $%.2f", sym.c_str(), last);
        g_NotificationService->Notify(
            core::services::NotificationSeverity::Warning,
            core::services::NotificationCategory::Signals,
            isLong ? core::services::NotificationEvent::LongSetup
                   : core::services::NotificationEvent::ShortSetup,
            isLong ? "Long setup" : "Short setup",
            body);
    };

    e.win->OnOrderSubmit = [](const core::Order& o) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return 0;
        int id = g_nextOrderId++;
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->SetNextOrderId(g_nextOrderId);
        core::Order order   = o;
        order.orderId       = id;
        order.account       = g_selectedAccount;
        order.status        = core::OrderStatus::Pending;
        order.submittedAt   = std::time(nullptr);
        order.updatedAt     = order.submittedAt;
        g_liveOrders[id]    = order;
        g_pendingLocalAccept.insert(id);
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(order);
        UpdateAllChartPendingOrders();
        g_IBClient->PlaceOrder(order);
        return id;
    };

    e.win->OnCancelOrder = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };

    e.win->OnBracketEntry = [](int lmtOrderId,
                                const core::PendingBracketStop& p) {
        g_pendingBracketStops[lmtOrderId] = p;
    };

    e.win->OnModifyOrder = [](int orderId, double newPrice, double newAuxPrice) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        auto it = g_liveOrders.find(orderId);
        if (it == g_liveOrders.end()) return;
        // Modify in place by re-issuing PlaceOrder with the same orderId.
        // IB treats this as an order modification and preserves any OCA
        // pairing — critical for bracket STP/TP legs sharing
        // ocaGroup="BRK_<entryId>". Cancel+re-place would break the pairing
        // (IB may cancel the OCA survivor or refuse to re-pair the new leg).
        core::Order rep = it->second;
        rep.account     = g_selectedAccount;
        rep.updatedAt   = std::time(nullptr);
        if (rep.type == core::OrderType::Limit)     { rep.limitPrice = newPrice;  rep.stopPrice  = 0.0; }
        else if (rep.type == core::OrderType::Stop) { rep.stopPrice  = newPrice;  rep.limitPrice = 0.0; }
        else if (rep.type == core::OrderType::StopLimit) {
            rep.stopPrice = newPrice; rep.limitPrice = newAuxPrice;
        }
        it->second = rep;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(rep);
        UpdateAllChartPendingOrders();
        // Re-send the full order including ocaGroup / ocaType. IB error
        // 10327 ("OCA group type revision is not allowed") fires when the
        // resent values *differ* from the server-side group membership,
        // not from re-sending matching values. As long as
        // IBKRClient::openOrder propagates ocaGroup / ocaType into
        // g_liveOrders correctly, the resend matches and IB accepts the
        // price update without breaking the OCA pairing.
        g_IBClient->PlaceOrder(rep);
    };

    g_chartEntries.push_back(std::move(e));
}

static void SpawnTradingWindow(int idx) {
    TradingEntry e;
    e.depthId = TradingDepthId(idx);
    e.mktId   = TradingMktId(idx);
    e.tickId  = TradingTickId(idx);
    e.win     = new ui::TradingWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId((idx % core::kNumGroups) + 1);
    e.win->SetNextOrderId(g_nextOrderId);

    e.win->OnOrderSubmit = [](const core::Order& o) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        core::Order order = o;
        order.account     = g_selectedAccount;
        g_pendingLocalAccept.insert(order.orderId);
        g_IBClient->PlaceOrder(order);
        if (order.orderId >= g_nextOrderId) g_nextOrderId = order.orderId + 1;
    };

    e.win->OnOrderCancel = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };

    e.win->OnSymbolChanged = [idx](const std::string& sym) {
        auto& te = g_tradingEntries[idx];
        ApplyTradingSymbol(te, sym);
        BroadcastGroupSymbol(te.win->groupId(), sym);
    };

    e.win->OnDepthModeChanged = [idx](bool useL2) {
        auto& te = g_tradingEntries[idx];
        if (!g_IBClient || !te.win) return;
        std::string sym = te.win->getSymbol();
        if (sym.empty()) return;
        g_IBClient->CancelMktDepth(te.depthId, !useL2);  // cancel the old mode
        te.depthId = AllocTradingDepthId();              // rotate to drop stale L1/L2 ticks
        g_IBClient->ReqMktDepth(te.depthId, sym, te.win->numDepthRows(), useL2);
    };

    e.win->OnDepthRowsChanged = [idx]() {
        auto& te = g_tradingEntries[idx];
        if (!g_IBClient || !te.win) return;
        std::string sym = te.win->getSymbol();
        if (sym.empty()) return;
        bool useL2 = te.win->useL2();
        g_IBClient->CancelMktDepth(te.depthId, useL2);   // mode unchanged
        te.depthId = AllocTradingDepthId();              // rotate to drop stale ticks at old row count
        g_IBClient->ReqMktDepth(te.depthId, sym, te.win->numDepthRows(), useL2);
    };

    g_tradingEntries.push_back(std::move(e));
}

static void SpawnScannerWindow(int idx) {
    ScannerEntry e;
    e.scanBase     = ScannerBase(idx);
    e.activeScanId = e.scanBase - 1;   // first increment lands on scanBase
    e.mktBase      = ScannerMktBase(idx);
    e.win          = new ui::ScannerWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId((idx % core::kNumGroups) + 1);

    e.win->OnSymbolSelected = [idx](const std::string& sym) {
        // Propagate to all charts/trading in the same group, or just broadcast
        BroadcastGroupSymbol(g_scannerEntries[idx].win->groupId(), sym);
        // Also update any chart windows not in a group (default behavior)
        for (auto& ce : g_chartEntries)
            if (ce.win && ce.win->groupId() == 0) {
                ReqChartData(ce, sym, core::Timeframe::D1, true, ce.pendingBars);
                ce.win->SetSymbol(sym);
                UpdateChartPendingOrders(ce.win);
                UpdateChartPosition(ce.win);
            }
        for (auto& te : g_tradingEntries)
            if (te.win && te.win->groupId() == 0)
                ApplyTradingSymbol(te, sym);  // rotates ids; drops stale ticks
    };

    e.win->OnScanRequest = [idx](const std::string& scanCode,
                                  const std::string& instrument,
                                  const std::string& location) {
        if (!g_IBClient) return;
        auto& se = g_scannerEntries[idx];
        if (se.subActive) {
            g_IBClient->CancelScannerData(se.activeScanId);
            se.subActive = false;
        }
        // Cancel stale market-data slots for this scanner
        for (int i = 0; i < ScannerEntry::kMktSlots; ++i) {
            int rid = se.mktBase + i;
            if (g_tickerSymbols.count(rid)) {
                g_IBClient->CancelMarketData(rid);
                g_tickerSymbols.erase(rid);
            }
        }
        se.pendingResults.clear();
        if (++se.activeScanId >= se.scanBase + 100)
            se.activeScanId = se.scanBase;
        g_IBClient->ReqScannerData(se.activeScanId, scanCode, instrument, location);
        se.subActive = true;
    };

    g_scannerEntries.push_back(std::move(e));
}

static void SpawnNewsWindow(int idx) {
    NewsEntry e;
    e.win          = new ui::NewsWindow();
    e.nextArtReqId = NewsArtBase(idx);
    e.artEnd       = NewsArtEnd(idx);
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId((idx % core::kNumGroups) + 1);

    e.win->SetStockNewsReqId(NewsHistStock(idx));
    e.win->SetPortNewsReqIdBase(NewsHistPort(idx));
    e.win->SetMktNewsReqIdBase(NewsHistMkt(idx));

    e.win->OnSymbolChanged = [idx](const std::string& sym) {
        if (idx < (int)g_newsEntries.size() && g_newsEntries[idx].win)
            BroadcastGroupSymbol(g_newsEntries[idx].win->groupId(), sym);
    };
    e.win->OnStockNewsRequested = [idx](const std::string& symbol) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->ReqContractDetails(NewsStockConId(idx), symbol);
    };
    e.win->OnPortfolioNewsRequested = [idx](const std::vector<std::string>& syms) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        for (int i = 0; i < (int)syms.size() && i < 20; ++i)
            g_IBClient->ReqContractDetails(NewsPortConId(idx) + i, syms[i]);
    };
    e.win->OnArticleRequested = [idx](int itemId, const std::string& provider,
                                      const std::string& articleId) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        auto& ne = g_newsEntries[idx];
        int reqId = ne.nextArtReqId++;
        if (ne.nextArtReqId > ne.artEnd) ne.nextArtReqId = NewsArtBase(idx);
        ne.artReqToItemId[reqId] = itemId;
        g_IBClient->ReqNewsArticle(reqId, provider, articleId);
    };

    g_newsEntries.push_back(std::move(e));

    // If already connected, seed market news for this new window
    if (g_IBClient && g_IBClient->IsConnected()) {
        for (int i = 0; i < kMktSeedCount; ++i)
            g_IBClient->ReqContractDetails(NewsConIdMkt(idx) + i, kMktSeedSymbols[i]);
    }
}

// ============================================================================
// Watchlist persistence (Task #49)
// ============================================================================
static std::string WatchlistsFilePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    return std::string(home) + "/.config/ibkr-trading-app/watchlists.cfg";
}

static void EnsureWatchlistConfigDir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return;
    std::filesystem::create_directories(std::string(home) + "/.config/ibkr-trading-app");
}

static void SaveWatchlistsFile() {
    if (g_watchlistEntries.empty()) return;
    EnsureWatchlistConfigDir();
    std::string path = WatchlistsFilePath();
    std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (const auto& we : g_watchlistEntries)
            if (we.win) f << we.win->serialize();
    }
    std::rename(tmp.c_str(), path.c_str());
}

struct WatchlistSaveBlock {
    int instanceId = 1;
    int groupId    = 0;
    std::vector<core::Watchlist> watchlists;
};

// ============================================================================
// Chart-mode persistence (Phase 13 — Trading-Style Modes)
//
// Stores per-chart `(instanceIdx, symbol, TradingStyle [+ TF for Free])` so a
// kill-and-restart returns each chart to its last-set mode. Symbol is part of
// the key so a saved "AAPL = Scalping" doesn't reapply when the user has since
// changed that chart's symbol — we fall back to the default Swing in that case.
//
// Format:
//   INSTANCE:0
//   SYMBOL:AAPL
//   STYLE:2          (integer enum value; 0=Scalping, 1=DayTrading, 2=Swing, 3=Investment, 4=Free)
//   TF:6             (optional; only written when STYLE==Free. Integer Timeframe enum value.)
// ============================================================================
static std::string ChartModesFilePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    return std::string(home) + "/.config/ibkr-trading-app/chart-modes.cfg";
}

struct ChartModeBlock {
    int         instanceIdx = 0;   // 0-based index, matches g_chartEntries position
    std::string symbol;
    int         style       = (int)core::services::TradingStyle::Swing;
    int         timeframe   = -1;  // -1 = use preset's TF; non-negative = override (Free mode)
};

static void SaveChartModesFile() {
    if (g_chartEntries.empty()) return;
    EnsureWatchlistConfigDir();   // same ~/.config/ibkr-trading-app/ root
    std::string path = ChartModesFilePath();
    std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (int i = 0; i < (int)g_chartEntries.size(); ++i) {
            const auto& ce = g_chartEntries[i];
            if (!ce.win || !ce.win->open()) continue;
            std::string sym = ce.win->getSymbol();
            if (sym.empty()) continue;
            f << "INSTANCE:" << i << "\n";
            f << "SYMBOL:"   << sym << "\n";
            auto style = ce.win->tradingStyle();
            f << "STYLE:"    << (int)style << "\n";
            // Free mode: also persist the user-picked TF so a restart
            // restores the chart to the same bar size.
            if (style == core::services::TradingStyle::Free)
                f << "TF:"   << (int)ce.win->getTimeframe() << "\n";
        }
    }
    std::rename(tmp.c_str(), path.c_str());
}

static std::vector<ChartModeBlock> LoadChartModesFromFile() {
    std::vector<ChartModeBlock> result;
    std::ifstream f(ChartModesFilePath());
    if (!f.is_open()) return result;

    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= 9 && line.substr(0, 9) == "INSTANCE:") {
            ChartModeBlock b;
            try { b.instanceIdx = std::stoi(line.substr(9)); } catch (...) {}
            result.push_back(std::move(b));
        } else if (!result.empty() && line.size() >= 7 && line.substr(0, 7) == "SYMBOL:") {
            result.back().symbol = line.substr(7);
        } else if (!result.empty() && line.size() >= 6 && line.substr(0, 6) == "STYLE:") {
            try {
                int v = std::stoi(line.substr(6));
                if (v >= 0 && v <= (int)core::services::TradingStyle::Free)
                    result.back().style = v;
            } catch (...) {}
        } else if (!result.empty() && line.size() >= 3 && line.substr(0, 3) == "TF:") {
            try {
                int v = std::stoi(line.substr(3));
                if (v >= (int)core::Timeframe::M1 && v <= (int)core::Timeframe::MN)
                    result.back().timeframe = v;
            } catch (...) {}
        }
    }
    return result;
}

// ---- News provider filter persistence -----------------------------------------

static std::string NewsProvidersFilePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    return std::string(home) + "/.config/ibkr-trading-app/news-providers.cfg";
}

static void SaveDisabledNewsProviders() {
    EnsureWatchlistConfigDir();
    std::string path = NewsProvidersFilePath();
    std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (const auto& code : g_disabledNewsProviders) f << code << '\n';
    }
    std::rename(tmp.c_str(), path.c_str());
}

static void LoadDisabledNewsProviders() {
    g_disabledNewsProviders.clear();
    std::ifstream f(NewsProvidersFilePath());
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) g_disabledNewsProviders.insert(line);
    }
}

// Rebuild the colon-joined cache from g_newsProvidersList minus the user's
// disabled set. Called from onNewsProviders (fresh IB list) and from the
// Settings checkbox toggle (re-apply disabled filter without a roundtrip).
static void RebuildEntitledNewsProviders() {
    std::string joined;
    for (const auto& [code, name] : g_newsProvidersList) {
        if (g_disabledNewsProviders.count(code)) continue;
        if (!joined.empty()) joined += ':';
        joined += code;
    }
    g_entitledNewsProviders = std::move(joined);
}

// ---- Per-chart UI settings persistence (Phase 17 Task #81) -------------------
//
// Stores every ChartWindow user-tunable setting: indicator toggles + params,
// auto-analysis toggles + params, setup-overlay knobs + confluence gates,
// useRTH / showOvernight / showLegend, subplot height ratios. Distinct from
// chart-modes.cfg (which holds symbol + trading-style + Free-mode TF).
//
// File: ~/.config/ibkr-trading-app/chart-settings.cfg
// Format: one INSTANCE block per chart, all fields serialized by
// ChartWindow::SerializeSettings.
//
// Per-second flush from RenderTradingUI uses a hash-diff against
// g_lastChartSettingsHash so unchanged state never touches disk — no need to
// scatter dirty-flag flips across every toolbar/popup mutation site.
static size_t g_lastChartSettingsHash = 0;

static std::string BuildChartSettingsText() {
    if (g_chartEntries.empty()) return std::string();
    std::vector<core::services::StateBlock> blocks;
    blocks.reserve(g_chartEntries.size());
    for (int i = 0; i < (int)g_chartEntries.size(); ++i) {
        const auto& ce = g_chartEntries[i];
        if (!ce.win || !ce.win->open()) continue;
        core::services::StateBlock b;
        b.instance = i;
        ce.win->SerializeSettings(b);
        blocks.push_back(std::move(b));
    }
    return core::services::FormatStateBlocks(blocks);
}

static void SaveChartSettingsFile() {
    std::string text = BuildChartSettingsText();
    if (text.empty()) return;
    size_t h = std::hash<std::string>{}(text);
    if (h == g_lastChartSettingsHash) return;   // no change since last write
    std::string path = core::services::ConfigFilePath("chart-settings.cfg");
    if (path.empty()) return;
    if (core::services::AtomicWriteText(path, text))
        g_lastChartSettingsHash = h;
}

static void LoadChartSettingsFromFile() {
    using namespace core::services;
    std::string path = ConfigFilePath("chart-settings.cfg");
    if (path.empty()) return;
    bool exists = false;
    std::string contents = ReadTextFile(path, &exists);
    if (!exists) return;
    auto blocks = ParseStateBlocks(contents);
    for (const auto& b : blocks) {
        if (b.instance < 0 || b.instance >= (int)g_chartEntries.size()) continue;
        auto& ce = g_chartEntries[b.instance];
        if (!ce.win) continue;
        ce.win->ApplySettings(b);
    }
    // Stash the on-disk hash so the next per-second flush is a no-op until
    // the user actually changes something.
    g_lastChartSettingsHash = std::hash<std::string>{}(contents);
}

// ---- Per-trading-window UI settings persistence (Phase 17 Task #82) ---------
//
// L2 mode, exchange filter (by name — survives smart-component refresh),
// depth row count, splitter ratios, click-to-trade, expand-spread, and order
// entry defaults (qty / side / type / TIF / outside-RTH).
//
// File: ~/.config/ibkr-trading-app/trading-settings.cfg
// Format: one INSTANCE block per TradingWindow, serialised via
// TradingWindow::SerializeSettings.
//
// Hash-diff flush identical to chart-settings.cfg — no dirty-flag plumbing.
static size_t g_lastTradingSettingsHash = 0;

static std::string BuildTradingSettingsText() {
    if (g_tradingEntries.empty()) return std::string();
    std::vector<core::services::StateBlock> blocks;
    blocks.reserve(g_tradingEntries.size());
    for (int i = 0; i < (int)g_tradingEntries.size(); ++i) {
        const auto& te = g_tradingEntries[i];
        if (!te.win || !te.win->open()) continue;
        core::services::StateBlock b;
        b.instance = i;
        te.win->SerializeSettings(b);
        blocks.push_back(std::move(b));
    }
    return core::services::FormatStateBlocks(blocks);
}

static void SaveTradingSettingsFile() {
    std::string text = BuildTradingSettingsText();
    if (text.empty()) return;
    size_t h = std::hash<std::string>{}(text);
    if (h == g_lastTradingSettingsHash) return;
    std::string path = core::services::ConfigFilePath("trading-settings.cfg");
    if (path.empty()) return;
    if (core::services::AtomicWriteText(path, text))
        g_lastTradingSettingsHash = h;
}

static void LoadTradingSettingsFromFile() {
    using namespace core::services;
    std::string path = ConfigFilePath("trading-settings.cfg");
    if (path.empty()) return;
    bool exists = false;
    std::string contents = ReadTextFile(path, &exists);
    if (!exists) return;
    auto blocks = ParseStateBlocks(contents);
    // Spawn missing instances so saved blocks beyond index 0 land.
    int maxInst = -1;
    for (const auto& b : blocks)
        if (b.instance > maxInst) maxInst = b.instance;
    while (maxInst >= (int)g_tradingEntries.size() &&
           (int)g_tradingEntries.size() < kMaxMultiWin)
        SpawnTradingWindow((int)g_tradingEntries.size());
    for (const auto& b : blocks) {
        if (b.instance < 0 || b.instance >= (int)g_tradingEntries.size()) continue;
        auto& te = g_tradingEntries[b.instance];
        if (!te.win) continue;
        te.win->ApplySettings(b);
    }
    g_lastTradingSettingsHash = std::hash<std::string>{}(contents);
}

// ---- Per-scanner UI settings persistence (Phase 17 Task #83) ----------------
//
// Asset class, preset index, ScanFilter ranges + UI buffers, column
// visibility (14 columns), sort column + direction, filter-bar expanded
// flag, auto-refresh toggle + interval.
//
// File: ~/.config/ibkr-trading-app/scanner-settings.cfg
// Hash-diff flush, same pattern as chart/trading.
static size_t g_lastScannerSettingsHash = 0;

static std::string BuildScannerSettingsText() {
    if (g_scannerEntries.empty()) return std::string();
    std::vector<core::services::StateBlock> blocks;
    blocks.reserve(g_scannerEntries.size());
    for (int i = 0; i < (int)g_scannerEntries.size(); ++i) {
        const auto& se = g_scannerEntries[i];
        if (!se.win || !se.win->open()) continue;
        core::services::StateBlock b;
        b.instance = i;
        se.win->SerializeSettings(b);
        blocks.push_back(std::move(b));
    }
    return core::services::FormatStateBlocks(blocks);
}

static void SaveScannerSettingsFile() {
    std::string text = BuildScannerSettingsText();
    if (text.empty()) return;
    size_t h = std::hash<std::string>{}(text);
    if (h == g_lastScannerSettingsHash) return;
    std::string path = core::services::ConfigFilePath("scanner-settings.cfg");
    if (path.empty()) return;
    if (core::services::AtomicWriteText(path, text))
        g_lastScannerSettingsHash = h;
}

static void LoadScannerSettingsFromFile() {
    using namespace core::services;
    std::string path = ConfigFilePath("scanner-settings.cfg");
    if (path.empty()) return;
    bool exists = false;
    std::string contents = ReadTextFile(path, &exists);
    if (!exists) return;
    auto blocks = ParseStateBlocks(contents);
    // Spawn missing instances so saved blocks beyond index 0 land.
    int maxInst = -1;
    for (const auto& b : blocks)
        if (b.instance > maxInst) maxInst = b.instance;
    while (maxInst >= (int)g_scannerEntries.size() &&
           (int)g_scannerEntries.size() < kMaxMultiWin)
        SpawnScannerWindow((int)g_scannerEntries.size());
    for (const auto& b : blocks) {
        if (b.instance < 0 || b.instance >= (int)g_scannerEntries.size()) continue;
        auto& se = g_scannerEntries[b.instance];
        if (!se.win) continue;
        se.win->ApplySettings(b);
    }
    g_lastScannerSettingsHash = std::hash<std::string>{}(contents);
}

// ---- Singleton-window settings persistence (Phase 17 Task #83) -------------
//
// One WINDOW:<name> block each for Portfolio, Orders, and WshCalendar.
// Portfolio: sort column + direction, 7 column-visibility toggles, trade-
//   history filter symbol buffer.
// Orders: history-tab filter symbol, side, date.
// WshCalendar: filter symbol, date range, type, importance, sort col + asc.
//
// Active-tab state on Portfolio / Orders is NOT persisted here — both use
// ImGui TabItem which ImGui's ini file already preserves (Task 2 wired
// IniFilename to a stable path).
//
// File: ~/.config/ibkr-trading-app/singleton-settings.cfg
// Hash-diff flush, same pattern as chart/trading/scanner.

static size_t g_lastSingletonSettingsHash = 0;

static std::string BuildSingletonSettingsText() {
    using namespace core::services;
    std::vector<StateBlock> blocks;
    blocks.reserve(3);

    if (g_PortfolioWindow) {
        StateBlock b;
        b.windowName = "portfolio";
        g_PortfolioWindow->SerializeSettings(b);
        blocks.push_back(std::move(b));
    }
    if (g_OrdersWindow) {
        StateBlock b;
        b.windowName = "orders";
        g_OrdersWindow->SerializeSettings(b);
        blocks.push_back(std::move(b));
    }
    if (g_WshCalendarWindow) {
        StateBlock b;
        b.windowName = "wsh";
        g_WshCalendarWindow->SerializeSettings(b);
        blocks.push_back(std::move(b));
    }
    return FormatStateBlocks(blocks);
}

static void SaveSingletonSettingsFile() {
    std::string text = BuildSingletonSettingsText();
    if (text.empty()) return;
    size_t h = std::hash<std::string>{}(text);
    if (h == g_lastSingletonSettingsHash) return;
    std::string path = core::services::ConfigFilePath("singleton-settings.cfg");
    if (path.empty()) return;
    if (core::services::AtomicWriteText(path, text))
        g_lastSingletonSettingsHash = h;
}

static void LoadSingletonSettingsFromFile() {
    using namespace core::services;
    std::string path = ConfigFilePath("singleton-settings.cfg");
    if (path.empty()) return;
    bool exists = false;
    std::string contents = ReadTextFile(path, &exists);
    if (!exists) return;
    auto blocks = ParseStateBlocks(contents);
    for (const auto& b : blocks) {
        if (b.windowName == "portfolio" && g_PortfolioWindow)
            g_PortfolioWindow->ApplySettings(b);
        else if (b.windowName == "orders" && g_OrdersWindow)
            g_OrdersWindow->ApplySettings(b);
        else if (b.windowName == "wsh" && g_WshCalendarWindow)
            g_WshCalendarWindow->ApplySettings(b);
    }
    g_lastSingletonSettingsHash = std::hash<std::string>{}(contents);
}

// ---- App-wide UI preferences persistence (Phase 17 Task #80) -----------------
//
// Stores font size, default trading style for newly-spawned charts, and the
// TWS Display Group sync toggle. Loaded once at app startup (before the main
// loop) so font scale is correct from the first frame. Saved on every
// Settings UI change.
//
// File: ~/.config/ibkr-trading-app/app-prefs.cfg
// Format (no INSTANCE: blocks — singleton settings):
//   FONT_SIZE:1                 # 0=Small, 1=Medium, 2=Large
//   DEFAULT_TRADING_STYLE:2     # int enum value (Scalping=0..Free=4)
//   SYNC_TWS_DISPLAY_GROUPS:0
static void SaveAppPrefsFile() {
    using namespace core::services;
    StateBlock block;
    SetInt (block, "FONT_SIZE",               (int)g_fontSize);
    SetInt (block, "DEFAULT_TRADING_STYLE",   (int)g_defaultTradingStyle);
    SetBool(block, "SYNC_TWS_DISPLAY_GROUPS", g_twsGroupSync);
    std::string path = ConfigFilePath("app-prefs.cfg");
    if (path.empty()) return;
    AtomicWriteText(path, FormatStateBlocks({block}));
}

static void LoadAppPrefsFromFile() {
    using namespace core::services;
    std::string path = ConfigFilePath("app-prefs.cfg");
    if (path.empty()) return;
    bool exists = false;
    std::string contents = ReadTextFile(path, &exists);
    if (!exists) return;   // first launch — keep construction defaults
    auto blocks = ParseStateBlocks(contents);
    if (blocks.empty()) return;
    const StateBlock& b = blocks[0];   // single singleton block

    int fs = GetInt(b, "FONT_SIZE", (int)g_fontSize, 0, 2);
    g_fontSize = static_cast<FontSize>(fs);

    int ts = GetInt(b, "DEFAULT_TRADING_STYLE", (int)g_defaultTradingStyle,
                    0, (int)core::services::TradingStyle::Free);
    g_defaultTradingStyle = static_cast<core::services::TradingStyle>(ts);

    g_twsGroupSync = GetBool(b, "SYNC_TWS_DISPLAY_GROUPS", g_twsGroupSync);
    // Note: g_twsGroupSync's IB subscribe call requires a live connection, so
    // the actual SubscribeToGroupEvents fan-out is left to FinishConnect's
    // existing post-connect block (line ~2238) which already inspects the
    // global. Loading the global here just stages it for that hook to fire.
}

// Apply font-related app-prefs to the live ImGui style. Separated from
// LoadAppPrefsFromFile because the style must already be set up (g_baseStyle
// captured) before we re-scale. Caller invokes after g_baseStyle is snapshot.
static void ApplyAppPrefsToStyle() {
    int fs = std::clamp((int)g_fontSize, 0, 2);
    ImGui::GetIO().FontGlobalScale = kFontScales[fs];
    ImGui::GetStyle() = g_baseStyle;
    ImGui::GetStyle().ScaleAllSizes(kFontScales[fs]);
}

// ---- Replay window persistence ------------------------------------------------

static std::string ReplayWindowsFilePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
    return std::string(home) + "/.config/ibkr-trading-app/replay-windows.cfg";
}

static void SaveReplayWindowsFile() {
    if (g_replayEntries.empty()) return;
    EnsureWatchlistConfigDir();
    std::string path = ReplayWindowsFilePath();
    std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        for (int i = 0; i < (int)g_replayEntries.size(); ++i) {
            const auto& re = g_replayEntries[i];
            if (!re.win || !re.win->open()) continue;
            std::string sym = re.win->getSymbol();
            if (sym.empty()) continue;
            f << "INSTANCE:"  << i << "\n";
            f << "GROUP:"     << re.win->groupId() << "\n";
            f << "SYMBOL:"    << sym << "\n";
            f << "DATE_FROM:" << re.win->getDateFromBuf() << "\n";
            f << "DATE_TO:"   << re.win->getDateToBuf() << "\n";
            f << "SESSION:"   << (int)re.win->getSession() << "\n";
            f << "TF:"       << (int)re.win->getTimeframe() << "\n";
            f << "SPEED:"    << re.win->getSpeed() << "\n";
            f << "MODE:"     << (int)re.win->getMode() << "\n";
            f << "CURSOR:"   << re.win->getCursorBarIdx() << "\n";
            f << "EQUITY:"   << re.win->getStartingCash() << "\n";
            f << "TICKFILLS:" << (re.win->getTickFills() ? 1 : 0) << "\n";
            // Indicator state — bit-packed flags + period values (replay-indicators plan §3g).
            const auto& ind = re.win->getIndicatorSettings();
            unsigned flags = 0;
            if (ind.sma20)     flags |= 1u;
            if (ind.sma50)     flags |= 2u;
            if (ind.ema20)     flags |= 4u;
            if (ind.bbands)    flags |= 8u;
            if (ind.vwap)      flags |= 16u;
            if (ind.vwapBands) flags |= 32u;
            if (ind.volume)    flags |= 64u;
            if (ind.rsi)       flags |= 128u;
            f << "IND_FLAGS:"     << flags << "\n";
            f << "IND_SMA1:"      << ind.smaPeriod1 << "\n";
            f << "IND_SMA2:"      << ind.smaPeriod2 << "\n";
            f << "IND_EMA:"       << ind.emaPeriod << "\n";
            f << "IND_BB_PERIOD:" << ind.bbPeriod << "\n";
            f << "IND_BB_SIGMA:"  << ind.bbSigma << "\n";
            f << "IND_RSI:"       << ind.rsiPeriod << "\n";
        }
    }
    std::rename(tmp.c_str(), path.c_str());
    g_replayWindowsDirty = false;
}

static void LoadReplayWindowsFromFile() {
    std::ifstream f(ReplayWindowsFilePath());
    if (!f.is_open()) return;

    struct ReplayBlock {
        int         instanceIdx = -1;
        int         groupId     = 1;
        std::string symbol;
        std::string dateFrom;
        std::string dateTo;
        int         session     = 1;    // Intraday
        int         tf          = 1;    // M5
        double      speed       = 1.0;
        int         mode        = 0;    // Analysis
        int         cursor      = 0;
        double      equity      = 100000.0;
        int         tickFills   = 0;
        bool        indSet      = false;   // any IND_* line seen → apply on restore
        ui::ReplayWindow::IndicatorSettings ind;
    };
    std::vector<ReplayBlock> blocks;

    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= 9 && line.substr(0, 9) == "INSTANCE:") {
            ReplayBlock b;
            try { b.instanceIdx = std::stoi(line.substr(9)); } catch (...) {}
            blocks.push_back(std::move(b));
        } else if (!blocks.empty()) {
            auto& b = blocks.back();
            if (line.size() >= 6 && line.substr(0, 6) == "GROUP:")
                try { b.groupId = std::stoi(line.substr(6)); } catch (...) {}
            else if (line.size() >= 7 && line.substr(0, 7) == "SYMBOL:")
                b.symbol = line.substr(7);
            else if (line.size() >= 10 && line.substr(0, 10) == "DATE_FROM:")
                b.dateFrom = line.substr(10);
            else if (line.size() >= 8 && line.substr(0, 8) == "DATE_TO:")
                b.dateTo = line.substr(8);
            else if (line.size() >= 5 && line.substr(0, 5) == "DATE:") {
                // Legacy single-day config: map to both endpoints.
                b.dateFrom = line.substr(5);
                b.dateTo   = line.substr(5);
            }
            else if (line.size() >= 8 && line.substr(0, 8) == "SESSION:")
                try { b.session = std::stoi(line.substr(8)); } catch (...) {}
            else if (line.size() >= 3 && line.substr(0, 3) == "TF:")
                try { b.tf = std::stoi(line.substr(3)); } catch (...) {}
            else if (line.size() >= 6 && line.substr(0, 6) == "SPEED:")
                try { b.speed = std::stod(line.substr(6)); } catch (...) {}
            else if (line.size() >= 5 && line.substr(0, 5) == "MODE:")
                try { b.mode = std::stoi(line.substr(5)); } catch (...) {}
            else if (line.size() >= 7 && line.substr(0, 7) == "CURSOR:")
                try { b.cursor = std::stoi(line.substr(7)); } catch (...) {}
            else if (line.size() >= 7 && line.substr(0, 7) == "EQUITY:")
                try { b.equity = std::stod(line.substr(7)); } catch (...) {}
            else if (line.size() >= 10 && line.substr(0, 10) == "TICKFILLS:")
                try { b.tickFills = std::stoi(line.substr(10)); } catch (...) {}
            else if (line.size() >= 10 && line.substr(0, 10) == "IND_FLAGS:") {
                try {
                    unsigned f = static_cast<unsigned>(std::stoul(line.substr(10)));
                    b.ind.sma20     = (f & 1u)   != 0;
                    b.ind.sma50     = (f & 2u)   != 0;
                    b.ind.ema20     = (f & 4u)   != 0;
                    b.ind.bbands    = (f & 8u)   != 0;
                    b.ind.vwap      = (f & 16u)  != 0;
                    b.ind.vwapBands = (f & 32u)  != 0;
                    b.ind.volume    = (f & 64u)  != 0;
                    b.ind.rsi       = (f & 128u) != 0;
                    b.indSet = true;
                } catch (...) {}
            }
            else if (line.size() >= 9  && line.substr(0, 9)  == "IND_SMA1:")
                try { b.ind.smaPeriod1 = std::stoi(line.substr(9));  b.indSet = true; } catch (...) {}
            else if (line.size() >= 9  && line.substr(0, 9)  == "IND_SMA2:")
                try { b.ind.smaPeriod2 = std::stoi(line.substr(9));  b.indSet = true; } catch (...) {}
            else if (line.size() >= 8  && line.substr(0, 8)  == "IND_EMA:")
                try { b.ind.emaPeriod  = std::stoi(line.substr(8));  b.indSet = true; } catch (...) {}
            else if (line.size() >= 14 && line.substr(0, 14) == "IND_BB_PERIOD:")
                try { b.ind.bbPeriod   = std::stoi(line.substr(14)); b.indSet = true; } catch (...) {}
            else if (line.size() >= 13 && line.substr(0, 13) == "IND_BB_SIGMA:")
                try { b.ind.bbSigma    = std::stod(line.substr(13)); b.indSet = true; } catch (...) {}
            else if (line.size() >= 8  && line.substr(0, 8)  == "IND_RSI:")
                try { b.ind.rsiPeriod  = std::stoi(line.substr(8));  b.indSet = true; } catch (...) {}
        }
    }

    // Apply blocks to existing replay windows
    for (const auto& b : blocks) {
        if (b.instanceIdx < 0 || b.instanceIdx >= (int)g_replayEntries.size()) continue;
        auto& re = g_replayEntries[b.instanceIdx];
        if (!re.win || b.symbol.empty()) continue;

        re.win->setGroupId(b.groupId);
        re.win->SetSymbol(b.symbol);
        // Back-compat: if legacy DATE: produced both endpoints equal, this is
        // identical to setDate(). New configs supply DATE_FROM/DATE_TO directly.
        re.win->setDateFrom(b.dateFrom.c_str());
        re.win->setDateTo(b.dateTo.empty() ? b.dateFrom.c_str() : b.dateTo.c_str());

        if (b.session >= 0 && b.session <= 3)
            re.win->setSession(static_cast<core::services::ReplaySession>(b.session));
        if (b.tf >= 0 && b.tf <= 8)
            re.win->setTimeframe(static_cast<core::Timeframe>(b.tf));

        re.win->setSpeed(b.speed);
        re.win->setMode(b.mode == 1 ? ui::ReplayWindow::Mode::Operate
                                     : ui::ReplayWindow::Mode::Analysis);
        re.win->setCursorBarIdx(b.cursor);
        re.win->setStartingCash(b.equity);
        re.win->setTickFills(b.tickFills != 0);
        if (b.indSet) re.win->setIndicatorSettings(b.ind);
    }
}

static std::vector<WatchlistSaveBlock> LoadWatchlistsFromFile() {
    std::vector<WatchlistSaveBlock> result;
    std::ifstream f(WatchlistsFilePath());
    if (!f.is_open()) return result;

    std::string line;
    while (std::getline(f, line)) {
        if (line.size() >= 9 && line.substr(0, 9) == "INSTANCE:") {
            WatchlistSaveBlock b;
            try { b.instanceId = std::stoi(line.substr(9)); } catch (...) {}
            result.push_back(std::move(b));
        } else if (!result.empty() && line.size() >= 6 && line.substr(0, 6) == "GROUP:") {
            try { result.back().groupId = std::stoi(line.substr(6)); } catch (...) {}
        } else if (!result.empty() && line.size() >= 6 && line.substr(0, 6) == "WATCH:") {
            result.back().watchlists.push_back({line.substr(6), {}});
        } else if (!result.empty() && !result.back().watchlists.empty() && !line.empty()) {
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
                result.back().watchlists.back().items.push_back(std::move(item));
        }
    }
    return result;
}

static void SpawnWatchlistWindow(int idx) {
    WatchlistEntry e;
    e.win = new ui::WatchlistWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId((idx % core::kNumGroups) + 1);
    e.win->setReqIdBase(WatchlistCdId(idx), WatchlistMktBase(idx));

    e.win->OnReqContractDetails = [idx](int reqId, const std::string& sym) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->ReqContractDetails(reqId, sym);
    };
    e.win->OnReqMktData = [](int reqId, const std::string& sym,
                              const std::string& /*secType*/, const std::string& /*exch*/,
                              const std::string& /*currency*/) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        g_IBClient->ReqMarketData(reqId, sym, "165,233");
    };
    e.win->OnCancelMktData = [](int reqId) {
        if (g_IBClient && g_IBClient->IsConnected())
            g_IBClient->CancelMarketData(reqId);
    };
    e.win->OnBroadcastSymbol = [idx](const std::string& sym) {
        if (idx < (int)g_watchlistEntries.size() && g_watchlistEntries[idx].win)
            BroadcastGroupSymbol(g_watchlistEntries[idx].win->groupId(), sym);
    };
    e.win->OnOpenChart = [](const std::string& sym) {
        int newIdx = (int)g_chartEntries.size();
        if (newIdx < kMaxMultiWin) {
            SpawnChartWindow(newIdx);
            if (!g_chartEntries.empty() && g_chartEntries.back().win)
                g_chartEntries.back().win->SetSymbol(sym);
        }
    };
    e.win->OnOpenOrderBook = [](const std::string& sym) {
        int newIdx = (int)g_tradingEntries.size();
        if (newIdx < kMaxMultiWin) {
            SpawnTradingWindow(newIdx);
            if (!g_tradingEntries.empty() && g_tradingEntries.back().win)
                ApplyTradingSymbol(g_tradingEntries.back(), sym);
        }
    };

    e.win->AddDefaultsIfEmpty();
    g_watchlistEntries.push_back(std::move(e));
}

static int ReplayBaseReqId(int idx) { return 11000 + idx * 100; }

static void SpawnReplayWindow(int idx) {
    ReplayEntry e;
    e.baseReqId = ReplayBaseReqId(idx);
    e.histId    = e.baseReqId + 0;
    e.extId     = e.baseReqId + 1;
    e.win       = new ui::ReplayWindow();
    e.win->setInstanceId(idx + 1);
    e.win->setGroupId((idx % core::kNumGroups) + 1);

    e.win->OnDataRequest = [idx](const std::string& sym,
                                  const std::string& dateFrom,
                                  const std::string& dateTo,
                                  core::services::ReplaySession /*session*/,
                                  core::Timeframe tf) {
        if (!g_IBClient || !g_IBClient->IsConnected()) return;
        if (idx < 0 || idx >= (int)g_replayEntries.size()) return;
        auto& re = g_replayEntries[idx];
        // Only cancel if a prior request is in-flight (avoids spurious error 366)
        if (re.histActive)
            g_IBClient->CancelHistoricalData(re.histId);
        re.histActive = true;
        re.pendingBars = core::BarSeries{};
        re.pendingBars.symbol    = sym;
        re.pendingBars.timeframe = tf;

        // End-datetime in IB format: yyyymmdd-hh:mm:ss (UTC dash format).
        std::string cleanTo = dateTo.empty() ? dateFrom : dateTo;
        cleanTo.erase(std::remove(cleanTo.begin(), cleanTo.end(), '-'), cleanTo.end());
        std::string endDt = cleanTo + "-23:59:59";

        // Duration: compute span in days when range is multi-day; fall back to
        // the timeframe's default single-day-ish duration when from==to.
        auto parseYmd = [](const std::string& s) -> std::time_t {
            int y = 0, m = 0, d = 0;
            if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
            std::tm tm{};
            tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d;
            tm.tm_hour = 12;
            return core::services::Timegm(&tm);
        };
        std::string duration = core::TimeframeIBDuration(tf);
        if (!dateFrom.empty() && !dateTo.empty() && dateFrom != dateTo) {
            std::time_t tFrom = parseYmd(dateFrom);
            std::time_t tTo   = parseYmd(dateTo);
            if (tFrom > 0 && tTo > tFrom) {
                int spanDays = static_cast<int>((tTo - tFrom) / 86400) + 1;
                if (spanDays < 1) spanDays = 1;
                // IB caps " D" units at 365; longer spans must be expressed in years.
                if (spanDays > 365)
                    duration = std::to_string((spanDays + 364) / 365) + " Y";
                else
                    duration = std::to_string(spanDays) + " D";
            }
        }

        g_IBClient->ReqHistoricalData(re.histId, sym,
                                      duration,
                                      core::TimeframeIBBarSize(tf),
                                      true, endDt);
        g_replayWindowsDirty = true;
    };

    e.win->OnPaperOrderSubmit = [idx](const core::Order& o) -> int {
        if (idx < 0 || idx >= (int)g_replayEntries.size()) return -1;
        auto& re = g_replayEntries[idx];
        if (!re.win) return -1;
        int localId = re.win->nextLocalId();
        re.win->incNextLocalId();
        (void)o;
        return localId;
    };

    e.win->OnPaperOrderCancel = [idx](int localId) {
        (void)idx; (void)localId;
        // The window handles removal from its own m_book directly.
    };

    e.win->OnCursorMove = [idx](std::time_t cursorTime) {
        if (idx < 0 || idx >= (int)g_replayEntries.size()) return;
        auto& re = g_replayEntries[idx];
        if (!re.win) return;
        int gid = re.win->groupId();
        double now = ImGui::GetTime();
        auto it = g_lastReplayCursorBroadcast.find(gid);
        if (it != g_lastReplayCursorBroadcast.end() && now - it->second < 0.1)
            return;  // throttle 100ms
        g_lastReplayCursorBroadcast[gid] = now;
        BroadcastReplayCursor(gid, cursorTime, idx);
        g_replayWindowsDirty = true;
    };

    g_replayEntries.push_back(std::move(e));
}

static void CreateTradingWindows() {
    // Singleton windows
    delete g_PortfolioWindow;   g_PortfolioWindow   = new ui::PortfolioWindow();
    delete g_OrdersWindow;      g_OrdersWindow      = new ui::OrdersWindow();
    delete g_WshCalendarWindow; g_WshCalendarWindow = new ui::WshCalendarWindow();

    g_WshCalendarWindow->OnReqWshEvents = [](int reqId, long conId) {
        if (g_IBClient) g_IBClient->ReqWshEventData(reqId, conId);
    };
    g_WshCalendarWindow->OnCancelWshEvents = [](int reqId) {
        if (g_IBClient) g_IBClient->CancelWshEventData(reqId);
    };
    g_WshCalendarWindow->OnBroadcastSymbol = [](const std::string& sym) {
        BroadcastGroupSymbol(1, sym);
    };

    // Spawn first instance of each multi-window type
    SpawnChartWindow(0);
    SpawnTradingWindow(0);
    SpawnScannerWindow(0);
    SpawnNewsWindow(0);
    SpawnWatchlistWindow(0);
    SpawnReplayWindow(0);

    // Wire OrdersWindow
    g_OrdersWindow->OnCancelOrder = [](int orderId) {
        if (g_IBClient) g_IBClient->CancelOrder(orderId);
    };
    g_OrdersWindow->OnRefresh = []() {
        if (g_IBClient) g_IBClient->ReqOpenOrders();
    };
    g_OrdersWindow->OnLoadHistory = [](const std::string& sym,
                                       const std::string& side,
                                       const std::string& dateFrom) {
        if (g_IBClient) g_IBClient->ReqExecutions(8001, sym, side, dateFrom);
    };
}

// Walk every active subscription registered against the current IB session and
// queue an explicit cancel.  Called on both shutdown paths (user Disconnect
// menu click + app exit) BEFORE g_IBClient->Disconnect() so IB Gateway sees
// orderly cancels rather than pending writes against a closing socket — the
// "Can't write, socket client is closing" log line was IB reporting it had
// queued depth / market-data updates for our active subscriptions when our
// eDisconnect() torn down the TCP socket out from under them.
//
// Catches the dominant per-instance subscriptions (market data, depth, hist,
// tick-by-tick) plus the four /ES + /NQ futures health feeds.  Watchlist and
// WshCalendar windows own their cancels in their dtors via CancelAll().
// Singleton account-level subscriptions (account updates, PnL, etc.) are
// already cancelled by the Disconnect() caller.
static void CancelAllSubscriptions() {
    if (!g_IBClient) return;

    for (auto& ce : g_chartEntries) {
        if (ce.mktId)  g_IBClient->CancelMarketData(ce.mktId);
        if (ce.histId) g_IBClient->CancelHistoricalData(ce.histId);
        if (ce.extStreamActive && ce.extId)
            g_IBClient->CancelHistoricalData(ce.extId);
    }

    for (auto& te : g_tradingEntries) {
        if (te.mktId)   g_IBClient->CancelMarketData(te.mktId);
        if (te.depthId) g_IBClient->CancelMktDepth(te.depthId,
                            te.win ? te.win->useL2() : false);
        if (te.tickId)  g_IBClient->CancelTickByTickData(te.tickId);
    }

    // Futures market health feeds (reqIds 140-143: /ES, /NQ front-month + Dec)
    for (int rid = 140; rid <= 143; ++rid)
        g_IBClient->CancelMarketData(rid);

    // Watchlist windows hold per-symbol market data subscriptions.  Their
    // CancelAll() walks the per-instance subs map and queues cancels.
    for (auto& we : g_watchlistEntries)
        if (we.win) we.win->CancelAll();

    // WSH calendar holds per-position WSH event subscriptions
    if (g_WshCalendarWindow) g_WshCalendarWindow->CancelAll();
}

static void DestroyTradingWindows() {
    SaveWatchlistsFile();
    // Synchronous flush of any unsaved chart-mode changes before the windows
    // are torn down. Only writes if something has changed since the last
    // once-per-second flush (or never saved at all this session).
    if (g_chartModesDirty) {
        SaveChartModesFile();
        g_chartModesDirty = false;
    }
    // Per-chart UI settings (indicator toggles, auto-analysis, setup overlay,
    // etc.) — hash-diff means this is a no-op when nothing changed since the
    // last per-second flush.
    SaveChartSettingsFile();
    // Per-TradingWindow UI settings (L2 toggle, exchange filter, depth rows,
    // splitter ratios, click-to-trade, order-entry defaults) — same hash-diff.
    SaveTradingSettingsFile();
    // Per-ScannerWindow UI settings (asset class, preset, filter ranges + UI
    // buffers, column visibility, sort, auto-refresh) — same hash-diff.
    SaveScannerSettingsFile();
    // Per-singleton-window settings (Portfolio sort/columns, Orders filter,
    // WshCalendar filter/sort) — same hash-diff.
    SaveSingletonSettingsFile();

    // Drop ticker-symbol slots before clearing entries — chart mktIds rotate
    // through AllocChartMktId(), so a static-range loop wouldn't catch them.
    for (auto& e : g_chartEntries)   g_tickerSymbols.erase(e.mktId);
    for (auto& e : g_tradingEntries) g_tickerSymbols.erase(e.mktId);
    for (auto& e : g_chartEntries)   { delete e.win; e.win = nullptr; }
    for (auto& e : g_tradingEntries) { delete e.win; e.win = nullptr; }
    for (auto& e : g_scannerEntries) { delete e.win; e.win = nullptr; }
    g_chartEntries.clear();
    g_tradingEntries.clear();
    g_scannerEntries.clear();

    for (auto& ne : g_newsEntries) { delete ne.win; ne.win = nullptr; }
    g_newsEntries.clear();
    for (auto& we : g_watchlistEntries) { if (we.win) { we.win->CancelAll(); delete we.win; we.win = nullptr; } }
    g_watchlistEntries.clear();
    if (g_replayWindowsDirty) SaveReplayWindowsFile();
    for (auto& re : g_replayEntries) { delete re.win; re.win = nullptr; }
    g_replayEntries.clear();
    delete g_PortfolioWindow;   g_PortfolioWindow   = nullptr;
    delete g_OrdersWindow;      g_OrdersWindow      = nullptr;
    delete g_WshCalendarWindow; g_WshCalendarWindow = nullptr;

    g_portfolioSymbols.clear();
    g_liveOrders.clear();
    g_positions.clear();
    g_symbolCommissions.clear();
    g_scannerPrevClose.clear();
    g_scannerVolume.clear();
    // Clear all multi-window market-data ticker slots
    for (int i = 0; i < kMaxMultiWin; ++i) {
        g_tickerSymbols.erase(ChartMktId(i));
        g_tickerSymbols.erase(TradingMktId(i));
        for (int s = 0; s < ScannerEntry::kMktSlots; ++s)
            g_tickerSymbols.erase(ScannerMktBase(i) + s);
    }
}

// ============================================================================
// Post-account-selection connect setup (called once account is known)
// ============================================================================
static void FinishConnect(bool isReconnect) {
    g_Login.state = ConnectionState::Connected;

    g_IBClient->ReqMarketDataType(g_Login.isLive ? 1 : 4);

    if (!isReconnect) {
        DestroyTradingWindows();
        CreateTradingWindows();

        // Restore persisted watchlists (overrides Mag 7 defaults if file exists)
        {
            auto saved = LoadWatchlistsFromFile();
            for (int i = 0; i < (int)saved.size(); ++i) {
                if (saved[i].watchlists.empty()) continue;
                if (i >= (int)g_watchlistEntries.size()) {
                    if ((int)g_watchlistEntries.size() >= kMaxMultiWin) break;
                    SpawnWatchlistWindow((int)g_watchlistEntries.size());
                }
                auto& we = g_watchlistEntries[i];
                if (!we.win) continue;
                we.win->setGroupId(saved[i].groupId);
                we.win->LoadWatchlists(saved[i].watchlists);
            }
        }

        // Restore persisted chart-mode selection (Phase 13). For each saved
        // block: stamp the chart's symbol, apply the style silently (sets
        // timeframe + auto/setup defaults, clears buffers), then issue the
        // historical+market-data subscription with the preset's history
        // horizon. Skips the regular AAPL seed below for any chart that was
        // restored from disk (tracked via `restoredCharts`).
        //
        // Spawn any missing instances so saved blocks beyond instance 0
        // (e.g. G2/G3 charts the user opened in a prior session) can land.
        std::vector<bool> restoredCharts;
        {
            auto savedModes = LoadChartModesFromFile();
            int  maxIdx = -1;
            for (const auto& b : savedModes)
                if (b.instanceIdx > maxIdx) maxIdx = b.instanceIdx;
            while (maxIdx >= (int)g_chartEntries.size() &&
                   (int)g_chartEntries.size() < kMaxMultiWin)
                SpawnChartWindow((int)g_chartEntries.size());

            restoredCharts.assign(g_chartEntries.size(), false);
            for (const auto& b : savedModes) {
                if (b.instanceIdx < 0 ||
                    b.instanceIdx >= (int)g_chartEntries.size()) continue;
                auto& ce = g_chartEntries[b.instanceIdx];
                if (!ce.win || b.symbol.empty()) continue;

                auto style  = static_cast<core::services::TradingStyle>(b.style);
                auto preset = core::services::GetPreset(style);

                // Apply style silently first — updates m_timeframe + settings
                // and clears every derived buffer, so the prefetch lands into
                // a clean slate. Note: setTradingStyle(Free) preserves the
                // chart's current m_timeframe; for restore we want the saved
                // TF if available, so we apply that after via setTimeframeFree.
                ce.win->setTradingStyle(style, /*silent=*/true);

                // Determine the effective TF + duration. Free mode honours the
                // saved per-chart TF (when present); other modes are bound to
                // the preset's TF.
                core::Timeframe tf       = preset.timeframe;
                std::string     duration = preset.historyDuration;
                if (style == core::services::TradingStyle::Free && b.timeframe >= 0) {
                    tf = static_cast<core::Timeframe>(b.timeframe);
                    ce.win->setTimeframeFree(tf, /*silent=*/true);
                    duration = core::TimeframeIBDuration(tf);
                } else if (style == core::services::TradingStyle::Free) {
                    // No saved TF: use whatever setTradingStyle preserved
                    // (the construction-default D1) and its dynamic duration.
                    tf       = ce.win->getTimeframe();
                    duration = core::TimeframeIBDuration(tf);
                }

                // Stamp the symbol without firing OnDataRequest — the
                // callback calls BroadcastGroupSymbol which would re-enter
                // SetSymbol on every sibling chart in the group, each of
                // which fires its own ReqChartData (default duration) +
                // BroadcastGroupSymbol chain.  With 3 charts in one group
                // that is 9+ redundant cancel→reissue cycles during connect,
                // overwhelming IB's socket with rapid-fire reqId rotations.
                // We suppress the callback and issue a single ReqChartData
                // with the correct preset duration below.
                {
                    auto saved = ce.win->OnDataRequest;
                    ce.win->OnDataRequest = nullptr;
                    ce.win->SetSymbol(b.symbol);
                    ce.win->OnDataRequest = saved;
                }
                // Daily / Weekly / Monthly bars: useRTH=true (no extended-
                // hours concept on those frames). Intraday: useRTH=false (the
                // m_useRTH default for a freshly created chart).
                bool isIntra = (tf != core::Timeframe::D1 &&
                                tf != core::Timeframe::W1 &&
                                tf != core::Timeframe::MN);
                ReqChartData(ce, b.symbol, tf,
                             /*useRTH=*/!isIntra,
                             ce.pendingBars, duration);
                restoredCharts[b.instanceIdx] = true;
            }
            // Loading from disk is not a user-initiated change.
            g_chartModesDirty = false;
        }

        // Apply per-chart UI settings (indicator toggles, auto-analysis,
        // setup overlay, useRTH / showOvernight / showLegend, splitter
        // ratios) on top of the chart-modes restore. setTradingStyle in the
        // restore loop above stamps preset defaults onto IndicatorSettings /
        // AutoAnalysisSettings / SetupSettings; this load layers the user's
        // customisations over those defaults. Settings outside the preset
        // (useRTH/showOvernight/showLegend/splitter ratios) are also
        // restored here. m_useRTH may now differ from the value used in the
        // ReqChartData call above — that just means a few extra bars from
        // IB get filtered at render time, not a correctness bug.
        LoadChartSettingsFromFile();

        // Apply per-TradingWindow UI settings (L2 mode, exchange filter,
        // depth row count, splitter ratios, click-to-trade, order-entry
        // defaults). No subscription side effects — these only take effect
        // when the user types a symbol; the user-symbol path then issues
        // ReqMktDepth/ReqMarketData with the restored numDepthRows/useL2.
        LoadTradingSettingsFromFile();

        // Apply per-ScannerWindow UI settings (asset class, preset, filter
        // ranges + UI buffers, column visibility, sort, auto-refresh).
        // No scan side effects — RunScan is user-initiated.
        LoadScannerSettingsFromFile();
        // Singleton-window settings: Portfolio sort/columns, Orders filter,
        // WshCalendar filter/sort. Applied before the first account-data
        // fan-out so sort orders are correct from the first frame.
        LoadSingletonSettingsFromFile();

        g_IBClient->ReqAccountUpdates(true, g_selectedAccount);
        g_IBClient->ReqPositions();
        g_IBClient->ReqAccountSummary(ACCT_SUMMARY_REQID, ACCT_SUMMARY_TAGS);
        g_IBClient->ReqOpenOrders();
        g_IBClient->ReqAllOpenOrders();
        g_IBClient->ReqExecutions(8001);
        for (auto& se : g_scannerEntries)
            g_IBClient->CancelScannerData(se.activeScanId);

        const std::string sym = "AAPL";
        if (!g_chartEntries.empty() && !restoredCharts[0]) {
            auto& ce = g_chartEntries[0];
            ce.pendingBars.symbol    = sym;
            ce.pendingBars.timeframe = core::Timeframe::D1;
            g_tickerSymbols[ce.mktId] = sym;
            g_IBClient->ReqHistoricalData(ce.histId, sym,
                                          core::TimeframeIBDuration(core::Timeframe::D1),
                                          core::TimeframeIBBarSize(core::Timeframe::D1),
                                          true);
            ce.histStreamActive = true;
            g_IBClient->ReqMarketData(ce.mktId, sym, MktDataTicks());
        }
        if (!g_tradingEntries.empty())
            ApplyTradingSymbol(g_tradingEntries[0], sym);

        g_IBClient->SubscribeToNews(NEWS_RT_REQID);
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni)
            for (int i = 0; i < kMktSeedCount; ++i)
                g_IBClient->ReqContractDetails(NewsConIdMkt(ni) + i, kMktSeedSymbols[i]);

        // Futures market health — reqIds 140-143
        g_IBClient->ReqFuturesMarketData(140, "/ES");
        g_IBClient->ReqFuturesMarketData(141, "/NQ");
        auto t = std::time(nullptr);
        int decYear = std::gmtime(&t)->tm_year + 1900;
        char esDec[20], nqDec[20];
        std::snprintf(esDec, sizeof(esDec), "/ES %04d12", decYear);
        std::snprintf(nqDec, sizeof(nqDec), "/NQ %04d12", decYear);
        g_IBClient->ReqFuturesMarketData(142, esDec);
        g_IBClient->ReqFuturesMarketData(143, nqDec);
    } else {
        printf("[IB] Reconnected — re-subscribing all windows.\n");
        g_IBClient->ReqAccountUpdates(true, g_selectedAccount);
        g_IBClient->ReqPositions();
        g_IBClient->ReqAccountSummary(ACCT_SUMMARY_REQID, ACCT_SUMMARY_TAGS);
        g_IBClient->ReqOpenOrders();
        g_IBClient->ReqAllOpenOrders();
        g_IBClient->ReqExecutions(8001);
        for (auto& se : g_scannerEntries)
            g_IBClient->CancelScannerData(se.activeScanId);

        for (auto& ce : g_chartEntries) {
            if (!ce.win) continue;
            std::string sym = ce.win->getSymbol();
            if (sym.empty()) continue;
            core::Timeframe tf = ce.win->getTimeframe();
            g_tickerSymbols[ce.mktId] = sym;
            ce.pendingBars.symbol    = sym;
            ce.pendingBars.timeframe = tf;
            ce.pendingBars.bars.clear();   // discard leftovers from pre-disconnect symbol
            g_IBClient->CancelHistoricalData(ce.extId);
            ce.extStreamActive  = false;
            ce.pendingExtBars   = core::BarSeries{};
            g_IBClient->ReqHistoricalData(ce.histId, sym,
                                          core::TimeframeIBDuration(tf),
                                          core::TimeframeIBBarSize(tf),
                                          true);
            ce.histStreamActive = true;
            g_IBClient->ReqMarketData(ce.mktId, sym, MktDataTicks());
        }
        for (auto& te : g_tradingEntries) {
            if (!te.win) continue;
            std::string sym = te.win->getSymbol();
            if (!sym.empty()) ApplyTradingSymbol(te, sym);
        }

        g_IBClient->SubscribeToNews(NEWS_RT_REQID);
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni)
            for (int i = 0; i < kMktSeedCount; ++i)
                g_IBClient->ReqContractDetails(NewsConIdMkt(ni) + i, kMktSeedSymbols[i]);

        // Futures market health
        g_IBClient->ReqFuturesMarketData(140, "/ES");
        g_IBClient->ReqFuturesMarketData(141, "/NQ");
        auto t2 = std::time(nullptr);
        int decYear2 = std::gmtime(&t2)->tm_year + 1900;
        char esDec2[20], nqDec2[20];
        std::snprintf(esDec2, sizeof(esDec2), "/ES %04d12", decYear2);
        std::snprintf(nqDec2, sizeof(nqDec2), "/NQ %04d12", decYear2);
        g_IBClient->ReqFuturesMarketData(142, esDec2);
        g_IBClient->ReqFuturesMarketData(143, nqDec2);
    }

    // Re-subscribe to TWS display groups if sync was already enabled before (re)connect.
    if (g_twsGroupSync)
        for (int g = 1; g <= 4; ++g)
            g_IBClient->SubscribeToGroupEvents(8060 + g, g);

    // Ask IB which news providers this account is entitled to. The response
    // populates g_entitledNewsProviders; reqHistoricalNews call sites check it
    // before firing so we never request unsubscribed providers.
    g_entitledNewsProviders.clear();
    g_newsProvidersList.clear();
    g_IBClient->ReqNewsProviders();

    fprintf(stderr, "[main] FinishConnect done isReconnect=%d\n", isReconnect); fflush(stderr);
}

// ============================================================================
// IB API connection wiring
// ============================================================================
static void WireIBCallbacks() {
    // ── Managed accounts (fires before nextValidId so account is known early) ─
    g_IBClient->onManagedAccounts = [](const std::vector<std::string>& accts) {
        g_managedAccounts = accts;
        if (accts.size() == 1)
            g_selectedAccount = accts[0];
        printf("[IB] Managed accounts: %zu account(s)\n", accts.size());
    };

    // ── Connection state ──────────────────────────────────────────────────
    g_IBClient->onConnectionChanged = [](bool connected, const std::string& info) {
        if (connected) {
            bool isReconnect = (g_Login.state == ConnectionState::LostConnection);
            g_Login.connectedAs = g_Login.isLive ? "[LIVE]" : "[PAPER]";
            printf("[IB] %s\n", info.c_str());

            if (g_selectedAccount.empty() && g_managedAccounts.size() > 1) {
                // Multi-account: defer window creation until user picks account.
                g_Login.state      = ConnectionState::SelectingAccount;
                g_pendingReconnect = isReconnect;
            } else {
                FinishConnect(isReconnect);
            }

            if (isReconnect && g_NotificationService) {
                g_NotificationService->Notify(
                    core::services::NotificationSeverity::Success,
                    core::services::NotificationCategory::Connection,
                    core::services::NotificationEvent::ConnectionRestored,
                    "Reconnected", "Session restored.");
            }
        } else {
            // Any disconnect while not in the normal "initial Connecting" flow
            // → keep windows alive, null client, schedule a reconnect attempt.
            if (g_Login.state == ConnectionState::Connecting) {
                g_Login.state    = ConnectionState::Error;
                g_Login.errorMsg = info;
            } else {
                delete g_IBClient;
                g_IBClient                = nullptr;
                g_Login.state             = ConnectionState::LostConnection;
                g_reconnectNextAttempt    = glfwGetTime() + kReconnectIntervalSec;
                if (g_NotificationService) {
                    g_NotificationService->Notify(
                        core::services::NotificationSeverity::Warning,
                        core::services::NotificationCategory::Connection,
                        core::services::NotificationEvent::ConnectionLost,
                        "Disconnected", "Lost connection to IB Gateway.");
                }
            }
            printf("[IB] Disconnected: %s\n", info.c_str());
        }
    };

    // ── Historical bars (chart) ───────────────────────────────────────────
    g_IBClient->onBarData = [](int reqId, const core::Bar& bar, bool done, bool isLive) {
        for (auto& ce : g_chartEntries) {
            if (reqId == ce.extId) {
                // Extend-history (pan-left) response — prepend to existing chart data.
                // Stream-active gate: drop bars that arrive after done=true so a
                // late or duplicate completion can't replay PrependHistoricalData.
                if (!ce.extStreamActive) return;
                if (!done) {
                    ce.pendingExtBars.bars.push_back(bar);
                } else if (ce.win) {
                    ce.win->PrependHistoricalData(ce.pendingExtBars);
                    ce.pendingExtBars.bars.clear();
                    ce.extStreamActive = false;
                }
                return;
            }
            if (reqId == ce.histId) {
                // Live updates (keepUpToDate) flow even after the historical
                // stream has ended, so they bypass the stream-active gate.
                if (isLive) {
                    if (ce.win) ce.win->UpdateLiveBar(bar);
                    return;
                }
                // Historical stream gate: drop late bars and duplicate done=true
                // packets so a phantom second SetHistoricalData(pendingBars) can't
                // wipe the chart with a 0/1-bar series. Symptom this prevents:
                // "everything erased and a single candle appears" on intraday
                // charts after a symbol switch.
                if (!ce.histStreamActive) return;
                if (!done) {
                    ce.pendingBars.bars.push_back(bar);
                } else if (ce.win) {
                    ce.win->SetHistoricalData(ce.pendingBars);
                    ce.pendingBars.bars.clear();
                    ce.histStreamActive = false;
                }
                return;
            }
        }
        // Replay entry historical data (reqIds 11000–11999)
        for (auto& re : g_replayEntries) {
            if (reqId == re.histId) {
                if (!done) {
                    re.pendingBars.bars.push_back(bar);
                } else if (re.win) {
                    // Build HistoricalDay from pending bars
                    core::HistoricalDay day;
                    day.symbol = re.pendingBars.symbol;
                    day.bars   = re.pendingBars.bars;
                    re.day     = day;
                    fprintf(stderr, "[replay] SetDay: %s %zu bars\n",
                            day.symbol.c_str(), day.bars.size());
                    re.win->SetDay(day);
                    re.pendingBars.bars.clear();
                    re.histActive = false;
                }
                return;
            }
        }
    };

    // ── Market data ticks ─────────────────────────────────────────────────
    g_IBClient->onTickSize = [](int tickerId, int field, double size) {
        // Normalise delayed-data variants (paper / reqMarketDataType(3)) to standard fields.
        // DELAYED_BID_SIZE=69, DELAYED_ASK_SIZE=70, DELAYED_LAST_SIZE=71, DELAYED_VOLUME=74
        switch (field) {
            case 69: field = 0; break;
            case 70: field = 3; break;
            case 71: field = 5; break;
            case 74: field = 8; break;  // DELAYED_VOLUME → VOLUME
            default: break;
        }

        // Trading entry: NBBO sizes and LAST_SIZE
        for (auto& te : g_tradingEntries) {
            if (tickerId != te.mktId) continue;
            switch (field) {
                case 5: te.lastTickSize = size; break;
                case 0:  // BID_SIZE
                    te.nbboBidSz = size;
                    if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    break;
                case 3:  // ASK_SIZE
                    te.nbboAskSz = size;
                    if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    break;
                default: break;
            }
        }

        // Watchlist entries (reqIds 7000–7999) — self-routing by reqId
        if (tickerId >= 7000 && tickerId < 8000) {
            for (auto& we : g_watchlistEntries)
                if (we.win) we.win->OnTickSize(tickerId, field, size);
            return;
        }

        // Scanner entries: avg volume (87) and day volume (8)
        auto symIt = g_tickerSymbols.find(tickerId);
        if (symIt == g_tickerSymbols.end()) return;
        const std::string& sym = symIt->second;
        for (auto& se : g_scannerEntries) {
            if (tickerId < se.mktBase || tickerId >= se.mktBase + ScannerEntry::kMktSlots) continue;
            if (field == 87 && se.win)
                se.win->SetAvgVolume(sym, size);
            if (field == 8) {
                g_scannerVolume[sym] = size;
                if (se.win) se.win->OnQuoteUpdate(sym, 0.0, 0.0, 0.0, size);
            }
        }
    };

    g_IBClient->onTickPrice = [](int tickerId, int field, double price) {
        // Normalise delayed-data tick fields (paper / reqMarketDataType(3)) to their
        // standard equivalents so the switch below handles both live and paper accounts.
        // DELAYED_BID=66, DELAYED_ASK=67, DELAYED_LAST=68,
        // DELAYED_HIGH=72, DELAYED_LOW=73, DELAYED_OPEN=76
        switch (field) {
            case 66: field = 1;  break;  // DELAYED_BID
            case 67: field = 2;  break;  // DELAYED_ASK
            case 68: field = 4;  break;  // DELAYED_LAST
            case 72: field = 6;  break;  // DELAYED_HIGH
            case 73: field = 7;  break;  // DELAYED_LOW
            case 75: field = 9;  break;  // DELAYED_CLOSE (previous session close)
            case 76: field = 14; break;  // DELAYED_OPEN
            default: break;
        }

        // Futures market health (reqIds 140-143 /ES, /NQ front+Dec) — fan out to all charts
        if (tickerId >= 140 && tickerId <= 143) {
            for (auto& ce : g_chartEntries)
                if (ce.win) ce.win->OnFuturesTick(tickerId, field, price);
            return;
        }

        // Watchlist entries (reqIds 7000–7999) — self-routing by reqId
        if (tickerId >= 7000 && tickerId < 8000) {
            for (auto& we : g_watchlistEntries)
                if (we.win) we.win->OnTickPrice(tickerId, field, price);
            return;
        }

        auto it = g_tickerSymbols.find(tickerId);
        if (it == g_tickerSymbols.end()) return;
        const std::string& sym = it->second;

        // Find which scanner entry owns this tickerId (if any)
        ScannerEntry* scanEntry = nullptr;
        for (auto& se : g_scannerEntries) {
            if (tickerId >= se.mktBase && tickerId < se.mktBase + ScannerEntry::kMktSlots) {
                scanEntry = &se;
                break;
            }
        }

        switch (field) {
            case 1:  // BID price — fire NBBO update
                for (auto& te : g_tradingEntries) {
                    if (tickerId == te.mktId) {
                        te.nbboBid = price;
                        if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    }
                }
                break;
            case 2:  // ASK price — fire NBBO update
                for (auto& te : g_tradingEntries) {
                    if (tickerId == te.mktId) {
                        te.nbboAsk = price;
                        if (te.win) te.win->OnNBBO(te.nbboBid, te.nbboBidSz, te.nbboAsk, te.nbboAskSz);
                    }
                }
                break;
            case 9: {  // CLOSE — previous session close price
                g_scannerPrevClose[sym] = price;
                if (scanEntry && scanEntry->win)
                    scanEntry->win->SetPrevClose(sym, price);
                break;
            }
            case 79: {  // IB_52_WK_HIGH (from generic tick 165)
                if (scanEntry && scanEntry->win)
                    scanEntry->win->Set52WHigh(sym, price);
                break;
            }
            case 80: {  // IB_52_WK_LOW (from generic tick 165)
                if (scanEntry && scanEntry->win)
                    scanEntry->win->Set52WLow(sym, price);
                break;
            }
            case 4: {  // LAST price
                // Trading windows
                for (auto& te : g_tradingEntries) {
                    if (tickerId == te.mktId) {
                        bool isUp = (price >= te.lastTickPrice);
                        te.lastTickPrice = price;
                        if (te.win) {
                            te.win->UpdateMidPrice(price);
                            te.win->OnTick(price, te.lastTickSize, isUp);
                        }
                    }
                }
                // Chart windows
                for (auto& ce : g_chartEntries) {
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol()) {
                        ce.win->OnLastPrice(price);
                        ce.win->OnDayTick(4, price);
                    }
                }
                // Position market-price update
                auto pit = g_positions.find(sym);
                if (pit != g_positions.end()) {
                    pit->second.marketPrice = price;
                    UpdateAllChartPositions();
                }
                // Scanner: compute change/% from stored prevClose
                if (scanEntry && scanEntry->win) {
                    double prevC = 0.0;
                    auto pcIt = g_scannerPrevClose.find(sym);
                    if (pcIt != g_scannerPrevClose.end()) prevC = pcIt->second;
                    double chg    = prevC > 0.0 ? price - prevC : 0.0;
                    double chgPct = prevC > 0.0 ? (chg / prevC) * 100.0 : 0.0;
                    double vol    = 0.0;
                    auto vIt = g_scannerVolume.find(sym);
                    if (vIt != g_scannerVolume.end()) vol = vIt->second;
                    scanEntry->win->OnQuoteUpdate(sym, price, chg, chgPct, vol);
                }
                break;
            }
            case 6: {  // HIGH
                for (auto& ce : g_chartEntries)
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol())
                        ce.win->OnDayTick(6, price);
                break;
            }
            case 7: {  // LOW
                for (auto& ce : g_chartEntries)
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol())
                        ce.win->OnDayTick(7, price);
                break;
            }
            case 14: {  // OPEN
                for (auto& ce : g_chartEntries)
                    if (tickerId == ce.mktId && ce.win && sym == ce.win->getSymbol())
                        ce.win->OnDayTick(14, price);
                break;
            }
            default: break;
        }
    };

    // ── Market depth (Level II order book) ───────────────────────────────
    g_IBClient->onDepthUpdate = [](int id, bool isBid, int pos, int op,
                                   double price, double size,
                                   const std::string& exchange, bool isSmartDepth) {
        for (auto& te : g_tradingEntries)
            if (id == te.depthId && te.win)
                te.win->OnDepthUpdate(id, isBid, pos, op, price, size,
                                      exchange, isSmartDepth);
    };

    // ── Account values ────────────────────────────────────────────────────
    g_IBClient->onAccountValue = [](const std::string& key, const std::string& val,
                                    const std::string& currency,
                                    const std::string& acct) {
        if (g_PortfolioWindow)
            g_PortfolioWindow->OnAccountValue(key, val, currency, acct);
        // Capture account ID on first receipt and subscribe account-level P&L.
        if (!acct.empty() && g_accountId.empty()) {
            g_accountId = acct;
        }
        if (!g_pnlSubscribed && !g_selectedAccount.empty() && g_IBClient) {
            g_pnlSubscribed = true;
            g_IBClient->ReqPnL(9000, g_selectedAccount);
        }
    };

    // ── Account summary (base currency via reqAccountSummary) ─────────────
    // tag="Currency", value="USD" — this is the authoritative source.
    g_IBClient->onAccountSummary = [](const std::string& tag, const std::string& value,
                                      const std::string& currency) {
        if (!g_PortfolioWindow) return;
        // Route every financial tag through OnAccountValue so the portfolio
        // header always has live data even when reqAccountUpdates is slow or
        // delivers currency-suffixed variants on live accounts.
        g_PortfolioWindow->OnAccountValue(tag, value, currency, g_selectedAccount);
    };

    // ── Real-time P&L ─────────────────────────────────────────────────────
    g_IBClient->onPnL = [](int /*reqId*/, double daily, double unrealized, double realized) {
        if (g_PortfolioWindow) g_PortfolioWindow->OnPnL(daily, unrealized, realized);
    };

    g_IBClient->onPnLSingle = [](int reqId, double daily,
                                  double /*unrealized*/, double /*realized*/, double /*value*/) {
        auto sit = g_pnlReqIdToSymbol.find(reqId);
        if (sit == g_pnlReqIdToSymbol.end()) return;
        const std::string& sym = sit->second;
        // Update the shared position map so ChartWindow picks it up.
        auto pit = g_positions.find(sym);
        if (pit != g_positions.end()) {
            pit->second.dailyPnL = daily;
            UpdateAllChartPositions();
        }
        if (g_PortfolioWindow) g_PortfolioWindow->OnPnLSingle(reqId, sym, daily);
    };

    // ── Symbol autocomplete ───────────────────────────────────────────────
    ui::g_symbolSearchFn = [](const std::string& pattern) {
        if (g_IBClient) g_IBClient->ReqMatchingSymbols(8000, pattern);
    };
    g_IBClient->onSymbolSamples = [](int /*reqId*/,
                                      const std::vector<core::services::ContractDesc>& results) {
        std::vector<ui::SymbolResult> out;
        out.reserve(results.size());
        for (const auto& r : results)
            out.push_back({r.symbol, r.secType, r.primaryExch, r.currency});
        ui::UpdateSymbolSearchResults(std::move(out));
    };

    // ── Positions ─────────────────────────────────────────────────────────
    g_IBClient->onPositionData = [](const core::Position& pos, bool done) {
        if (g_PortfolioWindow) {
            if (!done)
                g_PortfolioWindow->OnPositionUpdate(pos);
            else
                g_PortfolioWindow->OnAccountEnd();
        }
        // Mirror to g_positions so RecomputeUnguardedPositions sees this side
        // of the feed too. onPortfolioUpdate populates g_positions for held
        // symbols, but onPositionData is the canonical truth for quantity.
        if (!done && std::abs(pos.quantity) > 1e-9) {
            auto pit = g_positions.find(pos.symbol);
            double savedDailyPnL = (pit != g_positions.end()) ? pit->second.dailyPnL : 0.0;
            g_positions[pos.symbol] = pos;
            g_positions[pos.symbol].dailyPnL = savedDailyPnL;
        } else if (!done && std::abs(pos.quantity) < 1e-9) {
            // Flat — drop from the cache so the warning clears.
            g_positions.erase(pos.symbol);
        }
        // Accumulate symbols; on done push to scanner and news window
        if (!done) {
            auto it = std::find(g_portfolioSymbols.begin(),
                                g_portfolioSymbols.end(), pos.symbol);
            if (it == g_portfolioSymbols.end())
                g_portfolioSymbols.push_back(pos.symbol);
            // Subscribe WSH events for this position (reqPositions feed).
            if (pos.conId > 0 && g_WshCalendarWindow)
                g_WshCalendarWindow->SubscribeConId(static_cast<int>(pos.conId), pos.symbol);
        } else {
            for (auto& se : g_scannerEntries)
                if (se.win) se.win->SetPortfolioSymbols(g_portfolioSymbols);
            for (auto& ne : g_newsEntries)
                if (ne.win) ne.win->SetPortfolioSymbols(g_portfolioSymbols);
        }
        RecomputeUnguardedPositions();
    };

    // ── Portfolio updates (P&L etc.) ──────────────────────────────────────
    g_IBClient->onPortfolioUpdate = [](const core::Position& pos) {
        if (g_PortfolioWindow) g_PortfolioWindow->OnPositionUpdate(pos);
        // Preserve dailyPnL already populated by onPnLSingle before overwriting.
        auto it = g_positions.find(pos.symbol);
        double savedDailyPnL = (it != g_positions.end()) ? it->second.dailyPnL : 0.0;
        g_positions[pos.symbol] = pos;
        g_positions[pos.symbol].dailyPnL = savedDailyPnL;
        UpdateAllChartPositions();
        // Keep order book windows in sync with live position data
        for (auto& te : g_tradingEntries)
            if (te.win && te.win->getSymbol() == pos.symbol)
                te.win->SetPosition(pos.quantity, pos.avgCost);
        // Subscribe per-position real-time P&L the first time we see a conId.
        if (pos.conId > 0 && g_pnlSingleConIds.find(pos.conId) == g_pnlSingleConIds.end()
                && !g_accountId.empty() && g_IBClient) {
            int rid = g_pnlSingleNextReqId++;
            g_pnlSingleConIds[pos.conId]   = rid;
            g_pnlReqIdToSymbol[rid]        = pos.symbol;
            g_IBClient->ReqPnLSingle(rid, g_selectedAccount, "", static_cast<int>(pos.conId));
        }
        // Subscribe WSH events for this position in the calendar window.
        if (pos.conId > 0 && g_WshCalendarWindow)
            g_WshCalendarWindow->SubscribeConId(static_cast<int>(pos.conId), pos.symbol);
        RecomputeUnguardedPositions();
    };

    // ── Open orders (full detail on submit / reqOpenOrders) ───────────────
    g_IBClient->onOpenOrder = [](const core::Order& order) {
        g_liveOrders[order.orderId] = order;
        if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(order);
        UpdateAllChartPendingOrders();
        RecomputeUnguardedPositions();
        // IB acks a locally-placed order via onOpenOrder before
        // orderStatus arrives; fire the accept toast here so we don't miss
        // it if onOrderStatusChanged is delayed or coalesced. Idempotent.
        MaybeNotifyOrderAccepted(order.orderId);
    };
    g_IBClient->onOpenOrderEnd = []() {
        // nothing extra needed — data already pushed via onOpenOrder
    };

    // ── Order status ──────────────────────────────────────────────────────
    g_IBClient->onOrderStatusChanged = [](int orderId, core::OrderStatus status,
                                          double filled, double avgPrice) {
        for (auto& te : g_tradingEntries)
            if (te.win) te.win->OnOrderStatus(orderId, status, filled, avgPrice);
        if (g_OrdersWindow)
            g_OrdersWindow->OnOrderStatus(orderId, status, filled, avgPrice);
        // Keep our local order map in sync for the chart overlay.
        auto it = g_liveOrders.find(orderId);
        if (it != g_liveOrders.end()) {
            it->second.status       = status;
            it->second.filledQty    = filled;
            it->second.avgFillPrice = avgPrice;
        }
        UpdateAllChartPendingOrders();
        RecomputeUnguardedPositions();

        // Fire the OrderWorking / OrderHeld toast on first observation of
        // an accepted state for a locally-placed order. Idempotent across
        // both onOpenOrder and onOrderStatusChanged — whichever IB fires
        // first wins. On terminal status (Filled / Cancelled / Rejected)
        // before any Working observation (rare but possible on instant
        // rejection), drop the id silently so we don't toast Working for
        // an order that never made it.
        if (status == core::OrderStatus::Filled    ||
            status == core::OrderStatus::Cancelled ||
            status == core::OrderStatus::Rejected) {
            g_pendingLocalAccept.erase(orderId);
        } else {
            MaybeNotifyOrderAccepted(orderId);
        }

        // Bracket: LMT cancelled/rejected → forget pending STP
        if (status == core::OrderStatus::Cancelled ||
            status == core::OrderStatus::Rejected) {
            g_pendingBracketStops.erase(orderId);
        }

        // Cancel notification — fires on every Cancelled status, including
        // user-initiated cancels. Acts as confirmation that IB actually
        // processed the cancel request. Rejected is handled separately via
        // onError (which has the IB reason string).
        if (status == core::OrderStatus::Cancelled && g_NotificationService &&
            it != g_liveOrders.end()) {
            char body[160];
            std::snprintf(body, sizeof(body), "%s %s",
                          it->second.symbol.c_str(),
                          core::OrderTypeStr(it->second.type));
            g_NotificationService->Notify(
                core::services::NotificationSeverity::Warning,
                core::services::NotificationCategory::Orders,
                core::services::NotificationEvent::OrderCancelled,
                "Order cancelled", body);
        }
    };

    // ── Fills ─────────────────────────────────────────────────────────────
    g_IBClient->onFillReceived = [](const core::Fill& fill) {
        for (auto& te : g_tradingEntries)
            if (te.win && te.win->getSymbol() == fill.symbol) te.win->OnFill(fill);
        if (g_OrdersWindow) g_OrdersWindow->OnFill(fill);

        // Notification: full vs partial fill, derived from g_liveOrders.
        if (g_NotificationService) {
            auto oit = g_liveOrders.find(fill.orderId);
            const char* sideStr = (fill.side == core::OrderSide::Buy) ? "BUY" : "SELL";
            const bool isPartial = (oit != g_liveOrders.end()) &&
                (oit->second.filledQty + fill.quantity + 1e-9 < oit->second.quantity);
            char body[160];
            if (isPartial) {
                std::snprintf(body, sizeof(body), "%g/%g %s @ $%.2f",
                              oit->second.filledQty + fill.quantity,
                              oit->second.quantity, fill.symbol.c_str(), fill.price);
                g_NotificationService->Notify(
                    core::services::NotificationSeverity::Success,
                    core::services::NotificationCategory::Orders,
                    core::services::NotificationEvent::OrderPartialFill,
                    "Partial fill", body);
            } else {
                std::snprintf(body, sizeof(body), "%s %g %s @ $%.2f",
                              sideStr, fill.quantity, fill.symbol.c_str(), fill.price);
                g_NotificationService->Notify(
                    core::services::NotificationSeverity::Success,
                    core::services::NotificationCategory::Orders,
                    core::services::NotificationEvent::OrderFilled,
                    "Filled", body);
            }
        }

        // Accumulate commission per symbol for the P&L strip
        g_symbolCommissions[fill.symbol] += fill.commission;
        if (g_PortfolioWindow) {
            core::TradeRecord tr;
            tr.tradeId    = fill.orderId;
            tr.symbol     = fill.symbol;
            tr.side       = fill.side == core::OrderSide::Buy ? "BUY" : "SELL";
            tr.quantity   = fill.quantity;
            tr.price      = fill.price;
            tr.commission = fill.commission;
            tr.realizedPnL = fill.realizedPnL;
            tr.executedAt  = fill.timestamp;
            g_PortfolioWindow->OnTradeExecuted(tr);
        }
        UpdateAllChartPositions();

        // Bracket: LMT filled → submit the pending STP stop-loss (and TP take-profit
        // when present) as an OCA pair. ocaType=1 = cancel-with-block: when one
        // leg fills, IB cancels the survivor.
        auto bit = g_pendingBracketStops.find(fill.orderId);
        if (bit != g_pendingBracketStops.end()) {
            const auto& p = bit->second;
            std::string ocaTag = p.tpPrice > 0.0
                ? ("BRK_" + std::to_string(fill.orderId))
                : std::string{};
            std::time_t now = std::time(nullptr);

            int stopId = g_nextOrderId++;
            core::Order stop;
            stop.orderId    = stopId;
            stop.symbol     = p.symbol;
            stop.side       = p.stopSide;
            stop.type       = p.useStopLmt ? core::OrderType::StopLimit : core::OrderType::Stop;
            stop.quantity   = p.qty;
            stop.stopPrice  = p.stopPrice;
            if (p.useStopLmt) {
                // Offset the limit slightly past the stop so a fast move through
                // the level still fills.  For a long (SELL stop): limit = stop - 0.1%.
                // For a short (BUY stop):  limit = stop + 0.1%.
                // Round to $0.01 tick so IB doesn't reject with error 110.
                double pad = p.stopPrice * 0.001;
                double raw = (p.stopSide == core::OrderSide::Sell)
                    ? p.stopPrice - pad : p.stopPrice + pad;
                stop.limitPrice = core::services::RoundToTick(raw, 0.01);
            }
            stop.tif        = core::TimeInForce::GTC;
            // Stop triggers are evaluated by IB only when outsideRth=true outside
            // RTH (for instruments that allow ext-hours stops). For an intra-RTH
            // bracket this stays false; for an ext-hours bracket the caller already
            // set p.outsideRth=true so the stop is eligible to fire pre/post-market.
            stop.outsideRth = p.outsideRth;
            stop.account    = g_selectedAccount;
            stop.ocaGroup   = ocaTag;
            stop.ocaType    = ocaTag.empty() ? 0 : 1;
            stop.status     = core::OrderStatus::Pending;
            stop.submittedAt = now;
            stop.updatedAt   = now;
            g_liveOrders[stopId] = stop;
            g_pendingLocalAccept.insert(stopId);
            if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(stop);
            g_IBClient->PlaceOrder(stop);

            if (p.tpPrice > 0.0) {
                int tpId = g_nextOrderId++;
                core::Order tp;
                tp.orderId    = tpId;
                tp.symbol     = p.symbol;
                tp.side       = p.stopSide;             // same side as stop (close leg)
                tp.type       = core::OrderType::Limit;
                tp.quantity   = p.qty;
                tp.limitPrice = p.tpPrice;
                tp.tif        = core::TimeInForce::GTC;
                tp.outsideRth = p.outsideRth;
                tp.account    = g_selectedAccount;
                tp.ocaGroup   = ocaTag;
                tp.ocaType    = 1;
                tp.status     = core::OrderStatus::Pending;
                tp.submittedAt = now;
                tp.updatedAt   = now;
                g_liveOrders[tpId] = tp;
                g_pendingLocalAccept.insert(tpId);
                if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(tp);
                g_IBClient->PlaceOrder(tp);
            }

            UpdateAllChartPendingOrders();
            g_pendingBracketStops.erase(bit);
        }
    };
    g_IBClient->onQueriedFill = [](const core::Fill& fill) {
        if (g_OrdersWindow) g_OrdersWindow->OnQueriedFill(fill);
    };
    g_IBClient->onTickByTick = [](int reqId, const core::Tick& tick) {
        for (auto& te : g_tradingEntries)
            if (te.tickId == reqId && te.win) te.win->OnTickByTick(tick);
    };
    // ── Smart components / exchange routing ───────────────────────────────
    // reqId 8040–8049 = chart instances; 8050–8059 = trading instances.
    g_IBClient->onTickReqParams = [](int tickerId, const std::string& bboExchange) {
        auto applyToWindow = [&](auto& entries, int reqBase) {
            for (int i = 0; i < (int)entries.size(); ++i) {
                auto& e = entries[i];
                if (e.mktId != tickerId) continue;
                e.bboExchange = bboExchange;
                auto it = g_smartComponents.find(bboExchange);
                if (it != g_smartComponents.end()) {
                    std::vector<std::string> exch = {"SMART"};
                    for (const auto& r : it->second) exch.push_back(r.exchange);
                    if (e.win) e.win->SetExchangeList(exch);
                } else if (g_IBClient) {
                    g_IBClient->ReqSmartComponents(reqBase + i, bboExchange);
                }
                return true;
            }
            return false;
        };
        if (!applyToWindow(g_chartEntries,   8040))
              applyToWindow(g_tradingEntries, 8050);
    };
    g_IBClient->onSmartComponents = [](int reqId,
                                        const std::vector<core::SmartRoute>& routes) {
        std::vector<std::string> exch = {"SMART"};
        for (const auto& r : routes) exch.push_back(r.exchange);

        auto apply = [&](auto& entries, int reqBase) {
            int idx = reqId - reqBase;
            if (idx < 0 || idx >= (int)entries.size()) return false;
            auto& e = entries[idx];
            if (e.win) e.win->SetExchangeList(exch);
            if (!e.bboExchange.empty())
                g_smartComponents[e.bboExchange] = routes;
            return true;
        };
        if (!apply(g_chartEntries,   8040))
              apply(g_tradingEntries, 8050);
    };
    g_IBClient->onWshEvent = [](int reqId, const std::string& data) {
        // Chart per-symbol markers (8020–8029)
        WshData::WshEvent ev = WshData::ParseWshEvent(data);
        for (int ci = 0; ci < (int)g_chartEntries.size(); ++ci)
            if (reqId == ChartWshId(ci) && g_chartEntries[ci].win)
                g_chartEntries[ci].win->OnWshEvent(ev);
        // Calendar window aggregate view (8070–8199)
        if (reqId >= ui::WshCalendarWindow::kReqBase &&
            reqId <= ui::WshCalendarWindow::kReqEnd  && g_WshCalendarWindow)
            g_WshCalendarWindow->OnWshEvent(reqId, data);
    };

    // ── Scanner ───────────────────────────────────────────────────────────
    g_IBClient->onScanItem = [](int reqId, const core::ScanResult& result) {
        for (auto& se : g_scannerEntries)
            if (reqId == se.activeScanId) se.pendingResults.push_back(result);
    };
    g_IBClient->onScanEnd = [](int reqId) {
        for (auto& se : g_scannerEntries) {
            if (reqId != se.activeScanId || !se.win) continue;

            // Cancel previously-subscribed market data slots before (re)subscribing.
            // Without this, a rapid rescan reuses the same reqIds while the farm still has
            // them active → "Duplicate ticker id" errors for each slot.
            for (int i = 0; i < ScannerEntry::kMktSlots; ++i) {
                int rid = se.mktBase + i;
                if (g_tickerSymbols.count(rid)) {
                    g_IBClient->CancelMarketData(rid);
                    g_tickerSymbols.erase(rid);
                }
            }

            // Deliver results (empty vector clears m_scanning without wiping the table).
            se.win->OnScanData(reqId, se.pendingResults);

            // Subscribe market data for each result so price/change/volume columns live-update.
            int slot = 0;
            for (const auto& r : se.pendingResults) {
                if (slot >= ScannerEntry::kMktSlots) break;
                int rid = se.mktBase + slot;
                g_tickerSymbols[rid] = r.symbol;
                g_IBClient->ReqMarketData(rid, r.symbol, MktDataTicks());
                ++slot;
            }
            se.pendingResults.clear();
            break;
        }
    };

    // ── News — real-time (tickNews fires for subscribed market data) ──────
    g_IBClient->onNewsItem = [](std::time_t ts, const std::string& provider,
                                const std::string& articleId,
                                const std::string& headline) {
        core::NewsItem item;
        item.id        = ++g_newsItemId;
        item.headline  = headline;
        item.source    = provider;
        item.summary   = articleId;   // transport: window extracts → m_itemArticleIds
        item.timestamp = ts;
        item.sentiment = core::NewsSentiment::Neutral;
        item.category  = core::NewsCategory::Market;
        for (auto& ne : g_newsEntries)
            if (ne.win) ne.win->OnMarketNewsItem(item);
    };

    // ── News — contract details → historical news chain ───────────────────
    g_IBClient->onContractConId = [](int reqId, long conId,
                                      const std::string& description,
                                      const std::string& secType,
                                      const std::string& primaryExch,
                                      const std::string& currency) {
        if (!g_IBClient) return;
        // IB may call contractDetails multiple times (one per exchange match).
        // Only use the first conId per reqId so we don't issue duplicate requests.

        // Cache symbol → conId for display-group outbound sync.
        for (const auto& ce : g_chartEntries)
            if (reqId == ce.wshId && ce.win)
                g_symbolConIds[ce.win->getSymbol()] = conId;

        // Watchlist contract details (reqIds 6900–6909, one per instance)
        for (int wi = 0; wi < (int)g_watchlistEntries.size(); ++wi) {
            if (reqId == WatchlistCdId(wi)) {
                auto& we = g_watchlistEntries[wi];
                if (we.win)
                    we.win->SetContractDetails(reqId, conId, description,
                                               secType, primaryExch, currency);
                return;
            }
        }

        // Chart WSH event markers (reqIds 8020–8029)
        for (int ci = 0; ci < (int)g_chartEntries.size(); ++ci) {
            auto& ce = g_chartEntries[ci];
            if (reqId == ChartWshId(ci)) {
                if (ce.wshConIdFired) return;
                ce.wshConIdFired = true;
                g_IBClient->ReqWshEventData(reqId, (int)conId);
                // Also subscribe this symbol in the calendar aggregate view.
                if (g_WshCalendarWindow && ce.win)
                    g_WshCalendarWindow->SubscribeConId(
                        static_cast<int>(conId), ce.win->getSymbol());
                return;
            }
        }

        // Suppress historical-news requests when the account has no news
        // entitlements — IB returns error 321/502 with the full provider list.
        // The entitled list is populated asynchronously by ReqNewsProviders;
        // until it arrives, calls fall through and clear the loading state on
        // the news window. (g_entitledNewsProviders.empty() == "list not yet
        // received" OR "no entitlements at all" — both are non-firing cases.)
        const std::string& providers = g_entitledNewsProviders;
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (reqId == NewsStockConId(ni)) {
                if (ne.newsConIdFired[reqId]) return;
                ne.newsConIdFired[reqId] = true;
                if (providers.empty()) {
                    if (ne.win) ne.win->OnHistoricalNewsEnd(NewsHistStock(ni));
                } else {
                    g_IBClient->ReqHistoricalNews(NewsHistStock(ni), (int)conId, 30, providers);
                }
                return;
            }
            if (reqId >= NewsPortConId(ni) && reqId < NewsPortConId(ni) + 20) {
                if (ne.newsConIdFired[reqId]) return;
                ne.newsConIdFired[reqId] = true;
                int histId = NewsHistPort(ni) + (reqId - NewsPortConId(ni));
                if (providers.empty()) {
                    if (ne.win) ne.win->OnHistoricalNewsEnd(histId);
                } else {
                    g_IBClient->ReqHistoricalNews(histId, (int)conId, 10, providers);
                }
                return;
            }
            if (reqId >= NewsConIdMkt(ni) && reqId < NewsConIdMkt(ni) + kMktSeedCount) {
                if (ne.newsConIdFired[reqId]) return;
                ne.newsConIdFired[reqId] = true;
                int histId = NewsHistMkt(ni) + (reqId - NewsConIdMkt(ni));
                if (providers.empty()) {
                    if (ne.win) ne.win->OnHistoricalNewsEnd(histId);
                } else {
                    g_IBClient->ReqHistoricalNews(histId, (int)conId, 20, providers);
                }
                return;
            }
        }
    };

    g_IBClient->onHistoricalNews = [](int reqId, std::time_t ts,
                                      const std::string& provider,
                                      const std::string& articleId,
                                      const std::string& headline) {
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (!ne.win) continue;
            core::NewsCategory cat;
            bool found = false;
            if (reqId == NewsHistStock(ni)) {
                cat = core::NewsCategory::Stock; found = true;
            } else if (reqId >= NewsHistMkt(ni) && reqId < NewsHistMkt(ni) + kMktSeedCount) {
                cat = core::NewsCategory::Market; found = true;
            } else if (reqId >= NewsHistPort(ni) && reqId < NewsHistPort(ni) + 20) {
                cat = core::NewsCategory::Portfolio; found = true;
            }
            if (!found) continue;
            core::NewsItem item;
            item.id        = ++g_histNewsId;
            item.headline  = headline;
            item.source    = provider;
            item.summary   = articleId;
            item.timestamp = ts;
            item.sentiment = core::NewsSentiment::Neutral;
            item.category  = cat;
            ne.win->OnHistoricalNewsItem(reqId, item);
            return;
        }
    };

    g_IBClient->onHistoricalNewsEnd = [](int reqId) {
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (!ne.win) continue;
            if (reqId == NewsHistStock(ni) ||
                (reqId >= NewsHistMkt(ni)  && reqId < NewsHistMkt(ni)  + kMktSeedCount) ||
                (reqId >= NewsHistPort(ni) && reqId < NewsHistPort(ni) + 20)) {
                ne.win->OnHistoricalNewsEnd(reqId);
                return;
            }
        }
    };

    g_IBClient->onNewsArticle = [](int reqId, int /*articleType*/,
                                   const std::string& text) {
        for (auto& ne : g_newsEntries) {
            auto it = ne.artReqToItemId.find(reqId);
            if (it == ne.artReqToItemId.end()) continue;
            int itemId = it->second;
            ne.artReqToItemId.erase(it);
            if (ne.win) ne.win->OnArticleReceived(itemId, text);
            return;
        }
    };

    g_IBClient->onNewsProviders =
        [](const std::vector<std::pair<std::string, std::string>>& providers) {
            g_newsProvidersList = providers;
            // Drop stale entries from the disabled set — IB removed an
            // entitlement, so user's "disabled" preference for it is moot.
            for (auto it = g_disabledNewsProviders.begin();
                 it != g_disabledNewsProviders.end(); ) {
                bool stillEntitled = false;
                for (const auto& [code, _] : providers)
                    if (code == *it) { stillEntitled = true; break; }
                if (stillEntitled) ++it;
                else               it = g_disabledNewsProviders.erase(it);
            }
            RebuildEntitledNewsProviders();
            fprintf(stderr, "[news] entitled providers (%d), filtered to (%d): %s\n",
                    (int)providers.size(),
                    (int)(providers.size() - g_disabledNewsProviders.size()),
                    g_entitledNewsProviders.c_str());
        };

    // ── Errors ────────────────────────────────────────────────────────────
    g_IBClient->onError = [](int reqId, int code, const std::string& msg) {
        fprintf(stderr, "[IB Error reqId=%d code=%d] %s\n", reqId, code, msg.c_str());

        // ── Informational hold warnings ──────────────────────────────────────
        // IB sends these as error() but the order is still live — it's just
        // being held (typically until RTH open) or has had an attribute
        // adjusted by the routing engine. Stamp them onto the order's
        // holdReason so the UI can show a "(held)" tag without flipping the
        // order to Rejected.
        //  399  — order message warning (general info modifier)
        //  404  — order held until market open
        //  2109 — outside RTH attribute ignored on non-routed order
        //  2148 — order held: routed but not yet eligible
        //  10311 — order has a constraint but is not rejected
        const bool isHoldWarning =
            code == 399  || code == 404  || code == 2109 ||
            code == 2148 || code == 10311;
        if (isHoldWarning) {
            auto hit = g_liveOrders.find(reqId);
            if (hit != g_liveOrders.end() &&
                hit->second.status != core::OrderStatus::Filled    &&
                hit->second.status != core::OrderStatus::Cancelled &&
                hit->second.status != core::OrderStatus::Rejected) {
                char hold[512];
                std::snprintf(hold, sizeof(hold), "[%d] %s", code, msg.c_str());
                hit->second.holdReason = hold;
                // OrdersWindow is the global blotter; refresh it via OnOpenOrder
                // which preserves any commission / fill info already received.
                // TradingWindow's per-instance blotter only tracks locally-
                // submitted orders so we skip it here — chart overlay covers
                // the bracket case where the stop/TP legs were submitted by
                // main.cpp on entry fill.
                if (g_OrdersWindow) g_OrdersWindow->OnOpenOrder(hit->second);
                UpdateAllChartPendingOrders();
                if (g_NotificationService) {
                    char body[260];
                    std::snprintf(body, sizeof(body), "%s %s — %s",
                                  hit->second.symbol.c_str(),
                                  core::OrderTypeStr(hit->second.type),
                                  msg.c_str());
                    g_NotificationService->Notify(
                        core::services::NotificationSeverity::Warning,
                        core::services::NotificationCategory::Orders,
                        core::services::NotificationEvent::OrderHeld,
                        "Order held", body);
                }
            }
            return;  // do NOT fall through to the rejection path
        }

        // Order-related error: mark the order as Rejected in all windows.
        // IB sends error() for rejections alongside (or instead of) orderStatus().
        // We act on Pending OR Working orders — outside-RTH rejections arrive after
        // IB has already set the order to Working state.
        auto it = g_liveOrders.find(reqId);
        if (it != g_liveOrders.end() &&
            (it->second.status == core::OrderStatus::Pending  ||
             it->second.status == core::OrderStatus::Working  ||
             it->second.status == core::OrderStatus::PartialFill) &&
             it->second.status != core::OrderStatus::PendingCancel) {
            char reason[512];
            std::snprintf(reason, sizeof(reason), "[%d] %s", code, msg.c_str());
            it->second.status       = core::OrderStatus::Rejected;
            it->second.rejectReason = reason;
            if (g_OrdersWindow)
                g_OrdersWindow->OnOrderStatus(reqId, core::OrderStatus::Rejected, 0, 0, reason);
            for (auto& te : g_tradingEntries)
                if (te.win) te.win->OnOrderStatus(reqId, core::OrderStatus::Rejected, 0, 0);
            UpdateAllChartPendingOrders();

            // Notify on order rejection — distinct from generic IB-error toasts.
            if (g_NotificationService) {
                char body[260];
                std::snprintf(body, sizeof(body), "%s %s — %s",
                              it->second.symbol.c_str(),
                              core::OrderTypeStr(it->second.type),
                              msg.c_str());
                g_NotificationService->Notify(
                    core::services::NotificationSeverity::Error,
                    core::services::NotificationCategory::Orders,
                    core::services::NotificationEvent::OrderRejected,
                    "Order rejected", body);
            }
        }

        // Curated IB-error allowlist: only surface these as toasts. Everything
        // else stays in the log (filtered by the lambda above for order-status
        // tracking but not for user-facing alert).
        //
        // Code 202 ("Order Canceled - Reason: ...") is deliberately NOT in
        // this list: IB sends it alongside orderStatus(Cancelled) for every
        // cancel, and the orderStatus path below already fires an
        // OrderCancelled toast with proper user-cancel dedup. Surfacing 202
        // here as IbError would double-toast every cancel.
        if (g_NotificationService) {
            const bool surface =
                code == 110   ||   // price does not conform to min tick
                code == 201   ||   // order rejected
                code == 321   ||   // server validation error
                code == 354   ||   // not subscribed
                code == 10148 ||   // can't modify cancelled order
                code == 10149;     // can't cancel — order in terminal state
            // Skip if we already rendered the order-rejected toast above, or
            // if the order is already in a terminal Cancelled state (the
            // OrderCancelled toast handles those), to avoid duplicate noise.
            const bool alreadyHandled = (it != g_liveOrders.end()) &&
                (it->second.status == core::OrderStatus::Rejected ||
                 it->second.status == core::OrderStatus::Cancelled);
            if (surface && !alreadyHandled) {
                char title[64];
                std::snprintf(title, sizeof(title), "IB error %d", code);
                std::string body = msg.size() > 200 ? msg.substr(0, 200) : msg;
                g_NotificationService->Notify(
                    core::services::NotificationSeverity::Error,
                    core::services::NotificationCategory::Orders,
                    core::services::NotificationEvent::IbError,
                    title, std::move(body));
            }
        }

        // News errors: if reqHistoricalNews or reqContractDetails fails (e.g. no news
        // subscription, code 321/10197), IB sends an error instead of historicalNewsEnd.
        // Clear the loading state so the UI doesn't hang on "Loading..." indefinitely.
        for (int ni = 0; ni < (int)g_newsEntries.size(); ++ni) {
            auto& ne = g_newsEntries[ni];
            if (!ne.win) continue;
            if (reqId == NewsHistStock(ni) || reqId == NewsStockConId(ni)) {
                ne.win->OnHistoricalNewsEnd(NewsHistStock(ni));
                break;
            }
            if (reqId >= NewsHistPort(ni) && reqId < NewsHistPort(ni) + 20) {
                ne.win->OnHistoricalNewsEnd(reqId);
                break;
            }
            if (reqId >= NewsPortConId(ni) && reqId < NewsPortConId(ni) + 20) {
                ne.win->OnHistoricalNewsEnd(NewsHistPort(ni) + (reqId - NewsPortConId(ni)));
                break;
            }
        }

        // Subscription errors for market data (NBBO) and Level II depth per trading entry.
        // 354 = not subscribed, 10090 = partial, 10092 = deep book not allowed, 322 = no perms
        for (auto& te : g_tradingEntries) {
            if (!te.win) continue;
            if (reqId == te.mktId && (code == 354 || code == 10090))
                te.win->OnMktDataError(code);
            if (reqId == te.depthId && (code == 354 || code == 10090 || code == 10092 || code == 322))
                te.win->OnDepthError(code);
        }
    };

    // ── IB TWS Display Group sync ─────────────────────────────────────────
    g_IBClient->onDisplayGroupList = [](int /*reqId*/, const std::string& groups) {
        printf("[IB] Display groups available: %s\n", groups.c_str());
    };
    g_IBClient->onDisplayGroupUpdated = [](int reqId, const std::string& contractInfo) {
        // reqId 8061–8064 maps to G1–G4
        int groupId = reqId - 8060;  // 8061→1, 8062→2, ...
        if (groupId < 1 || groupId > 4) return;
        // Parse "symbol:secType:exchange:conId" — extract first colon-delimited field
        auto colon = contractInfo.find(':');
        std::string sym = (colon != std::string::npos)
                          ? contractInfo.substr(0, colon)
                          : contractInfo;
        if (!sym.empty())
            BroadcastGroupSymbol(groupId, sym);
    };

    // ── Next valid order id ───────────────────────────────────────────────
    g_IBClient->onNextValidId = [](int id) {
        g_nextOrderId = id;
        printf("[IB] Next valid order ID: %d\n", id);
    };
}

// ============================================================================
// Connect / Disconnect
// ============================================================================
static void StartConnect() {
    g_Login.state    = ConnectionState::Connecting;
    g_Login.errorMsg.clear();

    delete g_IBClient;
    g_IBClient = new core::services::IBKRClient();
    WireIBCallbacks();

    printf("[IB] Connecting to %s:%d  clientId=%d  account=%s\n",
           g_Login.host, g_Login.port, g_Login.clientId,
           g_Login.isLive ? "LIVE" : "PAPER");

    bool ok = g_IBClient->Connect(g_Login.host, g_Login.port, g_Login.clientId);
    if (!ok) {
        g_Login.state    = ConnectionState::Error;
        g_Login.errorMsg = std::string("Cannot reach ") + g_Login.host +
                           ":" + std::to_string(g_Login.port) +
                           " — is IB Gateway / TWS running?";
        delete g_IBClient;
        g_IBClient = nullptr;
    }
}

// Silent background reconnect — called from the main loop when LostConnection.
// State stays LostConnection until onConnectionChanged fires with connected=true.
static void StartSilentReconnect() {
    printf("[IB] Auto-reconnect attempt to %s:%d...\n", g_Login.host, g_Login.port);
    delete g_IBClient;
    g_IBClient = new core::services::IBKRClient();
    WireIBCallbacks();

    bool ok = g_IBClient->Connect(g_Login.host, g_Login.port, g_Login.clientId);
    if (!ok) {
        // Gateway still down — delete client and schedule next retry.
        delete g_IBClient;
        g_IBClient             = nullptr;
        g_reconnectNextAttempt = glfwGetTime() + kReconnectIntervalSec;
        printf("[IB] Reconnect failed — will retry in %.0fs.\n", kReconnectIntervalSec);
    }
    // On success the async onConnectionChanged(true) callback fires and sets Connected.
}

static void Disconnect() {
    if (g_IBClient) {
        CancelAllSubscriptions();   // flush per-instance cancels before socket close
        g_IBClient->ReqAccountUpdates(false);
        if (g_pnlSubscribed)
            g_IBClient->CancelPnL(9000);
        for (auto& [conId, rid] : g_pnlSingleConIds)
            g_IBClient->CancelPnLSingle(rid);
        g_IBClient->Disconnect();
        delete g_IBClient;
        g_IBClient = nullptr;
    }
    g_managedAccounts.clear();
    g_selectedAccount.clear();
    g_pendingReconnect = false;
    g_accountId.clear();
    g_pnlSubscribed = false;
    g_pnlSingleConIds.clear();
    g_pnlReqIdToSymbol.clear();
    g_pnlSingleNextReqId = 9001;
    ui::g_symbolSearchFn = nullptr;
    g_Login.state       = ConnectionState::Disconnected;
    g_Login.connectedAs.clear();
    g_Login.errorMsg.clear();
    g_scannerPrevClose.clear();
    g_scannerVolume.clear();
    g_tickerSymbols.clear();
    DestroyTradingWindows();
    printf("[IB] Disconnected.\n");
}

// ============================================================================
// Login Window
// ============================================================================
static void RenderLoginWindow() {
    ImGuiViewport* vp  = ImGui::GetMainViewport();
    const float    W   = vp->Size.x;
    const float    H   = vp->Size.y - kTitleBarH;
    const float    leftW  = W * 0.42f;
    const float    rightW = W - leftW;

    // ── Full-screen host window (below custom title bar) ──────────────────────
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kTitleBarH));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.051f, 0.067f, 0.090f, 1.0f));

    ImGui::Begin("##login", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      wp  = ImGui::GetWindowPos();

    // ── Left panel background + decorations ──────────────────────────────────
    dl->AddRectFilled(wp, ImVec2(wp.x + leftW, wp.y + H), IM_COL32(7, 10, 16, 255));

    // Subtle grid overlay
    for (int i = 1; i < 9; i++) {
        float x = wp.x + leftW * (i / 9.0f);
        dl->AddLine(ImVec2(x, wp.y), ImVec2(x, wp.y + H), IM_COL32(0, 180, 216, 7));
    }
    for (int i = 1; i < 12; i++) {
        float y = wp.y + H * (i / 12.0f);
        dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + leftW, y), IM_COL32(0, 180, 216, 7));
    }

    // Top-left corner accent brackets
    const float bk = 28.0f, bkT = 2.0f, bkOff = 32.0f;
    dl->AddRectFilled(ImVec2(wp.x + bkOff,      wp.y + bkOff),
                      ImVec2(wp.x + bkOff + bk,  wp.y + bkOff + bkT), IM_COL32(0,180,216,180));
    dl->AddRectFilled(ImVec2(wp.x + bkOff,      wp.y + bkOff),
                      ImVec2(wp.x + bkOff + bkT, wp.y + bkOff + bk),  IM_COL32(0,180,216,180));
    // Bottom-right corner accent brackets (relative to left panel)
    dl->AddRectFilled(ImVec2(wp.x + leftW - bkOff - bk, wp.y + H - bkOff - bkT),
                      ImVec2(wp.x + leftW - bkOff,       wp.y + H - bkOff),       IM_COL32(0,180,216,180));
    dl->AddRectFilled(ImVec2(wp.x + leftW - bkOff - bkT, wp.y + H - bkOff - bk),
                      ImVec2(wp.x + leftW - bkOff,        wp.y + H - bkOff),      IM_COL32(0,180,216,180));

    // Thin cyan separator between panels
    dl->AddRectFilled(ImVec2(wp.x + leftW - 1, wp.y),
                      ImVec2(wp.x + leftW + 1, wp.y + H), IM_COL32(0, 180, 216, 70));

    // ── Left child: branding ──────────────────────────────────────────────────
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##lp", ImVec2(leftW, H), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* ldl  = ImGui::GetWindowDrawList();
    ImVec2      lwp  = ImGui::GetWindowPos();

    // Vertical cyan accent bar left of brand text
    const float brandTopY = H * 0.36f;
    ldl->AddRectFilled(ImVec2(lwp.x + 58.0f, lwp.y + brandTopY),
                       ImVec2(lwp.x + 62.0f, lwp.y + brandTopY + 72.0f),
                       IM_COL32(0, 180, 216, 220));

    // Large IBKR logotype
    ImGui::SetCursorPos(ImVec2(76.0f, brandTopY));
    ImGui::SetWindowFontScale(2.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.706f, 0.847f, 1.0f));
    ImGui::Text("IBKR");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);

    ImGui::SetCursorPos(ImVec2(78.0f, brandTopY + 42.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.902f, 0.929f, 0.953f, 0.92f));
    ImGui::Text("TRADING TERMINAL");
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(78.0f, brandTopY + 62.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("Professional Market Access");
    ImGui::PopStyleColor();

    // Horizontal rule below tagline
    float ruleY = lwp.y + brandTopY + 82.0f;
    ldl->AddLine(ImVec2(lwp.x + 78.0f, ruleY),
                 ImVec2(lwp.x + leftW * 0.72f, ruleY),
                 IM_COL32(0, 180, 216, 55), 1.0f);

    // Bottom attribution
    ImGui::SetCursorPos(ImVec2(78.0f, H - 32.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.30f, 0.35f, 1.0f));
    ImGui::Text("Interactive Brokers LLC");
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg

    // ── Right panel: covers the full right side ──────────────────────────────
    const float formW    = std::min(380.0f, rightW - 80.0f);
    const float estFormH = 355.0f;
    const float formX    = (rightW - formW) * 0.5f;
    const float formY    = std::max(50.0f, (H - estFormH) * 0.5f);

    ImGui::SetCursorPos(ImVec2(leftW, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.051f, 0.067f, 0.090f, 1.0f));
    ImGui::BeginChild("##rp", ImVec2(rightW, H), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Form inner child — explicitly positioned at the visual center
    ImGui::SetCursorPos(ImVec2(formX, formY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8.0f, 8.0f));
    ImGui::BeginChild("##form", ImVec2(formW, H - formY - 20.0f), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* fdl = ImGui::GetWindowDrawList();

    // Section title + cyan underline
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("CONNECT TO IBKR");
    ImGui::PopStyleColor();
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        fdl->AddLine(ImVec2(sp.x, sp.y), ImVec2(sp.x + formW, sp.y),
                     IM_COL32(0, 180, 216, 60), 1.0f);
    }
    ImGui::Spacing();
    ImGui::Spacing();

    bool isConnecting = (g_Login.state == ConnectionState::Connecting);
    if (isConnecting) ImGui::BeginDisabled();

    bool changedType = false;

    // Segmented Paper | Live toggle
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 8.0f));
    const float segW = (formW - 2.0f) * 0.5f;
    auto segBtn = [&](const char* label, bool selected, bool isLiveBtn) -> bool {
        ImVec4 bg, bgH, bgA, fg;
        if (selected && isLiveBtn) {
            bg = bgH = bgA = ImVec4(0.451f, 0.094f, 0.094f, 1.0f);
            fg = ImVec4(1.000f, 0.420f, 0.420f, 1.0f);
        } else if (selected) {
            bg = bgH = bgA = ImVec4(0.000f, 0.353f, 0.424f, 1.0f);
            fg = ImVec4(0.000f, 0.706f, 0.847f, 1.0f);
        } else {
            bg  = ImVec4(0.039f, 0.055f, 0.075f, 1.0f);
            bgH = ImVec4(0.094f, 0.129f, 0.176f, 1.0f);
            bgA = ImVec4(0.059f, 0.094f, 0.141f, 1.0f);
            fg  = ImVec4(0.420f, 0.471f, 0.522f, 1.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Button,        bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bgA);
        ImGui::PushStyleColor(ImGuiCol_Text,          fg);
        bool clicked = ImGui::Button(label, ImVec2(segW, 28.0f));
        ImGui::PopStyleColor(4);
        return clicked;
    };
    if (segBtn("  Paper  ", !g_Login.isLive, false)) { g_Login.isLive = false; changedType = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (segBtn("  Live   ",  g_Login.isLive, true))  { g_Login.isLive = true;  changedType = true; }
    ImGui::PopStyleVar(2);

    ImGui::Spacing();

    // API row
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("API");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 16.0f);
    int apiIdx = (int)g_Login.apiType;
    if (ImGui::RadioButton("TWS",        &apiIdx, 0)) { g_Login.apiType = ApiType::TWS;     changedType = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("IB Gateway", &apiIdx, 1)) { g_Login.apiType = ApiType::Gateway; changedType = true; }
    if (changedType) g_Login.UpdatePort();

    ImGui::Spacing();
    {
        ImVec2 sp = ImGui::GetCursorScreenPos();
        fdl->AddLine(ImVec2(sp.x, sp.y), ImVec2(sp.x + formW, sp.y),
                     IM_COL32(48, 55, 62, 255), 1.0f);
    }
    ImGui::Spacing();
    ImGui::Spacing();

    // HOST label + input
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("HOST");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##host", g_Login.host, sizeof(g_Login.host));
    ImGui::Spacing();

    // PORT + CLIENT ID side by side
    const float halfW = (formW - 12.0f) * 0.5f;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.420f, 0.471f, 0.522f, 1.0f));
    ImGui::Text("PORT");
    ImGui::SameLine(0.0f, halfW - ImGui::CalcTextSize("PORT").x + 12.0f);
    ImGui::Text("CLIENT ID");
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(halfW);
    ImGui::InputInt("##port", &g_Login.port, 0);
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(halfW);
    ImGui::InputInt("##cid", &g_Login.clientId, 1);

    if (isConnecting) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Spacing();

    // Info note
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.300f, 0.355f, 0.400f, 1.0f));
    ImGui::PushTextWrapPos(formW);
    ImGui::TextWrapped("IB Gateway or TWS must be running with API enabled. "
                       "Credentials are managed by TWS/Gateway.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Error banner
    if (g_Login.state == ConnectionState::Error) {
        ImVec2 bMin = ImGui::GetCursorScreenPos();
        ImVec2 bMax = ImVec2(bMin.x + formW, bMin.y + 28.0f);
        fdl->AddRectFilled(bMin, bMax, IM_COL32(110, 18, 18, 200), 3.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::Text("  ! %s", g_Login.errorMsg.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Connect button / progress bar
    ImGui::Spacing();
    if (isConnecting) {
        using namespace std::chrono;
        static auto s_connectStart = steady_clock::now();
        float t = std::fmod((float)duration_cast<milliseconds>(
            steady_clock::now() - s_connectStart).count() / 1500.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.0f, 0.706f, 0.847f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,       ImVec4(0.039f, 0.055f, 0.075f, 1.0f));
        ImGui::ProgressBar(t, ImVec2(-1, 36.0f), "Connecting...");
        ImGui::PopStyleColor(2);
    } else {
        bool live = g_Login.isLive;
        ImVec4 bC = live ? ImVec4(0.55f,0.12f,0.00f,1.0f) : ImVec4(0.00f,0.353f,0.424f,1.0f);
        ImVec4 bH = live ? ImVec4(0.75f,0.18f,0.00f,1.0f) : ImVec4(0.00f,0.471f,0.565f,1.0f);
        ImVec4 bA = live ? ImVec4(0.38f,0.08f,0.00f,1.0f) : ImVec4(0.00f,0.235f,0.282f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button,        bC);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bH);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  bA);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        const char* lbl = live ? "Connect  —  Live Account" : "Connect  —  Paper Account";
        if (ImGui::Button(lbl, ImVec2(-1, 38.0f))) StartConnect();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        if (live) {
            ImGui::Spacing();
            ImVec2 wMin = ImGui::GetCursorScreenPos();
            ImVec2 wMax = ImVec2(wMin.x + formW, wMin.y + 28.0f);
            fdl->AddRectFilled(wMin, wMax, IM_COL32(75, 38, 0, 180), 3.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.0f, 1.0f));
            ImGui::Text("  ! LIVE — real orders will be executed");
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);  // form WindowPadding + ItemSpacing

    ImGui::EndChild();
    ImGui::PopStyleColor(); // ChildBg (##rp)
    ImGui::PopStyleVar();   // ##rp WindowPadding

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding + WindowBorderSize

    // Poll IB messages while connecting (connection is async)
    if (g_Login.state == ConnectionState::Connecting && g_IBClient)
        g_IBClient->ProcessMessages();
}

// ============================================================================
// Window presets
// ============================================================================
static const core::WindowPreset kBuiltinPresets[] = {
    // name           chart       trading     news        scanner     portfolio   orders
    { "Trading Focus",{true,  1}, {true,  1}, {false, 0}, {false, 0}, {false, 0}, {true,  1} },
    { "Research",     {true,  1}, {false, 0}, {true,  1}, {true,  1}, {false, 0}, {false, 0} },
    { "Full Desk",    {true,  1}, {true,  1}, {true,  2}, {true,  2}, {true,  0}, {true,  1} },
};
static constexpr int kNumBuiltinPresets = static_cast<int>(
    sizeof(kBuiltinPresets) / sizeof(kBuiltinPresets[0]));

static void ApplyPreset(const core::WindowPreset& p) {
    // Apply to first instance of each multi-instance type
    if (!g_chartEntries.empty() && g_chartEntries[0].win) {
        g_chartEntries[0].win->open()       = p.chart.visible;
        g_chartEntries[0].win->setGroupId(p.chart.groupId);
    }
    if (!g_tradingEntries.empty() && g_tradingEntries[0].win) {
        g_tradingEntries[0].win->open()     = p.trading.visible;
        g_tradingEntries[0].win->setGroupId(p.trading.groupId);
    }
    if (!g_scannerEntries.empty() && g_scannerEntries[0].win) {
        g_scannerEntries[0].win->open()     = p.scanner.visible;
        g_scannerEntries[0].win->setGroupId(p.scanner.groupId);
    }
    if (!g_newsEntries.empty() && g_newsEntries[0].win) {
        g_newsEntries[0].win->open() = p.news.visible;
        g_newsEntries[0].win->setGroupId(p.news.groupId);
    }
    if (g_PortfolioWindow) { g_PortfolioWindow->open() = p.portfolio.visible; }
    if (g_OrdersWindow)    { g_OrdersWindow->open()    = p.orders.visible; }
    // Reset group state so the next symbol change re-broadcasts correctly
    for (auto& gs : g_groups) gs.symbol.clear();
}

// ============================================================================
// Trading UI (post-login)
// ============================================================================
// ============================================================================
// Window resize (borderless window — manual edge/corner drag)
// ============================================================================
static void HandleWindowResize() {
    // Drag/resize state needs to be visible early so we can let an in-progress
    // resize continue when the cursor crosses out of the main window.
    static bool  s_resizing = false;
    static int   s_edge     = 0;
    static float s_startMX  = 0, s_startMY = 0;
    static int   s_startWX  = 0, s_startWY = 0, s_startWW = 0, s_startWH = 0;

    // No resize while the title bar is being dragged, or when maximized
    if (g_tbDragging) return;
    if (glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED)) return;

    // ImGuiConfigFlags_ViewportsEnable lets ImGui detach windows into separate
    // OS-level GLFW windows. glfwGetCursorPos still returns coordinates
    // relative to the main window, so the edge math below would mis-fire when
    // a floating viewport sits over (or past) the main window's border —
    // dragging the floating window would resize the main window instead.
    // GLFW_HOVERED is false whenever another window is between the cursor
    // and the main window's content. Skip in that case, but let an
    // already-running resize finish even if the cursor leaves the window.
    const bool mainHovered = glfwGetWindowAttrib(g_AppWindow, GLFW_HOVERED) == GLFW_TRUE;
    if (!mainHovered && !s_resizing) {
        glfwSetCursor(g_AppWindow, nullptr); // clear any leftover resize cursor
        return;
    }

    constexpr int kEdge   = 5;  // px — edge-only detection zone
    constexpr int kCorner = 16; // px — corner detection zone (must be > kEdge)

    double mx, my;
    glfwGetCursorPos(g_AppWindow, &mx, &my);  // cursor relative to window client area

    int ww, wh;
    glfwGetWindowSize(g_AppWindow, &ww, &wh);

    // Edge flags (narrow zone)
    const bool nearL = (mx < kEdge);
    const bool nearR = (mx > ww - kEdge);
    const bool nearB = (my > wh - kEdge);
    // Top edge excluded — title bar owns it (would conflict with drag-to-move).

    // Corner flags (wider zone) — checked first so corners take priority over edges
    const bool cnrL = (mx < kCorner);
    const bool cnrR = (mx > ww - kCorner);
    const bool cnrB = (my > wh - kCorner);

    // bit0=Left, bit1=Right, bit3=Bottom — corners use the wider zone
    int edge = 0;
    if      (cnrR && cnrB) edge = 2 | 8; // BR
    else if (cnrL && cnrB) edge = 1 | 8; // BL
    else if (nearR)        edge = 2;      // right edge
    else if (nearL)        edge = 1;      // left edge
    else if (nearB)        edge = 8;      // bottom edge

    // Resize cursors — GLFW 3.4 names with 3.3 fallback
    static GLFWcursor* s_curEW = glfwCreateStandardCursor(
#ifdef GLFW_RESIZE_EW_CURSOR
        GLFW_RESIZE_EW_CURSOR
#else
        GLFW_HRESIZE_CURSOR
#endif
    );
    static GLFWcursor* s_curNS = glfwCreateStandardCursor(
#ifdef GLFW_RESIZE_NS_CURSOR
        GLFW_RESIZE_NS_CURSOR
#else
        GLFW_VRESIZE_CURSOR
#endif
    );
#ifdef GLFW_RESIZE_NWSE_CURSOR
    static GLFWcursor* s_curNWSE = glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR);
    static GLFWcursor* s_curNESW = glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR);
#else
    static GLFWcursor* s_curNWSE = s_curEW;
    static GLFWcursor* s_curNESW = s_curEW;
#endif

    // Update cursor shape when not already dragging
    if (!s_resizing) {
        GLFWcursor* cur = nullptr;
        switch (edge) {
            case 1: case 2:  cur = s_curEW;   break; // L / R
            case 8:          cur = s_curNS;   break; // B
            case 9:          cur = s_curNESW; break; // BL
            case 10:         cur = s_curNWSE; break; // BR
            default:         cur = nullptr;   break;
        }
        glfwSetCursor(g_AppWindow, cur);
    }

    // Resize math uses global mouse coords (io.MousePos = screen space).
    const ImGuiIO& io = ImGui::GetIO();
    const bool lmb         = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    // Only START a resize on the click frame — never on "LMB happens to be
    // held while the cursor entered the edge zone." Otherwise mid-drag of an
    // ImGui floating viewport toward a screen edge will cause the OS to clamp
    // the floating window, the cursor exits it onto the main app's edge zone,
    // and we'd flip into resize mode — shrinking the main window every frame
    // and pulling the docking drop-target rectangles out from under the cursor.
    const bool lmbClicked  = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    if (!lmb) {
        s_resizing = false;
        s_edge     = 0;
    }

    if (!s_resizing && lmbClicked && edge != 0) {
        s_resizing = true;
        s_edge     = edge;
        s_startMX  = io.MousePos.x;
        s_startMY  = io.MousePos.y;
        glfwGetWindowPos (g_AppWindow, &s_startWX, &s_startWY);
        glfwGetWindowSize(g_AppWindow, &s_startWW, &s_startWH);
    }

    if (s_resizing) {
        constexpr int kMinW = 640, kMinH = 400;
        const int dx = (int)(io.MousePos.x - s_startMX);
        const int dy = (int)(io.MousePos.y - s_startMY);

        int nx = s_startWX, ny = s_startWY;
        int nw = s_startWW, nh = s_startWH;

        if (s_edge & 2) nw = std::max(kMinW, s_startWW + dx);  // right
        if (s_edge & 8) nh = std::max(kMinH, s_startWH + dy);  // bottom
        if (s_edge & 1) {                                        // left
            nw = std::max(kMinW, s_startWW - dx);
            nx = s_startWX + (s_startWW - nw);
        }

        glfwSetWindowPos (g_AppWindow, nx, ny);
        glfwSetWindowSize(g_AppWindow, nw, nh);
    }
}

// ============================================================================
// Custom Title Bar
// ============================================================================
static void RenderCustomTitleBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuiIO&       io = ImGui::GetIO();

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kTitleBarH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.016f, 0.020f, 0.031f, 1.0f)); // #040508

    ImGui::Begin("##titlebar", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoScrollbar  | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2  wp = ImGui::GetWindowPos();
    float   W  = vp->Size.x;

    // Bottom border line
    dl->AddLine(ImVec2(wp.x, wp.y + kTitleBarH - 1.0f),
                ImVec2(wp.x + W, wp.y + kTitleBarH - 1.0f),
                IM_COL32(0, 180, 216, 50), 1.0f);

    // ── Left: branding ────────────────────────────────────────────────────────
    // Cyan accent bar
    dl->AddRectFilled(ImVec2(wp.x + 10.0f, wp.y + 9.0f),
                      ImVec2(wp.x + 13.0f, wp.y + kTitleBarH - 9.0f),
                      IM_COL32(0, 180, 216, 240));

    float textY = (kTitleBarH - ImGui::GetFontSize()) * 0.5f;
    ImGui::SetCursorPos(ImVec2(20.0f, textY));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.706f, 0.847f, 1.0f));
    ImGui::Text("IBKR");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetCursorPosY(textY);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.61f, 0.67f, 1.0f));
    ImGui::Text("TRADING TERMINAL");
    ImGui::PopStyleColor();

    // ── Right: window control buttons ────────────────────────────────────────
    const float btnW = 46.0f;
    const float btnH = kTitleBarH;
    float btnX = W - btnW * 3.0f;

    // Returns true if clicked; fills outHovered
    auto ctrlBtn = [&](const char* id, float x, bool isClose,
                       bool& outHovered) -> bool {
        ImGui::SetCursorPos(ImVec2(x, 0.0f));
        ImGui::InvisibleButton(id, ImVec2(btnW, btnH));
        outHovered       = ImGui::IsItemHovered();
        bool active      = ImGui::IsItemActive();
        bool clicked     = ImGui::IsItemClicked();
        ImU32 bg = 0;
        if      (active)       bg = isClose ? IM_COL32(180, 28, 28, 255)
                                            : IM_COL32(55, 68, 85, 255);
        else if (outHovered)   bg = isClose ? IM_COL32(198, 40, 40, 230)
                                            : IM_COL32(38, 50, 65, 255);
        if (bg)
            dl->AddRectFilled(ImVec2(wp.x + x, wp.y),
                              ImVec2(wp.x + x + btnW, wp.y + btnH), bg);
        return clicked;
    };

    bool hMin = false, hMax = false, hClose = false;

    // Minimize
    if (ctrlBtn("##min", btnX, false, hMin))
        glfwIconifyWindow(g_AppWindow);
    {
        ImU32 ic = hMin ? IM_COL32(220, 230, 240, 255) : IM_COL32(140, 155, 170, 190);
        float cx = wp.x + btnX + btnW * 0.5f;
        float cy = wp.y + btnH * 0.5f + 2.0f;
        dl->AddLine(ImVec2(cx - 5.0f, cy), ImVec2(cx + 5.0f, cy), ic, 1.5f);
    }

    // Maximize / restore
    btnX += btnW;
    if (ctrlBtn("##max", btnX, false, hMax)) {
        if (glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED))
            glfwRestoreWindow(g_AppWindow);
        else
            glfwMaximizeWindow(g_AppWindow);
    }
    {
        ImU32 ic = hMax ? IM_COL32(220, 230, 240, 255) : IM_COL32(140, 155, 170, 190);
        float cx = wp.x + btnX + btnW * 0.5f;
        float cy = wp.y + btnH * 0.5f;
        bool maximized = glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED);
        if (maximized) {
            // Restore icon: two overlapping squares
            dl->AddRect(ImVec2(cx - 3.0f, cy - 5.0f),
                        ImVec2(cx + 5.0f, cy + 3.0f), ic, 0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(cx - 5.0f, cy - 3.0f),
                        ImVec2(cx + 3.0f, cy + 5.0f), ic, 0.0f, 0, 1.0f);
        } else {
            // Maximize icon: single square
            dl->AddRect(ImVec2(cx - 5.0f, cy - 5.0f),
                        ImVec2(cx + 5.0f, cy + 5.0f), ic, 0.0f, 0, 1.0f);
        }
    }

    // Close
    btnX += btnW;
    if (ctrlBtn("##close", btnX, true, hClose))
        glfwSetWindowShouldClose(g_AppWindow, GLFW_TRUE);
    {
        ImU32 ic = hClose ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 155, 170, 190);
        float cx = wp.x + btnX + btnW * 0.5f;
        float cy = wp.y + btnH * 0.5f;
        dl->AddLine(ImVec2(cx - 5.0f, cy - 5.0f),
                    ImVec2(cx + 5.0f, cy + 5.0f), ic, 1.5f);
        dl->AddLine(ImVec2(cx + 5.0f, cy - 5.0f),
                    ImVec2(cx - 5.0f, cy + 5.0f), ic, 1.5f);
    }

    // ── Drag to move ─────────────────────────────────────────────────────────
    static float s_dragStartMX = 0, s_dragStartMY = 0;
    static int   s_dragStartWX = 0, s_dragStartWY = 0;

    bool overBtns = (io.MousePos.x >= wp.x + W - btnW * 3.0f);

    if (!overBtns && ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_tbDragging   = true;
        s_dragStartMX  = io.MousePos.x;
        s_dragStartMY  = io.MousePos.y;
        glfwGetWindowPos(g_AppWindow, &s_dragStartWX, &s_dragStartWY);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        g_tbDragging = false;
    if (g_tbDragging) {
        glfwSetWindowPos(g_AppWindow,
            s_dragStartWX + (int)(io.MousePos.x - s_dragStartMX),
            s_dragStartWY + (int)(io.MousePos.y - s_dragStartMY));
    }

    // Double-click to maximize / restore
    if (!overBtns && ImGui::IsWindowHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (glfwGetWindowAttrib(g_AppWindow, GLFW_MAXIMIZED))
            glfwRestoreWindow(g_AppWindow);
        else
            glfwMaximizeWindow(g_AppWindow);
    }

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
    ImGui::PopStyleVar(2);  // WindowPadding + WindowBorderSize
}

// ============================================================================
// Settings window
// ============================================================================
// Subscribe / unsubscribe from IB TWS display groups G1–G4 (reqIds 8061–8064).
static void SetTwsGroupSync(bool enable) {
    g_twsGroupSync = enable;
    if (!g_IBClient || !g_IBClient->IsConnected()) return;
    if (enable) {
        for (int g = 1; g <= 4; ++g)
            g_IBClient->SubscribeToGroupEvents(8060 + g, g);
    } else {
        for (int g = 1; g <= 4; ++g)
            g_IBClient->UnsubscribeFromGroupEvents(8060 + g);
    }
}

static void RenderSettingsWindow() {
    if (!g_settingsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(300, 180), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Settings", &g_settingsOpen,
            ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Font Size");
    ImGui::Spacing();

    static const char* kLabels[] = { "Small", "Medium", "Large" };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0, 16);
        if (ImGui::RadioButton(kLabels[i], (int)g_fontSize == i)) {
            g_fontSize = static_cast<FontSize>(i);
            ApplyAppPrefsToStyle();
            SaveAppPrefsFile();
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Default Trading Style (new charts)");
    ImGui::Spacing();

    {
        using TS = core::services::TradingStyle;
        static const TS kOrder[] = { TS::Scalping, TS::DayTrading,
                                     TS::Swing,    TS::Investment, TS::Free };
        int curIdx = 0;
        for (int i = 0; i < 5; ++i)
            if (kOrder[i] == g_defaultTradingStyle) { curIdx = i; break; }
        const char* curLabel = core::services::TradingStyleLabel(kOrder[curIdx]);
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("##default_trading_style", curLabel)) {
            for (int i = 0; i < 5; ++i) {
                bool sel = (i == curIdx);
                if (ImGui::Selectable(core::services::TradingStyleLabel(kOrder[i]), sel)) {
                    g_defaultTradingStyle = kOrder[i];
                    SaveAppPrefsFile();
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Applies only to newly-spawned chart windows.\n"
                "Existing charts keep their own style (saved per-chart).");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("TWS Integration");
    ImGui::Spacing();

    bool syncVal = g_twsGroupSync;
    if (ImGui::Checkbox("Sync with TWS Display Groups", &syncVal)) {
        SetTwsGroupSync(syncVal);
        SaveAppPrefsFile();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "When ON: this app's G1–G4 groups stay in sync with\n"
            "TWS linked-window groups 1–4 (bidirectional).\n"
            "Requires IB Gateway / TWS with API access enabled.");

    // ── Notifications ────────────────────────────────────────────────────────
    if (g_NotificationService) {
        ImGui::Spacing();
        ImGui::SeparatorText("Notifications");
        ImGui::Spacing();

        auto s = g_NotificationService->settings();
        bool dirty = false;

        if (ImGui::Checkbox("Enable notifications##n_master", &s.masterEnable)) dirty = true;

        ImGui::BeginDisabled(!s.masterEnable);

        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt("Volume##n_vol", &s.masterVolume, 0, 100, "%d"))
            dirty = true;

        ImGui::TextDisabled("Surfaces:");
        if (ImGui::Checkbox("Alert tones##n_tones",   &s.enableTones))   dirty = true;
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("Voice phrases##n_voice", &s.enableVoice))   dirty = true;
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("Visual toasts##n_toast", &s.enableToasts))  dirty = true;

        ImGui::TextDisabled("Categories:");
        if (ImGui::Checkbox("Order events##n_ord",   &s.enableOrders))     dirty = true;
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("Connection events##n_conn", &s.enableConnection)) dirty = true;
        ImGui::SameLine(0, 16);
        if (ImGui::Checkbox("Signal events##n_sig",  &s.enableSignals))    dirty = true;

        ImGui::Spacing();
        if (ImGui::Button("Test tone##n_t1")) {
            g_NotificationService->NotifyForce(
                core::services::NotificationSeverity::Info,
                core::services::NotificationCategory::System,
                core::services::NotificationEvent::Test,
                "Test tone", "",
                /*playTone=*/true, /*playVoice=*/false, /*showToast=*/false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Test voice##n_t2")) {
            g_NotificationService->NotifyForce(
                core::services::NotificationSeverity::Info,
                core::services::NotificationCategory::System,
                core::services::NotificationEvent::Test,
                "Test voice", "",
                /*playTone=*/false, /*playVoice=*/true, /*showToast=*/false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Test toast##n_t3")) {
            g_NotificationService->NotifyForce(
                core::services::NotificationSeverity::Info,
                core::services::NotificationCategory::System,
                core::services::NotificationEvent::Test,
                "Test", "Notifications working.",
                /*playTone=*/false, /*playVoice=*/false, /*showToast=*/true);
        }

        ImGui::EndDisabled();

        if (dirty) g_NotificationService->setSettings(s);
    }

    // ── News providers ──────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("News providers");
    ImGui::Spacing();

    if (g_newsProvidersList.empty()) {
        ImGui::TextDisabled(g_IBClient && g_IBClient->IsConnected()
            ? "Waiting for IB to return entitled providers…"
            : "Connect to IB to load entitled providers.");
    } else {
        ImGui::TextDisabled("%d entitled · %d enabled",
                            (int)g_newsProvidersList.size(),
                            (int)(g_newsProvidersList.size() - g_disabledNewsProviders.size()));
        ImGui::SameLine(0, 16);
        if (ImGui::SmallButton("All##news_all")) {
            g_disabledNewsProviders.clear();
            RebuildEntitledNewsProviders();
            SaveDisabledNewsProviders();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("None##news_none")) {
            for (const auto& [code, _] : g_newsProvidersList)
                g_disabledNewsProviders.insert(code);
            RebuildEntitledNewsProviders();
            SaveDisabledNewsProviders();
        }

        ImGui::BeginChild("##news_provider_list",
                          ImVec2(0, 160), ImGuiChildFlags_Borders);
        for (const auto& [code, name] : g_newsProvidersList) {
            bool enabled = g_disabledNewsProviders.count(code) == 0;
            char label[256];
            std::snprintf(label, sizeof(label), "%s — %s##np_%s",
                          code.c_str(), name.c_str(), code.c_str());
            if (ImGui::Checkbox(label, &enabled)) {
                if (enabled) g_disabledNewsProviders.erase(code);
                else         g_disabledNewsProviders.insert(code);
                RebuildEntitledNewsProviders();
                SaveDisabledNewsProviders();
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

// ============================================================================
// Trading UI
// ============================================================================
static void RenderTradingUI() {
    fprintf(stderr, "[main] RenderTradingUI enter\n"); fflush(stderr);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kTitleBarH));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - kTitleBarH));
    ImGui::SetNextWindowBgAlpha(0.0f);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;

    // Taller menu bar via FramePadding — scale with font size so Large mode doesn't clip
    const float kMenuBarScale = ImGui::GetFontSize() / 13.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(6.0f * kMenuBarScale, 9.0f * kMenuBarScale));
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.020f, 0.027f, 0.039f, 1.0f)); // #050709

    if (ImGui::Begin("##TradingHost", nullptr, hostFlags)) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Disconnect")) Disconnect();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(g_AppWindow, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                if (ImGui::BeginMenu("IBKR")) {
                    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
                    // Per-instance chart windows
                    for (auto& ce : g_chartEntries) {
                        if (!ce.win) continue;
                        char lbl[64];
                        const std::string sym = ce.win->getSymbol();
                        const int gid = ce.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Chart %s %s###chart_menu_%d",
                            sym.empty() ? "--" : sym.c_str(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            ce.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &ce.win->open());
                    }
                    if ((int)g_chartEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Chart"))
                            SpawnChartWindow((int)g_chartEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance trading windows
                    for (auto& te : g_tradingEntries) {
                        if (!te.win) continue;
                        char lbl[64];
                        const std::string sym = te.win->getSymbol();
                        const int gid = te.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Order Book %s %s###ob_menu_%d",
                            sym.empty() ? "--" : sym.c_str(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            te.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &te.win->open());
                    }
                    if ((int)g_tradingEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Order Book"))
                            SpawnTradingWindow((int)g_tradingEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance scanner windows
                    for (auto& se : g_scannerEntries) {
                        if (!se.win) continue;
                        char lbl[64];
                        const int gid = se.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Scanner %s %s###sc_menu_%d",
                            se.win->getPresetLabel(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            se.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &se.win->open());
                    }
                    if ((int)g_scannerEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Scanner"))
                            SpawnScannerWindow((int)g_scannerEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance news windows
                    for (auto& ne : g_newsEntries) {
                        if (!ne.win) continue;
                        char lbl[64];
                        const char* sym = ne.win->getSymbol();
                        const int gid = ne.win->groupId();
                        char grp[8];
                        if (gid > 0) std::snprintf(grp, sizeof(grp), "G%d", gid);
                        else         std::strncpy(grp, "G-", sizeof(grp));
                        std::snprintf(lbl, sizeof(lbl), "News %s %s###news_menu_%d",
                            sym[0] == '\0' ? "--" : sym, grp, ne.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &ne.win->open());
                    }
                    if ((int)g_newsEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New News"))
                            SpawnNewsWindow((int)g_newsEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance watchlist windows
                    for (auto& we : g_watchlistEntries) {
                        if (!we.win) continue;
                        char lbl[64];
                        const int gid = we.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Watchlist %d %s###wl_menu_%d",
                            we.win->instanceId(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            we.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &we.win->open());
                    }
                    if ((int)g_watchlistEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Watchlist"))
                            SpawnWatchlistWindow((int)g_watchlistEntries.size());
                    }
                    ImGui::Separator();
                    // Per-instance replay windows
                    for (auto& re : g_replayEntries) {
                        if (!re.win) continue;
                        char lbl[64];
                        const std::string sym = re.win->getSymbol();
                        const int gid = re.win->groupId();
                        std::snprintf(lbl, sizeof(lbl), "Replay %s %s###replay_menu_%d",
                            sym.empty() ? "--" : sym.c_str(),
                            gid > 0 ? ("G" + std::to_string(gid)).c_str() : "G-",
                            re.win->instanceId());
                        ImGui::MenuItem(lbl, nullptr, &re.win->open());
                    }
                    if ((int)g_replayEntries.size() < kMaxMultiWin) {
                        if (ImGui::MenuItem("+ New Replay"))
                            SpawnReplayWindow((int)g_replayEntries.size());
                    }
                    ImGui::Separator();
                    // Singleton windows
                    if (g_OrdersWindow)       ImGui::MenuItem("Orders",        nullptr, &g_OrdersWindow->open());
                    if (g_PortfolioWindow)    ImGui::MenuItem("Portfolio",     nullptr, &g_PortfolioWindow->open());
                    if (g_WshCalendarWindow)  ImGui::MenuItem("WSH Calendar",  nullptr, &g_WshCalendarWindow->open());
                    if (g_NotificationsWindow) ImGui::MenuItem("Notifications", nullptr, &g_NotificationsWindow->open());
                    ImGui::PopItemFlag();
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Presets")) {
                for (int i = 0; i < kNumBuiltinPresets; i++) {
                    if (ImGui::MenuItem(kBuiltinPresets[i].name))
                        ApplyPreset(kBuiltinPresets[i]);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Settings"))
                g_settingsOpen = !g_settingsOpen;

            // Status indicator (right-aligned): [acct v]  DISCONNECTED  [LIVE]/[PAPER]
            const std::string& who = g_Login.connectedAs;
            bool lostConn = (g_Login.state == ConnectionState::LostConnection);
            static const char* kDiscLabel = " DISCONNECTED ";
            float discW = lostConn ? ImGui::CalcTextSize(kDiscLabel).x + 8.0f : 0.0f;
            float whoW  = ImGui::CalcTextSize(who.c_str()).x;

            // Account label/selector width
            std::string acctMenuLabel;
            float acctW = 0.0f;
            if (!g_selectedAccount.empty()) {
                if (g_managedAccounts.size() > 1)
                    acctMenuLabel = g_selectedAccount + " v";
                else
                    acctMenuLabel = g_selectedAccount;
                acctW = ImGui::CalcTextSize(acctMenuLabel.c_str()).x + 10.0f;
            }

            float totalR = discW + (acctW > 0 ? acctW + 6.0f : 0.0f) + whoW + 12.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - totalR);

            if (lostConn) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImVec2 sz = ImGui::CalcTextSize(kDiscLabel);
                float pad = 4.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(
                    ImVec2(p.x - pad, p.y - 1),
                    ImVec2(p.x + sz.x + pad, p.y + sz.y + 1),
                    IM_COL32(180, 60, 0, 220), 3.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
                ImGui::Text("%s", kDiscLabel);
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            if (!g_selectedAccount.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
                if (g_managedAccounts.size() > 1) {
                    if (ImGui::BeginMenu(acctMenuLabel.c_str())) {
                        for (const auto& acct : g_managedAccounts) {
                            bool sel = (acct == g_selectedAccount);
                            if (ImGui::MenuItem(acct.c_str(), nullptr, sel) && !sel) {
                                g_selectedAccount = acct;
                                if (g_IBClient) {
                                    g_IBClient->ReqAccountUpdates(false, "");
                                    g_IBClient->ReqAccountUpdates(true, g_selectedAccount);
                                    if (g_pnlSubscribed) {
                                        g_IBClient->CancelPnL(9000);
                                        g_pnlSubscribed = false;
                                    }
                                }
                            }
                        }
                        ImGui::EndMenu();
                    }
                } else {
                    ImGui::TextUnformatted(acctMenuLabel.c_str());
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            ImGui::PushStyleColor(ImGuiCol_Text,
                g_Login.isLive ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                               : ImVec4(0.4f, 1.0f, 0.5f, 1.0f));
            ImGui::Text("%s", who.c_str());
            ImGui::PopStyleColor();

            ImGui::EndMenuBar();

            // Cyan accent line at the bottom of the menu bar
            {
                ImVec2 cs = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(ImGui::GetWindowPos().x, cs.y),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), cs.y),
                    IM_COL32(0, 180, 216, 80), 1.0f);
            }
        }
        ImGui::PopStyleVar();   // FramePadding

        ImGuiID dockId = ImGui::GetID("TradingDock");
        ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
    }
    ImGui::PopStyleColor(); // MenuBarBg
    ImGui::End();

    // Drain at most one trading-style switch per second so a bulk-mode-change
    // doesn't blow past IB's per-contract pacing limit.
    DrainStyleSwitchQueue();

    // Once-per-second flush of chart-modes.cfg if any switch happened.
    if (g_chartModesDirty) {
        double now = glfwGetTime();
        if (now - g_lastChartModesSave > 1.0) {
            SaveChartModesFile();
            g_lastChartModesSave = now;
            g_chartModesDirty    = false;
        }
    }

    // Once-per-second flush of replay-windows.cfg
    if (g_replayWindowsDirty) {
        double now = glfwGetTime();
        if (now - g_lastReplayWindowsSave > 1.0) {
            SaveReplayWindowsFile();
            g_lastReplayWindowsSave = now;
        }
    }

    // Once-per-second flush of chart-settings.cfg. Internally hash-diff'd
    // against the last write so unchanged frames don't touch disk.
    {
        static double s_lastChartSettingsSave = 0.0;
        double now = glfwGetTime();
        if (now - s_lastChartSettingsSave > 1.0) {
            SaveChartSettingsFile();
            s_lastChartSettingsSave = now;
        }
    }

    // Once-per-second flush of trading-settings.cfg (hash-diff'd).
    {
        static double s_lastTradingSettingsSave = 0.0;
        double now = glfwGetTime();
        if (now - s_lastTradingSettingsSave > 1.0) {
            SaveTradingSettingsFile();
            s_lastTradingSettingsSave = now;
        }
    }

    // Once-per-second flush of scanner-settings.cfg (hash-diff'd).
    {
        static double s_lastScannerSettingsSave = 0.0;
        double now = glfwGetTime();
        if (now - s_lastScannerSettingsSave > 1.0) {
            SaveScannerSettingsFile();
            s_lastScannerSettingsSave = now;
        }
    }

    // Once-per-second flush of singleton-settings.cfg (hash-diff'd).
    {
        static double s_lastSingletonSettingsSave = 0.0;
        double now = glfwGetTime();
        if (now - s_lastSingletonSettingsSave > 1.0) {
            SaveSingletonSettingsFile();
            s_lastSingletonSettingsSave = now;
        }
    }

    // Push the unguarded-position warning hints once per frame using each
    // chart's freshly-detected S/R. Cheap (positions × charts is small).
    PushUnguardedHintsToWindows();

    // Dispatch any due voice/tone plays (delayed-voice scheduling).
    if (g_NotificationService) g_NotificationService->Tick();

    // Render all window instances
    for (auto& ce : g_chartEntries)   if (ce.win) ce.win->Render();
    for (auto& te : g_tradingEntries) if (te.win) te.win->Render();
    for (auto& se : g_scannerEntries) if (se.win) se.win->Render();
    for (auto& ne : g_newsEntries)    if (ne.win) ne.win->Render();
    for (auto& we : g_watchlistEntries) if (we.win) we.win->Render();
    for (auto& re : g_replayEntries)    if (re.win) re.win->Render();
    if (g_PortfolioWindow)   g_PortfolioWindow->Render();
    if (g_OrdersWindow)      g_OrdersWindow->Render();
    if (g_WshCalendarWindow) g_WshCalendarWindow->Render();
    if (g_NotificationsWindow && g_NotificationService)
        g_NotificationsWindow->Render(*g_NotificationService);
    RenderSettingsWindow();
}

// ============================================================================
// Top-level UI dispatcher
// ============================================================================
static void RenderAccountSelectorUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    ImGui::Begin("Select Account##acctsel",
                 nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("Multiple accounts found. Select one to continue:");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    for (const auto& acct : g_managedAccounts) {
        if (ImGui::Button(acct.c_str(), ImVec2(-1, 0))) {
            g_selectedAccount = acct;
            FinishConnect(g_pendingReconnect);
        }
    }
    ImGui::End();
}

static void RenderMainUI() {
    if (g_Login.state == ConnectionState::Connected ||
        g_Login.state == ConnectionState::LostConnection)
        RenderTradingUI();
    else if (g_Login.state == ConnectionState::SelectingAccount)
        RenderAccountSelectorUI();
    else
        RenderLoginWindow();

    // Toast overlay sits above everything but the title bar so the × close
    // button on the title bar still wins for hit-testing.
    if (g_NotificationService)
        ui::RenderNotificationOverlay(*g_NotificationService);

    RenderCustomTitleBar(); // always last so it renders on top of everything
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    std::cout << "============================================\n"
              << "Interactive Brokers Trading Application\n"
              << "Version: 1.0.0   Build: " << __DATE__ << " " << __TIME__ << "\n"
              << "============================================\n";

#if defined(__APPLE__)
    // macOS has no native Vulkan. We ship MoltenVK + the Vulkan loader next
    // to the binary (see CMakeLists.txt APPLE block) and point the loader at
    // our bundled ICD JSON via VK_ICD_FILENAMES *before* any Vulkan call.
    // VK_DRIVER_FILES is the newer (1.3.207+) loader env var; setting both
    // covers the entire range of loader versions a brew/SDK install might
    // bring along. Resolves the "Vulkan not supported" failure on stock
    // macOS without requiring the user to install the LunarG SDK.
    {
        std::filesystem::path exeDir;
        try {
            uint32_t bufSize = 0;
            _NSGetExecutablePath(nullptr, &bufSize);
            std::string buf(bufSize, '\0');
            if (_NSGetExecutablePath(buf.data(), &bufSize) == 0) {
                std::error_code ec;
                exeDir = std::filesystem::canonical(buf, ec).parent_path();
            }
        } catch (...) {}
        if (!exeDir.empty()) {
            std::error_code ec;
            auto icd = exeDir / "vulkan" / "icd.d" / "MoltenVK_icd.json";
            if (std::filesystem::exists(icd, ec)) {
                std::string s = icd.string();
                setenv("VK_ICD_FILENAMES", s.c_str(), 1);
                setenv("VK_DRIVER_FILES",  s.c_str(), 1);
            } else {
                fprintf(stderr,
                    "[vulkan] bundled MoltenVK_icd.json not found at %s — "
                    "falling back to system Vulkan ICD search\n",
                    icd.string().c_str());
            }
        }
    }
#endif

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) { std::cerr << "Failed to initialise GLFW\n"; return 1; }
    if (!glfwVulkanSupported()) {
        std::cerr << "Vulkan not supported\n"; glfwTerminate(); return 1;
    }

    ImVector<const char*> extensions;
    uint32_t ext_count = 0;
    const char** glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    for (uint32_t i = 0; i < ext_count; i++) extensions.push_back(glfw_exts[i]);
    SetupVulkan(extensions);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED,  GLFW_FALSE); // custom title bar replaces OS decoration
    g_AppWindow = glfwCreateWindow(1920, 1080, "IBKR Trading Terminal", nullptr, nullptr);
    if (!g_AppWindow) {
        std::cerr << "Failed to create window\n";
        CleanupVulkan(); glfwTerminate(); return 1;
    }

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, g_AppWindow, g_Allocator, &surface);
    check_vk_result(err);

    int fb_w, fb_h;
    glfwGetFramebufferSize(g_AppWindow, &fb_w, &fb_h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, fb_w, fb_h);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Point ImGui's auto-persisted settings at our config dir so window
    // positions, sizes, dock arrangement, splitter regions, and table column
    // widths survive across launches regardless of where the binary is invoked
    // from. ImGui defaults to "imgui.ini" in CWD, which is unstable (terminal
    // launch != desktop launch). The pointer must outlive ImGui — keep it
    // static so it lives for the duration of main().
    static const std::string g_imguiIniPath =
        core::services::ConfigFilePath("imgui.ini");
    if (!g_imguiIniPath.empty())
        io.IniFilename = g_imguiIniPath.c_str();

    // Font sizes are applied via FontGlobalScale at runtime (no atlas rebuild needed).
    // We load a TTF with extended Unicode coverage so IB-returned strings
    // (e.g. "BRFG — Briefing.com General Market Columns") and our own UI
    // strings (em dash, bullet, arrows, multiplication ×, sigma σ, ±) render
    // properly instead of '?' fallback glyphs from ProggyClean.
    {
        // Glyph range: ASCII + Latin-1 + General Punctuation + Arrows
        // + Mathematical Operators + Greek (for σ). Static so the array
        // outlives the AddFontFromFileTTF call (ImGui doesn't copy it).
        static const ImWchar kRanges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0x0370, 0x03FF,   // Greek (σ, µ, …)
            0x2010, 0x205F,   // General Punctuation (em/en dash, bullets, quotes, ellipsis)
            0x2190, 0x21FF,   // Arrows
            0x2200, 0x22FF,   // Mathematical Operators (×, ±, ≥, ≤, …)
            0,
        };

        // Try several common system TTFs in priority order; fall back to
        // ImGui's bundled ProggyClean if none are present (renders '?' for
        // out-of-ASCII glyphs but keeps the app launching on minimal systems).
        const char* kCandidates[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/Library/Fonts/Arial.ttf",                                 // macOS
            "C:\\Windows\\Fonts\\segoeui.ttf",                          // Windows
        };
        ImFont* loaded = nullptr;
        for (const char* path : kCandidates) {
            if (std::filesystem::exists(path)) {
                loaded = io.Fonts->AddFontFromFileTTF(path, 14.0f, nullptr, kRanges);
                if (loaded) {
                    fprintf(stderr, "[font] loaded %s\n", path);
                    break;
                }
            }
        }
        if (!loaded) {
            fprintf(stderr, "[font] no TTF candidate found; non-ASCII glyphs will render as '?'\n");
            io.Fonts->AddFontDefault();
        }
    }

    // ── Terminal Dark theme ───────────────────────────────────────────────────
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& s = ImGui::GetStyle();

        // Geometry
        s.WindowRounding    = 4.0f;
        s.ChildRounding     = 4.0f;
        s.FrameRounding     = 2.0f;
        s.PopupRounding     = 4.0f;
        s.ScrollbarRounding = 2.0f;
        s.GrabRounding      = 2.0f;
        s.TabRounding       = 2.0f;
        s.WindowBorderSize  = 1.0f;
        s.FrameBorderSize   = 0.0f;
        s.PopupBorderSize   = 1.0f;
        s.WindowPadding     = ImVec2(10.0f, 8.0f);
        s.FramePadding      = ImVec2(6.0f, 3.0f);
        s.ItemSpacing       = ImVec2(6.0f, 4.0f);
        s.ItemInnerSpacing  = ImVec2(4.0f, 4.0f);
        s.ScrollbarSize     = 10.0f;
        s.GrabMinSize       = 8.0f;
        s.IndentSpacing     = 14.0f;

        // Palette
        //   bg0  = near-black window bg
        //   bg1  = panel / child bg (slightly lighter)
        //   bg2  = frame / input bg
        //   fg0  = primary text
        //   fg1  = dimmed text
        //   acc  = cyan accent
        //   bdr  = border
        ImVec4* c = s.Colors;

        c[ImGuiCol_WindowBg]             = ImVec4(0.051f, 0.067f, 0.090f, 1.000f); // #0D1117
        c[ImGuiCol_ChildBg]              = ImVec4(0.086f, 0.106f, 0.141f, 1.000f); // #161B24
        c[ImGuiCol_PopupBg]              = ImVec4(0.063f, 0.082f, 0.110f, 1.000f); // #10151C
        c[ImGuiCol_Border]               = ImVec4(0.188f, 0.216f, 0.243f, 1.000f); // #30373E
        c[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);

        c[ImGuiCol_FrameBg]              = ImVec4(0.039f, 0.055f, 0.075f, 1.000f); // #0A0E13
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.094f, 0.129f, 0.176f, 1.000f);
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.059f, 0.094f, 0.141f, 1.000f);

        c[ImGuiCol_TitleBg]              = ImVec4(0.039f, 0.055f, 0.078f, 1.000f); // #0A0E14
        c[ImGuiCol_TitleBgActive]        = ImVec4(0.000f, 0.176f, 0.243f, 1.000f); // #002D3E (cyan-dark)
        c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.039f, 0.055f, 0.078f, 0.800f);
        c[ImGuiCol_MenuBarBg]            = ImVec4(0.027f, 0.039f, 0.055f, 1.000f); // #070A0E

        c[ImGuiCol_ScrollbarBg]          = ImVec4(0.039f, 0.055f, 0.075f, 1.000f);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.188f, 0.216f, 0.243f, 1.000f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.000f, 0.706f, 0.847f, 0.600f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        // Cyan accent: #00B4D8
        c[ImGuiCol_CheckMark]            = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.000f, 0.600f, 0.720f, 1.000f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        c[ImGuiCol_Button]               = ImVec4(0.000f, 0.353f, 0.424f, 1.000f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.000f, 0.471f, 0.565f, 1.000f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.000f, 0.235f, 0.282f, 1.000f);

        c[ImGuiCol_Header]               = ImVec4(0.000f, 0.353f, 0.424f, 0.700f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.000f, 0.471f, 0.565f, 0.800f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        c[ImGuiCol_Separator]            = ImVec4(0.188f, 0.216f, 0.243f, 1.000f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.000f, 0.706f, 0.847f, 0.600f);
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);

        c[ImGuiCol_ResizeGrip]           = ImVec4(0.000f, 0.353f, 0.424f, 0.400f);
        c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.000f, 0.706f, 0.847f, 0.600f);
        c[ImGuiCol_ResizeGripActive]     = ImVec4(0.000f, 0.706f, 0.847f, 0.900f);

        c[ImGuiCol_Tab]                  = ImVec4(0.051f, 0.082f, 0.118f, 1.000f);
        c[ImGuiCol_TabHovered]           = ImVec4(0.000f, 0.471f, 0.565f, 1.000f);
        c[ImGuiCol_TabSelected]          = ImVec4(0.000f, 0.353f, 0.424f, 1.000f);
        c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_TabDimmed]            = ImVec4(0.039f, 0.055f, 0.078f, 1.000f);
        c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.051f, 0.082f, 0.118f, 1.000f);
        c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.000f, 0.353f, 0.424f, 1.000f);

        c[ImGuiCol_DockingPreview]       = ImVec4(0.000f, 0.706f, 0.847f, 0.400f);
        c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.027f, 0.039f, 0.055f, 1.000f);

        c[ImGuiCol_PlotLines]            = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_PlotLinesHovered]     = ImVec4(1.000f, 0.600f, 0.000f, 1.000f);
        c[ImGuiCol_PlotHistogram]        = ImVec4(0.000f, 0.600f, 0.720f, 1.000f);
        c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.000f, 0.600f, 0.000f, 1.000f);

        c[ImGuiCol_TableHeaderBg]        = ImVec4(0.027f, 0.043f, 0.063f, 1.000f);
        c[ImGuiCol_TableBorderStrong]    = ImVec4(0.188f, 0.216f, 0.243f, 1.000f);
        c[ImGuiCol_TableBorderLight]     = ImVec4(0.102f, 0.122f, 0.153f, 1.000f);
        c[ImGuiCol_TableRowBg]           = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
        c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.000f, 1.000f, 1.000f, 0.030f);

        c[ImGuiCol_TextLink]             = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_TextSelectedBg]       = ImVec4(0.000f, 0.706f, 0.847f, 0.300f);

        c[ImGuiCol_DragDropTarget]       = ImVec4(1.000f, 0.600f, 0.000f, 0.900f);
        c[ImGuiCol_NavCursor]            = ImVec4(0.000f, 0.706f, 0.847f, 1.000f);
        c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.000f, 1.000f, 1.000f, 0.700f);
        c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.800f, 0.800f, 0.800f, 0.200f);
        c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.000f, 0.000f, 0.000f, 0.600f);

        c[ImGuiCol_Text]                 = ImVec4(0.902f, 0.929f, 0.953f, 1.000f); // #E6EDF3
        c[ImGuiCol_TextDisabled]         = ImVec4(0.420f, 0.471f, 0.522f, 1.000f); // #6B7885
    }
    g_baseStyle = ImGui::GetStyle(); // snapshot before any scaling

    // Restore persisted app-wide prefs (font size, default trading style for
    // new charts, TWS Display Group sync toggle) before the first frame
    // renders. Font scale applies immediately via ApplyAppPrefsToStyle.
    LoadAppPrefsFromFile();
    ApplyAppPrefsToStyle();

    ImGui_ImplGlfw_InitForVulkan(g_AppWindow, true);

    // Force ImGui's own hovered-viewport heuristic for docking drag-and-drop.
    // The GLFW backend sets ImGuiBackendFlags_HasMouseHoveredViewport and reports
    // the dragged viewport itself as hovered (because OS hit-testing sees the
    // floating window on top), which makes the dock-target highlight track the
    // wrong viewport — the blue marks shift away from the cursor and the window
    // snaps to the wrong dock node on release. The Win32 WndProc hook in the
    // GLFW backend doesn't fully fix this either, and on Linux GLFW < 3.4 has
    // no GLFW_MOUSE_PASSTHROUGH at all, so the misreport happens on every OS.
    // Clearing the flag makes ImGui ignore the backend value and call
    // FindHoveredViewportFromPlatformWindowStack(), which skips
    // ImGuiViewportFlags_NoInputs viewports and correctly returns the dock
    // target underneath the dragged window.
    io.BackendFlags &= ~ImGuiBackendFlags_HasMouseHoveredViewport;

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance              = g_Instance;
    init_info.PhysicalDevice        = g_PhysicalDevice;
    init_info.Device                = g_Device;
    init_info.QueueFamily           = g_QueueFamily;
    init_info.Queue                 = g_Queue;
    init_info.PipelineCache         = g_PipelineCache;
    init_info.DescriptorPool        = g_DescriptorPool;
    init_info.MinImageCount         = g_MinImageCount;
    init_info.ImageCount            = wd->ImageCount;
    init_info.Allocator             = g_Allocator;
    init_info.CheckVkResultFn       = check_vk_result;
    init_info.PipelineInfoMain.RenderPass  = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass     = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&init_info);

    // Notifications service. Audio init failure is non-fatal — the service
    // still queues toasts; PlayNow becomes a no-op when no audio device is
    // available (headless CI, Wayland-without-pulse, etc.).
    g_NotificationService = std::make_unique<core::services::NotificationService>();
    {
        // Cross-platform "where is my executable" lookup. Each branch returns
        // an absolute path to the running binary; we then take parent_path()
        // for the assets-dir search. Falls through to an empty path on
        // failure — the resolver below then only tries CWD.
        std::filesystem::path exeDir;
        try {
#if defined(_WIN32)
            wchar_t buf[32768] = {};
            DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
            if (n > 0 && n < 32768)
                exeDir = std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
            uint32_t bufSize = 0;
            _NSGetExecutablePath(nullptr, &bufSize);   // first call: size query
            std::string buf(bufSize, '\0');
            if (_NSGetExecutablePath(buf.data(), &bufSize) == 0) {
                std::error_code ec;
                exeDir = std::filesystem::canonical(buf, ec).parent_path();
            }
#else  // Linux, BSDs with procfs
            std::error_code ec;
            exeDir = std::filesystem::canonical("/proc/self/exe", ec).parent_path();
#endif
        } catch (...) {}

        // Asset directory resolution. Candidates ordered most-specific first:
        //   1. <exe>/assets/sounds      — shipped layout (binary + assets siblings)
        //   2. <exe>/../assets/sounds  — dev layout (build/ → repo)
        //   3. <cwd>/assets/sounds     — last-resort when exe-dir lookup failed
        std::vector<std::filesystem::path> candidates;
        if (!exeDir.empty()) {
            candidates.push_back(exeDir / "assets" / "sounds");
            candidates.push_back(exeDir / ".." / "assets" / "sounds");
        }
        try {
            candidates.push_back(std::filesystem::current_path() / "assets" / "sounds");
        } catch (...) {}

        bool resolved = false;
        for (const auto& c : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(c, ec)) {
                std::filesystem::path canon = std::filesystem::weakly_canonical(c, ec);
                std::string dir = ec ? c.string() : canon.string();
                g_NotificationService->SetAssetDir(dir);
                fprintf(stderr, "[notify] assets: %s\n", dir.c_str());
                resolved = true;
                break;
            }
        }
        if (!resolved)
            fprintf(stderr, "[notify] no assets/sounds dir found — audio disabled\n");
    }

    // History window — independent of IB connection lifetime so the user can
    // re-read events that fired before / during a disconnect.
    g_NotificationsWindow = new ui::NotificationsWindow();

    // Load saved news-provider disabled set so the filter is respected on
    // first connect (before IB even returns the entitled list).
    LoadDisabledNewsProviders();

    g_Login.UpdatePort();

    ImVec4 clear_color = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    printf("Application running. Close window to exit.\n");

    // ── Main loop ──────────────────────────────────────────────────────────
    while (!glfwWindowShouldClose(g_AppWindow)) {
        glfwPollEvents();

        // Drain IB message queue each frame (when connected)
        if (g_IBClient) g_IBClient->ProcessMessages();

        // Auto-reconnect when connection was lost unexpectedly
        if (g_Login.state == ConnectionState::LostConnection && !g_IBClient &&
            glfwGetTime() >= g_reconnectNextAttempt)
            StartSilentReconnect();

        int cur_w, cur_h;
        glfwGetFramebufferSize(g_AppWindow, &cur_w, &cur_h);
        if (cur_w > 0 && cur_h > 0 &&
            (g_SwapChainRebuild || wd->Width != cur_w || wd->Height != cur_h)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(
                g_Instance, g_PhysicalDevice, g_Device, wd,
                g_QueueFamily, g_Allocator, cur_w, cur_h, g_MinImageCount, 0);
            wd->FrameIndex     = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(g_AppWindow, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        HandleWindowResize(); // override cursor after ImGui sets it
        ImGui::NewFrame();

        RenderMainUI();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool minimized = (draw_data->DisplaySize.x <= 0.0f ||
                                 draw_data->DisplaySize.y <= 0.0f);

        wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
        wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
        wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
        wd->ClearValue.color.float32[3] = clear_color.w;

        if (!minimized) FrameRender(wd, draw_data);

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        if (!minimized) FramePresent(wd);
    }

    // Cleanup
    if (g_IBClient) {
        CancelAllSubscriptions();   // flush per-instance cancels before socket close
        g_IBClient->Disconnect();
        delete g_IBClient;
        g_IBClient = nullptr;
    }
    DestroyTradingWindows();

    err = vkDeviceWaitIdle(g_Device);
    check_vk_result(err);

    delete g_NotificationsWindow; g_NotificationsWindow = nullptr;
    g_NotificationService.reset();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupVulkanWindow(wd);
    CleanupVulkan();

    glfwDestroyWindow(g_AppWindow);
    glfwTerminate();

    std::cout << "Application terminated successfully.\n";
    return 0;
}
