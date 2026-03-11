#include "core/services/IBKRClient.h"

// IB API implementation headers
#include "EClientSocket.h"
#include "Contract.h"
#include "Order.h"
#include "OrderCancel.h"
#include "Execution.h"
#include "ScannerSubscription.h"
#include "TagValue.h"
#include "CommonDefs.h"
#include "Decimal.h"
#include "bar.h"

#include <cstring>
#include <cassert>

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

    return true;
}

void IBKRClient::Disconnect() {
    m_running.store(false);
    if (m_client->isConnected())
        m_client->eDisconnect();
    m_signal.issueSignal();
    if (m_readerThread.joinable())
        m_readerThread.join();
    m_reader.reset();
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
                                    const std::string& barSize) {
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    // endDateTime="" means now; useRTH=1; formatDate=1; keepUpToDate=false
    m_client->reqHistoricalData(reqId, c, "", duration, barSize,
                                "TRADES", 1, 1, false, empty);
}

void IBKRClient::CancelHistoricalData(int reqId) {
    m_client->cancelHistoricalData(reqId);
}

void IBKRClient::ReqMarketDataType(int type) {
    m_client->reqMarketDataType(type);
}

void IBKRClient::ReqMarketData(int reqId, const std::string& symbol) {
    Contract c = MakeStockContract(symbol);
    TagValueListSPtr empty;
    // genericTickList "233" = RT Volume + news ticks
    m_client->reqMktData(reqId, c, "233", false, false, empty);
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
                             double qty, double limitPrice) {
    Contract c = MakeStockContract(symbol);
    ::Order order;
    order.action        = action;
    order.totalQuantity = DecimalFunctions::doubleToDecimal(qty);
    order.orderType     = orderType;
    order.tif           = "DAY";
    if (orderType == "LMT")
        order.lmtPrice = limitPrice;
    m_client->placeOrder(orderId, c, order);
}

void IBKRClient::CancelOrder(int orderId) {
    OrderCancel oc;
    m_client->cancelOrder(orderId, oc);
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

    for (auto& msg : batch) {
        std::visit([this](auto&& m) {
            using T = std::decay_t<decltype(m)>;

            if constexpr (std::is_same_v<T, MsgConnection>) {
                if (onConnectionChanged) onConnectionChanged(m.connected, m.info);

            } else if constexpr (std::is_same_v<T, MsgBar>) {
                if (onBarData) onBarData(m.reqId, m.bar, m.done);

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
                if (onError) onError(m.code, m.msg);

            } else if constexpr (std::is_same_v<T, MsgNextOrderId>) {
                if (onNextValidId) onNextValidId(m.orderId);
            }
        }, msg);
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
    Push(MsgError{errorCode, errorString});
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
    Push(MsgBar{static_cast<int>(reqId), b, false});
}

void IBKRClient::historicalDataEnd(int reqId, const std::string& /*startDate*/,
                                    const std::string& /*endDate*/) {
    Push(MsgBar{reqId, {}, true});  // sentinel: done=true
}

void IBKRClient::historicalDataUpdate(TickerId reqId, const ::Bar& bar) {
    historicalData(reqId, bar);  // same handling
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
    if (s == "Filled")                                  return ::core::OrderStatus::Filled;
    if (s == "Cancelled" || s == "ApiCancelled")        return ::core::OrderStatus::Cancelled;
    if (s == "Submitted"  || s == "PreSubmitted")       return ::core::OrderStatus::Working;
    if (s == "PartiallyFilled")                         return ::core::OrderStatus::PartialFill;
    if (s == "Pending"    || s == "PendingSubmit")      return ::core::OrderStatus::Pending;
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
    fill.symbol    = contract.symbol;
    fill.side      = (execution.side == "BOT") ? ::core::OrderSide::Buy
                                                : ::core::OrderSide::Sell;
    fill.quantity  = DecimalFunctions::decimalToDouble(execution.shares);
    fill.price     = execution.price;
    fill.timestamp = std::time(nullptr);
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

} // namespace core::services
