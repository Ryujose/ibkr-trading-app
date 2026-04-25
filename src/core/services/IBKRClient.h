#pragma once

#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <variant>
#include <ctime>
#include <unordered_map>

// IB TWS API
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReaderOSSignal.h"
#include "EReader.h"

// Our domain models
#include "core/models/MarketData.h"
#include "core/models/NewsData.h"
#include "core/models/OrderData.h"
#include "core/models/ScannerData.h"
#include "core/models/PortfolioData.h"
#include "core/models/SmartRouting.h"

// Forward-declare IB API types we use only in the .cpp
struct Bar;
struct Contract;
struct ContractDetails;
struct Execution;
struct Order;
struct OrderCancel;
struct OrderState;
struct NewsProvider;
struct TickAttribLast;
struct TickAttribBidAsk;
struct WshEventData;

namespace core::services {

// ── Thread-safe message types (EReader thread → UI thread) ─────────────────

struct MsgConnection  { bool connected; std::string info; };
struct MsgBar         { int reqId; ::core::Bar bar; bool done; bool isLive; };
struct MsgTickPrice   { int tickerId; int field; double price; };
struct MsgTickSize    { int tickerId; int field; double size; };
struct MsgAccountVal  { std::string key, val, currency, account; };
struct MsgPosition    { ::core::Position pos; bool done; };
struct MsgPortfolio   { ::core::Position pos; };
struct MsgOrderStatus { int orderId; ::core::OrderStatus status;
                        double filled; double avgPrice; };
struct MsgFill        { ::core::Fill fill; bool fromQuery = false; };
struct MsgDepth       { int id; bool isBid; int pos; int op;
                        double price; double size; };
struct MsgScanItem    { int reqId; ::core::ScanResult result; };
struct MsgScanEnd     { int reqId; };
struct MsgNews        { std::time_t ts; std::string provider;
                        std::string articleId; std::string headline; };
struct MsgError       { int reqId; int code; std::string msg; };
struct MsgNextOrderId { int orderId; };
struct MsgOpenOrder      { ::core::Order order; };
struct MsgOpenOrderEnd   {};
struct MsgContractConId  { int reqId; long conId;
                           std::string description, secType, primaryExch, currency; };
struct MsgHistoricalNews { int reqId; std::time_t ts; std::string provider;
                           std::string articleId; std::string headline; };
struct MsgHistoricalNewsEnd { int reqId; };
struct MsgNewsArticle    { int reqId; std::string text; };
struct MsgAcctSummary    { std::string tag; std::string value; std::string currency; };
struct MsgPnL       { int reqId; double daily, unrealized, realized; };
struct MsgPnLSingle { int reqId; double daily, unrealized, realized, value; };
struct MsgManagedAccts   { std::vector<std::string> accounts; };
struct MsgPositionMulti  { int reqId; std::string account, modelCode;
                           ::core::Position pos; bool done; };
struct MsgAccountUpdateMulti { int reqId; std::string account, modelCode,
                                key, val, currency; bool done; };

// Symbol-search result (subset of IB ContractDescription)
struct ContractDesc {
    std::string symbol;
    std::string secType;
    std::string primaryExch;
    std::string currency;
};
struct MsgSymbolSamples { int reqId; std::vector<ContractDesc> results; };

// WSH (Wall Street Horizon) corporate event — one JSON blob per event
struct MsgWshEvent { int reqId; std::string data; };

// Tick-by-tick trade (AllLast / Last) — Time & Sales tape
struct MsgTickByTick {
    int         reqId;
    double      price;
    double      size;
    std::time_t time;
    std::string exchange;
    std::string specialConds;
    bool        isUptick  = true;
    bool        isNeutral = false;
};

// Tick request params — fires once per reqMktData subscription, delivers bboExchange.
struct MsgTickReqParams {
    int         tickerId;
    std::string bboExchange;
};

// Smart components: exchange routing destinations for a given bboExchange code.
// reqId range 8050–8059 (one per TradingWindow instance).
struct MsgSmartComponents {
    int                          reqId;
    std::vector<core::SmartRoute> routes;
};

// IB TWS Display Group sync (linked windows in TWS).
// queryDisplayGroups reqId: 8060; subscriptions G1–G4: 8061–8064.
struct MsgDisplayGroupList    { int reqId; std::string groups; };
// contractInfo format returned by IB: "symbol:secType:exchange:conId"
struct MsgDisplayGroupUpdated { int reqId; std::string contractInfo; };

using IBMessage = std::variant<
    MsgConnection, MsgBar, MsgTickPrice, MsgTickSize,
    MsgAccountVal, MsgPosition, MsgPortfolio, MsgOrderStatus,
    MsgFill, MsgDepth, MsgScanItem, MsgScanEnd, MsgNews,
    MsgError, MsgNextOrderId,
    MsgOpenOrder, MsgOpenOrderEnd,
    MsgContractConId, MsgHistoricalNews, MsgHistoricalNewsEnd, MsgNewsArticle,
    MsgAcctSummary, MsgPnL, MsgPnLSingle, MsgSymbolSamples,
    MsgManagedAccts, MsgPositionMulti, MsgAccountUpdateMulti,
    MsgTickByTick, MsgWshEvent,
    MsgTickReqParams, MsgSmartComponents,
    MsgDisplayGroupList, MsgDisplayGroupUpdated
>;

// ============================================================================
// IBKRClient
//   Bridges the IB C++ API (EWrapper/EClientSocket pattern) with our UI.
//   EWrapper callbacks run on the EReader thread and push messages into a
//   thread-safe queue.  Call ProcessMessages() once per UI frame to drain
//   the queue and deliver data to window callbacks (all on the UI thread).
// ============================================================================
class IBKRClient : public DefaultEWrapper {
public:
    IBKRClient();
    ~IBKRClient() override;

    // ── Connection ────────────────────────────────────────────────────────
    bool Connect(const std::string& host, int port, int clientId);
    void Disconnect();
    bool IsConnected() const;

    // ── Outgoing requests ─────────────────────────────────────────────────
    void ReqHistoricalData(int reqId, const std::string& symbol,
                           const std::string& duration    = "6 M",
                           const std::string& barSize     = "1 day",
                           bool               useRTH      = true,
                           const std::string& endDateTime = "");  // "" = now
    void CancelHistoricalData(int reqId);

    // Contract lookup (needed for reqHistoricalNews which takes conId, not symbol)
    void ReqContractDetails(int reqId, const std::string& symbol);

    // Historical news headlines for a specific contract.
    // providerCodes: colon-separated, e.g. "BRFUPDN:BRFG:DJ-N"
    //   empty → falls back to kFreeNewsProviders (all free providers)
    void ReqHistoricalNews(int reqId, int conId, int totalResults = 25,
                           const std::string& providerCodes = "");

    // Subscribe to real-time news ticks (fires tickNews / onNewsItem).
    // Uses "mdoff;292:PROVIDERS" so no market-data subscription is required.
    // Call once after connection; cancel with CancelMarketData(reqId).
    void SubscribeToNews(int reqId, const std::string& symbol = "AAPL");

    // News provider codes available without a paid subscription.
    // Used as default for both real-time and historical requests.
    static constexpr const char* kFreeNewsProviders =
        "BRFUPDN:BRFG:DJ-N:DJNL:DJ-RTA:DJ-RTE:DJ-RTG:DJ-RTPRO";

    // Full article body for an articleId returned by historicalNews / tickNews
    void ReqNewsArticle(int reqId, const std::string& providerCode,
                        const std::string& articleId);

    // Market data type:
    //   1 = Live (requires active subscription)
    //   2 = Frozen (last available real-time)
    //   3 = Delayed (15-20 min, free — use for paper accounts)
    //   4 = Delayed-Frozen
    void ReqMarketDataType(int type);

    // genericTickList: pass "" for paper/delayed mode (type 3/4) — the gateway
    // rejects ALL generic ticks for delayed data. Pass "165" for live mode to
    // receive 52-week hi/lo (fields 79/80). Standard price/volume ticks always arrive.
    void ReqMarketData(int reqId, const std::string& symbol,
                       const std::string& genericTickList = "");
    void CancelMarketData(int reqId);

    void ReqMktDepth(int reqId, const std::string& symbol, int numRows = 10);
    void CancelMktDepth(int reqId);

    void ReqAccountUpdates(bool subscribe, const std::string& acctCode = "");
    void ReqPositions();

    // Symbol autocomplete — fires onSymbolSamples with up to 16 matches.
    // IB matches both ticker prefix and company-name fragment.
    // reqId 8000 (cancel-before-reissue: just call again with the same reqId).
    void ReqMatchingSymbols(int reqId, const std::string& pattern);

    // Multi-account position and account-update subscriptions.
    // reqId: multi-positions 8030, multi-account-updates 8031–8040.
    void ReqPositionsMulti(int reqId, const std::string& account,
                           const std::string& modelCode = "");
    void CancelPositionsMulti(int reqId);
    void ReqAccountUpdatesMulti(int reqId, const std::string& account,
                                const std::string& modelCode = "",
                                bool ledgerAndNLV = false);
    void CancelAccountUpdatesMulti(int reqId);

    // Real-time P&L subscriptions (account-level and per-position).
    // reqPnL reqId: 9000. reqPnLSingle reqIds: 9001–9999.
    void ReqPnL(int reqId, const std::string& account, const std::string& modelCode = "");
    void CancelPnL(int reqId);
    void ReqPnLSingle(int reqId, const std::string& account, const std::string& modelCode, int conId);
    void CancelPnLSingle(int reqId);

    // Request account summary (reliable base-currency retrieval).
    // tags: comma-separated AccountSummaryTags, e.g. "Currency" or "Currency,NetLiquidation"
    void ReqAccountSummary(int reqId, const std::string& tags = "Currency");
    void CancelAccountSummary(int reqId);

    void ReqScannerData(int reqId,
                        const std::string& scanCode     = "TOP_PERC_GAIN",
                        const std::string& instrument   = "STK",
                        const std::string& locationCode = "STK.US.MAJOR");
    void CancelScannerData(int reqId);

    void PlaceOrder(const ::core::Order& order);
    void CancelOrder(int orderId);
    void ReqOpenOrders();
    // Returns all open orders across all client IDs (including previous sessions).
    void ReqAllOpenOrders();
    // Requests execution (fill) history for the current day. reqId 8001.
    // Empty filter = all executions → results arrive via onFillReceived.
    // Non-empty filter (symbol/side/dateFrom) → results arrive via onQueriedFill.
    // dateFrom format: "YYYYMMDD HH:MM:SS" or "YYYYMMDD" (IB ExecutionFilter.m_time).
    // WSH (Wall Street Horizon) corporate event calendar.
    // reqWshMetaData: describe available event types (reqId 8010).
    // reqWshEventData: fetch events for a conId (reqId range 8020–8029 per chart instance).
    void ReqWshMetaData(int reqId);
    void CancelWshMetaData(int reqId);
    void ReqWshEventData(int reqId, long conId, int totalLimit = 50);
    void CancelWshEventData(int reqId);

    // Tick-by-tick real-time tape (Time & Sales). tickType: "AllLast", "Last", "BidAsk", "MidPoint".
    // numberOfTicks=0 = continuous stream. ignoreSize=true = ignore odd-lot-only prints.
    // reqId range 130–139 (one per TradingWindow instance).
    void ReqTickByTickData(int reqId, const std::string& symbol,
                           const std::string& tickType = "AllLast",
                           int numberOfTicks = 0, bool ignoreSize = true);
    void CancelTickByTickData(int reqId);

    void ReqExecutions(int reqId, const std::string& symbol = "",
                       const std::string& side = "",
                       const std::string& dateFrom = "");

    // Smart components: exchange routing destinations for a bboExchange code.
    // bboExchange arrives via onTickReqParams after ReqMarketData.
    // reqId range 8050–8059 (one per TradingWindow instance).
    void ReqSmartComponents(int reqId, const std::string& bboExchange);

    // IB TWS Display Group sync (linked windows in TWS).
    // queryDisplayGroups reqId: 8060; subscriptions G1–G4: reqId 8061–8064.
    void QueryDisplayGroups(int reqId);
    void SubscribeToGroupEvents(int reqId, int groupId);
    void UpdateDisplayGroup(int reqId, const std::string& contractInfo);
    void UnsubscribeFromGroupEvents(int reqId);

    // ── UI-thread pump ────────────────────────────────────────────────────
    // Call once per frame from the render loop.
    void ProcessMessages();

    // ── Callbacks (set once before/after Connect) ─────────────────────────
    std::function<void(bool connected, const std::string& info)>            onConnectionChanged;
    std::function<void(int reqId, const ::core::Bar&, bool done, bool isLive)> onBarData;
    std::function<void(int tickerId, int field, double price)>              onTickPrice;
    std::function<void(int tickerId, int field, double size)>               onTickSize;
    std::function<void(const std::string& key, const std::string& val,
                       const std::string& currency,
                       const std::string& acct)>                            onAccountValue;
    std::function<void(const ::core::Position&, bool done)>                 onPositionData;
    std::function<void(const ::core::Position&)>                            onPortfolioUpdate;
    std::function<void(int orderId, ::core::OrderStatus,
                       double filled, double avgPrice)>                     onOrderStatusChanged;
    std::function<void(const ::core::Fill&)>                                onFillReceived;
    // Fills from a user-triggered filtered reqExecutions (distinct tint in OrdersWindow)
    std::function<void(const ::core::Fill&)>                                onQueriedFill;

    // Tick-by-tick trades (AllLast) routed by reqId to the owning TradingWindow
    std::function<void(int reqId, const ::core::Tick&)>                     onTickByTick;

    // WSH corporate event (one raw JSON blob per event; parse with WshData::ParseWshEvent)
    std::function<void(int reqId, const std::string& data)>                 onWshEvent;
    std::function<void(int id, bool isBid, int pos, int op,
                       double price, double size)>                          onDepthUpdate;
    std::function<void(int reqId, const ::core::ScanResult&)>              onScanItem;
    std::function<void(int reqId)>                                          onScanEnd;
    std::function<void(std::time_t, const std::string& provider,
                       const std::string& id,
                       const std::string& headline)>                        onNewsItem;

    // Account summary (e.g. tag="Currency", value="USD")
    std::function<void(const std::string& tag, const std::string& value,
                       const std::string& currency)>                        onAccountSummary;
    std::function<void(int reqId, int code, const std::string& msg)>        onError;
    std::function<void(int nextOrderId)>                                    onNextValidId;

    // Open orders (fired by reqOpenOrders and on submission confirmation)
    std::function<void(const ::core::Order&)>                               onOpenOrder;
    std::function<void()>                                                   onOpenOrderEnd;

    // Contract details (fired once per request; carries conId plus description/secType/exchange)
    std::function<void(int reqId, long conId, const std::string& description,
                       const std::string& secType, const std::string& primaryExch,
                       const std::string& currency)>                       onContractConId;

    // Historical news headlines
    std::function<void(int reqId, std::time_t ts, const std::string& provider,
                       const std::string& articleId,
                       const std::string& headline)>                        onHistoricalNews;
    std::function<void(int reqId)>                                          onHistoricalNewsEnd;

    // Full article body (articleType 0 = plain text, 1 = HTML)
    std::function<void(int reqId, int articleType,
                       const std::string& text)>                            onNewsArticle;

    // Real-time P&L (account-level and per-position)
    std::function<void(int reqId, double daily,
                       double unrealized, double realized)>                 onPnL;
    std::function<void(int reqId, double daily,
                       double unrealized, double realized,
                       double value)>                                       onPnLSingle;

    // Symbol autocomplete results (from reqMatchingSymbols)
    std::function<void(int reqId,
                       const std::vector<ContractDesc>&)>                   onSymbolSamples;

    // Tick request params — fires once per reqMktData subscription.
    // Delivers bboExchange code used to call ReqSmartComponents.
    std::function<void(int tickerId, const std::string& bboExchange)>        onTickReqParams;

    // Smart components: exchange routing destinations (reqId 8050–8059).
    std::function<void(int reqId,
                       const std::vector<core::SmartRoute>&)>                onSmartComponents;

    // IB TWS Display Group sync.
    std::function<void(int reqId, const std::string& groups)>                onDisplayGroupList;
    // contractInfo: "symbol:secType:exchange:conId" (parse first field for symbol).
    std::function<void(int reqId, const std::string& contractInfo)>          onDisplayGroupUpdated;

    // Managed accounts list (fires early during connection for FA / multi-account setups)
    std::function<void(const std::vector<std::string>& accounts)>           onManagedAccounts;

    // Multi-account position stream (reqPositionsMulti)
    std::function<void(int reqId, const std::string& account,
                       const std::string& modelCode,
                       const ::core::Position&, bool done)>                 onPositionMulti;

    // Multi-account value stream (reqAccountUpdatesMulti)
    std::function<void(int reqId, const std::string& account,
                       const std::string& modelCode,
                       const std::string& key, const std::string& val,
                       const std::string& currency, bool done)>             onAccountUpdateMulti;

private:
    // ── IB API handles ────────────────────────────────────────────────────
    EReaderOSSignal          m_signal;
    EClientSocket*           m_client;
    std::unique_ptr<EReader> m_reader;
    std::thread              m_readerThread;
    std::atomic<bool>        m_running{false};

    // ── Incoming message queue (EReader thread → UI thread) ──────────────
    std::mutex             m_queueMutex;
    std::vector<IBMessage> m_queue;

protected:
    // Exposed as protected so test subclasses can inject messages directly.
    void Push(IBMessage msg) {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_queue.push_back(std::move(msg));
    }

private:
    // ── Outgoing send thread (prevents PlaceOrder blocking the UI thread) ─
    // EClientSocket is not thread-safe for concurrent sends, so all outgoing
    // IB calls go through a single dedicated send thread via this queue.
    std::mutex                             m_sendMutex;
    std::condition_variable                m_sendCv;
    std::vector<std::function<void()>>     m_sendQueue;
    std::thread                            m_sendThread;
    std::atomic<bool>                      m_sendRunning{false};

    void PostSend(std::function<void()> cmd);
    void SendLoop();

    // Fills cached by execId until commissionReport arrives to complete them.
    // bool = fromQuery: true when fill originates from a filtered reqExecutions call.
    std::mutex                                                          m_fillsMutex;
    std::unordered_map<std::string,
        std::pair<::core::Fill, bool>>                                  m_pendingFills;
    int                                                                 m_filterReqId = -1;

    // Last trade price per tick-by-tick reqId — for uptick/downtick classification
    std::unordered_map<int, double>                                     m_lastTickPrice;

    // ── Helpers ───────────────────────────────────────────────────────────
    Contract MakeStockContract(const std::string& symbol) const;

    // ── EWrapper overrides (only non-trivial ones) ────────────────────────
    void connectAck() override;
    void connectionClosed() override;
    void nextValidId(OrderId orderId) override;

    void error(int id, time_t errorTime, int errorCode,
               const std::string& errorString,
               const std::string& advancedOrderRejectJson) override;

    void marketDataType(TickerId reqId, int marketDataType) override;
    void tickPrice(TickerId tickerId, TickType field, double price,
                   const TickAttrib& attrib) override;
    void tickSize(TickerId tickerId, TickType field, Decimal size) override;

    void updateMktDepth(TickerId id, int position, int operation, int side,
                        double price, Decimal size) override;
    void updateMktDepthL2(TickerId id, int position,
                          const std::string& marketMaker,
                          int operation, int side, double price,
                          Decimal size, bool isSmartDepth) override;

    void historicalData(TickerId reqId, const ::Bar& bar) override;
    void historicalDataEnd(int reqId, const std::string& startDate,
                           const std::string& endDate) override;
    void historicalDataUpdate(TickerId reqId, const ::Bar& bar) override;

    void updateAccountValue(const std::string& key, const std::string& val,
                            const std::string& currency,
                            const std::string& accountName) override;
    void updatePortfolio(const Contract& contract, Decimal position,
                         double marketPrice, double marketValue,
                         double averageCost, double unrealizedPNL,
                         double realizedPNL,
                         const std::string& accountName) override;

    void position(const std::string& account, const Contract& contract,
                  Decimal pos, double avgCost) override;
    void positionEnd() override;

    void orderStatus(OrderId orderId, const std::string& status,
                     Decimal filled, Decimal remaining, double avgFillPrice,
                     long long permId, int parentId, double lastFillPrice,
                     int clientId, const std::string& whyHeld,
                     double mktCapPrice) override;

    void execDetails(int reqId, const Contract& contract,
                     const Execution& execution) override;
    void commissionAndFeesReport(const CommissionAndFeesReport& report) override;

    void scannerData(int reqId, int rank,
                     const ContractDetails& contractDetails,
                     const std::string& distance,
                     const std::string& benchmark,
                     const std::string& projection,
                     const std::string& legsStr) override;
    void scannerDataEnd(int reqId) override;

    void tickNews(int tickerId, time_t timeStamp,
                  const std::string& providerCode,
                  const std::string& articleId,
                  const std::string& headline,
                  const std::string& extraData) override;

    void openOrder(OrderId orderId, const Contract&,
                   const ::Order&, const ::OrderState&) override;
    void openOrderEnd() override;

    void contractDetails(int reqId, const ContractDetails& cd) override;
    void contractDetailsEnd(int reqId) override;

    void historicalNews(int requestId, const std::string& time,
                        const std::string& providerCode,
                        const std::string& articleId,
                        const std::string& headline) override;
    void historicalNewsEnd(int requestId, bool hasMore) override;

    void newsArticle(int requestId, int articleType,
                     const std::string& articleText) override;

    void accountSummary(int reqId, const std::string& account, const std::string& tag,
                        const std::string& value, const std::string& currency) override;
    void accountSummaryEnd(int reqId) override;

    void tickByTickAllLast(int reqId, int tickType, time_t time, double price,
                           Decimal size, const TickAttribLast& attrib,
                           const std::string& exchange,
                           const std::string& specialConditions) override;

    void tickReqParams(int tickerId, double minTick,
                       const std::string& bboExchange,
                       int snapshotPermissions) override;
    void smartComponents(int reqId, const SmartComponentsMap& theMap) override;
    void displayGroupList(int reqId, const std::string& groups) override;
    void displayGroupUpdated(int reqId, const std::string& contractInfo) override;

    void wshMetaData(int reqId, const std::string& dataJson) override;
    void wshEventData(int reqId, const std::string& dataJson) override;

    void pnl(int reqId, double dailyPnL, double unrealizedPnL,
              double realizedPnL) override;
    void pnlSingle(int reqId, Decimal pos, double dailyPnL,
                   double unrealizedPnL, double realizedPnL, double value) override;

    void symbolSamples(int reqId,
                       const std::vector<ContractDescription>& contractDescriptions) override;

    void managedAccounts(const std::string& accountsList) override;

    void positionMulti(int reqId, const std::string& account,
                       const std::string& modelCode, const Contract& contract,
                       Decimal pos, double avgCost) override;
    void positionMultiEnd(int reqId) override;

    void accountUpdateMulti(int reqId, const std::string& account,
                            const std::string& modelCode, const std::string& key,
                            const std::string& value,
                            const std::string& currency) override;
    void accountUpdateMultiEnd(int reqId) override;
};

} // namespace core::services
