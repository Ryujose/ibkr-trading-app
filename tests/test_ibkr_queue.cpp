#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "core/services/IBKRClient.h"

using namespace core::services;

// ---------------------------------------------------------------------------
// TestableIBKRClient
//   Subclass that exposes the protected Push() method so tests can inject
//   messages directly without a live IB connection.
//   The constructor calls IBKRClient() which creates an EClientSocket and
//   EReaderOSSignal — both are safe to construct without ever connecting.
// ---------------------------------------------------------------------------
class TestableIBKRClient : public IBKRClient {
public:
    void inject(IBMessage msg) { Push(std::move(msg)); }
};

// ── MsgConnection ─────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgConnection to onConnectionChanged", "[queue][connection]") {
    TestableIBKRClient client;

    bool   receivedConnected = false;
    bool   callbackFired     = false;
    std::string receivedInfo;

    client.onConnectionChanged = [&](bool connected, const std::string& info) {
        callbackFired     = true;
        receivedConnected = connected;
        receivedInfo      = info;
    };

    client.inject(MsgConnection{true, "Connected"});
    client.ProcessMessages();

    REQUIRE(callbackFired);
    REQUIRE(receivedConnected);
    REQUIRE(receivedInfo == "Connected");
}

TEST_CASE("ProcessMessages dispatches MsgConnection disconnect event", "[queue][connection]") {
    TestableIBKRClient client;
    bool connected = true;

    client.onConnectionChanged = [&](bool c, const std::string&) { connected = c; };
    client.inject(MsgConnection{false, "Connection closed by remote host"});
    client.ProcessMessages();

    REQUIRE_FALSE(connected);
}

// ── MsgTickPrice ──────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgTickPrice to onTickPrice", "[queue][tick]") {
    TestableIBKRClient client;

    int    receivedTicker = -1;
    int    receivedField  = -1;
    double receivedPrice  = -1.0;

    client.onTickPrice = [&](int ticker, int field, double price) {
        receivedTicker = ticker;
        receivedField  = field;
        receivedPrice  = price;
    };

    client.inject(MsgTickPrice{42, 4, 150.25});
    client.ProcessMessages();

    REQUIRE(receivedTicker == 42);
    REQUIRE(receivedField  == 4);
    REQUIRE(receivedPrice  == Catch::Approx(150.25));
}

// ── MsgTickSize ───────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgTickSize to onTickSize", "[queue][tick]") {
    TestableIBKRClient client;

    int    receivedTicker = -1;
    int    receivedField  = -1;
    double receivedSize   = -1.0;

    client.onTickSize = [&](int ticker, int field, double size) {
        receivedTicker = ticker;
        receivedField  = field;
        receivedSize   = size;
    };

    client.inject(MsgTickSize{10, 8, 500.0});
    client.ProcessMessages();

    REQUIRE(receivedTicker == 10);
    REQUIRE(receivedField  == 8);
    REQUIRE(receivedSize   == Catch::Approx(500.0));
}

// ── MsgBar ────────────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgBar to onBarData", "[queue][bar]") {
    TestableIBKRClient client;

    int         receivedReqId = -1;
    core::Bar   receivedBar   = {};
    bool        receivedDone  = false;
    bool        receivedLive  = false;

    client.onBarData = [&](int reqId, const core::Bar& bar, bool done, bool live) {
        receivedReqId = reqId;
        receivedBar   = bar;
        receivedDone  = done;
        receivedLive  = live;
    };

    core::Bar bar;
    bar.timestamp = 1705329600.0;
    bar.open  = 100.0;
    bar.high  = 105.0;
    bar.low   =  98.0;
    bar.close = 103.0;
    bar.volume = 1'000'000.0;

    client.inject(MsgBar{7, bar, false, true});
    client.ProcessMessages();

    REQUIRE(receivedReqId      == 7);
    REQUIRE(receivedBar.open   == Catch::Approx(100.0));
    REQUIRE(receivedBar.high   == Catch::Approx(105.0));
    REQUIRE(receivedBar.low    == Catch::Approx( 98.0));
    REQUIRE(receivedBar.close  == Catch::Approx(103.0));
    REQUIRE(receivedBar.volume == Catch::Approx(1'000'000.0));
    REQUIRE_FALSE(receivedDone);
    REQUIRE(receivedLive);
}

TEST_CASE("ProcessMessages dispatches done-sentinel MsgBar correctly", "[queue][bar]") {
    TestableIBKRClient client;

    bool doneFired = false;
    int  doneReqId = -1;

    client.onBarData = [&](int reqId, const core::Bar&, bool done, bool) {
        if (done) { doneFired = true; doneReqId = reqId; }
    };

    client.inject(MsgBar{3, {}, true, false});
    client.ProcessMessages();

    REQUIRE(doneFired);
    REQUIRE(doneReqId == 3);
}

// ── MsgDepth ──────────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgDepth to onDepthUpdate", "[queue][depth]") {
    TestableIBKRClient client;

    int    id_out  = -1;
    bool   isBid   = false;
    int    pos_out = -1;
    int    op_out  = -1;
    double price_out = -1.0;
    double size_out  = -1.0;

    client.onDepthUpdate = [&](int id, bool bid, int pos, int op, double price, double size) {
        id_out    = id;
        isBid     = bid;
        pos_out   = pos;
        op_out    = op;
        price_out = price;
        size_out  = size;
    };

    client.inject(MsgDepth{120, true, 0, 1, 149.50, 200.0});
    client.ProcessMessages();

    REQUIRE(id_out    == 120);
    REQUIRE(isBid     == true);
    REQUIRE(pos_out   == 0);
    REQUIRE(op_out    == 1);
    REQUIRE(price_out == Catch::Approx(149.50));
    REQUIRE(size_out  == Catch::Approx(200.0));
}

// ── MsgError ─────────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgError to onError", "[queue][error]") {
    TestableIBKRClient client;

    int         receivedReqId = -1;
    int         receivedCode  = -1;
    std::string receivedMsg;

    client.onError = [&](int reqId, int code, const std::string& msg) {
        receivedReqId = reqId;
        receivedCode  = code;
        receivedMsg   = msg;
    };

    client.inject(MsgError{1, 200, "No security definition found"});
    client.ProcessMessages();

    REQUIRE(receivedReqId == 1);
    REQUIRE(receivedCode  == 200);
    REQUIRE(receivedMsg   == "No security definition found");
}

// ── MsgScanEnd ────────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgScanEnd to onScanEnd", "[queue][scanner]") {
    TestableIBKRClient client;

    int receivedReqId = -1;
    client.onScanEnd = [&](int reqId) { receivedReqId = reqId; };

    client.inject(MsgScanEnd{1000});
    client.ProcessMessages();

    REQUIRE(receivedReqId == 1000);
}

// ── MsgScanItem ───────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgScanItem to onScanItem", "[queue][scanner]") {
    TestableIBKRClient client;

    int             receivedReqId = -1;
    core::ScanResult receivedResult;

    client.onScanItem = [&](int reqId, const core::ScanResult& r) {
        receivedReqId  = reqId;
        receivedResult = r;
    };

    core::ScanResult item;
    item.symbol    = "AAPL";
    item.changePct = 3.5;

    client.inject(MsgScanItem{1000, item});
    client.ProcessMessages();

    REQUIRE(receivedReqId          == 1000);
    REQUIRE(receivedResult.symbol  == "AAPL");
    REQUIRE(receivedResult.changePct == Catch::Approx(3.5));
}

// ── MsgNextOrderId ────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgNextOrderId to onNextValidId", "[queue][order]") {
    TestableIBKRClient client;

    int receivedId = -1;
    client.onNextValidId = [&](int id) { receivedId = id; };

    client.inject(MsgNextOrderId{42});
    client.ProcessMessages();

    REQUIRE(receivedId == 42);
}

// ── Null-callback safety ──────────────────────────────────────────────────────

TEST_CASE("ProcessMessages does not crash when callbacks are unset", "[queue][safety]") {
    TestableIBKRClient client;
    // No callbacks assigned — all std::function members are empty.

    REQUIRE_NOTHROW([&] {
        client.inject(MsgTickPrice{1, 4, 100.0});
        client.inject(MsgBar{2, {}, false, false});
        client.inject(MsgError{3, 200, "test"});
        client.inject(MsgConnection{true, "Connected"});
        client.inject(MsgScanEnd{1000});
        client.ProcessMessages();
    }());
}

// ── Multiple messages in one call ─────────────────────────────────────────────

TEST_CASE("ProcessMessages drains all queued messages in one call", "[queue]") {
    TestableIBKRClient client;

    int tickCount = 0;
    client.onTickPrice = [&](int, int, double) { ++tickCount; };

    for (int i = 0; i < 10; ++i)
        client.inject(MsgTickPrice{i, 4, 100.0 + i});

    client.ProcessMessages();

    REQUIRE(tickCount == 10);
}

TEST_CASE("ProcessMessages preserves message ordering", "[queue]") {
    TestableIBKRClient client;

    std::vector<double> prices;
    client.onTickPrice = [&](int, int, double price) { prices.push_back(price); };

    client.inject(MsgTickPrice{1, 4, 10.0});
    client.inject(MsgTickPrice{1, 4, 20.0});
    client.inject(MsgTickPrice{1, 4, 30.0});
    client.ProcessMessages();

    REQUIRE(prices.size() == 3);
    REQUIRE(prices[0] == Catch::Approx(10.0));
    REQUIRE(prices[1] == Catch::Approx(20.0));
    REQUIRE(prices[2] == Catch::Approx(30.0));
}

// ── Queue is empty after ProcessMessages ──────────────────────────────────────

TEST_CASE("Queue is empty after ProcessMessages; second call is a no-op", "[queue]") {
    TestableIBKRClient client;

    int callCount = 0;
    client.onTickPrice = [&](int, int, double) { ++callCount; };

    client.inject(MsgTickPrice{1, 4, 50.0});
    client.ProcessMessages();
    REQUIRE(callCount == 1);

    // Second call — queue should be empty; callback should not fire again.
    client.ProcessMessages();
    REQUIRE(callCount == 1);
}

// ── MsgOrderStatus ────────────────────────────────────────────────────────────

TEST_CASE("ProcessMessages dispatches MsgOrderStatus to onOrderStatusChanged", "[queue][order]") {
    TestableIBKRClient client;

    int               receivedId     = -1;
    core::OrderStatus receivedStatus = core::OrderStatus::Pending;
    double            receivedFilled = -1.0;
    double            receivedAvg    = -1.0;

    client.onOrderStatusChanged = [&](int id, core::OrderStatus status, double filled, double avg) {
        receivedId     = id;
        receivedStatus = status;
        receivedFilled = filled;
        receivedAvg    = avg;
    };

    client.inject(MsgOrderStatus{99, core::OrderStatus::Filled, 100.0, 152.30});
    client.ProcessMessages();

    REQUIRE(receivedId     == 99);
    REQUIRE(receivedStatus == core::OrderStatus::Filled);
    REQUIRE(receivedFilled == Catch::Approx(100.0));
    REQUIRE(receivedAvg    == Catch::Approx(152.30));
}
