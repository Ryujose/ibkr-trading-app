#pragma once

#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
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

// Forward-declare IB API types we use only in the .cpp
struct Bar;
struct Contract;
struct ContractDetails;
struct Execution;
struct OrderCancel;

namespace core::services {

// ── Thread-safe message types (EReader thread → UI thread) ─────────────────

struct MsgConnection  { bool connected; std::string info; };
struct MsgBar         { int reqId; ::core::Bar bar; bool done; };
struct MsgTickPrice   { int tickerId; int field; double price; };
struct MsgTickSize    { int tickerId; int field; double size; };
struct MsgAccountVal  { std::string key, val, currency, account; };
struct MsgPosition    { ::core::Position pos; bool done; };
struct MsgPortfolio   { ::core::Position pos; };
struct MsgOrderStatus { int orderId; ::core::OrderStatus status;
                        double filled; double avgPrice; };
struct MsgFill        { ::core::Fill fill; };
struct MsgDepth       { int id; bool isBid; int pos; int op;
                        double price; double size; };
struct MsgScanItem    { int reqId; ::core::ScanResult result; };
struct MsgScanEnd     { int reqId; };
struct MsgNews        { std::time_t ts; std::string provider;
                        std::string articleId; std::string headline; };
struct MsgError       { int code; std::string msg; };
struct MsgNextOrderId { int orderId; };

using IBMessage = std::variant<
    MsgConnection, MsgBar, MsgTickPrice, MsgTickSize,
    MsgAccountVal, MsgPosition, MsgPortfolio, MsgOrderStatus,
    MsgFill, MsgDepth, MsgScanItem, MsgScanEnd, MsgNews,
    MsgError, MsgNextOrderId
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
                           const std::string& duration = "6 M",
                           const std::string& barSize  = "1 day");
    void CancelHistoricalData(int reqId);

    // Market data type:
    //   1 = Live (requires active subscription)
    //   2 = Frozen (last available real-time)
    //   3 = Delayed (15-20 min, free — use for paper accounts)
    //   4 = Delayed-Frozen
    void ReqMarketDataType(int type);

    void ReqMarketData(int reqId, const std::string& symbol);
    void CancelMarketData(int reqId);

    void ReqMktDepth(int reqId, const std::string& symbol, int numRows = 10);
    void CancelMktDepth(int reqId);

    void ReqAccountUpdates(bool subscribe, const std::string& acctCode = "");
    void ReqPositions();

    void ReqScannerData(int reqId,
                        const std::string& scanCode     = "TOP_PERC_GAIN",
                        const std::string& instrument   = "STK",
                        const std::string& locationCode = "STK.US.MAJOR");
    void CancelScannerData(int reqId);

    void PlaceOrder(int orderId, const std::string& symbol,
                    const std::string& action,     // "BUY" | "SELL"
                    const std::string& orderType,  // "MKT" | "LMT"
                    double qty, double limitPrice = 0.0);
    void CancelOrder(int orderId);
    void ReqOpenOrders();

    // ── UI-thread pump ────────────────────────────────────────────────────
    // Call once per frame from the render loop.
    void ProcessMessages();

    // ── Callbacks (set once before/after Connect) ─────────────────────────
    std::function<void(bool connected, const std::string& info)>            onConnectionChanged;
    std::function<void(int reqId, const ::core::Bar&, bool done)>           onBarData;
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
    std::function<void(int id, bool isBid, int pos, int op,
                       double price, double size)>                          onDepthUpdate;
    std::function<void(int reqId, const ::core::ScanResult&)>              onScanItem;
    std::function<void(int reqId)>                                          onScanEnd;
    std::function<void(std::time_t, const std::string& provider,
                       const std::string& id,
                       const std::string& headline)>                        onNewsItem;
    std::function<void(int code, const std::string& msg)>                  onError;
    std::function<void(int nextOrderId)>                                    onNextValidId;

private:
    // ── IB API handles ────────────────────────────────────────────────────
    EReaderOSSignal          m_signal;
    EClientSocket*           m_client;
    std::unique_ptr<EReader> m_reader;
    std::thread              m_readerThread;
    std::atomic<bool>        m_running{false};

    // ── Thread-safe queue ─────────────────────────────────────────────────
    std::mutex             m_queueMutex;
    std::vector<IBMessage> m_queue;

    void Push(IBMessage msg) {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_queue.push_back(std::move(msg));
    }

    // ── Helpers ───────────────────────────────────────────────────────────
    Contract MakeStockContract(const std::string& symbol) const;
    static ::core::OrderStatus ParseStatus(const std::string& s);
    static std::time_t         ParseIBTime(const std::string& ts);

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
};

} // namespace core::services
