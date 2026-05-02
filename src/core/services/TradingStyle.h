#pragma once

#include "core/models/MarketData.h"

namespace core::services {

// ============================================================================
// Trading-style preset definitions for ChartWindow.
// Each style hard-binds {timeframe + history horizon + auto-analysis params +
// setup-overlay params + default-overlay toggles}. Per-chart, persisted to
// ~/.config/ibkr-trading-app/chart-modes.cfg. See .claude/plans/trading-styles.md.
//
// Pure data — no allocation, no I/O, no IB / ImGui dependency.
// ============================================================================

enum class TradingStyle : int {
    Scalping    = 0,
    DayTrading  = 1,
    Swing       = 2,
    Investment  = 3,
    Free        = 4,   // user-driven: timeframe combo unlocked, settings preserved
};

inline const char* TradingStyleLabel(TradingStyle s) {
    switch (s) {
        case TradingStyle::Scalping:   return "Scalping";
        case TradingStyle::DayTrading: return "Day Trading";
        case TradingStyle::Swing:      return "Swing";
        case TradingStyle::Investment: return "Investment";
        case TradingStyle::Free:       return "Free";
    }
    return "?";
}

inline const char* TradingStyleShort(TradingStyle s) {
    switch (s) {
        case TradingStyle::Scalping:   return "SCALP";
        case TradingStyle::DayTrading: return "DAY";
        case TradingStyle::Swing:      return "SWING";
        case TradingStyle::Investment: return "INVEST";
        case TradingStyle::Free:       return "FREE";
    }
    return "?";
}

// Bundle of every per-style override.
//   timeframe       — the bar size hard-bound to this style
//   historyDuration — IB duration string (e.g. "2 D", "20 D", "1 Y", "5 Y")
//   <auto>          — AutoAnalysisSettings overrides
//   indVwap/Bands   — IndicatorSettings::vwap and ::vwapBands defaults
//   <setup>         — SetupSettings overrides
struct StylePreset {
    core::Timeframe timeframe;
    const char*     historyDuration;

    // AutoAnalysisSettings overrides
    bool supports;
    bool resistances;
    bool trend;
    bool donchian;
    bool keltner;
    bool autoFib;
    bool pivotPoints;
    bool breakouts;
    bool zones;
    int  swingK;
    int  trendLookback;
    int  donchianLen;
    int  maxLevels;
    int  minTouches;
    int  scanCap;
    bool trendChannel;

    // IndicatorSettings overrides (only VWAP-related differ across modes today)
    bool indVwap;
    bool indVwapBands;

    // SetupSettings overrides
    bool   setupOverlay;
    double rrMin;
    double atrPad;
    double roundPad;
    double stopOffset;
    double riskPct;
    bool   useStopLmt;
};

// The four canonical presets. Pure data — no allocation.
inline StylePreset GetPreset(TradingStyle s) {
    switch (s) {
        case TradingStyle::Scalping:
            return StylePreset{
                /*timeframe*/        core::Timeframe::M1,
                /*historyDuration*/  "2 D",
                /*supports*/         true,
                /*resistances*/      true,
                /*trend*/            false,
                /*donchian*/         false,
                /*keltner*/          false,
                /*autoFib*/          false,
                /*pivotPoints*/      true,
                /*breakouts*/        true,
                /*zones*/            true,
                /*swingK*/           3,
                /*trendLookback*/    30,
                /*donchianLen*/      20,
                /*maxLevels*/        4,
                /*minTouches*/       2,
                /*scanCap*/          1500,
                /*trendChannel*/     false,
                /*indVwap*/          true,
                /*indVwapBands*/     false,
                /*setupOverlay*/     false,
                /*rrMin*/            1.5,
                /*atrPad*/           0.4,
                /*roundPad*/         0.03,
                /*stopOffset*/       0.05,
                /*riskPct*/          0.5,
                /*useStopLmt*/       true,
            };
        case TradingStyle::DayTrading:
            return StylePreset{
                /*timeframe*/        core::Timeframe::M15,
                /*historyDuration*/  "20 D",
                /*supports*/         true,
                /*resistances*/      true,
                /*trend*/            true,
                /*donchian*/         false,
                /*keltner*/          false,
                /*autoFib*/          false,
                /*pivotPoints*/      true,
                /*breakouts*/        true,
                /*zones*/            true,
                /*swingK*/           3,
                /*trendLookback*/    40,
                /*donchianLen*/      20,
                /*maxLevels*/        4,
                /*minTouches*/       2,
                /*scanCap*/          1000,
                /*trendChannel*/     false,
                /*indVwap*/          true,
                /*indVwapBands*/     true,
                /*setupOverlay*/     false,
                /*rrMin*/            1.75,
                /*atrPad*/           0.5,
                /*roundPad*/         0.05,
                /*stopOffset*/       0.07,
                /*riskPct*/          0.75,
                /*useStopLmt*/       true,
            };
        case TradingStyle::Swing:
            return StylePreset{
                /*timeframe*/        core::Timeframe::D1,
                /*historyDuration*/  "1 Y",
                /*supports*/         true,
                /*resistances*/      true,
                /*trend*/            true,
                /*donchian*/         false,
                /*keltner*/          false,
                /*autoFib*/          true,
                /*pivotPoints*/      false,
                /*breakouts*/        true,
                /*zones*/            true,
                /*swingK*/           4,
                /*trendLookback*/    50,
                /*donchianLen*/      20,
                /*maxLevels*/        3,
                /*minTouches*/       2,
                /*scanCap*/          1000,
                /*trendChannel*/     false,
                /*indVwap*/          false,
                /*indVwapBands*/     false,
                /*setupOverlay*/     false,
                /*rrMin*/            2.0,
                /*atrPad*/           0.5,
                /*roundPad*/         0.07,
                /*stopOffset*/       0.10,
                /*riskPct*/          1.0,
                /*useStopLmt*/       true,
            };
        case TradingStyle::Investment:
            return StylePreset{
                /*timeframe*/        core::Timeframe::W1,
                /*historyDuration*/  "5 Y",
                /*supports*/         true,
                /*resistances*/      true,
                /*trend*/            true,
                /*donchian*/         false,
                /*keltner*/          false,
                /*autoFib*/          true,
                /*pivotPoints*/      false,
                /*breakouts*/        false,
                /*zones*/            false,
                /*swingK*/           5,
                /*trendLookback*/    52,
                /*donchianLen*/      20,
                /*maxLevels*/        3,
                /*minTouches*/       3,
                /*scanCap*/          1000,
                /*trendChannel*/     false,
                /*indVwap*/          false,
                /*indVwapBands*/     false,
                /*setupOverlay*/     false,
                /*rrMin*/            3.0,
                /*atrPad*/           1.0,
                /*roundPad*/         0.20,
                /*stopOffset*/       0.25,
                /*riskPct*/          1.5,
                /*useStopLmt*/       true,
            };
        case TradingStyle::Free:
            // Construction-default baseline. Switching INTO Free preserves
            // the chart's current m_timeframe (the whole point of Free is
            // to let the user pick any TF), so this preset's `timeframe`
            // is only the value used when no prior TF exists. The host
            // (ChartWindow::setTradingStyle) substitutes
            // TimeframeIBDuration(currentTF) for the actual fetch, so the
            // historyDuration here is mostly cosmetic.
            return StylePreset{
                /*timeframe*/        core::Timeframe::D1,
                /*historyDuration*/  "6 M",
                /*supports*/         true,
                /*resistances*/      true,
                /*trend*/            true,
                /*donchian*/         false,
                /*keltner*/          false,
                /*autoFib*/          false,
                /*pivotPoints*/      false,
                /*breakouts*/        false,
                /*zones*/            false,
                /*swingK*/           3,
                /*trendLookback*/    50,
                /*donchianLen*/      20,
                /*maxLevels*/        3,
                /*minTouches*/       2,
                /*scanCap*/          1000,
                /*trendChannel*/     false,
                /*indVwap*/          true,
                /*indVwapBands*/     false,
                /*setupOverlay*/     false,
                /*rrMin*/            2.0,
                /*atrPad*/           0.5,
                /*roundPad*/         0.07,
                /*stopOffset*/       0.10,
                /*riskPct*/          1.0,
                /*useStopLmt*/       true,
            };
    }
    return GetPreset(TradingStyle::Swing);  // unreachable, keeps compilers quiet
}

// Stamp every overridable field onto the existing settings structs.
// Caller is responsible for re-running DetectStructure() after this returns.
//
// Templated so this header can be included from tests without dragging in
// ChartWindow.h: tests instantiate with hand-rolled stub structs that have
// the same field names.
template <typename Ind, typename Auto, typename Setup>
inline void ApplyPreset(const StylePreset& p,
                        Ind& ind, Auto& a, Setup& s,
                        core::Timeframe& tf) {
    tf = p.timeframe;

    a.supports     = p.supports;
    a.resistances  = p.resistances;
    a.trend        = p.trend;
    a.donchian     = p.donchian;
    a.keltner      = p.keltner;
    a.autoFib      = p.autoFib;
    a.pivotPoints  = p.pivotPoints;
    a.breakouts    = p.breakouts;
    a.zones        = p.zones;
    a.swingK       = p.swingK;
    a.trendLookback= p.trendLookback;
    a.donchianLen  = p.donchianLen;
    a.maxLevels    = p.maxLevels;
    a.minTouches   = p.minTouches;
    a.scanCap      = p.scanCap;
    a.trendChannel = p.trendChannel;

    ind.vwap       = p.indVwap;
    ind.vwapBands  = p.indVwapBands;

    s.overlay     = p.setupOverlay;
    s.rrMin       = p.rrMin;
    s.atrPad      = p.atrPad;
    s.roundPad    = p.roundPad;
    s.stopOffset  = p.stopOffset;
    s.riskPct     = p.riskPct;
    s.useStopLmt  = p.useStopLmt;
}

}  // namespace core::services
