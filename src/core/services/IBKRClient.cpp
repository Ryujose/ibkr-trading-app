#include "core/services/IBKRClient.h"
#include "core/services/IBKRUtils.h"

// IB API implementation headers
#include "EClientSocket.h"
#include "Contract.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"
#include "Execution.h"
#include "CommissionAndFeesReport.h"
#include "ScannerSubscription.h"
#include "TagValue.h"
#include "CommonDefs.h"
#include "Decimal.h"
#include "bar.h"

#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <chrono>

namespace core::services {

// ============================================================================
// Construction / Destruction
// ============================================================================

IBKRClient::IBKRClient()
    : m_signal(2000)
    , m_client(new EClientSocket(this, &m_signal))
{}

IBKRClient::~IBKRClient() {
    Disconnect();
    delete m_client;
}

// ============================================================================
// Connection
// ============================================================================

bool IBKRClient::Connect(const std::string& host, int port, int clientId) {
    if (m_client->isConnected()) return true;

    bool ok = m_client->eConnect(host.c_str(), port, clientId, false);
    if (!ok) return false;

    m_reader = std::make_unique<EReader>(m_client, &m_signal);
    m_reader->start();
    m_running.store(true);

    m_readerThread = std::thread([this]() {
        while (m_running.load() && m_client->isConnected()) {
            m_signal.waitForSignal();
            if (m_running.load())
                m_reader->processMsgs();
        }
    });

    // Start the dedicated send thread so PlaceOrder/CancelOrder never block
    // the UI/render thread on a slow socket write.
    m_sendRunning.store(true);
    m_sendThread = std::thread(&IBKRClient::SendLoop, this);

    return true;
}

void IBKRClient::Disconnect() {
    // Stop send thread first so no new orders are sent while disconnecting.
    m_sendRunning.store(false);
    m_sendCv.notify_all();
    if (m_sendThread.joinable())
        m_sendThread.join();

    m_running.store(false);
    if (m_client->isConnected())
        m_client->eDisconnect();
    m_signal.issueSignal();
    if (m_readerThread.joinable())
        m_readerThread.join();
    m_reader.reset();
}

// ── Send thread ────────────────────────────────────────────────────────────

void IBKRClient::SendLoop() {
    while (m_sendRunning.load()) {
        std::vector<std::function<void()>> batch;
        {
            std::unique_lock<std::mutex> lk(m_sendMutex);
            m_sendCv.wait(lk, [this] {
                return !m_sendQueue.empty() || !m_sendRunning.load();
            });
            if (!m_sendRunning.load() && m_sendQueue.empty()) break;
            batch.swap(m_sendQueue);
        }
        for (auto& fn : batch)
            if (m_client->isConnected()) fn();
    }
}

void IBKRClient::PostSend(std::function<void()> cmd) {
    {
        std::lock_guard<std::mutex> lk(m_sendMutex);
        m_sendQueue.push_back(std::move(cmd));
    }
    m_sendCv.notify_one();
}

bool IBKRClient::IsConnected() const {
    return m_client->isConnected();
}

// ============================================================================
// Outgoing Requests
// ============================================================================

Contract IBKRClient::MakeStockContract(const std::string& symbol) const {
    Contract c;
    c.symbol      = symbol;
    c.secType     = "STK";
    c.currency    = "USD";
    c.exchange    = "SMART";
    c.primaryExchange = "NASDAQ";
    return c;
}

void IBKRClient::ReqHistoricalData(int reqId, const std::string& symbol,
                                    const std::string& duration,
                                    const std::string& barSize,
                                    bool useRTH,
                                    const std::string& endDateTime) {
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    // formatDate=2 → IB always returns Unix timestamps.
    // keepUpToDate only for intraday bars — IB doesn't support it for daily/weekly/monthly
    // and returns corrupt/zero timestamps for those when enabled.
    bool isIntraday = (barSize.find("day")   == std::string::npos &&
                       barSize.find("week")  == std::string::npos &&
                       barSize.find("month") == std::string::npos);
    // When endDateTime is provided (extend-history request), disable keepUpToDate
    // regardless of bar size — historical range requests can't use keepUpToDate.
    bool keepUpToDate = isIntraday && endDateTime.empty();
    m_client->reqHistoricalData(reqId, c, endDateTime, duration, barSize,
                                "TRADES", useRTH ? 1 : 0, 2, keepUpToDate, empty);
}

void IBKRClient::CancelHistoricalData(int reqId) {
    m_client->cancelHistoricalData(reqId);
}

void IBKRClient::ReqContractDetails(int reqId, const std::string& symbol) {
    Contract c;
    c.symbol   = symbol;
    c.secType  = "STK";
    c.currency = "USD";
    c.exchange = "SMART";
    // Do NOT set primaryExchange: filtering by it silently drops non-NASDAQ stocks
    // (e.g. SPY on ARCA, BRK.B on NYSE), so onContractConId would never fire.
    m_client->reqContractDetails(reqId, c);
}

void IBKRClient::ReqHistoricalNews(int reqId, int conId, int totalResults,
                                    const std::string& providerCodes) {
    TagValueListSPtr empty;
    const std::string& codes = providerCodes.empty() ? kFreeNewsProviders : providerCodes;
    m_client->reqHistoricalNews(reqId, conId, codes, "", "", totalResults, empty);
}

void IBKRClient::SubscribeToNews(int reqId, const std::string& symbol) {
    // "292" (Wide_news) is the only valid generic tick for news ticks on this gateway.
    // "mdoff" and provider-list suffixes ("292:BRFUPDN+...") are rejected with error 321.
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    m_client->reqMktData(reqId, c, "292", false, false, empty);
}

void IBKRClient::ReqNewsArticle(int reqId, const std::string& providerCode,
                                 const std::string& articleId) {
    TagValueListSPtr empty;
    m_client->reqNewsArticle(reqId, providerCode, articleId, empty);
}

void IBKRClient::ReqMarketDataType(int type) {
    m_client->reqMarketDataType(type);
}

void IBKRClient::ReqMarketData(int reqId, const std::string& symbol,
                                const std::string& genericTickList) {
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    m_client->reqMktData(reqId, c, genericTickList, false, false, empty);
}

void IBKRClient::CancelMarketData(int reqId) {
    m_client->cancelMktData(reqId);
}

void IBKRClient::ReqMktDepth(int reqId, const std::string& symbol, int numRows) {
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    m_client->reqMktDepth(reqId, c, numRows, false, empty);
}

void IBKRClient::CancelMktDepth(int reqId) {
    m_client->cancelMktDepth(reqId, false);
}

void IBKRClient::ReqAccountUpdates(bool subscribe, const std::string& acctCode) {
    m_client->reqAccountUpdates(subscribe, acctCode);
}

void IBKRClient::ReqPositions() {
    m_client->reqPositions();
}

void IBKRClient::ReqAccountSummary(int reqId, const std::string& tags) {
    m_client->reqAccountSummary(reqId, "All", tags);
}

void IBKRClient::CancelAccountSummary(int reqId) {
    m_client->cancelAccountSummary(reqId);
}

void IBKRClient::ReqScannerData(int reqId, const std::string& scanCode,
                                 const std::string& instrument,
                                 const std::string& locationCode) {
    ScannerSubscription sub;
    sub.instrument   = instrument;
    sub.locationCode = locationCode;
    sub.scanCode     = scanCode;
    sub.numberOfRows = 25;
    TagValueListSPtr empty;
    m_client->reqScannerSubscription(reqId, sub, empty, empty);
}

void IBKRClient::CancelScannerData(int reqId) {
    m_client->cancelScannerSubscription(reqId);
}

void IBKRClient::PlaceOrder(const ::core::Order& o) {
    // Capture order by value so the UI thread can mutate o after this call returns.
    PostSend([o, this]() {
        Contract c = MakeStockContract(o.symbol);
        ::Order ibOrder;
        ibOrder.action        = ::core::OrderSideStr(o.side);
        ibOrder.totalQuantity = DecimalFunctions::doubleToDecimal(o.quantity);
        ibOrder.orderType     = ::core::OrderTypeStr(o.type);
        ibOrder.outsideRth    = o.outsideRth;

        // TIF
        static constexpr const char* kTIFs[] = {"DAY", "GTC", "IOC", "FOK"};
        ibOrder.tif = kTIFs[static_cast<int>(o.tif)];

        // Price fields per order type
        switch (o.type) {
            case ::core::OrderType::Market:
            case ::core::OrderType::MOC:
            case ::core::OrderType::MTL:
                break;  // no price fields

            case ::core::OrderType::Limit:
            case ::core::OrderType::LOC:
                ibOrder.lmtPrice = o.limitPrice;
                break;

            case ::core::OrderType::Stop:
                ibOrder.auxPrice = o.stopPrice;
                break;

            case ::core::OrderType::StopLimit:
                ibOrder.auxPrice = o.stopPrice;
                ibOrder.lmtPrice = o.limitPrice;
                break;

            case ::core::OrderType::Trail:
                if (o.trailingPercent > 0.0)
                    ibOrder.trailingPercent = o.trailingPercent;
                else
                    ibOrder.auxPrice = o.auxPrice;       // trailing amount $
                if (o.trailStopPrice > 0.0)
                    ibOrder.trailStopPrice = o.trailStopPrice;
                break;

            case ::core::OrderType::TrailLimit:
                if (o.trailingPercent > 0.0)
                    ibOrder.trailingPercent = o.trailingPercent;
                else
                    ibOrder.auxPrice = o.auxPrice;       // trailing amount $
                ibOrder.lmtPriceOffset = o.lmtPriceOffset;
                if (o.trailStopPrice > 0.0)
                    ibOrder.trailStopPrice = o.trailStopPrice;
                else
                    printf("[IBKR] Warning: TRAIL LIMIT submitted without trailStopPrice. Symbols: %s\n", o.symbol.c_str());
                break;

            case ::core::OrderType::MIT:
                ibOrder.auxPrice = o.auxPrice;           // trigger price
                break;

            case ::core::OrderType::LIT:
                ibOrder.auxPrice = o.auxPrice;           // trigger price
                ibOrder.lmtPrice = o.limitPrice;
                break;

            case ::core::OrderType::Midprice:
                if (o.limitPrice > 0.0)
                    ibOrder.lmtPrice = o.limitPrice;     // optional price cap
                break;

            case ::core::OrderType::Relative:
                if (o.limitPrice > 0.0)
                    ibOrder.lmtPrice = o.limitPrice;     // absolute cap
                if (o.auxPrice > 0.0)
                    ibOrder.auxPrice = o.auxPrice;       // peg offset
                break;
        }

        if (!o.account.empty())
            ibOrder.account = o.account;

        m_client->placeOrder(o.orderId, c, ibOrder);
    });
}

void IBKRClient::CancelOrder(int orderId) {
    PostSend([=, this]() {
        OrderCancel oc;
        m_client->cancelOrder(orderId, oc);
    });
}

void IBKRClient::ReqOpenOrders() {
    m_client->reqOpenOrders();
}

void IBKRClient::ReqAllOpenOrders() {
    m_client->reqAllOpenOrders();
}

void IBKRClient::ReqExecutions(int reqId) {
    ExecutionFilter filter;   // all defaults = no filter → full day's history
    m_client->reqExecutions(reqId, filter);
}

void IBKRClient::ReqPnL(int reqId, const std::string& account, const std::string& modelCode) {
    m_client->reqPnL(reqId, account, modelCode);
}
void IBKRClient::CancelPnL(int reqId) {
    m_client->cancelPnL(reqId);
}
void IBKRClient::ReqPnLSingle(int reqId, const std::string& account,
                               const std::string& modelCode, int conId) {
    m_client->reqPnLSingle(reqId, account, modelCode, conId);
}
void IBKRClient::CancelPnLSingle(int reqId) {
    m_client->cancelPnLSingle(reqId);
}

void IBKRClient::ReqMatchingSymbols(int reqId, const std::string& pattern) {
    m_client->reqMatchingSymbols(reqId, pattern);
}

void IBKRClient::ReqPositionsMulti(int reqId, const std::string& account,
                                    const std::string& modelCode) {
    m_client->reqPositionsMulti(reqId, account, modelCode);
}
void IBKRClient::CancelPositionsMulti(int reqId) {
    m_client->cancelPositionsMulti(reqId);
}
void IBKRClient::ReqAccountUpdatesMulti(int reqId, const std::string& account,
                                         const std::string& modelCode,
                                         bool ledgerAndNLV) {
    m_client->reqAccountUpdatesMulti(reqId, account, modelCode, ledgerAndNLV);
}
void IBKRClient::CancelAccountUpdatesMulti(int reqId) {
    m_client->cancelAccountUpdatesMulti(reqId);
}

// ============================================================================
// UI-thread pump
// ============================================================================

void IBKRClient::ProcessMessages() {
    std::vector<IBMessage> batch;
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        batch.swap(m_queue);
    }

    // 5 ms time budget per frame: prevents a Level-II depth flood from stalling
    // the render loop. Unprocessed messages are re-queued at the front for the
    // next frame so no data is lost.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    std::size_t processed = 0;

    for (auto& msg : batch) {
        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, MsgConnection>) {
                if (onConnectionChanged) onConnectionChanged(m.connected, m.info);

            } else if constexpr (std::is_same_v<T, MsgBar>) {
                if (onBarData) onBarData(m.reqId, m.bar, m.done, m.isLive);

            } else if constexpr (std::is_same_v<T, MsgTickPrice>) {
                if (onTickPrice) onTickPrice(m.tickerId, m.field, m.price);

            } else if constexpr (std::is_same_v<T, MsgTickSize>) {
                if (onTickSize) onTickSize(m.tickerId, m.field, m.size);

            } else if constexpr (std::is_same_v<T, MsgAccountVal>) {
                if (onAccountValue) onAccountValue(m.key, m.val, m.currency, m.account);

            } else if constexpr (std::is_same_v<T, MsgPosition>) {
                if (onPositionData) onPositionData(m.pos, m.done);

            } else if constexpr (std::is_same_v<T, MsgPortfolio>) {
                if (onPortfolioUpdate) onPortfolioUpdate(m.pos);

            } else if constexpr (std::is_same_v<T, MsgOrderStatus>) {
                if (onOrderStatusChanged)
                    onOrderStatusChanged(m.orderId, m.status, m.filled, m.avgPrice);

            } else if constexpr (std::is_same_v<T, MsgFill>) {
                if (onFillReceived) onFillReceived(m.fill);

            } else if constexpr (std::is_same_v<T, MsgDepth>) {
                if (onDepthUpdate)
                    onDepthUpdate(m.id, m.isBid, m.pos, m.op, m.price, m.size);

            } else if constexpr (std::is_same_v<T, MsgScanItem>) {
                if (onScanItem) onScanItem(m.reqId, m.result);

            } else if constexpr (std::is_same_v<T, MsgScanEnd>) {
                if (onScanEnd) onScanEnd(m.reqId);

            } else if constexpr (std::is_same_v<T, MsgNews>) {
                if (onNewsItem) onNewsItem(m.ts, m.provider, m.articleId, m.headline);

            } else if constexpr (std::is_same_v<T, MsgError>) {
                if (onError) onError(m.reqId, m.code, m.msg);

            } else if constexpr (std::is_same_v<T, MsgNextOrderId>) {
                if (onNextValidId) onNextValidId(m.orderId);

            } else if constexpr (std::is_same_v<T, MsgOpenOrder>) {
                if (onOpenOrder) onOpenOrder(m.order);

            } else if constexpr (std::is_same_v<T, MsgOpenOrderEnd>) {
                if (onOpenOrderEnd) onOpenOrderEnd();

            } else if constexpr (std::is_same_v<T, MsgContractConId>) {
                if (onContractConId) onContractConId(m.reqId, m.conId);

            } else if constexpr (std::is_same_v<T, MsgHistoricalNews>) {
                if (onHistoricalNews)
                    onHistoricalNews(m.reqId, m.ts, m.provider, m.articleId, m.headline);

            } else if constexpr (std::is_same_v<T, MsgHistoricalNewsEnd>) {
                if (onHistoricalNewsEnd) onHistoricalNewsEnd(m.reqId);

            } else if constexpr (std::is_same_v<T, MsgNewsArticle>) {
                if (onNewsArticle) onNewsArticle(m.reqId, 0, m.text);

            } else if constexpr (std::is_same_v<T, MsgAcctSummary>) {
                if (onAccountSummary) onAccountSummary(m.tag, m.value, m.currency);

            } else if constexpr (std::is_same_v<T, MsgPnL>) {
                if (onPnL) onPnL(m.reqId, m.daily, m.unrealized, m.realized);

            } else if constexpr (std::is_same_v<T, MsgPnLSingle>) {
                if (onPnLSingle) onPnLSingle(m.reqId, m.daily, m.unrealized, m.realized, m.value);

            } else if constexpr (std::is_same_v<T, MsgSymbolSamples>) {
                if (onSymbolSamples) onSymbolSamples(m.reqId, m.results);

            } else if constexpr (std::is_same_v<T, MsgManagedAccts>) {
                if (onManagedAccounts) onManagedAccounts(m.accounts);

            } else if constexpr (std::is_same_v<T, MsgPositionMulti>) {
                if (onPositionMulti)
                    onPositionMulti(m.reqId, m.account, m.modelCode, m.pos, m.done);

            } else if constexpr (std::is_same_v<T, MsgAccountUpdateMulti>) {
                if (onAccountUpdateMulti)
                    onAccountUpdateMulti(m.reqId, m.account, m.modelCode,
                                         m.key, m.val, m.currency, m.done);
            }
        }, msg);

        ++processed;
        // Check clock every 64 messages to amortise the syscall cost.
        if ((processed & 63) == 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    // Re-queue any messages that were not reached within the time budget.
    if (processed < batch.size()) {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        // Remaining tail of this batch goes to the front so ordering is kept.
        std::vector<IBMessage> remaining(
            std::make_move_iterator(batch.begin() + processed),
            std::make_move_iterator(batch.end()));
        remaining.insert(remaining.end(),
                         std::make_move_iterator(m_queue.begin()),
                         std::make_move_iterator(m_queue.end()));
        m_queue = std::move(remaining);
    }
}

// ============================================================================
// EWrapper callbacks (EReader thread → push into queue)
// ============================================================================

void IBKRClient::connectAck() {
    // Nothing to do; we wait for nextValidId as the "ready" signal
}

void IBKRClient::connectionClosed() {
    m_running.store(false);
    Push(MsgConnection{false, "Connection closed by remote host"});
}

void IBKRClient::nextValidId(OrderId orderId) {
    // nextValidId signals that the connection is fully ready
    Push(MsgNextOrderId{static_cast<int>(orderId)});
    Push(MsgConnection{true, "Connected"});
}

void IBKRClient::error(int id, time_t /*errorTime*/, int errorCode,
                        const std::string& errorString,
                        const std::string& /*advancedOrderRejectJson*/) {
    // Filter pure informational codes (market data farm connection status, etc.)
    // 10148: order already in PendingCancel — duplicate cancel, not a rejection.
    // 10149: cancel attempt on PreSubmitted/Submitted order in transition — same.
    static const int info_codes[] = {
        2100, 2103, 2104, 2105, 2106, 2107, 2108, 2119, 2158, 10182,
        10148, 10149
    };
    for (int c : info_codes) {
        if (errorCode == c) return;
    }
    // Fatal connection errors
    if (errorCode == 1100 || errorCode == 1300 ||
        (id == -1 && errorCode >= 500 && errorCode < 600)) {
        Push(MsgConnection{false, errorString});
        return;
    }
    Push(MsgError{id, errorCode, errorString});
}

// ── Market data ────────────────────────────────────────────────────────────

void IBKRClient::marketDataType(TickerId /*reqId*/, int marketDataType) {
    static const char* names[] = {"", "Live", "Frozen", "Delayed", "Delayed-Frozen"};
    const char* name = (marketDataType >= 1 && marketDataType <= 4)
                       ? names[marketDataType] : "Unknown";
    printf("[IB] Market data type: %d (%s)\n", marketDataType, name);
}

void IBKRClient::tickPrice(TickerId tickerId, TickType field, double price,
                            const TickAttrib& /*attrib*/) {
    Push(MsgTickPrice{static_cast<int>(tickerId), static_cast<int>(field), price});
}

void IBKRClient::tickSize(TickerId tickerId, TickType field, Decimal size) {
    Push(MsgTickSize{static_cast<int>(tickerId),
                     static_cast<int>(field),
                     DecimalFunctions::decimalToDouble(size)});
}

// ── Market depth ───────────────────────────────────────────────────────────

void IBKRClient::updateMktDepth(TickerId id, int position, int operation,
                                 int side, double price, Decimal size) {
    Push(MsgDepth{static_cast<int>(id),
                  side == 1,  // 1=bid, 0=ask
                  position, operation,
                  price,
                  DecimalFunctions::decimalToDouble(size)});
}

void IBKRClient::updateMktDepthL2(TickerId id, int position,
                                   const std::string& /*marketMaker*/,
                                   int operation, int side, double price,
                                   Decimal size, bool /*isSmartDepth*/) {
    Push(MsgDepth{static_cast<int>(id),
                  side == 1,
                  position, operation,
                  price,
                  DecimalFunctions::decimalToDouble(size)});
}

// ── Historical data ────────────────────────────────────────────────────────

void IBKRClient::historicalData(TickerId reqId, const ::Bar& bar) {
    ::core::Bar b;
    b.timestamp = static_cast<double>(ParseIBTime(bar.time));
    b.open      = bar.open;
    b.high      = bar.high;
    b.low       = bar.low;
    b.close     = bar.close;
    b.volume    = DecimalFunctions::decimalToDouble(bar.volume);
    Push(MsgBar{static_cast<int>(reqId), b, false, false});
}

void IBKRClient::historicalDataEnd(int reqId, const std::string& /*startDate*/,
                                    const std::string& /*endDate*/) {
    Push(MsgBar{reqId, {}, true, false});  // sentinel: done=true, not a live bar
}

void IBKRClient::historicalDataUpdate(TickerId reqId, const ::Bar& bar) {
    // Live bar: forming bar update — push with isLive=true so chart updates in place
    ::core::Bar b;
    b.timestamp = static_cast<double>(ParseIBTime(bar.time));
    b.open      = bar.open;
    b.high      = bar.high;
    b.low       = bar.low;
    b.close     = bar.close;
    b.volume    = DecimalFunctions::decimalToDouble(bar.volume);
    Push(MsgBar{static_cast<int>(reqId), b, false, true});
}

// ── Account / Portfolio ────────────────────────────────────────────────────

void IBKRClient::updateAccountValue(const std::string& key,
                                     const std::string& val,
                                     const std::string& currency,
                                     const std::string& accountName) {
    Push(MsgAccountVal{key, val, currency, accountName});
}

void IBKRClient::updatePortfolio(const Contract& contract, Decimal position,
                                  double marketPrice, double marketValue,
                                  double averageCost, double unrealizedPNL,
                                  double realizedPNL,
                                  const std::string& /*accountName*/) {
    ::core::Position pos;
    pos.symbol        = contract.symbol;
    pos.assetClass    = contract.secType;
    pos.exchange      = contract.exchange;
    pos.currency      = contract.currency;
    pos.conId         = contract.conId;
    pos.quantity      = DecimalFunctions::decimalToDouble(position);
    pos.avgCost       = averageCost;
    pos.marketPrice   = marketPrice;
    pos.marketValue   = marketValue;
    pos.unrealizedPnL = unrealizedPNL;
    pos.realizedPnL   = realizedPNL;
    pos.costBasis     = pos.quantity * averageCost;
    if (averageCost > 0.0)
        pos.unrealizedPct = (marketPrice - averageCost) / averageCost * 100.0;
    Push(MsgPortfolio{pos});
}

// ── Positions ──────────────────────────────────────────────────────────────

void IBKRClient::position(const std::string& /*account*/,
                           const Contract& contract,
                           Decimal pos, double avgCost) {
    ::core::Position p;
    p.symbol     = contract.symbol;
    p.assetClass = contract.secType;
    p.exchange   = contract.exchange;
    p.currency   = contract.currency;
    p.quantity   = DecimalFunctions::decimalToDouble(pos);
    p.avgCost    = avgCost;
    Push(MsgPosition{p, false});
}

void IBKRClient::positionEnd() {
    Push(MsgPosition{{}, true});
}

// ── Managed accounts / multi-account ──────────────────────────────────────

void IBKRClient::managedAccounts(const std::string& accountsList) {
    std::vector<std::string> accts;
    std::istringstream ss(accountsList);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // trim whitespace
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t") + 1);
        if (!tok.empty()) accts.push_back(tok);
    }
    Push(MsgManagedAccts{std::move(accts)});
}

void IBKRClient::positionMulti(int reqId, const std::string& account,
                                const std::string& modelCode,
                                const Contract& contract,
                                Decimal pos, double avgCost) {
    ::core::Position p;
    p.symbol     = contract.symbol;
    p.assetClass = contract.secType;
    p.exchange   = contract.exchange;
    p.currency   = contract.currency;
    p.conId      = contract.conId;
    p.quantity   = DecimalFunctions::decimalToDouble(pos);
    p.avgCost    = avgCost;
    Push(MsgPositionMulti{reqId, account, modelCode, p, false});
}

void IBKRClient::positionMultiEnd(int reqId) {
    Push(MsgPositionMulti{reqId, {}, {}, {}, true});
}

void IBKRClient::accountUpdateMulti(int reqId, const std::string& account,
                                     const std::string& modelCode,
                                     const std::string& key,
                                     const std::string& value,
                                     const std::string& currency) {
    Push(MsgAccountUpdateMulti{reqId, account, modelCode, key, value, currency, false});
}

void IBKRClient::accountUpdateMultiEnd(int reqId) {
    Push(MsgAccountUpdateMulti{reqId, {}, {}, {}, {}, {}, true});
}

// ── Orders ─────────────────────────────────────────────────────────────────

void IBKRClient::orderStatus(OrderId orderId, const std::string& status,
                              Decimal filled, Decimal /*remaining*/,
                              double avgFillPrice, long long /*permId*/,
                              int /*parentId*/, double /*lastFillPrice*/,
                              int /*clientId*/, const std::string& /*whyHeld*/,
                              double /*mktCapPrice*/) {
    Push(MsgOrderStatus{
        static_cast<int>(orderId),
        ParseStatus(status),
        DecimalFunctions::decimalToDouble(filled),
        avgFillPrice
    });
}

void IBKRClient::execDetails(int /*reqId*/, const Contract& contract,
                              const Execution& execution) {
    ::core::Fill fill;
    fill.orderId   = static_cast<int>(execution.orderId);
    fill.execId    = execution.execId;
    fill.symbol    = contract.symbol;
    fill.side      = (execution.side == "BOT") ? ::core::OrderSide::Buy
                                                : ::core::OrderSide::Sell;
    fill.quantity  = DecimalFunctions::decimalToDouble(execution.shares);
    fill.price     = execution.price;
    fill.timestamp = std::time(nullptr);
    // Cache; commissionAndFeesReport will complete and push it
    std::lock_guard<std::mutex> lk(m_fillsMutex);
    m_pendingFills[fill.execId] = fill;
}

void IBKRClient::commissionAndFeesReport(const CommissionAndFeesReport& report) {
    std::unique_lock<std::mutex> lk(m_fillsMutex);
    auto it = m_pendingFills.find(report.execId);
    if (it == m_pendingFills.end()) return;  // stale / already handled
    ::core::Fill fill      = it->second;
    m_pendingFills.erase(it);
    lk.unlock();

    fill.commission  = report.commissionAndFees;
    fill.realizedPnL = report.realizedPNL;
    Push(MsgFill{fill});
}

// ── Scanner ────────────────────────────────────────────────────────────────

void IBKRClient::scannerData(int reqId, int /*rank*/,
                              const ContractDetails& cd,
                              const std::string& /*distance*/,
                              const std::string& /*benchmark*/,
                              const std::string& /*projection*/,
                              const std::string& /*legsStr*/) {
    ::core::ScanResult r;
    r.symbol   = cd.contract.symbol;
    r.company  = cd.longName;
    r.sector   = cd.industry;
    r.exchange = cd.contract.primaryExchange.empty()
                     ? cd.contract.exchange
                     : cd.contract.primaryExchange;
    Push(MsgScanItem{reqId, r});
}

void IBKRClient::scannerDataEnd(int reqId) {
    Push(MsgScanEnd{reqId});
}

// ── News ───────────────────────────────────────────────────────────────────

void IBKRClient::tickNews(int /*tickerId*/, time_t timeStamp,
                           const std::string& providerCode,
                           const std::string& articleId,
                           const std::string& headline,
                           const std::string& /*extraData*/) {
    Push(MsgNews{timeStamp, providerCode, articleId, headline});
}

// ── Open orders ─────────────────────────────────────────────────────────────

static ::core::OrderSide ParseSide(const std::string& action) {
    return (action == "BUY") ? ::core::OrderSide::Buy : ::core::OrderSide::Sell;
}

static ::core::OrderType ParseOrderType(const std::string& t) {
    if (t == "LMT")           return ::core::OrderType::Limit;
    if (t == "STP")           return ::core::OrderType::Stop;
    if (t == "STP LMT")       return ::core::OrderType::StopLimit;
    if (t == "TRAIL")         return ::core::OrderType::Trail;
    if (t == "TRAIL LIMIT")   return ::core::OrderType::TrailLimit;
    if (t == "MOC")           return ::core::OrderType::MOC;
    if (t == "LOC")           return ::core::OrderType::LOC;
    if (t == "MTL")           return ::core::OrderType::MTL;
    if (t == "MIT")           return ::core::OrderType::MIT;
    if (t == "LIT")           return ::core::OrderType::LIT;
    if (t == "MIDPRICE")      return ::core::OrderType::Midprice;
    if (t == "REL")           return ::core::OrderType::Relative;
    return ::core::OrderType::Market;
}

static ::core::TimeInForce ParseTIF(const std::string& t) {
    if (t == "GTC") return ::core::TimeInForce::GTC;
    if (t == "IOC") return ::core::TimeInForce::IOC;
    if (t == "FOK") return ::core::TimeInForce::FOK;
    return ::core::TimeInForce::Day;
}

void IBKRClient::openOrder(OrderId orderId, const Contract& c,
                            const ::Order& o, const ::OrderState& s) {
    ::core::Order order;
    order.orderId     = static_cast<int>(orderId);
    order.symbol      = c.symbol;
    order.side        = ParseSide(o.action);
    order.type        = ParseOrderType(o.orderType);
    order.tif         = ParseTIF(o.tif);
    order.quantity    = DecimalFunctions::decimalToDouble(o.totalQuantity);
    order.limitPrice  = (o.lmtPrice  != UNSET_DOUBLE) ? o.lmtPrice  : 0.0;
    
    // In core::Order, we use stopPrice for STP/STPLMT trigger, 
    // and auxPrice for others (MIT/LIT/TRAIL/REL).
    double aux = (o.auxPrice != UNSET_DOUBLE) ? o.auxPrice : 0.0;
    order.stopPrice   = aux;
    order.auxPrice    = aux;

    order.trailingPercent = (o.trailingPercent != UNSET_DOUBLE) ? o.trailingPercent : 0.0;
    order.trailStopPrice  = (o.trailStopPrice  != UNSET_DOUBLE) ? o.trailStopPrice  : 0.0;
    order.lmtPriceOffset  = (o.lmtPriceOffset  != UNSET_DOUBLE) ? o.lmtPriceOffset  : 0.0;
    order.outsideRth      = o.outsideRth;

    order.commission  = (s.commissionAndFees != UNSET_DOUBLE) ? s.commissionAndFees : 0.0;
    order.status      = ParseStatus(s.status);
    order.submittedAt = std::time(nullptr);
    order.updatedAt   = std::time(nullptr);
    Push(MsgOpenOrder{order});
}

void IBKRClient::openOrderEnd() {
    Push(MsgOpenOrderEnd{});
}

// ── Contract details ────────────────────────────────────────────────────────

void IBKRClient::contractDetails(int reqId, const ContractDetails& cd) {
    Push(MsgContractConId{reqId, cd.contract.conId});
}

void IBKRClient::contractDetailsEnd(int /*reqId*/) {
    // Nothing to do — we already pushed the conId via contractDetails
}

// ── Historical news ─────────────────────────────────────────────────────────

// IB historical news time format: "2024-01-15 09:30:00.0" or "20240115-09:30:00"
static std::time_t ParseNewsTime(const std::string& s) {
    if (s.empty()) return std::time(nullptr);
    // Try "YYYY-MM-DD HH:MM:SS" style
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec) >= 3) {
        tm.tm_year -= 1900;
        tm.tm_mon  -= 1;
        // timegm not portable; use mktime + UTC offset trick
        tm.tm_isdst = 0;
        std::time_t local = std::mktime(&tm);
        // mktime interprets as local time; correct to UTC via gmtime round-trip
        std::tm* utcCheck = std::gmtime(&local);
        if (utcCheck) {
            utcCheck->tm_isdst = 0;
            std::time_t utcT = std::mktime(utcCheck);
            return local - (utcT - local);
        }
        return local;
    }
    // Fall back to plain digits (unix timestamp)
    bool digits = true;
    for (char c : s) if (!isdigit((unsigned char)c)) { digits = false; break; }
    if (digits && !s.empty()) return (std::time_t)std::stoll(s);
    return std::time(nullptr);
}

void IBKRClient::historicalNews(int requestId, const std::string& time,
                                 const std::string& providerCode,
                                 const std::string& articleId,
                                 const std::string& headline) {
    Push(MsgHistoricalNews{requestId, ParseNewsTime(time),
                           providerCode, articleId, headline});
}

void IBKRClient::historicalNewsEnd(int requestId, bool /*hasMore*/) {
    Push(MsgHistoricalNewsEnd{requestId});
}

void IBKRClient::newsArticle(int requestId, int /*articleType*/,
                              const std::string& articleText) {
    Push(MsgNewsArticle{requestId, articleText});
}

// ── Account summary ──────────────────────────────────────────────────────────

void IBKRClient::accountSummary(int /*reqId*/, const std::string& /*account*/,
                                 const std::string& tag, const std::string& value,
                                 const std::string& currency) {
    Push(MsgAcctSummary{tag, value, currency});
}

void IBKRClient::accountSummaryEnd(int /*reqId*/) {
    // Nothing to do; all data was already pushed item-by-item.
}

// ── Real-time P&L ────────────────────────────────────────────────────────────

void IBKRClient::pnl(int reqId, double dailyPnL, double unrealizedPnL, double realizedPnL) {
    Push(MsgPnL{reqId, dailyPnL, unrealizedPnL, realizedPnL});
}

void IBKRClient::pnlSingle(int reqId, Decimal /*pos*/, double dailyPnL,
                            double unrealizedPnL, double realizedPnL, double value) {
    Push(MsgPnLSingle{reqId, dailyPnL, unrealizedPnL, realizedPnL, value});
}

void IBKRClient::symbolSamples(int reqId,
                                const std::vector<ContractDescription>& contractDescriptions) {
    MsgSymbolSamples msg;
    msg.reqId = reqId;
    msg.results.reserve(contractDescriptions.size());
    for (const auto& cd : contractDescriptions) {
        ContractDesc d;
        d.symbol      = cd.contract.symbol;
        d.secType     = cd.contract.secType;
        d.primaryExch = cd.contract.primaryExchange;
        d.currency    = cd.contract.currency;
        msg.results.push_back(std::move(d));
    }
    Push(std::move(msg));
}

} // namespace core::services
