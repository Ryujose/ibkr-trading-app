#include <catch2/catch_test_macros.hpp>
#include <string>

#include "core/models/OrderData.h"
#include "core/models/ScannerData.h"
#include "core/models/PortfolioData.h"
#include "core/models/MarketData.h"
#include "core/models/NewsData.h"

// ── OrderData string helpers ──────────────────────────────────────────────────

TEST_CASE("OrderSideStr returns correct strings", "[order]") {
    REQUIRE(std::string(core::OrderSideStr(core::OrderSide::Buy))  == "BUY");
    REQUIRE(std::string(core::OrderSideStr(core::OrderSide::Sell)) == "SELL");
}

TEST_CASE("OrderTypeStr returns correct IB order type strings", "[order]") {
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::Market))     == "MKT");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::Limit))      == "LMT");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::Stop))       == "STP");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::StopLimit))  == "STP LMT");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::Trail))      == "TRAIL");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::TrailLimit)) == "TRAIL LIMIT");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::MOC))        == "MOC");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::LOC))        == "LOC");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::MTL))        == "MTL");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::MIT))        == "MIT");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::LIT))        == "LIT");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::Midprice))   == "MIDPRICE");
    REQUIRE(std::string(core::OrderTypeStr(core::OrderType::Relative))   == "REL");
}

TEST_CASE("TIFStr returns correct time-in-force strings", "[order]") {
    REQUIRE(std::string(core::TIFStr(core::TimeInForce::Day)) == "DAY");
    REQUIRE(std::string(core::TIFStr(core::TimeInForce::GTC)) == "GTC");
    REQUIRE(std::string(core::TIFStr(core::TimeInForce::IOC))       == "IOC");
    REQUIRE(std::string(core::TIFStr(core::TimeInForce::FOK))       == "FOK");
    REQUIRE(std::string(core::TIFStr(core::TimeInForce::Overnight)) == "OVERNIGHT");
    REQUIRE(std::string(core::TIFStr(core::TimeInForce::OPG))       == "OPG");
}

TEST_CASE("OrderStatusStr returns correct status strings", "[order]") {
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::Pending))     == "PENDING");
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::Working))     == "WORKING");
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::PartialFill)) == "PARTIAL");
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::Filled))      == "FILLED");
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::Cancelled))     == "CANCELLED");
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::Rejected))      == "REJECTED");
    REQUIRE(std::string(core::OrderStatusStr(core::OrderStatus::PendingCancel)) == "CANCELLING");
}

// ── Order struct defaults ─────────────────────────────────────────────────────

TEST_CASE("Order struct has sane defaults", "[order][defaults]") {
    core::Order o;
    REQUIRE(o.orderId      == 0);
    REQUIRE(o.quantity     == 0.0);
    REQUIRE(o.limitPrice      == 0.0);
    REQUIRE(o.stopPrice       == 0.0);
    REQUIRE(o.auxPrice        == 0.0);
    REQUIRE(o.trailingPercent == 0.0);
    REQUIRE(o.trailStopPrice  == 0.0);
    REQUIRE(o.lmtPriceOffset  == 0.0);
    REQUIRE_FALSE(o.outsideRth);
    REQUIRE(o.filledQty       == 0.0);
    REQUIRE(o.avgFillPrice == 0.0);
    REQUIRE(o.commission   == 0.0);
    REQUIRE(o.status       == core::OrderStatus::Pending);
    REQUIRE(o.side         == core::OrderSide::Buy);
    REQUIRE(o.type         == core::OrderType::Market);
    REQUIRE(o.tif          == core::TimeInForce::Day);
    REQUIRE(o.submittedAt  == 0);
    REQUIRE(o.updatedAt    == 0);
    REQUIRE(o.symbol.empty());
    REQUIRE(o.rejectReason.empty());
}

// ── Fill struct defaults ──────────────────────────────────────────────────────

TEST_CASE("Fill struct has sane defaults", "[fill][defaults]") {
    core::Fill f;
    REQUIRE(f.orderId     == 0);
    REQUIRE(f.quantity    == 0.0);
    REQUIRE(f.price       == 0.0);
    REQUIRE(f.commission  == 0.0);
    REQUIRE(f.realizedPnL == 0.0);
    REQUIRE(f.timestamp   == 0);
    REQUIRE(f.side        == core::OrderSide::Buy);
    REQUIRE(f.symbol.empty());
    REQUIRE(f.execId.empty());
}

// ── DepthLevel struct defaults ────────────────────────────────────────────────

TEST_CASE("DepthLevel struct has sane defaults", "[depth][defaults]") {
    core::DepthLevel d;
    REQUIRE(d.price     == 0.0);
    REQUIRE(d.size      == 0.0);
    REQUIRE(d.numOrders == 1);
    REQUIRE(d.flashAge  == 0.0f);
}

// ── ScannerData string helpers ────────────────────────────────────────────────

TEST_CASE("ScanPresetLabel returns non-empty strings for all presets", "[scanner]") {
    using namespace core;
    const ScanPreset presets[] = {
        ScanPreset::TopGainers, ScanPreset::TopLosers, ScanPreset::VolumeLeaders,
        ScanPreset::NewHighs,   ScanPreset::NewLows,   ScanPreset::RSIOverbought,
        ScanPreset::RSIOversold, ScanPreset::NearEarnings, ScanPreset::MostActive,
        ScanPreset::Custom
    };
    for (auto p : presets) {
        std::string label = ScanPresetLabel(p);
        REQUIRE_FALSE(label.empty());
        REQUIRE(label != "?");
    }
}

TEST_CASE("AssetClassLabel returns correct strings", "[scanner]") {
    REQUIRE(std::string(core::AssetClassLabel(core::AssetClass::Stocks))  == "Stocks");
    REQUIRE(std::string(core::AssetClassLabel(core::AssetClass::Indexes)) == "Indexes");
    REQUIRE(std::string(core::AssetClassLabel(core::AssetClass::ETFs))    == "ETFs");
    REQUIRE(std::string(core::AssetClassLabel(core::AssetClass::Futures)) == "Futures");
}

// ── ScanResult struct defaults ────────────────────────────────────────────────

TEST_CASE("ScanResult struct has sane defaults", "[scanner][defaults]") {
    core::ScanResult r;
    REQUIRE(r.price      == 0.0);
    REQUIRE(r.changePct  == 0.0);
    REQUIRE(r.volume     == 0.0);
    REQUIRE(r.rsi        == 50.0);  // neutral RSI by default
    REQUIRE(r.updatedAt  == 0);
    REQUIRE_FALSE(r.isIndex);
    REQUIRE(r.symbol.empty());
    REQUIRE(r.sparkline.empty());
}

// ── ScanFilter defaults ───────────────────────────────────────────────────────

TEST_CASE("ScanFilter defaults cover the full valid range", "[scanner][defaults]") {
    core::ScanFilter f;
    REQUIRE(f.minPrice     == 0.0);
    REQUIRE(f.maxPrice     >  1e8);   // some very large number
    REQUIRE(f.minChangePct == -100.0);
    REQUIRE(f.maxChangePct ==  100.0);
    REQUIRE(f.minRsi       ==   0.0);
    REQUIRE(f.maxRsi       == 100.0);
    REQUIRE(f.sector.empty());
    REQUIRE(f.exchange.empty());
}

// ── PortfolioData struct defaults ─────────────────────────────────────────────

TEST_CASE("Position struct has sane defaults", "[portfolio][defaults]") {
    core::Position p;
    REQUIRE(p.quantity       == 0.0);
    REQUIRE(p.avgCost        == 0.0);
    REQUIRE(p.marketPrice    == 0.0);
    REQUIRE(p.marketValue    == 0.0);
    REQUIRE(p.unrealizedPnL  == 0.0);
    REQUIRE(p.realizedPnL    == 0.0);
    REQUIRE(p.portfolioWeight == 0.0);
    REQUIRE(p.updatedAt      == 0);
    REQUIRE(p.symbol.empty());
}

TEST_CASE("AccountValues struct has sane defaults", "[portfolio][defaults]") {
    core::AccountValues a;
    REQUIRE(a.netLiquidation  == 0.0);
    REQUIRE(a.totalCashValue  == 0.0);
    REQUIRE(a.buyingPower     == 0.0);
    REQUIRE(a.unrealizedPnL   == 0.0);
    REQUIRE(a.leverage        == 0.0);
    REQUIRE(a.updatedAt       == 0);
    REQUIRE(a.accountId.empty());
    REQUIRE(a.baseCurrency.empty());
}

// ── MarketData Bar struct ─────────────────────────────────────────────────────

TEST_CASE("Bar struct fields are zero-initialised by default", "[bar][defaults]") {
    core::Bar b{};
    REQUIRE(b.timestamp == 0.0);
    REQUIRE(b.open      == 0.0);
    REQUIRE(b.high      == 0.0);
    REQUIRE(b.low       == 0.0);
    REQUIRE(b.close     == 0.0);
    REQUIRE(b.volume    == 0.0);
}
