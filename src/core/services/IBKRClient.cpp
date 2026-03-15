#include "core/services/IBKRClient.h"

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
                                    bool useRTH) {
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    // formatDate=2 → IB always returns Unix timestamps.
    // keepUpToDate only for intraday bars — IB doesn't support it for daily/weekly/monthly
    // and returns corrupt/zero timestamps for those when enabled.
    bool isIntraday = (barSize.find("day")   == std::string::npos &&
                       barSize.find("week")  == std::string::npos &&
                       barSize.find("month") == std::string::npos);
    m_client->reqHistoricalData(reqId, c, "", duration, barSize,
                                "TRADES", useRTH ? 1 : 0, 2, isIntraday, empty);
}

void IBKRClient::CancelHistoricalData(int reqId) {
    m_client->cancelHistoricalData(reqId);
}

void IBKRClient::ReqContractDetails(int reqId, const std::string& symbol) {
    Contract c = MakeStockContract(symbol);
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

void IBKRClient::PlaceOrder(int orderId, const std::string& symbol,
                             const std::string& action,
                             const std::string& orderType,
                             double qty,
                             double price,
                             const std::string& tif,
                             bool outsideRth,
                             double auxPrice) {
    // Capture all parameters by value and execute on the send thread so the
    // blocking socket write never stalls the UI/render loop.
    PostSend([=, this]() {
        Contract c = MakeStockContract(symbol);
        ::Order order;
        order.action        = action;
        order.totalQuantity = DecimalFunctions::doubleToDecimal(qty);
        order.orderType     = orderType;
        order.tif           = tif;
        order.outsideRth    = outsideRth;

        if (orderType == "LMT") {
            order.lmtPrice = price;
        } else if (orderType == "STP") {
            order.auxPrice = price;
        } else if (orderType == "STP LMT") {
            order.auxPrice = price;      // stop trigger
            order.lmtPrice = auxPrice;   // limit price
        } else if (orderType == "MTL") {
            // Market-to-Limit: no price fields
        } else if (orderType == "TRAIL") {
            order.auxPrice = auxPrice;   // trailing amount in dollars
        } else if (orderType == "TRAIL LIMIT") {
            order.auxPrice = auxPrice;   // trailing amount
            order.lmtPrice = price;      // limit offset (price below/above trail stop)
        } else if (orderType == "LIT") {
            order.auxPrice = auxPrice;
            order.lmtPrice = price;
        } else if (orderType == "MIT") {
            order.auxPrice = auxPrice;
        }

        if (tif == "GTD") {
            std::time_t expiry = std::time(nullptr) + 90LL * 86400;
            char buf[32];
            std::tm* gm = std::gmtime(&expiry);
            if (gm) std::strftime(buf, sizeof(buf), "%Y%m%d %H:%M:%S", gm);
            else    std::strcpy(buf, "20991231 23:59:59");
            order.goodTillDate = buf;
        }

        m_client->placeOrder(orderId, c, order);
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
    static const int info_codes[] = {
        2100, 2103, 2104, 2105, 2106, 2107, 2108, 2119, 2158, 10182
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

std::time_t IBKRClient::ParseIBTime(const std::string& ts) {
    if (ts.empty()) return 0;
    // Plain unix timestamp (intraday bars)?
    bool allDigits = true;
    for (char c : ts) if (!isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
    if (allDigits) return static_cast<std::time_t>(std::stoll(ts));
    // "YYYYMMDD  HH:MM:SS" or "YYYYMMDD HH:MM:SS [TZ]"
    struct tm t = {};
    const char* res = strptime(ts.c_str(), "%Y%m%d %H:%M:%S", &t);
    if (!res) strptime(ts.c_str(), "%Y%m%d", &t);
    t.tm_isdst = -1;
    return mktime(&t);
}

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

// ── Orders ─────────────────────────────────────────────────────────────────

::core::OrderStatus IBKRClient::ParseStatus(const std::string& s) {
    if (s == "Filled")                                                return ::core::OrderStatus::Filled;
    if (s == "Cancelled" || s == "ApiCancelled" || s == "Inactive")  return ::core::OrderStatus::Cancelled;
    if (s == "Submitted"  || s == "PreSubmitted" || s == "ApiPending") return ::core::OrderStatus::Working;
    if (s == "PartiallyFilled")                                       return ::core::OrderStatus::PartialFill;
    if (s == "Pending" || s == "PendingSubmit" || s == "PendingCancel") return ::core::OrderStatus::Pending;
    if (s.empty())                                                    return ::core::OrderStatus::Pending;
    return ::core::OrderStatus::Rejected;
}

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
    if (t == "LMT")                    return ::core::OrderType::Limit;
    if (t == "STP")                    return ::core::OrderType::Stop;
    if (t == "STP LMT")               return ::core::OrderType::StopLimit;
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
    order.stopPrice   = (o.auxPrice  != UNSET_DOUBLE) ? o.auxPrice   : 0.0;
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

} // namespace core::services
